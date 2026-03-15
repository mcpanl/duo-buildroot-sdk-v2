# CVI TDL SDK — 外部 VPSS 预处理模式 接口说明

> 适用场景：使用 `CVI_TDL_CreateHandle3` 创建无内置 VPSS 的 handle，由外部自行完成图像预处理后送入推理。  
> 相关头文件：`include/core/cvi_tdl_core.h`、`include/core/core/cvtdl_vpss_types.h`、`include/core/face/cvtdl_face_types.h`、`include/core/cvi_tdl_rescale_bbox.h`

---

## 一、初始化流程接口

---

### `CVI_TDL_CreateHandle3`

```c
CVI_S32 CVI_TDL_CreateHandle3(cvitdl_handle_t *handle);
```

创建一个**不绑定任何 VPSS 引擎**的轻量 handle。

**参数**

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | out | 输出创建的 handle 指针 |

**说明**

- 适用于外部已有 VPSS 或其他预处理手段的场景
- 与 `CVI_TDL_CreateHandle` / `CVI_TDL_CreateHandle2` 的区别：内部 `vec_vpss_engine` 为空，SDK **不会**主动初始化或占用任何 VPSS Group
- 必须配合 `CVI_TDL_SetSkipVpssPreprocess(true)` 才能正常推理，否则调用推理接口时会返回 `CVI_TDL_ERR_INVALID_ARGS (0xC0010106)`
- 该函数本身不会失败

**返回值**

| 返回值 | 含义 |
|--------|------|
| `CVI_TDL_SUCCESS (0)` | 成功 |

---

### `CVI_TDL_OpenModel`

```c
CVI_S32 CVI_TDL_OpenModel(cvitdl_handle_t handle,
                          CVI_TDL_SUPPORTED_MODEL_E model,
                          const char *filepath);
```

加载 `.cvimodel` 模型文件并完成 TPU 推理引擎初始化。

**参数**

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | in | 已创建的 TDL handle |
| `model` | in | 模型枚举，人脸检测使用 `CVI_TDL_SUPPORTED_MODEL_SCRFDFACE` |
| `filepath` | in | `.cvimodel` 文件的绝对路径，文件必须存在且为普通文件 |

**说明**

- 此步骤会调用底层 `CVI_NN_RegisterModel`，需要 TPU 驱动已加载（`/dev/cvi-tpu0` 节点存在）
- 模型文件名中的芯片后缀（如 `_cv181x`）必须与实际运行的芯片型号匹配，否则驱动加载会失败

**返回值**

| 返回值 | 枚举值 | 含义 |
|--------|--------|------|
| `CVI_TDL_SUCCESS` | `0x00000000` | 成功 |
| `CVI_TDL_ERR_MODEL_INITIALIZED` | `0xC001010A` | 模型已加载，需先调用 `CVI_TDL_CloseModel` |
| `CVI_TDL_ERR_INVALID_MODEL_PATH` | `0xC0010101` | 文件路径不存在或不是普通文件 |
| `CVI_TDL_ERR_OPEN_MODEL` | `0xC0010102` | TPU 驱动加载模型失败（驱动未加载、模型与芯片平台不匹配、ION 内存不足等） |

---

### `CVI_TDL_SetSkipVpssPreprocess`

```c
CVI_S32 CVI_TDL_SetSkipVpssPreprocess(cvitdl_handle_t handle,
                                      CVI_TDL_SUPPORTED_MODEL_E model,
                                      bool skip);
```

设置是否跳过 SDK 内置的 VPSS 预处理。

**参数**

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | in | 已创建的 TDL handle |
| `model` | in | 模型枚举 |
| `skip` | in | `true`：跳过内部 VPSS，由外部负责预处理；`false`（默认）：SDK 内部自动调用 VPSS |

**说明**

- **必须在 `CVI_TDL_OpenModel` 之后调用**，模型实例在 `OpenModel` 时才会被创建
- `skip = true` 时：SDK 跳过缩放/格式转换，直接将传入帧注册到 TPU tensor，调用者需自行保证帧格式、分辨率、归一化完全符合模型要求
- ⚠️ **并非所有模型都支持此模式**。`CVI_TDL_SUPPORTED_MODEL_SCRFDFACE` 支持（内部 `allowExportChannelAttribute = true`）。不支持的模型在 `skip = true` 时调用推理会返回 `CVI_TDL_ERR_INVALID_ARGS`

**返回值**

| 返回值 | 含义 |
|--------|------|
| `CVI_TDL_SUCCESS (0)` | 成功 |
| `CVI_TDL_ERR_OPEN_MODEL (0xC0010102)` | 模型实例未创建（未调用 `OpenModel`） |

---

### `CVI_TDL_GetVpssChnConfig`

```c
CVI_S32 CVI_TDL_GetVpssChnConfig(cvitdl_handle_t handle,
                                 CVI_TDL_SUPPORTED_MODEL_E model,
                                 const CVI_U32 frameWidth,
                                 const CVI_U32 frameHeight,
                                 const CVI_U32 idx,
                                 cvtdl_vpssconfig_t *chnConfig);
```

从已加载的模型中**导出外部 VPSS 所需的通道配置参数**，包含目标分辨率、像素格式、归一化系数。

**参数**

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | in | 已创建的 TDL handle |
| `model` | in | 模型枚举 |
| `frameWidth` | in | **原始输入帧**的宽（非模型输入尺寸），SDK 自动计算等比缩放参数 |
| `frameHeight` | in | **原始输入帧**的高 |
| `idx` | in | 输入 tensor 索引，单输入模型传 `0` |
| `chnConfig` | out | 输出的 VPSS 通道配置结构体 |

**输出结构体定义**

```c
typedef struct {
    VPSS_SCALE_COEF_E chn_coeff;   // 缩放算法（如双三次 BICUBIC），可直接用于 SetChnAttr
    VPSS_CHN_ATTR_S   chn_attr;    // 完整的 VPSS 通道属性（目标尺寸、格式、归一化系数）
} cvtdl_vpssconfig_t;
```

**说明**

- **必须在 `CVI_TDL_OpenModel` 之后调用**
- 返回的 `chn_attr` 可**直接传入** `CVI_VPSS_SetChnAttr(grp, chn, &chnConfig.chn_attr)` 使用，无需手动填写归一化参数（SDK 已从模型 metadata 中提取）
- 对于 `SCRFDFACE`：归一化参数为 `factor = 1/128`，`mean = 127.5/128`，像素格式为 `PIXEL_FORMAT_BGR_888_PLANAR`
- ⚠️ 若模型不支持（`allowExportChannelAttribute = false`），返回 `CVI_TDL_ERR_GET_VPSS_CHN_CONFIG`

**返回值**

| 返回值 | 含义 |
|--------|------|
| `CVI_TDL_SUCCESS (0)` | 成功 |
| `CVI_TDL_ERR_NOT_YET_INITIALIZED (0xC001010B)` | 模型未 Open |
| `CVI_TDL_ERR_GET_VPSS_CHN_CONFIG (0xC0010104)` | 该模型不支持导出通道配置 |

---

## 二、推理接口

---

### `CVI_TDL_FaceDetection`

```c
CVI_S32 CVI_TDL_FaceDetection(const cvitdl_handle_t handle,
                              VIDEO_FRAME_INFO_S *frame,
                              CVI_TDL_SUPPORTED_MODEL_E model_index,
                              cvtdl_face_t *face_meta);
```

人脸检测推理接口，支持 RetinaFace、ScrFD、ThermalFace 等多种人脸检测模型。

**参数**

| 参数 | 方向 | 说明 |
|------|------|------|
| `handle` | in | 已创建的 TDL handle |
| `frame` | in | 输入视频帧，skip VPSS 模式下必须已完成预处理 |
| `model_index` | in | 模型枚举，如 `CVI_TDL_SUPPORTED_MODEL_SCRFDFACE` |
| `face_meta` | out | 检测结果输出 |

**skip VPSS 模式下对输入帧的严格要求**

| 要求 | 具体值 | 说明 |
|------|--------|------|
| 像素格式 | `PIXEL_FORMAT_BGR_888_PLANAR` | ScrFDFace 模型要求 BGR Planar，需通过 VPSS 完成格式转换 |
| 分辨率 | 必须严格等于模型输入尺寸 | 本例模型输入为 **432×768**，不一致会返回 `CVI_TDL_ERR_INFERENCE` |
| 内存类型 | ION 物理内存 | `u64PhyAddr[0/1/2]` 必须均有效，虚拟地址无需有效 |
| 归一化 | 由 VPSS 通道完成 | `chn_attr` 中已包含归一化系数，VPSS 硬件自动完成 |

**输出结构体说明**

```c
typedef struct {
    uint32_t           size;         // 检测到的人脸数量
    uint32_t           width;        // 推理所用帧的宽（用于坐标还原，框坐标基于此尺寸）
    uint32_t           height;       // 推理所用帧的高
    cvtdl_face_info_t  *info;        // 每个人脸的详细信息数组（size 个元素）
    cvtdl_dms_t        *dms;         // DMS 相关信息（人脸检测场景通常为 NULL）
} cvtdl_face_t;

typedef struct {
    char               name[128];    // 人脸名称（识别后填充）
    uint64_t           unique_id;    // 唯一 ID（追踪后填充）
    cvtdl_bbox_t       bbox;         // 检测框（x1, y1, x2, y2，相对于 width/height）
    cvtdl_pts_t        pts;          // 5 点关键点坐标
    float              score;        // 检测置信度
    // ... 其余字段为识别/属性分析后填充
} cvtdl_face_info_t;
```

**⚠️ 坐标系注意**

输出 `bbox` 坐标是相对于**传入帧的尺寸**（即模型输入 432×768）的。若需要映射回原始帧坐标，可使用 SDK 提供的辅助函数（传入**原始帧**）：

```c
// 居中等比缩放模式（SDK 默认，RESCALE_CENTER）
CVI_TDL_RescaleMetaCenterFace(const VIDEO_FRAME_INFO_S *原始帧, cvtdl_face_t *face_meta);

// 右下填充缩放模式（RESCALE_RB）
CVI_TDL_RescaleMetaRBFace(const VIDEO_FRAME_INFO_S *原始帧, cvtdl_face_t *face_meta);
```

ScrFDFace 使用 `RESCALE_CENTER`（居中缩放），故坐标还原应调用 `CVI_TDL_RescaleMetaCenterFace`。

**⚠️ 使用前后注意**

```c
// 推理前：必须清零初始化
cvtdl_face_t face_meta;
memset(&face_meta, 0, sizeof(cvtdl_face_t));

// 推理后：使用完毕必须释放，避免内存泄漏
CVI_TDL_FreeFace(&face_meta);
```

**返回值**

| 返回值 | 含义 |
|--------|------|
| `CVI_TDL_SUCCESS (0)` | 成功 |
| `CVI_TDL_ERR_NOT_YET_INITIALIZED (0xC001010B)` | 模型未调用 `OpenModel` |
| `CVI_TDL_ERR_INVALID_ARGS (0xC0010106)` | 模型不支持 skip VPSS 模式 |
| `CVI_TDL_ERR_INFERENCE (0xC0010105)` | 帧格式/尺寸不符合要求，或 TPU 推理失败 |
| `CVI_TDL_ERR_INIT_VPSS (0xC0010107)` | VPSS 初始化失败（非 skip 模式下） |

---

## 三、资源释放接口

```c
// 释放 cvtdl_face_t 内部动态分配的内存（info 数组等），每次推理后必须调用
void CVI_TDL_FreeFace(cvtdl_face_t *face);

// 关闭指定模型并释放其实例（可重新 OpenModel）
CVI_S32 CVI_TDL_CloseModel(cvitdl_handle_t handle, CVI_TDL_SUPPORTED_MODEL_E model);

// 销毁 handle，内部自动关闭所有模型并释放所有资源
CVI_S32 CVI_TDL_DestroyHandle(cvitdl_handle_t handle);
```

---

## 四、完整调用顺序

```
① CVI_TDL_CreateHandle3(handle)
        │  创建无 VPSS 的轻量 handle
        ▼
② CVI_TDL_OpenModel(handle, SCRFDFACE, filepath)
        │  加载模型，需 TPU 驱动已就绪
        ▼
③ CVI_TDL_SetSkipVpssPreprocess(handle, SCRFDFACE, true)
        │  声明跳过内部 VPSS
        ▼
④ CVI_TDL_GetVpssChnConfig(handle, SCRFDFACE, srcW, srcH, 0, &cfg)
        │  获取外部 VPSS 通道所需参数
        ▼
⑤ [外部] CVI_VPSS_SetChnAttr(grp, chn, &cfg.chn_attr)
         CVI_VPSS_EnableChn(grp, chn)
        │  按 SDK 导出参数配置外部 VPSS 通道
        ▼
⑥ CVI_TDL_SetModelThreshold / SetModelNmsThreshold  （可选）
        │
        ▼
──────────── 每帧循环 ────────────
⑦ [外部] CVI_VPSS_SendFrame / GetChnFrame
        │  原始帧 → 外部 VPSS → 432×768 BGR Planar ION 帧
        ▼
⑧ memset(&face_meta, 0, sizeof(cvtdl_face_t))
        ▼
⑨ CVI_TDL_FaceDetection(handle, &preprocessed_frame, SCRFDFACE, &face_meta)
        ▼
⑩ CVI_TDL_RescaleMetaCenterFace(&原始帧, &face_meta)   （可选，坐标还原）
        ▼
⑪ [使用 face_meta.info[i].bbox 等结果]
        ▼
⑫ CVI_TDL_FreeFace(&face_meta)
         CVI_VPSS_ReleaseChnFrame(grp, chn, &preprocessed_frame)
──────────────────────────────────
        ▼
⑬ CVI_TDL_DestroyHandle(handle)
```

---

## 五、错误码速查表

| 枚举名 | 十六进制 | 十进制（有符号） | 说明 |
|--------|----------|-----------------|------|
| `CVI_TDL_SUCCESS` | `0x00000000` | `0` | 成功 |
| `CVI_TDL_FAILURE` | `0xFFFFFFFF` | `-1` | 通用失败 |
| `CVI_TDL_ERR_INVALID_MODEL_PATH` | `0xC0010101` | `-1073676031` | 模型文件路径无效 |
| `CVI_TDL_ERR_OPEN_MODEL` | `0xC0010102` | `-1073676030` | 模型打开/加载失败 |
| `CVI_TDL_ERR_GET_VPSS_CHN_CONFIG` | `0xC0010104` | `-1073676028` | 不支持导出 VPSS 配置 |
| `CVI_TDL_ERR_INFERENCE` | `0xC0010105` | `-1073676027` | 推理失败（帧格式/尺寸不符或 TPU 错误） |
| `CVI_TDL_ERR_INVALID_ARGS` | `0xC0010106` | `-1073676026` | 参数无效（含模型不支持 skip VPSS） |
| `CVI_TDL_ERR_INIT_VPSS` | `0xC0010107` | `-1073676025` | VPSS 初始化失败 |
| `CVI_TDL_ERR_MODEL_INITIALIZED` | `0xC001010A` | `-1073676022` | 模型已初始化，无需重复加载 |
| `CVI_TDL_ERR_NOT_YET_INITIALIZED` | `0xC001010B` | `-1073676021` | 模型尚未初始化 |
