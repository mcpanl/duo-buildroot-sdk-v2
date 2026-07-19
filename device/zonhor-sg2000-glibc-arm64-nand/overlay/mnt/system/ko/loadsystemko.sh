#!/bin/sh
${CVI_SHOPTS}
#
# Start to insert kernel modules (idempotent).
#
KO_DIR=/mnt/system/ko
. "$KO_DIR/ko-common.sh"

safe_insmod "$KO_DIR/cv181x_sys.ko"
safe_insmod "$KO_DIR/cv181x_base.ko"
safe_insmod "$KO_DIR/cv181x_pwm.ko"

# CAM1 is disabled on this board. Keep CAM_MCLK1/CAM_PD1 pads as
# GPIOA3=low and GPIOA4=high before any camera stack modules can touch them.
if [ -x /usr/sbin/zonhor-cam-gpio-guard ]; then
	/usr/sbin/zonhor-cam-gpio-guard apply
fi

safe_insmod "$KO_DIR/cv181x_rtos_cmdqu.ko"
safe_insmod "$KO_DIR/cv181x_fast_image.ko"
# LCD backlight soft-PWM (GPIOA20). Load before proxy so fb blank can find it.
if [ -f "$KO_DIR/cv181x_zonhor_lcd_bl.ko" ]; then
	if [ -d /sys/firmware/devicetree/base/lcd-bl ]; then
		safe_insmod "$KO_DIR/cv181x_zonhor_lcd_bl.ko" || true
	else
		safe_insmod "$KO_DIR/cv181x_zonhor_lcd_bl.ko" gpio=500 brightness=60 || true
	fi
fi
# LCD proxy: /dev/fb0 when cvi.lcd_owner=rtos (default). Harmless no-op if module
# refuses probe under cvi.lcd_owner=linux.
if [ -f "$KO_DIR/cv181x_zonhor_lcd_proxy.ko" ]; then
	safe_insmod "$KO_DIR/cv181x_zonhor_lcd_proxy.ko" || true
fi
safe_insmod "$KO_DIR/cvi_mipi_rx.ko"
safe_insmod "$KO_DIR/snsr_i2c.ko"
safe_insmod "$KO_DIR/cv181x_vi.ko"
safe_insmod "$KO_DIR/cv181x_vpss.ko"
safe_insmod "$KO_DIR/cv181x_dwa.ko"
safe_insmod "$KO_DIR/cv181x_vo.ko"
safe_insmod "$KO_DIR/cv181x_mipi_tx.ko"
safe_insmod "$KO_DIR/cv181x_rgn.ko"

if [ -x /usr/sbin/zonhor-cam-gpio-guard ]; then
	/usr/sbin/zonhor-cam-gpio-guard apply
fi

#insmod /mnt/system/ko/cv181x_wdt.ko
safe_insmod "$KO_DIR/cv181x_clock_cooling.ko"

safe_insmod "$KO_DIR/cv181x_tpu.ko"
safe_insmod "$KO_DIR/cv181x_vcodec.ko"
safe_insmod "$KO_DIR/cv181x_jpeg.ko"
safe_insmod "$KO_DIR/cvi_vc_driver.ko" MaxVencChnNum=9 MaxVdecChnNum=9
safe_insmod "$KO_DIR/cv181x_rtc.ko"
safe_insmod "$KO_DIR/cv181x_ive.ko"

#insmod /mnt/system/ko/3rd/gt9xx.ko

echo 3 > /proc/sys/vm/drop_caches
dmesg -n 4

#usb hub control
#/etc/uhubon.sh host

exit 0
