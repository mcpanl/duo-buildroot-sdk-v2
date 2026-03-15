# tdl_sdk OpenCV 3.2 → 4.0.9 迁移记录

## 背景

`tdl_sdk` 原本依赖预编译的 OpenCV 3.2 库（`libopencv_*.so.3.2.0`），通过 FTP/OSS 自动下载。
为统一依赖、与其他项目共享同一套 OpenCV 4.0.9 编译产物，将其迁移至项目根目录下的 `opencv409/` 目录。

目标平台：**milkv-duos-musl-riscv64-sd**（RISC-V 64-bit，musl libc）

---

## opencv409 目录结构

```
opencv409/
├── include/
│   └── opencv4/
│       └── opencv2/          # OpenCV 4.x 标准头文件路径
└── lib/
    ├── libopencv_core.so.409
    ├── libopencv_imgproc.so.409
    └── libopencv_imgcodecs.so.409
```

库文件的 SONAME 均为 `libopencv_*.so.409`，为自定义命名（非标准的 `so.4.0.9`），运行时需确保目标设备上存在同名文件。

---

## 修改文件清单

共修改 **7 个文件**，以下按类别说明。

---

### 1. 构建系统 — `tdl_sdk/cmake/opencv.cmake`

**变更内容：** 完全重写。

原来的逻辑会根据工具链类型匹配架构字符串，然后从 FTP 服务器或本地 OSS tarball 下载对应的 `opencv_aisdk.tar.gz` 并解压使用。

新逻辑直接引用 `${TOP_DIR}/opencv409`（即项目根目录下的 `opencv409/`）：

```cmake
set(OPENCV_ROOT ${TOP_DIR}/opencv409)

set(OPENCV_INCLUDES
  ${OPENCV_ROOT}/include/opencv4          # OpenCV 4.x 头文件在 opencv4/ 子目录下
)

set(OPENCV_LIBS_IMCODEC
  ${OPENCV_ROOT}/lib/libopencv_core.so.409
  ${OPENCV_ROOT}/lib/libopencv_imgproc.so.409
  ${OPENCV_ROOT}/lib/libopencv_imgcodecs.so.409
)
```

**关键差异：**
- 头文件路径从 `include/` 改为 `include/opencv4/`（OpenCV 4.x 的规范目录布局）
- 不再有静态库 `.a`，静态目标 `cvi_tdl-static` 同样链接 `.so.409`
- 安装规则删除了 `so.3.2` 软链接的创建逻辑

---

### 2. 源码修复 — OpenCV 3→4 破坏性变更

#### 2.1 `modules/service/face_angle/face_angle.hpp`

```cpp
// 修改前
#include "opencv2/core/core.hpp"   // OpenCV 3.x 遗留路径

// 修改后
#include "opencv2/core.hpp"        // OpenCV 4.x 标准路径
```

#### 2.2 `modules/service/face_angle/face_angle.cpp`

`cvFastArctan` 是 C API 函数，在 OpenCV 4.x 中不再通过默认头文件暴露，需显式包含 `core_c.h`。
原来的版本守卫条件写的是 `>= 4 && >= 5`（即 4.5 才包含），导致 4.0.9 下缺失声明。

```cpp
// 修改前（条件错误，4.0.9 下不会包含）
#if CV_VERSION_MAJOR >= 4 && CV_VERSION_MINOR >= 5
#include "opencv2/core/core_c.h"
#endif

// 修改后（>= 4.0 即包含）
#if CV_VERSION_MAJOR >= 4
#include "opencv2/core/core_c.h"
#endif
```

> `core_c.h` 在 opencv409 中仍然存在，`cvFastArctan` 声明也在其中，所以此修复可行。

#### 2.3 `modules/core/face_quality/face_quality.cpp`

同上，同一个版本守卫问题，同样修复。

#### 2.4 `modules/core/utils/image_utils.cpp`（4 处）

OpenCV 4.x 的 `imgproc.hpp` 不再自动包含 `imgproc/types_c.h`，导致裸宏 `CV_YUV2RGB_NV21` 和 `CV_YUV2RGB_I420` 未定义。替换为带命名空间的枚举值：

```cpp
// 修改前
cv::cvtColor(src, dst, CV_YUV2RGB_NV21);
cv::cvtColor(src, dst, CV_YUV2RGB_I420);

// 修改后
cv::cvtColor(src, dst, cv::COLOR_YUV2RGB_NV21);
cv::cvtColor(src, dst, cv::COLOR_YUV2RGB_I420);
```

涉及行：第 194、213、316、339 行（两处 NV21、两处 I420）。

---

### 3. `modules/core/utils/cv/` 目录（内部 OpenCV 源码移植）

该目录包含从 OpenCV 3.x 内部源码移植的 NEON 加速实现（`warpAffine`、`findContours`、`cvtColor` 等），大量使用 C API 类型（`CvRect`、`CvArr`、`CvMat`、`CvPoint`、`CV_StsBadArg` 等）。

**根本原因：** OpenCV 3.x 中，`core.hpp` 会通过传递包含（transitive include）自动引入所有 C API 类型定义；OpenCV 4.x 切断了这条链路。

#### 3.1 补充 `core_c.h` 包含

在以下 4 个头文件中补充显式 include：

| 文件 | 需要的 C API 符号 |
|---|---|
| `color.hpp` | `CV_StsBadFlag` 等错误码 |
| `shapedescr.hpp` | `CvRect`、`CvArr*` |
| `thresh.hpp` | `CvArr*`、`cvarrToMat`、`CV_StsUnsupportedFormat` |
| `imgwarp.hpp` | `CV_StsBadArg` 等错误码 |

```cpp
// 在每个文件的 #include "opencv2/core.hpp" 后追加：
#include "opencv2/core/core_c.h"
```

#### 3.2 `modules/core/utils/cv/CMakeLists.txt`

添加编译定义 `CV__ENABLE_C_API_CTORS`：

```cmake
target_compile_definitions(${PROJECT_NAME} PRIVATE CV__ENABLE_C_API_CTORS)
```

**原因：** OpenCV 4.x 默认不为 C API 结构体提供 C++ 构造函数。`contours.cpp` 中大量使用了如下写法：

```cpp
CvPoint(1, 0)          // 需要 CvPoint::CvPoint(int, int)
CvMat _cimage = image; // 需要 CvMat::CvMat(const cv::Mat&)
```

`CV__ENABLE_C_API_CTORS` 宏是 OpenCV 官方为此类迁移场景预留的兼容开关，启用后会在 `types_c.h` 中恢复上述构造函数。

---

## 编译验证

```bash
cd /path/to/duo-buildroot-sdk-v2
source build/envsetup_milkv.sh milkv-duos-musl-riscv64-sd
build_tdl_sdk
```

编译产物：

```
tdl_sdk/install/lib/libcvi_tdl.so   (991K)
tdl_sdk/install/lib/libcvi_tdl.a
tdl_sdk/install/lib/libcvi_tdl_app.so
tdl_sdk/install/lib/libcvi_tdl_app.a
```

`libcvi_tdl.so` 的动态依赖（`readelf -d` 验证）：

```
NEEDED  libopencv_core.so.409
NEEDED  libopencv_imgproc.so.409
NEEDED  libopencv_imgcodecs.so.409
```

---

## 部署注意事项

目标设备上需要存在以下三个库文件（SONAME 完全匹配）：

```
/path/to/lib/libopencv_core.so.409
/path/to/lib/libopencv_imgproc.so.409
/path/to/lib/libopencv_imgcodecs.so.409
```

这些文件与 `opencv409/lib/` 下的文件相同，由另一项目统一部署，两个项目共享同一套 OpenCV 4.0.9 运行时。
