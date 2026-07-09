/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * HuaXuanYang ICM-42688P IMU register definitions
 *
 * Register map per ICM_42688P-HXY.pdf (flat 8-bit addressing).
 */

#ifndef ICM42688P_H_
#define ICM42688P_H_

#include <linux/iio/iio.h>
#include <linux/regmap.h>

#define ICM42688P_DRV_NAME		"icm42688p"

#define ICM42688P_REG_WHO_AM_I		0x01
#define ICM42688P_REG_COM_CFG		0x05
#define ICM42688P_REG_DATA_STAT		0x0b
#define ICM42688P_REG_ACC_XH		0x0c
#define ICM42688P_REG_GYR_XH		0x12
#define ICM42688P_REG_TEMP_H		0x22
#define ICM42688P_REG_ACC_CONF		0x40
#define ICM42688P_REG_ACC_RANGE		0x41
#define ICM42688P_REG_GYR_CONF		0x42
#define ICM42688P_REG_GYR_RANGE		0x43
#define ICM42688P_REG_SOFT_RST		0x4a
#define ICM42688P_REG_PWR_CTRL		0x7d
#define ICM42688P_REG_SEG_SEL		0x7f

#define ICM42688P_WHO_AM_I_VAL		0x6a
#define ICM42688P_SOFT_RST_VAL		0xa5

#define ICM42688P_PWR_CTRL_INIT		0x0e
#define ICM42688P_PWR_CTRL_RUN		0x07

#define ICM42688P_COM_CFG_DEFAULT	0x50

#define ICM42688P_ACC_CONF_100HZ	0xa8
#define ICM42688P_GYR_CONF_100HZ	0xa8

#define ICM42688P_ACC_FS_16G		0x03
#define ICM42688P_GYR_FS_2000DPS	0x00

#define ICM42688P_DATA_STAT_DRDY_ACC	BIT(7)
#define ICM42688P_DATA_STAT_DRDY_GYR	BIT(6)
#define ICM42688P_DATA_STAT_DRDY_TMP	BIT(2)

enum icm42688p_accel_fs {
	ICM42688P_ACCEL_FS_2G = 0,
	ICM42688P_ACCEL_FS_4G,
	ICM42688P_ACCEL_FS_8G,
	ICM42688P_ACCEL_FS_16G,
};

enum icm42688p_gyro_fs {
	ICM42688P_GYRO_FS_2000DPS = 0,
	ICM42688P_GYRO_FS_1000DPS,
	ICM42688P_GYRO_FS_500DPS,
	ICM42688P_GYRO_FS_250DPS,
	ICM42688P_GYRO_FS_125DPS,
};

struct icm42688p_state {
	struct regmap *map;
	struct mutex lock;
	enum icm42688p_accel_fs accel_fs;
	enum icm42688p_gyro_fs gyro_fs;
};

int icm42688p_core_probe(struct device *dev, struct regmap *regmap, int irq);

#endif
