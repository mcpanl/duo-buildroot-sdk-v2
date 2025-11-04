# 这两条有没有都无所谓
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm64)

# 指定交叉编译工具
set(tools "/home/satuo/milkv/duo-buildroot-sdk-v2/host-tools/gcc/gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu")
set(CMAKE_C_COMPILER ${tools}/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER ${tools}/bin/aarch64-linux-gnu-g++)

