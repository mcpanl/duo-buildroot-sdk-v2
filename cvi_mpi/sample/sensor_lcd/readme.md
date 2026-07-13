# sample_sensor_lcd

IMX678 camera preview on Zonhor JD9853 SPI LCD (`/dev/fb0`, 172x320 RGB565).

Pipeline: **VI -> VPSS (64-aligned scale + ROTATION_90 + ASPECT_RATIO_AUTO letterbox) -> NV21 to RGB565 -> framebuffer**.

VPSS channel size is **320x192** (not 320x172) because GDC rotation requires **64-pixel alignment** on both width and height. After `ROTATION_90` the effective portrait buffer is **192x320**; the LCD is **172x320**, so the CPU only center-crops 10px per side when blitting.

Hardware (VPSS): sensor resize, 90° rotate, aspect-ratio letterbox, black background.  
CPU: NV21→RGB565 1:1 (no scale/rotate), center-crop to panel, write `/dev/fb0`.

## Prerequisites

1. Place sensor config on the board:

```bash
cp /mnt/system/usr/bin/sensor_cfg.ini.imx678_5m /mnt/data/sensor_cfg.ini
```

2. Stop other programs using `/dev/fb0` (e.g. `screen_demo.py`, `zonhor-ota-ui`).

## Build

From SDK root (after MMF toolchain env is set):

```bash
make -C cvi_mpi/sample/sensor_lcd
```

Or build all samples:

```bash
make -C cvi_mpi sample
```

## One-shot build & deploy to board

Run inside `cvi_mpi/` (same layout as `sample_sensor_test` deploy):

```bash
cd cvi_mpi && rm -f component/isp/sensor/sg200x/sony_imx678/*.o lib/libsns_full.* sample/common/sample_common_sensor.o sample/sensor_lcd/*.o sample/sensor_lcd/sample_sensor_lcd && make -C component/isp/ all -j$(nproc) && make sample -j$(nproc) && scp sample/sensor_lcd/sample_sensor_lcd root@192.168.42.1:/mnt/system/usr/bin/ && scp lib/libsns_full.so lib/libsns_imx678.so root@192.168.42.1:/mnt/system/usr/lib
```

On the board:

```bash
cp /mnt/system/usr/bin/sensor_cfg.ini.imx678_5m /mnt/data/sensor_cfg.ini
killall screen_demo.py 2>/dev/null
sample_sensor_lcd
```


```bash
sample_sensor_lcd
```

Options:

| Flag | Description |
|------|-------------|
| `-m` | VPSS horizontal mirror |
| `-f` | VPSS vertical flip |
| `-h` | Help |

## Notes

- VPSS output must be 64-aligned; kernel log `requires 64 alignments` means fix `VPSS_OUT_H` (172→192).
- Framebuffer draw clears the full screen each frame and uses `fix.line_length` from ioctl.
- Chroma plane uses **UV (NV12) byte order** even when `fmt=19` (NV21) — SG2000 VPSS quirk; VU order swaps red/blue.
- `Cannot open '/dev/cvi-vo'` is expected on this board; this sample does not use VO.
- PQ bin sensor name mismatch warnings do not block preview (same as `sample_sensor_test`).
- First frame logs VPSS output width/height for verification.

## Expected behavior

- Portrait preview on 172x320 LCD
- Image rotated 90 degrees clockwise relative to sensor
- Aspect ratio preserved with black letterbox bars
