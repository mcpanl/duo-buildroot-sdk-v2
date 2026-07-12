# IMX678 MIPI 摄像头移植资料包

本目录整理自 SDK 配置 `rockchip_rk3566_taishanpi_1m_v10_minimal_media_defconfig`，用于将 **Sony IMX678** 从 RK3566 TaishanPi V10 移植到其他 SoC。

## MCLK 结论（直接回答）

**IMX678 使用的 MCLK（驱动中称 xvclk / INCK）为 37.125 MHz（37,125,000 Hz）。**

依据：

1. 驱动 `driver/imx678.c` 定义 `#define IMX678_XVCLK_FREQ_37M 37125000`，上电时 `clk_set_rate(xvclk, 37125000)`。
2. 4K 线性初始化表注释：`Xclk 37.125Mhz datarate 1188 12bit 4lane`。
3. 设备树 `device_tree/tspi-rk3566-csi-v10.dtsi` 注释：`xvclk 37.125 MHz (driver)`。
4. 模式寄存器 `0x3014 = 0x01` 对应 Sony 参考手册中 37.125 MHz INCK 选择。

RK3566 侧 MCLK 由 **CLK_CIF_OUT** 经 **GPIO4_PC0（cif_clkout）** 输出到模组；频率由驱动动态设置，而非 DT 写死。

---

## 本 defconfig 对应的构建链路

| 项 | 值 |
|----|-----|
| Board defconfig | `defconfig/rockchip_rk3566_taishanpi_1m_v10_minimal_media_defconfig` |
| 设备树 | `tspi-rk3566-media-linux.dts` → include `tspi-rk3566-csi-v10.dtsi` |
| 内核基础配置 | `rockchip_linux_defconfig`（含 `CONFIG_VIDEO_IMX678=y` 等） |
| 配置片段 | `kernel_config/tspi_media.config`（仅 fbdev 相关，**不含** IMX678） |

---

## 目录结构

```
imx678-mipi-porting/
├── README.md                          ← 本文件
├── PORTING_CHECKLIST.md               ← 移植步骤清单
├── defconfig/                         ← 板级 defconfig 原文
├── kernel_config/                     ← 内核 Kconfig 片段
├── device_tree/                       ← DTS/DTSI 与节点参考
├── driver/                            ← 传感器驱动及依赖头文件
└── init_sequences/                    ← 寄存器初始化表与时序说明
```

---

## 硬件连接摘要（RK3566 TaishanPi V10）

| 信号 | 配置 |
|------|------|
| I2C | `i2c4`，地址 `0x1a` |
| MIPI | 4-lane → `csi2_dphy0` → `rkisp_vir0`（full mode） |
| MCLK | `CLK_CIF_OUT` / `cif_clk` → GPIO4_PC0 |
| RESET | GPIO4_PB5，ACTIVE_LOW |
| PWDN | GPIO4_PB4，ACTIVE_HIGH |
| POWER | GPIO0_PB0，ACTIVE_HIGH |
| 供电 | avdd=3.3V, dovdd/dvdd=1.8V（模组请核对 dvdd 是否需 1.1–1.2V） |

---

## 移植到其他 SoC 时需替换的部分

1. **MCLK 时钟源**：提供可设 **37.125 MHz** 的时钟；DT 中 `clocks` + `clock-names = "xvclk"`。
2. **MCLK 引脚**：配置为 SoC 的 camera MCLK 输出脚（RK 系列多为 `cif_clkout` 类引脚）。
3. **I2C 总线**：保持 7-bit 地址 `0x1a`，400 kHz 通常足够。
4. **MIPI CSI**：4-lane D-PHY，link frequency 与模式表一致（当前默认半速率 **594 Mbps/lane**）。
5. **GPIO**：reset / pwdn / power 极性与上电时序见 `init_sequences/imx678_power_on_sequence.md`。
6. **Regulator**：`avdd`、`dovdd`、`dvdd` 三路；名称须与驱动 `imx678_supply_names[]` 一致。
7. **ISP/CSI 管线**：Rockchip 需 `csi2_dphy` + `rkisp`；其他厂商替换为对应 CSI + ISP 驱动，并保持 media graph endpoint 连接。
8. **内核配置**：启用 `CONFIG_VIDEO_IMX678` 及平台 CSI/ISP/PHY 选项（见 `kernel_config/rockchip_linux_defconfig.media.snippet`）。

---

## 相关源文件路径（SDK 内）

| 类型 | SDK 路径 |
|------|----------|
| 传感器驱动 | `kernel/drivers/media/i2c/imx678.c` |
| Kconfig | `kernel/drivers/media/i2c/Kconfig`（`VIDEO_IMX678`） |
| Makefile | `kernel/drivers/media/i2c/Makefile` |
| CSI 设备树 | `kernel/arch/arm64/boot/dts/rockchip/tspi-rk3566-csi-v10.dtsi` |
| 顶层 DTS | `kernel/arch/arm64/boot/dts/rockchip/tspi-rk3566-media-linux.dts` |
| MCLK 引脚 | `kernel/arch/arm64/boot/dts/rockchip/rk3568-pinctrl.dtsi`（`cif_clk`） |
| CRU 时钟 | `kernel/drivers/clk/rockchip/clk-rk3568.c`（`CLK_CIF_OUT`） |

---

## 调试提示

- 驱动版本日志：`driver version: 00.01.16`
- Chip ID 失败常见原因：MCLK 频率不对、上电时序不足、I2C 地址错误或供电不对
- 模块参数：`imx678_debug=1` 可打开详细日志
- 半速率 MIPI 测试宏 `IMX678_MIPI_TEST_HALF_LANE_RATE` 默认为 1；量产建议评估是否改回 0 以恢复 1188 Mbps/lane
