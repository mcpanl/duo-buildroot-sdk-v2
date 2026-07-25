#!/usr/bin/env python3
"""
Listen for GPIOE1 / rtc-wakeup-button input events (KEY_WAKEUP=143).

After DTS split (gpio-keys-rtc), this key has its own /dev/input/eventN.
Edge IRQ via porte — kernel gpio-keys, no userspace polling of the pin.

Usage:
  zonhor-gpioe1-input-watch.py           # auto-find device by name/keycode
  zonhor-gpioe1-input-watch.py /dev/input/event2
"""

from __future__ import annotations

import argparse
import os
import select
import struct
import sys

KEY_WAKEUP = 143
EV_KEY = 1
EV_SYN = 0

# input_event: timeval(ll) or (II) depending on arch; aarch64 uses long long
EVENT_FMT = "llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FMT)


def find_device() -> str:
	base = "/sys/class/input"
	if not os.path.isdir(base):
		raise SystemExit("no /sys/class/input")

	candidates = []
	for name in sorted(os.listdir(base)):
		if not name.startswith("event"):
			continue
		dev = f"/dev/input/{name}"
		sysfs = os.path.join(base, name, "device")
		# Prefer device name containing rtc-wakeup
		name_path = os.path.join(sysfs, "name")
		dev_name = ""
		if os.path.isfile(name_path):
			with open(name_path, "r", encoding="ascii", errors="ignore") as f:
				dev_name = f.read().strip()
		# capabilities/key bit for KEY_WAKEUP
		cap = os.path.join(sysfs, "capabilities", "key")
		has_wakeup = False
		if os.path.isfile(cap):
			with open(cap, "r", encoding="ascii") as f:
				words = f.read().split()
			# words are hex, least-significant long first
			bit = KEY_WAKEUP
			idx = bit // 64
			off = bit % 64
			if idx < len(words):
				try:
					val = int(words[idx], 16)
					has_wakeup = bool(val & (1 << off))
				except ValueError:
					pass
		if "gpio-keys" in dev_name.lower() or "rtc" in dev_name.lower():
			if has_wakeup or "rtc" in dev_name.lower():
				candidates.insert(0, (dev, dev_name, has_wakeup))
				continue
		if has_wakeup:
			candidates.append((dev, dev_name, has_wakeup))

	# Also match by parent platform name gpio-keys-rtc
	for name in sorted(os.listdir(base)):
		if not name.startswith("event"):
			continue
		uevent = os.path.join(base, name, "device", "uevent")
		phys = os.path.join(base, name, "device", "phys")
		text = ""
		for p in (uevent, phys):
			if os.path.isfile(p):
				with open(p, "r", encoding="ascii", errors="ignore") as f:
					text += f.read()
		if "gpio-keys-rtc" in text:
			dev = f"/dev/input/{name}"
			with open(os.path.join(base, name, "device", "name"), "r") as f:
				dev_name = f.read().strip()
			return dev

	if not candidates:
		raise SystemExit(
			"no KEY_WAKEUP gpio-keys device found; rebuild/flash DTS with gpio-keys-rtc"
		)
	return candidates[0][0]


def main() -> int:
	ap = argparse.ArgumentParser()
	ap.add_argument("device", nargs="?", help="/dev/input/eventN (optional)")
	args = ap.parse_args()

	dev = args.device or find_device()
	print(f"listening {dev} for KEY_WAKEUP({KEY_WAKEUP})  Ctrl-C to stop", flush=True)

	fd = os.open(dev, os.O_RDONLY)
	try:
		while True:
			r, _, _ = select.select([fd], [], [])
			if not r:
				continue
			data = os.read(fd, EVENT_SIZE)
			if len(data) < EVENT_SIZE:
				continue
			_sec, _usec, typ, code, val = struct.unpack(EVENT_FMT, data)
			if typ == EV_KEY and code == KEY_WAKEUP:
				state = {0: "release", 1: "press", 2: "repeat"}.get(val, str(val))
				print(f"GPIOE1 KEY_WAKEUP {state} (val={val})", flush=True)
			elif typ == EV_KEY:
				print(f"other key code={code} val={val}", flush=True)
	except KeyboardInterrupt:
		print("stop", flush=True)
	finally:
		os.close(fd)
	return 0


if __name__ == "__main__":
	sys.exit(main())
