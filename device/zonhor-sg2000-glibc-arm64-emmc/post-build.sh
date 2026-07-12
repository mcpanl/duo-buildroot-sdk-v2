#!/bin/sh
#
# Remove stale Dropbear files after switching to OpenSSH.
# Buildroot incremental rebuilds may leave old package files in target/.
#

set -e

rm -f "${TARGET_DIR}/usr/sbin/dropbear"
rm -f "${TARGET_DIR}/usr/bin/dropbearconvert"
rm -f "${TARGET_DIR}/usr/bin/dropbearkey"
rm -f "${TARGET_DIR}/usr/bin/dbclient"
rm -f "${TARGET_DIR}/etc/init.d/S50dropbear"
rm -rf "${TARGET_DIR}/etc/dropbear"

# Dropbear's ssh symlink must not shadow OpenSSH client.
if [ -L "${TARGET_DIR}/usr/bin/ssh" ]; then
	rm -f "${TARGET_DIR}/usr/bin/ssh"
fi
