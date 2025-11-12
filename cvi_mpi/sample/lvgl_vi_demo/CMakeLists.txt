cmake_minimum_required(VERSION 3.10)
project(lvgl_vi_example C)

# C标准设置
set(CMAKE_C_STANDARD 99)

# 源文件
file(GLOB_RECURSE SOURCES main.c lvgl/**/*.c)

# 头文件搜索路径
include_directories(
    .
    lvgl
    ../../include
    ../common
    ../../modules/isp/include
)

# 你可以根据需要修改编译选项
add_definitions(-Wall -g)

# 添加静态库文件
set(STATIC_LIBS
    #${CMAKE_SOURCE_DIR}/../demo-module/libvo_module.a
    ${CMAKE_SOURCE_DIR}/../demo-vi/libvi_module.a
    ${CMAKE_SOURCE_DIR}/../../lib/libvo.a
    ${CMAKE_SOURCE_DIR}/../../lib/libsys.a
    ${CMAKE_SOURCE_DIR}/../../lib/libmipi_tx.a
    ${CMAKE_SOURCE_DIR}/../../lib/libmisc.a
    ${CMAKE_SOURCE_DIR}/../../lib/libvi.a
    ${CMAKE_SOURCE_DIR}/../../lib/libvpss.a
)

# 最终目标
add_executable(lvgl_vi_example ${SOURCES})

# 链接静态库和系统库
target_link_libraries(lvgl_vi_example
    ${STATIC_LIBS}
    pthread
    atomic
    m
)

# 强制静态链接（可选，如果 toolchain 默认不是静态链接）
set_target_properties(lvgl_vi_example PROPERTIES LINK_SEARCH_START_STATIC TRUE)
set_target_properties(lvgl_vi_example PROPERTIES LINK_SEARCH_END_STATIC TRUE)

