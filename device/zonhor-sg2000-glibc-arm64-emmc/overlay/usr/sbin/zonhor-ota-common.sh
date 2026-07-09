#!/bin/sh
#
# Shared helpers for zonhor eMMC OTA.
#

OTA_DEV="${ZONHOR_OTA_DEV:-/dev/mmcblk0p5}"
OTA_MNT=/mnt/ota
OTA_STASH=/tmp/ota-stash

ota_part_exists() {
	[ -b "$OTA_DEV" ]
}

ota_is_mounted() {
	mountpoint -q "$OTA_MNT"
}

ota_stash_rootfs_files() {
	# Before mounting p5 on /mnt/ota, preserve files that live on rootfs.
	if ota_is_mounted; then
		return 0
	fi
	if [ ! -d "$OTA_MNT" ]; then
		return 0
	fi
	if [ -z "$(ls -A "$OTA_MNT" 2>/dev/null)" ]; then
		return 0
	fi

	rm -rf "$OTA_STASH"
	mkdir -p "$OTA_STASH"
	cp -a "$OTA_MNT/." "$OTA_STASH/" 2>/dev/null
}

ota_restore_stash() {
	if [ ! -d "$OTA_STASH" ]; then
		return 0
	fi
	if [ -z "$(ls -A "$OTA_STASH" 2>/dev/null)" ]; then
		return 0
	fi

	mkdir -p "$OTA_MNT"
	for f in "$OTA_STASH"/*; do
		[ -e "$f" ] || continue
		base=$(basename "$f")
		[ -e "$OTA_MNT/$base" ] || cp -a "$f" "$OTA_MNT/"
	done
	rm -rf "$OTA_STASH"
}

ota_format_dev() {
	mke2fs -t ext4 -F -L OTA "$OTA_DEV" >/dev/null 2>&1
}

ota_mount() {
	mkdir -p "$OTA_MNT"

	if ota_is_mounted; then
		return 0
	fi

	if ota_part_exists; then
		ota_stash_rootfs_files
		e2fsck -y "$OTA_DEV" >/dev/null 2>&1
		if mount -t ext4 -o sync "$OTA_DEV" "$OTA_MNT" 2>/dev/null; then
			ota_restore_stash
			return 0
		fi
		echo "zonhor-ota: formatting OTA partition on $OTA_DEV"
		ota_format_dev || return 1
		mount -t ext4 -o sync "$OTA_DEV" "$OTA_MNT" || return 1
		ota_restore_stash
		return 0
	fi

	# Fallback: use /mnt/ota directory on rootfs when bootloader is old.
	mkdir -p "$OTA_MNT"
	return 0
}

ota_warn_no_partition() {
	if ota_part_exists; then
		return 0
	fi
	echo "WARNING: $OTA_DEV not found; using rootfs directory $OTA_MNT"
	echo "WARNING: run 'zonhor-ota-migrate-fip' then reboot before ROOTFS OTA"
}

ota_find_upgrade_zip() {
	if [ -f "$OTA_MNT/upgrade.zip" ]; then
		echo "$OTA_MNT/upgrade.zip"
		return 0
	fi
	if [ -f "$OTA_STASH/upgrade.zip" ]; then
		echo "$OTA_STASH/upgrade.zip"
		return 0
	fi
	return 1
}

ota_cimg2raw() {
	file="$1"
	dir=$(dirname "$file")
	base=$(basename "$file")
	tmpdir=""

	if [ ! -f "$file" ]; then
		return 0
	fi

	if ! dd if="$file" bs=1 count=4 2>/dev/null | grep -q CIMG; then
		return 0
	fi

	if ! command -v python3 >/dev/null 2>&1 || [ ! -f /usr/sbin/zonhor-cimg2raw.py ]; then
		echo "ERROR: python3 or zonhor-cimg2raw.py missing"
		return 1
	fi

	tmpdir=$(mktemp -d)
	python3 /usr/sbin/zonhor-cimg2raw.py "$file" --output_dir "$tmpdir" || {
		rm -rf "$tmpdir"
		return 1
	}
	mv -f "$tmpdir/$base" "$file"
	rm -rf "$tmpdir"
}

ota_flash_boot_misc() {
	work="$1"
	cd "$work" || return 1

	if [ -f boot.emmc ]; then
		ota_cimg2raw boot.emmc || return 1
		dd if=boot.emmc of=/dev/mmcblk0p1 bs=4M conv=fsync status=none || return 1
	fi

	if [ -f logo.jpg ]; then
		ota_cimg2raw logo.jpg || return 1
		dd if=logo.jpg of=/dev/mmcblk0p2 bs=4M conv=fsync status=none || return 1
	fi

	return 0
}

ota_prepare_rootfs_image() {
	work="$1"
	cd "$work" || return 1
	[ -f rootfs_ext4.emmc ] || return 1
	ota_cimg2raw rootfs_ext4.emmc || return 1
	return 0
}

ota_prepare_recovery_root() {
	recovery_init="$1"
	mkdir -p "$OTA_MNT/bin"
	cp /bin/busybox "$OTA_MNT/bin/busybox"
	chmod +x "$OTA_MNT/bin/busybox"
	for app in sh dd sync reboot mount sleep mkdir echo cat tee rm mv usleep; do
		ln -sf busybox "$OTA_MNT/bin/$app"
	done
	cp "$recovery_init" "$OTA_MNT/recovery-init.sh"
	chmod +x "$OTA_MNT/recovery-init.sh"
	if [ -f /usr/sbin/zonhor-ota-led.sh ]; then
		cp /usr/sbin/zonhor-ota-led.sh "$OTA_MNT/recovery-led.sh"
		chmod +x "$OTA_MNT/recovery-led.sh"
	fi
	touch "$OTA_MNT/rootfs.pending"
}

ota_env_set_recovery() {
	if ! command -v fw_setenv >/dev/null 2>&1; then
		echo "ERROR: fw_setenv not installed"
		return 1
	fi
	fw_setenv root "root=/dev/mmcblk0p5 rootwait rw init=/recovery-init.sh"
}

ota_env_set_normal() {
	if ! command -v fw_setenv >/dev/null 2>&1; then
		return 1
	fi
	fw_setenv root "root=/dev/mmcblk0p4 rootwait rw"
}

ota_boot_recovery() {
	ota_env_set_recovery || return 1
	sync
	reboot -f
}

OTA_LED_SCRIPT=/usr/sbin/zonhor-ota-led.sh

ota_led_start() {
	[ -x "$OTA_LED_SCRIPT" ] || return 0
	"$OTA_LED_SCRIPT" start "$1" 2>/dev/null
}

ota_led_stop() {
	[ -x "$OTA_LED_SCRIPT" ] || return 0
	"$OTA_LED_SCRIPT" stop 2>/dev/null
}

ota_led_success() {
	[ -x "$OTA_LED_SCRIPT" ] || return 0
	"$OTA_LED_SCRIPT" success 2>/dev/null
}

ota_led_error() {
	[ -x "$OTA_LED_SCRIPT" ] || return 0
	"$OTA_LED_SCRIPT" error 2>/dev/null
}
