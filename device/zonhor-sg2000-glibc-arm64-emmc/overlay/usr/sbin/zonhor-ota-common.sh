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

OTA_FILE="${ZONHOR_OTA_FILE:-$DATA_MNT/upgrade.zip}"
OTA_PENDING="${ZONHOR_OTA_PENDING:-$DATA_MNT/upgrade.pending}"
OTA_WORK="${ZONHOR_OTA_WORK:-$DATA_MNT/work}"
OTA_LOG="${ZONHOR_OTA_LOG:-$DATA_MNT/upgrade.log}"
OTA_FAIL="${ZONHOR_OTA_FAIL:-$DATA_MNT/upgrade.failed}"

ota_find_upgrade_zip() {
	if [ -f "$OTA_FILE" ]; then
		echo "$OTA_FILE"
		return 0
	fi
	return 1
}

# True when stage already unpacked+converted images under $OTA_WORK.
ota_work_ready() {
	[ -f "$OTA_WORK/boot.emmc" ] || return 1
	[ -f "$OTA_WORK/rootfs_ext4.emmc" ] || return 1
	ota_verify_ext4_image "$OTA_WORK/rootfs_ext4.emmc"
}

ota_image_is_cimg() {
	img="$1"
	[ -e "$img" ] || return 1
	magic=$(dd if="$img" bs=1 count=4 2>/dev/null)
	[ "$magic" = "CIMG" ]
}

# Quick sanity check that a raw image looks like an ext4 filesystem.
# Superblock is at offset 1024; magic 0xEF53 is at +0x38 (little-endian).
ota_verify_ext4_image() {
	img="$1"
	[ -e "$img" ] || return 1
	if ota_image_is_cimg "$img"; then
		# CIMG produced by raw2cimg stores the first payload immediately after
		# the 64-byte image header and 64-byte first chunk header.
		magic=$(dd if="$img" bs=1 skip=1208 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')
		[ "$magic" = "53ef" ] || [ "$magic" = "53EF" ]
		return $?
	fi
	# od is more portable here than hexdump on busybox/buildroot.
	magic=$(dd if="$img" bs=1 skip=1080 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')
	[ "$magic" = "53ef" ] || [ "$magic" = "53EF" ]
}

ota_cimg2raw() {
	file="$1"
	dir=$(dirname "$file")
	base=$(basename "$file")
	tmpdir=""
	raw=""

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

	# Keep temp on the same filesystem as $file. /tmp is a small tmpfs and
	# cannot hold a 1.5G rootfs image during conversion.
	tmpdir=$(mktemp -d "$dir/.cimg.XXXXXX") || return 1
	python3 /usr/sbin/zonhor-cimg2raw.py "$file" --output_dir "$tmpdir" || {
		rm -rf "$tmpdir"
		return 1
	}
	raw="$tmpdir/$base"
	# Drop the CIMG source first so peak free-space need is ~1x image size.
	rm -f "$file" || {
		rm -rf "$tmpdir"
		return 1
	}
	mv -f "$raw" "$file" || {
		rm -rf "$tmpdir"
		return 1
	}
	rm -rf "$tmpdir"
}

ota_prepare_log() {
	msg="$*"
	ts=$(date '+%F %T' 2>/dev/null || echo "--")
	echo "$ts $msg" | tee -a "$OTA_LOG"
}

# Verify one file under $OTA_WORK against META/metadata.txt (cwd must be OTA_WORK).
ota_verify_one() {
	name="$1"
	[ -f "$name" ] || {
		echo "missing $name after unzip"
		return 1
	}
	line=$(grep -E "  ${name}\$" META/metadata.txt) || {
		echo "no checksum entry for $name"
		return 1
	}
	echo "$line" | md5sum -c >>"$OTA_LOG" 2>&1 || {
		echo "checksum verification failed: $name"
		return 1
	}
}

# Unpack zip + verify into $OTA_WORK; keep CIMG/sparse images for direct flash.
# Safe to run on the live system during zonhor-ota-stage.
# On failure prints reason to stdout and returns non-zero; leaves $OTA_WORK dirty.
ota_prepare_work() {
	_prep_fail() {
		echo "$*"
		ota_prepare_log "ERROR: $*"
		return 1
	}

	[ -f "$OTA_FILE" ] || _prep_fail "missing $OTA_FILE" || return 1

	rm -rf "$OTA_WORK"
	mkdir -p "$OTA_WORK" || _prep_fail "cannot create $OTA_WORK" || return 1

	ota_prepare_log "checking package metadata"
	ota_ui_update CHECK 5 "meta"
	unzip -q -d "$OTA_WORK" "$OTA_FILE" META/misc_info.txt || \
		_prep_fail "META/misc_info.txt missing" || return 1
	# shellcheck disable=SC1090
	. "$OTA_WORK/META/misc_info.txt"
	[ "$STORAGE_TYPE" = "emmc" ] || \
		_prep_fail "package STORAGE_TYPE=$STORAGE_TYPE, expected emmc" || return 1

	# Only extract images needed for inactive-slot flash. Full packages may also
	# contain boot_b/rootfs_b (~1.5G each); extracting them exhausts DATA free space.
	ota_prepare_log "unpacking required images from $OTA_FILE"
	ota_ui_update UNPACK 10 "unzip"
	unzip -o "$OTA_FILE" -d "$OTA_WORK" \
		META/metadata.txt \
		boot.emmc \
		rootfs_ext4.emmc || \
		_prep_fail "unzip required images failed" || return 1
	ota_ui_update UNPACK 22 "boot+root"
	unzip -o "$OTA_FILE" -d "$OTA_WORK" logo.jpg >/dev/null 2>&1 || true
	ota_ui_update UNPACK 25 "done"

	[ -f "$OTA_WORK/META/metadata.txt" ] || \
		_prep_fail "missing META/metadata.txt" || return 1

	ota_prepare_log "verifying image checksums"
	ota_ui_update VERIFY 27 "md5"
	cd "$OTA_WORK" || _prep_fail "cannot enter $OTA_WORK" || return 1
	ota_verify_one boot.emmc || _prep_fail "boot.emmc checksum failed" || return 1
	ota_ui_update VERIFY 28 "boot"
	ota_verify_one rootfs_ext4.emmc || _prep_fail "rootfs_ext4.emmc checksum failed" || return 1
	ota_ui_update VERIFY 29 "rootfs"
	if [ -f logo.jpg ]; then
		ota_verify_one logo.jpg || _prep_fail "logo.jpg checksum failed" || return 1
	fi
	ota_ui_update VERIFY 30 "done"

	ota_prepare_log "keeping CIMG/sparse images for direct flash"
	ota_ui_update CONVERT 32 "boot"
	# New OTA apply can flash CIMG directly. Keeping the packed form avoids
	# expanding rootfs to a second 1.5G raw copy under DATA.
	ota_ui_update CONVERT 33 "rootfs"
	if ! ota_verify_ext4_image rootfs_ext4.emmc; then
		_prep_fail "rootfs image failed ext4 magic check" || return 1
	fi
	ota_prepare_log "rootfs staged size=$(wc -c < rootfs_ext4.emmc) bytes, ext4 magic OK"
	if [ -f logo.jpg ]; then
		ota_ui_update CONVERT 34 "logo"
	fi
	ota_ui_update CONVERT 35 "done"
	ota_prepare_log "prepare complete: $OTA_WORK ready"
	return 0
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

# ---- OTA framebuffer UI ----------------------------------------------------

OTA_UI_BIN=/usr/sbin/zonhor-ota-ui.py
OTA_FLASH_BIN=/usr/sbin/zonhor-ota-flash.py
OTA_UI_STATUS="${ZONHOR_OTA_STATUS:-/run/zonhor-ota.status}"
OTA_UI_T0=0
OTA_UI_ACTIVE=
OTA_UI_TARGET=

ota_ui_ensure_run() {
	if ! mountpoint -q /run 2>/dev/null; then
		mkdir -p /run
		mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /run 2>/dev/null || true
	fi
	mkdir -p /run 2>/dev/null || true
}

ota_ui_available() {
	[ -f "$OTA_UI_BIN" ] && [ -e /dev/fb0 ] && command -v python3 >/dev/null 2>&1
}

ota_ui_start() {
	OTA_UI_ACTIVE="$1"
	OTA_UI_TARGET="$2"
	OTA_UI_T0=$(date +%s 2>/dev/null || echo 0)
	ota_ui_available || return 0
	ota_ui_ensure_run
	python3 "$OTA_UI_BIN" --status "$OTA_UI_STATUS" stop >/dev/null 2>&1 || true
	: >"$OTA_UI_STATUS" 2>/dev/null || true
	python3 "$OTA_UI_BIN" --status "$OTA_UI_STATUS" update \
		--stage CHECK --pct 0 --msg "" --eta-s -1 --error "" \
		--active "${OTA_UI_ACTIVE}" --target "${OTA_UI_TARGET}" >/dev/null 2>&1 || true
	python3 "$OTA_UI_BIN" --status "$OTA_UI_STATUS" start >/dev/null 2>&1 || true
}

ota_ui_eta() {
	pct="$1"
	[ -n "$OTA_UI_T0" ] && [ "$OTA_UI_T0" -gt 0 ] 2>/dev/null || {
		echo -1
		return
	}
	[ "$pct" -ge 5 ] 2>/dev/null || {
		echo -1
		return
	}
	now=$(date +%s 2>/dev/null || echo 0)
	elapsed=$((now - OTA_UI_T0))
	[ "$elapsed" -ge 1 ] 2>/dev/null || {
		echo -1
		return
	}
	# eta = elapsed * (100 - pct) / pct
	echo $((elapsed * (100 - pct) / pct))
}

ota_ui_update() {
	stage="$1"
	pct="$2"
	msg="${3:-}"
	ota_ui_available || return 0
	eta=$(ota_ui_eta "$pct")
	python3 "$OTA_UI_BIN" --status "$OTA_UI_STATUS" update \
		--stage "$stage" --pct "$pct" --msg "$msg" --eta-s "$eta" \
		--active "${OTA_UI_ACTIVE}" --target "${OTA_UI_TARGET}" \
		--error "" >/dev/null 2>&1 || true
}

ota_ui_success() {
	ota_ui_available || return 0
	python3 "$OTA_UI_BIN" --status "$OTA_UI_STATUS" update \
		--stage SUCCESS --pct 100 --msg "reboot" --eta-s 0 \
		--active "${OTA_UI_ACTIVE}" --target "${OTA_UI_TARGET}" \
		--error "" >/dev/null 2>&1 || true
	# Let the daemon redraw SUCCESS before we kill it / reboot.
	sleep 1
}

ota_ui_error() {
	err="${1:-ERROR}"
	ota_ui_available || return 0
	# Keep first ~24 safe ASCII chars for the tiny font.
	err_safe=$(printf '%s' "$err" | tr -cd 'A-Za-z0-9 ._/-' | cut -c1-24)
	python3 "$OTA_UI_BIN" --status "$OTA_UI_STATUS" update \
		--stage ERROR --pct 0 --msg "" --eta-s -1 \
		--active "${OTA_UI_ACTIVE}" --target "${OTA_UI_TARGET}" \
		--error "$err_safe" >/dev/null 2>&1 || true
	# If the daemon is not running (early fail), draw one ERROR frame.
	if [ ! -f /run/zonhor-ota-ui.pid ]; then
		python3 "$OTA_UI_BIN" --status "$OTA_UI_STATUS" once \
			--stage ERROR --active "${OTA_UI_ACTIVE}" --target "${OTA_UI_TARGET}" \
			--error "$err_safe" --hold 0 >/dev/null 2>&1 || true
	fi
	sleep 1
}

ota_ui_stop() {
	[ -f "$OTA_UI_BIN" ] || return 0
	python3 "$OTA_UI_BIN" --status "$OTA_UI_STATUS" stop >/dev/null 2>&1 || true
}

ota_ui_staged() {
	active="$1"
	target="$2"
	ota_ui_available || return 0
	python3 "$OTA_UI_BIN" --status "$OTA_UI_STATUS" once \
		--stage STAGED --active "$active" --target "$target" --hold 2 >/dev/null 2>&1 || true
}

ota_slot_boot_dev() {
	case "$1" in
	a|A) echo /dev/mmcblk0p1 ;;
	b|B) echo /dev/mmcblk0p2 ;;
	*) return 1 ;;
	esac
}

ota_slot_root_dev() {
	case "$1" in
	a|A) echo /dev/mmcblk0p5 ;;
	b|B) echo /dev/mmcblk0p6 ;;
	*) return 1 ;;
	esac
}

# Inject into kernel log (dmesg). Does not print to stdout.
ota_kmsg_only() {
	# <6> = KERN_INFO
	printf '<6>zonhor-ota: %s\n' "$*" >/dev/kmsg 2>/dev/null || true
}

# Serial console + dmesg.
ota_kmsg() {
	echo "zonhor-ota: $*"
	ota_kmsg_only "$*"
}

ota_flash_image() {
	# ota_flash_image <image> <device> <stage> <pct_lo> <pct_hi>
	img="$1"
	dev="$2"
	stage="$3"
	plo="$4"
	phi="$5"
	sz=$(wc -c <"$img" 2>/dev/null || echo 0)

	ota_kmsg "$stage begin $img -> $dev ($sz bytes)"

	if [ -f "$OTA_FLASH_BIN" ] && command -v python3 >/dev/null 2>&1; then
		# --log is optional: older on-device flash.py may not accept it.
		set -- "$img" "$dev" \
			--stage "$stage" --pct-lo "$plo" --pct-hi "$phi" \
			--status "$OTA_UI_STATUS" --t0 "$OTA_UI_T0" \
			--active "${OTA_UI_ACTIVE}" --target "${OTA_UI_TARGET}"
		if [ -n "$OTA_LOG" ] && grep -q -- '--log' "$OTA_FLASH_BIN" 2>/dev/null; then
			set -- "$@" --log "$OTA_LOG"
		fi
		python3 "$OTA_FLASH_BIN" "$@" || return 1
		return 0
	fi

	# Fallback without progress UI helper: timed dd with console notes.
	ota_ui_update "$stage" "$plo" "flash"
	t0=$(date +%s 2>/dev/null || echo 0)
	dd if="$img" of="$dev" bs=4M conv=fsync status=none || return 1
	t1=$(date +%s 2>/dev/null || echo 0)
	elapsed=$((t1 - t0))
	[ "$elapsed" -lt 1 ] && elapsed=1
	# avg MiB/s ≈ size / (elapsed * 1MiB)
	avg=$((sz / elapsed / 1024 / 1024))
	ota_kmsg "$stage dd done in ${elapsed}s avg~${avg} MB/s"
	if [ -b /dev/mmcblk0 ]; then
		blockdev --flushbufs /dev/mmcblk0 2>/dev/null || true
	fi
	ota_ui_update "$stage" "$phi" "done"
}
