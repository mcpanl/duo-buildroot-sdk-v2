#include "rgb888_rgb565.h"

int rgb888_frame_to_rgb565(const VIDEO_FRAME_S *frame, uint16_t *dst)
{
	const uint8_t *src;
	CVI_U32 w, h, stride;
	int x, y;

	if (!frame || !dst)
		return -1;

	if (frame->enPixelFormat != PIXEL_FORMAT_RGB_888)
		return -1;

	if (!frame->pu8VirAddr[0])
		return -1;

	w = frame->u32Width;
	h = frame->u32Height;
	if (w == 0 || h == 0)
		return -1;

	stride = frame->u32Stride[0];
	src = frame->pu8VirAddr[0];

	for (y = 0; y < (int)h; y++) {
		const uint8_t *src_row = src + y * (int)stride;
		uint16_t *dst_row = dst + y * (int)w;

		for (x = 0; x < (int)w; x++) {
			/*
			 * SG2000 VPSS reports PIXEL_FORMAT_RGB_888 (R-lsb) but the
			 * packed bytes are B-G-R in practice. Taking R-G-B order
			 * swaps red/blue on the LCD.
			 */
			uint8_t B = src_row[x * 3 + 0];
			uint8_t G = src_row[x * 3 + 1];
			uint8_t R = src_row[x * 3 + 2];

			dst_row[x] = (uint16_t)(((R & 0xF8) << 8) |
						 ((G & 0xFC) << 3) |
						 (B >> 3));
		}
	}

	return 0;
}
