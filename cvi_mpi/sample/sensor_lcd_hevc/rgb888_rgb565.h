#ifndef RGB888_RGB565_H
#define RGB888_RGB565_H

#include <stdint.h>

#include "sample_comm.h"

/* 1:1 Packed RGB888 -> RGB565 using frame width/height and strides (no scale/rotate). */
int rgb888_frame_to_rgb565(const VIDEO_FRAME_S *frame, uint16_t *dst);

#endif /* RGB888_RGB565_H */
