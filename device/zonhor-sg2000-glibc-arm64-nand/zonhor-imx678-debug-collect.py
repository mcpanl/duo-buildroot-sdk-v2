#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Zonhor SG2000 (sg2000_zonhor) IMX678 diagnostic collector.

Run on the target device (as root recommended) to gather system, kernel,
I2C, MIPI, GPIO, and CVITEK media-pipeline information into one text file.

Usage:
  python3 zonhor-imx678-debug-collect.py
  python3 zonhor-imx678-debug-collect.py -o /tmp/my_diag.txt
  python3 zonhor-imx678-debug-collect.py --with-camera-app   # also snapshot camera-related processes

Best practice:
  1. Reproduce the IMX678 problem first (start your camera app / sample_vio).
  2. Run this script while the failure is visible.
  3. Send the generated .txt file back for analysis.
"""

from __future__ import print_function

import argparse
import datetime
import fcntl
import glob
import os
import platform
import re
import shutil
import socket
import struct
import subprocess
import sys
import time

SCRIPT_VERSION = "1.0"
BOARD = "sg2000_zonhor"
SENSOR = "SONY_IMX678"

# From device overlay sensor_cfg.ini / imx678_sensor_ctl.c
IMX678_I2C_BUS = 3
IMX678_I2C_ADDR = 0x1A
IMX678_CHIP_ID_REG = 0x3022
IMX678_CHIP_ID_EXPECT = 0x01

I2C_SLAVE = 0x0703
I2C_SLAVE_FORCE = 0x0706

SENSOR_CFG_PATHS = [
    "/mnt/system/usr/bin/sensor_cfg.ini",
    "/mnt/data/sensor_cfg.ini",
    "/mnt/system/sensor_cfg.ini.imx678",
    "/mnt/system/usr/bin/sensor_cfg.ini.imx678_1080p",
    "/mnt/system/usr/bin/sensor_cfg.ini.imx678_1080p_bin",
    "/mnt/system/usr/bin/sensor_cfg.ini.imx678_5m",
]

DMESG_KEYWORDS = re.compile(
    r"imx678|imx.?678|sony|sensor|snsr|mipi|vi\b|isp\b|cvi|cvitek|i2c|csi|cif|"
    r"combo|lane|raw|vpss|venc|vdec|clk|mclk|gpio|cam|reset|standby|xmsta|"
    r"chip.?id|mismatch|error|fail|timeout|underrun|overflow",
    re.IGNORECASE,
)

CVITEK_PROC_GLOBS = [
    "/proc/cvitek/*",
    "/proc/mipi-rx",
]

# Register names match Sony IMX678 / Linux upstream imx678 driver
IMX678_KEY_REGS = [
    (0x3000, "STANDBY"),
    (0x3002, "XMSTA (master mode start)"),
    (0x3014, "INCK_SEL"),
    (0x3015, "DATARATE_SEL"),
    (0x3018, "WINMODE"),
    (0x301A, "WDMODE"),
    (0x301B, "ADDMODE (0=all-pixel, 1=2x2 bin)"),
    (0x301C, "THIN_V_EN"),
    (0x301E, "VCMODE"),
    (0x3020, "HREVERSE"),
    (0x3021, "VREVERSE"),
    (0x3022, "ADBIT (0=10bit, 1=12bit; also used as probe ID)"),
    (0x3023, "MDBIT (MIPI data bit)"),
    (0x3028, "VMAX_LSB"),
    (0x3029, "VMAX_MID"),
    (0x302A, "VMAX_MSB"),
    (0x302C, "HMAX_LSB"),
    (0x302D, "HMAX_MSB"),
    (0x303C, "PIX_HST_LSB"),
    (0x303D, "PIX_HST_MSB"),
    (0x303E, "PIX_HWIDTH_LSB"),
    (0x303F, "PIX_HWIDTH_MSB"),
    (0x3044, "PIX_VST_LSB"),
    (0x3045, "PIX_VST_MSB"),
    (0x3046, "PIX_VWIDTH_LSB"),
    (0x3047, "PIX_VWIDTH_MSB"),
    (0x3050, "SHR0_LSB"),
    (0x3051, "SHR0_MID"),
    (0x3052, "SHR0_MSB"),
]


class Collector(object):
    def __init__(self, out_fp):
        self.out = out_fp
        self._section = 0

    def writeln(self, text=""):
        self.out.write(text + "\n")

    def section(self, title):
        self._section += 1
        bar = "=" * 78
        self.writeln()
        self.writeln(bar)
        self.writeln("[%02d] %s" % (self._section, title))
        self.writeln(bar)

    def subsection(self, title):
        self.writeln()
        self.writeln("--- %s ---" % title)

    def note(self, text):
        self.writeln("NOTE: %s" % text)

    def run_cmd(self, cmd, timeout=30):
        self.writeln("$ %s" % cmd)
        try:
            proc = subprocess.Popen(
                cmd,
                shell=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                universal_newlines=True,
            )
            out, _ = proc.communicate(timeout=timeout)
            if out:
                self.writeln(out.rstrip("\n"))
            self.writeln("(exit %d)" % proc.returncode)
            return proc.returncode, out
        except subprocess.TimeoutExpired:
            proc.kill()
            self.writeln("(timeout after %ds)" % timeout)
            return -1, ""
        except Exception as exc:
            self.writeln("(error: %s)" % exc)
            return -1, ""

    def read_file(self, path, max_bytes=512 * 1024):
        self.writeln(">>> %s" % path)
        if not os.path.exists(path):
            self.writeln("(missing)")
            return None
        try:
            if os.path.isdir(path):
                names = sorted(os.listdir(path))
                if not names:
                    self.writeln("(empty directory)")
                    return ""
                for name in names[:200]:
                    self.writeln(name)
                if len(names) > 200:
                    self.writeln("... (%d more entries)" % (len(names) - 200))
                return "\n".join(names)
            size = os.path.getsize(path)
            if size > max_bytes:
                self.writeln("(file size %d bytes, truncating to %d)" % (size, max_bytes))
            with open(path, "r", errors="replace") as fp:
                data = fp.read(max_bytes)
            self.writeln(data.rstrip("\n"))
            return data
        except PermissionError:
            self.writeln("(permission denied)")
        except Exception as exc:
            self.writeln("(read error: %s)" % exc)
        return None

    def glob_and_read(self, pattern):
        paths = sorted(glob.glob(pattern))
        if not paths:
            self.writeln("pattern %s -> (no matches)" % pattern)
            return
        for path in paths:
            self.read_file(path)

    def collect_meta(self):
        self.section("Collection metadata")
        self.writeln("script_version: %s" % SCRIPT_VERSION)
        self.writeln("board: %s" % BOARD)
        self.writeln("sensor: %s" % SENSOR)
        self.writeln("hostname: %s" % socket.gethostname())
        self.writeln("timestamp_local: %s" % datetime.datetime.now().isoformat(sep=" ", timespec="seconds"))
        self.writeln("timestamp_utc: %s" % datetime.datetime.utcnow().isoformat(sep=" ", timespec="seconds"))
        self.writeln("python: %s" % sys.version.replace("\n", " "))
        self.writeln("uid: %d  euid: %d" % (os.getuid(), os.geteuid()))
        self.writeln("cwd: %s" % os.getcwd())

    def collect_system(self):
        self.section("System / hardware summary")
        for cmd in [
            "uname -a",
            "cat /proc/version",
            "uptime",
            "cat /proc/cpuinfo",
            "cat /proc/meminfo",
            "free",
            "df -h",
            "mount",
            "lsblk 2>/dev/null || true",
            "cat /proc/cmdline",
            "cat /sys/firmware/devicetree/base/model 2>/dev/null | tr -d '\\0'",
        ]:
            self.subsection(cmd)
            self.run_cmd(cmd)

        if os.path.isfile("/usr/sbin/zonhor-firmware-banner"):
            self.subsection("zonhor-firmware-banner")
            self.run_cmd("/usr/sbin/zonhor-firmware-banner")

        if shutil.which("fw_printenv"):
            self.subsection("U-Boot environment (fw_printenv)")
            self.run_cmd("fw_printenv 2>/dev/null || true")

    def collect_modules(self):
        self.section("Kernel modules (camera / media related)")
        self.run_cmd("lsmod")
        self.subsection("Filtered lsmod")
        self.run_cmd(
            "lsmod | egrep -i 'cv181x|cvi|mipi|vi|vpss|snsr|isp|vo|rgn|vcodec|tpu|ive|jpeg' || true"
        )
        self.subsection("Module parameters")
        for mod in [
            "cv181x_vi", "cvi_mipi_rx", "snsr_i2c", "cv181x_vpss",
            "cv181x_vo", "cv181x_sys", "cv181x_base",
        ]:
            param_dir = "/sys/module/%s/parameters" % mod
            if os.path.isdir(param_dir):
                self.writeln(">>> %s" % param_dir)
                for name in sorted(os.listdir(param_dir)):
                    self.read_file(os.path.join(param_dir, name))

    def collect_dmesg(self):
        self.section("Kernel log (dmesg)")
        self.subsection("Full dmesg")
        self.run_cmd("dmesg", timeout=60)
        self.subsection("Filtered dmesg (imx678 / mipi / vi / i2c / sensor / error)")
        self.run_cmd(
            "dmesg | egrep -i 'imx678|imx.?678|sony|sensor|snsr|mipi|\\\\bvi\\\\b|isp|cvi|cvitek|i2c|csi|cif|combo|lane|raw|error|fail|timeout|mismatch|chip.?id' || true",
            timeout=60,
        )

        for log_path in ["/var/log/messages", "/var/log/kern.log", "/var/log/syslog"]:
            if os.path.isfile(log_path):
                self.subsection("Tail of %s" % log_path)
                self.run_cmd("tail -n 300 %s" % log_path)

    def collect_sensor_cfg(self):
        self.section("Sensor configuration files")
        for path in SENSOR_CFG_PATHS:
            self.read_file(path)

        self.subsection("libsns_imx678 on filesystem")
        self.run_cmd("ls -la /mnt/system/lib/libsns_imx678.* 2>/dev/null || true")
        self.run_cmd("find /mnt/system /usr /lib -name 'libsns_imx678*' 2>/dev/null || true")

        self.subsection("loadsystemko.sh")
        self.read_file("/mnt/system/ko/loadsystemko.sh")

    def collect_gpio(self):
        self.section("Camera GPIO / pinmux (Zonhor CAM1 guard)")
        if os.path.isfile("/usr/sbin/zonhor-cam-gpio-guard"):
            self.subsection("zonhor-cam-gpio-guard show")
            self.run_cmd("/usr/sbin/zonhor-cam-gpio-guard show")
        else:
            self.note("zonhor-cam-gpio-guard not found")

        self.subsection("GPIO chip summary")
        self.run_cmd("ls -la /sys/class/gpio/ 2>/dev/null || true")
        for label_pat in ["*cam*", "*mipi*", "*snsr*", "*reset*"]:
            self.run_cmd("grep -ril '%s' /sys/kernel/debug/gpio 2>/dev/null || true" % label_pat.replace("*", ""))

        self.subsection("Device-tree mipi_rx / snsr-reset")
        self.run_cmd(
            "find /sys/firmware/devicetree -name '*mipi*' -o -name '*snsr*' 2>/dev/null | head -50 || true"
        )

        # GPIOA2 is snsr-reset per DTS
        self.subsection("snsr-reset GPIO (GPIOA2) via sysfs if exported")
        self.run_cmd("cat /sys/kernel/debug/gpio 2>/dev/null | head -80 || true")

    def collect_i2c_sysfs(self):
        self.section("I2C bus topology")
        self.run_cmd("ls -la /dev/i2c-* 2>/dev/null || true")
        self.run_cmd("ls -la /sys/bus/i2c/devices/ 2>/dev/null || true")
        for bus in range(0, 6):
            dev_dir = "/sys/bus/i2c/devices"
            if not os.path.isdir(dev_dir):
                break
            self.subsection("i2c-%d devices" % bus)
            self.run_cmd(
                "ls -la /sys/bus/i2c/devices/*-%d 2>/dev/null; "
                "for d in /sys/bus/i2c/devices/i2c-%d/*; do "
                "[ -f \"$d/name\" ] && echo \"$(basename $d): $(cat $d/name 2>/dev/null)\"; "
                "done 2>/dev/null || true" % (bus, bus)
            )

        if shutil.which("i2cdetect"):
            for bus in [IMX678_I2C_BUS, 1, 2, 3, 4]:
                self.subsection("i2cdetect -y %d" % bus)
                self.run_cmd("i2cdetect -y %d 2>/dev/null || true" % bus)
        else:
            self.note("i2cdetect not installed; using Python bus scan for bus %d" % IMX678_I2C_BUS)
            self.i2c_bus_scan(IMX678_I2C_BUS)

    def i2c_bus_open(self, bus):
        path = "/dev/i2c-%d" % bus
        if not os.path.exists(path):
            return None, "device missing: %s" % path
        try:
            fd = os.open(path, os.O_RDWR)
            return fd, None
        except OSError as exc:
            return None, str(exc)

    def i2c_set_addr(self, fd, addr, force=True):
        cmd = I2C_SLAVE_FORCE if force else I2C_SLAVE
        fcntl.ioctl(fd, cmd, addr)

    def i2c_read_reg(self, bus, addr, reg, addr_bytes=2, data_bytes=1, force=True):
        fd, err = self.i2c_bus_open(bus)
        if fd is None:
            return None, err
        try:
            self.i2c_set_addr(fd, addr, force=force)
            buf = bytearray()
            if addr_bytes == 2:
                buf.append((reg >> 8) & 0xFF)
            buf.append(reg & 0xFF)
            os.write(fd, buf)
            raw = os.read(fd, data_bytes)
            if not raw:
                return None, "empty read"
            val = raw[0]
            if data_bytes > 1:
                val = int.from_bytes(raw, byteorder="big")
            return val, None
        except OSError as exc:
            return None, str(exc)
        finally:
            os.close(fd)

    def i2c_probe_addr(self, bus, addr):
        fd, err = self.i2c_bus_open(bus)
        if fd is None:
            return False, err
        try:
            self.i2c_set_addr(fd, addr, force=True)
            # SMBus quick write equivalent: zero-length write often fails on absent devices
            try:
                os.write(fd, b"")
                return True, None
            except OSError:
                return False, None
        finally:
            os.close(fd)

    def i2c_bus_scan(self, bus):
        self.subsection("Python I2C scan bus %d" % bus)
        found = []
        for addr in range(0x03, 0x78):
            ok, _ = self.i2c_probe_addr(bus, addr)
            if ok:
                found.append(addr)
        if found:
            self.writeln("Detected addresses: %s" % ", ".join("0x%02X" % a for a in found))
        else:
            self.writeln("No devices acknowledged on bus %d" % bus)

    def collect_imx678_regs(self):
        self.section("IMX678 direct I2C probe (bus=%d addr=0x%02X)" % (IMX678_I2C_BUS, IMX678_I2C_ADDR))

        val, err = self.i2c_read_reg(IMX678_I2C_BUS, IMX678_I2C_ADDR, IMX678_CHIP_ID_REG)
        if err:
            self.writeln("CHIP_ID read failed: %s" % err)
            self.note(
                "If I2C fails here, check bus_id in sensor_cfg.ini, power, reset GPIO, "
                "and whether another process holds the sensor."
            )
        else:
            match = (val & 0xFF) == IMX678_CHIP_ID_EXPECT
            self.writeln(
                "CHIP_ID reg 0x%04X = 0x%02X (expect 0x%02X) -> %s"
                % (IMX678_CHIP_ID_REG, val & 0xFF, IMX678_CHIP_ID_EXPECT, "OK" if match else "MISMATCH")
            )

        self.subsection("Key IMX678 registers")
        self.writeln("%-8s  %-6s  %s" % ("Address", "Value", "Name"))
        self.writeln("%-8s  %-6s  %s" % ("-" * 8, "-" * 6, "-" * 24))
        regs = {}
        for reg, name in IMX678_KEY_REGS:
            val, err = self.i2c_read_reg(IMX678_I2C_BUS, IMX678_I2C_ADDR, reg)
            if err:
                self.writeln("0x%04X    ERROR   %s  (%s)" % (reg, name, err))
            else:
                regs[reg] = val & 0xFF
                self.writeln("0x%04X    0x%02X    %s" % (reg, val & 0xFF, name))

        self.subsection("Decoded mode / window (crop vs 2x2 bin)")
        need = [0x3018, 0x301B, 0x3022, 0x303C, 0x303D, 0x303E, 0x303F,
                0x3044, 0x3045, 0x3046, 0x3047]
        if all(r in regs for r in need):
            addmode = regs[0x301B]
            winmode = regs[0x3018]
            adbit = regs[0x3022]
            pix_hst = regs[0x303C] | (regs[0x303D] << 8)
            pix_hw = regs[0x303E] | (regs[0x303F] << 8)
            pix_vst = regs[0x3044] | (regs[0x3045] << 8)
            pix_vw = regs[0x3046] | (regs[0x3047] << 8)
            self.writeln("ADDMODE=%d WINMODE=%d ADBIT=%d" % (addmode, winmode, adbit))
            self.writeln("PIX window: HST=%d HWIDTH=%d VST=%d VWIDTH=%d" % (
                pix_hst, pix_hw, pix_vst, pix_vw))
            if addmode == 1 and pix_hw == 3840 and pix_vw == 2160:
                self.writeln(
                    "Heuristic: 2x2 BINNING (expect MIPI ~1920x1080 RAW10, full FOV)"
                )
            elif pix_hw == 1920 and pix_vw == 1080 and addmode == 0:
                self.writeln(
                    "Heuristic: 1080p CENTER CROP (MIPI still ~3856x2180 RAW12)"
                )
            elif pix_hw == 2880 and pix_vw == 1620 and addmode == 0:
                self.writeln(
                    "Heuristic: 5MP CENTER CROP (MIPI still ~3856x2180 RAW12)"
                )
            else:
                self.writeln("Heuristic: unknown / custom window")
            self.writeln(
                "Note: check /proc/cvitek/mipi-rx for actual CSIBDG frame size "
                "(crop keeps 3856x2180; bin should drop to ~1920x1080)."
            )
        else:
            self.note("Skip decode: incomplete register read")

        if shutil.which("i2cget"):
            self.subsection("i2cget cross-check (if available)")
            for reg, name in [(IMX678_CHIP_ID_REG, "ADBIT/CHIP_ID"), (0x3000, "STANDBY"),
                              (0x3002, "XMSTA"), (0x301B, "ADDMODE")]:
                cmd = "i2cget -y %d 0x%02x 0x%04x b 2>/dev/null || true" % (
                    IMX678_I2C_BUS, IMX678_I2C_ADDR, reg,
                )
                self.run_cmd(cmd)

    def collect_cvitek_proc(self):
        self.section("CVITEK /proc and MIPI debug nodes")
        for pattern in CVITEK_PROC_GLOBS:
            self.subsection("glob %s" % pattern)
            self.glob_and_read(pattern)

        self.subsection("/proc filesystem entries containing vi/mipi/isp")
        self.run_cmd(
            "find /proc -maxdepth 2 \\( -name '*vi*' -o -name '*mipi*' -o -name '*isp*' -o -name '*cif*' \\) "
            "2>/dev/null | sort | head -80 || true"
        )

        for path in [
            "/sys/module/snsr_i2c/parameters/snsr_i2c_dbg",
            "/sys/module/cvi_mipi_rx",
            "/sys/module/cv181x_vi",
        ]:
            if os.path.exists(path):
                self.read_file(path)

    def collect_media_devices(self):
        self.section("Media / V4L2 devices")
        self.run_cmd("ls -la /dev/media* /dev/video* /dev/v4l-subdev* 2>/dev/null || true")
        if shutil.which("media-ctl"):
            self.subsection("media-ctl -p")
            self.run_cmd("media-ctl -p 2>/dev/null || true")
        if shutil.which("v4l2-ctl"):
            self.subsection("v4l2-ctl --all-devices")
            self.run_cmd("v4l2-ctl --all-devices 2>/dev/null || true")

    def collect_processes(self, with_camera_app):
        self.section("Processes")
        self.run_cmd("ps w")
        patterns = r"sample_vio|vi_|isp|camera|stream|encode|venc|cvitek|mpi|sns|sensor"
        self.subsection("Filtered camera-related processes")
        self.run_cmd("ps w | egrep -i '%s' | grep -v egrep || true" % patterns)

        if with_camera_app:
            self.note("Snapshot taken with --with-camera-app; ensure your failing app was running.")

    def collect_libs_and_bins(self):
        self.section("Binaries / libraries / samples")
        for pattern in [
            "/mnt/system/usr/bin/*vi*",
            "/mnt/system/usr/bin/sample_*",
            "/usr/bin/sample_*",
            "/mnt/system/lib/libcvi_*",
            "/mnt/system/lib/libisp*",
            "/mnt/system/lib/libsns_*",
        ]:
            paths = sorted(glob.glob(pattern))
            if paths:
                self.subsection(pattern)
                for path in paths[:80]:
                    try:
                        st = os.stat(path)
                        self.writeln("%s  size=%d" % (path, st.st_size))
                    except OSError:
                        self.writeln(path)

        self.subsection("ldconfig / library resolution for libsns_imx678")
        self.run_cmd("ldconfig -p 2>/dev/null | grep -i sns || true")

    def collect_devmem_pinmux(self):
        self.section("Pinmux / GPIO register snapshot (same addresses as zonhor-cam-gpio-guard)")
        regs = {
            "PINMUX_CAM_MCLK1": 0x0300100C,
            "PINMUX_CAM_PD1": 0x03001010,
            "GPIOA_DR": 0x03020000,
            "GPIOA_DDR": 0x03020004,
        }
        if os.geteuid() != 0:
            self.note("Not running as root; /dev/mem reads may fail.")
        if not os.path.exists("/dev/mem"):
            self.note("/dev/mem not available")
            return
        for name, addr in regs.items():
            val = self.devmem_read(addr)
            if val is None:
                self.writeln("%s (0x%08X): read failed" % (name, addr))
            else:
                self.writeln("%s (0x%08X) = 0x%08X" % (name, addr, val))

    def devmem_read(self, addr):
        try:
            import mmap
            page = addr & ~0xFFF
            off = addr & 0xFFF
            with open("/dev/mem", "rb", buffering=0) as fd:
                mm = mmap.mmap(fd.fileno(), 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=page)
                try:
                    return struct.unpack_from("<I", mm, off)[0]
                finally:
                    mm.close()
        except Exception:
            return None

    def collect_interrupts(self):
        self.section("Interrupts / clocks (if exposed)")
        self.run_cmd("cat /proc/interrupts | head -60")
        for path in glob.glob("/sys/kernel/debug/clk/*mipi*"):
            self.read_file(path)
        for path in glob.glob("/sys/kernel/debug/clk/*vi*"):
            self.read_file(path)
        for path in glob.glob("/sys/kernel/debug/clk/*cam*"):
            self.read_file(path)


def default_output_path():
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    return "/tmp/zonhor_imx678_diag_%s.txt" % ts


def parse_args():
    parser = argparse.ArgumentParser(
        description="Collect IMX678 / camera debug info on Zonhor SG2000 device.",
    )
    parser.add_argument(
        "-o", "--output",
        help="Output text file path (default: /tmp/zonhor_imx678_diag_<timestamp>.txt)",
    )
    parser.add_argument(
        "--with-camera-app",
        action="store_true",
        help="Hint in report that camera app was running during capture",
    )
    parser.add_argument(
        "--skip-i2c",
        action="store_true",
        help="Skip direct IMX678 I2C register reads (non-intrusive mode)",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    out_path = os.path.abspath(args.output or default_output_path())

    print("Zonhor IMX678 diagnostic collector v%s" % SCRIPT_VERSION)
    print("Writing to: %s" % out_path)

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "w", encoding="utf-8", errors="replace") as fp:
        col = Collector(fp)
        col.collect_meta()
        col.collect_system()
        col.collect_modules()
        col.collect_sensor_cfg()
        col.collect_gpio()
        col.collect_i2c_sysfs()
        if not args.skip_i2c:
            col.collect_imx678_regs()
        else:
            col.section("IMX678 I2C probe skipped (--skip-i2c)")
        col.collect_cvitek_proc()
        col.collect_media_devices()
        col.collect_devmem_pinmux()
        col.collect_interrupts()
        col.collect_libs_and_bins()
        col.collect_processes(args.with_camera_app)
        col.collect_dmesg()
        col.section("Collection complete")
        col.writeln("output_file: %s" % out_path)
        col.writeln("Done.")

    size = os.path.getsize(out_path)
    print("Done. %d bytes written." % size)
    print("Please send this file for analysis:")
    print("  %s" % out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
