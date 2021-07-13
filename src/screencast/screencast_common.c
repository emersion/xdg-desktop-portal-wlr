#include "screencast_common.h"
#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libdrm/drm_fourcc.h>

void randname(char *buf) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		assert(buf[i] == 'X');
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

int anonymous_shm_open(void) {
	char name[] = "/xdpw-shm-XXXXXX";
	int retries = 100;

	do {
		randname(name + strlen(name) - 6);

		--retries;
		// shm_open guarantees that O_CLOEXEC is set
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);

	return -1;
}

struct wl_buffer *import_shm_buffer(struct xdpw_screencast_instance *cast, int fd,
		enum wl_shm_format fmt, int width, int height, int stride) {
	struct xdpw_screencast_context *ctx = cast->ctx;
	int size = stride * height;

	if (fd < 0) {
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, size);
	struct wl_buffer *buffer =
		wl_shm_pool_create_buffer(pool, 0, width, height, stride, fmt);
	wl_shm_pool_destroy(pool);

	return buffer;
}

bool wlr_query_dmabuf_modifiers(struct xdpw_screencast_context *ctx, uint32_t drm_format,
		uint32_t num_modifiers, uint64_t *modifiers, uint32_t *max_modifiers) {
	if (wl_list_empty(&ctx->format_modifier_pairs))
		return false;
	struct xdpw_format_modifier_pair *fm_pair;
	if (num_modifiers == 0) {
		*max_modifiers = 0;
		wl_list_for_each(fm_pair, &ctx->format_modifier_pairs, link) {
			if (fm_pair->fourcc == drm_format)
				*max_modifiers += 1;
		}
		return true;
	}

	uint32_t i = 0;
	wl_list_for_each(fm_pair, &ctx->format_modifier_pairs, link) {
		if (i == num_modifiers)
			break;
		modifiers[i] = fm_pair->modifier;
		i++;
	}
	*max_modifiers = num_modifiers;
	return true;
}

enum wl_shm_format xdpw_format_wl_shm_from_drm_fourcc(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return WL_SHM_FORMAT_ARGB8888;
	case DRM_FORMAT_XRGB8888:
		return WL_SHM_FORMAT_XRGB8888;
	case DRM_FORMAT_RGBA8888:
		return WL_SHM_FORMAT_RGBA8888;
	case DRM_FORMAT_RGBX8888:
		return WL_SHM_FORMAT_RGBX8888;
	case DRM_FORMAT_ABGR8888:
		return WL_SHM_FORMAT_ABGR8888;
	case DRM_FORMAT_XBGR8888:
		return WL_SHM_FORMAT_XBGR8888;
	case DRM_FORMAT_BGRA8888:
		return WL_SHM_FORMAT_BGRA8888;
	case DRM_FORMAT_BGRX8888:
		return WL_SHM_FORMAT_BGRX8888;
	case DRM_FORMAT_NV12:
		return WL_SHM_FORMAT_NV12;
	default:
		abort();
	}
}

uint32_t xdpw_format_drm_fourcc_from_wl_shm(enum wl_shm_format format) {
	switch (format) {
	case WL_SHM_FORMAT_ARGB8888:
		return DRM_FORMAT_ARGB8888;
	case WL_SHM_FORMAT_XRGB8888:
		return DRM_FORMAT_XRGB8888;
	case WL_SHM_FORMAT_RGBA8888:
		return DRM_FORMAT_RGBA8888;
	case WL_SHM_FORMAT_RGBX8888:
		return DRM_FORMAT_RGBX8888;
	case WL_SHM_FORMAT_ABGR8888:
		return DRM_FORMAT_ABGR8888;
	case WL_SHM_FORMAT_XBGR8888:
		return DRM_FORMAT_XBGR8888;
	case WL_SHM_FORMAT_BGRA8888:
		return DRM_FORMAT_BGRA8888;
	case WL_SHM_FORMAT_BGRX8888:
		return DRM_FORMAT_BGRX8888;
	case WL_SHM_FORMAT_NV12:
		return DRM_FORMAT_NV12;
	default:
		abort();
	}
}

enum spa_video_format xdpw_format_pw_from_drm_fourcc(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return SPA_VIDEO_FORMAT_BGRA;
	case DRM_FORMAT_XRGB8888:
		return SPA_VIDEO_FORMAT_BGRx;
	case DRM_FORMAT_RGBA8888:
		return SPA_VIDEO_FORMAT_ABGR;
	case DRM_FORMAT_RGBX8888:
		return SPA_VIDEO_FORMAT_xBGR;
	case DRM_FORMAT_ABGR8888:
		return SPA_VIDEO_FORMAT_RGBA;
	case DRM_FORMAT_XBGR8888:
		return SPA_VIDEO_FORMAT_RGBx;
	case DRM_FORMAT_BGRA8888:
		return SPA_VIDEO_FORMAT_ARGB;
	case DRM_FORMAT_BGRX8888:
		return SPA_VIDEO_FORMAT_xRGB;
	case DRM_FORMAT_NV12:
		return SPA_VIDEO_FORMAT_NV12;
	default:
		abort();
	}
}

enum spa_video_format xdpw_format_pw_strip_alpha(enum spa_video_format format) {
	switch (format) {
	case SPA_VIDEO_FORMAT_BGRA:
		return SPA_VIDEO_FORMAT_BGRx;
	case SPA_VIDEO_FORMAT_ABGR:
		return SPA_VIDEO_FORMAT_xBGR;
	case SPA_VIDEO_FORMAT_RGBA:
		return SPA_VIDEO_FORMAT_RGBx;
	case SPA_VIDEO_FORMAT_ARGB:
		return SPA_VIDEO_FORMAT_xRGB;
	default:
		return SPA_VIDEO_FORMAT_UNKNOWN;
	}
}

enum xdpw_chooser_types get_chooser_type(const char *chooser_type) {
	if (!chooser_type || strcmp(chooser_type, "default") == 0) {
		return XDPW_CHOOSER_DEFAULT;
	} else if (strcmp(chooser_type, "none") == 0) {
		return XDPW_CHOOSER_NONE;
	} else if (strcmp(chooser_type, "simple") == 0) {
		return XDPW_CHOOSER_SIMPLE;
	} else if (strcmp(chooser_type, "dmenu") == 0) {
		return XDPW_CHOOSER_DMENU;
	}
	fprintf(stderr, "Could not understand chooser type %s\n", chooser_type);
	exit(1);
}

const char *chooser_type_str(enum xdpw_chooser_types chooser_type) {
	switch (chooser_type) {
	case XDPW_CHOOSER_DEFAULT:
		return "default";
	case XDPW_CHOOSER_NONE:
		return "none";
	case XDPW_CHOOSER_SIMPLE:
		return "simple";
	case XDPW_CHOOSER_DMENU:
		return "dmenu";
	}
	fprintf(stderr, "Could not find chooser type %d\n", chooser_type);
	abort();
}
