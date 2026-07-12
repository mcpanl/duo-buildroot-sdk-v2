# IMX678 移植检查清单

## 1. 硬件确认

- [ ] 确认模组 INCK 支持 **37.125 MHz**（Sony IMX678 标准速率之一）
- [ ] 确认 MIPI **4-lane** 走线
- [ ] 核对 dvdd 电压（本 DT 用 1.8V，部分模组规格为 1.1–1.2V）
- [ ] 核对 reset / pwdn / power GPIO 极性

## 2. 设备树

- [ ] 添加 `imx678@1a` 节点，`compatible = "sony,imx678"`
- [ ] 配置 `clocks` + `clock-names = "xvclk"`（37.125 MHz 可编程时钟）
- [ ] 配置 `reset-gpios` / `pwdn-gpios` / `power-gpios`
- [ ] 配置 `avdd-supply` / `dovdd-supply` / `dvdd-supply`
- [ ] 配置 `port/endpoint`，`data-lanes = <1 2 3 4>`
- [ ] 连接 CSI D-PHY / ISP 的 `remote-endpoint`（参考 `device_tree/tspi-rk3566-csi-v10.dtsi`）
- [ ] 使能 `csi2_dphy_hw`、`csi2_dphy0`、`rkisp`、`rkisp_vir0`（Rockchip）

参考文件：
- `device_tree/imx678-node-reference.dtsi`
- `device_tree/tspi-rk3566-csi-v10.dtsi`

## 3. 时钟 / 引脚

- [ ] SoC 侧提供 MCLK 输出（RK3566: `CLK_CIF_OUT` + `cif_clk` pinctrl）
- [ ] 验证 `clk_set_rate(xvclk, 37125000)` 后实测频率
- [ ] MCLK 在 reset 释放后、I2C 访问前稳定

参考：
- `device_tree/rk3568-pinctrl.cif_clk.snippet`
- `init_sequences/imx678_power_on_sequence.md`

## 4. 内核驱动

- [ ] 拷贝或合并 `driver/imx678.c`
- [ ] 拷贝依赖头文件 `cam-tb-setup.h`、`cam-sleep-wakeup.h`（若使用 thunderboot / sleep-wakeup）
- [ ] 在 `Kconfig` 添加 `VIDEO_IMX678`
- [ ] 在 `Makefile` 添加 `obj-$(CONFIG_VIDEO_IMX678) += imx678.o`
- [ ] 启用 `CONFIG_VIDEO_IMX678=y` 或 `=m`

参考：
- `driver/Kconfig.imx678.snippet`
- `driver/Makefile.imx678.snippet`
- `kernel_config/rockchip_linux_defconfig.media.snippet`

## 5. 平台 CSI / ISP（Rockchip）

- [ ] `CONFIG_PHY_ROCKCHIP_CSI2_DPHY`
- [ ] `CONFIG_VIDEO_ROCKCHIP_ISP` / `CONFIG_VIDEO_ROCKCHIP_RKISP1`
- [ ] `CONFIG_VIDEO_ROCKCHIP_CIF`（若走 CIF 路径）
- [ ] 确认 `mipi_csi2` / `rkcif` 拓扑与传感器路由一致

## 6. 初始化与模式

- [ ] 使用 `imx678_linear_12bit_3840x2160_1188M_regs.txt` 作为 4K 线性基准表
- [ ] 确认 `IMX678_MIPI_TEST_HALF_LANE_RATE` 是否符合目标链路速率
- [ ] 校验 chip id `0x3022 == 0x01`
- [ ] 开流：写寄存器表 →（可选 patch）→ `0x3000=0x00`

参考：
- `init_sequences/imx678_stream_sequence.md`
- `init_sequences/imx678_mipi_half_lane_patch.txt`

## 7. 验证命令（Linux）

```bash
# 查看传感器是否枚举
dmesg | grep -i imx678
media-ctl -p -d /dev/media0

# v4l2 子设备信息
v4l2-ctl -d /dev/v4l-subdevX --all
```

期望 dmesg 关键字：
- `detect imx678 lane 4`
- `default mode [linear_4K_12bit] 3840x2160`
- `Detected imx678 id 000001`

## 8. 常见问题

| 现象 | 可能原因 |
|------|----------|
| `chip id read failed (-5)` | MCLK 未输出 / 频率错误 / 上电时序不足 |
| `xvclk mismatched` | SoC 时钟树无法精确产生 37.125 MHz |
| 有 I2C 无图像 | MIPI lane 连接或 link frequency 不匹配 |
| 偏色 | Bayer order 与 ISP IQ 不匹配 |
