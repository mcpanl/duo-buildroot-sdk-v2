#!/bin/bash
#
# Write /etc/zonhor-firmware-version into the staged rootfs target directory.
# Optional second argument copies the same file to the build output tree.
#

set -euo pipefail

TARGET_DIR="${1:?target directory required}"
OUTPUT_COPY="${2:-}"
BOARD="${3:-${MV_BOARD:-unknown}}"

VERSION="$(date +%Y%m%d%H%M)"
BUILD_TIME="$(date +"%Y-%m-%d %H:%M:%S")"

mkdir -p "${TARGET_DIR}/etc"
cat > "${TARGET_DIR}/etc/zonhor-firmware-version" <<EOF
VERSION=${VERSION}
BUILD_TIME="${BUILD_TIME}"
BOARD=${BOARD}
EOF
chmod 644 "${TARGET_DIR}/etc/zonhor-firmware-version"

if [ -n "${OUTPUT_COPY}" ]; then
	mkdir -p "$(dirname "${OUTPUT_COPY}")"
	cp -f "${TARGET_DIR}/etc/zonhor-firmware-version" "${OUTPUT_COPY}"
fi
