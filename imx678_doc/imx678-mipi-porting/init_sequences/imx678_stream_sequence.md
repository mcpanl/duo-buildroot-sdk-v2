# IMX678 开流 / 关流序列

## 默认工作模式（当前 SDK 启用）

| 参数 | 值 |
|------|-----|
| 模式名 | `linear_4K_12bit` |
| 分辨率 | 3840 × 2160 |
| HDR | NO_HDR（线性） |
| 位深 | 12-bit RAW (SRGGB12) |
| MIPI lane | 4 |
| INCK (MCLK) | 37.125 MHz |
| MIPI link freq | 594 MHz（半速率测试，`IMX678_MIPI_TEST_HALF_LANE_RATE=1`） |
| 帧率 | ~20 fps（半速率时） |
| I2C 地址 | 0x1a |

> 若将 `IMX678_MIPI_TEST_HALF_LANE_RATE` 设为 0，则为 1188 Mbps/lane、~30 fps。

## `__imx678_start_stream()` 流程

1. 写入模式寄存器表 `imx678_linear_12bit_3840x2160_1188M_regs[]`（见 `init_sequences/`）
2. （可选）若半速率测试开启，追加 `imx678_mipi_half_lane_patch` 寄存器
3. 设置 `streaming = true`
4. `__v4l2_ctrl_handler_setup()` 应用曝光/增益等控制项
5. 线性模式：读回并初始化 gain/exposure
6. 写 `0x3000 = 0x00`（`IMX678_MODE_STREAMING`）开始输出

## `__imx678_stop_stream()` 流程

1. `streaming = false`
2. 写 `0x3000 = 0x01`（`IMX678_MODE_SW_STANDBY`）进入待机

## 关键模式寄存器（4K 线性 1188M 表头）

| 寄存器 | 值 | 含义 |
|--------|-----|------|
| 0x3014 | 0x01 | INCK_SEL — 选择 37.125 MHz 输入 |
| 0x3015 | 0x04 | DATARATE_SEL — 1188 Mbps/lane |
| 0x3022 | 0x01 | ADBIT 12-bit |
| 0x3023 | 0x01 | MDBIT |
| 0x3260 | 0x01 | DOL_PATH — 非 HDR 路径 |
| 0x3040 | 0x03 | LANEMODE — 4-lane |

完整表见：`imx678_linear_12bit_3840x2160_1188M_regs.txt`
