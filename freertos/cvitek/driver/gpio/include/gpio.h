#ifndef __GPIO_H__
#define __GPIO_H__

#include <stddef.h>
#include <stdint.h>

#define CVI_GPIOA_BASE		0x03020000
#define CVI_GPIOB_BASE		0x03021000
#define CVI_GPIOC_BASE		0x03022000
#define CVI_GPIOD_BASE		0x03023000

/* Pin encoding: (bank_letter << 8) | pin_index, e.g. GPIOB20 = 0x0B14 */
#define GPIO_PIN(bank, n)	((((bank) & 0xff) << 8) | ((n) & 0xff))
#define GPIOA(n)		GPIO_PIN(0xA, n)
#define GPIOB(n)		GPIO_PIN(0xB, n)
#define GPIOC(n)		GPIO_PIN(0xC, n)
#define GPIOD(n)		GPIO_PIN(0xD, n)

enum of_gpio_flags {
	OF_GPIO_ACTIVE_LOW  = 0x1
};

int gpio_is_valid(int pin);
void gpio_direction_output(int pin, int val);
void gpio_direction_input(int pin);
int gpio_get_value(int pin);
void gpio_set_value(int pin, int val);

#endif
