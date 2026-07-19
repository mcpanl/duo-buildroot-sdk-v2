#ifndef FB_LCD_H
#define FB_LCD_H

#include <stddef.h>
#include <stdint.h>

#define FB_LCD_WIDTH  172
#define FB_LCD_HEIGHT 320
#define FB_LCD_DEV    "/dev/fb0"

/* Inset HUD from rounded glass corners (logical top-left origin). */
#define FB_LCD_HUD_INSET_X  10
#define FB_LCD_HUD_INSET_Y  12

/* Half-screen menu starts at mid height. */
#define FB_LCD_MENU_Y0      (FB_LCD_HEIGHT / 2)

typedef struct FB_LCD_S {
	int fd;
	uint16_t *fb;
	size_t size;
	int width;
	int height;
	int line_length;
} FB_LCD_S;

#define FB_LCD_COLOR_WHITE  0xFFFFu
#define FB_LCD_COLOR_BLACK  0x0000u
#define FB_LCD_COLOR_GREEN  0x07E0u
#define FB_LCD_COLOR_RED    0xF800u
#define FB_LCD_COLOR_YELLOW 0xFFE0u
#define FB_LCD_COLOR_ORANGE 0xFD20u
#define FB_LCD_COLOR_CYAN   0x07FFu
#define FB_LCD_COLOR_GRAY   0x8410u
#define FB_LCD_COLOR_DKGRAY 0x4208u

typedef struct FB_LCD_BTN_S {
	int x;
	int y;
	int w;
	int h;
} FB_LCD_BTN_S;

int fb_lcd_open(FB_LCD_S *fb);
void fb_lcd_close(FB_LCD_S *fb);
void fb_lcd_clear(FB_LCD_S *fb, uint16_t color);
void fb_lcd_draw_rgb565(FB_LCD_S *fb, const uint16_t *src, int src_w, int src_h);
void fb_lcd_draw_status_hud(FB_LCD_S *fb, int bat_valid, int bat_pct,
			    int temp_valid, int temp_c);

/* Recording HUD at top-left: "REC MM:SS" when recording, hidden otherwise. */
void fb_lcd_draw_rec_hud(FB_LCD_S *fb, int recording, int elapsed_sec);

/*
 * Half-screen menu (y >= MENU_Y0). recording=0 shows Record (green),
 * recording=1 shows Stop (red). Fills *btn with hit-test rect.
 */
void fb_lcd_draw_menu(FB_LCD_S *fb, int recording, FB_LCD_BTN_S *btn);

#endif /* FB_LCD_H */
