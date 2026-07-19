#!/bin/bash
# Fetch Milk-V packaged SDCC (x86_64) into mcu51/tools/sdcc for firmware builds.
# System sdcc from apt also works if present in PATH.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MCU51_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEST="${MCU51_DIR}/tools/sdcc"
TMPDIR_FETCH="${TMPDIR:-/tmp}/duo-8051-sdcc-$$"

if [ -x "${DEST}/bin/sdcc" ]; then
	echo "SDCC already present: ${DEST}/bin/sdcc"
	exit 0
fi

if command -v sdcc >/dev/null 2>&1; then
	echo "System sdcc found: $(command -v sdcc) (no download needed)"
	exit 0
fi

echo "Cloning milkv-duo/duo-8051 for tools/sdcc ..."
rm -rf "${TMPDIR_FETCH}"
git clone --depth=1 --filter=blob:none --sparse \
	https://github.com/milkv-duo/duo-8051.git "${TMPDIR_FETCH}"
(
	cd "${TMPDIR_FETCH}"
	git sparse-checkout set tools/sdcc
)
mkdir -p "${MCU51_DIR}/tools"
rm -rf "${DEST}"
cp -a "${TMPDIR_FETCH}/tools/sdcc" "${DEST}"
rm -rf "${TMPDIR_FETCH}"

if [ ! -x "${DEST}/bin/sdcc" ]; then
	echo "Failed to obtain SDCC at ${DEST}/bin/sdcc" >&2
	exit 1
fi

echo "Installed SDCC to ${DEST}"
"${DEST}/bin/sdcc" --version | head -1
