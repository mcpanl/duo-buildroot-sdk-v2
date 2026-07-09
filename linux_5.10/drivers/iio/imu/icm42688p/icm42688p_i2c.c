// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HuaXuanYang ICM-42688P I2C bus driver
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/of.h>

#include "icm42688p.h"

static const struct regmap_config icm42688p_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = ICM42688P_REG_SEG_SEL,
};

static int icm42688p_i2c_probe(struct i2c_client *client,
			       const struct i2c_device_id *id)
{
	struct regmap *regmap;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENOTSUPP;

	regmap = devm_regmap_init_i2c(client, &icm42688p_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	return icm42688p_core_probe(&client->dev, regmap, client->irq);
}

static const struct of_device_id icm42688p_of_match[] = {
	{ .compatible = "hxymos,icm42688p" },
	{ .compatible = "invensense,icm42688p" },
	{}
};
MODULE_DEVICE_TABLE(of, icm42688p_of_match);

static const struct i2c_device_id icm42688p_i2c_id[] = {
	{ "icm42688p", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, icm42688p_i2c_id);

static struct i2c_driver icm42688p_i2c_driver = {
	.driver = {
		.name = ICM42688P_DRV_NAME,
		.of_match_table = icm42688p_of_match,
	},
	.probe = icm42688p_i2c_probe,
	.id_table = icm42688p_i2c_id,
};
module_i2c_driver(icm42688p_i2c_driver);

MODULE_AUTHOR("Zonhor");
MODULE_DESCRIPTION("HuaXuanYang ICM-42688P I2C driver");
MODULE_LICENSE("GPL");
