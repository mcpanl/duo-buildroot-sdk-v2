# IMX678 颜色异常：ISP Bayer 配错（RGGB vs BGGR）

## 症状

- **VI / VPSS-YUV / VPSS-RGB 三行预览一起偏色**（例如红↔蓝、蓝偏橙）
- 用 `sample_sensor_color_bench` 对比时，上/中/下三路颜色错误**一致**
- 仅改 VPSS RGB888 字节序**无法**解释三行同时出错

典型误判：以为是 VPSS `PIXEL_FORMAT_RGB_888` 实际内存为 B-G-R，在 `rgb888_rgb565.c` 里做 R/B 对调。  
在 Bayer 配错时，该 workaround **可能让预览“看起来正常”**，但：

1. **没有修根因** — ISP Demosaic 仍按错误 Bayer 解 Raw  
2. **只影响 RGB888 显示路径** — YUV / 编码链路颜色仍错  
3. **与 HW CSC 架构无关** — 双 VPSS（NV21 rotate + RGB888 CSC）本身是性能优化，应保留

## 根因

IMX678 硬件与 Linux 上游驱动输出 **SRGGB（RGGB）** Raw。本 SDK 中：

| 层级 | 正确值 | 错误配置（移植遗漏） |
|------|--------|----------------------|
| Sensor / MIPI | RGGB | — |
| VI `enBayerFormat` | `BAYER_FORMAT_RG` | ✓ 已配 |
| ISP `ISP_PUB_ATTR_S.enBayer` | `BAYER_RGGB` (3) | **落入 default `BAYER_BGGR` (0)** |

IMX675 在 `sample_common_sensor.c` 的 `SAMPLE_COMM_SNS_GetIspAttrBySns()` 里已列入 `BAYER_RGGB` 分支；**IMX678 移植时漏加**，导致 Demosaic R↔B 全局对调。

CviIspTool 调参 JSON 若含 `"ISP_PUB_ATTR_S.enBayer": 0`，在线加载后会**再次覆盖**为 BGGR，需同步改为 `3`。

## 正确修复

### 1. 代码（必须）

在 `cvi_mpi/sample/common/sample_common_sensor.c` 的 `BAYER_RGGB` 分支加入：

```c
case SONY_IMX678_MIPI_8M_30FPS_12BIT:
case SONY_IMX678_MIPI_2M_30FPS_12BIT:
case SONY_IMX678_MIPI_2M_30FPS_10BIT_BIN:  /* 若该枚举在本分支存在 */
```

与 IMX675 并列，**不要**留在 `default: BAYER_BGGR`。

### 2. 调参 JSON（使用 CviIspTool / PQ bin 时）

```json
"PATH": "ISP_PUB_ATTR_S.enBayer",
"VALUE": 3
```

`BAYER_BGGR=0`, `BAYER_GBRG=1`, `BAYER_GRBG=2`, `BAYER_RGGB=3`。

### 3. 撤销错误 workaround（必须）

`cvi_mpi/sample/sensor_lcd/rgb888_rgb565.c`（及 `sensor_lcd_hevc` 同名文件）应使用 **API 字面 R-G-B 顺序**：

```c
uint8_t R = src_row[x * 3 + 0];
uint8_t G = src_row[x * 3 + 1];
uint8_t B = src_row[x * 3 + 2];
```

**不要**再按 B-G-R 读取。Bayer 修对后，color bench 已验证 naive R-G-B 三路均正常。

## 如何区分两类问题

| 现象 | 更可能原因 |
|------|------------|
| VI + VPSS-YUV + VPSS-RGB **三行一起**红蓝错 | **ISP Bayer 配错** |
| 仅 VPSS-RGB 行 R/B 对调，YUV 行正常 | VPSS RGB888 字节序 / 枚举不符 |
| `pn_swap` 配错 | 花屏、同步失败等，**不会**单独造成全局 R↔B |

## 验证步骤

```bash
cp /mnt/system/usr/bin/sensor_cfg.ini.imx678_1080p_bin /mnt/data/sensor_cfg.ini
killall sample_sensor_lcd sample_sensor_color_bench 2>/dev/null
/mnt/data/sample_sensor_color_bench   # 或重新编译部署后的 sample
```

- 修复后：三行颜色应一致且正常  
- 再跑 `sample_sensor_lcd`：预览颜色应与 bench 一致，且 `rgb888_rgb565` perf 仍应远低于旧 CPU NV21 路径

## 相关文件

- `cvi_mpi/sample/common/sample_common_sensor.c` — ISP PubAttr Bayer  
- `cvi_mpi/sample/sensor_lcd/rgb888_rgb565.c` — 预览 RGB565 打包（勿用 B-G-R workaround）  
- `cvi_mpi/sample/sensor_color_bench/` — 三阶段颜色对比基准  
- `sg2000_imx678_*_tuned.json` — 调参 JSON 中的 `enBayer` 字段  

## 历史说明

2026-07 曾在 `display: speed up sensor LCD preview` 中引入 VPSS HW CSC + B-G-R 字节序 workaround，当时颜色“正常”实为 **Bayer 错误 + RGB 对调偶然抵消**。  
正确做法是 **只修 Bayer，保留 HW CSC 性能路径，去掉 B-G-R workaround**。
