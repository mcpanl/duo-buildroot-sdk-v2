# CSI / ISP 管线参考（来自 tspi-rk3566-csi-v10.dtsi）

本配置使用 **full mode**：传感器 → csi2_dphy0 → rkisp_vir0（不经 mipi_csi2/rkcif）。

## 数据路径

```
IMX678 (4-lane MIPI)
    └─ endpoint imx678_out
         └─ remote → csi2_dphy0 port@0 dphy0_in
              └─ csi2_dphy0 port@1 dphy0_out
                   └─ remote → rkisp_vir0 isp0_in
```

## 需使能的 DT 节点

| 节点 | status |
|------|--------|
| combphy1_usq | okay |
| combphy2_psq | okay |
| csi2_dphy_hw | okay |
| csi2_dphy0 | okay |
| rkisp | okay |
| rkisp_mmu | okay |
| rkisp_vir0 | okay |
| rkcif_mmu | okay |
| mipi_csi2 | disabled（full mode 不用） |
| rkcif | disabled |
| csi2_dphy1/2 | disabled |

## dphy0_in endpoint

```dts
dphy0_in: endpoint@1 {
    reg = <1>;
    remote-endpoint = <&imx678_out>;
    data-lanes = <1 2 3 4>;
};
```

## 移植到其他 Rockchip SoC

- 替换 `combphy` / `csi2_dphy` / `rkisp` 节点名与 compatible
- 保持 **单传感器单 dphy 输入**（注释说明 dphy0 仅 full mode 接一路传感器）
- 非 Rockchip：用目标 SoC 的 MIPI CSI-2 host + ISP 驱动，保持 lane 数与 link rate 一致

完整拓扑见：`device_tree/tspi-rk3566-csi-v10.dtsi`
