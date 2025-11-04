cmake_minimum_required(VERSION 3.10)
project(lvgl_example C)

# C标准设置
set(CMAKE_C_STANDARD 99)

# 源文件
file(GLOB_RECURSE SOURCES main.c lvgl/**/*.c)

# 头文件搜索路径
include_directories(
    .
    lvgl
)

# 你可以根据需要修改编译选项
add_definitions(-Wall -g)

# 最终目标
add_executable(lvgl_example ${SOURCES})

# 铅接时需要的数据
# (比如需要数学库时可以解除注释)
# target_link_libraries(lvgl_example m)

