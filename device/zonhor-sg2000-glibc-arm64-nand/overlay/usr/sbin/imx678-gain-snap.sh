#!/bin/sh
# Snapshot IMX678 gain/HCG registers and CVITEK proc while camera is streaming.
# Usage: imx678-gain-snap.sh [tag]
# Example:
#   imx678-gain-snap.sh LOW_GAIN_OK  > /tmp/low.txt
#   imx678-gain-snap.sh HIGH_GAIN_BAD > /tmp/high.txt

BUS=3
ADDR=0x1a
TAG="${1:-snap}"

# i2ctransfer prints "0xNN"; strip prefix for display and arithmetic.
i2c_read() {
	reg=$1
	hi=$(( (reg >> 8) & 0xff ))
	lo=$(( reg & 0xff ))
	raw=$(i2ctransfer -y "$BUS" w2@"$ADDR" "$hi" "$lo" r1 2>/dev/null | head -1)
	case "$raw" in
	0x*|0X*) printf '%s\n' "$raw" ;;
	*) printf 'ERR\n' ;;
	esac
}

hex_byte() {
	v=$1
	v=${v#0x}
	v=${v#0X}
	case "$v" in
	''|*[!0-9a-fA-F]*) printf '0' ;;
	*) printf '%d' "0x$v" ;;
	esac
}

rr() {
	reg=$1
	name=$2
	val=$(i2c_read "$reg")
	printf "0x%04X  %-22s  %s\n" "$reg" "$name" "$val"
}

echo "======== IMX678 @$TAG ========"
date
echo "--- sensor regs (camera must be streaming) ---"
rr 0x3000 "STANDBY"
rr 0x3002 "XMSTA"
rr 0x301B "ADDMODE (1=2x2 bin)"
rr 0x3022 "ADBIT/CHIP_ID"
rr 0x3030 "HCG/FDG_SEL (bit0=HCG?)"
rr 0x3400 "GAIN_PGC_FIDMD"
rr 0x3050 "SHR0_LSB"
rr 0x3051 "SHR0_MID"
rr 0x3052 "SHR0_MSB"
rr 0x3070 "ANALOG_GAIN"
rr 0x3076 "DIGITAL_GAIN"
rr 0x3028 "VMAX_LSB"
rr 0x3029 "VMAX_MID"
rr 0x302A "VMAX_MSB"

shr_l=$(i2c_read 0x3050)
shr_m=$(i2c_read 0x3051)
shr_h=$(i2c_read 0x3052)
shr_val=$(( $(hex_byte "$shr_h") * 65536 + $(hex_byte "$shr_m") * 256 + $(hex_byte "$shr_l") ))
printf "SHR0 combined = %d (0x%06X)  [raw: %s %s %s]\n" "$shr_val" "$shr_val" "$shr_l" "$shr_m" "$shr_h"

echo "--- /proc/mipi-rx errors ---"
grep -E "Devno|EccErr|CrcErr|HdrErr|WcErr|fifofull|CK_ERR" /proc/mipi-rx 2>/dev/null || true

echo "--- /proc/cvitek/vi ---"
grep -E "DetectErrFrame|DropErrFrame|LostFrame|VbFail|FrameRate|RecvPic" /proc/cvitek/vi 2>/dev/null || true
