#!/usr/bin/env bash
set -euo pipefail

# 简单脚本：在已完成 Buildroot 构建后，为目标根文件系统额外交叉编译并安装 Python 3.11.x
# 使用方式示例：
#   cd /home/satuo/maix/duo-buildroot-sdk-v2
#   ./scripts/build-python3.11.sh milkv-duos-musl-riscv64-sd 3.11.6
#   或由构建系统传入第三个参数作为目标 rootfs 路径，例如：
#   ./scripts/build-python3.11.sh milkv-duos-musl-riscv64-sd 3.11.6 /path/to/rootfs
# 第一个参数：Buildroot 输出目录名（即 buildroot/output/<board_name>）
# 第二个参数：可选，Python 3.11 版本号，默认为 3.11.6
# 第三个参数：可选，自定义安装的目标 rootfs 目录；不传则默认使用 buildroot/output/<board_name>/target

SDK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BOARD_OUTPUT_NAME="${1:-}"  # 例如: milkv-duos-musl-riscv64-sd
PY_VER="${2:-3.11.6}"
TARGET_ROOTFS_DIR_OVERRIDE="${3:-}"

if [[ -z "${BOARD_OUTPUT_NAME}" ]]; then
  echo "用法: $0 <board-output-name> [python-version(默认3.11.6)] [target-rootfs-dir]" >&2
    exit 1
fi

OUTPUT_DIR="${SDK_ROOT}/buildroot/output/${BOARD_OUTPUT_NAME}"
STAGING_DIR="${OUTPUT_DIR}/staging"
if [[ -n "${TARGET_ROOTFS_DIR_OVERRIDE}" ]]; then
  TARGET_DIR="${TARGET_ROOTFS_DIR_OVERRIDE}"
else
  TARGET_DIR="${OUTPUT_DIR}/target"
fi
WORK_DIR="${SDK_ROOT}/build/python3-${PY_VER}-extra"
DL_DIR="${SDK_ROOT}/dl/python3_11"

if [[ ! -d "${STAGING_DIR}" ]]; then
  echo "找不到 Buildroot staging 目录: ${STAGING_DIR}" >&2
  echo "请先正常执行 Buildroot 构建 (例如运行 build.sh)" >&2
  exit 1
fi

if [[ ! -d "${TARGET_DIR}" ]]; then
  echo "找不到目标根文件系统目录: ${TARGET_DIR}" >&2
  echo "请先正常执行 Buildroot 构建 (例如运行 build.sh)，或者确认第三个参数是否为正确的 rootfs 路径" >&2
  exit 1
fi

mkdir -p "${DL_DIR}" "${WORK_DIR}"

TARBALL="Python-${PY_VER}.tar.xz"
TARBALL_PATH="${DL_DIR}/${TARBALL}"

if [[ ! -f "${TARBALL_PATH}" ]]; then
    echo "下载 Python-${PY_VER} 源码..."
    curl -L -o "${TARBALL_PATH}" "https://www.python.org/ftp/python/${PY_VER}/${TARBALL}"
fi

rm -rf "${WORK_DIR:?}"/*

SRC_DIR="${WORK_DIR}/src"
BUILD_HOST_DIR="${WORK_DIR}/build-host"
BUILD_CROSS_DIR="${WORK_DIR}/build-cross"

mkdir -p "${SRC_DIR}" "${BUILD_HOST_DIR}" "${BUILD_CROSS_DIR}"

tar -C "${SRC_DIR}" --strip-components=1 -xf "${TARBALL_PATH}"

###############################################################################
# 第一步：在本机上构建一个同版本 Python（build-python），供交叉编译阶段使用
###############################################################################

pushd "${BUILD_HOST_DIR}" > /dev/null
"${SRC_DIR}/configure" --prefix="${BUILD_HOST_DIR}/hostprefix" --without-ensurepip
make -j"$(nproc)" python
BUILD_PYTHON="${BUILD_HOST_DIR}/python"
popd > /dev/null

###############################################################################
# 第二步：使用交叉工具链，为目标架构构建并安装 Python 3.11
###############################################################################

# 工具链路径：这里直接使用 SDK 自带的 riscv64-unknown-linux-musl 工具链
export PATH="${SDK_ROOT}/host-tools/gcc/riscv64-linux-musl-x86_64/bin:${PATH}"

pushd "${BUILD_CROSS_DIR}" > /dev/null

# 配置交叉编译
# --host: 目标架构三元组（需与工具链前缀一致）
# --build: 使用 config.guess 检测本机
# --prefix: 安装前缀，装到 /usr/local 下，避免覆盖 Buildroot 自带 python3.12
# DESTDIR: 指向 Buildroot 生成的 target 根目录

HOST_TRIPLET="riscv64-unknown-linux-musl"
BUILD_TRIPLET="$("${SRC_DIR}/config.guess")"

export CC="${HOST_TRIPLET}-gcc"
export CXX="${HOST_TRIPLET}-g++"
export AR="${HOST_TRIPLET}-ar"
export RANLIB="${HOST_TRIPLET}-ranlib"
export READELF="${HOST_TRIPLET}-readelf"

# 让 setup.py 知道自己在交叉编译，并额外把 Buildroot staging 的头文件/库目录塞进去
export _PYTHON_HOST_PLATFORM="linux-${HOST_TRIPLET}"

CPPFLAGS="--sysroot=${STAGING_DIR} -I${STAGING_DIR}/usr/include"
LDFLAGS="--sysroot=${STAGING_DIR} -L${STAGING_DIR}/usr/lib"

export CPPFLAGS LDFLAGS

# 一些自动检测在交叉编译时会失败，这里直接回答为“yes”
export ac_cv_file__dev_ptmx=yes
export ac_cv_file__dev_ptc=yes
export ac_cv_working_tzset=yes

# 若后续需要，可以在这里追加更多 ac_cv_xxx 变量

"${SRC_DIR}/configure" \
  --host="${HOST_TRIPLET}" \
  --build="${BUILD_TRIPLET}" \
  --prefix=/usr/local \
  --with-build-python="${BUILD_PYTHON}" \
  --disable-ipv6 \
  --enable-shared \
  --without-ensurepip

make -j"$(nproc)"

make DESTDIR="${TARGET_DIR}" install

popd > /dev/null

cat <<EOF

Python ${PY_VER} 已安装到镜像根文件系统的 /usr/local 下面，例如：
  /usr/local/bin/python3.11

重新打包镜像 (例如执行 gen_burn_image_sd.sh) 后，
在板子上可以通过完整路径显式指定使用这个 3.11 版本。
EOF
