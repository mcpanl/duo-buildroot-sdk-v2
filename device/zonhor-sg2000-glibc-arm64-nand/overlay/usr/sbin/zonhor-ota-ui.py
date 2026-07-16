#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Minimal OTA progress UI for JD9853 /dev/fb0 (172x320 RGB565).
Drawing helpers adapted from screen_demo.py (fixed portrait orientation).
"""

from __future__ import print_function

import argparse
import atexit
import mmap
import os
import signal
import struct
import sys
import time

WIDTH = 172
HEIGHT = 320
FBDEV = "/dev/fb0"

STATUS_PATH = "/run/zonhor-ota.status"
PID_PATH = "/run/zonhor-ota-ui.pid"
POLL_S = 0.2

BLACK = 0x0000
WHITE = 0xFFFF
GREEN = 0x07E0
RED = 0xF800
YELLOW = 0xFFE0
CYAN = 0x07FF
ORANGE = 0xFD20
GRAY = 0x8410
DARK = 0x2104

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
    "%": [0x62, 0x64, 0x08, 0x10, 0x26, 0x46, 0x00, 0x00],
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
    ">": [0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00],
    "<": [0x18, 0x30, 0x60, 0xC0, 0x60, 0x30, 0x18, 0x00],
}

STAGE_LABEL = {
    "CHECK": "CHECK",
    "UNPACK": "UNPACK",
    "VERIFY": "VERIFY",
    "CONVERT": "CONVERT",
    "FLASH_BOOT": "FLASH BOOT",
    "FLASH_ROOT": "FLASH ROOT",
    "FLASH_MISC": "FLASH LOGO",
    "SWITCH": "SWITCH",
    "SUCCESS": "SUCCESS",
    "ERROR": "FAILED",
    "STAGED": "STAGED",
}


class FrameBuffer(object):
    def __init__(self):
        self.fd = os.open(FBDEV, os.O_RDWR)
        self.size = WIDTH * HEIGHT * 2
        self.fb = mmap.mmap(
            self.fd, self.size, mmap.MAP_SHARED, mmap.PROT_WRITE | mmap.PROT_READ
        )
        self.logical_w = WIDTH
        self.logical_h = HEIGHT

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

    def fill_rect(self, x, y, w, h, color):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.pixel(xx, yy, color)

    def hline(self, x, y, w, color):
        for xx in range(x, x + w):
            self.pixel(xx, y, color)

    def vline(self, x, y, h, color):
        for yy in range(y, y + h):
            self.pixel(x, yy, color)

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
        x = max(0, (self.logical_w - width) // 2)
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


def fit_scale(text, logical_w, prefer=2, margin=4):
    scale = prefer
    while scale > 1 and len(text) * 8 * scale > logical_w - margin:
        scale -= 1
    return scale


def truncate_text(text, max_chars):
    if len(text) <= max_chars:
        return text
    if max_chars <= 1:
        return text[:max_chars]
    return text[: max_chars - 1] + "-"


def format_eta(eta_s):
    try:
        eta = int(eta_s)
    except (TypeError, ValueError):
        return "ETA --"
    if eta < 0:
        return "ETA --"
    if eta < 60:
        return "ETA %ds" % eta
    mins = eta // 60
    secs = eta % 60
    if mins >= 100:
        return "ETA %dm" % mins
    return "ETA %dm%02ds" % (mins, secs)


def read_status(path):
    data = {
        "stage": "CHECK",
        "pct": "0",
        "msg": "",
        "active": "?",
        "target": "?",
        "eta_s": "-1",
        "error": "",
    }
    try:
        with open(path, "r") as f:
            for raw in f:
                line = raw.strip()
                if not line or "=" not in line or line.startswith("#"):
                    continue
                key, val = line.split("=", 1)
                data[key.strip()] = val.strip()
    except OSError:
        pass
    return data


def write_status(path, **fields):
    cur = read_status(path)
    cur.update({k: str(v) for k, v in fields.items() if v is not None})
    tmp = path + ".tmp"
    lines = []
    for key in ("stage", "pct", "msg", "active", "target", "eta_s", "error"):
        if key in cur:
            lines.append("%s=%s" % (key, cur[key]))
    try:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    except TypeError:
        # Python 3.5 may lack exist_ok on some builds; ignore if present.
        d = os.path.dirname(path)
        if d and not os.path.isdir(d):
            os.makedirs(d)
    with open(tmp, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, path)


def draw_progress_bar(fb, x, y, w, h, pct, fill=CYAN, border=WHITE, bg=DARK):
    pct = max(0, min(100, int(pct)))
    fb.fill_rect(x, y, w, h, bg)
    fb.hline(x, y, w, border)
    fb.hline(x, y + h - 1, w, border)
    fb.vline(x, y, h, border)
    fb.vline(x + w - 1, y, h, border)
    inner_w = max(0, w - 4)
    fill_w = inner_w * pct // 100
    if fill_w > 0:
        fb.fill_rect(x + 2, y + 2, fill_w, h - 4, fill)


def draw_progress(fb, st):
    stage = (st.get("stage") or "CHECK").upper()
    try:
        pct = int(float(st.get("pct") or 0))
    except ValueError:
        pct = 0
    pct = max(0, min(100, pct))
    active = (st.get("active") or "?").upper()
    target = (st.get("target") or "?").upper()
    msg = st.get("msg") or ""
    error = st.get("error") or ""

    fb.clear(BLACK)

    if stage == "SUCCESS":
        fb.text_center(90, "UPDATE OK", GREEN, 2)
        fb.text_center(140, "REBOOTING...", CYAN, 1)
        fb.text_center(180, "%d%%" % pct, WHITE, 2)
        return

    if stage == "ERROR":
        fb.text_center(70, "FAILED", RED, 2)
        err = truncate_text(error or msg or "ERROR", 18)
        scale = fit_scale(err, WIDTH, 1)
        fb.text_center(120, err, ORANGE, scale)
        return

    if stage == "STAGED":
        fb.text_center(80, "STAGED", YELLOW, 2)
        fb.text_center(130, "REBOOT TO", WHITE, 1)
        fb.text_center(150, "APPLY", WHITE, 1)
        slot = "%s -> %s" % (active, target)
        fb.text_center(200, slot, CYAN, 1)
        return

    fb.text_center(12, "OTA UPDATE", GRAY, 1)

    slot = "%s -> %s" % (active, target)
    fb.text_center(36, slot, CYAN, fit_scale(slot, WIDTH, 2))

    label = STAGE_LABEL.get(stage, stage.replace("_", " "))
    fb.text_center(70, label, WHITE, fit_scale(label, WIDTH, 2))

    if msg:
        m = truncate_text(msg, 20)
        fb.text_center(96, m, GRAY, fit_scale(m, WIDTH, 1))

    bar_x, bar_y, bar_w, bar_h = 12, 130, WIDTH - 24, 18
    draw_progress_bar(fb, bar_x, bar_y, bar_w, bar_h, pct)

    fb.text_center(160, "%d%%" % pct, WHITE, 2)
    fb.text_center(200, format_eta(st.get("eta_s")), YELLOW, 1)


def ensure_run_dir():
    try:
        os.makedirs("/run", exist_ok=True)
    except TypeError:
        if not os.path.isdir("/run"):
            try:
                os.makedirs("/run")
            except OSError:
                pass


def read_pid():
    try:
        with open(PID_PATH, "r") as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def write_pid():
    ensure_run_dir()
    with open(PID_PATH, "w") as f:
        f.write("%d\n" % os.getpid())


def remove_pid():
    try:
        os.unlink(PID_PATH)
    except OSError:
        pass


def is_running(pid):
    if not pid:
        return False
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def cmd_stop(_args):
    pid = read_pid()
    if pid and is_running(pid):
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
        for _ in range(20):
            if not is_running(pid):
                break
            time.sleep(0.05)
        if is_running(pid):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
    remove_pid()
    return 0


def cmd_update(args):
    fields = {}
    if args.stage is not None:
        fields["stage"] = args.stage
    if args.pct is not None:
        fields["pct"] = args.pct
    if args.msg is not None:
        fields["msg"] = args.msg
    if args.active is not None:
        fields["active"] = args.active
    if args.target is not None:
        fields["target"] = args.target
    if args.eta_s is not None:
        fields["eta_s"] = args.eta_s
    if args.error is not None:
        fields["error"] = args.error
    write_status(args.status, **fields)
    return 0


def cmd_once(args):
    """Draw one frame from status (or STAGED defaults) and exit."""
    if not os.path.exists(FBDEV):
        return 0
    st = read_status(args.status)
    if args.stage:
        st["stage"] = args.stage
    if args.active:
        st["active"] = args.active
    if args.target:
        st["target"] = args.target
    if args.msg:
        st["msg"] = args.msg
    if args.pct is not None:
        st["pct"] = str(args.pct)
    if getattr(args, "error", None):
        st["error"] = args.error
    try:
        fb = FrameBuffer()
    except OSError:
        return 0
    try:
        draw_progress(fb, st)
        if args.hold > 0:
            time.sleep(args.hold)
    finally:
        fb.close()
    return 0


def cmd_start(args):
    if not os.path.exists(FBDEV):
        print("zonhor-ota-ui: %s missing, skip" % FBDEV, file=sys.stderr)
        return 0

    if not args.foreground:
        # Daemonize: parent exits, child continues.
        pid = os.fork()
        if pid > 0:
            return 0
        os.setsid()
        pid2 = os.fork()
        if pid2 > 0:
            os._exit(0)
        sys.stdin.close()
        try:
            sys.stdout.close()
            sys.stderr.close()
        except Exception:
            pass
        devnull = os.open(os.devnull, os.O_RDWR)
        os.dup2(devnull, 0)
        os.dup2(devnull, 1)
        os.dup2(devnull, 2)
        if devnull > 2:
            os.close(devnull)

    old = read_pid()
    if old and is_running(old) and old != os.getpid():
        try:
            os.kill(old, signal.SIGTERM)
        except OSError:
            pass
        time.sleep(0.1)

    ensure_run_dir()
    write_pid()
    atexit.register(remove_pid)

    stop = {"flag": False}

    def _stop(_signum, _frame):
        stop["flag"] = True

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    try:
        fb = FrameBuffer()
    except OSError:
        remove_pid()
        return 0

    last_sig = None
    try:
        write_status(
            args.status,
            stage="CHECK",
            pct=0,
            msg="",
            eta_s=-1,
            error="",
        )
        while not stop["flag"]:
            st = read_status(args.status)
            sig = (
                st.get("stage"),
                st.get("pct"),
                st.get("msg"),
                st.get("active"),
                st.get("target"),
                st.get("eta_s"),
                st.get("error"),
            )
            if sig != last_sig:
                draw_progress(fb, st)
                last_sig = sig
            time.sleep(POLL_S)
    finally:
        fb.close()
        remove_pid()
    return 0


def build_parser():
    p = argparse.ArgumentParser(description="Zonhor OTA framebuffer UI")
    p.add_argument("--status", default=STATUS_PATH, help="status file path")
    sub = p.add_subparsers(dest="cmd")

    sp = sub.add_parser("start", help="start UI daemon")
    sp.add_argument("--foreground", action="store_true")
    sp.set_defaults(func=cmd_start)

    sp = sub.add_parser("stop", help="stop UI daemon")
    sp.set_defaults(func=cmd_stop)

    sp = sub.add_parser("update", help="update status fields")
    sp.add_argument("--stage")
    sp.add_argument("--pct", type=int)
    sp.add_argument("--msg")
    sp.add_argument("--active")
    sp.add_argument("--target")
    sp.add_argument("--eta-s", dest="eta_s", type=int)
    sp.add_argument("--error")
    sp.set_defaults(func=cmd_update)

    sp = sub.add_parser("once", help="draw one frame and exit")
    sp.add_argument("--stage")
    sp.add_argument("--pct", type=int)
    sp.add_argument("--msg")
    sp.add_argument("--active")
    sp.add_argument("--target")
    sp.add_argument("--error")
    sp.add_argument("--hold", type=float, default=0.0)
    sp.set_defaults(func=cmd_once)

    return p


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if not args.cmd:
        parser.print_help()
        return 1
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
