# IMX678 @ SG2000 (Zonhor) 移植踩坑与方案总结

> 平台：CVITEK SG2000 / cv181x ISP，4-lane MIPI RAW12 @ 1188 Mbps/lane  
> 记录时间：2026-07  
> 状态：**1080p 中心裁剪模式已验证 RAW/YUV 正常出图**；原生 4K 模式 MIPI/SOF 正常但 ISP 后级管线未跑通。

---

## 1. 问题背景

在 Zonhor SG2000 板上移植 Sony IMX678（3840×2160，12-bit，4-lane）时：

- Sensor I2C、寄存器、MIPI 物理层、SOF 均正常
- **4K 模式**下 `/proc/cvitek/vi` 中 `IntCnt/RecvPic` 长期为 0，`vi_dbg` 显示 `VIPreFECh0Cnt` 极低、`VIPostCnt=0` → **ISP FE→Postraw 在 4K RAW12 规模下卡住**
- 同 SoC 上 IMX675（2560×1944）等传感器工作正常；**IMX678 是该 SDK 树中唯一的 3840×2160 传感器**

结论方向：**不是 lane order / PN swap / 单次 mipi-rx 快照问题**，而是 **SoC ISP/VI 对 8MP RAW12 全分辨率处理能力不足或配置未完备**。

---

## 2. 踩坑日志

### 2.1 诊断与误判

| 坑 | 说明 |
|----|------|
| 用单次 `/proc/cvitek/mipi-rx` 判断 MIPI 失败 | D0/CLK lane 为 0 在本 SoC 上可属正常；应 `watch` 多帧或结合 `vi_dbg` SOF |
| 怀疑 lane order / PN swap | 用户侧已验证硬件正确，非根因 |
| 把 `0x3050/0x3051` 当成输出宽度 | 实为 **SHR0（曝光）**；输出尺寸在 `0x303E/0x303F`（宽）、`0x3046/0x3047`（高） |
| I2C 读寄存器用 8-bit 地址 | IMX678 为 **16-bit 地址**，需 `i2ctransfer -y 3 w2@0x1a 0x30 0x00 r1` 读 `0x3000` |

### 2.2 驱动与出流

| 坑 | 说明 |
|----|------|
| 4K 表末尾 `XMSTA=0` 后直接出流 | CVITEK 上需：**表末 XMSTA=0 → 再写 XMSTA=1 → STANDBY=0 → 延时 80ms → XMSTA=0**（见 `imx678_init`） |
| `imx678_get_mode()` 写在宏定义之前 | 编译报 `IMX678_SENSOR_GET_CTX` 隐式声明；宏须先于 helper 函数 |
| `cmos_set_image_mode` 只支持 8M | 需按宽高选择 `IMX678_MODE_8M30` / `IMX678_MODE_2M30`，并在 `init` 前由 `SAMPLE_COMM_ISP_SetSensorMode` 设置 |

### 2.3 720p binning 方案（已放弃）

| 坑 | 说明 |
|----|------|
| 照搬 RK `imx678_linear_12bit_1284x720` 表 | RK 自家 `supported_modes` 里该模式也是**注释掉的**，实机未验证 |
| 与 4K 共用同一套 XMSTA 时序 | Binning 表在 RK 上为：表末 `XMSTA=0` 后**只清 STANDBY**，不应再拉回 XMSTA=1 |
| 现象 | 日志显示 2M，但 **mipi-rx 完全无数据**；寄存器宽高混搭（如宽 3856 + 高 2880） |

**结论**：720p binning 暂不作为 SG2000 可用路径，除非拿到 Sony 官方 mode table 并在实机逐项验证。

### 2.4 1080p 中心裁剪 + CSIBDG 尺寸（关键）

PIX 裁剪只改变传感器**有效像素窗口**，**不改变 MIPI 帧的 CSIBDG 报告尺寸**。

| 配置错误 | dmesg | 根因 |
|----------|-------|------|
| `stSnsSize = 1936×1088` | `frm width greater than setting(1936)` | MIPI 行宽仍为 **3856** |
| `stSnsSize = 3856×1088` | `frm height greater than setting(1088)` | MIPI 帧高仍为 **2180** |
| **正确** `stSnsSize = 3856×2180` + `stWndRect = (968,550,1920,1080)` | 无 CSIBDG 宽高告警，RAW/YUV 正常 | CSIBDG 按全帧收，ISP 按窗口裁 |

类比 4K 模式：`stSnsSize=3856×2180`，`stWndRect=(0,0,3840,2160)`。

### 2.5 其它

- 改 `sample_common_sensor.c` 后须**同时部署** `libsns_imx678.so` 与 `sample_sensor_test`
- 1080p 模式建议开启 `disEnableSbm`（与 8M 相同，见 `sample_common_sensor.c`）
- 枚举名 `SONY_IMX678_MIPI_2M_30FPS_12BIT` 保留兼容，实际语义为 **1080p 中心裁剪**

---

## 3. 最终解决方案（推荐生产配置）

### 3.1 模式定义

| 枚举 | 内部模式 | 用途 |
|------|----------|------|
| `SONY_IMX678_MIPI_8M_30FPS_12BIT` | `IMX678_MODE_8M30` | **5MP 中心裁剪** 2880×1620（SG2000 ISP 上限） |
| `SONY_IMX678_MIPI_2M_30FPS_12BIT` | `IMX678_MODE_2M30` | 1080p 中心裁剪（FOV 缩小，MIPI 仍 4K） |
| `SONY_IMX678_MIPI_2M_30FPS_10BIT_BIN` | `IMX678_MODE_2M30_BIN` | **1080p 2×2 硬件融合**（全 FOV，RAW10；见 §7） |

### 3.2 Sensor 侧

1. 始终加载已验证的 **4K 线性寄存器表** `imx678_linear_12bit_3840x2160_regs`
2. `IMX678_MODE_2M30` 时在 standby 阶段调用 `imx678_apply_1080p_crop()`：

   | 寄存器 | 值 | 含义 |
   |--------|-----|------|
   | `0x303C/0x303D` | `0x03C8` | PIX_HST = 968 |
   | `0x303E/0x303F` | `0x0780` | PIX_HWIDTH = 1920 |
   | `0x3044/0x3045` | `0x0226` | PIX_VST = 550 |
   | `0x3046/0x3047` | `0x0438` | PIX_VWIDTH = 1080 |

3. 出流顺序（与 4K 相同）：`STBY=1,XMSTA=1` → 写表 → `XMSTA=1` → `STBY=0` → 80ms → `XMSTA=0`

### 3.3 ISP / CSIBDG 侧（`imx678_cmos_param.h`）

```c
/* IMX678_MODE_2M30 — 1080P30 */
.stSnsSize  = { 3856, 2180 };   /* 必须等于 MIPI 实帧，不是 1920×1080 */
.stWndRect  = { 968, 550, 1920, 1080 };  /* ISP 裁切输出 */
```

MIPI 参数不变：RAW12、4-lane、1188 Mbps/lane、`RX_MAC_CLK_600M`。

### 3.4 设备配置

```bash
cp /mnt/system/usr/bin/sensor_cfg.ini.imx678_1080p /mnt/data/sensor_cfg.ini
# name = SONY_IMX678_MIPI_2M_30FPS_12BIT
sample_sensor_test
```

Init 日志期望：

```text
===IMX678 1080P30fps 12bit LINE(crop) Init OK!=== STBY=0x0 XMSTA=0x0 HMAX=1100 VMAX=2250 PIX=1920x1080
```

I2C 复核（16-bit 地址）：

```bash
i2ctransfer -y 3 w2@0x1a 0x30 0x3e r1   # 0x80
i2ctransfer -y 3 w2@0x1a 0x30 0x3f r1   # 0x07
i2ctransfer -y 3 w2@0x1a 0x30 0x46 r1   # 0x38
i2ctransfer -y 3 w2@0x1a 0x30 0x47 r1   # 0x04
```

### 3.5 相关文件

| 文件 | 作用 |
|------|------|
| `imx678_sensor_ctl.c` | 4K 表、`imx678_apply_1080p_crop()`、`imx678_init()` |
| `imx678_cmos_param.h` | 模式时序、`stSnsSize` / `stWndRect`、MIPI RX 属性 |
| `imx678_cmos.c` | `cmos_set_image_mode`、AE 与 `u8ImgMode` 联动 |
| `sensor_list.h/c` | `SONY_IMX678_MIPI_*` 枚举与字符串 |
| `sample_common_sensor.c` | PIC_1080P、`disEnableSbm`、sns obj |
| `device/.../sensor_cfg.ini.imx678_1080p` | 板级默认 sensor 名 |
| `device/.../zonhor-imx678-debug-collect.py` | 现场一键抓日志 |

---

## 4. 1080p 裁剪方案的局限

- **MIPI 带宽未降低**：线上仍是 3856×2180 RAW12 全帧，仅 ISP 输出 1920×1080
- **不能用来验证「降 MIPI 带宽能否打通 4K 管线」**；只能验证 **裁切后 ISP 处理路径** 是否正常
- Sensor 端有效感光区为 1920×1080，四周为裁掉区域（黑/无效），属预期行为

---

## 5. SoC 不支持 8MP 时的后续方向

### 5.1 短期（可落地）

1. **量产默认 1080p 裁剪模式**（当前已通）
   - `sensor_cfg.ini` → `SONY_IMX678_MIPI_2M_30FPS_12BIT`
   - 应用层按 1920×1080 接流；若需 16:9 显示可直接用

2. **保留 4K 枚举作调试**
   - 用于 MIPI/SOF/寄存器回归；不用于产品出图

3. **完善诊断脚本**
   - 16-bit I2C 读 `0x3000/0x3002/0x303E/0x3046`
   - 多帧采样 `mipi-rx`、`vi`、`vi_dbg`

### 5.2 中期（若必须更高分辨率）

| 方向 | 说明 | 难度 |
|------|------|------|
| **A. 查清 4K ISP 卡点** | 对比 IMX675/OS08A20 与 IMX678 在 `vi.c` / postraw / SBM / DMA 路径差异；抓 4K 失败时 `VIPreFECh0Cnt`、`VIPostCnt`、RDMA 状态 | 高，需芯原/方案商 |
| **B. Sony 官方 binning / 读数更低模式** | 如 1284×720、2560×1440 等，需**完整 mode table + 实测 MIPI 真降带宽**；勿再用未验证 RK 注释表 | 中 |
| **C. WINMODE 裁剪模式** | 参考 IMX335 `0x3018=0x04`；查 IMX678 手册是否支持 MIPI 输出随窗口缩小 | 中 |
| **D. 换传感器** | 同平台已验证 **IMX675 5M**、**OS08A20 8M（10bit）** 等；若项目可接受 5MP/10bit，风险最低 | 低（硬件变更） |

### 5.3 长期 / 架构

1. **向 CVITEK 确认 SG2000 8MP RAW12 规格**
   - 是否官方支持 3840×2160 @ 30fps RAW12 全 pipeline
   - 是否需要专用 `disEnableSbm`、FBC、tile 模式或更小 DDR 带宽配置

2. **若硬件固定 IMX678 + 要 4K 出图**
   - 路径1：SoC 侧 ISP/DMA 调通 8MP（依赖芯片厂）
   - 路径2：外挂 ISP / FPGA 做 RAW 缩放后再进 SoC
   - 路径3：Sensor 端真 binning 到 ≤5MP 且 MIPI 带宽下降（需 Sony setting）

3. **若 1080p 已满足产品**
   - 维持当前裁剪方案；优化 `stWndRect` 与 AE/AWB 标定
   - 评估中心裁剪与光学中心对齐（`PIX_HST/VST` 微调）

---

## 6. 快速对照表

| 现象 | 优先检查 |
|------|----------|
| mipi-rx 无数据 | `STBY/XMSTA`、是否用了未验证 binning 表、出流时序 |
| `width greater than setting` | `stSnsSize.width` 是否仍为 **3856** |
| `height greater than setting` | `stSnsSize.height` 是否仍为 **2180** |
| SOF 有、RecvPic=0（4K） | ISP 8MP 能力 / postraw，非 MIPI 物理层 |
| PIX=1920×1080 但 CSIBDG 报错 | `stSnsSize` 误设为 PIX 尺寸而非 MIPI 帧尺寸 |
| 显示 2M 但寄存器仍是 4K 宽高 | 新 `libsns_imx678.so` 未部署或 `u8ImgMode` 未切到 `2M30` |
| 5MP 录像 ~100KB、黑屏洋红条 | VPSS 预旋转宽 ≥2880；见 [IMX678_HEVC_record_resolution_SG2000.md](../../../../../../imx678_doc/IMX678_HEVC_record_resolution_SG2000.md) |

---

## 7. 1080p 2×2 硬件融合模式（并存）

与中心裁剪 1080p **并存**：新增枚举 `SONY_IMX678_MIPI_2M_30FPS_10BIT_BIN` /
内部 `IMX678_MODE_2M30_BIN`，不改动已验证的 crop 路径。

### 7.1 模式对比

| 项目 | `2M_30FPS_12BIT` (crop) | `2M_30FPS_10BIT_BIN` (bin) |
|------|-------------------------|----------------------------|
| 传感器 | PIX 中心裁 1920×1080 | ADDMODE=1，PIX=3840×2160 |
| FOV | 缩小（中心 1/4） | 全画幅 |
| MIPI 帧 | 仍 **3856×2180** RAW12 | 期望 **~1920×1080** RAW10 |
| ISP `stWndRect` | (968,550,1920,1080) | (0,0,1920,1080) |
| 灵敏度 | 单像素 | 2×2 融合，低光更好 |

### 7.2 关键寄存器（Linux 上游 `imx678_program_window`）

| 寄存器 | 值 | 含义 |
|--------|-----|------|
| `0x301B` ADDMODE | `0x01` | 2×2 binning |
| `0x3018` WINMODE | `0x04` | 窗口模式 |
| `0x303C/D` PIX_HST | `8` | 居中 (3856−3840)/2 |
| `0x303E/F` PIX_HWIDTH | `3840` | 融合前宽度 |
| `0x3044/5` PIX_VST | `8` | 居中 (2180−2160)/2 |
| `0x3046/7` PIX_VWIDTH | `2160` | 融合前高度 |
| `0x3022` ADBIT | `0x00` | 10-bit AD |
| `0x3023` MDBIT | `0x00` | 10-bit MIPI |

模式选择：sample 层 `ISP_PUB_ATTR.u8SnsMode=1` → `cmos_set_image_mode` 选 BIN。

### 7.3 板级启用

```bash
cp /mnt/system/usr/bin/sensor_cfg.ini.imx678_1080p_bin /mnt/data/sensor_cfg.ini
# 部署新 libsns_imx678.so / sample 后
sample_sensor_test   # 或 sample_sensor_lcd
```

**ISP 调参工具（CviIspTool）**：除 `cfg_*_imx678.json`（offline + compress none）外，还须部署带
`-Wl,-Bsymbolic` 的 `libcvi_rtsp_service.so`——否则运行时 `libsample.so` 会抢占
`SAMPLE_PLAT_VI_INIT`，导致 `vi init failed` 且无 dmesg。

```bash
cp /mnt/system/usr/bin/sensor_cfg.ini.imx678_1080p_bin /mnt/data/sensor_cfg.ini
cd /mnt/system/usr/bin
./CviIspTool.sh 128M
# 期望日志: IMX678: using ./cfg_128M_imx678.json
#           ===IMX678 1080P30fps 10bit LINE(bin) Init OK!===
```

回退裁剪模式：

```bash
cp /mnt/system/usr/bin/sensor_cfg.ini.imx678_1080p /mnt/data/sensor_cfg.ini
```

### 7.4 实机验证 checklist（Phase 1–3）

**Phase 1 — Sensor + MIPI**

- [ ] Init 日志含 `1080P30fps 10bit LINE(bin)` 且 `ADDMODE=0x1 ADBIT=0x0 PIX=3840x2160`
- [ ] I2C：`0x301B=0x01`，`0x303E/F=0x0F00`，`0x3046/7=0x0870`
- [ ] `/proc/cvitek/mipi-rx` 帧宽约 **1920**、高约 **1080**（非 3856×2180）
- [ ] 无 `frm width/height greater than setting`

若 MIPI 无数据：仅切换 `imx678_stream_on_binning()` → 改用 `imx678_stream_on_cvitek()`，勿改 ADDMODE/PIX。

**Phase 2 — CSIBDG / ISP**

- [ ] 若实测为 1936×1088 等 padding：改 `stSnsSize` 为实测值，`stWndRect` 保留 1920×1080
- [ ] `/proc/cvitek/vi`：`RecvPic > 0`，RAW/YUV 有帧

**Phase 3 — 画质**

- [ ] FOV 明显大于 crop 1080p（近似全景）
- [ ] 低光噪声应优于 crop；细节略软属预期
- [ ] 亮度/AE：融合增益约 +6dB，必要时收紧 again 或调 BLC

诊断：`python3 zonhor-imx678-debug-collect.py`（已解码 ADDMODE / PIX / crop-vs-bin）。

---

## 8. 修订历史

| 日期 | 内容 |
|------|------|
| 2026-07 | 初版：SG2000 移植踩坑、1080p 裁剪定稿、8MP 后续方向 |
| 2026-07 | 新增 1080p 2×2 硬件融合并存路径（`2M_30FPS_10BIT_BIN`） |
