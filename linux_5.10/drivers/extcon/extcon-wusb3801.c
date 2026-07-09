// SPDX-License-Identifier: GPL-2.0-only
/*
 * extcon-wusb3801.c - Willsemi WUSB3801 Type-C CC logic extcon driver
 *
 * Register definitions are based on WUSB3801 datasheet (Rev 1.1).
 */

#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/extcon-provider.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

/* WUSB3801 registers */
#define WUSB3801_REG_DEVICE_ID		0x01
#define WUSB3801_REG_CONTROL		0x02
#define WUSB3801_REG_INT_STATUS		0x03
#define WUSB3801_REG_CC_STATUS		0x04
#define WUSB3801_REG_MAX		WUSB3801_REG_CC_STATUS

/* DEVICE_ID (0x01) */
#define WUSB3801_DEVICE_ID_VERSION_MASK	GENMASK(7, 3)
#define WUSB3801_DEVICE_ID_VENDOR_MASK	GENMASK(2, 0)

/* CONTROL (0x02) */
#define WUSB3801_CTRL_INT_EN		BIT(0)
#define WUSB3801_CTRL_WORK_MODE_MASK	GENMASK(2, 1)
#define WUSB3801_CTRL_WORK_SNK		(0 << 1)
#define WUSB3801_CTRL_WORK_SRC		(1 << 1)
#define WUSB3801_CTRL_WORK_DRP		(2 << 1)
#define WUSB3801_CTRL_SRC_CURR_MASK	GENMASK(4, 3)
#define WUSB3801_CTRL_SRC_CURR_DEFAULT	(0 << 3)
#define WUSB3801_CTRL_SRC_CURR_1P5A	(1 << 3)
#define WUSB3801_CTRL_SRC_CURR_3P0A	(2 << 3)
#define WUSB3801_CTRL_TRY_MASK		GENMASK(6, 5)

/* INT_STATUS (0x03): Detach/Attach state */
#define WUSB3801_INT_DETACH_ATTACH_MASK	GENMASK(1, 0)
#define WUSB3801_INT_NO_EVENT		0x0
#define WUSB3801_INT_ATTACHED		0x1
#define WUSB3801_INT_DETACHED		0x2

/* CC_STATUS (0x04) */
#define WUSB3801_CC_VBUS_DETECTED	BIT(7)
#define WUSB3801_CC_CHG_CURRENT_MASK	GENMASK(6, 5)
#define WUSB3801_CC_PORT_STATUS_MASK	GENMASK(4, 2)
#define WUSB3801_CC_ORIENTATION_MASK	GENMASK(1, 0)

#define WUSB3801_PORT_STANDBY		0x0
#define WUSB3801_PORT_SNK		0x1
#define WUSB3801_PORT_SRC		0x2
#define WUSB3801_PORT_AUDIO		0x3
#define WUSB3801_PORT_DEBUG		0x4

struct wusb3801_info {
	struct device *dev;
	struct extcon_dev *edev;
	struct regmap *regmap;
	int irq;
	struct work_struct irq_work;
	struct mutex lock;
};

/* List of detectable cables */
static const unsigned int wusb3801_extcon_cables[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_NONE,
};

static const struct regmap_config wusb3801_regmap_cfg = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = WUSB3801_REG_MAX,
};

static const char *wusb3801_port_role_str(unsigned int role)
{
	switch (role) {
	case WUSB3801_PORT_SNK:
		return "snk";
	case WUSB3801_PORT_SRC:
		return "src";
	case WUSB3801_PORT_AUDIO:
		return "audio";
	case WUSB3801_PORT_DEBUG:
		return "debug";
	default:
		return "standby";
	}
}

static const char *wusb3801_work_mode_str(unsigned int mode)
{
	switch (mode) {
	case 0:
		return "snk";
	case 1:
		return "src";
	case 2:
		return "drp";
	default:
		return "unknown";
	}
}

static const char *wusb3801_orientation_str(unsigned int orient)
{
	switch (orient) {
	case 1:
		return "cc1";
	case 2:
		return "cc2";
	case 3:
		return "both";
	default:
		return "standby";
	}
}

static const char *wusb3801_charge_str(unsigned int charge)
{
	switch (charge) {
	case 1:
		return "default";
	case 2:
		return "1.5a";
	case 3:
		return "3.0a";
	default:
		return "standby";
	}
}

static int wusb3801_parse_work_mode(const char *buf, size_t count, unsigned int *mode)
{
	char tmp[16];

	if (count >= sizeof(tmp))
		return -EINVAL;

	memcpy(tmp, buf, count);
	tmp[count] = '\0';
	strim(tmp);

	if (!strcasecmp(tmp, "snk") || !strcasecmp(tmp, "device"))
		*mode = 0;
	else if (!strcasecmp(tmp, "src") || !strcasecmp(tmp, "host"))
		*mode = 1;
	else if (!strcasecmp(tmp, "drp") || !strcasecmp(tmp, "dual"))
		*mode = 2;
	else
		return -EINVAL;

	return 0;
}

static const char *wusb3801_src_current_str(unsigned int level)
{
	switch (level) {
	case 0:
		return "default";
	case 1:
		return "1.5a";
	case 2:
		return "3.0a";
	default:
		return "unknown";
	}
}

static int wusb3801_parse_src_current(const char *buf, size_t count,
				      unsigned int *level_out)
{
	char tmp[16];

	if (count >= sizeof(tmp))
		return -EINVAL;

	memcpy(tmp, buf, count);
	tmp[count] = '\0';
	strim(tmp);

	if (!strcasecmp(tmp, "default") || !strcasecmp(tmp, "std"))
		*level_out = 0;
	else if (!strcasecmp(tmp, "1.5a") || !strcasecmp(tmp, "1500"))
		*level_out = 1;
	else if (!strcasecmp(tmp, "3.0a") || !strcasecmp(tmp, "3000"))
		*level_out = 2;
	else
		return -EINVAL;

	return 0;
}

static int wusb3801_read_cc_status(struct wusb3801_info *info, unsigned int *val)
{
	return regmap_read(info->regmap, WUSB3801_REG_CC_STATUS, val);
}

static int wusb3801_read_control(struct wusb3801_info *info, unsigned int *val)
{
	return regmap_read(info->regmap, WUSB3801_REG_CONTROL, val);
}

static int wusb3801_write_control(struct wusb3801_info *info, unsigned int val)
{
	return regmap_write(info->regmap, WUSB3801_REG_CONTROL, val);
}

static int wusb3801_set_work_mode(struct wusb3801_info *info, unsigned int mode)
{
	unsigned int ctrl;
	int ret;

	ret = wusb3801_read_control(info, &ctrl);
	if (ret)
		return ret;

	ctrl &= ~WUSB3801_CTRL_WORK_MODE_MASK;
	switch (mode) {
	case 0:
		ctrl |= WUSB3801_CTRL_WORK_SNK;
		break;
	case 1:
		ctrl |= WUSB3801_CTRL_WORK_SRC;
		break;
	case 2:
		ctrl |= WUSB3801_CTRL_WORK_DRP;
		break;
	default:
		return -EINVAL;
	}

	ctrl |= WUSB3801_CTRL_INT_EN;

	return wusb3801_write_control(info, ctrl);
}

static int wusb3801_set_src_current(struct wusb3801_info *info, unsigned int level)
{
	unsigned int ctrl;
	int ret;

	ret = wusb3801_read_control(info, &ctrl);
	if (ret)
		return ret;

	ctrl &= ~WUSB3801_CTRL_SRC_CURR_MASK;
	switch (level) {
	case 0:
		ctrl |= WUSB3801_CTRL_SRC_CURR_DEFAULT;
		break;
	case 1:
		ctrl |= WUSB3801_CTRL_SRC_CURR_1P5A;
		break;
	case 2:
		ctrl |= WUSB3801_CTRL_SRC_CURR_3P0A;
		break;
	default:
		return -EINVAL;
	}

	return wusb3801_write_control(info, ctrl);
}

static void wusb3801_update_state(struct wusb3801_info *info)
{
	unsigned int val;
	unsigned int port_status;
	unsigned int orientation;
	unsigned int charge;
	int ret;

	ret = wusb3801_read_cc_status(info, &val);
	if (ret) {
		dev_err(info->dev, "failed to read CC status: %d\n", ret);
		return;
	}

	port_status = FIELD_GET(WUSB3801_CC_PORT_STATUS_MASK, val);
	orientation = FIELD_GET(WUSB3801_CC_ORIENTATION_MASK, val);
	charge = FIELD_GET(WUSB3801_CC_CHG_CURRENT_MASK, val);
	dev_info(info->dev,
		 "CC status=0x%02x role=%s orientation=%s vbus=%u current=%s\n",
		 val, wusb3801_port_role_str(port_status),
		 wusb3801_orientation_str(orientation),
		 !!(val & WUSB3801_CC_VBUS_DETECTED),
		 wusb3801_charge_str(charge));

	switch (port_status) {
	case WUSB3801_PORT_SNK:
		extcon_set_state_sync(info->edev, EXTCON_USB_HOST, false);
		extcon_set_state_sync(info->edev, EXTCON_USB, true);
		break;
	case WUSB3801_PORT_SRC:
		extcon_set_state_sync(info->edev, EXTCON_USB, false);
		extcon_set_state_sync(info->edev, EXTCON_USB_HOST, true);
		break;
	default:
		extcon_set_state_sync(info->edev, EXTCON_USB_HOST, false);
		extcon_set_state_sync(info->edev, EXTCON_USB, false);
		break;
	}
}

static void wusb3801_irq_work(struct work_struct *work)
{
	struct wusb3801_info *info = container_of(work, struct wusb3801_info,
						  irq_work);
	unsigned int int_status;
	unsigned int event;
	int ret;

	mutex_lock(&info->lock);

	/* INT_STATUS is read-clear according to datasheet. */
	ret = regmap_read(info->regmap, WUSB3801_REG_INT_STATUS, &int_status);
	if (ret) {
		dev_err(info->dev, "failed to read interrupt status: %d\n", ret);
		goto out;
	}

	event = int_status & WUSB3801_INT_DETACH_ATTACH_MASK;
	dev_info(info->dev, "INT# status=0x%02x event=%u\n",
		 int_status, event);

	if (event == WUSB3801_INT_DETACHED) {
		dev_info(info->dev, "USB Type-C detached\n");
		extcon_set_state_sync(info->edev, EXTCON_USB_HOST, false);
		extcon_set_state_sync(info->edev, EXTCON_USB, false);
	} else {
		wusb3801_update_state(info);
	}

out:
	mutex_unlock(&info->lock);
}

static irqreturn_t wusb3801_irq_handler(int irq, void *data)
{
	struct wusb3801_info *info = data;

	dev_info(info->dev, "INT# asserted on irq %d\n", irq);
	schedule_work(&info->irq_work);
	return IRQ_HANDLED;
}

static int wusb3801_read_device_id(struct wusb3801_info *info)
{
	unsigned int val;
	unsigned int version;
	unsigned int vendor;
	int ret;

	ret = regmap_read(info->regmap, WUSB3801_REG_DEVICE_ID, &val);
	if (ret)
		return dev_err_probe(info->dev, ret, "failed to read device id\n");

	version = FIELD_GET(WUSB3801_DEVICE_ID_VERSION_MASK, val);
	vendor = FIELD_GET(WUSB3801_DEVICE_ID_VENDOR_MASK, val);
	dev_info(info->dev, "WUSB3801 detected: vendor=0x%x version=0x%x\n",
		 vendor, version);

	return 0;
}

static struct wusb3801_info *wusb3801_drvdata(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	return i2c_get_clientdata(client);
}

static ssize_t wusb3801_status_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int dev_id, ctrl, cc, irq_stat;
	unsigned int role, orient, charge, work_mode;
	bool vbus;
	ssize_t len = 0;
	int ret;

	mutex_lock(&info->lock);

	ret = regmap_read(info->regmap, WUSB3801_REG_DEVICE_ID, &dev_id);
	if (ret)
		goto unlock;

	ret = wusb3801_read_control(info, &ctrl);
	if (ret)
		goto unlock;

	ret = wusb3801_read_cc_status(info, &cc);
	if (ret)
		goto unlock;

	ret = regmap_read(info->regmap, WUSB3801_REG_INT_STATUS, &irq_stat);
	if (ret)
		goto unlock;

	role = FIELD_GET(WUSB3801_CC_PORT_STATUS_MASK, cc);
	orient = FIELD_GET(WUSB3801_CC_ORIENTATION_MASK, cc);
	charge = FIELD_GET(WUSB3801_CC_CHG_CURRENT_MASK, cc);
	work_mode = FIELD_GET(WUSB3801_CTRL_WORK_MODE_MASK, ctrl);
	vbus = !!(cc & WUSB3801_CC_VBUS_DETECTED);

	len = scnprintf(buf, PAGE_SIZE,
			"device_id=0x%02x vendor=0x%x version=0x%x\n"
			"role=%s work_mode=%s\n"
			"orientation=%s vbus=%d charge_current=%s\n"
			"control=0x%02x cc_status=0x%02x interrupt=0x%02x\n"
			"extcon_usb=%d extcon_usb_host=%d\n",
			dev_id,
			(unsigned int)FIELD_GET(WUSB3801_DEVICE_ID_VENDOR_MASK, dev_id),
			(unsigned int)FIELD_GET(WUSB3801_DEVICE_ID_VERSION_MASK, dev_id),
			wusb3801_port_role_str(role),
			wusb3801_work_mode_str(work_mode),
			wusb3801_orientation_str(orient),
			vbus,
			wusb3801_charge_str(charge),
			ctrl, cc, irq_stat,
			extcon_get_state(info->edev, EXTCON_USB),
			extcon_get_state(info->edev, EXTCON_USB_HOST));
unlock:
	mutex_unlock(&info->lock);

	return ret < 0 ? ret : len;
}

static ssize_t wusb3801_role_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int cc, role;
	int ret;

	mutex_lock(&info->lock);
	ret = wusb3801_read_cc_status(info, &cc);
	if (!ret)
		role = FIELD_GET(WUSB3801_CC_PORT_STATUS_MASK, cc);
	mutex_unlock(&info->lock);

	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n", wusb3801_port_role_str(role));
}

static ssize_t wusb3801_work_mode_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int ctrl, mode;
	int ret;

	mutex_lock(&info->lock);
	ret = wusb3801_read_control(info, &ctrl);
	mutex_unlock(&info->lock);

	if (ret)
		return ret;

	mode = FIELD_GET(WUSB3801_CTRL_WORK_MODE_MASK, ctrl);
	return sysfs_emit(buf, "%s\n", wusb3801_work_mode_str(mode));
}

static ssize_t wusb3801_work_mode_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int mode;
	int ret;

	ret = wusb3801_parse_work_mode(buf, count, &mode);
	if (ret)
		return ret;

	mutex_lock(&info->lock);
	ret = wusb3801_set_work_mode(info, mode);
	if (!ret)
		wusb3801_update_state(info);
	mutex_unlock(&info->lock);

	return ret ? ret : count;
}

static ssize_t wusb3801_orientation_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int cc, orient;
	int ret;

	mutex_lock(&info->lock);
	ret = wusb3801_read_cc_status(info, &cc);
	if (!ret)
		orient = FIELD_GET(WUSB3801_CC_ORIENTATION_MASK, cc);
	mutex_unlock(&info->lock);

	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n", wusb3801_orientation_str(orient));
}

static ssize_t wusb3801_vbus_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int cc;
	int ret;

	mutex_lock(&info->lock);
	ret = wusb3801_read_cc_status(info, &cc);
	mutex_unlock(&info->lock);

	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", !!(cc & WUSB3801_CC_VBUS_DETECTED));
}

static ssize_t wusb3801_charge_current_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int cc, charge;
	int ret;

	mutex_lock(&info->lock);
	ret = wusb3801_read_cc_status(info, &cc);
	if (!ret)
		charge = FIELD_GET(WUSB3801_CC_CHG_CURRENT_MASK, cc);
	mutex_unlock(&info->lock);

	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n", wusb3801_charge_str(charge));
}

static ssize_t wusb3801_src_current_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int ctrl, level;
	int ret;

	mutex_lock(&info->lock);
	ret = wusb3801_read_control(info, &ctrl);
	mutex_unlock(&info->lock);

	if (ret)
		return ret;

	level = FIELD_GET(WUSB3801_CTRL_SRC_CURR_MASK, ctrl);
	return sysfs_emit(buf, "%s\n", wusb3801_src_current_str(level));
}

static ssize_t wusb3801_src_current_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int level;
	int ret;

	ret = wusb3801_parse_src_current(buf, count, &level);
	if (ret)
		return ret;

	mutex_lock(&info->lock);
	ret = wusb3801_set_src_current(info, level);
	mutex_unlock(&info->lock);

	return ret ? ret : count;
}

static ssize_t wusb3801_control_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int ctrl;
	int ret;

	mutex_lock(&info->lock);
	ret = wusb3801_read_control(info, &ctrl);
	mutex_unlock(&info->lock);

	if (ret)
		return ret;

	return sysfs_emit(buf, "0x%02x\n", ctrl);
}

static ssize_t wusb3801_control_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int ctrl;
	int ret;

	ret = kstrtouint(buf, 0, &ctrl);
	if (ret || ctrl > 0xff)
		return -EINVAL;

	mutex_lock(&info->lock);
	ret = wusb3801_write_control(info, ctrl);
	if (!ret)
		wusb3801_update_state(info);
	mutex_unlock(&info->lock);

	return ret ? ret : count;
}

static ssize_t wusb3801_refresh_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct wusb3801_info *info = wusb3801_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret || val != 1)
		return -EINVAL;

	mutex_lock(&info->lock);
	wusb3801_update_state(info);
	mutex_unlock(&info->lock);

	return count;
}

static DEVICE_ATTR(status, 0444, wusb3801_status_show, NULL);
static DEVICE_ATTR(role, 0444, wusb3801_role_show, NULL);
static DEVICE_ATTR(work_mode, 0644, wusb3801_work_mode_show,
		   wusb3801_work_mode_store);
static DEVICE_ATTR(orientation, 0444, wusb3801_orientation_show, NULL);
static DEVICE_ATTR(vbus, 0444, wusb3801_vbus_show, NULL);
static DEVICE_ATTR(charge_current, 0444, wusb3801_charge_current_show, NULL);
static DEVICE_ATTR(src_current, 0644, wusb3801_src_current_show,
		   wusb3801_src_current_store);
static DEVICE_ATTR(control, 0644, wusb3801_control_show,
		   wusb3801_control_store);
static DEVICE_ATTR(refresh, 0200, NULL, wusb3801_refresh_store);

static struct attribute *wusb3801_attrs[] = {
	&dev_attr_status.attr,
	&dev_attr_role.attr,
	&dev_attr_work_mode.attr,
	&dev_attr_orientation.attr,
	&dev_attr_vbus.attr,
	&dev_attr_charge_current.attr,
	&dev_attr_src_current.attr,
	&dev_attr_control.attr,
	&dev_attr_refresh.attr,
	NULL,
};

static const struct attribute_group wusb3801_attr_group = {
	.attrs = wusb3801_attrs,
};

static const struct attribute_group *wusb3801_attr_groups[] = {
	&wusb3801_attr_group,
	NULL,
};

static int wusb3801_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct wusb3801_info *info;
	unsigned int int_status;
	int ret;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	info->irq = i2c->irq;
	mutex_init(&info->lock);
	INIT_WORK(&info->irq_work, wusb3801_irq_work);
	i2c_set_clientdata(i2c, info);

	info->regmap = devm_regmap_init_i2c(i2c, &wusb3801_regmap_cfg);
	if (IS_ERR(info->regmap))
		return dev_err_probe(dev, PTR_ERR(info->regmap),
				     "failed to initialize regmap\n");

	info->edev = devm_extcon_dev_allocate(dev, wusb3801_extcon_cables);
	if (IS_ERR(info->edev))
		return dev_err_probe(dev, PTR_ERR(info->edev),
				     "failed to allocate extcon device\n");

	ret = devm_extcon_dev_register(dev, info->edev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register extcon\n");

	ret = wusb3801_read_device_id(info);
	if (ret)
		return ret;

	/* Clear any pending interrupt event and sync initial state. */
	ret = regmap_read(info->regmap, WUSB3801_REG_INT_STATUS, &int_status);
	if (ret)
		dev_warn(dev, "failed to clear pending interrupt: %d\n", ret);

	mutex_lock(&info->lock);
	wusb3801_update_state(info);
	mutex_unlock(&info->lock);

	if (info->irq > 0) {
		ret = devm_request_threaded_irq(dev, info->irq, NULL,
						wusb3801_irq_handler,
						IRQF_ONESHOT |
						IRQF_TRIGGER_FALLING,
						dev_name(dev), info);
		if (ret)
			return dev_err_probe(dev, ret, "failed to request irq\n");
	}

	return 0;
}

static int wusb3801_remove(struct i2c_client *i2c)
{
	struct wusb3801_info *info = i2c_get_clientdata(i2c);

	cancel_work_sync(&info->irq_work);
	return 0;
}

static const struct of_device_id wusb3801_of_match[] = {
	{ .compatible = "willsemi,wusb3801q" },
	{ }
};
MODULE_DEVICE_TABLE(of, wusb3801_of_match);

static const struct i2c_device_id wusb3801_i2c_ids[] = {
	{ "wusb3801", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, wusb3801_i2c_ids);

static struct i2c_driver wusb3801_driver = {
	.driver = {
		.name = "wusb3801",
		.of_match_table = wusb3801_of_match,
		.dev_groups = wusb3801_attr_groups,
	},
	.probe_new = wusb3801_probe,
	.remove = wusb3801_remove,
	.id_table = wusb3801_i2c_ids,
};
module_i2c_driver(wusb3801_driver);

MODULE_DESCRIPTION("Willsemi WUSB3801 Type-C CC extcon driver");
MODULE_AUTHOR("Zonhor");
MODULE_LICENSE("GPL");
