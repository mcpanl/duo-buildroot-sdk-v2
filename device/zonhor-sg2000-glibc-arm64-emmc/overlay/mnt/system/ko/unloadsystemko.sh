#!/bin/sh
# Unload CVITEK camera/media stack (reverse of loadsystemko.sh).
# Optionally reload cv181x_base to reset VB/ION bookkeeping.

${CVI_SHOPTS}

KO_DIR=/mnt/system/ko
. "$KO_DIR/ko-common.sh"

RELOAD_BASE=1
VERBOSE=0

while [ "$#" -gt 0 ]; do
	case "$1" in
	--keep-base)
		RELOAD_BASE=0
		;;
	-v|--verbose)
		VERBOSE=1
		;;
	*)
		echo "unloadsystemko.sh: unknown option: $1" >&2
		exit 1
		;;
	esac
	shift
done

log() {
	[ "$VERBOSE" -eq 1 ] && echo "$*"
}

warn_stuck() {
	mod="$1"
	if mod_loaded "$mod"; then
		echo "unloadsystemko.sh: still loaded: $mod" >&2
		return 1
	fi
	return 0
}

# Reverse dependency order of loadsystemko.sh (do not unload sys/pwm/rtc).
UNLOAD_ORDER="
cv181x_ive
cvi_vc_driver
cv181x_jpeg
cv181x_vcodec
cv181x_tpu
cv181x_clock_cooling
cv181x_rgn
cv181x_mipi_tx
cv181x_vo
cv181x_dwa
cv181x_vpss
cv181x_vi
snsr_i2c
cvi_mipi_rx
cv181x_zonhor_lcd_proxy
cv181x_zonhor_lcd_bl
cv181x_fast_image
cv181x_rtos_cmdqu
"

fail=0
for mod in $UNLOAD_ORDER; do
	log "rmmod $mod ..."
	if ! safe_rmmod "$mod" 10 0.3; then
		fail=1
	fi
done

if [ "$RELOAD_BASE" -eq 1 ]; then
	log "rmmod cv181x_base ..."
	if safe_rmmod cv181x_base 10 0.3; then
		log "insmod cv181x_base.ko ..."
		safe_insmod "$KO_DIR/cv181x_base.ko" || fail=1
	else
		fail=1
	fi
fi

for mod in $UNLOAD_ORDER; do
	warn_stuck "$mod" || fail=1
done

if [ "$RELOAD_BASE" -eq 1 ]; then
	if ! mod_loaded cv181x_base; then
		echo "unloadsystemko.sh: cv181x_base failed to reload" >&2
		fail=1
	fi
fi

exit "$fail"
