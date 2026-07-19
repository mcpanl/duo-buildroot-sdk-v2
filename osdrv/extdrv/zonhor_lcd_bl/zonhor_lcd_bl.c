// SPDX-License-Identifier: GPL-2.0
/*
 * Zonhor JD9853 LCD backlight: GPIO soft-PWM under Linux.
 *
 * Works for both cvi.lcd_owner=rtos (proxy fb) and linux (fbtft): the BL pin
 * (PAD_JTAG_CPU_TRST / GPIOA20, active-high) is owned here so userspace always
 * has /sys/class/backlight/zonhor-lcd-bl with 0..100 brightness.
 *
 * Module params (no DTB update needed for bring-up):
 *   gpio=<linux gpio number>   e.g. 500 for GPIOA20 on this board
 *   brightness=<0..100>        default duty after probe
 *   pwm_hz=<Hz>                soft-PWM frequency (default 1000)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio.h>
#include <linux/backlight.h>
#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/fb.h>

#define DRVNAME			"zonhor_lcd_bl"
#define BL_NAME			"zonhor-lcd-bl"
#define BL_MAX			100
#define BL_DEFAULT		60
#define PWM_HZ_DEFAULT		1000

struct zonhor_bl {
	struct device *dev;
	struct gpio_desc *gpiod;
	struct backlight_device *bd;
	struct hrtimer timer;
	spinlock_t lock;
	unsigned int pwm_hz;
	unsigned int period_ns;
	unsigned int brightness;	/* requested 0..100 */
	unsigned int duty_ns;		/* high time within period */
	bool pin_high;
	bool running;
	bool gpio_is_desc;		/* true if from gpiod_get */
	int gpio_num;			/* legacy gpio when !gpio_is_desc */
};

static int param_gpio = -1;
static int param_brightness = BL_DEFAULT;
static int param_pwm_hz = PWM_HZ_DEFAULT;

module_param_named(gpio, param_gpio, int, 0444);
MODULE_PARM_DESC(gpio, "Linux GPIO number for BL (e.g. 500 = GPIOA20)");
module_param_named(brightness, param_brightness, int, 0444);
MODULE_PARM_DESC(brightness, "Initial brightness 0..100");
module_param_named(pwm_hz, param_pwm_hz, int, 0444);
MODULE_PARM_DESC(pwm_hz, "Soft-PWM frequency in Hz");

static struct platform_device *zonhor_bl_pdev;
static struct zonhor_bl *g_bl;

static void zonhor_bl_apply(struct zonhor_bl *bl);

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

static void zonhor_bl_set_pin(struct zonhor_bl *bl, bool high)
{
	if (bl->gpio_is_desc)
		gpiod_set_value(bl->gpiod, high ? 1 : 0);
	else
		gpio_set_value(bl->gpio_num, high ? 1 : 0);
	bl->pin_high = high;
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

/*
 * Update pin / soft-PWM. Never call hrtimer_cancel while holding bl->lock
 * (timer callback also takes the lock).
 */
static void zonhor_bl_apply(struct zonhor_bl *bl)
{
	unsigned long flags;
	unsigned int bri;
	bool need_cancel = false;
	bool need_start = false;
	unsigned int start_ns = 0;

	spin_lock_irqsave(&bl->lock, flags);
	bri = zonhor_bl_effective_bri(bl);

	if (bri == 0) {
		need_cancel = bl->running;
		bl->running = false;
		bl->duty_ns = 0;
		zonhor_bl_set_pin(bl, false);
	} else if (bri >= BL_MAX) {
		need_cancel = bl->running;
		bl->running = false;
		bl->duty_ns = bl->period_ns;
		zonhor_bl_set_pin(bl, true);
	} else {
		bl->duty_ns = (bl->period_ns / BL_MAX) * bri;
		if (!bl->duty_ns)
			bl->duty_ns = 1;
		if (!bl->running) {
			bl->running = true;
			bl->pin_high = false;
			zonhor_bl_set_pin(bl, true);
			need_start = true;
			start_ns = bl->duty_ns;
		}
	}
	spin_unlock_irqrestore(&bl->lock, flags);

	if (need_cancel)
		hrtimer_cancel(&bl->timer);
	if (need_start)
		hrtimer_start(&bl->timer, ns_to_ktime(start_ns), HRTIMER_MODE_REL);
}

static enum hrtimer_restart zonhor_bl_timer(struct hrtimer *t)
{
	struct zonhor_bl *bl = container_of(t, struct zonhor_bl, timer);
	unsigned long flags;
	ktime_t next;

	spin_lock_irqsave(&bl->lock, flags);
	if (!bl->running) {
		spin_unlock_irqrestore(&bl->lock, flags);
		return HRTIMER_NORESTART;
	}

	if (bl->pin_high) {
		zonhor_bl_set_pin(bl, false);
		next = ns_to_ktime(bl->period_ns - bl->duty_ns);
	} else {
		zonhor_bl_set_pin(bl, true);
		next = ns_to_ktime(bl->duty_ns);
	}
	hrtimer_forward_now(t, next);
	spin_unlock_irqrestore(&bl->lock, flags);
	return HRTIMER_RESTART;
}

static int zonhor_bl_update_status(struct backlight_device *bd)
{
	struct zonhor_bl *bl = bl_get_data(bd);
	unsigned long flags;
	int bri = backlight_get_brightness(bd);

	if (bri < 0)
		bri = 0;
	if (bri > BL_MAX)
		bri = BL_MAX;

	spin_lock_irqsave(&bl->lock, flags);
	bl->brightness = bri;
	spin_unlock_irqrestore(&bl->lock, flags);
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
	int hz = param_pwm_hz;
	int ret;

	if (bri < 0)
		bri = 0;
	if (bri > BL_MAX)
		bri = BL_MAX;
	if (hz < 50)
		hz = 50;
	if (hz > 5000)
		hz = 5000;

	if (dev->of_node) {
		u32 def_bri = bri;

		if (!of_property_read_u32(dev->of_node, "default-brightness",
					  &def_bri) &&
		    def_bri <= BL_MAX)
			bri = def_bri;
	}

	spin_lock_init(&bl->lock);
	bl->dev = dev;
	bl->pwm_hz = hz;
	bl->period_ns = NSEC_PER_SEC / hz;
	bl->brightness = bri;

	hrtimer_init(&bl->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	bl->timer.function = zonhor_bl_timer;

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

	backlight_update_status(bl->bd);
	g_bl = bl;
	dev_info(dev,
		 "%s: soft-PWM %u Hz on gpio, brightness=%u/100\n",
		 BL_NAME, bl->pwm_hz, bl->brightness);
	return 0;
}

static int zonhor_bl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zonhor_bl *bl;
	int ret;
	int gpio;

	bl = devm_kzalloc(dev, sizeof(*bl), GFP_KERNEL);
	if (!bl)
		return -ENOMEM;

	if (dev->of_node) {
		bl->gpiod = devm_gpiod_get(dev, NULL, GPIOD_OUT_HIGH);
		if (IS_ERR(bl->gpiod)) {
			ret = PTR_ERR(bl->gpiod);
			dev_err(dev, "gpiod_get failed: %d\n", ret);
			return ret;
		}
		bl->gpio_is_desc = true;
	} else {
		gpio = param_gpio;
		if (gpio < 0) {
			dev_err(dev, "no DT gpio and gpio= param unset\n");
			return -EINVAL;
		}
		ret = gpio_request(gpio, BL_NAME);
		if (ret) {
			dev_err(dev, "gpio_request(%d) failed: %d\n", gpio, ret);
			return ret;
		}
		ret = gpio_direction_output(gpio, 1);
		if (ret) {
			gpio_free(gpio);
			return ret;
		}
		bl->gpio_is_desc = false;
		bl->gpio_num = gpio;
	}

	platform_set_drvdata(pdev, bl);
	ret = zonhor_bl_probe_common(dev, bl);
	if (ret && !bl->gpio_is_desc)
		gpio_free(bl->gpio_num);
	return ret;
}

static int zonhor_bl_remove(struct platform_device *pdev)
{
	struct zonhor_bl *bl = platform_get_drvdata(pdev);
	unsigned long flags;

	if (!bl)
		return 0;

	spin_lock_irqsave(&bl->lock, flags);
	bl->running = false;
	spin_unlock_irqrestore(&bl->lock, flags);
	hrtimer_cancel(&bl->timer);
	zonhor_bl_set_pin(bl, false);

	if (!bl->gpio_is_desc)
		gpio_free(bl->gpio_num);

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

	ret = platform_driver_register(&zonhor_bl_driver);
	if (ret)
		return ret;

	/*
	 * Bring-up without DTB: insmod cv181x_zonhor_lcd_bl.ko gpio=500
	 * Creates a non-OF platform device that uses the module param.
	 */
	if (param_gpio >= 0) {
		zonhor_bl_pdev = platform_device_register_simple(DRVNAME, -1,
								 NULL, 0);
		if (IS_ERR(zonhor_bl_pdev)) {
			ret = PTR_ERR(zonhor_bl_pdev);
			zonhor_bl_pdev = NULL;
			platform_driver_unregister(&zonhor_bl_driver);
			return ret;
		}
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
MODULE_DESCRIPTION("Zonhor LCD GPIO soft-PWM backlight");
MODULE_LICENSE("GPL");
