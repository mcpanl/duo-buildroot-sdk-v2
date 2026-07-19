#!/bin/bash
# Install mcu51 firmware + helpers into a rootfs TARGET_DIR.
# Usage: install_to_rootfs.sh <TARGET_DIR> [OUTPUT_DIR]

set -euo pipefail

TARGET_DIR="${1:?TARGET_DIR required}"
OUTPUT_DIR="${2:-}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MCU51_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

FW_SRC=""
if [ -n "${OUTPUT_DIR}" ] && [ -f "${OUTPUT_DIR}/mcu51/mars_mcu_fw.bin" ]; then
	FW_SRC="${OUTPUT_DIR}/mcu51/mars_mcu_fw.bin"
elif [ -f "${MCU51_DIR}/prebuilt/mars_mcu_fw.bin" ]; then
	FW_SRC="${MCU51_DIR}/prebuilt/mars_mcu_fw.bin"
fi

if [ -z "${FW_SRC}" ]; then
	echo "mcu51: no firmware binary found, skip install" >&2
	exit 0
fi

install -D -m 0644 "${FW_SRC}" \
	"${TARGET_DIR}/lib/firmware/mcu51/mars_mcu_fw.bin"

# Default boot address: RTC AHB SRAM
printf '0x05200000\n' > "${TARGET_DIR}/lib/firmware/mcu51/boot_cfg.ini"

if [ -n "${OUTPUT_DIR}" ] && [ -x "${OUTPUT_DIR}/mcu51/mcu51_up" ]; then
	install -D -m 0755 "${OUTPUT_DIR}/mcu51/mcu51_up" \
		"${TARGET_DIR}/usr/sbin/mcu51-up"
fi

if [ -n "${OUTPUT_DIR}" ] && [ -x "${OUTPUT_DIR}/mcu51/mcu51_ledctl" ]; then
	install -D -m 0755 "${OUTPUT_DIR}/mcu51/mcu51_ledctl" \
		"${TARGET_DIR}/usr/sbin/mcu51-ledctl"
fi

if [ -f "${MCU51_DIR}/scripts/mcu51-update" ]; then
	install -D -m 0755 "${MCU51_DIR}/scripts/mcu51-update" \
		"${TARGET_DIR}/usr/sbin/mcu51-update"
fi

if [ -f "${MCU51_DIR}/scripts/S30mcu51" ]; then
	install -D -m 0755 "${MCU51_DIR}/scripts/S30mcu51" \
		"${TARGET_DIR}/etc/init.d/S30mcu51"
fi

# Seed runtime directory layout (DATA may remount over /mnt/data)
install -d -m 0755 "${TARGET_DIR}/mnt/data/mcu51"

echo "mcu51: installed firmware from ${FW_SRC}"
