#ifndef NV21_RGB565_H
#define NV21_RGB565_H

#include <stdint.h>

#include "sample_comm.h"

/* 1:1 NV21/NV12 -> RGB565 using frame width/height and strides (no scale/rotate). */
int nv21_frame_to_rgb565(const VIDEO_FRAME_S *frame, uint16_t *dst);

#endif /* NV21_RGB565_H */
