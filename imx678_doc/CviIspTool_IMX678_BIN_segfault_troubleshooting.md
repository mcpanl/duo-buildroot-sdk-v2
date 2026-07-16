# CviIspTool / isp_tool_daemon：IMX678 2×2 Bin 模式 PQ 客户端连接后段错误排障记录

> 平台：Zonhor SG2000（CV181x）eMMC  
> 传感器：`SONY_IMX678_MIPI_2M_30FPS_10BIT_BIN`（1080p 硬件 2×2 融合）  
> 记录时间：2026-07  
> 状态：**已解决**（板端 `192.168.42.1` 验证通过）

---

## 1. 现象

### 1.1 失败场景

在 `/mnt/data/sensor_cfg.ini` 已配置为 IMX678 bin 模式，且 **VI 初始化问题已按前序修复**（`cfg_128M_imx678.json`、`-Bsymbolic` 等，见 `CviIspTool_IMX678_BIN_troubleshooting.md`）之后：

- `sample_sensor_lcd` / `sample_sensor_test` **正常出图**
- `./CviIspTool.sh 128M` 能完成 IMX678 初始化、VPSS 1920×1080、RTSP 启动
- **PQ 调参客户端连接后约 2 秒，`isp_tool_daemon` 段错误退出**
- `dmesg` **看不出有效信息**

典型日志（段错误前）：

```text
ViPipe:0,===IMX678 1080P30fps 10bit LINE(bin) Init OK!=== ...
VPSS init with src (1920, 1080) dst (1920, 1080).
rtsp://192.168.100.139:8554/stream0
CVI_RTSP_SERVICE_CreateFromJsonFile[./cfg_128M_imx678.json]
waiting for connect...
Segmentation fault
```

### 1.2 易误判信息

| 日志 | 说明 |
|------|------|
| `sensorName(0) mismatch, mwSns:678 != pqBinSns:2053` | 默认 PQ bin 为 GC2083，**不是段错误根因** |
| `pstFocusMpiAttr is NULL` | 无 AF 镜头，PQ 工具常见警告 |
| `Cannot open '/dev/cvi-vo'` | 本板无 VO，正常现象 |
| 段错误后相机异常 | 需执行 `zonhor-cam-recover`，否则可能引发更严重内核错误 |

### 1.3 与「VI init failed」的区别

| 项目 | VI init failed（前序问题） | 本次段错误 |
|------|--------------------------|------------|
| 发生时机 | `CVI_RTSP_SERVICE` 创建 / VI 初始化阶段 | RTSP 已启动，**客户端连上后** |
| 表现 | `vi init failed 0xffffffff` | `Segmentation fault`，`cvi_uv_read` 线程 |
| 根因层 | RTSP 配置 / 符号抢占 | **ISP daemon 二进制接收状态机** |
| 关联 IMX678 bin | 需要 offline + compress none | **无直接关系**，任意传感器用 PQ 工具均可触发 |

---

## 2. 调试过程

### 2.1 板端复现

```bash
zonhor-cam-recover
cd /mnt/system/usr/bin
./CviIspTool.sh 128M
# 等待 PQ 工具自动连接 → 段错误
```

### 2.2 gdb 定位线程

```bash
export LD_LIBRARY_PATH=/mnt/system/usr/bin/lib:/mnt/system/lib:/mnt/system/usr/lib:/lib/3rd
export CVI_RTSP_JSON=./cfg_128M_imx678.json
gdb ./isp_tool_daemon
# run → 崩溃线程名: cvi_uv_read
# 崩溃库: libcvi_ispd2.so
```

带调试符号的 `libcvi_ispd2.so` 回溯显示崩溃在：

`CVI_ISPD2_ES_HandleMessageBuffer()` → `CVI_ISPD2_ES_HandleSocketPacket()` → `CVI_ISPD2_ES_CB_SocketRead()`

**未进入** `CVI_ISPD2_CBFunc_GetTopInfo()`，说明 JSON 请求被错误地走进了 **binary 接收分支**。

### 2.3 strace 抓到触发包（关键）

```bash
strace -f -e trace=network,read,write,recvfrom ./isp_tool_daemon
```

首包内容（51 字节）：

```json
{"id":43,"jsonrpc":"2.0","method":"CVI_GetTopInfo"}
```

随后 `Thread cvi_uv_read received signal SIGSEGV`。

---

## 3. 根因分析

### 3.1 全局二进制状态 `tBinaryInData` 管理缺陷

ISP daemon（`libcvi_ispd2.so`）用全局 `TISPDeviceInfo.tBinaryInData` 跟踪 PQ 工具下发的 **大块二进制数据**（tuning bin、RAW replay 等）。存在三处问题：

| # | 问题 | 后果 |
|---|------|------|
| 1 | `CVI_ISPD2_InitialDaemonInfo()` **未初始化** `tBinaryInData` | 仅依赖 BSS 清零，状态机边界不清晰 |
| 2 | PQ 工具常开 **多条 TCP 连接**（一条 `PrepareBinaryData` 进 binary 模式，另一条发 JSON） | 全局 `tBinaryInData` 被连接 A 置位后，连接 B 的 JSON 仍走 binary 分支 |
| 3 | 客户端断开时 **未清理** `tBinaryInData`；`pu8Buffer` 可能指向该连接的 `pszRecvBuffer` | 断连 `free` 后，`memcpy` 写入悬空指针 → **SIGSEGV** |

### 3.2 为何 sample 正常而 CviIspTool 崩溃

`sample_sensor_lcd` 不启动 ISP daemon 的 JSON-RPC 服务（端口 5566），PQ 工具不会对其发 `CVI_GetTopInfo` 等命令，因此不会触发该路径。

### 3.3 与 IMX678 2×2 bin 的关系

IMX678 bin 模式只是让你更常跑 `CviIspTool.sh`；段错误来自 **isp-daemon2 协议层**，与 `stSnsSize`、VI-VPSS bind、compress mode 等传感器适配无关。

---

## 4. 解决方案

### 4.1 代码改动（`cvi_mpi/modules/isp/cv181x|cv180x/isp-daemon2/`）

| 文件 | 改动 |
|------|------|
| `src/cvi_ispd2.c` | 启动时 `CVI_ISPD2_InitialBinaryData(&tBinaryInData)` |
| `src/cvi_ispd2_event_server.c` | 收到数据若以 `{`/`[` 开头则强制 JSON 模式并重置 binary 状态；导出 `CVI_ISPD2_ES_OnClientDisconnect()` |
| `src/cvi_ispd2_event_server.h` | 声明 `CVI_ISPD2_ES_OnClientDisconnect()` |
| `src/cvi_ispd2_uv_dummy.c` | 读线程退出时调用 `CVI_ISPD2_ES_OnClientDisconnect()`，避免悬空 `pu8Buffer` |

核心逻辑摘要：

1. **初始化**：daemon 启动时显式清零 `tBinaryInData`
2. **JSON 优先**：binary 模式已 armed 但收到 JSON-RPC 文本时，重置 binary 状态并走 JSON 解析
3. **断连清理**：释放 `pszRecvBuffer` 前，若 `pu8Buffer` 指向该 buffer 则置空并 reset

### 4.2 编译与热更新

```bash
source build/envsetup_soc.sh
defconfig sg2000_zonhor_sg2000_glibc_arm64_emmc

# 若缺少 cvi_pqtool_json.h，需先生成
bash cvi_mpi/modules/isp/common/toolJsonGenerator/generate_toolJson.sh cv181x

make -C cvi_mpi/modules/isp/cv181x/isp-daemon2 -j$(nproc)

scp cvi_mpi/modules/isp/cv181x/isp-daemon2/build/libcvi_ispd2.so \
    root@192.168.42.1:/mnt/system/usr/lib/
```

> 仅更新脚本/json **不够**，必须部署新的 `libcvi_ispd2.so`。

### 4.3 板端验证

```bash
zonhor-cam-recover
cd /mnt/system/usr/bin
./CviIspTool.sh 128M
```

期望：

- PQ 工具连接后 **不再段错误**
- `CVI_GetTopInfo` 返回 `ViPipe/ViChn/VpssGrp/VpssChn`
- 可持续 `CVI_ISP_QueryExposureInfo` 等轮询

本地验证 `CVI_GetTopInfo` 示例响应：

```json
{ "jsonrpc": "2.0", "result": { "status": 0, "params": { "ViPipe": 0, "ViChn": 0, "VpssGrp": 0, "VpssChn": 0 } }, "id": 43 }
```

### 4.4 段错误后的恢复

```bash
zonhor-cam-recover reload
# 或
zonhor-cam-recover
```

未恢复就再次启动相机相关进程，可能导致更严重内核错误。

---

## 5. 经验与检查清单

排查「CviIspTool VI/出图正常，PQ 客户端一连就崩」时：

1. 确认 VI 初始化已按 `CviIspTool_IMX678_BIN_troubleshooting.md` 修复（否则先解决 VI 问题）
2. 用 **gdb** 看崩溃线程是否为 `cvi_uv_read` / `libcvi_ispd2.so`
3. 用 **strace** 看端口 5566 首包是否为 JSON（如 `CVI_GetTopInfo`）
4. 检查板端 `libcvi_ispd2.so` 是否为修复后版本（热更新或新固件）
5. 段错误后务必 **`zonhor-cam-recover`**
6. 勿将 `sensorName mismatch`、`AF NULL`、`cvi-vo` 当作段错误根因

---

## 6. 相关文档

- VI 初始化失败：`imx678_doc/CviIspTool_IMX678_BIN_troubleshooting.md`
- IMX678 移植总览：`cvi_mpi/component/isp/sensor/sg200x/sony_imx678/PORTING_NOTES_SG2000.md`
- IMX678 模式说明：`imx678_doc/readme.md`

---

## 7. 修订历史

| 日期 | 内容 |
|------|------|
| 2026-07 | 初版：PQ 客户端连接后段错误排障与 `tBinaryInData` 修复记录 |
