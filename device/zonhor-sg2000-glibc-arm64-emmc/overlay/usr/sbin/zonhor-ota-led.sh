#!/bin/sh
#
# SYS_LED (sys-led) status indicator for OTA.
#
# Modes (blink frequency):
#   staged   - 1 Hz  : upgrade staged, waiting for reboot
#   fip      - 2 Hz  : flashing bootloader / enabling OTA partition
#   phase1   - 4 Hz  : phase-1 verify and BOOT/MISC flash
#   recovery - 8 Hz  : phase-2 ROOTFS flash (do not power off)
#   error    - 10 Hz : failure indication
#
# Usage:
#   zonhor-ota-led.sh start <mode>
#   zonhor-ota-led.sh stop
#   zonhor-ota-led.sh success
#   zonhor-ota-led.sh error
#

LED_NAME="${ZONHOR_USER_LED:-sys-led}"
LED_SYSFS="/sys/class/leds/$LED_NAME"
PID_FILE="${ZONHOR_OTA_LED_PID:-/var/run/zonhor-ota-led.pid}"

led_sleep_ms() {
	ms="$1"
	if command -v usleep >/dev/null 2>&1; then
		usleep $((ms * 1000)) 2>/dev/null
	else
		sleep $(( (ms + 999) / 1000 ))
	fi
}

led_init() {
	[ -d "$LED_SYSFS" ] || return 1
	echo none >"$LED_SYSFS/trigger" 2>/dev/null
	return 0
}

led_set() {
	[ -d "$LED_SYSFS" ] || return 1
	echo "$1" >"$LED_SYSFS/brightness" 2>/dev/null
}

led_stop_bg() {
	if [ -f "$PID_FILE" ]; then
		pid=$(cat "$PID_FILE" 2>/dev/null)
		[ -n "$pid" ] && kill "$pid" 2>/dev/null
		rm -f "$PID_FILE"
	fi
	led_set 0
}

led_blink_loop() {
	mode="$1"
	on_ms=500
	off_ms=500

	case "$mode" in
	staged) on_ms=500; off_ms=500 ;;
	fip) on_ms=250; off_ms=250 ;;
	phase1) on_ms=125; off_ms=125 ;;
	recovery) on_ms=62; off_ms=62 ;;
	error) on_ms=50; off_ms=50 ;;
	*) on_ms=500; off_ms=500 ;;
	esac

	led_init || exit 0
	while true; do
		led_set 1
		led_sleep_ms "$on_ms"
		led_set 0
		led_sleep_ms "$off_ms"
	done
}

led_start() {
	mode="$1"
	led_stop_bg
	led_blink_loop "$mode" &
	echo $! >"$PID_FILE"
}

led_success() {
	led_stop_bg
	led_init || return 0
	led_set 1
	led_sleep_ms 3000
	led_set 0
}

led_error() {
	led_start error
	led_sleep_ms 10000
	led_stop_bg
}

case "$1" in
start)
	[ -n "$2" ] || exit 1
	led_start "$2"
	;;
stop)
	led_stop_bg
	;;
success)
	led_success
	;;
error)
	led_error
	;;
*)
	echo "Usage: $0 start <staged|fip|phase1|recovery|error>"
	echo "       $0 stop|success|error"
	exit 1
	;;
esac

exit 0
