// SPDX-License-Identifier: GPL-2.0+
/*
 * FB driver for the JD9853 LCD display controller
 *
 * BOE WV015GES-NB80 1.47" 172x320 panel support.
 * Uses 4-wire SPI (8-bit + DC GPIO).
 *
 * Copyright (C) 2013 Christian Vogelgsang
 * Based on adafruit22fb.c by Noralf Tronnes
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <video/mipi_display.h>

#include "fbtft.h"

#define DRVNAME		"fb_jd9853"
#define WIDTH		172
#define HEIGHT		320
#define X_OFFSET	34
#define TXBUFLEN	(4 * PAGE_SIZE)
#define RGB565_BLUE	0x001F

static int init_display_preserve(struct fbtft_par *par)
{
	/* Orientation must match U-Boot jd9853_logo (mirror X + invert) */
	write_reg(par, MIPI_DCS_SET_ADDRESS_MODE, BIT(6));
	write_reg(par, 0x21);
	/* Enable TE output on GPIO (vblank sync pulse) */
	write_reg(par, MIPI_DCS_SET_TEAR_ON, 0x00);
	return 0;
}

static int init_display(struct fbtft_par *par)
{
	if (par->skip_init)
		return init_display_preserve(par);

	par->fbtftops.reset(par);

	if (par->gpio.cs)
		gpiod_set_value(par->gpio.cs, 0);

	/* Init sequence from ESP-IDF vendor_specific_init_default */
	write_reg(par, 0x11);
	mdelay(120);

	write_reg(par, 0xDF, 0x98, 0x53);
	write_reg(par, 0xDF, 0x98, 0x53);
	write_reg(par, 0xB2, 0x23);
	write_reg(par, 0xB7, 0x00, 0x47, 0x00, 0x6F);
	write_reg(par, 0xBB, 0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0);
	write_reg(par, 0xC0, 0x44, 0xA4);
	write_reg(par, 0xC1, 0x16);
	write_reg(par, 0xC3, 0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77);
	write_reg(par, 0xC4, 0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79,
		  0x0B, 0x0A, 0x16, 0x82);
	write_reg(par, 0xC8, 0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
		  0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00, 0x3F,
		  0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26,
		  0x25, 0x17, 0x12, 0x0D, 0x04, 0x00);
	write_reg(par, 0xD0, 0x04, 0x06, 0x6B, 0x0F, 0x00);
	write_reg(par, 0xD7, 0x00, 0x30);
	write_reg(par, 0xE6, 0x14);

	write_reg(par, 0xDE, 0x01);
	write_reg(par, 0xB7, 0x03, 0x13, 0xEF, 0x35, 0x35);
	write_reg(par, 0xC1, 0x14, 0x15, 0xC0);
	write_reg(par, 0xC2, 0x06, 0x3A);
	write_reg(par, 0xC4, 0x72, 0x12);
	write_reg(par, 0xBE, 0x00);

	write_reg(par, 0xDE, 0x02);
	write_reg(par, 0xE5, 0x00, 0x02, 0x00);
	write_reg(par, 0xE5, 0x01, 0x02, 0x00);

	write_reg(par, 0xDE, 0x00);
	write_reg(par, MIPI_DCS_SET_TEAR_ON, 0x00);
	write_reg(par, 0x3A, 0x05);
	write_reg(par, 0x2A, 0x00, 0x22, 0x00, 0xCD);
	write_reg(par, 0x2B, 0x00, 0x00, 0x01, 0x3F);

	write_reg(par, 0xDE, 0x02);
	write_reg(par, 0xE5, 0x00, 0x02, 0x00);
	write_reg(par, 0xDE, 0x00);

	write_reg(par, 0x29);
	mdelay(20);

	/* Match ESP example: mirror X + invert color */
	write_reg(par, MIPI_DCS_SET_ADDRESS_MODE, BIT(6));
	write_reg(par, 0x21);

	/* Fill framebuffer blue for bring-up visibility */
	if (par->info && par->info->screen_base) {
		u16 *fb = (u16 *)par->info->screen_base;
		size_t pixels = par->info->fix.smem_len / sizeof(u16);
		size_t i;

		for (i = 0; i < pixels; i++)
			fb[i] = RGB565_BLUE;
	}

	return 0;
}

static void set_addr_win(struct fbtft_par *par, int xs, int ys, int xe, int ye)
{
	xs += X_OFFSET;
	xe += X_OFFSET;

	write_reg(par, MIPI_DCS_SET_COLUMN_ADDRESS,
		  (xs >> 8) & 0xFF, xs & 0xFF, (xe >> 8) & 0xFF, xe & 0xFF);

	write_reg(par, MIPI_DCS_SET_PAGE_ADDRESS,
		  (ys >> 8) & 0xFF, ys & 0xFF, (ye >> 8) & 0xFF, ye & 0xFF);

	write_reg(par, MIPI_DCS_WRITE_MEMORY_START);
}

#define MEM_Y   BIT(7)
#define MEM_X   BIT(6)
#define MEM_V   BIT(5)
#define MEM_L   BIT(4)
#define MEM_H   BIT(2)
#define MEM_BGR (3)

static int set_var(struct fbtft_par *par)
{
	switch (par->info->var.rotate) {
	case 0:
		write_reg(par, MIPI_DCS_SET_ADDRESS_MODE,
			  MEM_X | (par->bgr << MEM_BGR));
		break;
	case 270:
		write_reg(par, MIPI_DCS_SET_ADDRESS_MODE,
			  MEM_V | MEM_L | (par->bgr << MEM_BGR));
		break;
	case 180:
		write_reg(par, MIPI_DCS_SET_ADDRESS_MODE,
			  MEM_Y | (par->bgr << MEM_BGR));
		break;
	case 90:
		write_reg(par, MIPI_DCS_SET_ADDRESS_MODE,
			  MEM_Y | MEM_X | MEM_V | (par->bgr << MEM_BGR));
		break;
	}

	return 0;
}

static struct fbtft_display display = {
	.regwidth = 8,
	.buswidth = 8,
	.width = WIDTH,
	.height = HEIGHT,
	.txbuflen = TXBUFLEN,
	.fbtftops = {
		.init_display = init_display,
		.set_addr_win = set_addr_win,
		.set_var = set_var,
	},
};

FBTFT_REGISTER_DRIVER(DRVNAME, "jadard,jd9853", &display);

MODULE_ALIAS("spi:" DRVNAME);
MODULE_ALIAS("platform:" DRVNAME);
MODULE_ALIAS("spi:jd9853");
MODULE_ALIAS("platform:jd9853");

MODULE_DESCRIPTION("FB driver for the JD9853 LCD display controller");
MODULE_AUTHOR("Christian Vogelgsang");
MODULE_LICENSE("GPL");
