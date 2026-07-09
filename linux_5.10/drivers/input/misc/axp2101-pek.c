// SPDX-License-Identifier: GPL-2.0-only
/*
 * AXP2101 power button driver.
 *
 * Copyright (C) 2026 Zonhor SG2000
 */

#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/axp20x.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

struct axp2101_pek {
	struct axp20x_dev *axp20x;
	struct input_dev *input;
	int irq_dbr;
	int irq_dbf;
};

static irqreturn_t axp2101_pek_irq(int irq, void *pwr)
{
	struct axp2101_pek *pek = pwr;
	struct input_dev *idev = pek->input;

	if (!idev)
		return IRQ_HANDLED;

	if (irq == pek->irq_dbf) {
		dev_info(&idev->dev, "AXP2101 INT# PEK_DBF: power key pressed\n");
		input_report_key(idev, KEY_POWER, true);
		input_sync(idev);
	} else if (irq == pek->irq_dbr) {
		dev_info(&idev->dev, "AXP2101 INT# PEK_DBR: power key released\n");
		input_report_key(idev, KEY_POWER, false);
		input_sync(idev);
	}

	return IRQ_HANDLED;
}

static int axp2101_pek_probe(struct platform_device *pdev)
{
	struct axp20x_dev *axp20x = dev_get_drvdata(pdev->dev.parent);
	struct axp2101_pek *pek;
	int error;

	pek = devm_kzalloc(&pdev->dev, sizeof(*pek), GFP_KERNEL);
	if (!pek)
		return -ENOMEM;

	pek->axp20x = axp20x;

	pek->irq_dbr = platform_get_irq_byname(pdev, "PEK_DBR");
	if (pek->irq_dbr < 0)
		return pek->irq_dbr;
	pek->irq_dbr = regmap_irq_get_virq(axp20x->regmap_irqc, pek->irq_dbr);

	pek->irq_dbf = platform_get_irq_byname(pdev, "PEK_DBF");
	if (pek->irq_dbf < 0)
		return pek->irq_dbf;
	pek->irq_dbf = regmap_irq_get_virq(axp20x->regmap_irqc, pek->irq_dbf);

	pek->input = devm_input_allocate_device(&pdev->dev);
	if (!pek->input)
		return -ENOMEM;

	pek->input->name = "axp2101-pek";
	pek->input->phys = "axp2101/input0";
	pek->input->dev.parent = &pdev->dev;
	input_set_capability(pek->input, EV_KEY, KEY_POWER);
	input_set_drvdata(pek->input, pek);

	error = input_register_device(pek->input);
	if (error)
		return error;

	error = regmap_update_bits(axp20x->regmap, AXP2101_IRQ1_EN,
				   AXP2101_PONP_IRQ_EN | AXP2101_PONN_IRQ_EN,
				   AXP2101_PONP_IRQ_EN | AXP2101_PONN_IRQ_EN);
	if (error)
		return error;

	/* Keep hardware long-press power-off enabled for PMIC shutdown. */
	regmap_update_bits(axp20x->regmap, AXP2101_PWROFF_EN,
			   AXP2101_BTN_PWROFF_EN, AXP2101_BTN_PWROFF_EN);

	error = devm_request_any_context_irq(&pdev->dev, pek->irq_dbr,
					     axp2101_pek_irq, 0,
					     "axp2101-pek-dbr", pek);
	if (error < 0)
		return error;

	error = devm_request_any_context_irq(&pdev->dev, pek->irq_dbf,
					     axp2101_pek_irq, 0,
					     "axp2101-pek-dbf", pek);
	if (error < 0)
		return error;

	device_init_wakeup(&pdev->dev, true);
	platform_set_drvdata(pdev, pek);

	return 0;
}

static int __maybe_unused axp2101_pek_suspend(struct device *dev)
{
	struct axp2101_pek *pek = dev_get_drvdata(dev);

	if (device_may_wakeup(dev)) {
		enable_irq_wake(pek->irq_dbf);
		enable_irq_wake(pek->irq_dbr);
	} else {
		disable_irq(pek->irq_dbf);
		disable_irq(pek->irq_dbr);
	}

	return 0;
}

static int __maybe_unused axp2101_pek_resume(struct device *dev)
{
	struct axp2101_pek *pek = dev_get_drvdata(dev);

	if (device_may_wakeup(dev)) {
		disable_irq_wake(pek->irq_dbf);
		disable_irq_wake(pek->irq_dbr);
	} else {
		enable_irq(pek->irq_dbf);
		enable_irq(pek->irq_dbr);
	}

	return 0;
}

static const struct dev_pm_ops axp2101_pek_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(axp2101_pek_suspend, axp2101_pek_resume)
};

static const struct platform_device_id axp2101_pek_id_match[] = {
	{ .name = "axp2101-pek" },
	{ }
};
MODULE_DEVICE_TABLE(platform, axp2101_pek_id_match);

static struct platform_driver axp2101_pek_driver = {
	.probe = axp2101_pek_probe,
	.id_table = axp2101_pek_id_match,
	.driver = {
		.name = "axp2101-pek",
		.pm = &axp2101_pek_pm_ops,
	},
};
module_platform_driver(axp2101_pek_driver);

MODULE_DESCRIPTION("AXP2101 Power Button");
MODULE_AUTHOR("Zonhor SG2000");
MODULE_LICENSE("GPL");
