file(GLOB _SOURCES "src/*.c")
file(GLOB _HEADERS "include/*.h")

include_directories(include)
include_directories(${CMAKE_INSTALL_INC_PREFIX}/arch)
include_directories(${CMAKE_INSTALL_INC_PREFIX}/common)
include_directories(${CMAKE_INSTALL_INC_PREFIX}/kernel)
include_directories(${CMAKE_INSTALL_INC_PREFIX}/driver/rtos_cmdqu)
include_directories(${TOP_DIR}/task/display/include)

add_library(stats STATIC ${_SOURCES})
target_compile_options(stats PRIVATE -Wno-unused-parameter)
install(TARGETS stats DESTINATION lib)
install(FILES ${_HEADERS} DESTINATION include/task/stats)
