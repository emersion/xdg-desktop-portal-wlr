#include "screencast_common.h"

enum spa_video_format pipewire_from_wl_shm(void *data) {
	struct screencast_context *ctx = data;

	if (ctx->forced_pixelformat) {
		if (strcmp(ctx->forced_pixelformat, "BGRx") == 0) {
			return SPA_VIDEO_FORMAT_BGRx;
		}
		if (strcmp(ctx->forced_pixelformat, "RGBx") == 0) {
			return SPA_VIDEO_FORMAT_RGBx;
		}
	}

	switch (ctx->simple_frame.format) {
	case WL_SHM_FORMAT_ARGB8888:
		return SPA_VIDEO_FORMAT_BGRA;
	case WL_SHM_FORMAT_XRGB8888:
		return SPA_VIDEO_FORMAT_BGRx;
	case WL_SHM_FORMAT_RGBA8888:
		return SPA_VIDEO_FORMAT_ABGR;
	case WL_SHM_FORMAT_RGBX8888:
		return SPA_VIDEO_FORMAT_xBGR;
	case WL_SHM_FORMAT_ABGR8888:
		return SPA_VIDEO_FORMAT_RGBA;
	case WL_SHM_FORMAT_XBGR8888:
		return SPA_VIDEO_FORMAT_RGBx;
	case WL_SHM_FORMAT_BGRA8888:
		return SPA_VIDEO_FORMAT_ARGB;
	case WL_SHM_FORMAT_BGRX8888:
		return SPA_VIDEO_FORMAT_xRGB;
	case WL_SHM_FORMAT_NV12:
		return SPA_VIDEO_FORMAT_NV12;
	default:
		abort();
	}
}
