#ifndef TOUCH_INPUT_H
#define TOUCH_INPUT_H

#include <stdbool.h>

#define TOUCH_DEV_DEFAULT "/dev/input/event2"

typedef enum {
	TOUCH_EVT_NONE = 0,
	TOUCH_EVT_TAP,
	TOUCH_EVT_SWIPE_UP,
	TOUCH_EVT_SWIPE_DOWN,
	TOUCH_EVT_SWIPE_LEFT,
	TOUCH_EVT_SWIPE_RIGHT,
} touch_evt_e;

typedef struct TOUCH_EVT_S {
	touch_evt_e type;
	int x; /* panel logical coords (0..W-1, 0..H-1 top-left) */
	int y;
} TOUCH_EVT_S;

typedef struct TOUCH_INPUT_S {
	int fd;
	int panel_w;
	int panel_h;
	int cur_x;
	int cur_y;
	int start_x;
	int start_y;
	bool tracking;
	bool have_pos;
} TOUCH_INPUT_S;

int touch_input_open(TOUCH_INPUT_S *ti, const char *dev, int panel_w, int panel_h);
void touch_input_close(TOUCH_INPUT_S *ti);
bool touch_input_available(const TOUCH_INPUT_S *ti);

/* Non-blocking: drain evdev, return one classified gesture or NONE. */
touch_evt_e touch_input_poll(TOUCH_INPUT_S *ti, TOUCH_EVT_S *out);

#endif /* TOUCH_INPUT_H */
