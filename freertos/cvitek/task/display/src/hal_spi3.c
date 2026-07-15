/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal DesignWare APB SSI master driver for SPI3 @ 0x041B0000.
 * TX-only path used by JD9853 panel refresh.
 */
#include <stdio.h>
#include "mmio.h"
#include "hal_pinmux.h"
#include "cv181x_pinmux.h"
#include "hal_spi3.h"

#define SPI3_BASE		0x041B0000u

#define DW_SPI_CTRLR0		0x00
#define DW_SPI_SSIENR		0x08
#define DW_SPI_SER		0x10
#define DW_SPI_BAUDR		0x14
#define DW_SPI_TXFTLR		0x18
#define DW_SPI_SR		0x28
#define DW_SPI_IMR		0x2c
#define DW_SPI_DR		0x60

#define SR_BUSY			(1u << 0)
#define SR_TF_NOT_FULL		(1u << 1)
#define SR_TF_EMPT		(1u << 2)

#define CV181X_CLK_BASE		0x03002000u
#define CV181X_REG_CLK_EN_1	0x004
#define CV181X_REG_CLK_EN_3	0x00C
#define CV181X_REG_DIV_CLK_SPI	0x100
#define CV181X_SPI_PARENT_HZ	1000000000UL

static uint32_t spi3_clk_hz;

static inline uint32_t spi3_read(uint32_t off)
{
	return mmio_read_32(SPI3_BASE + off);
}

static inline void spi3_write(uint32_t off, uint32_t val)
{
	mmio_write_32(SPI3_BASE + off, val);
}

static void spi3_enable_clock(void)
{
	uint32_t en, div_reg, div;

	en = mmio_read_32(CV181X_CLK_BASE + CV181X_REG_CLK_EN_1);
	mmio_write_32(CV181X_CLK_BASE + CV181X_REG_CLK_EN_1, en | (1u << 12));
	en = mmio_read_32(CV181X_CLK_BASE + CV181X_REG_CLK_EN_3);
	mmio_write_32(CV181X_CLK_BASE + CV181X_REG_CLK_EN_3, en | (1u << 6));

	div_reg = mmio_read_32(CV181X_CLK_BASE + CV181X_REG_DIV_CLK_SPI);
	div = (div_reg >> 16) & 0x3f;
	if (div < 1)
		div = 8;
	spi3_clk_hz = CV181X_SPI_PARENT_HZ / div;
}

static void spi3_wait_idle(void)
{
	unsigned int guard = 1000000;

	while ((spi3_read(DW_SPI_SR) & SR_BUSY) && guard--)
		;
}

int hal_spi3_init(uint32_t hz)
{
	uint32_t baud, cr0;

	hal_pinmux_config(PINMUX_SPI3);
	spi3_enable_clock();

	if (!spi3_clk_hz)
		spi3_clk_hz = 125000000;

	spi3_write(DW_SPI_SSIENR, 0);
	spi3_write(DW_SPI_IMR, 0);

	/* DFS=7 (8-bit), FRF=SPI, MODE0, TMOD=TO (tx only) */
	cr0 = (7u << 0) | (0u << 4) | (0u << 6) | (1u << 8);
	spi3_write(DW_SPI_CTRLR0, cr0);

	baud = (spi3_clk_hz + hz - 1) / hz;
	if (baud < 2)
		baud = 2;
	if (baud & 1)
		baud++;
	spi3_write(DW_SPI_BAUDR, baud);
	spi3_write(DW_SPI_SER, 1); /* CS0 */
	spi3_write(DW_SPI_TXFTLR, 0);
	spi3_write(DW_SPI_SSIENR, 1);

	printf("spi3: clk=%u Hz baud=%u target=%u Hz\n",
	       (unsigned)spi3_clk_hz, (unsigned)baud, (unsigned)hz);
	return 0;
}

int hal_spi3_xfer(const void *tx, size_t len)
{
	const uint8_t *p = tx;
	size_t i = 0;

	if (!tx || !len)
		return 0;

	spi3_wait_idle();
	while (i < len) {
		while (!(spi3_read(DW_SPI_SR) & SR_TF_NOT_FULL))
			;
		spi3_write(DW_SPI_DR, p[i++]);
	}
	spi3_wait_idle();
	while (!(spi3_read(DW_SPI_SR) & SR_TF_EMPT))
		;
	return 0;
}
