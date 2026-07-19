#ifndef UI_OVERLAY_H
#define UI_OVERLAY_H

#include <stdbool.h>
#include <pthread.h>

#include "fb_lcd.h"
#include "touch_input.h"

typedef enum {
	UI_CMD_NONE = 0,
	UI_CMD_TOGGLE_RECORD,
} ui_cmd_e;

typedef struct UI_OVERLAY_S {
	pthread_mutex_t lock;
	bool menu_visible;
	bool recording;
	int elapsed_sec;
	FB_LCD_BTN_S record_btn;
	bool btn_valid;
} UI_OVERLAY_S;

void ui_overlay_init(UI_OVERLAY_S *ui);
void ui_overlay_set_recording(UI_OVERLAY_S *ui, bool recording, int elapsed_sec);
void ui_overlay_get(UI_OVERLAY_S *ui, bool *menu_visible, bool *recording,
		    int *elapsed_sec);

/* Apply one touch event; returns a command for the main loop. */
ui_cmd_e ui_overlay_handle_touch(UI_OVERLAY_S *ui, const TOUCH_EVT_S *evt);

/* Draw status + rec HUD + optional menu on top of the frame already blitted. */
void ui_overlay_draw(UI_OVERLAY_S *ui, FB_LCD_S *fb, int bat_valid, int bat_pct,
		     int temp_valid, int temp_c);

#endif /* UI_OVERLAY_H */
