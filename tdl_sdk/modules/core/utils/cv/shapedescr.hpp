#pragma once
#include "opencv2/core.hpp"
// OpenCV 4.x no longer auto-includes C API types via core.hpp
#include "opencv2/core/core_c.h"

/* From imgproc_c.h */
/** @brief Calculates contour bounding rectangle (update=1) or
   just retrieves pre-calculated rectangle (update=0)
@see cv::boundingRect
*/
CvRect cvBoundingRect(CvArr* points, int update = 0);

namespace cvitdl {

double contourArea(cv::InputArray contour, bool oriented = false);

cv::Rect boundingRect(cv::InputArray points);

}  // namespace cvitdl
