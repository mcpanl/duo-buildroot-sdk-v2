/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __RTOS_LED_H__
#define __RTOS_LED_H__

void rtos_led_start(void);
void rtos_led_set_mode(unsigned mode);
unsigned rtos_led_get_mode(void);

#endif
