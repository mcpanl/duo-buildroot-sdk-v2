// SPDX-License-Identifier: GPL-2.0
/*
 * Zonhor LCD proxy framebuffer: exposes /dev/fb0 backed by display_shm,
 * while FreeRTOS owns SPI3 + TE refresh when cvi.lcd_owner=rtos.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fb.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/vmalloc.h>
#include <linux/device.h>

#include "display_shm.h"
#include "rtos_cmdqu.h"

#define DRVNAME		"zonhor_lcd_proxy"
#define DEF_FPS		30

struct zonhor_lcd {
	struct fb_info *info;
	struct display_shm *shm;
	phys_addr_t phys;
	size_t map_size;
	void *vmem;
	struct mutex lock;
	struct fb_deferred_io defio;
};

static struct zonhor_lcd *g_lcd;

static const char *zonhor_get_bootargs(void)
{
	struct device_node *chosen;
	const char *bootargs = NULL;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return NULL;
	of_property_read_string(chosen, "bootargs", &bootargs);
	of_node_put(chosen);
	return bootargs;
}

static bool zonhor_lcd_owner_is_rtos(void)
{
	const char *bootargs = zonhor_get_bootargs();
	const char *s;

	if (!bootargs)
		return true;
	s = strstr(bootargs, "cvi.lcd_owner=");
	if (!s)
		return true;
	s += strlen("cvi.lcd_owner=");
	return strncmp(s, "linux", 5) != 0;
}

static void zonhor_lcd_submit(struct zonhor_lcd *lcd)
{
	cmdqu_t cmdq = { 0 };
	u32 idx = 0;

	if (!lcd->shm || !lcd->vmem)
		return;

	memcpy(lcd->shm->buf[idx], lcd->vmem, DISPLAY_FRAME_BYTES);

	lcd->shm->write_idx = idx;
	lcd->shm->dirty = 1;
	lcd->shm->frame_seq++;
	wmb();

	cmdq.ip_id = IP_DISPLAY;
	cmdq.cmd_id = DISPLAY_CMD_FLUSH;
	cmdq.resv.valid.linux_valid = 1;
	cmdq.param_ptr = 0;
	(void)rtos_cmdqu_send(&cmdq);

	pr_debug("%s: dirty=1 seq=%u rtos_ready=%u te=%u\n",
		 DRVNAME, lcd->shm->frame_seq, lcd->shm->rtos_ready,
		 lcd->shm->te_sync_cnt);
}

static void zonhor_lcd_deferred_io(struct fb_info *info,
				   struct list_head *pagelist)
{
	struct zonhor_lcd *lcd = info->par;

	mutex_lock(&lcd->lock);
	zonhor_lcd_submit(lcd);
	mutex_unlock(&lcd->lock);
}

static ssize_t zonhor_lcd_write(struct fb_info *info, const char __user *buf,
				size_t count, loff_t *ppos)
{
	ssize_t ret = fb_sys_write(info, buf, count, ppos);
	struct zonhor_lcd *lcd = info->par;

	if (ret > 0) {
		mutex_lock(&lcd->lock);
		zonhor_lcd_submit(lcd);
		mutex_unlock(&lcd->lock);
	}
	return ret;
}

static int zonhor_lcd_blank(int blank, struct fb_info *info)
{
	cmdqu_t cmdq = { 0 };
	struct zonhor_lcd *lcd = info->par;

	cmdq.ip_id = IP_DISPLAY;
	cmdq.cmd_id = DISPLAY_CMD_BL;
	cmdq.resv.valid.linux_valid = 1;
	cmdq.param_ptr = (blank == FB_BLANK_UNBLANK) ? 1 : 0;
	(void)rtos_cmdqu_send(&cmdq);

	if (lcd->shm) {
		lcd->shm->bl_on = cmdq.param_ptr ? 1 : 0;
		wmb();
	}
	return 0;
}

static struct fb_ops zonhor_lcd_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= zonhor_lcd_write,
	.fb_fillrect	= sys_fillrect,
	.fb_copyarea	= sys_copyarea,
	.fb_imageblit	= sys_imageblit,
	.fb_blank	= zonhor_lcd_blank,
};

static ssize_t status_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct zonhor_lcd *lcd = g_lcd;

	if (!lcd || !lcd->shm)
		return sysfs_emit(buf, "no shm\n");

	return sysfs_emit(buf,
		"magic=0x%08x owner=%u linux_ready=%u rtos_ready=%u\n"
		"dirty=%u write_idx=%u frame_seq=%u te_sync_cnt=%u\n",
		lcd->shm->magic, lcd->shm->owner, lcd->shm->linux_ready,
		lcd->shm->rtos_ready, lcd->shm->dirty, lcd->shm->write_idx,
		lcd->shm->frame_seq, lcd->shm->te_sync_cnt);
}
static DEVICE_ATTR_RO(status);

static ssize_t flush_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct zonhor_lcd *lcd = g_lcd;

	if (!lcd)
		return -ENODEV;
	mutex_lock(&lcd->lock);
	zonhor_lcd_submit(lcd);
	mutex_unlock(&lcd->lock);
	return count;
}
static DEVICE_ATTR_WO(flush);

static int zonhor_lcd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zonhor_lcd *lcd;
	struct fb_info *info;
	struct device_node *np;
	struct resource res;
	int ret;

	if (!zonhor_lcd_owner_is_rtos()) {
		dev_info(dev, "skipped: cvi.lcd_owner=linux (fbtft owns SPI)\n");
		return -ENODEV;
	}

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np) {
		dev_err(dev, "missing memory-region\n");
		return -EINVAL;
	}
	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret)
		return ret;

	lcd = devm_kzalloc(dev, sizeof(*lcd), GFP_KERNEL);
	if (!lcd)
		return -ENOMEM;

	mutex_init(&lcd->lock);
	lcd->phys = res.start;
	lcd->map_size = resource_size(&res);
	if (lcd->map_size < sizeof(struct display_shm)) {
		dev_err(dev, "display_shm region too small\n");
		return -ENOMEM;
	}

	lcd->shm = memremap(lcd->phys, lcd->map_size, MEMREMAP_WC);
	if (!lcd->shm)
		return -ENOMEM;

	if (lcd->shm->magic != DISPLAY_SHM_MAGIC) {
		dev_warn(dev, "display_shm magic missing, initializing\n");
		memset(lcd->shm, 0, offsetof(struct display_shm, buf));
		lcd->shm->magic = DISPLAY_SHM_MAGIC;
		lcd->shm->version = DISPLAY_SHM_VERSION;
		lcd->shm->owner = DISPLAY_OWNER_RTOS;
		lcd->shm->bl_on = 1;
	} else {
		lcd->shm->owner = DISPLAY_OWNER_RTOS;
	}

	/*
	 * Signal RTOS that Linux SPI3 has had a chance to probe (and should
	 * have returned -ENODEV). RTOS must not open SPI before this.
	 */
	lcd->shm->linux_ready = 1;
	wmb();
	dev_info(dev, "linux_ready=1, waiting for RTOS SPI bring-up\n");

	lcd->vmem = vzalloc(DISPLAY_FRAME_BYTES);
	if (!lcd->vmem) {
		memunmap(lcd->shm);
		return -ENOMEM;
	}

	info = framebuffer_alloc(0, dev);
	if (!info) {
		vfree(lcd->vmem);
		memunmap(lcd->shm);
		return -ENOMEM;
	}

	lcd->info = info;
	info->par = lcd;
	info->fbops = &zonhor_lcd_ops;
	info->flags = FBINFO_FLAG_DEFAULT | FBINFO_VIRTFB;
	info->screen_size = DISPLAY_FRAME_BYTES;
	info->screen_base = lcd->vmem;
	info->fix.smem_start = (unsigned long)lcd->vmem;
	info->fix.smem_len = DISPLAY_FRAME_BYTES;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.line_length = DISPLAY_W * 2;

	info->var.xres = DISPLAY_W;
	info->var.yres = DISPLAY_H;
	info->var.xres_virtual = DISPLAY_W;
	info->var.yres_virtual = DISPLAY_H;
	info->var.bits_per_pixel = 16;
	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.offset = 0;
	info->var.blue.length = 5;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.height = -1;
	info->var.width = -1;

	lcd->defio.delay = HZ / DEF_FPS;
	lcd->defio.deferred_io = zonhor_lcd_deferred_io;
	info->fbdefio = &lcd->defio;
	fb_deferred_io_init(info);

	ret = register_framebuffer(info);
	if (ret) {
		fb_deferred_io_cleanup(info);
		framebuffer_release(info);
		vfree(lcd->vmem);
		memunmap(lcd->shm);
		return ret;
	}

	device_create_file(dev, &dev_attr_status);
	device_create_file(dev, &dev_attr_flush);

	platform_set_drvdata(pdev, lcd);
	g_lcd = lcd;
	dev_info(dev,
		 "fb%d: zonhor lcd proxy %dx%d RGB565, shm@%pa (RTOS SPI+TE)\n",
		 info->node, DISPLAY_W, DISPLAY_H, &lcd->phys);
	return 0;
}

static int zonhor_lcd_remove(struct platform_device *pdev)
{
	struct zonhor_lcd *lcd = platform_get_drvdata(pdev);

	if (!lcd)
		return 0;
	device_remove_file(&pdev->dev, &dev_attr_status);
	device_remove_file(&pdev->dev, &dev_attr_flush);
	g_lcd = NULL;
	unregister_framebuffer(lcd->info);
	fb_deferred_io_cleanup(lcd->info);
	framebuffer_release(lcd->info);
	vfree(lcd->vmem);
	memunmap(lcd->shm);
	return 0;
}

static const struct of_device_id zonhor_lcd_of_match[] = {
	{ .compatible = "cvitek,zonhor-lcd-proxy" },
	{},
};
MODULE_DEVICE_TABLE(of, zonhor_lcd_of_match);

static struct platform_driver zonhor_lcd_driver = {
	.probe = zonhor_lcd_probe,
	.remove = zonhor_lcd_remove,
	.driver = {
		.name = DRVNAME,
		.of_match_table = zonhor_lcd_of_match,
	},
};

static int __init zonhor_lcd_init(void)
{
	if (!zonhor_lcd_owner_is_rtos()) {
		pr_info("%s: cvi.lcd_owner=linux, not loading\n", DRVNAME);
		return 0;
	}
	return platform_driver_register(&zonhor_lcd_driver);
}

static void __exit zonhor_lcd_exit(void)
{
	if (zonhor_lcd_owner_is_rtos())
		platform_driver_unregister(&zonhor_lcd_driver);
}

module_init(zonhor_lcd_init);
module_exit(zonhor_lcd_exit);

MODULE_DESCRIPTION("Zonhor LCD proxy fbdev over FreeRTOS SPI display");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Zonhor");
