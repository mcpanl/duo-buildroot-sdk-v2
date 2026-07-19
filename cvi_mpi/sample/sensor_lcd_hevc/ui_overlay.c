#include "ui_overlay.h"

#include <string.h>

void ui_overlay_init(UI_OVERLAY_S *ui)
{
	if (!ui)
		return;
	memset(ui, 0, sizeof(*ui));
	pthread_mutex_init(&ui->lock, NULL);
}

void ui_overlay_set_recording(UI_OVERLAY_S *ui, bool recording, int elapsed_sec)
{
	if (!ui)
		return;
	pthread_mutex_lock(&ui->lock);
	ui->recording = recording;
	ui->elapsed_sec = elapsed_sec;
	pthread_mutex_unlock(&ui->lock);
}

void ui_overlay_get(UI_OVERLAY_S *ui, bool *menu_visible, bool *recording,
		    int *elapsed_sec)
{
	if (!ui)
		return;
	pthread_mutex_lock(&ui->lock);
	if (menu_visible)
		*menu_visible = ui->menu_visible;
	if (recording)
		*recording = ui->recording;
	if (elapsed_sec)
		*elapsed_sec = ui->elapsed_sec;
	pthread_mutex_unlock(&ui->lock);
}

static int point_in_btn(const FB_LCD_BTN_S *btn, int x, int y)
{
	if (!btn)
		return 0;
	return x >= btn->x && x < btn->x + btn->w &&
	       y >= btn->y && y < btn->y + btn->h;
}

ui_cmd_e ui_overlay_handle_touch(UI_OVERLAY_S *ui, const TOUCH_EVT_S *evt)
{
	ui_cmd_e cmd = UI_CMD_NONE;
	bool menu;
	bool recording;
	FB_LCD_BTN_S btn;
	bool btn_valid;

	if (!ui || !evt || evt->type == TOUCH_EVT_NONE)
		return UI_CMD_NONE;

	pthread_mutex_lock(&ui->lock);
	menu = ui->menu_visible;
	recording = ui->recording;
	btn = ui->record_btn;
	btn_valid = ui->btn_valid;

	switch (evt->type) {
	case TOUCH_EVT_SWIPE_UP:
		ui->menu_visible = true;
		break;
	case TOUCH_EVT_SWIPE_DOWN:
		ui->menu_visible = false;
		break;
	case TOUCH_EVT_TAP:
		if (menu) {
			if (evt->y < FB_LCD_MENU_Y0) {
				/* Tap upper half hides menu */
				ui->menu_visible = false;
			} else if (btn_valid && point_in_btn(&btn, evt->x, evt->y)) {
				cmd = UI_CMD_TOGGLE_RECORD;
			}
		}
		(void)recording;
		break;
	default:
		break;
	}
	pthread_mutex_unlock(&ui->lock);

	return cmd;
}

void ui_overlay_draw(UI_OVERLAY_S *ui, FB_LCD_S *fb, int bat_valid, int bat_pct,
		     int temp_valid, int temp_c)
{
	bool menu;
	bool recording;
	int elapsed;
	FB_LCD_BTN_S btn;

	if (!ui || !fb)
		return;

	pthread_mutex_lock(&ui->lock);
	menu = ui->menu_visible;
	recording = ui->recording;
	elapsed = ui->elapsed_sec;
	pthread_mutex_unlock(&ui->lock);

	fb_lcd_draw_status_hud(fb, bat_valid, bat_pct, temp_valid, temp_c);
	fb_lcd_draw_rec_hud(fb, recording ? 1 : 0, elapsed);

	if (menu) {
		fb_lcd_draw_menu(fb, recording ? 1 : 0, &btn);
		pthread_mutex_lock(&ui->lock);
		ui->record_btn = btn;
		ui->btn_valid = true;
		pthread_mutex_unlock(&ui->lock);
	} else {
		pthread_mutex_lock(&ui->lock);
		ui->btn_valid = false;
		pthread_mutex_unlock(&ui->lock);
	}
}
