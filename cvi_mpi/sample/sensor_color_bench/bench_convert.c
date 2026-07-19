#include "bench_convert.h"

#include <stdint.h>

static int clamp_u8(int v)
{
	if (v < 0)
		return 0;
	if (v > 255)
		return 255;
	return v;
}

static uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t yuv_to_rgb565(int y, int u, int v)
{
	int r = y + ((1436 * (v - 128)) >> 10);
	int g = y - ((352 * (u - 128) + 731 * (v - 128)) >> 10);
	int b = y + ((1814 * (u - 128)) >> 10);

	return pack_rgb565(clamp_u8(r), clamp_u8(g), clamp_u8(b));
}

static void nv21_sample(const VIDEO_FRAME_S *frame, int sx, int sy,
			int *out_y, int *out_u, int *out_v)
{
	const uint8_t *y_plane = frame->pu8VirAddr[0];
	const uint8_t *uv_plane = frame->pu8VirAddr[1];
	CVI_U32 y_stride = frame->u32Stride[0];
	CVI_U32 uv_stride = frame->u32Stride[1];
	int uv_x = (sx / 2) * 2;
	int uv_y = sy / 2;

	*out_y = y_plane[sy * (int)y_stride + sx];
	if (frame->enPixelFormat == PIXEL_FORMAT_NV12) {
		*out_u = uv_plane[uv_y * (int)uv_stride + uv_x];
		*out_v = uv_plane[uv_y * (int)uv_stride + uv_x + 1];
	} else {
		/* NV21 default: V then U */
		*out_v = uv_plane[uv_y * (int)uv_stride + uv_x];
		*out_u = uv_plane[uv_y * (int)uv_stride + uv_x + 1];
	}
}

int bench_nv21_scale_to_rgb565(const VIDEO_FRAME_S *frame, uint16_t *dst,
			       int dst_w, int dst_h)
{
	CVI_U32 src_w, src_h;
	int dy, dx;

	if (!frame || !dst || dst_w <= 0 || dst_h <= 0)
		return -1;
	if (!frame->pu8VirAddr[0] || !frame->pu8VirAddr[1])
		return -1;

	src_w = frame->u32Width;
	src_h = frame->u32Height;
	if (src_w == 0 || src_h == 0)
		return -1;

	for (dy = 0; dy < dst_h; dy++) {
		int sy = (int)((CVI_U64)dy * src_h / (CVI_U32)dst_h);
		uint16_t *row = dst + dy * dst_w;

		if (sy >= (int)src_h)
			sy = (int)src_h - 1;

		for (dx = 0; dx < dst_w; dx++) {
			int sx = (int)((CVI_U64)dx * src_w / (CVI_U32)dst_w);
			int y, u, v;

			if (sx >= (int)src_w)
				sx = (int)src_w - 1;

			nv21_sample(frame, sx, sy, &y, &u, &v);
			row[dx] = yuv_to_rgb565(y, u, v);
		}
	}

	return 0;
}

int bench_rgb888_scale_to_rgb565(const VIDEO_FRAME_S *frame, uint16_t *dst,
				 int dst_w, int dst_h)
{
	const uint8_t *src;
	CVI_U32 src_w, src_h, stride;
	int dy, dx;

	if (!frame || !dst || dst_w <= 0 || dst_h <= 0)
		return -1;
	if (frame->enPixelFormat != PIXEL_FORMAT_RGB_888)
		return -1;
	if (!frame->pu8VirAddr[0])
		return -1;

	src_w = frame->u32Width;
	src_h = frame->u32Height;
	stride = frame->u32Stride[0];
	if (src_w == 0 || src_h == 0)
		return -1;

	src = frame->pu8VirAddr[0];

	for (dy = 0; dy < dst_h; dy++) {
		int sy = (int)((CVI_U64)dy * src_h / (CVI_U32)dst_h);
		uint16_t *row = dst + dy * dst_w;

		if (sy >= (int)src_h)
			sy = (int)src_h - 1;

		for (dx = 0; dx < dst_w; dx++) {
			int sx = (int)((CVI_U64)dx * src_w / (CVI_U32)dst_w);
			const uint8_t *px;

			if (sx >= (int)src_w)
				sx = (int)src_w - 1;

			px = src + sy * (int)stride + sx * 3;
			row[dx] = pack_rgb565(px[0], px[1], px[2]);
		}
	}

	return 0;
}
