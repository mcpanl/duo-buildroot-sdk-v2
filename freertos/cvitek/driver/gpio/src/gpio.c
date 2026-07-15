#include <stdio.h>
#ifdef RUN_IN_SRAM
#include "system_common.h"
#endif

#include "mmio.h"
#include "gpio.h"

static uint32_t gpio_base_of(int pin)
{
	switch ((pin >> 8) & 0xff) {
	case 0xB:
		return CVI_GPIOB_BASE;
	case 0xC:
		return CVI_GPIOC_BASE;
	case 0xD:
		return CVI_GPIOD_BASE;
	case 0xA:
		return CVI_GPIOA_BASE;
	default:
		return 0;
	}
}

int gpio_is_valid(int pin)
{
	return gpio_base_of(pin) != 0;
}

void gpio_direction_output(int pin, int val)
{
	uint32_t gpio_base = gpio_base_of(pin);
	uint32_t bit;

	if (!gpio_base)
		return;

	bit = 1u << (pin & 0xff);
	/* SWPORTA_DDR */
	mmio_setbits_32(gpio_base + 4, bit);
	/* SWPORTA_DR */
	if (val)
		mmio_setbits_32(gpio_base, bit);
	else
		mmio_clrbits_32(gpio_base, bit);
}

void gpio_direction_input(int pin)
{
	uint32_t gpio_base = gpio_base_of(pin);
	uint32_t bit;

	if (!gpio_base)
		return;

	bit = 1u << (pin & 0xff);
	mmio_clrbits_32(gpio_base + 4, bit);
}

int gpio_get_value(int pin)
{
	uint32_t gpio_base = gpio_base_of(pin);

	if (!gpio_base)
		return 0;

	/* EXT_PORTA */
	return !!(mmio_read_32(gpio_base + 0x50) & (1u << (pin & 0xff)));
}

void gpio_set_value(int pin, int val)
{
	uint32_t gpio_base = gpio_base_of(pin);
	uint32_t bit;

	if (!gpio_base)
		return;

	bit = 1u << (pin & 0xff);
	if (val)
		mmio_setbits_32(gpio_base, bit);
	else
		mmio_clrbits_32(gpio_base, bit);
}
