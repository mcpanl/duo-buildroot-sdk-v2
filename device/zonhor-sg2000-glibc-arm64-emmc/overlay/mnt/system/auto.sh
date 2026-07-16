#!/bin/sh
#
# Boot-time WiFi auto-connect for zonhor-sg2000.
# Started by /etc/init.d/S99user after duo-init.sh loads the WLAN driver.
#

interface="wlan0"
max_attempts=100
attempt=0
log_file="/var/log/auto.sh.log"
wpa_conf="/etc/wpa_supplicant.conf"
ip_ui="/usr/sbin/zonhor-wlan-ip-ui.py"

log() {
	echo "$(date +'%Y-%m-%d %H:%M:%S') $*" >>"$log_file"
}

get_wlan_ip() {
	ip -4 -o addr show dev "$interface" 2>/dev/null |
		awk '/inet/ {print $4}' | head -1 | cut -d/ -f1
}

get_wpa_state() {
	wpa_cli -i "$interface" status 2>/dev/null |
		awk -F= '/^wpa_state=/ {print $2; exit}'
}

is_real_ip() {
	ip="$1"
	[ -n "$ip" ] || return 1
	case "$ip" in
	169.254.*|0.*) return 1 ;;
	esac
	return 0
}

draw_screen() {
	ip="$1"
	state="$2"
	if [ ! -x "$ip_ui" ] || [ ! -e /dev/fb0 ]; then
		return 0
	fi
	"$ip_ui" --state "$state" --ip "$ip" >>"$log_file" 2>&1 || true
}

echo "start auto.sh" >"$log_file"

while [ "$attempt" -lt "$max_attempts" ]; do
	if ip link show "$interface" >/dev/null 2>&1; then
		log "$interface interface exists, starting wpa_supplicant..."
		ip link set "$interface" up >>"$log_file" 2>&1 || true
		if ! pidof wpa_supplicant >/dev/null 2>&1; then
			wpa_supplicant -B -i "$interface" -c "$wpa_conf" >>"$log_file" 2>&1
		fi
		break
	fi

	log "$interface interface not found, waiting..."
	sleep 1
	attempt=$((attempt + 1))
done

if [ "$attempt" -eq "$max_attempts" ]; then
	log "Interface $interface not found after $max_attempts attempts"
	draw_screen "" "FAILED"
	exit 1
fi

last_ip=""
last_state=""
while true; do
	state=$(get_wpa_state)
	ip=$(get_wlan_ip)
	redraw=0

	if [ "$state" = "COMPLETED" ] && [ "$last_state" != "COMPLETED" ]; then
		log "WiFi associated"
		redraw=1
	fi

	if [ -n "$ip" ] && [ "$ip" != "$last_ip" ]; then
		log "wlan IP changed: ${last_ip:-none} -> $ip"
		redraw=1
	fi

	if [ "$redraw" -eq 1 ]; then
		draw_screen "$ip" "$state"
	fi

	if is_real_ip "$ip" && ! is_real_ip "$last_ip"; then
		log "real IP acquired, stop monitoring: $ip"
		exit 0
	fi

	[ -n "$ip" ] && last_ip="$ip"
	[ -n "$state" ] && last_state="$state"
	sleep 2
done
