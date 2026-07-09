// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HuaXuanYang ICM-42688P 6-axis IMU IIO driver
 *
 * Register map per ICM_42688P-HXY.pdf. This part is not compatible with the
 * InvenSense ICM-426xx banked register layout used by inv_icm42600.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

#include "icm42688p.h"

/* Sensitivity in mg/LSB per ICM_42688P-HXY.pdf mechanical parameters */
static const int icm42688p_accel_sensitivity[][2] = {
	[ICM42688P_ACCEL_FS_2G]  = { 0, 61035 },  /* 0.061035 mg/LSB */
	[ICM42688P_ACCEL_FS_4G]  = { 0, 122070 },
	[ICM42688P_ACCEL_FS_8G]  = { 0, 244141 },
	[ICM42688P_ACCEL_FS_16G] = { 0, 488281 },
};

/* Sensitivity in mdps/LSB */
static const int icm42688p_gyro_sensitivity[][2] = {
	[ICM42688P_GYRO_FS_2000DPS] = { 61, 0 },
	[ICM42688P_GYRO_FS_1000DPS] = { 30, 500000 },
	[ICM42688P_GYRO_FS_500DPS]  = { 15, 250000 },
	[ICM42688P_GYRO_FS_250DPS]  = { 7, 625000 },
	[ICM42688P_GYRO_FS_125DPS]  = { 3, 812500 },
};

static int icm42688p_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	struct icm42688p_state *st = iio_priv(indio_dev);
	__be16 raw_be;
	int16_t raw;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&st->lock);
		switch (chan->type) {
		case IIO_ACCEL:
			ret = regmap_bulk_read(st->map,
					       ICM42688P_REG_ACC_XH +
					       2 * (chan->channel2 - IIO_MOD_X),
					       &raw_be, sizeof(raw_be));
			break;
		case IIO_ANGL_VEL:
			ret = regmap_bulk_read(st->map,
					       ICM42688P_REG_GYR_XH +
					       2 * (chan->channel2 - IIO_MOD_X),
					       &raw_be, sizeof(raw_be));
			break;
		case IIO_TEMP:
			ret = regmap_bulk_read(st->map, ICM42688P_REG_TEMP_H,
					       &raw_be, sizeof(raw_be));
			break;
		default:
			ret = -EINVAL;
			break;
		}
		mutex_unlock(&st->lock);
		if (ret)
			return ret;

		raw = be16_to_cpu(raw_be);
		*val = raw;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			*val = icm42688p_accel_sensitivity[st->accel_fs][0];
			*val2 = icm42688p_accel_sensitivity[st->accel_fs][1];
			return IIO_VAL_INT_PLUS_MICRO;
		case IIO_ANGL_VEL:
			*val = icm42688p_gyro_sensitivity[st->gyro_fs][0];
			*val2 = icm42688p_gyro_sensitivity[st->gyro_fs][1];
			return IIO_VAL_INT_PLUS_MICRO;
		case IIO_TEMP:
			/* T = raw / 512 + 23 C */
			*val = 0;
			*val2 = 1953125;
			return IIO_VAL_INT_PLUS_MICRO;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:
		if (chan->type != IIO_TEMP)
			return -EINVAL;
		*val = 23;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = 100;
		*val2 = 0;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static const struct iio_info icm42688p_info = {
	.read_raw = icm42688p_read_raw,
};

static const struct iio_chan_spec icm42688p_channels[] = {
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_X,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
					    BIT(IIO_CHAN_INFO_SAMP_FREQ),
	}, {
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_Y,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
					    BIT(IIO_CHAN_INFO_SAMP_FREQ),
	}, {
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_Z,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
					    BIT(IIO_CHAN_INFO_SAMP_FREQ),
	}, {
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_X,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
					    BIT(IIO_CHAN_INFO_SAMP_FREQ),
	}, {
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_Y,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
					    BIT(IIO_CHAN_INFO_SAMP_FREQ),
	}, {
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_Z,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |
					    BIT(IIO_CHAN_INFO_SAMP_FREQ),
	}, {
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int icm42688p_chip_init(struct icm42688p_state *st)
{
	unsigned int whoami;
	int ret;

	ret = regmap_read(st->map, ICM42688P_REG_WHO_AM_I, &whoami);
	if (ret)
		return ret;

	if (whoami != ICM42688P_WHO_AM_I_VAL) {
		dev_err(regmap_get_device(st->map),
			"unexpected WHO_AM_I 0x%02x, expected 0x%02x\n",
			whoami, ICM42688P_WHO_AM_I_VAL);
		return -ENODEV;
	}

	ret = regmap_write(st->map, ICM42688P_REG_SOFT_RST,
			   ICM42688P_SOFT_RST_VAL);
	if (ret)
		return ret;
	msleep(1);

	ret = regmap_write(st->map, ICM42688P_REG_PWR_CTRL,
			   ICM42688P_PWR_CTRL_INIT);
	if (ret)
		return ret;
	msleep(10);

	ret = regmap_write(st->map, ICM42688P_REG_COM_CFG,
			   ICM42688P_COM_CFG_DEFAULT);
	if (ret)
		return ret;

	ret = regmap_write(st->map, ICM42688P_REG_ACC_CONF,
			   ICM42688P_ACC_CONF_100HZ);
	if (ret)
		return ret;

	ret = regmap_write(st->map, ICM42688P_REG_ACC_RANGE,
			   ICM42688P_ACC_FS_16G);
	if (ret)
		return ret;

	ret = regmap_write(st->map, ICM42688P_REG_GYR_CONF,
			   ICM42688P_GYR_CONF_100HZ);
	if (ret)
		return ret;

	ret = regmap_write(st->map, ICM42688P_REG_GYR_RANGE,
			   ICM42688P_GYR_FS_2000DPS);
	if (ret)
		return ret;

	msleep(1);

	ret = regmap_write(st->map, ICM42688P_REG_PWR_CTRL,
			   ICM42688P_PWR_CTRL_RUN);
	if (ret)
		return ret;

	msleep(250);

	st->accel_fs = ICM42688P_ACCEL_FS_16G;
	st->gyro_fs = ICM42688P_GYRO_FS_2000DPS;

	return 0;
}

int icm42688p_core_probe(struct device *dev, struct regmap *regmap, int irq)
{
	struct icm42688p_state *st;
	struct iio_dev *indio_dev;
	int ret;

	(void)irq;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->map = regmap;
	mutex_init(&st->lock);

	ret = icm42688p_chip_init(st);
	if (ret) {
		dev_err(dev, "chip init failed: %d\n", ret);
		return ret;
	}

	indio_dev->name = ICM42688P_DRV_NAME;
	indio_dev->info = &icm42688p_info;
	indio_dev->channels = icm42688p_channels;
	indio_dev->num_channels = ARRAY_SIZE(icm42688p_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return ret;

	dev_info(dev, "ICM-42688P-HXY ready (WHO_AM_I=0x%02x)\n",
		 ICM42688P_WHO_AM_I_VAL);

	return 0;
}
