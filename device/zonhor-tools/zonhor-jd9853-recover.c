/*
 * Live JD9853 panel recovery: unbind fb, full SPI init, rebind fb.
 * Cross-compile for aarch64 and run on board when TE is stuck low.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GPIO_TE  459 /* XGPIOB_11 */
#define GPIO_RST 460 /* XGPIOB_12, active-low */
#define GPIO_DC  468 /* XGPIOB_20 */

static int gpio_export(int gpio)
{
	char path[64];
	int fd, n;

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio);
	if (access(path, F_OK) == 0)
		return 0;
	fd = open("/sys/class/gpio/export", O_WRONLY);
	if (fd < 0)
		return -errno;
	n = dprintf(fd, "%d", gpio);
	close(fd);
	return n < 0 ? -errno : 0;
}

static int gpio_set_dir(int gpio, int out)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;
	if (dprintf(fd, "%s", out ? "out" : "in") < 0) {
		close(fd);
		return -errno;
	}
	close(fd);
	return 0;
}

static int gpio_set(int gpio, int val)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -errno;
	if (dprintf(fd, "%d", !!val) < 0) {
		close(fd);
		return -errno;
	}
	close(fd);
	return 0;
}

static int gpio_get(int gpio)
{
	char path[64], buf[8];
	int fd, n;

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -EIO;
	return buf[0] == '1';
}

static int sysfs_write(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0)
		return -errno;
	if (write(fd, val, strlen(val)) < 0) {
		close(fd);
		return -errno;
	}
	close(fd);
	return 0;
}

static int spi_xfer(int fd, const uint8_t *tx, size_t len)
{
	struct spi_ioc_transfer tr = {
		.tx_buf = (unsigned long)tx,
		.len = len,
		.speed_hz = 10000000,
		.bits_per_word = 8,
	};
	return ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 0 ? -errno : 0;
}

static int write_cmd(int fd, uint8_t cmd)
{
	int ret;

	gpio_set(GPIO_DC, 0);
	ret = spi_xfer(fd, &cmd, 1);
	gpio_set(GPIO_DC, 1);
	return ret;
}

static int write_reg(int fd, uint8_t cmd, const uint8_t *data, size_t n)
{
	int ret = write_cmd(fd, cmd);

	if (ret || !n)
		return ret;
	gpio_set(GPIO_DC, 1);
	return spi_xfer(fd, data, n);
}

#define W0(c) do { if ((ret = write_cmd(fd, c))) goto fail; } while (0)
#define W(c, ...) do { \
	const uint8_t _d[] = { __VA_ARGS__ }; \
	if ((ret = write_reg(fd, c, _d, sizeof(_d)))) goto fail; \
} while (0)

static int panel_full_init(int fd)
{
	int ret;

	gpio_set(GPIO_RST, 0);
	usleep(20000);
	gpio_set(GPIO_RST, 1);
	usleep(120000);

	W0(0x11);
	usleep(120000);

	W(0xDF, 0x98, 0x53);
	W(0xDF, 0x98, 0x53);
	W(0xB2, 0x23);
	W(0xB7, 0x00, 0x47, 0x00, 0x6F);
	W(0xBB, 0x1C, 0x1A, 0x55, 0x73, 0x63, 0xF0);
	W(0xC0, 0x44, 0xA4);
	W(0xC1, 0x16);
	W(0xC3, 0x7D, 0x07, 0x14, 0x06, 0xCF, 0x71, 0x72, 0x77);
	W(0xC4, 0x00, 0x00, 0xA0, 0x79, 0x0B, 0x0A, 0x16, 0x79,
	  0x0B, 0x0A, 0x16, 0x82);
	W(0xC8, 0x3F, 0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28,
	  0x28, 0x26, 0x25, 0x17, 0x12, 0x0D, 0x04, 0x00, 0x3F,
	  0x32, 0x29, 0x29, 0x27, 0x2B, 0x27, 0x28, 0x28, 0x26,
	  0x25, 0x17, 0x12, 0x0D, 0x04, 0x00);
	W(0xD0, 0x04, 0x06, 0x6B, 0x0F, 0x00);
	W(0xD7, 0x00, 0x30);
	W(0xE6, 0x14);

	W(0xDE, 0x01);
	W(0xB7, 0x03, 0x13, 0xEF, 0x35, 0x35);
	W(0xC1, 0x14, 0x15, 0xC0);
	W(0xC2, 0x06, 0x3A);
	W(0xC4, 0x72, 0x12);
	W(0xBE, 0x00);

	W(0xDE, 0x02);
	W(0xE5, 0x00, 0x02, 0x00);
	W(0xE5, 0x01, 0x02, 0x00);

	W(0xDE, 0x00);
	W(0x35, 0x00);
	W(0x3A, 0x05);
	W(0x2A, 0x00, 0x22, 0x00, 0xCD);
	W(0x2B, 0x00, 0x00, 0x01, 0x3F);

	W(0xDE, 0x02);
	W(0xE5, 0x00, 0x02, 0x00);
	W(0xDE, 0x00);

	W0(0x29);
	usleep(20000);

	W(0x36, 0x40);
	W0(0x21);
	return 0;
fail:
	return ret;
}

static int te_high_count(void)
{
	int i, high = 0;

	for (i = 0; i < 100; i++) {
		if (gpio_get(GPIO_TE) > 0)
			high++;
		usleep(1000);
	}
	return high;
}

int main(int argc, char **argv)
{
	int fd = -1, ret, mode = SPI_MODE_0, bits = 8;
	uint32_t speed = 10000000;
	int rebind = 1;

	if (argc > 1 && !strcmp(argv[1], "--no-rebind"))
		rebind = 0;

	printf("jd9853-recover: unbind fb_jd9853\n");
	sysfs_write("/sys/bus/spi/drivers/fb_jd9853/unbind", "spi3.0");
	sysfs_write("/sys/bus/spi/drivers/spidev/unbind", "spi3.0");
	usleep(100000);

	/* OF compatible is jadard,jd9853 — force spidev via driver_override */
	printf("jd9853-recover: bind spidev (driver_override)\n");
	ret = sysfs_write("/sys/bus/spi/devices/spi3.0/driver_override",
			  "spidev");
	if (ret) {
		fprintf(stderr, "driver_override failed: %s\n", strerror(-ret));
		return 1;
	}
	ret = sysfs_write("/sys/bus/spi/drivers/spidev/bind", "spi3.0");
	if (ret) {
		fprintf(stderr, "spidev bind failed: %s\n", strerror(-ret));
		return 1;
	}
	usleep(100000);

	if (gpio_export(GPIO_TE) || gpio_export(GPIO_RST) ||
	    gpio_export(GPIO_DC)) {
		fprintf(stderr, "gpio export failed\n");
		return 1;
	}
	gpio_set_dir(GPIO_TE, 0);
	gpio_set_dir(GPIO_RST, 1);
	gpio_set_dir(GPIO_DC, 1);
	gpio_set(GPIO_RST, 1);
	gpio_set(GPIO_DC, 1);

	printf("jd9853-recover: TE samples before init: %d/100 high\n",
	       te_high_count());

	fd = open("/dev/spidev3.0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/spidev3.0");
		return 1;
	}
	ioctl(fd, SPI_IOC_WR_MODE, &mode);
	ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
	ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

	printf("jd9853-recover: full panel init...\n");
	ret = panel_full_init(fd);
	close(fd);
	if (ret) {
		fprintf(stderr, "panel init failed: %s\n", strerror(-ret));
		return 1;
	}

	ret = te_high_count();
	printf("jd9853-recover: TE samples after init: %d/100 high\n", ret);
	if (ret <= 0) {
		fprintf(stderr, "TE still dead after full init\n");
		return 2;
	}

	sysfs_write("/sys/class/gpio/unexport", "459");
	sysfs_write("/sys/class/gpio/unexport", "460");
	sysfs_write("/sys/class/gpio/unexport", "468");

	sysfs_write("/sys/bus/spi/drivers/spidev/unbind", "spi3.0");
	usleep(50000);

	if (rebind) {
		printf("jd9853-recover: rebind fb_jd9853\n");
		/* clear override so OF match returns to fb_jd9853 */
		sysfs_write("/sys/bus/spi/devices/spi3.0/driver_override",
			    "\n");
		ret = sysfs_write("/sys/bus/spi/drivers/fb_jd9853/bind",
				  "spi3.0");
		if (ret) {
			fprintf(stderr, "fb bind failed: %s\n", strerror(-ret));
			return 1;
		}
	}

	printf("jd9853-recover: done\n");
	return 0;
}
