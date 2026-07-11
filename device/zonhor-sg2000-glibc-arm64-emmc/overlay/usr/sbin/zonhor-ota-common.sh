#!/bin/sh
#
# Shared helpers for zonhor eMMC A/B OTA.
#

DATA_DEV="${ZONHOR_DATA_DEV:-/dev/mmcblk0p7}"
DATA_MNT=/mnt/data

data_part_exists() {
	[ -b "$DATA_DEV" ]
}

data_is_mounted() {
	mountpoint -q "$DATA_MNT"
}

data_format_dev() {
	mke2fs -t ext4 -F -L DATA "$DATA_DEV" >/dev/null 2>&1
}

data_mount() {
	mkdir -p "$DATA_MNT"

	if data_is_mounted; then
		return 0
	fi

	if ! data_part_exists; then
		echo "WARNING: $DATA_DEV not found; using directory $DATA_MNT on rootfs"
		mkdir -p "$DATA_MNT"
		return 0
	fi

	e2fsck -y "$DATA_DEV" >/dev/null 2>&1
	if mount -t ext4 -o sync "$DATA_DEV" "$DATA_MNT" 2>/dev/null; then
		return 0
	fi

	echo "zonhor-ota: formatting DATA partition on $DATA_DEV"
	data_format_dev || return 1
	mount -t ext4 -o sync "$DATA_DEV" "$DATA_MNT" || return 1
}

ota_find_upgrade_zip() {
	if [ -f "$DATA_MNT/upgrade.zip" ]; then
		echo "$DATA_MNT/upgrade.zip"
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
