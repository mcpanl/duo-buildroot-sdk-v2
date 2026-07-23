#!/bin/sh
#
# Manual WiFi / BT low-power control for zonhor SG2000 + AIC8800.
# Does NOT auto-sleep and does NOT cut module total power (GPIOA15).
#
# Usage:
#   wifi-bt-lpm.sh status
#   wifi-bt-lpm.sh wifi-sleep | wifi-wake
#   wifi-bt-lpm.sh bt-lpm-on | bt-lpm-off
#   wifi-bt-lpm.sh bt-allow-sleep | bt-wake
#   wifi-bt-lpm.sh prepare-mem
#   wifi-bt-lpm.sh rtc-info
#

set -eu

WIFI_SUSPEND=/proc/wifi_suspend/suspend
WIFI_STATUS=/proc/wifi_suspend/status
BT_LPM=/proc/bluetooth/sleep/lpm
BT_WRITE=/proc/bluetooth/sleep/btwrite
BT_WAKE=/proc/bluetooth/sleep/btwake
BT_HOSTWAKE=/proc/bluetooth/sleep/hostwake
BT_STATUS=/proc/bluetooth/sleep/status

die() {
	echo "wifi-bt-lpm: $*" >&2
	exit 1
}

need() {
	[ -e "$1" ] || die "missing $1 (is aic8800_* loaded with LPM enabled?)"
}

cmd_status() {
	echo "=== modules ==="
	lsmod 2>/dev/null | grep -E 'aic8800|cvi_wifi' || true
	echo
	echo "=== wifi ==="
	if [ -e "$WIFI_STATUS" ]; then
		cat "$WIFI_STATUS"
	else
		echo "no $WIFI_STATUS"
	fi
	echo
	echo "=== bluetooth ==="
	if [ -e "$BT_STATUS" ]; then
		cat "$BT_STATUS"
	else
		echo "no $BT_STATUS"
	fi
	if [ -e "$BT_LPM" ]; then
		echo -n "lpm proc: "
		cat "$BT_LPM"
	fi
	if [ -e "$BT_WAKE" ]; then
		echo -n "btwake: "
		cat "$BT_WAKE"
	fi
	if [ -e "$BT_HOSTWAKE" ]; then
		echo -n "hostwake: "
		cat "$BT_HOSTWAKE"
	fi
	echo
	echo "=== net ==="
	ip -br link show wlan0 2>/dev/null || true
	hciconfig 2>/dev/null || true
}

cmd_wifi_sleep() {
	need "$WIFI_SUSPEND"
	echo 1 > "$WIFI_SUSPEND"
	echo "wifi-bt-lpm: wifi sleep requested"
	[ -e "$WIFI_STATUS" ] && cat "$WIFI_STATUS" || true
}

cmd_wifi_wake() {
	need "$WIFI_SUSPEND"
	echo 0 > "$WIFI_SUSPEND"
	echo "wifi-bt-lpm: wifi wake requested"
	[ -e "$WIFI_STATUS" ] && cat "$WIFI_STATUS" || true
}

cmd_bt_lpm_on() {
	need "$BT_LPM"
	echo 1 > "$BT_LPM"
	echo "wifi-bt-lpm: BT LPM enabled"
	[ -e "$BT_STATUS" ] && cat "$BT_STATUS" || true
}

cmd_bt_lpm_off() {
	need "$BT_LPM"
	echo 0 > "$BT_LPM"
	echo "wifi-bt-lpm: BT LPM disabled"
	[ -e "$BT_STATUS" ] && cat "$BT_STATUS" || true
}

cmd_bt_allow_sleep() {
	need "$BT_WRITE"
	# Deassert TX busy so bluesleep may drop HOST_WAKE_BT
	echo 0 > "$BT_WRITE"
	if [ -e "$BT_WAKE" ]; then
		echo 0 > "$BT_WAKE"
	fi
	echo "wifi-bt-lpm: BT allow-sleep (btwrite=0, btwake=0)"
	[ -e "$BT_STATUS" ] && cat "$BT_STATUS" || true
}

cmd_bt_wake() {
	need "$BT_WAKE"
	echo 1 > "$BT_WAKE"
	if [ -e "$BT_WRITE" ]; then
		echo 1 > "$BT_WRITE"
	fi
	echo "wifi-bt-lpm: BT wake asserted"
	[ -e "$BT_STATUS" ] && cat "$BT_STATUS" || true
}

cmd_prepare_mem() {
	echo "wifi-bt-lpm: prepare-mem (manual; does NOT echo mem)"
	if [ -e "$WIFI_SUSPEND" ]; then
		echo 1 > "$WIFI_SUSPEND" || true
	fi
	if [ -e "$BT_LPM" ]; then
		echo 1 > "$BT_LPM" || true
	fi
	if [ -e "$BT_WRITE" ]; then
		echo 0 > "$BT_WRITE" || true
	fi
	if [ -e "$BT_WAKE" ]; then
		echo 0 > "$BT_WAKE" || true
	fi
	echo "wifi-bt-lpm: WiFi sleep + BT LPM allow-sleep armed"
	echo "wifi-bt-lpm: next (from serial console): echo mem > /sys/power/state"
	cmd_status
}

cmd_rtc_info() {
	# SoC RTC mailbox / power detect (needs /dev/mem + busybox devmem or Python)
	echo "=== RTC_INFO / power detect (via /dev/mem) ==="
	if [ ! -e /dev/mem ]; then
		die "/dev/mem missing"
	fi
	if command -v devmem >/dev/null 2>&1; then
		printf "RTC_INFO0 (0x0502601C): 0x%08x\n" "$(devmem 0x0502601C 32)"
		printf "RTC_INFO1 (0x05026020): 0x%08x\n" "$(devmem 0x05026020 32)"
		printf "RTC_INFO2 (0x05026024): 0x%08x\n" "$(devmem 0x05026024 32)"
		printf "RTC_INFO3 (0x05026028): 0x%08x\n" "$(devmem 0x05026028 32)"
		printf "RTC_EN_PWR_VBAT_DET (0x050260D0): 0x%08x\n" "$(devmem 0x050260D0 32)"
		sleep 1
		info2_b=$(devmem 0x05026024 32)
		printf "RTC_INFO2 after 1s: 0x%08x (should advance if MCU alive)\n" "$info2_b"
	elif command -v python3 >/dev/null 2>&1; then
		python3 - <<'PY'
import mmap, struct, time
def rd(off):
    with open("/dev/mem","rb") as f:
        m=mmap.mmap(f.fileno(), 0x1000, offset=0x05026000, access=mmap.ACCESS_READ)
        v=struct.unpack_from("<I", m, off)[0]
        m.close()
        return v
print("RTC_INFO0: 0x%08x" % rd(0x1C))
print("RTC_INFO1: 0x%08x" % rd(0x20))
a=rd(0x24); print("RTC_INFO2: 0x%08x" % a)
print("RTC_INFO3: 0x%08x" % rd(0x28))
print("RTC_EN_PWR_VBAT_DET: 0x%08x" % rd(0xD0))
time.sleep(1)
b=rd(0x24); print("RTC_INFO2 after 1s: 0x%08x (delta=%d)" % (b, (b-a)&0xffffffff))
PY
	else
		die "need devmem or python3 to read RTC regs"
	fi
	echo
	echo "Note: normal boot must NOT clear RTC_INFO2."
	echo "Only 'mcu51-up -F' / firmware reload resets run_ms."
}

usage() {
	cat <<EOF
Usage: $0 <command>

  status          Show WiFi/BT LPM GPIO/IRQ status
  wifi-sleep      Put WiFi into manual sleep
  wifi-wake       Wake WiFi from manual sleep
  bt-lpm-on       Enable BT bluesleep protocol
  bt-lpm-off      Disable BT bluesleep protocol
  bt-allow-sleep  Allow BT chip sleep (deassert HOST_WAKE_BT)
  bt-wake         Assert HOST_WAKE_BT to wake BT
  prepare-mem     Arm WiFi sleep + BT LPM (does NOT enter mem)
  rtc-info        Dump RTC_INFO0-3 / power-detect (check not cleared)

Safety:
  - Does not toggle AIC8800 total power (GPIOA15)
  - Prefer serial console when testing wifi-sleep (SSH via WiFi will drop)
EOF
}

main() {
	cmd=${1:-}
	case "$cmd" in
	status) cmd_status ;;
	wifi-sleep) cmd_wifi_sleep ;;
	wifi-wake) cmd_wifi_wake ;;
	bt-lpm-on) cmd_bt_lpm_on ;;
	bt-lpm-off) cmd_bt_lpm_off ;;
	bt-allow-sleep) cmd_bt_allow_sleep ;;
	bt-wake) cmd_bt_wake ;;
	prepare-mem) cmd_prepare_mem ;;
	rtc-info) cmd_rtc_info ;;
	-h|--help|help|"") usage ;;
	*) die "unknown command: $cmd" ;;
	esac
}

main "$@"
