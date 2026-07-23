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
#include <linux/string.h>
#include <linux/device.h>
#include <video/mipi_display.h>

#include "fbtft.h"

#define DRVNAME		"fb_jd9853"
#define WIDTH		172
#define HEIGHT		320
#define X_OFFSET	34
#define TXBUFLEN	(4 * PAGE_SIZE)
#define RGB565_BLUE	0x001F
#define LCD_MIRROR_X_DEFAULT	1
#define LCD_MIRROR_Y_DEFAULT	0

static int g_mirror_x = LCD_MIRROR_X_DEFAULT;
static int g_mirror_y = LCD_MIRROR_Y_DEFAULT;
static struct spi_device *g_jd9853_spi;

static int zonhor_mirror_bootarg(const char *name, int defval)
{
	const char *s = strstr(saved_command_line, name);

	if (!s)
		return defval;
	s += strlen(name);
	return (*s == '1') ? 1 : 0;
}

static void jd9853_apply_mirror_hw(struct fbtft_par *par)
{
	u8 madctl = 0;

	if (g_mirror_x)
		madctl |= BIT(6);
	if (g_mirror_y)
		madctl |= BIT(7);

	write_reg(par, MIPI_DCS_SET_ADDRESS_MODE, madctl);
	write_reg(par, 0x21);
}

static bool jd9853_te_is_alive(struct fbtft_par *par)
{
	unsigned int i, high = 0;

	if (!par->gpio.te)
		return true;

	/*
	 * TE is mostly low; a healthy panel produces brief highs ~once per
	 * frame. Sample for ~50ms. Linux SPI controller reset after U-Boot
	 * often leaves the panel without a working TEON state even when
	 * skip-init was requested.
	 */
	for (i = 0; i < 50; i++) {
		if (gpiod_get_value_cansleep(par->gpio.te))
			high++;
		usleep_range(1000, 1200);
	}
	return high > 0;
}

static int init_display_preserve(struct fbtft_par *par)
{
	/* Orientation from U-Boot env via kernel cmdline */
	write_reg(par, MIPI_DCS_SET_TEAR_ON, 0x00);
	jd9853_apply_mirror_hw(par);
	return 0;
}

static int init_display_full(struct fbtft_par *par)
{
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

	/* Match saved mirror settings */
	jd9853_apply_mirror_hw(par);

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

static int init_display(struct fbtft_par *par)
{
	if (par->skip_init) {
		init_display_preserve(par);
		if (jd9853_te_is_alive(par))
			return 0;
		dev_warn(par->info->device,
			 "TE inactive after skip-init; falling back to full panel init\n");
	}

	init_display_full(par);
	if (!jd9853_te_is_alive(par)) {
		dev_warn(par->info->device,
			 "TE inactive after panel init; retrying full init\n");
		init_display_full(par);
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
	u8 madctl = 0;

	switch (par->info->var.rotate) {
	case 0:
		if (g_mirror_x)
			madctl |= MEM_X;
		if (g_mirror_y)
			madctl |= MEM_Y;
		madctl |= (par->bgr << MEM_BGR);
		write_reg(par, MIPI_DCS_SET_ADDRESS_MODE, madctl);
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

/* Default owner is RTOS (SPI owned by C906L). Only probe when linux owns SPI. */
static bool zonhor_lcd_owner_is_linux(void)
{
	const char *s = strstr(saved_command_line, "cvi.lcd_owner=");

	if (!s)
		return false;
	s += strlen("cvi.lcd_owner=");
	return strncmp(s, "linux", 5) == 0;
}

static struct fbtft_par *jd9853_get_par(void)
{
	struct fb_info *info;

	if (!g_jd9853_spi)
		return NULL;
	info = spi_get_drvdata(g_jd9853_spi);
	if (!info)
		return NULL;
	return info->par;
}

static ssize_t mirror_x_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "%d\n", g_mirror_x);
}

static ssize_t mirror_x_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct fbtft_par *par;
	int v;

	if (kstrtoint(buf, 10, &v))
		return -EINVAL;
	g_mirror_x = v ? 1 : 0;
	par = jd9853_get_par();
	if (par) {
		jd9853_apply_mirror_hw(par);
		par->fbtftops.update_display(par, 0, par->info->var.yres - 1);
	}
	return count;
}
static DEVICE_ATTR_RW(mirror_x);

static ssize_t mirror_y_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "%d\n", g_mirror_y);
}

static ssize_t mirror_y_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct fbtft_par *par;
	int v;

	if (kstrtoint(buf, 10, &v))
		return -EINVAL;
	g_mirror_y = v ? 1 : 0;
	par = jd9853_get_par();
	if (par) {
		jd9853_apply_mirror_hw(par);
		par->fbtftops.update_display(par, 0, par->info->var.yres - 1);
	}
	return count;
}
static DEVICE_ATTR_RW(mirror_y);

static int fbtft_driver_probe_spi(struct spi_device *spi)
{
	int ret;

	if (!zonhor_lcd_owner_is_linux()) {
		dev_info(&spi->dev,
			 "fb_jd9853 skipped (cvi.lcd_owner!=linux, RTOS owns SPI)\n");
		return -ENODEV;
	}

	g_mirror_x = zonhor_mirror_bootarg("cvi.lcd_mirror_x=", LCD_MIRROR_X_DEFAULT);
	g_mirror_y = zonhor_mirror_bootarg("cvi.lcd_mirror_y=", LCD_MIRROR_Y_DEFAULT);

	ret = fbtft_probe_common(&display, spi, NULL);
	if (ret)
		return ret;

	g_jd9853_spi = spi;
	device_create_file(&spi->dev, &dev_attr_mirror_x);
	device_create_file(&spi->dev, &dev_attr_mirror_y);
	dev_info(&spi->dev, "mirror_x=%d mirror_y=%d\n", g_mirror_x, g_mirror_y);
	return 0;
}

static int fbtft_driver_remove_spi(struct spi_device *spi)
{
	struct fb_info *info = spi_get_drvdata(spi);

	device_remove_file(&spi->dev, &dev_attr_mirror_x);
	device_remove_file(&spi->dev, &dev_attr_mirror_y);
	if (g_jd9853_spi == spi)
		g_jd9853_spi = NULL;
	return fbtft_remove_common(&spi->dev, info);
}

static const struct of_device_id dt_ids[] = {
	{ .compatible = "jadard,jd9853" },
	{},
};
MODULE_DEVICE_TABLE(of, dt_ids);

static struct spi_driver fbtft_driver_spi_driver = {
	.driver = {
		.name   = DRVNAME,
		.of_match_table = of_match_ptr(dt_ids),
	},
	.probe  = fbtft_driver_probe_spi,
	.remove = fbtft_driver_remove_spi,
};

static int __init fbtft_driver_module_init(void)
{
	return spi_register_driver(&fbtft_driver_spi_driver);
}

static void __exit fbtft_driver_module_exit(void)
{
	spi_unregister_driver(&fbtft_driver_spi_driver);
}

module_init(fbtft_driver_module_init);
module_exit(fbtft_driver_module_exit);

MODULE_ALIAS("spi:" DRVNAME);
MODULE_ALIAS("spi:jd9853");

MODULE_DESCRIPTION("FB driver for the JD9853 LCD display controller");
MODULE_AUTHOR("Christian Vogelgsang");
MODULE_LICENSE("GPL");
