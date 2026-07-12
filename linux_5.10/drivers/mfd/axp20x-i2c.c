// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C driver for the X-Powers' Power Management ICs
 *
 * AXP20x typically comprises an adaptive USB-Compatible PWM charger, BUCK DC-DC
 * converters, LDOs, multiple 12-bit ADCs of voltage, current and temperature
 * as well as configurable GPIOs.
 *
 * This driver supports the I2C variants.
 *
 * Copyright (C) 2014 Carlo Caione
 *
 * Author: Carlo Caione <carlo@caione.org>
 */

#include <linux/acpi.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mfd/axp20x.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define AXP2101_IRQ_STATUS0	0x48

/*
 * Before I2C late-suspend, clear latched AXP2101 IRQ status so INT# is not
 * left asserted into freeze/s2idle (handler would then hit -ESHUTDOWN).
 */
static int axp20x_i2c_suspend(struct device *dev)
{
	struct axp20x_dev *axp20x = dev_get_drvdata(dev);
	unsigned int status;
	int ret;
	int i;

	if (!axp20x || axp20x->variant != AXP2101_ID)
		return 0;

	for (i = 0; i < 3; i++) {
		if (regmap_read(axp20x->regmap, AXP2101_IRQ_STATUS0 + i, &status))
			continue;
		if (status)
			regmap_write(axp20x->regmap, AXP2101_IRQ_STATUS0 + i,
				     status);
	}

	if (device_may_wakeup(dev) && axp20x->irq > 0) {
		ret = enable_irq_wake(axp20x->irq);
		if (ret)
			dev_warn(dev, "failed to enable AXP2101 wake IRQ %d: %d\n",
				 axp20x->irq, ret);

		/*
		 * Keep the IRQ line armed as a wake source, but stop the normal
		 * regmap-irq thread before I2C is suspended. Otherwise a late
		 * AXP INT# can try to read IRQ status after the adapter is down.
		 */
		disable_irq(axp20x->irq);
	}

	return 0;
}

static int axp20x_i2c_resume(struct device *dev)
{
	struct axp20x_dev *axp20x = dev_get_drvdata(dev);

	if (axp20x && axp20x->variant == AXP2101_ID &&
	    device_may_wakeup(dev) && axp20x->irq > 0) {
		enable_irq(axp20x->irq);
		disable_irq_wake(axp20x->irq);
	}

	return 0;
}

static const struct dev_pm_ops axp20x_i2c_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(axp20x_i2c_suspend, axp20x_i2c_resume)
};

static int axp20x_i2c_probe(struct i2c_client *i2c,
			    const struct i2c_device_id *id)
{
	struct axp20x_dev *axp20x;
	int ret;

	axp20x = devm_kzalloc(&i2c->dev, sizeof(*axp20x), GFP_KERNEL);
	if (!axp20x)
		return -ENOMEM;

	axp20x->dev = &i2c->dev;
	axp20x->irq = i2c->irq;
	dev_set_drvdata(axp20x->dev, axp20x);

	ret = axp20x_match_device(axp20x);
	if (ret)
		return ret;

	axp20x->regmap = devm_regmap_init_i2c(i2c, axp20x->regmap_cfg);
	if (IS_ERR(axp20x->regmap)) {
		ret = PTR_ERR(axp20x->regmap);
		dev_err(&i2c->dev, "regmap init failed: %d\n", ret);
		return ret;
	}

	device_init_wakeup(&i2c->dev,
			   of_property_read_bool(i2c->dev.of_node,
						 "wakeup-source"));

	return axp20x_device_probe(axp20x);
}

static int axp20x_i2c_remove(struct i2c_client *i2c)
{
	struct axp20x_dev *axp20x = i2c_get_clientdata(i2c);

	return axp20x_device_remove(axp20x);
}

static const struct of_device_id axp20x_i2c_of_match[] = {
	{ .compatible = "x-powers,axp152", .data = (void *)AXP152_ID },
	{ .compatible = "x-powers,axp202", .data = (void *)AXP202_ID },
	{ .compatible = "x-powers,axp209", .data = (void *)AXP209_ID },
	{ .compatible = "x-powers,axp221", .data = (void *)AXP221_ID },
	{ .compatible = "x-powers,axp223", .data = (void *)AXP223_ID },
	{ .compatible = "x-powers,axp2101", .data = (void *)AXP2101_ID },
	{ .compatible = "x-powers,axp803", .data = (void *)AXP803_ID },
	{ .compatible = "x-powers,axp806", .data = (void *)AXP806_ID },
	{ },
};
MODULE_DEVICE_TABLE(of, axp20x_i2c_of_match);

static const struct i2c_device_id axp20x_i2c_id[] = {
	{ "axp152", 0 },
	{ "axp202", 0 },
	{ "axp209", 0 },
	{ "axp221", 0 },
	{ "axp223", 0 },
	{ "axp2101", 0 },
	{ "axp803", 0 },
	{ "axp806", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, axp20x_i2c_id);

#ifdef CONFIG_ACPI
static const struct acpi_device_id axp20x_i2c_acpi_match[] = {
	{
		.id = "INT33F4",
		.driver_data = AXP288_ID,
	},
	{ },
};
MODULE_DEVICE_TABLE(acpi, axp20x_i2c_acpi_match);
#endif

static struct i2c_driver axp20x_i2c_driver = {
	.driver = {
		.name	= "axp20x-i2c",
		.of_match_table	= of_match_ptr(axp20x_i2c_of_match),
		.acpi_match_table = ACPI_PTR(axp20x_i2c_acpi_match),
		.pm	= &axp20x_i2c_pm_ops,
	},
	.probe		= axp20x_i2c_probe,
	.remove		= axp20x_i2c_remove,
	.id_table	= axp20x_i2c_id,
};

module_i2c_driver(axp20x_i2c_driver);

MODULE_DESCRIPTION("PMIC MFD I2C driver for AXP20X");
MODULE_AUTHOR("Carlo Caione <carlo@caione.org>");
MODULE_LICENSE("GPL");
