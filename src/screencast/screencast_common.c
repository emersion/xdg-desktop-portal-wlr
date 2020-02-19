#include "screencast_common.h"

char *strdup(const char *src) {
	char *dst = malloc(strlen(src) + 1); // Space for length plus nul
	if (dst == NULL)
		return NULL;		// No memory
	strcpy(dst, src); // Copy the characters
	return dst;			 // Return the new string
}

uint32_t pipewire_from_wl_shm(void *data) {
	struct screencast_context *ctx = data;

	if(ctx->forced_pixelformat){
		if(strcmp(ctx->forced_pixelformat, "BGRx") == 0) {
			return ctx->type.video_format.BGRx;
		}
		if(strcmp(ctx->forced_pixelformat, "RGBx") == 0){
			return ctx->type.video_format.RGBx;
		}
	}

	switch (ctx->simple_frame.format) {
		case WL_SHM_FORMAT_ARGB8888:
			return ctx->type.video_format.BGRA;
		case WL_SHM_FORMAT_XRGB8888:
			return ctx->type.video_format.BGRx;
		case WL_SHM_FORMAT_RGBA8888:
			return ctx->type.video_format.ABGR;
		case WL_SHM_FORMAT_RGBX8888:
			return ctx->type.video_format.xBGR;
		case WL_SHM_FORMAT_ABGR8888:
			return ctx->type.video_format.RGBA;
		case WL_SHM_FORMAT_XBGR8888:
			return ctx->type.video_format.RGBx;
		case WL_SHM_FORMAT_BGRA8888:
			return ctx->type.video_format.ARGB;
		case WL_SHM_FORMAT_BGRX8888:
			return ctx->type.video_format.xRGB;
		case WL_SHM_FORMAT_NV12:
			return ctx->type.video_format.NV12;
		default:
			exit(EXIT_FAILURE);
	}
}
