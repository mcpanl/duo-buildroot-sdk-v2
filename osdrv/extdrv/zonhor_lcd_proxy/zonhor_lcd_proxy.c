// SPDX-License-Identifier: GPL-2.0
/*
 * Zonhor LCD proxy framebuffer: exposes /dev/fb0 backed by display_shm,
 * while FreeRTOS owns SPI3 + TE refresh when cvi.lcd_owner=rtos.
 * Also exports FreeRTOS performance stats via debugfs (rtos_stats).
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
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "display_shm.h"
#include "rtos_stats_shm.h"
#include "rtos_cmdqu.h"

#define DRVNAME		"zonhor_lcd_proxy"
#define DEF_FPS		30

struct zonhor_lcd {
	struct fb_info *info;
	struct display_shm *shm;
	struct rtos_stats_shm *stats;
	phys_addr_t phys;
	size_t map_size;
	void *vmem;
	struct mutex lock;
	struct fb_deferred_io defio;
	struct dentry *dbg_dir;
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

static int zonhor_mirror_bootarg(const char *name, int defval)
{
	const char *bootargs = zonhor_get_bootargs();
	const char *s;

	if (!bootargs)
		return defval;
	s = strstr(bootargs, name);
	if (!s)
		return defval;
	s += strlen(name);
	return (*s == '1') ? 1 : 0;
}

static void zonhor_lcd_publish_hdr(struct zonhor_lcd *lcd)
{
	u8 tmp[64];

	if (!lcd || !lcd->shm)
		return;
	/* Rewrite a full cache line then barrier so C906 sees updates. */
	memcpy(tmp, lcd->shm, sizeof(tmp));
	memcpy(lcd->shm, tmp, sizeof(tmp));
	mb();
}

static void zonhor_lcd_mirror_apply_rtos(struct zonhor_lcd *lcd)
{
	cmdqu_t cmdq = { 0 };

	if (!lcd || !lcd->shm)
		return;

	cmdq.ip_id = IP_DISPLAY;
	cmdq.cmd_id = DISPLAY_CMD_MIRROR;
	cmdq.resv.valid.linux_valid = 1;
	(void)rtos_cmdqu_send(&cmdq);
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
	zonhor_lcd_publish_hdr(lcd);

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

/* Provided by cv181x_zonhor_lcd_bl.ko when loaded */
extern int zonhor_lcd_bl_set_enable(int on) __attribute__((weak));

/*
 * Prefer zonhor_lcd_bl sysfs proxy (mailbox → RTOS soft-PWM). Fall back to
 * DISPLAY_CMD_BL directly if that module is not loaded (on=100%, off=0).
 */
static void zonhor_lcd_set_bl(struct zonhor_lcd *lcd, int on)
{
	if (lcd && lcd->shm) {
		if (on) {
			lcd->shm->bl_on = 1;
			/* Keep user/sysfs brightness; do not force 100 on fb unblank. */
		} else {
			lcd->shm->bl_on = 0;
			lcd->shm->bl_level = 0;
		}
		zonhor_lcd_publish_hdr(lcd);
	}

	if (zonhor_lcd_bl_set_enable) {
		(void)zonhor_lcd_bl_set_enable(on);
		return;
	}

	{
		cmdqu_t cmdq = { 0 };

		cmdq.ip_id = IP_DISPLAY;
		cmdq.cmd_id = DISPLAY_CMD_BL;
		cmdq.resv.valid.linux_valid = 1;
		cmdq.param_ptr = on ? 100 : 0;
		(void)rtos_cmdqu_send(&cmdq);
	}
}

static int zonhor_lcd_blank(int blank, struct fb_info *info)
{
	struct zonhor_lcd *lcd = info->par;

	zonhor_lcd_set_bl(lcd, blank == FB_BLANK_UNBLANK);
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
		"magic=0x%08x owner=%u linux_ready=%u rtos_ready=%u prog=%u\n"
		"mirror_x=%u mirror_y=%u bl_level=%u\n"
		"dirty=%u write_idx=%u frame_seq=%u te_sync_cnt=%u\n",
		lcd->shm->magic, lcd->shm->owner, lcd->shm->linux_ready,
		lcd->shm->rtos_ready, lcd->shm->reserved_mirror,
		lcd->shm->mirror_x, lcd->shm->mirror_y, lcd->shm->bl_level,
		lcd->shm->dirty, lcd->shm->write_idx,
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

static ssize_t mirror_x_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct zonhor_lcd *lcd = g_lcd;

	if (!lcd || !lcd->shm)
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "%u\n", lcd->shm->mirror_x);
}

static ssize_t mirror_x_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct zonhor_lcd *lcd = g_lcd;
	unsigned int v;

	if (!lcd || !lcd->shm)
		return -ENODEV;
	if (kstrtouint(buf, 10, &v))
		return -EINVAL;
	lcd->shm->mirror_x = v ? 1 : 0;
	wmb();
	zonhor_lcd_mirror_apply_rtos(lcd);
	return count;
}
static DEVICE_ATTR_RW(mirror_x);

static ssize_t mirror_y_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct zonhor_lcd *lcd = g_lcd;

	if (!lcd || !lcd->shm)
		return sysfs_emit(buf, "0\n");
	return sysfs_emit(buf, "%u\n", lcd->shm->mirror_y);
}

static ssize_t mirror_y_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct zonhor_lcd *lcd = g_lcd;
	unsigned int v;

	if (!lcd || !lcd->shm)
		return -ENODEV;
	if (kstrtouint(buf, 10, &v))
		return -EINVAL;
	lcd->shm->mirror_y = v ? 1 : 0;
	wmb();
	zonhor_lcd_mirror_apply_rtos(lcd);
	return count;
}
static DEVICE_ATTR_RW(mirror_y);

/* ---- RTOS stats debugfs ---- */

static struct rtos_stats_shm *zonhor_stats_ptr(struct zonhor_lcd *lcd)
{
	if (!lcd || !lcd->stats)
		return NULL;
	if (lcd->stats->magic != RTOS_STATS_SHM_MAGIC)
		return NULL;
	return lcd->stats;
}

static int zonhor_stats_read_sample(struct rtos_stats_shm *st, u32 idx,
				    struct rtos_stats_sample *out)
{
	u32 seq1, seq2;
	int tries = 0;

	do {
		seq1 = READ_ONCE(st->seq);
		if (seq1 & 1) {
			cpu_relax();
			continue;
		}
		memcpy(out, &st->samples[idx], sizeof(*out));
		/* Ensure compiler does not reorder around seq check */
		rmb();
		seq2 = READ_ONCE(st->seq);
		if (seq1 == seq2 && !(seq2 & 1))
			return 0;
	} while (++tries < 8);

	return -EAGAIN;
}

static int zonhor_stats_latest_idx(struct rtos_stats_shm *st, u32 *idx_out)
{
	u64 total;
	u32 cap, write_idx;

	if (!st || st->version != RTOS_STATS_SHM_VERSION)
		return -ENODEV;

	cap = st->ring_capacity;
	if (!cap || cap > RTOS_STATS_RING_CAP)
		cap = RTOS_STATS_RING_CAP;

	total = st->total_samples;
	if (!total)
		return -ENODATA;

	write_idx = st->write_idx % cap;
	*idx_out = (write_idx + cap - 1) % cap;
	return 0;
}

static int zonhor_stats_summary_show(struct seq_file *m, void *v)
{
	struct zonhor_lcd *lcd = m->private;
	struct rtos_stats_shm *st;
	struct rtos_stats_sample s;
	u32 idx;
	int ret, i;

	st = zonhor_stats_ptr(lcd);
	if (!st) {
		seq_puts(m, "rtos_stats: not ready (magic missing)\n");
		return 0;
	}

	ret = zonhor_stats_latest_idx(st, &idx);
	if (ret) {
		seq_puts(m, "rtos_stats: no samples yet\n");
		return 0;
	}

	ret = zonhor_stats_read_sample(st, idx, &s);
	if (ret) {
		seq_puts(m, "rtos_stats: read busy\n");
		return 0;
	}

	seq_printf(m,
		   "timestamp_ms=%u cpu_pct=%u heap_total_kb=%u heap_free_kb=%u "
		   "heap_min_free_kb=%u display_fps=%u.%02u display_ready=%u "
		   "te_sync_cnt=%u frame_seq=%u total_samples=%llu\n",
		   s.timestamp_ms, s.cpu_usage_pct, s.heap_total_kb,
		   s.heap_free_kb, s.heap_min_free_kb,
		   s.display_fps_x100 / 100, s.display_fps_x100 % 100,
		   s.display_ready, s.te_sync_cnt, s.frame_seq,
		   (unsigned long long)st->total_samples);

	seq_puts(m, "tasks:\n");
	for (i = 0; i < s.task_count && i < RTOS_STATS_MAX_TASKS; i++) {
		seq_printf(m, "  %-16s cpu=%3u%% state=%u stack_hwm=%u\n",
			   s.tasks[i].name, s.tasks[i].cpu_pct,
			   s.tasks[i].state, s.tasks[i].stack_hwm_words);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(zonhor_stats_summary);

static int zonhor_stats_history_show(struct seq_file *m, void *v)
{
	struct zonhor_lcd *lcd = m->private;
	struct rtos_stats_shm *st;
	struct rtos_stats_sample s;
	u64 total;
	u32 cap, write_idx, start, n, i, j, idx;

	st = zonhor_stats_ptr(lcd);
	if (!st) {
		seq_puts(m, "# rtos_stats not ready\n");
		return 0;
	}

	cap = st->ring_capacity;
	if (!cap || cap > RTOS_STATS_RING_CAP)
		cap = RTOS_STATS_RING_CAP;
	total = st->total_samples;
	if (!total) {
		seq_puts(m, "# no samples\n");
		return 0;
	}

	n = (total < cap) ? (u32)total : cap;
	write_idx = st->write_idx % cap;
	start = (write_idx + cap - n) % cap;

	seq_puts(m,
		 "timestamp_ms,cpu_pct,heap_total_kb,heap_free_kb,heap_min_free_kb,"
		 "display_fps_x100,te_sync_cnt,frame_seq,display_ready,task_count");
	for (j = 0; j < RTOS_STATS_MAX_TASKS; j++)
		seq_printf(m, ",task%u_name,task%u_cpu,task%u_state,task%u_stack",
			   j, j, j, j);
	seq_putc(m, '\n');

	for (i = 0; i < n; i++) {
		idx = (start + i) % cap;
		if (zonhor_stats_read_sample(st, idx, &s))
			continue;
		seq_printf(m, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
			   s.timestamp_ms, s.cpu_usage_pct, s.heap_total_kb,
			   s.heap_free_kb, s.heap_min_free_kb,
			   s.display_fps_x100, s.te_sync_cnt, s.frame_seq,
			   s.display_ready, s.task_count);
		for (j = 0; j < RTOS_STATS_MAX_TASKS; j++) {
			if (j < s.task_count)
				seq_printf(m, ",%s,%u,%u,%u",
					   s.tasks[j].name, s.tasks[j].cpu_pct,
					   s.tasks[j].state,
					   s.tasks[j].stack_hwm_words);
			else
				seq_puts(m, ",,,,");
		}
		seq_putc(m, '\n');
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(zonhor_stats_history);

static int zonhor_stats_fps_show(struct seq_file *m, void *v)
{
	struct zonhor_lcd *lcd = m->private;
	struct rtos_stats_shm *st;
	struct rtos_stats_sample s;
	u64 total;
	u32 cap, write_idx, start, n, i, idx;

	st = zonhor_stats_ptr(lcd);
	if (!st) {
		seq_puts(m, "# rtos_stats not ready\n");
		return 0;
	}

	cap = st->ring_capacity;
	if (!cap || cap > RTOS_STATS_RING_CAP)
		cap = RTOS_STATS_RING_CAP;
	total = st->total_samples;
	if (!total) {
		seq_puts(m, "# no samples\n");
		return 0;
	}

	n = (total < cap) ? (u32)total : cap;
	write_idx = st->write_idx % cap;
	start = (write_idx + cap - n) % cap;

	seq_puts(m, "timestamp_ms,display_fps_x100,te_sync_cnt,frame_seq\n");
	for (i = 0; i < n; i++) {
		idx = (start + i) % cap;
		if (zonhor_stats_read_sample(st, idx, &s))
			continue;
		seq_printf(m, "%u,%u,%u,%u\n", s.timestamp_ms,
			   s.display_fps_x100, s.te_sync_cnt, s.frame_seq);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(zonhor_stats_fps);

static int zonhor_stats_interval_show(struct seq_file *m, void *v)
{
	struct zonhor_lcd *lcd = m->private;
	struct rtos_stats_shm *st = zonhor_stats_ptr(lcd);

	if (!st)
		seq_printf(m, "%u\n", RTOS_STATS_SAMPLE_INTERVAL_MS);
	else
		seq_printf(m, "%u\n", st->sample_interval_ms);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(zonhor_stats_interval);

static void zonhor_stats_debugfs_init(struct zonhor_lcd *lcd)
{
	lcd->dbg_dir = debugfs_create_dir("rtos_stats", NULL);
	if (IS_ERR_OR_NULL(lcd->dbg_dir)) {
		lcd->dbg_dir = NULL;
		return;
	}
	debugfs_create_file("summary", 0444, lcd->dbg_dir, lcd,
			    &zonhor_stats_summary_fops);
	debugfs_create_file("history", 0444, lcd->dbg_dir, lcd,
			    &zonhor_stats_history_fops);
	debugfs_create_file("fps", 0444, lcd->dbg_dir, lcd,
			    &zonhor_stats_fps_fops);
	debugfs_create_file("interval_ms", 0444, lcd->dbg_dir, lcd,
			    &zonhor_stats_interval_fops);
}

static void zonhor_stats_debugfs_exit(struct zonhor_lcd *lcd)
{
	debugfs_remove_recursive(lcd->dbg_dir);
	lcd->dbg_dir = NULL;
}

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
	if (lcd->map_size < RTOS_STATS_SHM_OFFSET + sizeof(struct rtos_stats_shm)) {
		dev_err(dev, "display_shm too small for rtos_stats @0x%x\n",
			RTOS_STATS_SHM_OFFSET);
		return -ENOMEM;
	}

	/*
	 * Write-through mapping so handshake flags (linux_ready, dirty) are
	 * pushed toward DRAM for C906L inv_dcache reads. Plain WB would need
	 * explicit clean; WC can leave small stores buffered.
	 */
	lcd->shm = memremap(lcd->phys, lcd->map_size, MEMREMAP_WT);
	if (!lcd->shm)
		lcd->shm = memremap(lcd->phys, lcd->map_size, MEMREMAP_WC);
	if (!lcd->shm)
		return -ENOMEM;

	lcd->stats = (struct rtos_stats_shm *)((u8 *)lcd->shm +
					       RTOS_STATS_SHM_OFFSET);

	if (lcd->shm->magic != DISPLAY_SHM_MAGIC) {
		dev_warn(dev, "display_shm magic missing, initializing\n");
		memset(lcd->shm, 0, offsetof(struct display_shm, buf));
		lcd->shm->magic = DISPLAY_SHM_MAGIC;
		lcd->shm->version = DISPLAY_SHM_VERSION;
		lcd->shm->owner = DISPLAY_OWNER_RTOS;
		lcd->shm->bl_on = 1;
		lcd->shm->mirror_x = zonhor_mirror_bootarg("cvi.lcd_mirror_x=",
							   LCD_MIRROR_X_DEFAULT);
		lcd->shm->mirror_y = zonhor_mirror_bootarg("cvi.lcd_mirror_y=",
							   LCD_MIRROR_Y_DEFAULT);
	} else {
		lcd->shm->owner = DISPLAY_OWNER_RTOS;
		if (lcd->shm->version < DISPLAY_SHM_VERSION) {
			lcd->shm->mirror_x = zonhor_mirror_bootarg(
				"cvi.lcd_mirror_x=", LCD_MIRROR_X_DEFAULT);
			lcd->shm->mirror_y = zonhor_mirror_bootarg(
				"cvi.lcd_mirror_y=", LCD_MIRROR_Y_DEFAULT);
			lcd->shm->version = DISPLAY_SHM_VERSION;
		}
	}

	/*
	 * Signal RTOS that Linux SPI3 has had a chance to probe (and should
	 * have returned -ENODEV). RTOS must not open SPI before this.
	 */
	lcd->shm->linux_ready = 1;
	zonhor_lcd_publish_hdr(lcd);
	dev_info(dev, "linux_ready=1, mirror=%u,%u, waiting for RTOS SPI\n",
		 lcd->shm->mirror_x, lcd->shm->mirror_y);

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
	device_create_file(dev, &dev_attr_mirror_x);
	device_create_file(dev, &dev_attr_mirror_y);

	zonhor_stats_debugfs_init(lcd);

	platform_set_drvdata(pdev, lcd);
	g_lcd = lcd;
	dev_info(dev,
		 "fb%d: zonhor lcd proxy %dx%d RGB565, shm@%pa (RTOS SPI+TE), stats@+0x%x\n",
		 info->node, DISPLAY_W, DISPLAY_H, &lcd->phys,
		 RTOS_STATS_SHM_OFFSET);
	return 0;
}

static int zonhor_lcd_remove(struct platform_device *pdev)
{
	struct zonhor_lcd *lcd = platform_get_drvdata(pdev);

	if (!lcd)
		return 0;
	zonhor_stats_debugfs_exit(lcd);
	device_remove_file(&pdev->dev, &dev_attr_status);
	device_remove_file(&pdev->dev, &dev_attr_flush);
	device_remove_file(&pdev->dev, &dev_attr_mirror_x);
	device_remove_file(&pdev->dev, &dev_attr_mirror_y);
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
