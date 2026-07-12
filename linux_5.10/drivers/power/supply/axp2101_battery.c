// SPDX-License-Identifier: GPL-2.0-only
/*
 * Battery and charger power supply driver for X-Powers AXP2101 PMIC
 *
 * Copyright (C) 2026 Zonhor SG2000
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/mfd/axp20x.h>

#define AXP2101_CHG_TRI			0
#define AXP2101_CHG_PRE			1
#define AXP2101_CHG_CC			2
#define AXP2101_CHG_CV			3
#define AXP2101_CHG_DONE		4
#define AXP2101_CHG_STOP		5

struct axp2101_batt {
	struct regmap *regmap;
	struct device *dev;
	struct power_supply *psy;
	int irq_chgdn;
	int irq_vinsert;
	int irq_vremove;
};

static const int axp2101_cc_lut[] = {
	0, 0, 0, 0,
	100000, 125000, 150000, 175000, 200000, 300000, 400000,
	500000, 600000, 700000, 800000, 900000, 1000000,
};

static int axp2101_cc_to_idx(int ua)
{
	int i, best = 0, best_err = INT_MAX;

	for (i = 0; i < ARRAY_SIZE(axp2101_cc_lut); i++) {
		int err = abs(axp2101_cc_lut[i] - ua);

		if (err < best_err) {
			best_err = err;
			best = i;
		}
	}

	return best;
}

static int axp2101_read_vbat_uv(struct axp2101_batt *batt, int *val)
{
	unsigned int hi, lo;
	int ret;

	ret = regmap_read(batt->regmap, AXP2101_VBAT_H, &hi);
	if (ret)
		return ret;
	ret = regmap_read(batt->regmap, AXP2101_VBAT_L, &lo);
	if (ret)
		return ret;

	*val = (((hi & AXP2101_VBAT_H_MASK) << 8) | lo) * 1000;
	return 0;
}

static int axp2101_get_status(struct axp2101_batt *batt)
{
	unsigned int reg;
	int ret;

	ret = regmap_read(batt->regmap, AXP2101_COMM_STAT1, &reg);
	if (ret)
		return ret;

	switch (reg & AXP2101_CHG_STAT_MASK) {
	case AXP2101_CHG_DONE:
		return POWER_SUPPLY_STATUS_FULL;
	case AXP2101_CHG_TRI:
	case AXP2101_CHG_PRE:
	case AXP2101_CHG_CC:
	case AXP2101_CHG_CV:
		return POWER_SUPPLY_STATUS_CHARGING;
	case AXP2101_CHG_STOP:
	default:
		return POWER_SUPPLY_STATUS_DISCHARGING;
	}
}

static int axp2101_get_cc(struct axp2101_batt *batt, int *val)
{
	unsigned int reg;
	int ret;

	ret = regmap_read(batt->regmap, AXP2101_ICC_CFG, &reg);
	if (ret)
		return ret;

	reg &= AXP2101_ICC_MASK;
	if (reg >= ARRAY_SIZE(axp2101_cc_lut))
		return -EINVAL;

	*val = axp2101_cc_lut[reg];
	return 0;
}

static int axp2101_set_cc(struct axp2101_batt *batt, int val)
{
	int idx = axp2101_cc_to_idx(val);

	if (idx >= ARRAY_SIZE(axp2101_cc_lut))
		return -EINVAL;

	return regmap_update_bits(batt->regmap, AXP2101_ICC_CFG,
				 AXP2101_ICC_MASK, idx);
}

static int axp2101_battery_get_prop(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct axp2101_batt *batt = power_supply_get_drvdata(psy);
	unsigned int reg;
	int ret, tmp;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		ret = regmap_read(batt->regmap, AXP2101_COMM_STAT0, &reg);
		if (ret)
			return ret;
		val->intval = !!(reg & AXP2101_BAT_PRESENT);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		ret = regmap_read(batt->regmap, AXP2101_COMM_STAT0, &reg);
		if (ret)
			return ret;
		val->intval = !!(reg & AXP2101_VBUS_GOOD);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		ret = axp2101_get_status(batt);
		if (ret < 0)
			return ret;
		val->intval = ret;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = axp2101_read_vbat_uv(batt, &tmp);
		if (ret)
			return ret;
		val->intval = tmp;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = regmap_read(batt->regmap, AXP2101_BAT_PERCENT, &reg);
		if (ret)
			return ret;
		val->intval = reg & 0x7f;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = axp2101_get_cc(batt, &tmp);
		if (ret)
			return ret;
		val->intval = tmp;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = 1000000;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int axp2101_battery_set_prop(struct power_supply *psy,
				    enum power_supply_property psp,
				    const union power_supply_propval *val)
{
	struct axp2101_batt *batt = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		return axp2101_set_cc(batt, val->intval);
	default:
		return -EINVAL;
	}
}

static int axp2101_battery_prop_writeable(struct power_supply *psy,
					  enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		return 1;
	default:
		return 0;
	}
}

static enum power_supply_property axp2101_battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
};

static const struct power_supply_desc axp2101_battery_desc = {
	.name = "axp2101-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = axp2101_battery_props,
	.num_properties = ARRAY_SIZE(axp2101_battery_props),
	.get_property = axp2101_battery_get_prop,
	.set_property = axp2101_battery_set_prop,
	.property_is_writeable = axp2101_battery_prop_writeable,
};

static irqreturn_t axp2101_battery_irq(int irq, void *data)
{
	struct axp2101_batt *batt = data;
	const char *name = "unknown";
	unsigned int stat0 = 0;
	unsigned int stat1 = 0;
	int ret0;
	int ret1;

	if (irq == batt->irq_chgdn)
		name = "CHGDN";
	else if (irq == batt->irq_vinsert)
		name = "VINSET";
	else if (irq == batt->irq_vremove)
		name = "VREMOV";

	ret0 = regmap_read(batt->regmap, AXP2101_COMM_STAT0, &stat0);
	ret1 = regmap_read(batt->regmap, AXP2101_COMM_STAT1, &stat1);
	if (ret0 || ret1)
		dev_info(batt->dev,
			 "AXP2101 INT# %s on irq %d, failed to read status: %d/%d\n",
			 name, irq, ret0, ret1);
	else
		dev_info(batt->dev,
			 "AXP2101 INT# %s on irq %d, COMM_STAT0=0x%02x COMM_STAT1=0x%02x\n",
			 name, irq, stat0, stat1);

	power_supply_changed(batt->psy);
	return IRQ_HANDLED;
}

static int axp2101_battery_probe(struct platform_device *pdev)
{
	struct axp20x_dev *axp20x = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config psy_cfg = {};
	struct axp2101_batt *batt;
	int ret;

	if (!axp20x || !axp20x->regmap)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing AXP2101 parent device\n");

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		dev_warn(&pdev->dev, "failed to set DMA mask: %d\n", ret);

	batt = devm_kzalloc(&pdev->dev, sizeof(*batt), GFP_KERNEL);
	if (!batt)
		return -ENOMEM;

	batt->regmap = axp20x->regmap;
	batt->dev = &pdev->dev;
	batt->irq_chgdn = -1;
	batt->irq_vinsert = -1;
	batt->irq_vremove = -1;

	ret = regmap_update_bits(batt->regmap, AXP2101_MODULE_EN,
				 AXP2101_GAUGE_EN | AXP2101_CHG_EN,
				 AXP2101_GAUGE_EN | AXP2101_CHG_EN);
	if (ret)
		return ret;

	ret = regmap_update_bits(batt->regmap, AXP2101_ADC_CH_EN0,
				 BIT(0) | BIT(2), BIT(0) | BIT(2));
	if (ret)
		return ret;

	ret = regmap_update_bits(batt->regmap, AXP2101_BAT_DET, BIT(0), BIT(0));
	if (ret)
		return ret;

	psy_cfg.of_node = pdev->dev.of_node;
	psy_cfg.drv_data = batt;

	batt->psy = devm_power_supply_register(&pdev->dev,
					       &axp2101_battery_desc, &psy_cfg);
	if (IS_ERR(batt->psy)) {
		ret = PTR_ERR(batt->psy);
		dev_err(&pdev->dev, "failed to register power supply: %d\n", ret);
		return ret;
	}

	batt->irq_chgdn = platform_get_irq_byname_optional(pdev, "CHGDN");
	if (batt->irq_chgdn >= 0) {
		batt->irq_chgdn = regmap_irq_get_virq(axp20x->regmap_irqc,
						      batt->irq_chgdn);
		ret = devm_request_threaded_irq(&pdev->dev, batt->irq_chgdn,
						NULL, axp2101_battery_irq,
						IRQF_ONESHOT, "axp2101-chgdn",
						batt);
		if (ret)
			return ret;
	}

	batt->irq_vinsert = platform_get_irq_byname_optional(pdev, "VINSET");
	if (batt->irq_vinsert >= 0) {
		batt->irq_vinsert = regmap_irq_get_virq(axp20x->regmap_irqc,
							batt->irq_vinsert);
		ret = devm_request_threaded_irq(&pdev->dev, batt->irq_vinsert,
						NULL, axp2101_battery_irq,
						IRQF_ONESHOT, "axp2101-vinset",
						batt);
		if (ret)
			return ret;
	}

	batt->irq_vremove = platform_get_irq_byname_optional(pdev, "VREMOV");
	if (batt->irq_vremove >= 0) {
		batt->irq_vremove = regmap_irq_get_virq(axp20x->regmap_irqc,
							batt->irq_vremove);
		ret = devm_request_threaded_irq(&pdev->dev, batt->irq_vremove,
						NULL, axp2101_battery_irq,
						IRQF_ONESHOT, "axp2101-vremov",
						batt);
		if (ret)
			return ret;
	}

	platform_set_drvdata(pdev, batt);
	return 0;
}

/*
 * Charger / VBUS IRQs must not stay armed across freeze/s2idle: USB suspend
 * commonly glitches VIN and immediately wakes the system (seen as
 * axp20x-i2c "Failed to read IRQ status: -108" after I2C late-suspend).
 * Power-key wake remains handled by axp2101-pek.
 */
static int __maybe_unused axp2101_battery_suspend(struct device *dev)
{
	struct axp2101_batt *batt = dev_get_drvdata(dev);

	if (batt->irq_chgdn >= 0)
		disable_irq(batt->irq_chgdn);
	if (batt->irq_vinsert >= 0)
		disable_irq(batt->irq_vinsert);
	if (batt->irq_vremove >= 0)
		disable_irq(batt->irq_vremove);

	return 0;
}

static int __maybe_unused axp2101_battery_resume(struct device *dev)
{
	struct axp2101_batt *batt = dev_get_drvdata(dev);

	if (batt->irq_chgdn >= 0)
		enable_irq(batt->irq_chgdn);
	if (batt->irq_vinsert >= 0)
		enable_irq(batt->irq_vinsert);
	if (batt->irq_vremove >= 0)
		enable_irq(batt->irq_vremove);

	return 0;
}

static const struct dev_pm_ops axp2101_battery_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(axp2101_battery_suspend, axp2101_battery_resume)
};

static const struct of_device_id axp2101_battery_of_match[] = {
	{ .compatible = "x-powers,axp2101-battery-power-supply" },
	{ }
};
MODULE_DEVICE_TABLE(of, axp2101_battery_of_match);

static struct platform_driver axp2101_battery_driver = {
	.probe = axp2101_battery_probe,
	.driver = {
		.name = "axp2101-battery-power-supply",
		.of_match_table = axp2101_battery_of_match,
		.pm = &axp2101_battery_pm_ops,
	},
};
module_platform_driver(axp2101_battery_driver);

MODULE_AUTHOR("Zonhor SG2000");
MODULE_DESCRIPTION("AXP2101 battery and charger power supply driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:axp2101-battery-power-supply");
