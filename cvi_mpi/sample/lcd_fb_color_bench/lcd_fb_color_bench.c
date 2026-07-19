/*
 * LCD framebuffer color-order benchmark.
 *
 * Draws five horizontal bands on /dev/fb0 using the kernel-reported RGB
 * bitfield layout (no camera / ISP / VPSS).  Use this to verify panel color
 * order before debugging camera pipelines.
 *
 * Bands (top -> bottom, logical screen orientation):
 *   1. Red
 *   2. Green
 *   3. Blue
 *   4. Yellow  (R+G) — swaps to Cyan if R/B channels are reversed
 *   5. Cyan    (G+B) — swaps to Yellow if R/B channels are reversed
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/fb.h>

#define FB_DEV_DEFAULT "/dev/fb0"

struct fb_ctx {
	int fd;
	void *map;
	size_t map_len;
	int width;
	int height;
	int line_length;
	int bpp;
	struct fb_var_screeninfo var;
};

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static uint32_t fb_pack_rgb(const struct fb_var_screeninfo *v,
			    uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t pixel = 0;

	if (v->red.length)
		pixel |= ((uint32_t)(r >> (8 - v->red.length)) << v->red.offset);
	if (v->green.length)
		pixel |= ((uint32_t)(g >> (8 - v->green.length)) << v->green.offset);
	if (v->blue.length)
		pixel |= ((uint32_t)(b >> (8 - v->blue.length)) << v->blue.offset);

	return pixel;
}

static void fb_store_pixel(uint8_t *row, int bpp, uint32_t pixel)
{
	if (bpp == 16) {
		*(uint16_t *)row = (uint16_t)pixel;
	} else if (bpp == 32) {
		*(uint32_t *)row = pixel;
	} else if (bpp == 24) {
		row[0] = (uint8_t)(pixel & 0xFF);
		row[1] = (uint8_t)((pixel >> 8) & 0xFF);
		row[2] = (uint8_t)((pixel >> 16) & 0xFF);
	} else {
		/* Fallback: treat as 16-bit. */
		*(uint16_t *)row = (uint16_t)pixel;
	}
}

static uint8_t *fb_row_ptr(struct fb_ctx *fb, int logical_y)
{
	/*
	 * Match sensor_lcd HUD orientation: logical y=0 is the top edge as
	 * the user sees the panel (framebuffer row is vertically flipped).
	 */
	int phys_y = fb->height - 1 - logical_y;

	return (uint8_t *)fb->map + phys_y * fb->line_length;
}

static void fb_fill_band(struct fb_ctx *fb, int y0, int y1, uint32_t pixel)
{
	int y, x;

	for (y = y0; y < y1; y++) {
		uint8_t *row = fb_row_ptr(fb, y);

		for (x = 0; x < fb->width; x++)
			fb_store_pixel(row + x * (fb->bpp / 8), fb->bpp, pixel);
	}
}

static void fb_print_info(const struct fb_ctx *fb)
{
	const struct fb_var_screeninfo *v = &fb->var;

	printf("=== LCD framebuffer color bench ===\n");
	printf("device:      %s\n", FB_DEV_DEFAULT);
	printf("resolution:  %dx%d\n", fb->width, fb->height);
	printf("bpp:         %d (effective store: %d)\n", v->bits_per_pixel, fb->bpp);
	printf("line_length: %d bytes\n", fb->line_length);
	printf("RGB fields:  R off=%u len=%u  G off=%u len=%u  B off=%u len=%u\n",
	       v->red.offset, v->red.length,
	       v->green.offset, v->green.length,
	       v->blue.offset, v->blue.length);
	printf("orientation: logical top -> physical row (height-1)\n");
	printf("\n");
}

static void fb_print_bands(const struct fb_ctx *fb)
{
	static const struct {
		const char *name;
		uint8_t r, g, b;
	} bands[] = {
		{ "1 RED",    255,   0,   0 },
		{ "2 GREEN",    0, 255,   0 },
		{ "3 BLUE",     0,   0, 255 },
		{ "4 YELLOW", 255, 255,   0 },
		{ "5 CYAN",     0, 255, 255 },
	};
	unsigned i;

	printf("Expected bands (top -> bottom):\n");
	for (i = 0; i < sizeof(bands) / sizeof(bands[0]); i++) {
		uint32_t px = fb_pack_rgb(&fb->var, bands[i].r, bands[i].g, bands[i].b);

		printf("  %s  RGB(%3u,%3u,%3u)  pixel=0x%04X\n",
		       bands[i].name, bands[i].r, bands[i].g, bands[i].b,
		       fb->bpp <= 16 ? (unsigned)(px & 0xFFFFu) : (unsigned)px);
	}
	printf("\n");
	printf("If R/B are swapped: RED<->BLUE, YELLOW<->CYAN, GREEN unchanged.\n");
	printf("Press Ctrl+C to exit.\n\n");
}

static int fb_open(struct fb_ctx *fb, const char *dev)
{
	struct fb_fix_screeninfo fix;

	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;

	fb->fd = open(dev, O_RDWR);
	if (fb->fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return -1;
	}

	if (ioctl(fb->fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		fprintf(stderr, "FBIOGET_FSCREENINFO: %s\n", strerror(errno));
		goto fail;
	}

	if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &fb->var) < 0) {
		fprintf(stderr, "FBIOGET_VSCREENINFO: %s\n", strerror(errno));
		goto fail;
	}

	fb->width = (int)fb->var.xres;
	fb->height = (int)fb->var.yres;
	fb->line_length = (int)fix.line_length;
	fb->bpp = fb->var.bits_per_pixel;
	if (fb->bpp <= 0)
		fb->bpp = 16;

	fb->map_len = fix.smem_len;
	if (fb->map_len < (size_t)fb->line_length * fb->height)
		fb->map_len = (size_t)fb->line_length * fb->height;

	fb->map = mmap(NULL, fb->map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		       fb->fd, 0);
	if (fb->map == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		fb->map = NULL;
		goto fail;
	}

	return 0;

fail:
	if (fb->map && fb->map != MAP_FAILED)
		munmap(fb->map, fb->map_len);
	if (fb->fd >= 0)
		close(fb->fd);
	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
	return -1;
}

static void fb_close(struct fb_ctx *fb)
{
	if (!fb)
		return;
	if (fb->map && fb->map != MAP_FAILED)
		munmap(fb->map, fb->map_len);
	if (fb->fd >= 0)
		close(fb->fd);
	memset(fb, 0, sizeof(*fb));
	fb->fd = -1;
}

static int fb_draw_bands(struct fb_ctx *fb)
{
	static const struct {
		uint8_t r, g, b;
	} bands[] = {
		{ 255,   0,   0 },
		{   0, 255,   0 },
		{   0,   0, 255 },
		{ 255, 255,   0 },
		{   0, 255, 255 },
	};
	const int n = (int)(sizeof(bands) / sizeof(bands[0]));
	int band_h = fb->height / n;
	int i;

	if (band_h <= 0)
		return -1;

	for (i = 0; i < n; i++) {
		int y0 = i * band_h;
		int y1 = (i == n - 1) ? fb->height : (i + 1) * band_h;
		uint32_t px = fb_pack_rgb(&fb->var, bands[i].r, bands[i].g, bands[i].b);

		fb_fill_band(fb, y0, y1, px);
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct fb_ctx fb;
	const char *dev = FB_DEV_DEFAULT;
	int hold_sec = 0;

	if (argc >= 2)
		dev = argv[1];
	if (argc >= 3)
		hold_sec = atoi(argv[2]);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (fb_open(&fb, dev) != 0)
		return 1;

	fb_print_info(&fb);
	fb_print_bands(&fb);

	if (fb_draw_bands(&fb) != 0) {
		fprintf(stderr, "draw failed\n");
		fb_close(&fb);
		return 1;
	}

	printf("Pattern drawn. Holding display");
	if (hold_sec > 0)
		printf(" for %d s", hold_sec);
	printf("...\n");

	if (hold_sec > 0) {
		int elapsed = 0;

		while (!g_stop && elapsed < hold_sec) {
			sleep(1);
			elapsed++;
		}
	} else {
		while (!g_stop)
			sleep(1);
	}

	fb_close(&fb);
	return 0;
}
