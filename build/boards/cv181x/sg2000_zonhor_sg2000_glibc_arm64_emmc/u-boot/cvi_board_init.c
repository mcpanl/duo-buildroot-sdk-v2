static void set_rtc_register_for_power(void)
{
	printf("set_rtc_register_for_power\n");

	/* Reset key */
	mmio_write_32(0x050260D0, 0x7);
}

/*
 * U-Boot has no Cvitek CLK driver (CONFIG_CLK is off). DesignWare SPI probe
 * calls this weak hook for the SSI input clock. Default CV181x clk_spi is
 * FPLL(1GHz)/8 = 125MHz; also ungates APB SPI3 + clk_spi.
 */
#define CV181X_CLK_BASE		0x03002000
#define CV181X_REG_CLK_EN_1	0x004
#define CV181X_REG_CLK_EN_3	0x00C
#define CV181X_REG_DIV_CLK_SPI	0x100
#define CV181X_SPI_PARENT_HZ	1000000000UL

#define CV181X_GPIOA_BASE	0x03020000
#define CV181X_GPIO_SWPORTA_DR	0x000
#define CV181X_GPIO_SWPORTA_DDR	0x004
#define GPIOA3			BIT(3)
#define GPIOA4			BIT(4)

int dw_spi_get_clk(struct udevice *bus, ulong *rate)
{
	u32 en, div_reg, div;

	en = mmio_read_32(CV181X_CLK_BASE + CV181X_REG_CLK_EN_1);
	mmio_write_32(CV181X_CLK_BASE + CV181X_REG_CLK_EN_1, en | BIT(12));
	en = mmio_read_32(CV181X_CLK_BASE + CV181X_REG_CLK_EN_3);
	mmio_write_32(CV181X_CLK_BASE + CV181X_REG_CLK_EN_3, en | BIT(6));

	div_reg = mmio_read_32(CV181X_CLK_BASE + CV181X_REG_DIV_CLK_SPI);
	div = (div_reg >> 16) & 0x3f;
	if (div < 1)
		div = 8;

	*rate = CV181X_SPI_PARENT_HZ / div;
	printf("jd9853/clk: en1=0x%x en3=0x%x div_reg=0x%x div=%u rate=%lu Hz\n",
	       mmio_read_32(CV181X_CLK_BASE + CV181X_REG_CLK_EN_1),
	       mmio_read_32(CV181X_CLK_BASE + CV181X_REG_CLK_EN_3),
	       div_reg, div, *rate);
	return 0;
}

static void zonhor_config_cam_gpio_fixed(void)
{
	/*
	 * CAM1 pads are board sideband GPIOs:
	 *   CAM_MCLK1 -> GPIOA3, fixed output low
	 *   CAM_PD1   -> GPIOA4, fixed output high
	 * Set data before direction to avoid a visible high pulse on A3.
	 */
	PINMUX_CONFIG(CAM_MCLK1, XGPIOA_3);
	PINMUX_CONFIG(CAM_PD1, XGPIOA_4);
	mmio_clrsetbits_32(CV181X_GPIOA_BASE + CV181X_GPIO_SWPORTA_DR,
			    GPIOA3 | GPIOA4, GPIOA4);
	mmio_clrsetbits_32(CV181X_GPIOA_BASE + CV181X_GPIO_SWPORTA_DDR,
			    GPIOA3 | GPIOA4, GPIOA3 | GPIOA4);
}

int cvi_board_init(void)
{
	/* Camera0 */
	PINMUX_CONFIG(IIC3_SCL, IIC3_SCL);
	PINMUX_CONFIG(IIC3_SDA, IIC3_SDA);
	PINMUX_CONFIG(CAM_MCLK0, CAM_MCLK0);
	PINMUX_CONFIG(CAM_RST0, XGPIOA_2);

	/* Camera1 disabled; keep its sideband pads as fixed GPIO outputs. */
	zonhor_config_cam_gpio_fixed();

	/* User/Sys LEDs */
	PINMUX_CONFIG(JTAG_CPU_TCK, XGPIOA_18); /* USER_LED */
	PINMUX_CONFIG(IIC0_SDA, XGPIOA_29);     /* SYS_LED */

	/* USER_BUTTON and PMIC IRQ */
	PINMUX_CONFIG(USB_ID, XGPIOB_4);        /* USER_BUTTON */
	/*
	 * AXP2101 INT# on PAD_SD0_PWR_EN / XGPIOA_14 (open-drain, active-low):
	 * pinmux GPIO, weak pull-up (PU=bit2, PD=bit3), direction input.
	 * Matches: 0x03001038=0x3, 0x03001904 PU, 0x03020004 bit14 clear.
	 */
	PINMUX_CONFIG(SD0_PWR_EN, XGPIOA_14);   /* PMIC_IRQ */
	mmio_clrsetbits_32(PINMUX_BASE + 0x904, BIT(3) | BIT(2), BIT(2));
	mmio_clrbits_32(0x03020004, BIT(14));
	PINMUX_CONFIG(JTAG_CPU_TMS, XGPIOA_19); /* WUSB3801Q INT */
	PINMUX_CONFIG(AUX0, XGPIOA_30);         /* WUSB3801Q ID */
	PINMUX_CONFIG(VIVO_CLK, XGPIOB_22);     /* ICM-42688 INT */

	/* I2C1 devices: AXP2101/ICM-42688/Touch/WUSB3801Q */
	PINMUX_CONFIG(VIVO_D4, IIC1_SDA);
	PINMUX_CONFIG(VIVO_D3, IIC1_SCL);

	/* SPI3 */
	PINMUX_CONFIG(VIVO_D8, SPI3_SDO);
	PINMUX_CONFIG(VIVO_D7, SPI3_SDI);
	PINMUX_CONFIG(VIVO_D6, SPI3_SCK);
	PINMUX_CONFIG(VIVO_D5, SPI3_CS_X);

	/* PWM0 buzzer */
	PINMUX_CONFIG(PWM0_BUCK, PWM_0);

	/* Analog MIC inputs (PAD_AUD_AINL_MIC / PAD_AUD_AINR_MIC, func 0) */
	mmio_write_32(PINMUX_BASE + FMUX_GPIO_FUNCSEL_PAD_AUD_AINL_MIC, 0);
	mmio_write_32(PINMUX_BASE + FMUX_GPIO_FUNCSEL_PAD_AUD_AINR_MIC, 0);

	/* Screen/touch reserved GPIOs */
	PINMUX_CONFIG(JTAG_CPU_TRST, XGPIOA_20); /* LCD backlight */
	/*
	 * Active-high backlight on PAD_JTAG_CPU_TRST / XGPIOA_20.
	 * Pad conf @ 0x03001938: PU=bit2, PD=bit3. Force pull-down so that
	 * when mem (S2RAM) drops the GPIO bank, the pin does not float high
	 * (JTAG TRST default pull-up) and re-light the panel. freeze keeps
	 * GPIO drive so this only matters for deep sleep.
	 */
	mmio_clrsetbits_32(PINMUX_BASE + 0x938, BIT(3) | BIT(2), BIT(3));
	PINMUX_CONFIG(IIC0_SCL, XGPIOA_28);      /* Touch IRQ */
	PINMUX_CONFIG(VIVO_D1, XGPIOB_20);       /* LCD RS */
	PINMUX_CONFIG(VIVO_D0, XGPIOB_21);       /* Touch reset */
	PINMUX_CONFIG(VIVO_D10, XGPIOB_11);      /* LCD TE */
	PINMUX_CONFIG(VIVO_D9, XGPIOB_12);       /* LCD reset */

	/* USB */
	PINMUX_CONFIG(USB_VBUS_EN, XGPIOB_5);

	/* WIFI/BT wake + HCI UART4 */
	PINMUX_CONFIG(PWR_SEQ1, PWR_GPIO_3);   /* HOST_WAKE_WF */
	PINMUX_CONFIG(PWR_SEQ3, PWR_GPIO_5);   /* WIFI_WAKE_HOST */
	PINMUX_CONFIG(CLK32K, PWR_GPIO_10);    /* HOST_WAKE_BT */
	PINMUX_CONFIG(CLK25M, PWR_GPIO_11);    /* BT_WAKE_HOST */
	PINMUX_CONFIG(UART2_RX, UART4_RX);
	PINMUX_CONFIG(UART2_TX, UART4_TX);
	PINMUX_CONFIG(UART2_CTS, UART4_CTS);
	PINMUX_CONFIG(UART2_RTS, UART4_RTS);

	/* EPHY LEDs */
	PINMUX_CONFIG(PWR_WAKEUP0, EPHY_LNK_LED);
	PINMUX_CONFIG(PWR_BUTTON1, EPHY_SPD_LED);

	set_rtc_register_for_power();

	return 0;
}
