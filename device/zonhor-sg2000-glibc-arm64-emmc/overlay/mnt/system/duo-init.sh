#!/bin/sh
#
# Board-specific late init (runs from S99user after MPP modules).
# WiFi/BT is handled earlier by S35zonhor-wifi-bt to avoid PMIC brownout with MPP.
#

# Insmod PWM Module
if ! lsmod | grep -q '^cv181x_pwm '; then
	insmod /mnt/system/ko/cv181x_pwm.ko
fi

# LCD backlight on JTAG_CPU_TRST / GPIOA20 is active-high. Enable pad pull-down
# (and clear pull-up) so mem deep-sleep cannot float the pin high and light the
# panel. bl_power sysfs is NOT inverted: 0=on (FB_BLANK_UNBLANK), 1=off.
if [ -x /usr/sbin/zonhor-lcd-bl-pad ]; then
	/usr/sbin/zonhor-lcd-bl-pad pulldown >/tmp/lcd-bl-pad.log 2>&1 || true
fi

# Shallow/deep sleep helper:
#   zonhor-wifi-bt-suspend freeze
#   zonhor-wifi-bt-suspend mem
