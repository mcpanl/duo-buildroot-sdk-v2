#ifndef FB_LCD_H
#define FB_LCD_H

#include <stddef.h>
#include <stdint.h>

#define FB_LCD_WIDTH  172
#define FB_LCD_HEIGHT 320
#define FB_LCD_DEV    "/dev/fb0"

typedef struct FB_LCD_S {
	int fd;
	uint16_t *fb;
	size_t size;
	int width;
	int height;
	int line_length;
} FB_LCD_S;

int fb_lcd_open(FB_LCD_S *fb);
void fb_lcd_close(FB_LCD_S *fb);
void fb_lcd_clear(FB_LCD_S *fb, uint16_t color);
void fb_lcd_draw_rgb565(FB_LCD_S *fb, const uint16_t *src, int src_w, int src_h);

#endif /* FB_LCD_H */
