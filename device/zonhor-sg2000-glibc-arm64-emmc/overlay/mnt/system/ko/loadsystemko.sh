#!/bin/sh
${CVI_SHOPTS}
#
# Start to insert kernel modules
#
if ! lsmod | grep -q '^cv181x_sys '; then
	insmod /mnt/system/ko/cv181x_sys.ko
fi
if ! lsmod | grep -q '^cv181x_base '; then
	insmod /mnt/system/ko/cv181x_base.ko
fi
if ! lsmod | grep -q '^cv181x_pwm '; then
	insmod /mnt/system/ko/cv181x_pwm.ko
fi

# CAM1 is disabled on this board. Keep CAM_MCLK1/CAM_PD1 pads as
# GPIOA3=low and GPIOA4=high before any camera stack modules can touch them.
if [ -x /usr/sbin/zonhor-cam-gpio-guard ]; then
	/usr/sbin/zonhor-cam-gpio-guard apply
fi

insmod /mnt/system/ko/cv181x_rtos_cmdqu.ko
insmod /mnt/system/ko/cv181x_fast_image.ko
insmod /mnt/system/ko/cvi_mipi_rx.ko
insmod /mnt/system/ko/snsr_i2c.ko
insmod /mnt/system/ko/cv181x_vi.ko
insmod /mnt/system/ko/cv181x_vpss.ko
insmod /mnt/system/ko/cv181x_dwa.ko
insmod /mnt/system/ko/cv181x_vo.ko
insmod /mnt/system/ko/cv181x_mipi_tx.ko
insmod /mnt/system/ko/cv181x_rgn.ko

if [ -x /usr/sbin/zonhor-cam-gpio-guard ]; then
	/usr/sbin/zonhor-cam-gpio-guard apply
fi

#insmod /mnt/system/ko/cv181x_wdt.ko
insmod /mnt/system/ko/cv181x_clock_cooling.ko

insmod /mnt/system/ko/cv181x_tpu.ko
insmod /mnt/system/ko/cv181x_vcodec.ko
insmod /mnt/system/ko/cv181x_jpeg.ko
insmod /mnt/system/ko/cvi_vc_driver.ko MaxVencChnNum=9 MaxVdecChnNum=9
if ! lsmod | grep -q '^cv181x_rtc '; then
	insmod /mnt/system/ko/cv181x_rtc.ko
fi
insmod /mnt/system/ko/cv181x_ive.ko

#insmod /mnt/system/ko/3rd/gt9xx.ko

echo 3 > /proc/sys/vm/drop_caches
dmesg -n 4

#usb hub control
#/etc/uhubon.sh host

exit $?
