#include "bench_fb.h"

#include <string.h>

static uint16_t *bench_fb_row(FB_LCD_S *fb, int y_top)
{
	int py = fb->height - 1 - y_top;

	return (uint16_t *)((uint8_t *)fb->fb + py * fb->line_length);
}

void bench_fb_draw_composite(FB_LCD_S *fb, const uint16_t *composite,
			     int comp_w, int comp_h)
{
	int y;

	if (!fb || !fb->fb || !composite || comp_w <= 0 || comp_h <= 0)
		return;

	for (y = 0; y < comp_h && y < fb->height; y++) {
		uint16_t *dst_row = bench_fb_row(fb, y);
		const uint16_t *src_row = composite + y * comp_w;
		int copy_w = comp_w;

		if (copy_w > fb->width)
			copy_w = fb->width;

		memcpy(dst_row, src_row, (size_t)copy_w * sizeof(uint16_t));
	}
}
