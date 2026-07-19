#ifndef BENCH_CONVERT_H
#define BENCH_CONVERT_H

#include "sample_comm.h"

/* Naive NV21 -> RGB565 (BT.601), nearest-neighbor scale into dst band. */
int bench_nv21_scale_to_rgb565(const VIDEO_FRAME_S *frame, uint16_t *dst,
			       int dst_w, int dst_h);

/*
 * Naive packed RGB888 -> RGB565: byte0=R, byte1=G, byte2=B (as PIXEL_FORMAT
 * name implies). No B-G-R workaround.
 */
int bench_rgb888_scale_to_rgb565(const VIDEO_FRAME_S *frame, uint16_t *dst,
				 int dst_w, int dst_h);

#endif /* BENCH_CONVERT_H */
