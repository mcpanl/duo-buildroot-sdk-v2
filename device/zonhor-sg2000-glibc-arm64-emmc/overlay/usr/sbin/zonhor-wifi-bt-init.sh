#!/bin/sh
# Load AIC8800 WiFi/BT stack with safe power sequencing.
#
# Usage:
#   zonhor-wifi-bt-init.sh boot     # early boot: modules + MAC, wlan0 stays down
#   zonhor-wifi-bt-init.sh resume   # after mem: same module path, BT UART attach
#
# Design goals:
#   - Avoid RF + BT + MPP peak current at the same instant (PMIC brownout reset).
#   - Keep wlan0 down until userspace explicitly needs it (dhcpcd / ifconfig).
#   - Do not enable bluesleep LPM (BlueZ does not drive btwrite).
#   - Idempotent: safe if modules are already loaded.

MODE=${1:-boot}
KO_DIR=/mnt/system/ko
LOG=/tmp/zonhor-wifi-bt-init.log
BT_TTY=/dev/ttyS4
BT_BAUD=1500000
BSP_DELAY=1
FDRV_DELAY=1

mod_loaded() {
	lsmod | grep -q "^$1 "
}

log() {
	echo "$*" | tee -a "${LOG}"
}

insmod_once() {
	mod_name=$1
	mod_path=$2

	mod_loaded "${mod_name}" && return 0
	[ -f "${mod_path}" ] || {
		log "missing ${mod_path}"
		return 1
	}
	insmod "${mod_path}" >>"${LOG}" 2>&1
}

sideband_prepare() {
	if [ -x /usr/sbin/zonhor-sideband-gpio.sh ]; then
		/usr/sbin/zonhor-sideband-gpio.sh >>"${LOG}" 2>&1 || true
	fi
}

load_modules() {
	sideband_prepare

	insmod_once aic8800_bsp "${KO_DIR}/aic8800_bsp.ko" || return 1
	sleep "${BSP_DELAY}"

	insmod_once aic8800_fdrv "${KO_DIR}/aic8800_fdrv.ko" || return 1
	sleep "${FDRV_DELAY}"

	insmod_once aic8800_btlpm "${KO_DIR}/aic8800_btlpm.ko" || true
	return 0
}

set_wlan_mac() {
	if [ ! -x /usr/sbin/zonhor-mac-from-uid ]; then
		return 0
	fi
	/usr/sbin/zonhor-mac-from-uid --no-up wlan0 >>"${LOG}" 2>&1 || \
		log "wlan0 MAC setup failed (non-fatal)"
}

attach_bt_uart() {
	if [ ! -c "${BT_TTY}" ] || ! command -v hciattach >/dev/null 2>&1; then
		return 0
	fi

	i=0
	while [ "$i" -lt 30 ]; do
		[ -e /proc/bluetooth/sleep/lpm ] && break
		sleep 0.1
		i=$((i + 1))
	done

	killall hciattach 2>/dev/null || true
	sleep 0.2

	hciattach "${BT_TTY}" any ${BT_BAUD} flow >>/tmp/hciattach.log 2>&1 || {
		log "hciattach ${BT_TTY} failed (see /tmp/hciattach.log)"
		return 1
	}

	# Leave hci0 down; bluetoothd (S40) brings it up when ready.
	# Bringing BT RF up here overlaps with WiFi SDIO probe and caused PMIC resets.
	if command -v hciconfig >/dev/null 2>&1; then
		hciconfig hci0 down >>/tmp/hciattach.log 2>&1 || true
	fi
	return 0
}

case "${MODE}" in
boot|resume)
	: > "${LOG}"
	log "zonhor-wifi-bt-init: mode=${MODE}"

	if ! load_modules; then
		log "module load failed"
		exit 1
	fi

	set_wlan_mac
	attach_bt_uart

	log "zonhor-wifi-bt-init: done"
	exit 0
	;;
*)
	echo "Usage: $0 {boot|resume}" >&2
	exit 1
	;;
esac
