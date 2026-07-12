#include <debug.h>
#include <delay_timer.h>
#include <mmio.h>
#include <platform_def.h>
#include <stdint.h>

#define CV181X_I2C1_BASE		0x04010000U

#define DW_IC_CON			0x00
#define DW_IC_TAR			0x04
#define DW_IC_DATA_CMD			0x10
#define DW_IC_SS_SCL_HCNT		0x14
#define DW_IC_SS_SCL_LCNT		0x18
#define DW_IC_INTR_MASK			0x30
#define DW_IC_RAW_INTR_STAT		0x34
#define DW_IC_RX_TL			0x38
#define DW_IC_TX_TL			0x3c
#define DW_IC_CLR_INTR			0x40
#define DW_IC_CLR_TX_ABRT		0x54
#define DW_IC_CLR_STOP_DET		0x60
#define DW_IC_ENABLE			0x6c
#define DW_IC_STATUS			0x70
#define DW_IC_TX_ABRT_SOURCE		0x80
#define DW_IC_ENABLE_STATUS		0x9c

#define DW_IC_CON_MASTER		BIT(0)
#define DW_IC_CON_SPEED_STD		BIT(1)
#define DW_IC_CON_RESTART_EN		BIT(5)
#define DW_IC_CON_SLAVE_DISABLE		BIT(6)
#define DW_IC_CMD_READ			BIT(8)
#define DW_IC_CMD_STOP			BIT(9)
#define DW_IC_INTR_STOP_DET		BIT(9)
#define DW_IC_INTR_TX_ABRT		BIT(6)
#define DW_IC_STATUS_TFNF		BIT(1)
#define DW_IC_STATUS_TFE		BIT(2)
#define DW_IC_STATUS_RFNE		BIT(3)
#define DW_IC_STATUS_MASTER_ACTIVITY	BIT(5)
#define DW_IC_ENABLE_EN			BIT(0)

#define AXP2101_I2C_ADDR		0x34
#define AXP2101_PWR_CTRL		0x80
#define AXP2101_DCDC4_V_OUT		0x85
#define AXP2101_DCDC4_1100MV		0x3c
#define AXP2101_PWR_OUT_DCDC4		BIT(3)

#define I2C_TIMEOUT_US			10000

static uint32_t i2c_read(uint32_t reg)
{
	return mmio_read_32(CV181X_I2C1_BASE + reg);
}

static void i2c_write(uint32_t reg, uint32_t value)
{
	mmio_write_32(CV181X_I2C1_BASE + reg, value);
}

static int i2c_wait_bits(uint32_t reg, uint32_t mask, uint32_t value)
{
	unsigned int timeout = I2C_TIMEOUT_US;

	while ((i2c_read(reg) & mask) != value) {
		if (!timeout--)
			return -1;
		udelay(1);
	}

	return 0;
}

static int i2c_enable(int enable)
{
	uint32_t value = enable ? DW_IC_ENABLE_EN : 0;

	i2c_write(DW_IC_ENABLE, value);
	return i2c_wait_bits(DW_IC_ENABLE_STATUS, DW_IC_ENABLE_EN, value);
}

static int i2c_wait_idle(void)
{
	return i2c_wait_bits(DW_IC_STATUS,
			     DW_IC_STATUS_MASTER_ACTIVITY | DW_IC_STATUS_TFE,
			     DW_IC_STATUS_TFE);
}

static int i2c_wait_tx_space(void)
{
	return i2c_wait_bits(DW_IC_STATUS, DW_IC_STATUS_TFNF,
			     DW_IC_STATUS_TFNF);
}

static int i2c_wait_stop(void)
{
	return i2c_wait_bits(DW_IC_RAW_INTR_STAT, DW_IC_INTR_STOP_DET,
			     DW_IC_INTR_STOP_DET);
}

static int i2c_check_abort(void)
{
	uint32_t raw = i2c_read(DW_IC_RAW_INTR_STAT);

	if (!(raw & DW_IC_INTR_TX_ABRT))
		return 0;

	WARN("AXP2101 early: I2C TX_ABRT raw=0x%x src=0x%x\n",
	     raw, i2c_read(DW_IC_TX_ABRT_SOURCE));
	(void)i2c_read(DW_IC_CLR_TX_ABRT);
	return -1;
}

static int i2c_init(void)
{
	if (i2c_enable(0))
		return -1;

	i2c_write(DW_IC_CON, DW_IC_CON_SLAVE_DISABLE |
		  DW_IC_CON_RESTART_EN | DW_IC_CON_SPEED_STD |
		  DW_IC_CON_MASTER);
	i2c_write(DW_IC_SS_SCL_HCNT, 717);
	i2c_write(DW_IC_SS_SCL_LCNT, 774);
	i2c_write(DW_IC_RX_TL, 0);
	i2c_write(DW_IC_TX_TL, 0);
	i2c_write(DW_IC_INTR_MASK, 0);
	i2c_write(DW_IC_TAR, AXP2101_I2C_ADDR);
	(void)i2c_read(DW_IC_CLR_INTR);

	return i2c_enable(1);
}

static int axp2101_write_reg(uint8_t reg, uint8_t value)
{
	if (i2c_wait_idle() || i2c_wait_tx_space())
		return -1;

	i2c_write(DW_IC_DATA_CMD, reg);
	if (i2c_wait_tx_space())
		return -1;
	i2c_write(DW_IC_DATA_CMD, value | DW_IC_CMD_STOP);

	if (i2c_wait_stop()) {
		(void)i2c_check_abort();
		return -1;
	}

	(void)i2c_read(DW_IC_CLR_STOP_DET);

	return i2c_check_abort() || i2c_wait_idle();
}

static int axp2101_read_reg(uint8_t reg, uint8_t *value)
{
	if (i2c_wait_idle() || i2c_wait_tx_space())
		return -1;

	i2c_write(DW_IC_DATA_CMD, reg);
	if (i2c_wait_tx_space())
		return -1;
	i2c_write(DW_IC_DATA_CMD, DW_IC_CMD_READ | DW_IC_CMD_STOP);

	if (i2c_wait_bits(DW_IC_STATUS, DW_IC_STATUS_RFNE,
			  DW_IC_STATUS_RFNE)) {
		(void)i2c_check_abort();
		return -1;
	}

	*value = (uint8_t)i2c_read(DW_IC_DATA_CMD);

	if (i2c_wait_stop()) {
		(void)i2c_check_abort();
		return -1;
	}

	(void)i2c_read(DW_IC_CLR_STOP_DET);

	return i2c_check_abort() || i2c_wait_idle();
}

static void axp2101_i2c1_pinmux(void)
{
	/* VIVO_D4/VIVO_D3 route to IIC1 SDA/SCL on this board. */
	mmio_clrsetbits_32(PINMUX_BASE + 0x14c, 0x7, 0x5);
	mmio_clrsetbits_32(PINMUX_BASE + 0x150, 0x7, 0x5);
}

int axp2101_early_dcdc4_enable(void)
{
	uint8_t pwr_ctrl;

	axp2101_i2c1_pinmux();

	if (i2c_init()) {
		WARN("AXP2101 early: I2C1 init failed\n");
		return -1;
	}

	if (axp2101_write_reg(AXP2101_DCDC4_V_OUT, AXP2101_DCDC4_1100MV)) {
		WARN("AXP2101 early: set DCDC4 voltage failed\n");
		return -1;
	}

	if (axp2101_read_reg(AXP2101_PWR_CTRL, &pwr_ctrl)) {
		WARN("AXP2101 early: read PWR_CTRL failed\n");
		return -1;
	}

	pwr_ctrl |= AXP2101_PWR_OUT_DCDC4;
	if (axp2101_write_reg(AXP2101_PWR_CTRL, pwr_ctrl)) {
		WARN("AXP2101 early: enable DCDC4 failed\n");
		return -1;
	}

	NOTICE("AXP2101 early: DCDC4 enabled at 1.1V\n");
	return 0;
}
