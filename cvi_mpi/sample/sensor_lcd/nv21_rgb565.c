#include "nv21_rgb565.h"

#include <string.h>

#define CLIP8(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))

static uint16_t yuv_to_rgb565(uint8_t y, uint8_t u, uint8_t v)
{
	int c = (int)y - 16;
	int d = (int)u - 128;
	int e = (int)v - 128;
	int r = (298 * c + 409 * e + 128) >> 8;
	int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
	int b = (298 * c + 516 * d + 128) >> 8;
	uint8_t R = (uint8_t)CLIP8(r);
	uint8_t G = (uint8_t)CLIP8(g);
	uint8_t B = (uint8_t)CLIP8(b);

	return (uint16_t)(((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3));
}

static void read_chroma(const uint8_t *vu, int x, uint8_t *u, uint8_t *v)
{
	int idx = (x / 2) * 2;

	/*
	 * SG2000 VPSS reports PIXEL_FORMAT_NV21 (fmt=19) but the chroma plane
	 * is UV-interleaved (NV12 layout). Using VU order swaps red/blue.
	 */
	*u = vu[idx + 0];
	*v = vu[idx + 1];
}

int nv21_frame_to_rgb565(const VIDEO_FRAME_S *frame, uint16_t *dst)
{
	const uint8_t *y_plane;
	const uint8_t *vu_plane;
	CVI_U32 w, h, y_stride, vu_stride;
	int x, y;

	if (!frame || !dst)
		return -1;

	if (!frame->pu8VirAddr[0] || !frame->pu8VirAddr[1])
		return -1;

	w = frame->u32Width;
	h = frame->u32Height;
	if (w == 0 || h == 0)
		return -1;

	y_stride = frame->u32Stride[0];
	vu_stride = frame->u32Stride[1];
	y_plane = frame->pu8VirAddr[0];
	vu_plane = frame->pu8VirAddr[1];

	for (y = 0; y < (int)h; y++) {
		const uint8_t *y_row = y_plane + y * (int)y_stride;
		const uint8_t *vu_row = vu_plane + (y / 2) * (int)vu_stride;
		uint16_t *dst_row = dst + y * (int)w;

		for (x = 0; x < (int)w; x++) {
			uint8_t Y = y_row[x];
			uint8_t U, V;

			read_chroma(vu_row, x, &U, &V);
			dst_row[x] = yuv_to_rgb565(Y, U, V);
		}
	}

	return 0;
}
