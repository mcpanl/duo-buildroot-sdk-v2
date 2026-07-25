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
#include <linux/thermal.h>
#include <linux/mfd/axp20x.h>

#define AXP2101_CHG_TRI			0
#define AXP2101_CHG_PRE			1
#define AXP2101_CHG_CC			2
#define AXP2101_CHG_CV			3
#define AXP2101_CHG_DONE		4
#define AXP2101_CHG_STOP		5

#define AXP2101_ADC_DATA_MASK		GENMASK(5, 0)
#define AXP2101_TDIE_OFFSET_MC		22000
#define AXP2101_TDIE_SCALE_RAW		7274
#define AXP2101_TDIE_LSB_MC		50	/* 1000/20 */

enum axp2101_tz_kind {
	AXP2101_TZ_NTC,
	AXP2101_TZ_DIE,
};

struct axp2101_batt;

struct axp2101_tz {
	struct axp2101_batt *batt;
	enum axp2101_tz_kind kind;
	struct thermal_zone_device *tzd;
};

struct axp2101_batt {
	struct regmap *regmap;
	struct device *dev;
	struct power_supply *psy;
	struct axp2101_tz tz_ntc;
	struct axp2101_tz tz_die;
	int irq_chgdn;
	int irq_vinsert;
	int irq_vremove;
};

/*
 * Datasheet Table 7-2: 10kOhm@25C NTC with 50uA TS bias.
 * ADC code decreases as temperature rises.
 */
static const struct {
	u16 adc;
	s16 temp_c;
} axp2101_ntc_lut[] = {
	{ 0x189c, -20 },
	{ 0x1398, -15 },
	{ 0x0fba, -10 },
	{ 0x0cb8, -5 },
	{ 0x0a5a, 0 },
	{ 0x0878, 5 },
	{ 0x06f2, 10 },
	{ 0x05b8, 15 },
	{ 0x04b8, 20 },
	{ 0x03e8, 25 },
	{ 0x0340, 30 },
	{ 0x02b8, 35 },
	{ 0x0248, 40 },
	{ 0x01ec, 45 },
	{ 0x01a2, 50 },
	{ 0x0162, 55 },
	{ 0x0130, 60 },
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

static int axp2101_read_adc14(struct axp2101_batt *batt, u8 reg_h, u8 reg_l,
			      unsigned int *raw)
{
	unsigned int hi, lo;
	int ret;

	ret = regmap_read(batt->regmap, reg_h, &hi);
	if (ret)
		return ret;
	ret = regmap_read(batt->regmap, reg_l, &lo);
	if (ret)
		return ret;

	*raw = ((hi & AXP2101_ADC_DATA_MASK) << 8) | lo;
	return 0;
}

static int axp2101_read_vbat_uv(struct axp2101_batt *batt, int *val)
{
	unsigned int raw;
	int ret;

	ret = axp2101_read_adc14(batt, AXP2101_VBAT_H, AXP2101_VBAT_L, &raw);
	if (ret)
		return ret;

	*val = raw * 1000;
	return 0;
}

/* Linear interpolate datasheet NTC LUT; return millidegree Celsius. */
static int axp2101_ntc_raw_to_mC(unsigned int raw)
{
	int i;

	if (raw >= axp2101_ntc_lut[0].adc)
		return axp2101_ntc_lut[0].temp_c * 1000;

	if (raw <= axp2101_ntc_lut[ARRAY_SIZE(axp2101_ntc_lut) - 1].adc)
		return axp2101_ntc_lut[ARRAY_SIZE(axp2101_ntc_lut) - 1].temp_c *
		       1000;

	for (i = 0; i < ARRAY_SIZE(axp2101_ntc_lut) - 1; i++) {
		unsigned int a0 = axp2101_ntc_lut[i].adc;
		unsigned int a1 = axp2101_ntc_lut[i + 1].adc;
		int t0 = axp2101_ntc_lut[i].temp_c;
		int t1 = axp2101_ntc_lut[i + 1].temp_c;

		if (raw <= a0 && raw >= a1) {
			/* temp = t0 + (t1-t0)*(a0-raw)/(a0-a1) */
			return t0 * 1000 +
			       (t1 - t0) * 1000 * (int)(a0 - raw) /
			       (int)(a0 - a1);
		}
	}

	return axp2101_ntc_lut[ARRAY_SIZE(axp2101_ntc_lut) - 1].temp_c * 1000;
}

static int axp2101_tdie_raw_to_mC(unsigned int raw)
{
	return AXP2101_TDIE_OFFSET_MC +
	       (AXP2101_TDIE_SCALE_RAW - (int)raw) * AXP2101_TDIE_LSB_MC;
}

static int axp2101_read_ntc_mC(struct axp2101_batt *batt, int *temp_mC)
{
	unsigned int raw;
	int ret;

	ret = axp2101_read_adc14(batt, AXP2101_TS_H, AXP2101_TS_L, &raw);
	if (ret)
		return ret;

	*temp_mC = axp2101_ntc_raw_to_mC(raw);
	return 0;
}

static int axp2101_read_die_mC(struct axp2101_batt *batt, int *temp_mC)
{
	unsigned int raw;
	int ret;

	ret = axp2101_read_adc14(batt, AXP2101_TDIE_H, AXP2101_TDIE_L, &raw);
	if (ret)
		return ret;

	*temp_mC = axp2101_tdie_raw_to_mC(raw);
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
	case POWER_SUPPLY_PROP_TEMP:
		/* External NTC on TS pin, unit: 0.1 degC */
		ret = axp2101_read_ntc_mC(batt, &tmp);
		if (ret)
			return ret;
		val->intval = tmp / 100;
		break;
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		/* PMIC die temperature, unit: 0.1 degC */
		ret = axp2101_read_die_mC(batt, &tmp);
		if (ret)
			return ret;
		val->intval = tmp / 100;
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
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TEMP_AMBIENT,
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
	/* Custom axp2101-ntc / axp2101-die zones registered below. */
	.no_thermal = true,
};

#ifdef CONFIG_THERMAL
static int axp2101_tz_get_temp(struct thermal_zone_device *tzd, int *temp)
{
	struct axp2101_tz *tz = tzd->devdata;
	int ret;

	if (!tz || !tz->batt)
		return -EINVAL;

	switch (tz->kind) {
	case AXP2101_TZ_NTC:
		ret = axp2101_read_ntc_mC(tz->batt, temp);
		break;
	case AXP2101_TZ_DIE:
		ret = axp2101_read_die_mC(tz->batt, temp);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static struct thermal_zone_device_ops axp2101_tz_ops = {
	.get_temp = axp2101_tz_get_temp,
};

static int axp2101_register_tz(struct axp2101_batt *batt, struct axp2101_tz *tz,
			       const char *name, enum axp2101_tz_kind kind)
{
	int ret;

	tz->batt = batt;
	tz->kind = kind;
	tz->tzd = thermal_zone_device_register(name, 0, 0, tz,
					       &axp2101_tz_ops, NULL, 0, 0);
	if (IS_ERR(tz->tzd)) {
		ret = PTR_ERR(tz->tzd);
		tz->tzd = NULL;
		return ret;
	}

	ret = thermal_zone_device_enable(tz->tzd);
	if (ret) {
		thermal_zone_device_unregister(tz->tzd);
		tz->tzd = NULL;
		return ret;
	}

	return 0;
}

static void axp2101_unregister_tz(struct axp2101_tz *tz)
{
	if (!tz->tzd)
		return;

	thermal_zone_device_unregister(tz->tzd);
	tz->tzd = NULL;
}
#else
static int axp2101_register_tz(struct axp2101_batt *batt, struct axp2101_tz *tz,
			       const char *name, enum axp2101_tz_kind kind)
{
	return 0;
}

static void axp2101_unregister_tz(struct axp2101_tz *tz)
{
}
#endif

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

	/* VBAT + TS(NTC) + VBUS + TDIE */
	ret = regmap_update_bits(batt->regmap, AXP2101_ADC_CH_EN0,
				 AXP2101_ADC_VBAT_EN | AXP2101_ADC_TS_EN |
				 AXP2101_ADC_VBUS_EN | AXP2101_ADC_TDIE_EN,
				 AXP2101_ADC_VBAT_EN | AXP2101_ADC_TS_EN |
				 AXP2101_ADC_VBUS_EN | AXP2101_ADC_TDIE_EN);
	if (ret)
		return ret;

	/* External NTC: affect charger, always-on 50uA bias */
	ret = regmap_update_bits(batt->regmap, AXP2101_TS_CFG,
				 AXP2101_TS_FUNC_MASK | AXP2101_TS_SRC_MASK |
				 AXP2101_TS_CURR_MASK,
				 AXP2101_TS_FUNC_NTC |
				 AXP2101_TS_SRC_ALWAYS_ON |
				 AXP2101_TS_CURR_50UA);
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

	ret = axp2101_register_tz(batt, &batt->tz_ntc, "axp2101-ntc",
				  AXP2101_TZ_NTC);
	if (ret) {
		dev_err(&pdev->dev, "failed to register NTC thermal zone: %d\n",
			ret);
		return ret;
	}

	ret = axp2101_register_tz(batt, &batt->tz_die, "axp2101-die",
				  AXP2101_TZ_DIE);
	if (ret) {
		dev_err(&pdev->dev, "failed to register die thermal zone: %d\n",
			ret);
		axp2101_unregister_tz(&batt->tz_ntc);
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
			goto err_tz;
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
			goto err_tz;
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
			goto err_tz;
	}

	platform_set_drvdata(pdev, batt);
	return 0;

err_tz:
	axp2101_unregister_tz(&batt->tz_die);
	axp2101_unregister_tz(&batt->tz_ntc);
	return ret;
}

static int axp2101_battery_remove(struct platform_device *pdev)
{
	struct axp2101_batt *batt = platform_get_drvdata(pdev);

	axp2101_unregister_tz(&batt->tz_die);
	axp2101_unregister_tz(&batt->tz_ntc);
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
	.remove = axp2101_battery_remove,
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
