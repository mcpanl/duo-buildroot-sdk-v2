/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __JD9853_PANEL_H__
#define __JD9853_PANEL_H__

#include <stddef.h>
#include <stdint.h>

#define JD9853_WIDTH		172
#define JD9853_HEIGHT		320
#define JD9853_X_OFFSET		34

int jd9853_panel_init(int skip_init);
int jd9853_apply_orientation(void);
void jd9853_set_mirror(uint8_t mirror_x, uint8_t mirror_y);
void jd9853_get_mirror(uint8_t *mirror_x, uint8_t *mirror_y);
int jd9853_set_addr_win(int xs, int ys, int xe, int ye);
int jd9853_write_pixels_be(const uint16_t *src, size_t pixels);
int jd9853_fill_color(uint16_t rgb565);
void jd9853_bl_pwm_init(void);
void jd9853_set_backlight(int on);
void jd9853_set_backlight_level(unsigned level); /* 0..100 */
unsigned jd9853_get_backlight_level(void);
void jd9853_wait_te(void);

#endif
