file(GLOB _SOURCES "src/*.c")
file(GLOB _HEADERS "include/*.h")

include_directories(include)
include_directories(${CMAKE_INSTALL_INC_PREFIX}/arch)
include_directories(${CMAKE_INSTALL_INC_PREFIX}/common)
include_directories(${CMAKE_INSTALL_INC_PREFIX}/kernel)
include_directories(${CMAKE_INSTALL_INC_PREFIX}/driver/gpio)
include_directories(${CMAKE_INSTALL_INC_PREFIX}/driver/rtos_cmdqu)
include_directories(${CMAKE_INSTALL_INC_PREFIX}/hal/config)
include_directories(${CMAKE_INSTALL_INC_PREFIX}/hal/pinmux)
include_directories(../comm/include)

# Ensure mmio helpers available for PINMUX_CONFIG
include_directories(${CMAKE_INSTALL_INC_PREFIX}/common)

add_library(display STATIC ${_SOURCES})
target_compile_options(display PRIVATE -Wno-unused-parameter)
install(TARGETS display DESTINATION lib)
install(FILES ${_HEADERS} DESTINATION include/task/display)
