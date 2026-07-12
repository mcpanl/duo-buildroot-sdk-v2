#!/bin/sh

function set_gpio()
{
	local gpio_num=$1
	local gpio_val=$2
	local gpio_path="/sys/class/gpio/gpio${gpio_num}"

	if test -d ${gpio_path}; then
		echo "GPIO ${gpio_num} already exported" >> /tmp/gpio.log 2>&1
	else
		echo ${gpio_num} > /sys/class/gpio/export
	fi

	echo out > ${gpio_path}/direction
	sleep 0.1
	echo ${gpio_val} > ${gpio_path}/value
}

# Hardware V1.1 board sideband
gpio_b17=465
set_gpio ${gpio_b17} 0

# WIFI/BT Module (wake GPIOs owned by drivers via DT)
insmod /mnt/system/ko/aic8800_bsp.ko
sleep 0.5
insmod /mnt/system/ko/aic8800_fdrv.ko
sleep 0.5
insmod /mnt/system/ko/aic8800_btlpm.ko

# Deterministic wlan0 MAC from chip UID (distinct from eth0)
if [ -x /usr/sbin/zonhor-mac-from-uid ]; then
	/usr/sbin/zonhor-mac-from-uid wlan0
fi

# BT HCI over UART4 (U-Boot pinmux: UART2 pads -> UART4)
BT_TTY=/dev/ttyS4
BT_BAUD=1500000
if [ -c "${BT_TTY}" ] && command -v hciattach >/dev/null 2>&1; then
	# Wait briefly for btlpm /proc nodes
	i=0
	while [ $i -lt 20 ]; do
		[ -e /proc/bluetooth/sleep/lpm ] && break
		sleep 0.1
		i=$((i + 1))
	done

	hciattach "${BT_TTY}" any ${BT_BAUD} flow >/tmp/hciattach.log 2>&1 || \
		echo "hciattach ${BT_TTY} failed" >> /tmp/hciattach.log
	sleep 0.5
	if command -v hciconfig >/dev/null 2>&1; then
		hciconfig hci0 up >/tmp/hciattach.log 2>&1 || true
	fi

	# Do NOT enable /proc/bluetooth/sleep/lpm here.
	# AIC bluesleep is BlueDroid-oriented (needs btwrite 1/0 around each HCI TX).
	# BlueZ never drives btwrite, so enabling LPM leaves BT_WAKE asserted,
	# holds the bluesleep wakelock, and spams bluesleep_rx_timer_expire —
	# which prevents a stable freeze. GPIOs stay owned by aic8800_btlpm;
	# zonhor-wifi-bt-suspend manages LPM (freeze) and full reload (mem).
fi

# Sleep helper: /usr/sbin/zonhor-wifi-bt-suspend [freeze|mem]
# mem tears down / reloads AIC8800 WiFi+BT; freeze only uses LP + clears LPM.

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
