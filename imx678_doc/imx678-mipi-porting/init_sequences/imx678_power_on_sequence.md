# IMX678 上电时序（来自 `imx678.c` → `__imx678_power_on()`）

## MCLK / XVCLK

| 项目 | 值 |
|------|-----|
| 驱动宏 | `IMX678_XVCLK_FREQ_37M = 37125000` |
| 频率 | **37.125 MHz** |
| DT 时钟名 | `clock-names = "xvclk"` |
| RK3566 时钟源 | `CLK_CIF_OUT`（CRU 可编程，驱动 `clk_set_rate()` 设 37.125 MHz） |
| MCLK 输出引脚 | `cif_clk` → GPIO4_PC0, func=cif_clkout |

## 上电步骤（非 thunderboot）

1. `pinctrl` 切到 `default`（含 `cif_clk` 引脚复用）
2. `power-gpios` → 高（若存在）
3. 延时 10–20 ms
4. `reset-gpios` → 低（释放复位，ACTIVE_LOW）
5. 延时 10–20 ms
6. `pwdn-gpios` → 高（退出掉电）
7. `clk_set_rate(xvclk, 37125000)` + `clk_prepare_enable(xvclk)`
8. 使能三路 regulator：`dvdd`, `dovdd`, `avdd`（顺序见 supply 数组）
9. 延时 20–30 ms 后再做 I2C 通信

## 下电步骤（`__imx678_power_off()`）

1. `pwdn-gpios` → 低
2. `reset-gpios` → 高
3. 关闭 `xvclk`
4. `pinctrl` → `sleep`（若有）
5. `power-gpios` → 低
6. 关闭 regulators

## 传感器 ID 校验

- 寄存器：`0x3022`（`IMX678_REG_CHIP_ID`）
- 期望值：`0x01`
