#!/bin/sh
# Shared helpers for CVITEK kernel module load/unload scripts.

mod_loaded() {
	mod="$1"
	# Match whole module name at start of lsmod line.
	lsmod | awk 'NR>1 {print $1}' | grep -qx "$mod"
}

safe_insmod() {
	ko="$1"
	shift

	[ -f "$ko" ] || return 1

	mod=$(basename "$ko" .ko)
	if mod_loaded "$mod"; then
		return 0
	fi

	insmod "$ko" "$@"
}

safe_rmmod() {
	mod="$1"
	tries="${2:-8}"
	delay="${3:-0.3}"

	while [ "$tries" -gt 0 ]; do
		if ! mod_loaded "$mod"; then
			return 0
		fi
		rmmod "$mod" 2>/dev/null && return 0
		tries=$((tries - 1))
		sleep "$delay"
	done

	mod_loaded "$mod"
}
