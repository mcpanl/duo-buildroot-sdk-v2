#!/bin/bash

SYSTEM_DIR=$1
SNS_CFG=$SYSTEM_DIR/mnt/system/usr/bin/sensor_cfg.ini
SNS_CFG_BAK=$SYSTEM_DIR/mnt/system/sensor_cfg.ini.imx678

if [ -f "$SNS_CFG" ]; then
	cp "$SNS_CFG" "$SNS_CFG_BAK"
fi

rm -rf $SYSTEM_DIR/mnt/system/usr
if [ -f "$SNS_CFG_BAK" ]; then
	mkdir -p "$(dirname "$SNS_CFG")"
	mv "$SNS_CFG_BAK" "$SNS_CFG"
fi

rm -rf $SYSTEM_DIR/mnt/system/lib/libsns_gc*
for snslib in $SYSTEM_DIR/mnt/system/lib/libsns_imx*; do
	case "$(basename "$snslib")" in
		libsns_imx678.*) ;;
		*) rm -rf "$snslib" ;;
	esac
done
rm -rf $SYSTEM_DIR/mnt/system/lib/libsns_sc*
rm -rf $SYSTEM_DIR/mnt/system/lib/libcipher.so

rm -rf $SYSTEM_DIR/mnt/system/m2m-deinterlace.ko
rm -rf $SYSTEM_DIR/mnt/system/efivarfs.ko

du -sh $SYSTEM_DIR/* |sort -rh

rm -rf $SYSTEM_DIR/etc/init.d/S23ntp
rm -rf $SYSTEM_DIR/bin/ntpd
