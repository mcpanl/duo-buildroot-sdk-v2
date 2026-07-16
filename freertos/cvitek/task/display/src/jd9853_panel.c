/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <string.h>
#include "gpio.h"
#include "delay.h"
#include "mmio.h"
#include "pinctrl.h"
#include "hal_spi3.h"
#include "display_shm.h"
#include "jd9853_panel.h"

#define DCS_SLPOUT		0x11
#define DCS_DISPON		0x29
#define DCS_CASET		0x2a
#define DCS_PASET		0x2b
#define DCS_RAMWR		0x2c
#define DCS_MADCTL		0x36
#define DCS_COLMOD		0x3a
#define DCS_INVOFF		0x20
#define DCS_INVON		0x21
#define DCS_TEON		0x35

/* Match Linux fb_jd9853 / U-Boot jd9853_logo: MADCTL MX/MY + color invert */
#define JD9853_MADCTL_MX	0x40u
#define JD9853_MADCTL_MY	0x80u

static uint8_t g_mirror_x = LCD_MIRROR_X_DEFAULT;
static uint8_t g_mirror_y = LCD_MIRROR_Y_DEFAULT;

#define PIN_DC			GPIOB(20)
#define PIN_RST			GPIOB(12)
#define PIN_TE			GPIOB(11)
#define PIN_BL			GPIOA(20)

#define SPI_HZ			40000000
#define WRBUF_BYTES		4096

static uint8_t wrbuf[WRBUF_BYTES];

static int jd9853_write_cmd(uint8_t cmd)
{
	gpio_set_value(PIN_DC, 0);
	return hal_spi3_xfer(&cmd, 1);
}

static int jd9853_write_data(const void *buf, size_t len)
{
	gpio_set_value(PIN_DC, 1);
	return hal_spi3_xfer(buf, len);
}

static int jd9853_write_reg(uint8_t cmd, const uint8_t *data, size_t len)
{
	int ret = jd9853_write_cmd(cmd);

	if (ret)
		return ret;
	if (len)
		ret = jd9853_write_data(data, len);
	return ret;
}

#define NUMARGS(...) (sizeof((uint8_t[]){ __VA_ARGS__ }))
#define WR_REG(cmd, ...) \
	jd9853_write_reg(cmd, (uint8_t[]){ __VA_ARGS__ }, NUMARGS(__VA_ARGS__))

static void jd9853_gpio_setup(void)
{
	/* Match U-Boot pinmux for control GPIOs */
	PINMUX_CONFIG(VIVO_D1, XGPIOB_20);	/* DC */
	PINMUX_CONFIG(VIVO_D9, XGPIOB_12);	/* RST */
	PINMUX_CONFIG(VIVO_D10, XGPIOB_11);	/* TE */
	PINMUX_CONFIG(JTAG_CPU_TRST, XGPIOA_20); /* BL */

	gpio_direction_output(PIN_DC, 1);
	gpio_direction_output(PIN_RST, 1); /* physical high (deasserted) */
	gpio_direction_output(PIN_BL, 0);
	gpio_direction_input(PIN_TE);
}

static void jd9853_reset_panel(void)
{
	/* Physical: idle high -> assert low -> idle high */
	gpio_set_value(PIN_RST, 1);
	udelay(30);
	gpio_set_value(PIN_RST, 0);
	mdelay(120);
	gpio_set_value(PIN_RST, 1);
	mdelay(10);
}

static int jd9853_full_init(void)
{
	int ret;

	jd9853_reset_panel();

	ret = jd9853_write_cmd(DCS_SLPOUT);
	if (ret)
		return ret;
	mdelay(120);

	ret = WR_REG(0xDF, 0x98, 0x53);
	if (ret)
		return ret;
	ret = WR_REG(0xDF, 0x98, 0x53);
	if (ret)
		return ret;
	ret = WR_REG(0xB2, 0x23);
	if (ret)
		return ret;
	ret = WR_REG(0xB7, 0x00, 0x47, 0x00, 0x6F);
	if (ret)
		return ret;
	ret = WR_REG(0xBB, 0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0);
	if (ret)
		return ret;
	ret = WR_REG(0xC0, 0x44, 0xA4);
	if (ret)
		return ret;
	ret = WR_REG(0xC1, 0x16);
	if (ret)
		return ret;
	ret = WR_REG(0xC3, 0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77);
	if (ret)
		return ret;
	ret = WR_REG(0xC4, 0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79,
		     0x0B, 0x0A, 0x16, 0x82);
	if (ret)
		return ret;
	ret = WR_REG(0xC8, 0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
		     0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00, 0x3F,
		     0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26,
		     0x25, 0x17, 0x12, 0x0D, 0x04, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(0xD0, 0x04, 0x06, 0x6B, 0x0F, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(0xD7, 0x00, 0x30);
	if (ret)
		return ret;
	ret = WR_REG(0xE6, 0x14);
	if (ret)
		return ret;
	ret = WR_REG(0xDE, 0x01);
	if (ret)
		return ret;
	ret = WR_REG(0xB7, 0x03, 0x13, 0xEF, 0x35, 0x35);
	if (ret)
		return ret;
	ret = WR_REG(0xC1, 0x14, 0x15, 0xC0);
	if (ret)
		return ret;
	ret = WR_REG(0xC2, 0x06, 0x3A);
	if (ret)
		return ret;
	ret = WR_REG(0xC4, 0x72, 0x12);
	if (ret)
		return ret;
	ret = WR_REG(0xBE, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(0xDE, 0x02);
	if (ret)
		return ret;
	ret = WR_REG(0xE5, 0x00, 0x02, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(0xE5, 0x01, 0x02, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(0xDE, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(DCS_TEON, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(DCS_COLMOD, 0x05);
	if (ret)
		return ret;
	ret = WR_REG(DCS_CASET, 0x00, 0x22, 0x00, 0xCD);
	if (ret)
		return ret;
	ret = WR_REG(DCS_PASET, 0x00, 0x00, 0x01, 0x3F);
	if (ret)
		return ret;
	ret = WR_REG(0xDE, 0x02);
	if (ret)
		return ret;
	ret = WR_REG(0xE5, 0x00, 0x02, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(0xDE, 0x00);
	if (ret)
		return ret;
	ret = jd9853_write_cmd(DCS_DISPON);
	if (ret)
		return ret;
	mdelay(20);
	return jd9853_apply_orientation();
}

/*
 * Re-apply panel scan order after SPI controller re-init.
 * Use INVOFF then INVON so repeated skip-init cannot leave inversion in an
 * unknown toggled state (RTOS path re-opens DW SSI after U-Boot).
 */
int jd9853_apply_orientation(void)
{
	uint8_t madctl = 0;
	int ret;

	if (g_mirror_x)
		madctl |= JD9853_MADCTL_MX;
	if (g_mirror_y)
		madctl |= JD9853_MADCTL_MY;

	ret = WR_REG(DCS_MADCTL, madctl);
	if (ret)
		return ret;
	ret = jd9853_write_cmd(DCS_INVOFF);
	if (ret)
		return ret;
	ret = jd9853_write_cmd(DCS_INVON);
	if (ret)
		return ret;
	return WR_REG(DCS_TEON, 0x00);
}

void jd9853_set_mirror(uint8_t mirror_x, uint8_t mirror_y)
{
	g_mirror_x = mirror_x ? 1 : 0;
	g_mirror_y = mirror_y ? 1 : 0;
}

void jd9853_get_mirror(uint8_t *mirror_x, uint8_t *mirror_y)
{
	if (mirror_x)
		*mirror_x = g_mirror_x;
	if (mirror_y)
		*mirror_y = g_mirror_y;
}

static int jd9853_skip_init(void)
{
	return jd9853_apply_orientation();
}

int jd9853_panel_init(int skip_init)
{
	int ret;

	jd9853_gpio_setup();
	ret = hal_spi3_init(SPI_HZ);
	if (ret)
		return ret;

	if (skip_init) {
		printf("jd9853: skip-init (preserve U-Boot state)\n");
		ret = jd9853_skip_init();
	} else {
		printf("jd9853: full panel init\n");
		ret = jd9853_full_init();
	}
	if (ret) {
		printf("jd9853: init failed (%d)\n", ret);
		return ret;
	}

	jd9853_set_backlight(1);
	printf("jd9853: ready %dx%d\n", JD9853_WIDTH, JD9853_HEIGHT);
	return 0;
}

int jd9853_set_addr_win(int xs, int ys, int xe, int ye)
{
	uint8_t caset[4], paset[4];
	int ret;

	xs += JD9853_X_OFFSET;
	xe += JD9853_X_OFFSET;

	caset[0] = (xs >> 8) & 0xff;
	caset[1] = xs & 0xff;
	caset[2] = (xe >> 8) & 0xff;
	caset[3] = xe & 0xff;
	paset[0] = (ys >> 8) & 0xff;
	paset[1] = ys & 0xff;
	paset[2] = (ye >> 8) & 0xff;
	paset[3] = ye & 0xff;

	ret = jd9853_write_reg(DCS_CASET, caset, sizeof(caset));
	if (ret)
		return ret;
	ret = jd9853_write_reg(DCS_PASET, paset, sizeof(paset));
	if (ret)
		return ret;
	return jd9853_write_cmd(DCS_RAMWR);
}

static uint16_t cpu_to_be16_u(uint16_t v)
{
	return (uint16_t)((v << 8) | (v >> 8));
}

static int jd9853_write_pixels_be_linear(const uint16_t *src, size_t pixels)
{
	size_t off = 0;
	int ret;

	gpio_set_value(PIN_DC, 1);
	while (off < pixels) {
		size_t i, n = WRBUF_BYTES / 2;

		if (n > pixels - off)
			n = pixels - off;
		for (i = 0; i < n; i++)
			((uint16_t *)wrbuf)[i] = cpu_to_be16_u(src[off + i]);

		ret = hal_spi3_xfer(wrbuf, n * 2);
		if (ret)
			return ret;
		off += n;
	}
	return 0;
}

int jd9853_write_pixels_be(const uint16_t *src, size_t pixels)
{
	return jd9853_write_pixels_be_linear(src, pixels);
}

int jd9853_fill_color(uint16_t rgb565)
{
	size_t pixels = (size_t)JD9853_WIDTH * JD9853_HEIGHT;
	size_t off = 0;
	uint16_t be = cpu_to_be16_u(rgb565);
	int ret;

	ret = jd9853_set_addr_win(0, 0, JD9853_WIDTH - 1, JD9853_HEIGHT - 1);
	if (ret)
		return ret;

	gpio_set_value(PIN_DC, 1);
	while (off < pixels) {
		size_t i, n = WRBUF_BYTES / 2;

		if (n > pixels - off)
			n = pixels - off;
		for (i = 0; i < n; i++)
			((uint16_t *)wrbuf)[i] = be;
		ret = hal_spi3_xfer(wrbuf, n * 2);
		if (ret)
			return ret;
		off += n;
	}
	return 0;
}

void jd9853_set_backlight(int on)
{
	gpio_set_value(PIN_BL, on ? 1 : 0);
}

void jd9853_wait_te(void)
{
	/* Align with Linux fbtft_wait_te(): exit VBANK then wait rising TE */
	unsigned int guard = 50000;

	while (gpio_get_value(PIN_TE) && guard--)
		udelay(100);
	guard = 50000;
	while (!gpio_get_value(PIN_TE) && guard--)
		udelay(100);
}
