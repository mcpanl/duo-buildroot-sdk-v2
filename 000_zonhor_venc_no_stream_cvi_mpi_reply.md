# Zonhor VENC 不出流：基于 `cvi_mpi` 源码的回复结论

> 用途：给 `cvi_mpi` 使用者解释为什么会出现“`CVI_VENC_CreateChn` 成功、`StartRecvFrame` 成功，但 `GetStream` 长期返回 `0xc0078012`、输出文件 0 字节”的现象，以及应优先怎么修。
>
> 结论依据：本仓库内 `cvi_mpi` 用户态、`osdrv/interdrv/cvi_vc_drv` VENC 驱动、`osdrv/interdrv/vpss`、以及官方 sample。

---

## 1. 结论先说

结合你新补充的运行态统计，结论需要前移一层：

1. **当前最上游的主因不是 VENC 取流，而是 `VPSS G2(Dev1)` 根本没有把链路跑起来。**  
   你补充的统计已经非常明确：

   - `VENC EncodedFrame=0 / SendFramePerSec=0 / PicAddr=0 / PixFmt=N/A`
   - `VPSS G2 RecvCnt≈1202，但 StartFailCnt=1202，SendOK 全 0`
   - `VPSS G3 RecvCnt=0`
   - `Dev1 UserTrigCnt=0 / IrqCnt=0`

   这组现象串起来说明的不是“VENC 收到帧但编码失败”，而是：

   ```text
   G2(Dev1) StartFail
   -> G2 没有成功产出下游帧
   -> G3(SUB_VENC) 根本无帧
   -> VENC 从未收到输入
   -> GetStream 最终表现为 0xc0078012
   ```

   所以现在应把 `GetStream == BUSY` 视为**表象**，而不是第一落点。

2. **`GetStream == BUSY` 在 `cvi_mpi` 中依然表示“还没等到编码完成信号”或“底层编码/取流超时”，不是 `BUF_EMPTY`。**  
   这和新统计并不冲突，反而是互相印证的：

   - 当上游根本没有把帧送到 VENC 时，VENC 就只会空转等待
   - 这时应用层最常见看到的就是 `CVI_ERR_VENC_BUSY`

3. **软件送编路径里的 `ReleaseChnFrame` 时序仍然是一个真实风险点，但它不再是当前第一优先级。**  
   也就是说：

   - 如果 `G2/G3` 现在本来就没有帧，那么先改 `SendFrame -> GetStream -> ReleaseStream -> ReleaseChnFrame` 也未必立刻出流
   - 但等你把 `G2(Dev1)` 启动问题修掉之后，这个时序问题仍然必须修，否则会在下一阶段继续卡住

换句话说，**这次问题已经可以拆成“两层根因”**：

- 第一层：`G2(Dev1)` 启动失败，导致 `G3` 无帧、VENC 从未收到输入
- 第二层：即便后续有帧，当前软件送编时序也仍有把输入帧过早释放的风险

### 1.1 最新板端日志（再次确认：还没到 VENC）

你这次贴的用户态 + `dmesg` 进一步证明：**现在连 `GetChnFrame` 都拿不到帧，问题仍卡在 VPSS，而不是编码。**

用户态：

| 日志 | 含义 |
|------|------|
| `SUB_VENC GetChnFrame failed: 0xc006800e` | `CVI_ERR_VPSS_BUF_EMPTY`：G3-Ch0 输出队列空 |
| `ZONHOR_MMF_CamGetFrame failed: 0xc006800e` | MAIN_RGB（G2 用户通道）同样空 → **G2 整体没出帧**，不只是 SUB_VENC |
| `[zonhor_venc] open ... H264 540x960` | VENC 通道已创建，但上游无帧时它帮不上忙 |

内核：

| 日志 | 含义 |
|------|------|
| `base_get_chn_buffer: Mod(VPSS) Grp(3) Chn(0), jobs wait(0) work(0) done(0)` | G3-Ch0 的 job 队列全空，没有任何可取帧 |
| `vpss_get_chn_frame: Grp(3) Chn(0) get chn frame fail ... ret=-1` | 与上面用户态 `0xc006800e` 一一对应 |
| `base_mod_jobs_init: mod(VENC) job init fail, already inited` + `close/open channel no. 0` | VENC 残留未清干净就二次打开；应先杀进程/重启后再测，避免干扰判断 |
| `CHECK_VPSS_CHN_VALID: Grp(N) Chn(M) invalid for VPSS-Dual(...)` | Dual 模式下通道数受限时的探测噪音，**不是**本次主因 |
| `isp_fbc_info_debug` 持续刷 | ISP/VI 仍在跑；坏点在 **G0 之后的 Dev1(G2/G3)** |

驱动侧 Dual 模式硬限制（`vpss_create_grp`）：

```c
chnNum = (SINGLE) ? MAX
       : (Dev0)   ? 1              /* Dev0 只允许 Ch0 */
       :            MAX - 1;       /* Dev1 通常 Ch0~Ch2，Ch3 非法 */
```

因此 `Grp(0) Chn(1/2/3) invalid`、`Grp(2/3) Chn(3) invalid` 在 Dual 下是预期现象；真正要盯的仍是 **G2 `StartFailCnt` / Dev1 不触发 / fill buffer / apply hw settings**。

**对这次“还是不行”的直接答复：**  
当前失败点是 `GetChnFrame → BUF_EMPTY`，不是 `GetStream → BUSY`。在 `CamGetFrame` 成功之前，改 VENC `Release` 时序或 `u32BufSize` 都不会让文件开始变大。

下一步必须从 `dmesg` 里把 G2 每次失败的**具体分支**打出来（见 §10）。

---

## 2. 错误码精确语义

### 2.1 `0xc0078012` 是什么

在 `cvi_mpi/include/linux/cvi_comm_venc.h` 中：

- `CVI_ERR_VENC_BUF_EMPTY`
- `CVI_ERR_VENC_BUSY`

是分别定义的，`BUSY` 的注释是：

```c
CVI_ERR_VENC_BUSY, /* Device in use. */
```

用户态 `cvi_mpi/modules/venc/src/cvi_venc.c` **并不自己解释这个错误**，只是把内核 ioctl 的返回值原样返回给应用。

真正的语义要看驱动 `osdrv/interdrv/cvi_vc_drv/module/src/cvi_venc.c` 和 `enc_ctx.c`。

### 2.2 `GetStream` 在什么情况下返回 `BUSY`

从驱动看，`CVI_VENC_GetStream` 返回 `BUSY` 的典型来源有两类：

1. **bind 模式下等待信号量超时**

   `CVI_VENC_GetStream()` 在 bind 模式会先等 `sem_send`：

   - 等到了，说明内部 bind 线程已经把一帧编码完成并准备好 stream。
   - 没等到，就返回 `CVI_ERR_VENC_BUSY`。

   也就是说，bind 模式下的 `BUSY` 更接近：

   > “编码完成信号还没到 / 这次调用超时了”

   而不是：

   > “stream 队列里明确有个空包”

2. **non-bind 模式下编码器内部 `ENC_TIMEOUT`**

   `enc_ctx.c` 里的 H.264/H.265 `getStream()`、`encOnePic()`，都会把底层 `ENC_TIMEOUT` 映射成 `CVI_ERR_VENC_BUSY`。

   所以 non-bind 路径下的 `BUSY` 更接近：

   > “这一帧编码或取流超时，还没拿到有效输出”

### 2.3 `BUSY` 和 `BUF_EMPTY` 不是一回事

`BUF_EMPTY` 更像“当前没有可取的 VPSS/VI 输出 buffer”；  
`BUSY` 更像“这个模块还没准备好让你完成当前这一步”。

对本问题来说：

- `VENC GetStream = BUSY`：更说明**编码完成/取流阶段没有走通**，或者**根本没有拿到可编码输入**
- `VPSS GetChnFrame = BUF_EMPTY`：更说明**VPSS 输出队列当前没给到用户帧**

两者一起出现时，通常意味着**前级供帧和后级编码都没有形成稳定闭环**。

### 2.4 `0xc006800e` 是什么

在 `cvi_mpi/include/linux/cvi_errno.h` 中：

- `EN_ERR_BUF_EMPTY = 14`
- `CVI_ERR_VPSS_BUF_EMPTY = CVI_DEF_ERR(CVI_ID_VPSS, ..., EN_ERR_BUF_EMPTY)`

因此 `0xc006800e` 对应的是 **`CVI_ERR_VPSS_BUF_EMPTY`**，不是 VPSS `BUSY`。

这和 `osdrv/interdrv/vpss/common/vpss.c` 里的 `vpss_get_chn_frame()` 行为也一致：

- `base_get_chn_buffer()` 取不到 buffer
- 或拿到的 vb 已被释放
- 都返回 `CVI_ERR_VPSS_BUF_EMPTY`

所以你们预览路径里看到的 `0xc006800e`，源码层面的含义就是：

> 这一时刻 VPSS 用户取帧队列里没有可给你的输出帧

---

## 3. 官方 sample 给出的正确调用顺序

## 3.1 硬件 bind 路径

`cvi_mpi/sample/common/sample_common_venc.c` 的 `SAMPLE_COMM_VENC_Start()` 顺序是：

```text
CreateChn
->（如果 bind_mode == VENC_BIND_VPSS）SAMPLE_COMM_VPSS_Bind_VENC
-> StartRecvFrame
```

也就是 sample 习惯上采用：

> **先 Bind，再 StartRecvFrame**

这不是说反过来一定绝对不行，但如果要和官方路径保持一致，建议按这个顺序。

bind 成功后，sample 的取流线程只做：

```text
QueryStatus -> GetStream -> 保存 -> ReleaseStream
```

**不会再去同一个源通道 `GetChnFrame`。**

这点非常重要：  
如果已经走 `VPSS -> VENC` 硬 bind，就不要再把这个源通道当成应用层拉帧通道去消费。

## 3.2 软件送编路径

`cvi_mpi/sample/venc/src/sample_venc_lib.c` 的 non-bind 路径是：

```text
_getNonBindModeSrcFrame
-> _SAMPLE_VENC_SendFrame
-> _SAMPLE_VENC_GetStream
-> _releaseNonBindModeSrcFrame
```

其中 `_releaseNonBindModeSrcFrame()` 是在 **GetStream + ReleaseStream 完成之后** 才调用。

这等于给出了官方 sample 的推荐时序：

```text
GetChnFrame
-> SendFrame
-> GetStream
-> ReleaseStream
-> ReleaseChnFrame
```

这和你们当前实现最大的差异，就是**不能在 `SendFrame` 后立刻释放源帧**。

---

## 4. 从源码回答提问清单

## 4.1 `SendFrame` 后能不能立刻 `ReleaseChnFrame`？

**不建议。按 sample 和驱动实现看，都不应该这么做。**

理由有两层：

1. `CVI_VENC_SendFrame()` 用户态只是把 `VIDEO_FRAME_INFO_S` 透传给驱动。
2. 驱动编码时直接使用 `pstFrame->stVFrame.u64PhyAddr[]`、`u32Stride[]`、`enPixelFormat`。

也就是说，这条路径明显是**拿传入帧的物理地址做编码**，不是用户态先 memcpy 到某个独立输入缓存后再异步编码。

因此更安全、也更符合官方 sample 的写法是：

```text
GetChnFrame
-> SendFrame
-> GetStream
-> ReleaseStream
-> ReleaseChnFrame
```

如果你们现在是：

```text
GetChnFrame
-> SendFrame
-> ReleaseChnFrame
-> GetStream
```

那这就是本问题的 **P0 级风险点**。

## 4.2 `SendFrame` 需要在 `CreateChn` 时声明输入像素格式吗？

**不需要在 `VENC_CHN_ATTR_S` 里预声明。**

从驱动 `CVI_VENC_SendFrame()` 看：

- 第一次送帧时，它根据 `pstFrame->stVFrame.enPixelFormat`
  - 判断是不是 `NV12`
  - 判断是不是 `NV21`
  - 然后调用内部 `_cviVencSetInPixelFormat(...)`

所以输入像素格式是**从第一帧推断并锁定**的，不是在 `CreateChn` 时通过 `VENC_CHN_ATTR_S` 指定。

同时，驱动还会检查：

> 首帧之后如果 `enPixelFormat` 改变，H.264/H.265 直接报 `CVI_ERR_VENC_ILLEGAL_PARAM`

所以结论是：

- `CreateChn` 时不需要额外声明 `NV21`
- 但你送进来的第一帧必须就是正确的 `PIXEL_FORMAT_NV21`
- 后续帧的像素格式不能变

## 4.3 `VIDEO_FRAME_INFO_S` 里哪些字段是关键？

从驱动和 `enc_ctx.c::setSrcInfo()` 看，至少这些字段会被真正使用：

- `stVFrame.enPixelFormat`
- `stVFrame.u32Stride[0..1]`
- `stVFrame.u64PhyAddr[0..1]`
- `stVFrame.u32Width`
- `stVFrame.u32Height`
- `stVFrame.u64PTS`

其中最关键的是：

- **像素格式**
- **stride**
- **物理地址**

因为编码器最终是按这些信息去解释输入图像内存布局的。

## 4.4 `u32PicWidth/Height` 应填 logical 还是对齐后的 buffer 宽高？

结合 sample 和驱动实现，结论更偏向：

> **`u32PicWidth/Height` 填 logical 宽高；stride 通过 `VIDEO_FRAME_INFO_S` 单独表达。**

依据：

1. `sample_common_venc.c` 在创建 VENC 通道时，直接把业务宽高写进 `u32PicWidth/Height`，没有改成 stride 对齐宽。
2. `sample_venc_lib.c` 在为输入帧分配 buffer 时，会用 `VENC_GetPicBufferConfig()` 计算 **对齐后的 stride / length**，但 `VIDEO_FRAME_S.u32Width/u32Height` 仍然保持原始 logical 尺寸。
3. 驱动初始化编码器时，把 `u32PicWidth/u32PicHeight` 作为编码分辨率；而送帧时又单独读取帧里的 stride 和物理地址。

这说明源码设计本身就是：

- 编码分辨率：`u32PicWidth/u32PicHeight`
- 存储布局：`u32Stride[] + u64PhyAddr[]`

所以：

- `540x960` 作为 `u32PicWidth/u32PicHeight` 是合理的
- `stride = 576` 也是合理的
- **“logical 540, stride 576” 本身不是错误**

因此，现阶段不建议把 `u32PicWidth` 改成 `576` 作为首选修复。

## 4.5 `u32BufSize` 应按 raw YUV stride 算吗？

**不应该。**

`cvi_comm_venc.h` 里 `VENC_ATTR_S.u32BufSize` 的语义是：

> `size of encoded bitstream buffer`

它表示的是**码流缓冲区大小**，不是输入原始帧大小，也不是 NV21 stride 对应的源图 buffer 大小。

所以这类写法在语义上是不对的：

```c
attr.stVencAttr.u32BufSize = W * H * 3 / 2;
```

因为这实际上是在拿 **raw YUV 大小** 去填 **bitstream buffer 大小**。

这不一定就是当前 0 字节主因，但它是一个应该修正的配置问题。

更接近 sample 的做法是：

- 让它为 `0`，交给 sample/驱动默认处理
- 或者按码流需求单独估算
- 大图时再显式增大 ES buffer

## 4.6 VPSS `u32Depth` 到底影响什么？

从 `VPSS_CHN_ATTR_S` 头文件注释和 `vpss.c` 看，`u32Depth` 是：

> 用户取帧队列深度（user get list depth）

启用通道时，VPSS 会用 `u32Depth` 初始化该输出通道的 job/buffer 队列。

这意味着：

- **如果应用层要 `CVI_VPSS_GetChnFrame`，通常就需要 `u32Depth > 0`**
- **如果通道只用于 bind 给下游模块，`depth=0` 是常见做法**

但一旦你同时做这两件事：

1. `VPSS -> VENC` bind
2. 应用层也 `GetChnFrame` 同一个 VPSS channel

那就很容易出现：

- 应用层把通道帧取走
- VENC bind 线程等不到稳定输入
- 或者 VPSS 用户队列经常空

最终表现就是你看到的组合：

- `GetStream == BUSY`
- `GetChnFrame == VPSS_BUF_EMPTY`

因此，**同一个编码输入通道最好只保留一个消费者模型**：

- 要么纯 bind
- 要么纯软件拉帧送编

不要混用。

## 4.7 Bind 路径下 `Enable` / `Bind` / `StartRecvFrame` 的推荐顺序

结合 sample，推荐顺序是：

```text
先保证 VPSS group/channel 已 create + start + enable，且上游能稳定出帧
-> CVI_SYS_Bind(VPSS -> VENC)
-> CVI_VENC_StartRecvFrame()
-> 只做 GetStream / ReleaseStream
```

另外，`sample/sensor_lcd_hevc/hevc_recorder.c` 还专门在启用通道后加了一个很短的等待：

```text
Enable VPSS chn
-> usleep(50ms)
-> Start VENC
```

这说明 sample 也默认：

> 新启用的 VPSS 输出通道需要一点时间开始稳定供帧

---

## 5. 结合新统计，当前链路应该怎么判断

你补充的运行态信息，已经把排查顺序从“VENC API 时序”改成了“先修 VPSS Dev1 启动链路”。

最关键的几条证据是：

- `VENC EncodedFrame=0 / SendFramePerSec=0 / PicAddr=0 / PixFmt=N/A`
  - 说明 **VENC 从未真正吃到输入帧**
- `VPSS G2 RecvCnt≈1202` 且 `StartFailCnt=1202`
  - 说明 **G2 一直在收上游触发，但每次启动输出都失败**
- `VPSS G3 RecvCnt=0`
  - 说明 **SUB_VENC 输入通道根本没有收到来自 G2:2 的帧**
- `Dev1 UserTrigCnt=0 / IrqCnt=0`
  - 说明 **承载 G2/G3 的 Device1 根本没有被真正驱动起来**

因此目前最合理的源码级结论是：

```text
VI -> G0 正常
G0 -> G2 绑定存在
但 G2(Dev1) 每次启动输出失败
-> G2 chn SendOK 全 0
-> G2:2 无法喂给 G3
-> G3 SUB_VENC 无帧
-> VENC 从未收到输入
-> GetStream 看到 BUSY
```

## 5.1 路径 A：`SYS_Bind(G3-Ch0 -> VENC0)` 后不出流

结合新统计，bind 路径下最先要确认的已经不是 “VENC bind 顺序”，而是：

1. **G2(Dev1) 为什么 `StartFailCnt` 持续增加**
2. **G2:2 -> G3 的下游投递是否从未实际发生**
3. **G3-Ch0 在 Enable 后是否根本没有收到任何 frame**
4. **确认没有应用层再次消费同一源通道**

如果上游到 G3 的帧流本来就断了，那么即使 `CVI_SYS_Bind(VPSS->VENC)` 成功，内部 bind 线程也只能一直等输入帧，最后 `GetStream` 持续 `BUSY`。

所以 bind 路径的最小正确形态应该是：

```text
先修复 G2(Dev1) StartFail，让 G2-Ch2 真正出帧
-> 确认 G2-Ch2 -> G3 正在稳定投递 NV21
-> Enable G3-Ch0
-> Bind G3-Ch0 -> VENC0
-> StartRecvFrame
-> 只循环 QueryStatus/GetStream/ReleaseStream
```

**不要在同一时段再对 G3-Ch0 调 `GetChnFrame`。**

## 5.2 路径 B：`GetChnFrame -> SendFrame -> GetStream`

结合新统计，软件拉帧送编路径要分两步看：

1. **先确认 `CVI_VPSS_GetChnFrame(G3,0)` 现在是不是根本就拿不到帧**
2. **如果 G3 后续恢复出帧，再去修 `ReleaseChnFrame` 时序**

也就是说，这条路径目前的第一问题是：

> `G3` 本身没有帧可拉

而不是：

> VENC 已经拿到 `G3` 的帧，但被 `ReleaseChnFrame` 时序搞坏

不过从源码角度，这条路径依然存在一个明确缺陷：你们当前时序比 sample 少了“等码流并释放码流之后再释放源帧”这一步。

如果只改一件事，优先改成：

```text
GetChnFrame
-> SendFrame
-> GetStream
-> ReleaseStream
-> ReleaseChnFrame
```

同时把每一步返回值和 frame 元数据都打出来：

```text
GetChnFrame ret
SendFrame ret
GetStream ret
ReleaseStream ret
ReleaseChnFrame ret
fmt / width / height / stride / phyAddr / length
```

如果后续 `G2/G3` 修好以后，改完这个时序就开始出流，那么第二层问题也就坐实了。

---

## 6. 这次问题里，哪些怀疑点优先级最高

按“运行态统计 + 源码证据”重新排序，建议优先级如下。

## P0. `G2(Dev1)` 启动失败，导致 `G3` 无帧、VENC 从未收到输入

这是新的最强证据点。

- `VENC EncodedFrame=0 / PicAddr=0 / PixFmt=N/A`
- `G2 RecvCnt` 有增长，但 `StartFailCnt` 同步增长
- `G2` 通道 `SendOK` 全 0
- `G3 RecvCnt=0`
- `Dev1 UserTrigCnt=0 / IrqCnt=0`

这说明应优先把问题定位在：

- `G2` 为什么对每次输入都 `StartFail`
- `Dev1` 为什么从未真正触发
- `G2 -> G3` 为什么完全没有实际帧流

## P1. bind 路径里多消费者/抢帧

如果 G3-Ch0 一边 bind 给 VENC，一边还被应用层拿来 `GetChnFrame`，这是非常危险的用法。

bind 模式下应该让该通道成为：

> VENC 的专用输入通道

## P2. `SendFrame` 后过早 `ReleaseChnFrame`

这是明确的源码风险点，但根据你新补充的统计，它更像**第二层问题**。

- 官方 sample 明确不是这么写的
- 驱动直接引用传入帧的物理地址和 stride
- 但当前更关键的是：**VENC 还没走到真正消费输入帧的阶段**

所以它应该在 `G2/G3` 恢复出帧之后立刻修，但不是当前最先落刀的位置。

## P3. `u32BufSize` 被当成 raw frame size 使用

这不是最像当前 0 字节主因的点，但配置语义明显不对，建议一起修。

## P4. VB 容量不足

源码里确实有 `NOBUF` 错误路径，但你们当前主错误是 `BUSY`。  
所以 VB 不足应作为第二轮排查项，不应放在第一优先级。

## P5. “VENC 宽度必须写成 576”

从 sample 和 buffer helper 看，这条暂时**不像主因**。  
更符合源码语义的是：

- 编码宽高写 logical 540x960
- 输入 frame stride 用 576

---

## 7. 推荐修复顺序

建议按下面顺序做，避免一次改太多看不出因果。

1. **先把 `G2(Dev1)` 的 `StartFail` 根因定位出来**

   当前最该补的是 `G2` / `Dev1` 侧日志，而不是先改 VENC 参数。建议优先补：

   - `G2` 每次 `StartFail` 的返回码/分支原因
   - `Dev1` 为什么 `UserTrigCnt=0 / IrqCnt=0`
   - `G2-Ch2 -> G3` 的实际投递计数

2. **确认 `G3-Ch0` 是否恢复出帧**

   只有 `G3` 真正有帧了，软件送编或硬 bind 才有意义。

3. **再修软件送编时序**

   改成：

   ```text
   GetChnFrame
   -> SendFrame
   -> GetStream
   -> ReleaseStream
   -> ReleaseChnFrame
   ```

4. **把 `SendFrame` 返回值打印出来**

   现在如果 `SendFrame` 已经失败，但日志只盯着 `GetStream`，很容易误判。

5. **打印 frame 元数据**

   每次送编前至少打印：

   - `enPixelFormat`
   - `u32Width/u32Height`
   - `u32Stride[0]/u32Stride[1]`
   - `u32Length[0]/u32Length[1]`
   - `u64PhyAddr[0]/u64PhyAddr[1]`

6. **修正 `u32BufSize` 的语义**

   - 不要按 `W*H*3/2` 理解它
   - 先尝试置 `0` 或按码流 buffer 单独估算

7. **若软件路径修完能出流，再回头恢复硬 bind**

   bind 路径应保证：

   - 源通道只给 VENC 用
   - 应用层不再 `GetChnFrame` 同一通道
   - 顺序按 sample：Bind 后 StartRecvFrame

8. **最后再看 VB 扩容**

   只有在时序正确、单消费者模型正确后还不稳定，再去扩大 pool 才更有意义。

---

## 8. 最小可工作调用序列

## 8.1 软件送编最小序列

```c
VIDEO_FRAME_INFO_S frame;
VENC_STREAM_S stream;

CVI_VPSS_GetChnFrame(3, 0, &frame, 1000);

ret = CVI_VENC_SendFrame(0, &frame, 1000);
if (ret == CVI_SUCCESS) {
    ret = CVI_VENC_GetStream(0, &stream, 1000);
    if (ret == CVI_SUCCESS) {
        /* write stream */
        CVI_VENC_ReleaseStream(0, &stream);
    }
}

CVI_VPSS_ReleaseChnFrame(3, 0, &frame);
```

关键点只有一个：

> **`ReleaseChnFrame` 放到最后。**

## 8.2 硬件 bind 最小序列

```c
Enable VPSS channel;
Bind VPSS -> VENC;
StartRecvFrame;

for (;;) {
    QueryStatus;
    GetStream;
    write stream;
    ReleaseStream;
}
```

关键点也只有一个：

> **bind 路径下不要再对这个源 VPSS chn 做 `GetChnFrame`。**

---

## 9. 最终判断

基于本仓库 `cvi_mpi` 源码，再叠加你新补充的运行态统计，这次 Zonhor “VENC 不出流”的最可能根因可以概括为：

1. **第一层根因：`VPSS G2(Dev1)` 持续 `StartFail`，导致 `G3 SUB_VENC` 根本无帧，VENC 从未收到输入。**  
   最新日志里 `CamGetFrame` 与 `SUB_VENC GetChnFrame` 同为 `0xc006800e`，且内核 `Grp(3) Chn(0) jobs wait/work/done 全 0`，已把这一点钉死。
2. **第二层根因：即便后续恢复到软件送编路径，当前 `SendFrame -> ReleaseChnFrame -> GetStream` 的时序也不符合官方 sample，仍可能在下一阶段造成不出流。**
3. **bind 路径若继续保留多消费者模型，也会让 `GetStream == BUSY` 长期出现。**

而以下几点目前更像“应当修正，但不像主因”：

- `u32BufSize` 语义用错
- 想把 `u32PicWidth` 从 `540` 改成 `576`
- 继续怀疑 `GetStream == BUSY` 其实是 `BUF_EMPTY`
- Dual 模式下 `Chn invalid for VPSS-Dual` 的探测日志
- `VENC job init fail, already inited`（应清状态，但不是 G3 空队列的根因）

如果只允许先改一个地方，**优先把 `G2(Dev1)` 的 `StartFail` 原因打穿**（先看 dmesg 是 `Can't acquire VB` 还是 `apply hw settings NG`），确认为什么 `Dev1` 从未触发；在此之后，再改 `ReleaseChnFrame` 时序，然后重新跑：

```bash
./encode_demo 1 /tmp/output.h264
```

若文件开始大于 0，再继续把 bind 路径收敛成单消费者模型，问题大概率就能闭环。

---

## 10. 直接可落地的下一步（先诊断，再改编码）

### 10.0 立刻在板子上抓 G2 StartFail 分支（优先级最高）

驱动里 `u32StartFailCnt++` 只会出现在这些分支（`osdrv/interdrv/vpss/common/vpss.c`）：

| dmesg 关键字 | 含义 |
|--------------|------|
| `grp(2) apply hw settings NG` | `commitHWSettings` 失败（尺寸/格式/旋转/CSC 配置非法） |
| `grp(2) fill buffer NG` / `fill 2nd buffer NG` | 出帧 VB 申请失败等 |
| `Grp(2) Chn(N) Can't acquire VB BLK for VPSS` | **VB pool 不够或 blk_size 不匹配**（最常见） |
| `grp(2) run NG` / `stop before run NG` | `hw_start` 失败 |
| `grp(2) workingMask zero` | 通道 mask 为 0（无启用输出 / 帧率控掉） |

复现前先清状态，再开 log：

```bash
# 1) 杀干净旧进程，最好 reboot 一次（消掉 VENC already inited）
killall encode_demo 2>/dev/null; sync

# 2) 打开 VPSS 错误级日志后立刻复现
echo 4 > /proc/cvitek/vpss   # 若节点支持 level；否则依赖内核已有 CVI_TRACE

# 3) 复现后过滤
dmesg -c >/dev/null
./encode_demo ...   # 或你的 app
dmesg | grep -E 'grp\(2\)|Grp\(2\)|Can't acquire VB|fill buffer|apply hw|StartFail|Dev1|UserTrig'

# 4) 同时看 proc 统计（确认 StartFail 是否仍在涨）
cat /proc/cvitek/vpss
cat /proc/cvitek/vb
```

**验收门槛（在动 VENC 之前）：**

1. `ZONHOR_MMF_CamGetFrame` 不再报 `0xc006800e`
2. `SUB_VENC GetChnFrame` 不再报 `0xc006800e`
3. `/proc/cvitek/vpss` 里 G2：`StartFailCnt` 不再与 `RecvCnt` 同步增长，且有 `SendOK`
4. Dev1：`UserTrigCnt` / `IrqCnt` 开始非 0

若 `Can't acquire VB` 出现：优先扩 VB / 检查 G2 多路 RGB888+NV21 的 pool 尺寸与 `blk_size`，而不是改 VENC。

### 10.1 G2/G3 恢复出帧之后，再改编码路径

1. `zonhor_venc_send_frame` / `encode()` 路径中，把：

   ```text
   SendFrame -> ReleaseChnFrame -> GetStream
   ```

   改成：

   ```text
   SendFrame -> GetStream -> ReleaseStream -> ReleaseChnFrame
   ```

2. 为每次送编打印：

   ```text
   SendFrame ret=%#x fmt=%d w=%u h=%u stride0=%u stride1=%u len0=%u len1=%u phy0=%#llx phy1=%#llx
   ```

3. `VENC_ATTR_S.u32BufSize` 不再按 raw YUV 大小填写，改成先设 `0`，或按码流 buffer 需求填写。

这样改完之后，再判断是否还需要进一步调整 bind 顺序、depth、Dev1 配置或 VB pool。
