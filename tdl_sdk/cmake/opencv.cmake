project(opencv409_local)

# Use the pre-built OpenCV 4.0.9 directory at the top-level of the SDK
set(OPENCV_ROOT ${TOP_DIR}/opencv409)

message(STATUS "Using pre-built OpenCV 4.0.9 from: ${OPENCV_ROOT}")

if(NOT IS_DIRECTORY "${OPENCV_ROOT}")
  message(FATAL_ERROR "opencv409 directory not found at ${OPENCV_ROOT}. "
                      "Please ensure the opencv409 directory exists.")
endif()

# OpenCV 4.x uses include/opencv4/ prefix
set(OPENCV_INCLUDES
  ${OPENCV_ROOT}/include/opencv4
)

# Shared libraries (soname: libopencv_*.so.409)
set(OPENCV_LIBS_IMCODEC
  ${OPENCV_ROOT}/lib/libopencv_core.so.409
  ${OPENCV_ROOT}/lib/libopencv_imgproc.so.409
  ${OPENCV_ROOT}/lib/libopencv_imgcodecs.so.409
)

# No static (.a) archives available; reuse shared libs for the static target
set(OPENCV_LIBS_IMCODEC_STATIC
  ${OPENCV_ROOT}/lib/libopencv_core.so.409
  ${OPENCV_ROOT}/lib/libopencv_imgproc.so.409
  ${OPENCV_ROOT}/lib/libopencv_imgcodecs.so.409
)

set(OPENCV_PATH ${CMAKE_INSTALL_PREFIX}/sample/3rd/opencv)

# Install opencv libraries and headers
install(PROGRAMS ${OPENCV_ROOT}/lib/libopencv_core.so.409
        DESTINATION ${OPENCV_PATH}/lib)
install(PROGRAMS ${OPENCV_ROOT}/lib/libopencv_imgproc.so.409
        DESTINATION ${OPENCV_PATH}/lib)
install(PROGRAMS ${OPENCV_ROOT}/lib/libopencv_imgcodecs.so.409
        DESTINATION ${OPENCV_PATH}/lib)
install(DIRECTORY ${OPENCV_ROOT}/include/opencv4/
        DESTINATION ${OPENCV_PATH}/include)
