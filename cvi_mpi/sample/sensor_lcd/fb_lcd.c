#include "fb_lcd.h"
#include "perf_stats.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/fb.h>

int fb_lcd_open(FB_LCD_S *fb)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo var;

	if (!fb)
		return -1;

	memset(fb, 0, sizeof(*fb));

	fb->fd = open(FB_LCD_DEV, O_RDWR);
	if (fb->fd < 0) {
		perror("open " FB_LCD_DEV);
		return -1;
	}

	if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("FBIOGET_FSCREENINFO");
		close(fb->fd);
		fb->fd = -1;
		return -1;
	}

	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &var) < 0) {
		perror("FBIOGET_VSCREENINFO");
		close(fb->fd);
		fb->fd = -1;
		return -1;
	}

	fb->width = (int)var.xres;
	fb->height = (int)var.yres;
	fb->line_length = (int)fix.line_length;
	if (fb->width <= 0)
		fb->width = FB_LCD_WIDTH;
	if (fb->height <= 0)
		fb->height = FB_LCD_HEIGHT;
	if (fb->line_length < fb->width * 2)
		fb->line_length = fb->width * 2;

	fb->size = fix.smem_len;
	if (fb->size < (size_t)fb->line_length * fb->height)
		fb->size = (size_t)fb->line_length * fb->height;

	fb->fb = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
	if (fb->fb == MAP_FAILED) {
		perror("mmap fb");
		close(fb->fd);
		fb->fd = -1;
		fb->fb = NULL;
		return -1;
	}

	return 0;
}

void fb_lcd_close(FB_LCD_S *fb)
{
	if (!fb)
		return;

	if (fb->fb && fb->fb != MAP_FAILED) {
		munmap(fb->fb, fb->size);
		fb->fb = NULL;
	}
	if (fb->fd >= 0) {
		close(fb->fd);
		fb->fd = -1;
	}
}

void fb_lcd_clear(FB_LCD_S *fb, uint16_t color)
{
	int y;
	struct timespec t0, t1;

	if (!fb || !fb->fb)
		return;

	perf_timespec_now(&t0);
	for (y = 0; y < fb->height; y++) {
		uint16_t *row = (uint16_t *)((uint8_t *)fb->fb + y * fb->line_length);
		int x;

		for (x = 0; x < fb->width; x++)
			row[x] = color;
	}
	perf_timespec_now(&t1);
	perf_record(PERF_FB_CLEAR, perf_elapsed_ns(&t0, &t1));
}

void fb_lcd_draw_rgb565(FB_LCD_S *fb, const uint16_t *src, int src_w, int src_h)
{
	int x, y;
	int crop_x = 0;
	int crop_y = 0;
	int draw_w;
	int draw_h;
	int off_x;
	int off_y;

	if (!fb || !fb->fb || !src || src_w <= 0 || src_h <= 0)
		return;

	fb_lcd_clear(fb, 0x0000);

	draw_w = src_w;
	draw_h = src_h;

	/* Center-crop when VPSS buffer is wider/taller than the panel (e.g. 192 -> 172). */
	if (draw_w > fb->width) {
		crop_x = (draw_w - fb->width) / 2;
		draw_w = fb->width;
	}
	if (draw_h > fb->height) {
		crop_y = (draw_h - fb->height) / 2;
		draw_h = fb->height;
	}

	off_x = (fb->width - draw_w) / 2;
	off_y = (fb->height - draw_h) / 2;

	{
		struct timespec t0, t1;

		perf_timespec_now(&t0);
		for (y = 0; y < draw_h; y++) {
			int src_y = crop_y + y;
			int dst_y = fb->height - 1 - (off_y + y);
			const uint16_t *src_row = src + src_y * src_w + crop_x;
			uint16_t *dst_row = (uint16_t *)((uint8_t *)fb->fb +
							 dst_y * fb->line_length);

			for (x = 0; x < draw_w; x++)
				dst_row[off_x + x] = src_row[x];
		}
		perf_timespec_now(&t1);
		perf_record(PERF_FB_BLIT, perf_elapsed_ns(&t0, &t1));
	}
}
