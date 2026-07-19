#include "touch_input.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SWIPE_MIN_DIST   40
#define SWIPE_AXIS_RATIO 1.2f
#define TAP_MAX_DIST     18

/*
 * Touch reports framebuffer physical coords with Y already flipped relative
 * to drawing; X is mirrored vs display (same as screen_demo.py).
 */
static void touch_to_panel(const TOUCH_INPUT_S *ti, int tx, int ty, int *px, int *py)
{
	*px = ti->panel_w - 1 - tx;
	*py = ti->panel_h - 1 - ty;
	if (*px < 0)
		*px = 0;
	if (*py < 0)
		*py = 0;
	if (*px >= ti->panel_w)
		*px = ti->panel_w - 1;
	if (*py >= ti->panel_h)
		*py = ti->panel_h - 1;
}

static touch_evt_e classify(int x0, int y0, int x1, int y1)
{
	int dx, dy, adx, ady;

	if (x0 < 0 || y0 < 0 || x1 < 0 || y1 < 0)
		return TOUCH_EVT_NONE;

	dx = x1 - x0;
	dy = y1 - y0;
	adx = dx < 0 ? -dx : dx;
	ady = dy < 0 ? -dy : dy;

	if (adx <= TAP_MAX_DIST && ady <= TAP_MAX_DIST)
		return TOUCH_EVT_TAP;

	if (ady >= SWIPE_MIN_DIST && ady > (int)(adx * SWIPE_AXIS_RATIO))
		return (dy < 0) ? TOUCH_EVT_SWIPE_UP : TOUCH_EVT_SWIPE_DOWN;

	if (adx >= SWIPE_MIN_DIST && adx > (int)(ady * SWIPE_AXIS_RATIO))
		return (dx > 0) ? TOUCH_EVT_SWIPE_RIGHT : TOUCH_EVT_SWIPE_LEFT;

	return TOUCH_EVT_NONE;
}

int touch_input_open(TOUCH_INPUT_S *ti, const char *dev, int panel_w, int panel_h)
{
	if (!ti)
		return -1;

	memset(ti, 0, sizeof(*ti));
	ti->fd = -1;
	ti->panel_w = panel_w > 0 ? panel_w : 172;
	ti->panel_h = panel_h > 0 ? panel_h : 320;
	ti->cur_x = -1;
	ti->cur_y = -1;
	ti->start_x = -1;
	ti->start_y = -1;

	if (!dev)
		dev = TOUCH_DEV_DEFAULT;

	ti->fd = open(dev, O_RDONLY | O_NONBLOCK);
	if (ti->fd < 0) {
		perror("open touch");
		return -1;
	}
	return 0;
}

void touch_input_close(TOUCH_INPUT_S *ti)
{
	if (!ti)
		return;
	if (ti->fd >= 0) {
		close(ti->fd);
		ti->fd = -1;
	}
}

bool touch_input_available(const TOUCH_INPUT_S *ti)
{
	return ti && ti->fd >= 0;
}

touch_evt_e touch_input_poll(TOUCH_INPUT_S *ti, TOUCH_EVT_S *out)
{
	struct input_event ev;
	touch_evt_e result = TOUCH_EVT_NONE;
	int rx = -1, ry = -1;
	ssize_t n;

	if (out) {
		out->type = TOUCH_EVT_NONE;
		out->x = 0;
		out->y = 0;
	}

	if (!touch_input_available(ti))
		return TOUCH_EVT_NONE;

	while (1) {
		n = read(ti->fd, &ev, sizeof(ev));
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			break;
		}
		if (n != (ssize_t)sizeof(ev))
			break;

		if (ev.type == EV_ABS) {
			if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) {
				rx = (int)ev.value;
				ti->have_pos = true;
			} else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) {
				ry = (int)ev.value;
				ti->have_pos = true;
			} else if (ev.code == ABS_MT_TRACKING_ID) {
				if (ev.value < 0) {
					/* Finger up */
					if (ti->tracking) {
						int px0, py0, px1, py1;
						touch_evt_e t;

						touch_to_panel(ti, ti->start_x, ti->start_y,
							       &px0, &py0);
						touch_to_panel(ti, ti->cur_x, ti->cur_y,
							       &px1, &py1);
						t = classify(px0, py0, px1, py1);
						if (t != TOUCH_EVT_NONE && result == TOUCH_EVT_NONE) {
							result = t;
							if (out) {
								out->type = t;
								out->x = px1;
								out->y = py1;
							}
						}
					}
					ti->tracking = false;
					ti->have_pos = false;
					ti->start_x = ti->start_y = -1;
					ti->cur_x = ti->cur_y = -1;
				} else if (!ti->tracking) {
					ti->tracking = true;
					if (ti->cur_x >= 0 && ti->cur_y >= 0) {
						ti->start_x = ti->cur_x;
						ti->start_y = ti->cur_y;
					}
				}
			}
		} else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
			if (ev.value == 0 && ti->tracking) {
				int px0, py0, px1, py1;
				touch_evt_e t;

				touch_to_panel(ti, ti->start_x, ti->start_y, &px0, &py0);
				touch_to_panel(ti, ti->cur_x, ti->cur_y, &px1, &py1);
				t = classify(px0, py0, px1, py1);
				if (t != TOUCH_EVT_NONE && result == TOUCH_EVT_NONE) {
					result = t;
					if (out) {
						out->type = t;
						out->x = px1;
						out->y = py1;
					}
				}
				ti->tracking = false;
			} else if (ev.value == 1) {
				ti->tracking = true;
				if (ti->cur_x >= 0 && ti->cur_y >= 0) {
					ti->start_x = ti->cur_x;
					ti->start_y = ti->cur_y;
				}
			}
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			if (rx >= 0)
				ti->cur_x = rx;
			if (ry >= 0)
				ti->cur_y = ry;
			if (ti->tracking && ti->start_x < 0 &&
			    ti->cur_x >= 0 && ti->cur_y >= 0) {
				ti->start_x = ti->cur_x;
				ti->start_y = ti->cur_y;
			}
			rx = ry = -1;
		}
	}

	return result;
}
