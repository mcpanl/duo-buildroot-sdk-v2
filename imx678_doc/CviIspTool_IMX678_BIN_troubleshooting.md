# CviIspTool / isp_tool_daemon：IMX678 2×2 Bin 模式 `vi init failed` 排障记录

> 平台：Zonhor SG2000（CV181x）  
> 传感器：`SONY_IMX678_MIPI_2M_30FPS_10BIT_BIN`（1080p 硬件 2×2 融合）  
> 记录时间：2026-07  
> 状态：**已解决**（板端 `192.168.42.1` 验证通过）

---

## 1. 现象

### 1.1 失败场景

在 `/mnt/data/sensor_cfg.ini` 配置为 IMX678 bin 模式时：

- `sample_sensor_test`、`sample_sensor_lcd` **可正常出图**
- `./CviIspTool.sh 128M`（`isp_tool_daemon`）**VI 初始化失败**

典型日志：

```text
[parse_sensor_name] sensor = SONY_IMX678_MIPI_2M_30FPS_10BIT_BIN
...
vi-vpss-mode : 0          # 或最初为 1
compress_mode: 0          # 修复 cfg 后为 none
Cannot open '/dev/cvi-vo': 2, No such file or directory
family ID request : receive error
vi init failed. s32Ret: 0xffffffff !
init vi fail
CVI_RTSP_SERVICE_CreateFromJsonFile fail -1
```

`dmesg` 仅见短暂的 `vi_open` / `vi_release`，**无 ISP/sensor/MIPI 相关内核报错**。

### 1.2 易误判信息

| 日志 | 说明 |
|------|------|
| `Cannot open '/dev/cvi-vo'` | 本板无 VO 设备，**正常现象**；`sample_sensor_lcd` 同样出现且可出图 |
| `family ID request : receive error` | thermal netlink，与相机无关 |
| `vi init failed 0xffffffff` | 即 `CVI_FAILURE (-1)`，需结合用户态调用链定位 |

---

## 2. 对比：为何 sample 能工作而 CviIspTool 不能

| 项目 | `sample_sensor_lcd` / `sample_sensor_test` | `CviIspTool` / `isp_tool_daemon` |
|------|---------------------------------------------|-----------------------------------|
| 入口 | 直接 `SAMPLE_COMM_SYS_Init` → `SAMPLE_PLAT_VI_INIT` | `CVI_RTSP_SERVICE` → `init_vi()` → `SAMPLE_PLAT_VI_INIT` |
| VI-VPSS 模式 | 默认 offline-offline | 默认 `cfg_128M.json` 为 `vi-vpss-mode: 1`（offline-online） |
| VI 压缩 | `IniToViCfg` 默认 tile（sample 路径通常可跑通） | JSON 显式 `compress-mode: tile` |
| VPSS 模式设置 | **不调用** `CVI_SYS_SetVPSSModeEx` | `init_vi()` 在 VI init **之前**调用 `SetVPSSModeEx` |
| `SAMPLE_PLAT_VI_INIT` 来源 | `libsample.so` | `libcvi_rtsp_service.so` 内嵌一份 + 动态链接 `libsample.so`（**符号抢占**，见 §4.3） |

---

## 3. 调试过程（时间线）

### 3.1 第一轮：改 JSON / 脚本（部分必要，不足够）

**假设**：默认 RTSP 配置与 IMX678 bin（`disEnableSbm=1`）不兼容。

**改动**：

1. 新增 `cfg_128M_imx678.json` / `cfg_64M_imx678.json`  
   - `vi-vpss-mode: 0`（offline-offline）  
   - `compress-mode: none`  
   - `buf-blk-cnt: 5`
2. `CviIspTool.sh` 检测 `/mnt/data/sensor_cfg.ini` 含 `SONY_IMX678` 时自动选用上述 cfg
3. `vpss_helper.cpp` 中对 IMX678 强制 offline + none + VB 块数

**板端结果**：日志已显示 `IMX678: using ./cfg_128M_imx678.json`、`vi-vpss-mode: 0`，但 **仍 `vi init failed`**。

**结论**：配置问题是必要条件之一，但不是唯一根因。

### 3.2 第二轮：SSH 实机对比

在 `192.168.42.1` 上：

```bash
# sample 正常
/mnt/system/usr/bin/sample_sensor_lcd
# 期望: ===IMX678 1080P30fps 10bit LINE(bin) Init OK!===

# isp tool 仍失败
cd /mnt/system/usr/bin && ./CviIspTool.sh 128M
```

`strace` 显示：`vi_open` → 很快 `vi_release`，**未进入 sensor init 日志**（无 `ISP Vipipe Allocate`、无 `IMX678 ... Init OK`）。

**结论**：失败发生在 `SAMPLE_PLAT_VI_INIT` 内部或根本没走到正确实现。

### 3.3 第三轮：去掉 offline 路径的 `SetVPSSModeEx`

**发现**：`cvi_rtsp/service/vpss_helper.cpp` 的 `init_vi()` 在 `vi-vpss-mode=0` 时仍调用：

```c
CVI_SYS_SetVPSSModeEx(&stVPSSMode);  // VPSS_MODE_DUAL, VPSS_INPUT_MEM
```

而 `sample_sensor_lcd` **从不**在 VI init 前调用此接口。

**改动**：offline-offline 模式下 **跳过** `SetVPSSModeEx`（仅 online 模式保留 `SetVIVPSSMode` + `SetVPSSModeEx`）。

**板端结果**：仍失败（后续发现被符号抢占掩盖）。

### 3.4 第四轮：定位符号抢占（真正根因）

在 `libcvi_rtsp_service.so` 的 `SAMPLE_PLAT_VI_INIT` 中加入 `[PLAT_VI]` 调试打印并部署后：

- 日志里 **看不到** `[PLAT_VI] start ...`
- 但能看到 `init_vi: SYS_Init ok` 与 `vi init failed`

说明 `init_vi()` 调用的 **不是** `libcvi_rtsp_service.so` 内嵌的实现。

**原因**：

- `libcvi_rtsp_service.so` 编译时 **静态编入** `sample_common_platform.c` 等 sample 源码
- `isp_tool_daemon` 同时链接 `libsample.so`
- 运行时动态链接器将 `SAMPLE_PLAT_VI_INIT` **解析到 `libsample.so` 的旧符号**，内嵌修复代码从未执行

**验证**：`Makefile` 增加 `-Wl,-Bsymbolic` 后，日志出现完整 PLAT_VI 流程及：

```text
ViPipe:0,===IMX678 1080P30fps 10bit LINE(bin) Init OK!===
CVI_RTSP_SERVICE_CreateFromJsonFile[./cfg_128M_imx678.json]
rtsp://127.0.1.1:8554/stream0
```

---

## 4. 根因总结（三层）

### 4.1 配置层（第一层）

默认 `cfg_128M.json`：

- `vi-vpss-mode: 1`（VI offline + VPSS online）
- `compress-mode: tile`

IMX678（尤其 bin + `disEnableSbm=1`）在 SG2000 上与 sample 的 offline 路径不一致，易导致 VI 管线异常。

**修复**：`cfg_*_imx678.json` + 脚本自动选择 + `vpss_helper` IMX678 兜底。

### 4.2 初始化顺序（第二层）

`init_vi()` 在 offline 模式下不应在 `SAMPLE_PLAT_VI_INIT` 之前调用 `CVI_SYS_SetVPSSModeEx`。

**修复**：仅 online 模式设置 VI-VPSS / VPSS 模式。

### 4.3 动态链接符号抢占（第三层，关键）

| 符号 | 期望调用方 | 实际调用方（修复前） |
|------|-----------|---------------------|
| `SAMPLE_PLAT_VI_INIT` | `libcvi_rtsp_service.so` 内嵌版 | `libsample.so` 旧版 |

表现：只更新 JSON/脚本无效；`dmesg` 无实质错误；与 sensor 驱动无关。

**修复**：`cvi_rtsp/service/Makefile`：

```makefile
LDFLAGS += ... -Wl,-Bsymbolic
```

强制 `libcvi_rtsp_service.so` 使用自身符号，避免被 `libsample.so` 抢占。

---

## 5. 涉及文件与改动清单

| 文件 | 改动摘要 |
|------|----------|
| `cvi_mpi/modules/isp/cv181x/isp-tool-daemon/cfg_128M_imx678.json` | IMX678 专用 RTSP 配置 |
| `cvi_mpi/modules/isp/cv181x/isp-tool-daemon/cfg_64M_imx678.json` | 64M 版本 |
| `cvi_mpi/modules/isp/cv181x/isp-tool-daemon/CviIspTool.sh` | 自动选 cfg；启动前 kill 占用相机的进程 |
| `cvi_rtsp/service/vpss_helper.cpp` | IMX678 参数兜底；offline 跳过 `SetVPSSModeEx` |
| `cvi_rtsp/service/Makefile` | **`-Wl,-Bsymbolic`** |
| `tdl_sdk/sample_video/middleware_utils.c` | IMX678：`offline-offline`、VI compress `none`、跳过 `SetVPSSModeEx` |
| `device/.../overlay/mnt/system/usr/bin/camera-test.sh` | 启动前 kill 占用相机的进程 |
| `device/.../overlay/usr/sbin/zonhor-cam-recover` | kill 列表增加 `isp_tool_daemon` |

---

## 6. 部署与验证

### 6.1 传感器配置

```bash
cp /mnt/system/usr/bin/sensor_cfg.ini.imx678_1080p_bin /mnt/data/sensor_cfg.ini
```

### 6.2 快速热更新（项目根目录）

**CviIspTool** 必须包含 `libcvi_rtsp_service.so`（仅拷脚本/json 不够）：

```bash
scp cvi_rtsp/service/libcvi_rtsp_service.so root@192.168.42.1:/mnt/system/usr/lib/ && \
scp cvi_mpi/modules/isp/cv181x/isp-tool-daemon/{CviIspTool.sh,cfg_128M_imx678.json,cfg_64M_imx678.json} root@192.168.42.1:/mnt/system/usr/bin/ && \
ssh root@192.168.42.1 'chmod +x /mnt/system/usr/bin/CviIspTool.sh'
```

**camera-test.sh / sample_vi_fd** 需重新编译并部署 `sample_vi_fd`（`middleware_utils.c` 含 IMX678 修复）：

```bash
# 编译后从 tdl_sdk 安装目录拷贝，例如：
scp install/soc/*/bin/sample_vi_fd root@192.168.42.1:/mnt/system/usr/bin/ && \
scp device/zonhor-sg2000-glibc-arm64-emmc/overlay/mnt/system/usr/bin/camera-test.sh \
    root@192.168.42.1:/mnt/system/usr/bin/ && \
ssh root@192.168.42.1 'chmod +x /mnt/system/usr/bin/camera-test.sh'
```

### 6.3 正式固件

```bash
source build/envsetup_soc.sh
defconfig sg2000_zonhor_sg2000_glibc_arm64_emmc
build_cvi_rtsp          # 生成带 -Bsymbolic 的 libcvi_rtsp_service.so
build_pqtool_server     # 可选，更新 isp_tool_daemon 安装包
# 全量打包 rootfs / OTA
```

### 6.4 期望成功日志

```text
IMX678: using ./cfg_128M_imx678.json (vi-vpss-mode=0, compress-mode=none)
...
vi-vpss-mode : 0
compress_mode: 0
...
ViPipe:0,===IMX678 1080P30fps 10bit LINE(bin) Init OK!=== STBY=0 XMSTA=0 ...
CVI_RTSP_SERVICE_CreateFromJsonFile[./cfg_128M_imx678.json]
rtsp://127.0.1.1:8554/stream0
```

### 6.5 若仍失败

```bash
killall sample_sensor_lcd sample_sensor_test isp_tool_daemon 2>/dev/null
zonhor-cam-recover reload
cd /mnt/system/usr/bin && ./CviIspTool.sh 128M
```

---

## 7. 经验与检查清单

排查 `CviIspTool` VI 失败而 sample 正常时，建议按序检查：

1. **传感器 cfg** 是否为 `SONY_IMX678_MIPI_2M_30FPS_10BIT_BIN`
2. **RTSP json** 是否 `vi-vpss-mode=0`、`compress-mode=none`（或 `cfg_*_imx678.json`）
3. **`libcvi_rtsp_service.so` 是否已重新编译并部署**（含 `-Bsymbolic`）
4. 是否有其它进程占用 VI（先 `killall` 再启动）
5. 勿将 `family ID request` / `Cannot open cvi-vo` 当作根因

若 `strings libcvi_rtsp_service.so | grep IMX678` 有 IMX678 相关字符串，但运行无 `IMX678:` / `Init OK` 日志，优先怀疑 **符号抢占** 或未部署新 so。

---

## 8. 相关文档

- 传感器移植总览：`cvi_mpi/component/isp/sensor/sg200x/sony_imx678/PORTING_NOTES_SG2000.md`（§7 bin 模式）
- IMX678 模式说明：`imx678_doc/readme.md`
- 异构架构背景：`SG2000_ZONHOR_HETEROGENEOUS_ARCH.md`

---

## 9. 修订历史

| 日期 | 内容 |
|------|------|
| 2026-07 | 初版：CviIspTool IMX678 bin `vi init failed` 完整排障与修复记录 |
