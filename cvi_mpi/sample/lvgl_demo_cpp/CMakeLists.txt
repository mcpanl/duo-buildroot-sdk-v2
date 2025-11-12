cmake_minimum_required(VERSION 3.10)
project(lvgl_example C CXX)

# C 和 C++ 标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源文件（匹配 C 和 C++）
file(GLOB_RECURSE SOURCES 
    main.cpp
    lvgl/**/*.c
    lvgl/**/*.cpp
)

# 头文件搜索路径
include_directories(
    .
    lvgl
    ../../include
    ../common
    ../../modules/isp/include
)

# 编译选项
add_definitions(-Wall -g)

# 你也可以用下面这种方式设置 C++ 特有的选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -g -fpermissive")

# 添加静态库文件
set(STATIC_LIBS
    ${CMAKE_SOURCE_DIR}/../demo-module/libvo_module.a
    ${CMAKE_SOURCE_DIR}/../../lib/libvo.a
    ${CMAKE_SOURCE_DIR}/../../lib/libsys.a
    ${CMAKE_SOURCE_DIR}/../../lib/libmipi_tx.a
    ${CMAKE_SOURCE_DIR}/../../lib/libmisc.a
)

# 生成可执行文件
add_executable(lvgl_example ${SOURCES})

# 链接静态库和系统库
target_link_libraries(lvgl_example
    ${STATIC_LIBS}
    pthread
    atomic
    m
)

# 强制静态链接（可选）
set_target_properties(lvgl_example PROPERTIES LINK_SEARCH_START_STATIC TRUE)
set_target_properties(lvgl_example PROPERTIES LINK_SEARCH_END_STATIC TRUE)

