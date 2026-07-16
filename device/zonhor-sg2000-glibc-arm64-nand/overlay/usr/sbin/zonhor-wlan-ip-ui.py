#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Draw WiFi association / IP on JD9853 /dev/fb0 (172x320 RGB565).
"""

from __future__ import print_function

import argparse
import mmap
import os
import struct
import sys

WIDTH = 172
HEIGHT = 320
FBDEV = "/dev/fb0"
WPA_CONF = "/etc/wpa_supplicant.conf"

BLACK = 0x0000
WHITE = 0xFFFF
GREEN = 0x07E0
RED = 0xF800
YELLOW = 0xFFE0
CYAN = 0x07FF
GRAY = 0x8410

FONT = {
    "0": [0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00],
    "1": [0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00],
    "2": [0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00],
    "3": [0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00],
    "4": [0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00],
    "5": [0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00],
    "6": [0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00],
    "7": [0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00],
    "8": [0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00],
    "9": [0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00],
    ".": [0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00],
    "A": [0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00],
    "B": [0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00],
    "C": [0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00],
    "D": [0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00],
    "E": [0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00],
    "F": [0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00],
    "G": [0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00],
    "H": [0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00],
    "I": [0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00],
    "J": [0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00],
    "K": [0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00],
    "L": [0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00],
    "M": [0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00],
    "N": [0x66, 0x76, 0x7E, 0x6E, 0x66, 0x66, 0x66, 0x00],
    "O": [0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00],
    "P": [0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00],
    "Q": [0x3C, 0x66, 0x66, 0x66, 0x6E, 0x3C, 0x0E, 0x00],
    "R": [0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00],
    "S": [0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00],
    "T": [0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00],
    "U": [0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00],
    "V": [0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00],
    "W": [0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00],
    "X": [0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00],
    "Y": [0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00],
    "Z": [0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00],
    "-": [0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00],
    "_": [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00],
    "/": [0x02, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00],
    ":": [0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00],
}


class FrameBuffer(object):
    def __init__(self):
        self.fd = os.open(FBDEV, os.O_RDWR)
        self.size = WIDTH * HEIGHT * 2
        self.fb = mmap.mmap(
            self.fd, self.size, mmap.MAP_SHARED, mmap.PROT_WRITE | mmap.PROT_READ
        )

    def clear(self, color=BLACK):
        pixel = struct.pack("<H", color)
        self.fb.seek(0)
        self.fb.write(pixel * (WIDTH * HEIGHT))

    def pixel(self, x, y, color):
        if x < 0 or y < 0 or x >= WIDTH or y >= HEIGHT:
            return
        py = HEIGHT - 1 - y
        pos = (py * WIDTH + x) * 2
        self.fb.seek(pos)
        self.fb.write(struct.pack("<H", color))

    def text(self, x, y, text, color=WHITE, scale=1):
        for ch in text:
            if ch == " ":
                x += 8 * scale
                continue
            bitmap = FONT.get(ch)
            if not bitmap and "a" <= ch <= "z":
                bitmap = FONT.get(ch.upper())
            if not bitmap:
                x += 8 * scale
                continue
            for row in range(8):
                bits = bitmap[row]
                for col in range(8):
                    if bits & (1 << (7 - col)):
                        for sy in range(scale):
                            for sx in range(scale):
                                self.pixel(
                                    x + col * scale + sx,
                                    y + row * scale + sy,
                                    color,
                                )
            x += 8 * scale

    def text_center(self, y, text, color=WHITE, scale=1):
        width = len(text) * 8 * scale
        x = max(0, (WIDTH - width) // 2)
        self.text(x, y, text, color, scale)

    def close(self):
        try:
            self.fb.close()
        except Exception:
            pass
        try:
            os.close(self.fd)
        except Exception:
            pass


def fit_scale(text, prefer=2, margin=4):
    scale = prefer
    while scale > 1 and len(text) * 8 * scale > WIDTH - margin:
        scale -= 1
    return scale


def truncate_text(text, max_chars):
    if len(text) <= max_chars:
        return text
    if max_chars <= 1:
        return text[:max_chars]
    return text[: max_chars - 1] + "-"


def is_real_ip(ip):
    if not ip:
        return False
    if ip.startswith("169.254.") or ip.startswith("0."):
        return False
    return True


def read_conf_ssid(path=WPA_CONF):
    try:
        with open(path, "r") as f:
            for line in f:
                s = line.strip()
                if s.startswith("ssid="):
                    val = s.split("=", 1)[1].strip()
                    if val.startswith('"') and val.endswith('"'):
                        val = val[1:-1]
                    return val or None
    except OSError:
        pass
    return None


def flush_proxy():
    for p in (
        "/sys/devices/platform/zonhor-lcd-proxy/flush",
        "/sys/bus/platform/devices/zonhor-lcd-proxy/flush",
    ):
        try:
            with open(p, "w") as f:
                f.write("1\n")
            return
        except OSError:
            continue


def draw_wifi_screen(ip, ssid, state):
    fb = FrameBuffer()
    try:
        fb.clear(BLACK)
        fb.text_center(16, "WiFi", GRAY, 1)

        ssid_text = truncate_text(ssid or "unknown", 18)
        fb.text_center(44, ssid_text, CYAN, fit_scale(ssid_text, 1))

        if ip:
            ip_text = ip
            ip_color = GREEN
            status = "CONNECTED"
        elif state == "COMPLETED":
            ip_text = "Getting IP..."
            ip_color = YELLOW
            status = "CONNECTED"
        elif state == "FAILED":
            ip_text = "No WLAN"
            ip_color = RED
            status = "FAILED"
        else:
            ip_text = "Connecting..."
            ip_color = YELLOW
            status = state or "INIT"

        fb.text_center(120, ip_text, ip_color, fit_scale(ip_text, 2))
        fb.text_center(200, status, GRAY, 1)
    finally:
        fb.close()
    flush_proxy()


def main(argv=None):
    parser = argparse.ArgumentParser(description="Draw WiFi IP on /dev/fb0")
    parser.add_argument("--ip", default="", help="IPv4 address (empty if pending)")
    parser.add_argument("--ssid", default="", help="SSID override")
    parser.add_argument("--state", default="", help="wpa_supplicant wpa_state")
    args = parser.parse_args(argv)

    if not os.path.exists(FBDEV):
        return 0

    ssid = args.ssid or read_conf_ssid() or "WiFi"
    draw_wifi_screen(args.ip, ssid, args.state.upper())
    return 0


if __name__ == "__main__":
    sys.exit(main())
