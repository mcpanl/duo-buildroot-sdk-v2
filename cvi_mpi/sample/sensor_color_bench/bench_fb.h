#ifndef BENCH_FB_H
#define BENCH_FB_H

#include <stdint.h>
#include "../sensor_lcd/fb_lcd.h"

#define BENCH_BANDS       3
#define BENCH_BAND_H      (FB_LCD_HEIGHT / BENCH_BANDS)
#define BENCH_BAND_LAST_H (FB_LCD_HEIGHT - BENCH_BAND_H * (BENCH_BANDS - 1))

/* Marker colors at top of each band (2 px): VI=red, VPSS-YUV=green, VPSS-RGB=blue */
#define BENCH_MARK_VI   0xF800u
#define BENCH_MARK_YUV  0x07E0u
#define BENCH_MARK_RGB  0x001Fu

void bench_fb_draw_composite(FB_LCD_S *fb, const uint16_t *composite,
			     int comp_w, int comp_h);

#endif /* BENCH_FB_H */
