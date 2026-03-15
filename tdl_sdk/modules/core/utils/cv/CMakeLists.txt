project(utils-cv)
add_library(${PROJECT_NAME} OBJECT color.cpp
                                   contours.cpp
                                   imgwarp.cpp
                                   shapedescr.cpp
                                   thresh.cpp)
# OpenCV 4.x removed C++ constructors from C API structs (CvPoint, CvMat, CvRect...).
# CV__ENABLE_C_API_CTORS restores them for backward compatibility in these
# internally-copied OpenCV source files.
target_compile_definitions(${PROJECT_NAME} PRIVATE CV__ENABLE_C_API_CTORS)