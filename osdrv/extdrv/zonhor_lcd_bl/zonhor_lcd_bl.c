// SPDX-License-Identifier: GPL-2.0
/*
 * Zonhor JD9853 LCD backlight: Linux sysfs proxy → FreeRTOS soft-PWM.
 *
 * Userspace keeps /sys/class/backlight/zonhor-lcd-bl (0..100). This driver
 * does NOT drive GPIOA20; it sends DISPLAY_CMD_BL (param_ptr = duty 0..100)
 * to C906L FreeRTOS, which bit-bangs soft-PWM on PAD_JTAG_CPU_TRST.
 *
 * Module params:
 *   brightness=<0..100>   default duty after probe
 *   pwm_hz=<Hz>           kept for ABI; ignored (RTOS uses fixed 1 kHz)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/backlight.h>
#include <linux/fb.h>
#include <linux/slab.h>
#include <linux/io.h>

#include "display_shm.h"
#include "rtos_cmdqu.h"

#define DRVNAME			"zonhor_lcd_bl"
#define BL_NAME			"zonhor-lcd-bl"
#define BL_MAX			100
#define BL_DEFAULT		60

struct zonhor_bl {
	struct device *dev;
	struct backlight_device *bd;
	unsigned int brightness;	/* requested 0..100 */
};

static int param_brightness = BL_DEFAULT;
static int param_pwm_hz = 1000;	/* ABI compatibility only */

module_param_named(brightness, param_brightness, int, 0444);
MODULE_PARM_DESC(brightness, "Initial brightness 0..100");
module_param_named(pwm_hz, param_pwm_hz, int, 0444);
MODULE_PARM_DESC(pwm_hz, "Unused (RTOS soft-PWM frequency is fixed)");

static struct platform_device *zonhor_bl_pdev;
static struct zonhor_bl *g_bl;

/*
 * display_shm header shares one 64B cache line with RTOS heartbeat. Publish
 * the full line after Linux updates bl_on/bl_level so C906L does not
 * resurrect a stale duty from its next clean_dcache_range().
 */
static void zonhor_bl_publish_hdr(struct display_shm *shm)
{
	u8 tmp[64];

	memcpy(tmp, shm, sizeof(tmp));
	memcpy(shm, tmp, sizeof(tmp));
	mb();
}

static void zonhor_bl_send_rtos(unsigned int level)
{
	cmdqu_t cmdq = { 0 };
	struct display_shm *shm;

	if (level > BL_MAX)
		level = BL_MAX;

	/* Also poke SHM so RTOS can apply BL if mailbox slots are exhausted. */
	shm = memremap(CVIMMAP_DISPLAY_SHM_ADDR, sizeof(*shm), MEMREMAP_WT);
	if (!shm)
		shm = memremap(CVIMMAP_DISPLAY_SHM_ADDR, sizeof(*shm), MEMREMAP_WC);
	if (shm) {
		if (shm->magic == DISPLAY_SHM_MAGIC) {
			shm->bl_level = level;
			shm->bl_on = level ? 1 : 0;
			zonhor_bl_publish_hdr(shm);
		}
		memunmap(shm);
	}

	cmdq.ip_id = IP_DISPLAY;
	cmdq.cmd_id = DISPLAY_CMD_BL;
	cmdq.resv.valid.linux_valid = 1;
	cmdq.param_ptr = level;
	(void)rtos_cmdqu_send(&cmdq);
}

static unsigned int zonhor_bl_effective_bri(struct zonhor_bl *bl)
{
	unsigned int bri = bl->brightness;

	if (bl->bd) {
		if (bl->bd->props.power != FB_BLANK_UNBLANK ||
		    bl->bd->props.fb_blank != FB_BLANK_UNBLANK ||
		    (bl->bd->props.state & (BL_CORE_FBBLANK | BL_CORE_SUSPENDED)))
			bri = 0;
	}
	return bri;
}

static void zonhor_bl_apply(struct zonhor_bl *bl)
{
	zonhor_bl_send_rtos(zonhor_bl_effective_bri(bl));
}

/* Optional API for zonhor_lcd_proxy fb_blank */
int zonhor_lcd_bl_set_enable(int on)
{
	struct zonhor_bl *bl = g_bl;

	if (!bl || !bl->bd)
		return -ENODEV;

	bl->bd->props.power = on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN;
	zonhor_bl_apply(bl);
	return 0;
}
EXPORT_SYMBOL_GPL(zonhor_lcd_bl_set_enable);

int zonhor_lcd_bl_set_brightness(int brightness)
{
	struct zonhor_bl *bl = g_bl;

	if (!bl || !bl->bd)
		return -ENODEV;
	if (brightness < 0)
		brightness = 0;
	if (brightness > BL_MAX)
		brightness = BL_MAX;
	bl->bd->props.brightness = brightness;
	return backlight_update_status(bl->bd);
}
EXPORT_SYMBOL_GPL(zonhor_lcd_bl_set_brightness);

static int zonhor_bl_update_status(struct backlight_device *bd)
{
	struct zonhor_bl *bl = bl_get_data(bd);
	int bri = backlight_get_brightness(bd);

	if (bri < 0)
		bri = 0;
	if (bri > BL_MAX)
		bri = BL_MAX;

	bl->brightness = bri;
	zonhor_bl_apply(bl);
	return 0;
}

static int zonhor_bl_get_brightness(struct backlight_device *bd)
{
	struct zonhor_bl *bl = bl_get_data(bd);

	return bl->brightness;
}

static const struct backlight_ops zonhor_bl_ops = {
	.update_status = zonhor_bl_update_status,
	.get_brightness = zonhor_bl_get_brightness,
};

static int zonhor_bl_probe_common(struct device *dev, struct zonhor_bl *bl)
{
	struct backlight_properties props = { };
	int bri = param_brightness;
	int ret;

	if (bri < 0)
		bri = 0;
	if (bri > BL_MAX)
		bri = BL_MAX;

	if (dev->of_node) {
		u32 def_bri = bri;

		if (!of_property_read_u32(dev->of_node, "default-brightness",
					  &def_bri) &&
		    def_bri <= BL_MAX)
			bri = def_bri;
	}

	bl->dev = dev;
	bl->brightness = bri;

	props.type = BACKLIGHT_RAW;
	props.max_brightness = BL_MAX;
	props.brightness = bri;
	props.power = FB_BLANK_UNBLANK;

	bl->bd = devm_backlight_device_register(dev, BL_NAME, dev, bl,
					       &zonhor_bl_ops, &props);
	if (IS_ERR(bl->bd)) {
		ret = PTR_ERR(bl->bd);
		dev_err(dev, "backlight register failed: %d\n", ret);
		return ret;
	}

	g_bl = bl;
	backlight_update_status(bl->bd);
	dev_info(dev,
		 "%s: RTOS soft-PWM via mailbox, brightness=%u/100\n",
		 BL_NAME, bl->brightness);
	return 0;
}

static int zonhor_bl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zonhor_bl *bl;

	bl = devm_kzalloc(dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;

	platform_set_drvdata(pdev, bl);
	return zonhor_bl_probe_common(dev, bl);
}

static int zonhor_bl_remove(struct platform_device *pdev)
{
	struct zonhor_bl *bl = platform_get_drvdata(pdev);

	if (!bl)
		return 0;

	zonhor_bl_send_rtos(0);
	if (g_bl == bl)
		g_bl = NULL;
	return 0;
}

static const struct of_device_id zonhor_bl_of_match[] = {
	{ .compatible = "zonhor,lcd-bl" },
	{ .compatible = "cvitek,zonhor-lcd-bl" },
	{},
};
MODULE_DEVICE_TABLE(of, zonhor_bl_of_match);

static struct platform_driver zonhor_bl_driver = {
	.probe = zonhor_bl_probe,
	.remove = zonhor_bl_remove,
	.driver = {
		.name = DRVNAME,
		.of_match_table = zonhor_bl_of_match,
	},
};

static int __init zonhor_bl_init(void)
{
	int ret;
	struct device_node *np;

	ret = platform_driver_register(&zonhor_bl_driver);
	if (ret)
		return ret;

	/*
	 * Bring-up without DTB: insmod cv181x_zonhor_lcd_bl.ko
	 * Creates a non-OF platform device (brightness= module param).
	 */
	np = of_find_compatible_node(NULL, NULL, "zonhor,lcd-bl");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "cvitek,zonhor-lcd-bl");
	if (np) {
		of_node_put(np);
		return 0;
	}

	zonhor_bl_pdev = platform_device_register_simple(DRVNAME, -1, NULL, 0);
	if (IS_ERR(zonhor_bl_pdev)) {
		ret = PTR_ERR(zonhor_bl_pdev);
		zonhor_bl_pdev = NULL;
		platform_driver_unregister(&zonhor_bl_driver);
		return ret;
	}
	return 0;
}

static void __exit zonhor_bl_exit(void)
{
	if (zonhor_bl_pdev)
		platform_device_unregister(zonhor_bl_pdev);
	platform_driver_unregister(&zonhor_bl_driver);
}

module_init(zonhor_bl_init);
module_exit(zonhor_bl_exit);

MODULE_AUTHOR("Zonhor");
MODULE_DESCRIPTION("Zonhor LCD backlight mailbox proxy (RTOS soft-PWM)");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: cv181x_rtos_cmdqu");
