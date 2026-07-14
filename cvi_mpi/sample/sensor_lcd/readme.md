# sample_sensor_lcd

IMX678 camera preview on Zonhor JD9853 SPI LCD (`/dev/fb0`, 172x320 RGB565).

Pipeline: **VI (NV21) -> VPSS0 (scale + ROTATION_90 NV21) -> VPSS1 (HW CSC to Packed RGB888) -> CPU RGB888 to RGB565 -> framebuffer**.

VPSS0 channel size is **320x192** (not 320x172) because GDC rotation requires **64-pixel alignment** and **NV12/NV21 only** (`GDC_SUPPORT_FMT`). After `ROTATION_90` the portrait buffer is **192x320**; VPSS1 converts that to RGB888; the LCD is **172x320**, so the CPU center-crops 10px per side when blitting.

Hardware:
- VPSS0: resize, 90° rotate, aspect-ratio letterbox (NV21 — required by GDC)
- VPSS1: NV21→Packed RGB888 CSC (no rotation)

CPU: Packed RGB888→RGB565 1:1 (bit packing only), center-crop to panel, write `/dev/fb0`.

VB pools: [0] sensor, [1] VPSS0 NV21, [2] VPSS1 RGB888.

## Prerequisites

1. Place sensor config on the board:

```bash
cp /mnt/system/usr/bin/sensor_cfg.ini.imx678_5m /mnt/data/sensor_cfg.ini
```

2. Stop other programs using `/dev/fb0` (e.g. `screen_demo.py`, `zonhor-ota-ui`).

## Build

From SDK root (after MMF toolchain env is set):

```bash
make -C cvi_mpi/sample/sensor_lcd clean
make -C cvi_mpi/sample/sensor_lcd
```

Or build all samples:

```bash
make -C cvi_mpi sample
```

## One-shot build & deploy to board

Run inside `cvi_mpi/` (same layout as `sample_sensor_test` deploy):

```bash
cd cvi_mpi && rm -f component/isp/sensor/sg200x/sony_imx678/*.o lib/libsns_full.* sample/common/sample_common_sensor.o sample/sensor_lcd/*.o sample/sensor_lcd/*.d sample/sensor_lcd/sample_sensor_lcd && make -C component/isp/ all -j$(nproc) && make sample -j$(nproc) && scp sample/sensor_lcd/sample_sensor_lcd root@192.168.42.1:/mnt/system/usr/bin/ && scp lib/libsns_full.so lib/libsns_imx678.so root@192.168.42.1:/mnt/system/usr/lib
```

On the board:

```bash
cp /mnt/system/usr/bin/sensor_cfg.ini.imx678_5m /mnt/data/sensor_cfg.ini
killall screen_demo.py 2>/dev/null
sample_sensor_lcd
```

Options:

| Flag | Description |
|------|-------------|
| `-m` | VPSS horizontal mirror (applied on Grp0) |
| `-f` | VPSS vertical flip (applied on Grp0) |
| `-h` | Help |

## Notes

- Single-channel `ROTATION_90 + RGB888` fails with `0xc0068003` (`CVI_ERR_VPSS_ILLEGAL_PARAM`) because GDC rejects non-NV12/NV21 formats.
- Framebuffer draw clears the full screen each frame and uses `fix.line_length` from ioctl.
- VPSS1 output is Packed `PIXEL_FORMAT_RGB_888` (`fmt=0`); CPU only packs to RGB565.
- `Cannot open '/dev/cvi-vo'` is expected on this board; this sample does not use VO.
- First frame logs VPSS1 width/height/fmt (`fmt=0` = RGB_888).

## Expected behavior

- Portrait preview on 172x320 LCD
- Image rotated 90 degrees clockwise relative to sensor
- Aspect ratio preserved with black letterbox bars
- Perf: `rgb888_rgb565` should be far below the old ~87ms `nv21_rgb565` path
