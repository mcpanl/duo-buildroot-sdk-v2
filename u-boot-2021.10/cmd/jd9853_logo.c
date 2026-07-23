// SPDX-License-Identifier: GPL-2.0+
/*
 * U-Boot early splash / debug helpers for Zonhor SPI JD9853 172x320 panel.
 *
 * Boot path: jd9853_logo (init + embedded RGB565 logo + backlight)
 * Debug (manual only):
 *   jd9853 init
 *   jd9853 fill [rgb565_hex]
 *   jd9853 bl <0|1>
 *   jd9853 logo
 */

#include <common.h>
#include <command.h>
#include <dm.h>
#include <dm/device.h>
#include <dm/ofnode.h>
#include <env.h>
#include <errno.h>
#include <spi.h>
#include <asm/gpio.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <asm/byteorder.h>
#include <cpu_func.h>
#include <cvi_board_memmap.h>
#include <display_shm.h>

#define JD9853_WIDTH		172
#define JD9853_HEIGHT		320
#define JD9853_X_OFFSET		34
#define JD9853_SPI_CS		0
#define JD9853_SPI_HZ		40000000
#define JD9853_SPI_MODE		SPI_MODE_0
#define JD9853_RGB565_BLUE	0x001F

#define DCS_SLPOUT		0x11
#define DCS_DISPON		0x29
#define DCS_CASET		0x2a
#define DCS_PASET		0x2b
#define DCS_RAMWR		0x2c
#define DCS_MADCTL		0x36
#define DCS_COLMOD		0x3a
#define DCS_INVOFF		0x20
#define DCS_INVON		0x21
#define DCS_TEON		0x35

extern const u8 jd9853_logo_data[];
extern const u8 jd9853_logo_end[];

struct jd9853_priv {
	struct spi_slave *slave;
	struct gpio_desc gpio_dc;
	struct gpio_desc gpio_reset;
	struct gpio_desc gpio_te;
	struct gpio_desc gpio_bl;
	bool ready;
	u8 wrbuf[4096];
};

static struct jd9853_priv jd9853;

static void jd9853_dbg_gpio(const char *name, struct gpio_desc *desc)
{
	struct gpio_dev_priv *uc_priv;
	const char *bank = "?";
	int val;

	if (!dm_gpio_is_valid(desc)) {
		printf("jd9853:   %-5s: invalid\n", name);
		return;
	}

	uc_priv = dev_get_uclass_priv(desc->dev);
	if (uc_priv && uc_priv->bank_name)
		bank = uc_priv->bank_name;

	val = dm_gpio_get_value(desc);
	printf("jd9853:   %-5s: %s%d flags=0x%lx logical=%d\n",
	       name, bank, desc->offset, (ulong)desc->flags, val);
}

static int jd9853_find_panel_node(ofnode *panel_node)
{
	*panel_node = ofnode_by_compatible(ofnode_null(), "jadard,jd9853");
	if (!ofnode_valid(*panel_node)) {
		printf("jd9853: DT node jadard,jd9853 not found\n");
		return -ENODEV;
	}
	printf("jd9853: found DT node '%s'\n", ofnode_get_name(*panel_node));
	return 0;
}

static int jd9853_gpio_setup(struct jd9853_priv *priv, ofnode panel_node)
{
	int ret;

	printf("jd9853: requesting GPIOs (dc/reset/led)...\n");

	ret = gpio_request_by_name_nodev(panel_node, "dc", 0, &priv->gpio_dc,
					 GPIOD_IS_OUT);
	if (ret) {
		printf("jd9853: request dc failed (%d)\n", ret);
		return ret;
	}

	ret = gpio_request_by_name_nodev(panel_node, "reset", 0,
					 &priv->gpio_reset, GPIOD_IS_OUT);
	if (ret) {
		printf("jd9853: request reset failed (%d)\n", ret);
		return ret;
	}

	ret = gpio_request_by_name_nodev(panel_node, "led", 0, &priv->gpio_bl,
					 GPIOD_IS_OUT);
	if (ret) {
		printf("jd9853: request led/backlight failed (%d)\n", ret);
		return ret;
	}

	ret = gpio_request_by_name_nodev(panel_node, "te", 0, &priv->gpio_te,
					 GPIOD_IS_IN);
	if (ret) {
		printf("jd9853: request te failed (%d)\n", ret);
		return ret;
	}

	jd9853_dbg_gpio("dc", &priv->gpio_dc);
	jd9853_dbg_gpio("reset", &priv->gpio_reset);
	jd9853_dbg_gpio("te", &priv->gpio_te);
	jd9853_dbg_gpio("led", &priv->gpio_bl);
	return 0;
}

static int jd9853_spi_setup(struct jd9853_priv *priv, ofnode panel_node)
{
	struct udevice *bus;
	ofnode spi_node;
	char name[32];
	int ret, busnum, cs;
	u32 speed;

	spi_node = ofnode_get_parent(panel_node);
	printf("jd9853: SPI parent '%s'\n", ofnode_get_name(spi_node));

	ret = uclass_get_device_by_ofnode(UCLASS_SPI, spi_node, &bus);
	if (ret) {
		printf("jd9853: SPI controller probe/find failed (%d)\n", ret);
		return ret;
	}

	busnum = dev_seq(bus);
	cs = ofnode_read_u32_default(panel_node, "reg", JD9853_SPI_CS);
	speed = ofnode_read_u32_default(panel_node, "spi-max-frequency",
					JD9853_SPI_HZ);

	printf("jd9853: SPI bus=%d cs=%d speed=%u Hz mode=%d (%s)\n",
	       busnum, cs, speed, JD9853_SPI_MODE, bus->name);

	snprintf(name, sizeof(name), "jd9853_%d:%d", busnum, cs);
	ret = spi_get_bus_and_cs(busnum, cs, speed, JD9853_SPI_MODE,
				 "spi_generic_drv", name, &bus, &priv->slave);
	if (ret) {
		printf("jd9853: spi_get_bus_and_cs failed (%d)\n", ret);
		return ret;
	}

	ret = spi_claim_bus(priv->slave);
	if (ret) {
		printf("jd9853: spi_claim_bus failed (%d)\n", ret);
		return ret;
	}

	printf("jd9853: SPI claimed OK\n");
	return 0;
}

static int jd9853_spi_write(struct jd9853_priv *priv, const void *buf,
			    size_t len)
{
	return spi_xfer(priv->slave, len * 8, buf, NULL,
			SPI_XFER_BEGIN | SPI_XFER_END);
}

static int jd9853_write_cmd(struct jd9853_priv *priv, u8 cmd)
{
	int ret;

	dm_gpio_set_value(&priv->gpio_dc, 0);
	ret = jd9853_spi_write(priv, &cmd, 1);
	if (ret)
		printf("jd9853: write_cmd 0x%02x failed (%d)\n", cmd, ret);
	return ret;
}

static int jd9853_write_data(struct jd9853_priv *priv, const void *buf,
			     size_t len)
{
	int ret;

	dm_gpio_set_value(&priv->gpio_dc, 1);
	ret = jd9853_spi_write(priv, buf, len);
	if (ret)
		printf("jd9853: write_data len=%zu failed (%d)\n", len, ret);
	return ret;
}

static int jd9853_write_reg(struct jd9853_priv *priv, u8 cmd, const u8 *data,
			    size_t len)
{
	int ret;

	ret = jd9853_write_cmd(priv, cmd);
	if (ret)
		return ret;
	if (len)
		ret = jd9853_write_data(priv, data, len);
	return ret;
}

/*
 * DT marks reset as GPIO_ACTIVE_LOW, so dm_gpio_set_value() uses logical
 * levels: 1=assert (phys low), 0=deassert (phys high). Match fbtft_reset()
 * physical pulse: idle high -> assert low -> idle high.
 */
static void jd9853_reset_panel(struct jd9853_priv *priv)
{
	printf("jd9853: reset pulse (logical assert=1 for ACTIVE_LOW)...\n");
	dm_gpio_set_value(&priv->gpio_reset, 0);
	printf("jd9853:   reset deassert, logical=%d\n",
	       dm_gpio_get_value(&priv->gpio_reset));
	udelay(30);
	dm_gpio_set_value(&priv->gpio_reset, 1);
	printf("jd9853:   reset assert,   logical=%d\n",
	       dm_gpio_get_value(&priv->gpio_reset));
	mdelay(120);
	dm_gpio_set_value(&priv->gpio_reset, 0);
	printf("jd9853:   reset release,  logical=%d\n",
	       dm_gpio_get_value(&priv->gpio_reset));
	mdelay(10);
}

#define NUMARGS(...) (sizeof((u8[]){ __VA_ARGS__ }))

static int jd9853_write_reg_buf(struct jd9853_priv *priv, u8 cmd,
				const u8 *data, size_t len)
{
	return jd9853_write_reg(priv, cmd, data, len);
}

#define WR_REG(priv, cmd, ...) \
	jd9853_write_reg_buf(priv, cmd, (u8[]){ __VA_ARGS__ }, \
			     NUMARGS(__VA_ARGS__))

static int jd9853_env_mirror(const char *name, int defval);
static u8 jd9853_madctl_from_env(void);
static int jd9853_apply_orientation(struct jd9853_priv *priv);
static void jd9853_wait_te(struct jd9853_priv *priv);

static int jd9853_init_panel(struct jd9853_priv *priv)
{
	int ret;

	printf("jd9853: panel init start\n");
	jd9853_reset_panel(priv);

	printf("jd9853: SLPOUT\n");
	ret = jd9853_write_cmd(priv, DCS_SLPOUT);
	if (ret)
		return ret;
	mdelay(120);

	printf("jd9853: vendor init sequence...\n");
	ret = WR_REG(priv, 0xDF, 0x98, 0x53);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xDF, 0x98, 0x53);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xB2, 0x23);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xB7, 0x00, 0x47, 0x00, 0x6F);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xBB, 0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xC0, 0x44, 0xA4);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xC1, 0x16);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xC3, 0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xC4, 0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79,
		     0x0B, 0x0A, 0x16, 0x82);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xC8, 0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
		     0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00, 0x3F,
		     0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26,
		     0x25, 0x17, 0x12, 0x0D, 0x04, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xD0, 0x04, 0x06, 0x6B, 0x0F, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xD7, 0x00, 0x30);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xE6, 0x14);
	if (ret)
		return ret;

	ret = WR_REG(priv, 0xDE, 0x01);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xB7, 0x03, 0x13, 0xEF, 0x35, 0x35);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xC1, 0x14, 0x15, 0xC0);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xC2, 0x06, 0x3A);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xC4, 0x72, 0x12);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xBE, 0x00);
	if (ret)
		return ret;

	ret = WR_REG(priv, 0xDE, 0x02);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xE5, 0x00, 0x02, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xE5, 0x01, 0x02, 0x00);
	if (ret)
		return ret;

	ret = WR_REG(priv, 0xDE, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(priv, DCS_TEON, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(priv, DCS_COLMOD, 0x05);
	if (ret)
		return ret;
	ret = WR_REG(priv, DCS_CASET, 0x00, 0x22, 0x00, 0xCD);
	if (ret)
		return ret;
	ret = WR_REG(priv, DCS_PASET, 0x00, 0x00, 0x01, 0x3F);
	if (ret)
		return ret;

	ret = WR_REG(priv, 0xDE, 0x02);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xE5, 0x00, 0x02, 0x00);
	if (ret)
		return ret;
	ret = WR_REG(priv, 0xDE, 0x00);
	if (ret)
		return ret;

	printf("jd9853: DISPON + orientation\n");
	ret = jd9853_write_cmd(priv, DCS_DISPON);
	if (ret)
		return ret;
	mdelay(20);

	ret = jd9853_apply_orientation(priv);
	if (ret)
		return ret;

	printf("jd9853: panel init OK\n");
	return 0;
}

static int jd9853_env_mirror(const char *name, int defval)
{
	const char *s = env_get(name);

	if (!s)
		return defval;
	return s[0] == '1';
}

static u8 jd9853_madctl_from_env(void)
{
	u8 madctl = 0;

	if (jd9853_env_mirror("lcd_mirror_x", LCD_MIRROR_X_DEFAULT))
		madctl |= BIT(6);
	if (jd9853_env_mirror("lcd_mirror_y", LCD_MIRROR_Y_DEFAULT))
		madctl |= BIT(7);
	return madctl;
}

/*
 * Match FreeRTOS jd9853_apply_orientation(): MADCTL + INVOFF/INVON + TEON.
 * INVOFF before INVON avoids an unknown inversion state after warm reset or
 * SPI glitches; re-apply immediately before pixel writes for boot logo.
 */
static int jd9853_apply_orientation(struct jd9853_priv *priv)
{
	u8 madctl = jd9853_madctl_from_env();
	int ret;

	ret = WR_REG(priv, DCS_MADCTL, madctl);
	if (ret)
		return ret;
	ret = jd9853_write_cmd(priv, DCS_INVOFF);
	if (ret)
		return ret;
	ret = jd9853_write_cmd(priv, DCS_INVON);
	if (ret)
		return ret;
	return WR_REG(priv, DCS_TEON, 0x00);
}

static void jd9853_wait_te(struct jd9853_priv *priv)
{
	unsigned int guard = 50000;

	if (!dm_gpio_is_valid(&priv->gpio_te))
		return;

	/* Match RTOS/Linux: exit active VBANK, then wait for TE rising edge */
	while (dm_gpio_get_value(&priv->gpio_te) && guard--)
		udelay(100);
	guard = 50000;
	while (!dm_gpio_get_value(&priv->gpio_te) && guard--)
		udelay(100);
}

static int jd9853_set_addr_win(struct jd9853_priv *priv, int xs, int ys,
			       int xe, int ye)
{
	u8 caset[4], paset[4];
	int ret;

	xs += JD9853_X_OFFSET;
	xe += JD9853_X_OFFSET;

	caset[0] = (xs >> 8) & 0xff;
	caset[1] = xs & 0xff;
	caset[2] = (xe >> 8) & 0xff;
	caset[3] = xe & 0xff;
	paset[0] = (ys >> 8) & 0xff;
	paset[1] = ys & 0xff;
	paset[2] = (ye >> 8) & 0xff;
	paset[3] = ye & 0xff;

	ret = jd9853_write_reg(priv, DCS_CASET, caset, sizeof(caset));
	if (ret)
		return ret;
	ret = jd9853_write_reg(priv, DCS_PASET, paset, sizeof(paset));
	if (ret)
		return ret;
	return jd9853_write_cmd(priv, DCS_RAMWR);
}

/* Match fbtft write_vmem16_bus8: send RGB565 as big-endian on the wire. */
static int jd9853_write_pixels_be(struct jd9853_priv *priv, const u16 *src,
				  size_t pixels)
{
	size_t off = 0;
	int ret;

	printf("jd9853: pixel write %zu px (%zu bytes, BE)\n",
	       pixels, pixels * 2);
	dm_gpio_set_value(&priv->gpio_dc, 1);
	while (off < pixels) {
		size_t i, n = min(sizeof(priv->wrbuf) / 2, pixels - off);
		u16 *dst = (u16 *)priv->wrbuf;

		for (i = 0; i < n; i++)
			dst[i] = cpu_to_be16(src[off + i]);

		ret = jd9853_spi_write(priv, priv->wrbuf, n * 2);
		if (ret) {
			printf("jd9853: pixel xfer failed at %zu/%zu (%d)\n",
			       off, pixels, ret);
			return ret;
		}
		off += n;
	}

	printf("jd9853: pixel write done\n");
	return 0;
}

static int jd9853_blit_logo(struct jd9853_priv *priv)
{
	const u16 *src = (const u16 *)jd9853_logo_data;
	size_t total = jd9853_logo_end - jd9853_logo_data;
	size_t pixels = JD9853_WIDTH * JD9853_HEIGHT;
	int ret;

	printf("jd9853: logo blit %dx%d, data=%zu bytes\n",
	       JD9853_WIDTH, JD9853_HEIGHT, total);

	if (total != pixels * 2) {
		printf("jd9853: bad logo size %zu (expected %zu)\n",
		       total, pixels * 2);
		return -EINVAL;
	}

	/* Re-apply scan order right before blit (init may be ms earlier). */
	ret = jd9853_apply_orientation(priv);
	if (ret) {
		printf("jd9853: apply_orientation failed (%d)\n", ret);
		return ret;
	}
	jd9853_wait_te(priv);

	ret = jd9853_set_addr_win(priv, 0, 0, JD9853_WIDTH - 1,
				  JD9853_HEIGHT - 1);
	if (ret) {
		printf("jd9853: set_addr_win failed (%d)\n", ret);
		return ret;
	}

	return jd9853_write_pixels_be(priv, src, pixels);
}

static int jd9853_fill_color(struct jd9853_priv *priv, u16 color)
{
	size_t pixels = JD9853_WIDTH * JD9853_HEIGHT;
	size_t off = 0;
	u16 be = cpu_to_be16(color);
	int ret;

	printf("jd9853: fill color=0x%04x (%zu px)\n", color, pixels);

	ret = jd9853_set_addr_win(priv, 0, 0, JD9853_WIDTH - 1,
				  JD9853_HEIGHT - 1);
	if (ret) {
		printf("jd9853: set_addr_win failed (%d)\n", ret);
		return ret;
	}

	dm_gpio_set_value(&priv->gpio_dc, 1);
	while (off < pixels) {
		size_t i, n = min(sizeof(priv->wrbuf) / 2, pixels - off);
		u16 *dst = (u16 *)priv->wrbuf;

		for (i = 0; i < n; i++)
			dst[i] = be;

		ret = jd9853_spi_write(priv, priv->wrbuf, n * 2);
		if (ret) {
			printf("jd9853: fill xfer failed at %zu/%zu (%d)\n",
			       off, pixels, ret);
			return ret;
		}
		off += n;
	}

	printf("jd9853: fill done\n");
	return 0;
}

static int jd9853_open(struct jd9853_priv *priv, bool force_reinit)
{
	ofnode panel_node;
	int ret;

	if (priv->ready && !force_reinit) {
		printf("jd9853: already initialized, reuse\n");
		return 0;
	}

	printf("jd9853: open%s\n", force_reinit ? " (force reinit)" : "");
	memset(priv, 0, sizeof(*priv));

	ret = jd9853_find_panel_node(&panel_node);
	if (ret)
		return ret;

	ret = jd9853_gpio_setup(priv, panel_node);
	if (ret) {
		printf("jd9853: gpio setup failed (%d)\n", ret);
		return ret;
	}

	ret = jd9853_spi_setup(priv, panel_node);
	if (ret) {
		printf("jd9853: spi setup failed (%d)\n", ret);
		return ret;
	}

	ret = jd9853_init_panel(priv);
	if (ret) {
		printf("jd9853: panel init failed (%d)\n", ret);
		return ret;
	}

	priv->ready = true;
	printf("jd9853: open complete\n");
	return 0;
}

static void jd9853_init_display_shm(void)
{
	struct display_shm *shm = (struct display_shm *)(uintptr_t)CVIMMAP_DISPLAY_SHM_ADDR;
	const char *owner = env_get("lcd_owner");
	uint8_t owner_id = DISPLAY_OWNER_RTOS;

	if (owner && !strcmp(owner, "linux"))
		owner_id = DISPLAY_OWNER_LINUX;

	memset(shm, 0, sizeof(*shm));
	shm->magic = DISPLAY_SHM_MAGIC;
	shm->version = DISPLAY_SHM_VERSION;
	shm->owner = owner_id;
	shm->bl_on = 1;
	shm->bl_level = 100;
	shm->mirror_x = jd9853_env_mirror("lcd_mirror_x", LCD_MIRROR_X_DEFAULT);
	shm->mirror_y = jd9853_env_mirror("lcd_mirror_y", LCD_MIRROR_Y_DEFAULT);
	flush_dcache_range((unsigned long)shm, sizeof(*shm));
	printf("jd9853: display_shm @ 0x%x owner=%s mirror=%u,%u\n",
	       CVIMMAP_DISPLAY_SHM_ADDR,
	       owner_id == DISPLAY_OWNER_LINUX ? "linux" : "rtos",
	       shm->mirror_x, shm->mirror_y);
}

/*
 * Seed SHM buf[0] with the same host-endian RGB565 logo that was blitted to
 * the panel. RTOS full-init clears GRAM; dirty=1 lets it re-push this frame.
 */
static void jd9853_publish_logo_frame(void)
{
	struct display_shm *shm = (struct display_shm *)(uintptr_t)CVIMMAP_DISPLAY_SHM_ADDR;
	size_t total = jd9853_logo_end - jd9853_logo_data;

	if (total != DISPLAY_FRAME_BYTES) {
		printf("jd9853: skip shm logo seed, size %zu != %u\n",
		       total, (unsigned)DISPLAY_FRAME_BYTES);
		return;
	}

	memcpy(shm->buf[0], jd9853_logo_data, DISPLAY_FRAME_BYTES);
	shm->write_idx = 0;
	shm->dirty = 1;
	shm->frame_seq = 0;
	flush_dcache_range((unsigned long)shm, sizeof(*shm));
	printf("jd9853: logo seeded to display_shm buf[0] dirty=1\n");
}

static int do_jd9853_logo(struct cmd_tbl *cmdtp, int flag, int argc,
			  char *const argv[])
{
	int ret;

	printf("jd9853_logo: start\n");
	ret = jd9853_open(&jd9853, true);
	if (ret) {
		/*
		 * Still publish display_shm so RTOS/Linux ownership handshake
		 * works even if panel SPI init failed this boot.
		 */
		jd9853_init_display_shm();
		printf("jd9853_logo: open failed (%d), shm published\n", ret);
		return CMD_RET_FAILURE;
	}

	ret = jd9853_blit_logo(&jd9853);
	if (ret)
		printf("jd9853_logo: blit failed (%d)\n", ret);

	dm_gpio_set_value(&jd9853.gpio_bl, 1);
	printf("jd9853_logo: backlight on, logical=%d\n",
	       dm_gpio_get_value(&jd9853.gpio_bl));
	jd9853_init_display_shm();
	if (!ret)
		jd9853_publish_logo_frame();
	printf("jd9853_logo: done (%dx%d)\n", JD9853_WIDTH, JD9853_HEIGHT);

	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

static int do_jd9853(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	u16 color = JD9853_RGB565_BLUE;
	int ret, on;

	if (argc < 2)
		return CMD_RET_USAGE;

	if (!strcmp(argv[1], "init")) {
		printf("jd9853: cmd init\n");
		ret = jd9853_open(&jd9853, true);
		if (ret)
			return CMD_RET_FAILURE;
		dm_gpio_set_value(&jd9853.gpio_bl, 1);
		printf("jd9853: panel init done, backlight on (logical=%d)\n",
		       dm_gpio_get_value(&jd9853.gpio_bl));
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "fill")) {
		if (argc >= 3)
			color = (u16)simple_strtoul(argv[2], NULL, 16);

		printf("jd9853: cmd fill 0x%04x\n", color);
		ret = jd9853_open(&jd9853, false);
		if (ret)
			return CMD_RET_FAILURE;

		ret = jd9853_fill_color(&jd9853, color);
		if (ret) {
			printf("jd9853: fill failed (%d)\n", ret);
			return CMD_RET_FAILURE;
		}
		dm_gpio_set_value(&jd9853.gpio_bl, 1);
		printf("jd9853: fill 0x%04x done, backlight on\n", color);
		return CMD_RET_SUCCESS;
	}

	if (!strcmp(argv[1], "logo"))
		return do_jd9853_logo(cmdtp, flag, 1, argv);

	if (!strcmp(argv[1], "bl")) {
		if (argc < 3)
			return CMD_RET_USAGE;
		on = (int)simple_strtol(argv[2], NULL, 10);

		printf("jd9853: cmd bl %d\n", on);
		ret = jd9853_open(&jd9853, false);
		if (ret)
			return CMD_RET_FAILURE;

		dm_gpio_set_value(&jd9853.gpio_bl, on ? 1 : 0);
		printf("jd9853: backlight %s (logical=%d)\n",
		       on ? "on" : "off",
		       dm_gpio_get_value(&jd9853.gpio_bl));
		return CMD_RET_SUCCESS;
	}

	return CMD_RET_USAGE;
}

U_BOOT_CMD(jd9853_logo, 1, 0, do_jd9853_logo,
	   "Show JD9853 SPI boot logo (Zonhor)",
	   "");

U_BOOT_CMD(jd9853, 3, 0, do_jd9853,
	   "JD9853 SPI panel debug helpers (manual)",
	   "init              - reset/init panel, turn backlight on\n"
	   "jd9853 fill [rgb] - fill solid RGB565 color (default 0x001F blue)\n"
	   "jd9853 bl <0|1>   - backlight off/on\n"
	   "jd9853 logo       - same as jd9853_logo\n");
