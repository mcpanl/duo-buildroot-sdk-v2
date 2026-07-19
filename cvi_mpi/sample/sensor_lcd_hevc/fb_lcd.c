#include "fb_lcd.h"
#include "perf_stats.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/fb.h>

/* 8x8 bitmap font (from screen_demo.py), one row per byte MSB=left. */
static const uint8_t g_font8[128][8] = {
	['0'] = { 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00 },
	['1'] = { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 },
	['2'] = { 0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00 },
	['3'] = { 0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00 },
	['4'] = { 0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00 },
	['5'] = { 0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00 },
	['6'] = { 0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00 },
	['7'] = { 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00 },
	['8'] = { 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00 },
	['9'] = { 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00 },
	['%'] = { 0x62, 0x64, 0x08, 0x10, 0x26, 0x46, 0x00, 0x00 },
	[':'] = { 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00 },
	['-'] = { 0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00 },
	['A'] = { 0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00 },
	['C'] = { 0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00 },
	['D'] = { 0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00 },
	['E'] = { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00 },
	['O'] = { 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 },
	['P'] = { 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00 },
	['R'] = { 0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00 },
	['S'] = { 0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00 },
	['T'] = { 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },
};

#define HUD_TEXT_SCALE 1
#define HUD_CHAR_W     (8 * HUD_TEXT_SCALE)
#define HUD_CHAR_H     (8 * HUD_TEXT_SCALE)
#define HUD_PAD        1

static uint16_t *fb_lcd_row(FB_LCD_S *fb, int y_top)
{
	int py = fb->height - 1 - y_top;

	return (uint16_t *)((uint8_t *)fb->fb + py * fb->line_length);
}

static void fb_lcd_set_pixel(FB_LCD_S *fb, int x, int y, uint16_t color)
{
	if (!fb || !fb->fb || x < 0 || x >= fb->width || y < 0 || y >= fb->height)
		return;
	fb_lcd_row(fb, y)[x] = color;
}

static const uint8_t *fb_lcd_glyph(char ch)
{
	unsigned idx = (unsigned char)ch;
	const uint8_t *glyph;
	int row, any = 0;

	if (idx >= 128)
		return NULL;

	glyph = g_font8[idx];
	for (row = 0; row < 8; row++)
		any |= glyph[row];
	return any ? glyph : NULL;
}

static void fb_lcd_fill_rect(FB_LCD_S *fb, int x, int y, int w, int h, uint16_t color)
{
	int yy, xx;

	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > fb->width)
		w = fb->width - x;
	if (y + h > fb->height)
		h = fb->height - y;
	if (w <= 0 || h <= 0)
		return;

	for (yy = y; yy < y + h; yy++) {
		uint16_t *row = fb_lcd_row(fb, yy);

		for (xx = x; xx < x + w; xx++)
			row[xx] = color;
	}
}

/* Darken existing pixels for a translucent-ish panel. */
static void fb_lcd_dim_rect(FB_LCD_S *fb, int x, int y, int w, int h)
{
	int yy, xx;

	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > fb->width)
		w = fb->width - x;
	if (y + h > fb->height)
		h = fb->height - y;
	if (w <= 0 || h <= 0)
		return;

	for (yy = y; yy < y + h; yy++) {
		uint16_t *row = fb_lcd_row(fb, yy);

		for (xx = x; xx < x + w; xx++) {
			uint16_t c = row[xx];
			uint16_t r = (c >> 11) & 0x1F;
			uint16_t g = (c >> 5) & 0x3F;
			uint16_t b = c & 0x1F;

			r >>= 2;
			g >>= 2;
			b >>= 2;
			row[xx] = (uint16_t)((r << 11) | (g << 5) | b);
		}
	}
}

static void fb_lcd_draw_char(FB_LCD_S *fb, int x, int y, char ch, int scale,
			     uint16_t fg, uint16_t bg)
{
	const uint8_t *glyph = fb_lcd_glyph(ch);
	int row, col;

	if (!glyph)
		return;

	if (scale == 1) {
		for (row = 0; row < 8; row++) {
			uint8_t bits = glyph[row];
			uint16_t *dst = fb_lcd_row(fb, y + row);

			for (col = 0; col < 8; col++)
				dst[x + col] = (bits & (1u << (7 - col))) ? fg : bg;
		}
		return;
	}

	for (row = 0; row < 8; row++) {
		uint8_t bits = glyph[row];

		for (col = 0; col < 8; col++) {
			uint16_t color = (bits & (1u << (7 - col))) ? fg : bg;
			int sy, sx;

			for (sy = 0; sy < scale; sy++) {
				for (sx = 0; sx < scale; sx++)
					fb_lcd_set_pixel(fb, x + col * scale + sx,
							 y + row * scale + sy, color);
			}
		}
	}
}

static int fb_lcd_text_width(const char *text, int scale)
{
	int len = 0;

	if (!text)
		return 0;
	while (text[len])
		len++;
	return len * 8 * scale;
}

static void fb_lcd_draw_text(FB_LCD_S *fb, int x, int y, const char *text,
			     int scale, uint16_t fg, uint16_t bg)
{
	int cx = x;
	const char *p;

	if (!fb || !text)
		return;

	for (p = text; *p; p++) {
		if (*p == ' ') {
			cx += 8 * scale;
			continue;
		}
		fb_lcd_draw_char(fb, cx, y, *p, scale, fg, bg);
		cx += 8 * scale;
	}
}

static uint16_t fb_lcd_battery_color(int pct)
{
	if (pct <= 15)
		return FB_LCD_COLOR_RED;
	if (pct <= 30)
		return FB_LCD_COLOR_ORANGE;
	if (pct <= 60)
		return FB_LCD_COLOR_YELLOW;
	return FB_LCD_COLOR_GREEN;
}

void fb_lcd_draw_status_hud(FB_LCD_S *fb, int bat_valid, int bat_pct,
			    int temp_valid, int temp_c)
{
	char bat_text[8];
	char temp_text[8];
	int bat_w, temp_w;
	int bat_x, temp_x;
	int bat_y = FB_LCD_HUD_INSET_Y;
	int temp_y = fb->height - FB_LCD_HUD_INSET_Y - HUD_CHAR_H;

	if (!fb || !fb->fb)
		return;

	if (bat_valid)
		snprintf(bat_text, sizeof(bat_text), "%d%%", bat_pct);
	else
		snprintf(bat_text, sizeof(bat_text), "--");

	if (temp_valid)
		snprintf(temp_text, sizeof(temp_text), "%dC", temp_c);
	else
		snprintf(temp_text, sizeof(temp_text), "--C");

	bat_w = fb_lcd_text_width(bat_text, HUD_TEXT_SCALE);
	temp_w = fb_lcd_text_width(temp_text, HUD_TEXT_SCALE);
	bat_x = fb->width - FB_LCD_HUD_INSET_X - bat_w;
	temp_x = FB_LCD_HUD_INSET_X;

	fb_lcd_fill_rect(fb, bat_x - HUD_PAD, bat_y - HUD_PAD, bat_w + HUD_PAD * 2,
			 HUD_CHAR_H + HUD_PAD * 2, FB_LCD_COLOR_BLACK);
	fb_lcd_fill_rect(fb, temp_x - HUD_PAD, temp_y - HUD_PAD,
			 temp_w + HUD_PAD * 2, HUD_CHAR_H + HUD_PAD * 2,
			 FB_LCD_COLOR_BLACK);

	fb_lcd_draw_text(fb, bat_x, bat_y, bat_text, HUD_TEXT_SCALE,
			 bat_valid ? fb_lcd_battery_color(bat_pct) : FB_LCD_COLOR_WHITE,
			 FB_LCD_COLOR_BLACK);
	fb_lcd_draw_text(fb, temp_x, temp_y, temp_text, HUD_TEXT_SCALE,
			 FB_LCD_COLOR_CYAN, FB_LCD_COLOR_BLACK);
}

void fb_lcd_draw_rec_hud(FB_LCD_S *fb, int recording, int elapsed_sec)
{
	char text[16];
	int tw, tx, ty;
	int mm, ss;

	if (!fb || !fb->fb || !recording)
		return;

	if (elapsed_sec < 0)
		elapsed_sec = 0;
	mm = elapsed_sec / 60;
	ss = elapsed_sec % 60;
	if (mm > 99)
		mm = 99;

	snprintf(text, sizeof(text), "REC %02d:%02d", mm, ss);
	tw = fb_lcd_text_width(text, HUD_TEXT_SCALE);
	tx = FB_LCD_HUD_INSET_X;
	ty = FB_LCD_HUD_INSET_Y;

	fb_lcd_fill_rect(fb, tx - HUD_PAD, ty - HUD_PAD, tw + HUD_PAD * 2 + 10,
			 HUD_CHAR_H + HUD_PAD * 2, FB_LCD_COLOR_BLACK);
	/* Red recording dot */
	fb_lcd_fill_rect(fb, tx, ty + 2, 6, 6, FB_LCD_COLOR_RED);
	fb_lcd_draw_text(fb, tx + 10, ty, text, HUD_TEXT_SCALE,
			 FB_LCD_COLOR_RED, FB_LCD_COLOR_BLACK);
}

void fb_lcd_draw_menu(FB_LCD_S *fb, int recording, FB_LCD_BTN_S *btn)
{
	const char *label;
	uint16_t btn_fg;
	uint16_t btn_bg;
	int menu_y0;
	int btn_w, btn_h, btn_x, btn_y;
	int tw, scale = 1;

	if (!fb || !fb->fb)
		return;

	menu_y0 = fb->height / 2;
	fb_lcd_dim_rect(fb, 0, menu_y0, fb->width, fb->height - menu_y0);
	fb_lcd_fill_rect(fb, 0, menu_y0, fb->width, 2, FB_LCD_COLOR_GRAY);

	if (recording) {
		label = "STOP";
		btn_bg = FB_LCD_COLOR_RED;
		btn_fg = FB_LCD_COLOR_WHITE;
	} else {
		label = "RECORD";
		btn_bg = FB_LCD_COLOR_GREEN;
		btn_fg = FB_LCD_COLOR_BLACK;
	}

	tw = fb_lcd_text_width(label, scale);
	btn_w = tw + 24;
	btn_h = HUD_CHAR_H + 20;
	if (btn_w > fb->width - 20)
		btn_w = fb->width - 20;
	btn_x = (fb->width - btn_w) / 2;
	btn_y = menu_y0 + (fb->height - menu_y0 - btn_h) / 2;

	fb_lcd_fill_rect(fb, btn_x, btn_y, btn_w, btn_h, btn_bg);
	fb_lcd_draw_text(fb, btn_x + (btn_w - tw) / 2,
			 btn_y + (btn_h - HUD_CHAR_H) / 2, label, scale,
			 btn_fg, btn_bg);

	if (btn) {
		btn->x = btn_x;
		btn->y = btn_y;
		btn->w = btn_w;
		btn->h = btn_h;
	}
}

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
