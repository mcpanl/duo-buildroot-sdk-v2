#!/bin/sh
#
# Minimal init for phase-2 rootfs OTA.
# Kernel boots with: root=/dev/mmcblk0p5 init=/recovery-init.sh
#

export PATH=/bin:/sbin

OTA_LOG=/upgrade.log
WORK=/work
LED_PID=/tmp/zonhor-ota-led.pid

log() {
	echo "$(date '+%F %T') recovery: $*" | tee -a "$OTA_LOG"
}

led_start_recovery() {
	if [ -x /recovery-led.sh ]; then
		ZONHOR_OTA_LED_PID="$LED_PID" /recovery-led.sh start recovery &
	fi
}

led_stop() {
	if [ -x /recovery-led.sh ]; then
		ZONHOR_OTA_LED_PID="$LED_PID" /recovery-led.sh stop
	fi
}

led_error() {
	if [ -x /recovery-led.sh ]; then
		ZONHOR_OTA_LED_PID="$LED_PID" /recovery-led.sh error
	fi
}

log "starting rootfs recovery"

mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
[ -c /dev/null ] || mdev -s 2>/dev/null

led_start_recovery

[ -f /rootfs.pending ] || log "warning: rootfs.pending missing"
[ -f "$WORK/rootfs_ext4.emmc" ] || {
	log "ERROR: $WORK/rootfs_ext4.emmc missing"
	led_error
	reboot -f
}

log "flashing ROOTFS to /dev/mmcblk0p4"
dd if="$WORK/rootfs_ext4.emmc" of=/dev/mmcblk0p4 bs=4M conv=fsync 2>>"$OTA_LOG" || {
	log "ERROR: dd failed"
	led_error
	reboot -f
}

sync
sleep 2

if command -v fw_setenv >/dev/null 2>&1; then
	fw_setenv root "root=/dev/mmcblk0p4 rootwait rw" || log "warning: fw_setenv failed"
fi

rm -f /rootfs.pending /upgrade.pending
[ -f /upgrade.zip ] && mv -f /upgrade.zip /upgrade.last.zip 2>/dev/null
sync

log "done, rebooting"
led_stop
if [ -x /recovery-led.sh ]; then
	ZONHOR_OTA_LED_PID="$LED_PID" /recovery-led.sh success
fi
reboot -f

while sleep 5; do reboot -f; done
