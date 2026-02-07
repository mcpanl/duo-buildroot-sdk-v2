#if 0
int cvi_board_init(void)
{
	// USER_LED
	PINMUX_CONFIG(EMMC_CMD, XGPIOA_23);

	// USER_BUTTON
	PINMUX_CONFIG(AUX0, XGPIOA_30);

	// LCD
	PINMUX_CONFIG(IIC3_SCL, XGPIOA_5);
	PINMUX_CONFIG(EMMC_DAT1, XGPIOA_24);

	// CAMERA
	PINMUX_CONFIG(ADC1, XGPIOB_3);
	PINMUX_CONFIG(PAD_MIPIRX3N, CAM_MCLK0);
	PINMUX_CONFIG(PWR_BUTTON1, IIC4_SDA);
	PINMUX_CONFIG(PWR_WAKEUP0, IIC4_SCL);




    return 0;
}
#endif


__attribute__((optimize("O0")))
// 1.26ms
void suck_loop(uint64_t loop) {
        loop = loop * 50 * 100;
        uint64_t a;
        while (loop > 0) {
                a = loop / (uint64_t)99;
                a = loop / (uint64_t)77;
                a = loop / (uint64_t)55;
                a = loop / (uint64_t)33;
                a = loop / (uint64_t)11;
                a = loop / (uint64_t)999;
                a = loop / (uint64_t)777;
                a = loop / (uint64_t)555;
                a = loop / (uint64_t)333;
                a = loop / (uint64_t)111;
                a = loop / (uint64_t)9999;
                a = loop / (uint64_t)7777;
                a = loop / (uint64_t)5555;
                a = loop / (uint64_t)3333;
                a = loop / (uint64_t)1111;
                a = loop / (uint64_t)99999;
                a = loop / (uint64_t)77777;
                a = loop / (uint64_t)55555;
                a = loop / (uint64_t)33333;
                a = loop / (uint64_t)11111;
                loop--;
        }
}

static inline void user_led_on(void) {
	uint32_t val;

        val = mmio_read_32(0x03020000);
        val |= (1 << 23);
        mmio_write_32(0x03020000, val);
}

static inline void user_led_off(void) {
	uint32_t val;

        val = mmio_read_32(0x03020000);
        val &= ~(1 << 23);
        mmio_write_32(0x03020000, val);
}

static inline void user_led_toggle(void) {
	uint32_t val;

        val = mmio_read_32(0x03020000);
        val ^= (1 << 23);
        mmio_write_32(0x03020000, val);
}

int cvi_board_init(void)
{
#if 1
	uint32_t val;


	// user led
	mmio_write_32(0x0300105C, 0x3); // GPIOA 23 GPIO_MODE
	val = mmio_read_32(0x03020004); // GPIOA DIR
	val |= (1 << 23); // output
	mmio_write_32(0x03020004, val);
	user_led_toggle();

	// lcd reset
	// mmio_write_32(0x030010A4, 0x0); // PWRGPIO 0 GPIO_MODE
	mmio_write_32(0x0300103C,0x3); //SPK_EN XGPIOA 15

	// tp function
	mmio_write_32(0x03001050, 0x03); // GPIOA 22 GPIO_MODE
	mmio_write_32(0x03001058, 0x03); // GPIOA 27 GPIO_MODE 23tpreset
	mmio_write_32(0x05027078, 0x11);// Unlock PWR_GPIO[3]
	mmio_write_32(0x0502707c, 0x11);// Unlock PWR_GPIO[4]

	// wifi power reset
	mmio_write_32(0x0300104C, 0x3); // GPIOA 26
	mmio_write_32(0x03001054, 0x3); // GPIOA 25
	val = mmio_read_32(0x03020004); // GPIOA DIR
	val |= (1 << 26); // output
	val |= (1 << 27); // output
	val |= (1 << 25); // output
	val |= (1 << 15); // output
	mmio_write_32(0x03020004, val);

	val = mmio_read_32(0x03020000); // signal level
	val &= ~(1 << 26); // set level to low
	val &= ~(1 << 27); // set level to low
	val &= ~(1 << 25); // set level to low
	val &= ~(1 << 15); // set level to low
	mmio_write_32(0x03020000, val);

	suck_loop(50);
	user_led_toggle();

	val = mmio_read_32(0x03020000); // signal level
	val |= (1 << 26); // set level to high
	val |= (1 << 25); // set level to high
	val |= (1 << 15); // set level to high
	val |= (1 << 27); // set level to high
	mmio_write_32(0x03020000, val);

	// wifi sdio pinmux
	mmio_write_32(0x030010D0, 0x0); // D3
	mmio_write_32(0x030010D4, 0x0); // D2
	mmio_write_32(0x030010D8, 0x0); // D1
	mmio_write_32(0x030010DC, 0x0); // D0
	mmio_write_32(0x030010E0, 0x0); // CMD
	mmio_write_32(0x030010E4, 0x0); // CLK

	// spi2 pinmux
	// mmio_write_32(0x030010D0, 0x1); // CS
	// mmio_write_32(0x030010DC, 0x1); // MISO
	// mmio_write_32(0x030010E0, 0x1); // MOSI
	// mmio_write_32(0x030010E4, 0x1); // SCK
	// mmio_write_32(0x030010D8, 0x3); // DC
	// mmio_write_32(0x03001038, 0x3); // RESET

	// uart bluetooth
	mmio_write_32(0x03001070, 0x1); // GPIOA 28 UART1 TX
	mmio_write_32(0x03001074, 0x1); // GPIOA 29 UART1 RX
	mmio_write_32(0x03001068, 0x4); // GPIOA 18 UART1 CTS
	mmio_write_32(0x03001064, 0x4); // GPIOA 19 UART1 RTS

	// PWM
	//mmio_write_32(0x03001068, 0x2); // GPIOA 18 PWM 6


	user_led_toggle();
	// lcd backlight
	mmio_write_32(0x030010EC, 0x0); // XGPIOB 0 PWM0_BUCK
									// for licheervnano alpha
	mmio_write_32(0x030010f8, 0x3); // XGPIOB 3 CAM_RESET
	val = mmio_read_32(0x03021000); // signal level
	val |= (1 << 0); // set level to high
	val |= (1 << 3); // set level to high
	mmio_write_32(0x03021000, val);
	val = mmio_read_32(0x03021004); // GPIOB DIR
	val |= (1 << 0); // output
	val |= (1 << 3); // output
	mmio_write_32(0x03021004, val);

	// for licheervnano beta
	//mmio_write_32(0x030010ac, 0x4); // PWRGPIO 2 PWM 10
	mmio_write_32(0x030010ac, 0x0); // PWRGPIO 2 GPIO_MODE

	// camera function
//	mmio_write_32(0x0300116C, 0x5); // RX4N CAM_MCLK0 for alpha
//	mmio_write_32(0x03001170, 0x5); // RX4P CAM_MCLK1 for alpha
									//mmio_write_32(0x0300118C, 0x5); // RX0N CAM_MCLK1 for beta

									// spi1 on mipi csi
	/*
	   mmio_write_32(0x0300116C, 0x7); // spi1 clk   GPIOC2 MIPI_RX4N
	   mmio_write_32(0x03001170, 0x7); // spi1 cs    GPIOC3 MIPI_RX4P
	   mmio_write_32(0x03001174, 0x7); // spi1 miso  GPIOC4 MIPI_RX3N
	   mmio_write_32(0x03001178, 0x7); // spi1 mosi  GPIOC5 MIPI_RX3P
	   */


	//camclk
	mmio_write_32(0x03001174, 0x4); // CAM_MCLK0 GPIOC4 MIPI_RX3N

	// camera/tp i2c
	mmio_write_32(0x03001090, 0x5); // PWR_GPIO6 IIC4_SCL
	mmio_write_32(0x03001098, 0x5); // PWR_GPIO8 IIC4_SDA

	mmio_write_32(0x03001124, 0x2); // 62 IIC1_SCL */
	mmio_write_32(0x03001128, 0x2); // 63 IIC1_SDA */


        /* mmio_write_32(0x030011a4, 0x2);//83
        mmio_write_32(0x030011a8, 0x2);//84
        mmio_write_32(0x030011ac, 0x2);//85
        mmio_write_32(0x030011b0, 0x2);//86
        mmio_write_32(0x030011b4, 0x2);//87
        mmio_write_32(0x030011b8, 0x2);//88 */


        // wait hardware bootup
        suck_loop(50);
	user_led_off();

#endif
        return 0;
}
