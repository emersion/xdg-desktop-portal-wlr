#include "xdpw.h"
#include "screencast_common.h"
#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libdrm/drm_fourcc.h>
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include "logger.h"

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

struct xdpw_gbm_device *xdpw_gbm_device_create(drmDevice *device) {
	if (!(device->available_nodes & (1 << DRM_NODE_RENDER))) {
		logprint(ERROR, "xdpw: DRM device has no render node");
		return NULL;
	}

	struct xdpw_gbm_device *dev = calloc(1, sizeof(*dev));
	if (!dev) {
		return NULL;
	}

	const char *render_node = device->nodes[DRM_NODE_RENDER];
	logprint(INFO, "xdpw: Using render node %s", render_node);

	int fd = open(render_node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		logprint(ERROR, "xdpw: Could not open render node %s", render_node);
		free(dev);
		return NULL;
	}

	dev->gbm = gbm_create_device(fd);
	if (dev->gbm == NULL) {
		free(dev);
		close(fd);
		return NULL;
	}
	dev->dmabuf_device = device;
	dev->refcnt = 1;

	return dev;
}

struct xdpw_gbm_device *xdpw_gbm_device_ref(struct xdpw_gbm_device *gbm) {
	gbm->refcnt++;
	return gbm;
}

void xdpw_gbm_device_unref(struct xdpw_gbm_device *gbm) {
	if (!gbm || --gbm->refcnt > 0) {
		return;
	}

	int fd = gbm_device_get_fd(gbm->gbm);
	gbm_device_destroy(gbm->gbm);
	drmFreeDevice(&gbm->dmabuf_device);
	close(fd);
}

struct gbm_device *xdpw_get_gbm(struct xdpw_screencast_instance *cast) {
	if (cast->current_constraints.gbm) {
		// ext_image_copy_capture
		return cast->current_constraints.gbm->gbm;
	} else if (cast->ctx->gbm) {
		// wlr_screencast
		return cast->ctx->gbm->gbm;
	}
	return NULL;
}

static int anonymous_shm_open(void) {
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

static struct wl_buffer *import_wl_shm_buffer(struct xdpw_screencast_instance *cast, int fd,
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

struct xdpw_buffer *xdpw_buffer_create(struct xdpw_screencast_instance *cast,
		enum buffer_type buffer_type) {
	struct xdpw_buffer *buffer = calloc(1, sizeof(struct xdpw_buffer));

	uint32_t format = xdpw_format_drm_fourcc_from_pw_format(cast->pwr_format.format);
	assert(format != DRM_FORMAT_INVALID);

	buffer->width = cast->current_constraints.width;
	buffer->height = cast->current_constraints.height;
	buffer->buffer_type = buffer_type;
	buffer->format = format;
	wl_array_init(&buffer->damage);

	struct gbm_device *gbm = xdpw_get_gbm(cast);

	switch (buffer_type) {
	case WL_SHM:;
		struct xdpw_shm_format *fmt;
		bool found = false;
		wl_array_for_each(fmt, &cast->current_constraints.shm_formats) {
			if (fmt->fourcc == format) {
				found = true;
				break;
			}
		}
		if (!found) {
			logprint(ERROR, "xdpw: unable to find format: %d", format);
			xdpw_buffer_destroy(buffer);
			return NULL;

		}

		buffer->plane_count = 1;
		buffer->size[0] = fmt->stride * buffer->height;
		buffer->stride[0] = fmt->stride;
		buffer->offset[0] = 0;
		buffer->fd[0] = anonymous_shm_open();
		if (buffer->fd[0] == -1) {
			logprint(ERROR, "xdpw: unable to create anonymous filedescriptor");
			xdpw_buffer_destroy(buffer);
			return NULL;
		}

		if (ftruncate(buffer->fd[0], buffer->size[0]) < 0) {
			logprint(ERROR, "xdpw: unable to truncate filedescriptor");
			xdpw_buffer_destroy(buffer);
			return NULL;
		}

		buffer->buffer = import_wl_shm_buffer(cast, buffer->fd[0], xdpw_format_wl_shm_from_drm_fourcc(format),
			buffer->width, buffer->height, fmt->stride);
		if (buffer->buffer == NULL) {
			logprint(ERROR, "xdpw: unable to create wl_buffer");
			close(buffer->fd[0]);
			free(buffer);
			return NULL;
		}
		break;
	case DMABUF:;
		struct gbm_bo *bo;
		uint32_t flags = GBM_BO_USE_RENDERING;
		if (cast->pwr_format.modifier != DRM_FORMAT_MOD_INVALID) {
			uint64_t *modifiers = (uint64_t*)&cast->pwr_format.modifier;
			bo = gbm_bo_create_with_modifiers2(gbm, buffer->width, buffer->height,
				format, modifiers, 1, flags);
		} else {
			if (cast->ctx->state->config->screencast_conf.force_mod_linear) {
				flags |= GBM_BO_USE_LINEAR;
			}
			bo = gbm_bo_create(gbm, buffer->width, buffer->height, format, flags);
		}

		// Fallback for linear buffers via the implicit api
		if (bo == NULL && cast->pwr_format.modifier == DRM_FORMAT_MOD_LINEAR) {
			bo = gbm_bo_create(gbm, buffer->width, buffer->height,
				format, flags | GBM_BO_USE_LINEAR);
		}

		if (bo == NULL) {
			logprint(ERROR, "xdpw: failed to create gbm_bo");
			xdpw_buffer_destroy(buffer);
			return NULL;
		}
		buffer->plane_count = gbm_bo_get_plane_count(bo);

		struct zwp_linux_buffer_params_v1 *params;
		params = zwp_linux_dmabuf_v1_create_params(cast->ctx->linux_dmabuf);
		if (!params) {
			logprint(ERROR, "xdpw: failed to create linux_buffer_params");
			gbm_bo_destroy(bo);
			xdpw_buffer_destroy(buffer);
			return NULL;
		}

		buffer->modifier = gbm_bo_get_modifier(bo);
		for (int plane = 0; plane < buffer->plane_count; plane++) {
			buffer->size[plane] = 0;
			buffer->stride[plane] = gbm_bo_get_stride_for_plane(bo, plane);
			buffer->offset[plane] = gbm_bo_get_offset(bo, plane);
			buffer->fd[plane] = gbm_bo_get_fd_for_plane(bo, plane);

			if (buffer->fd[plane] < 0) {
				logprint(ERROR, "xdpw: failed to get file descriptor");
				zwp_linux_buffer_params_v1_destroy(params);
				gbm_bo_destroy(bo);
				xdpw_buffer_destroy(buffer);
				return NULL;
			}

			zwp_linux_buffer_params_v1_add(params, buffer->fd[plane], plane,
				buffer->offset[plane], buffer->stride[plane], buffer->modifier >> 32,
				buffer->modifier & 0xffffffff);
		}
		buffer->buffer = zwp_linux_buffer_params_v1_create_immed(params,
			buffer->width, buffer->height,
			buffer->format, /* flags */ 0);
		zwp_linux_buffer_params_v1_destroy(params);
		gbm_bo_destroy(bo);

		if (!buffer->buffer) {
			logprint(ERROR, "xdpw: failed to create buffer");
			xdpw_buffer_destroy(buffer);
			return NULL;
		}
	}

	return buffer;
}

void xdpw_buffer_destroy(struct xdpw_buffer *buffer) {
	if (buffer->buffer) {
		wl_buffer_destroy(buffer->buffer);
	}
	for (int plane = 0; plane < buffer->plane_count; plane++) {
		close(buffer->fd[plane]);
	}
	wl_array_release(&buffer->damage);
	free(buffer);
}

enum wl_shm_format xdpw_format_wl_shm_from_drm_fourcc(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_ARGB8888:
		return WL_SHM_FORMAT_ARGB8888;
	case DRM_FORMAT_XRGB8888:
		return WL_SHM_FORMAT_XRGB8888;
	default:
		return (enum wl_shm_format)format;
	}
}

uint32_t xdpw_format_drm_fourcc_from_wl_shm(enum wl_shm_format format) {
	switch (format) {
	case WL_SHM_FORMAT_ARGB8888:
		return DRM_FORMAT_ARGB8888;
	case WL_SHM_FORMAT_XRGB8888:
		return DRM_FORMAT_XRGB8888;
	default:
		return (uint32_t)format;
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
	case DRM_FORMAT_XRGB2101010:
		return SPA_VIDEO_FORMAT_xRGB_210LE;
	case DRM_FORMAT_XBGR2101010:
		return SPA_VIDEO_FORMAT_xBGR_210LE;
	case DRM_FORMAT_RGBX1010102:
		return SPA_VIDEO_FORMAT_RGBx_102LE;
	case DRM_FORMAT_BGRX1010102:
		return SPA_VIDEO_FORMAT_BGRx_102LE;
	case DRM_FORMAT_ARGB2101010:
		return SPA_VIDEO_FORMAT_ARGB_210LE;
	case DRM_FORMAT_ABGR2101010:
		return SPA_VIDEO_FORMAT_ABGR_210LE;
	case DRM_FORMAT_RGBA1010102:
		return SPA_VIDEO_FORMAT_RGBA_102LE;
	case DRM_FORMAT_BGRA1010102:
		return SPA_VIDEO_FORMAT_BGRA_102LE;
	case DRM_FORMAT_BGR888:
		return SPA_VIDEO_FORMAT_RGB;
	case DRM_FORMAT_RGB888:
		return SPA_VIDEO_FORMAT_BGR;
	default:
		return SPA_VIDEO_FORMAT_UNKNOWN;
	}
}

int xdpw_bpp_from_drm_fourcc(uint32_t format) {
	switch (format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_BGRA1010102:
		return 4;
	case DRM_FORMAT_BGR888:
	case DRM_FORMAT_RGB888:
		return 3;
	default:
		return -1;
	}
}

uint32_t xdpw_format_drm_fourcc_from_pw_format(enum spa_video_format format) {
	switch (format) {
	case SPA_VIDEO_FORMAT_BGRA:
		return DRM_FORMAT_ARGB8888;
	case SPA_VIDEO_FORMAT_BGRx:
		return DRM_FORMAT_XRGB8888;
	case SPA_VIDEO_FORMAT_ABGR:
		return DRM_FORMAT_RGBA8888;
	case SPA_VIDEO_FORMAT_xBGR:
		return DRM_FORMAT_RGBX8888;
	case SPA_VIDEO_FORMAT_RGBA:
		return DRM_FORMAT_ABGR8888;
	case SPA_VIDEO_FORMAT_RGBx:
		return DRM_FORMAT_XBGR8888;
	case SPA_VIDEO_FORMAT_ARGB:
		return DRM_FORMAT_BGRA8888;
	case SPA_VIDEO_FORMAT_xRGB:
		return DRM_FORMAT_BGRX8888;
	case SPA_VIDEO_FORMAT_NV12:
		return DRM_FORMAT_NV12;
	case SPA_VIDEO_FORMAT_xRGB_210LE:
		return DRM_FORMAT_XRGB2101010;
	case SPA_VIDEO_FORMAT_xBGR_210LE:
		return DRM_FORMAT_XBGR2101010;
	case SPA_VIDEO_FORMAT_RGBx_102LE:
		return DRM_FORMAT_RGBX1010102;
	case SPA_VIDEO_FORMAT_BGRx_102LE:
		return DRM_FORMAT_BGRX1010102;
	case SPA_VIDEO_FORMAT_ARGB_210LE:
		return DRM_FORMAT_ARGB2101010;
	case SPA_VIDEO_FORMAT_ABGR_210LE:
		return DRM_FORMAT_ABGR2101010;
	case SPA_VIDEO_FORMAT_RGBA_102LE:
		return DRM_FORMAT_RGBA1010102;
	case SPA_VIDEO_FORMAT_BGRA_102LE:
		return DRM_FORMAT_BGRA1010102;
	case SPA_VIDEO_FORMAT_RGB:
		return DRM_FORMAT_BGR888;
	case SPA_VIDEO_FORMAT_BGR:
		return DRM_FORMAT_RGB888;
	default:
		return DRM_FORMAT_INVALID;
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
	case SPA_VIDEO_FORMAT_ARGB_210LE:
		return SPA_VIDEO_FORMAT_xRGB_210LE;
	case SPA_VIDEO_FORMAT_ABGR_210LE:
		return SPA_VIDEO_FORMAT_xBGR_210LE;
	case SPA_VIDEO_FORMAT_RGBA_102LE:
		return SPA_VIDEO_FORMAT_RGBx_102LE;
	case SPA_VIDEO_FORMAT_BGRA_102LE:
		return SPA_VIDEO_FORMAT_BGRx_102LE;
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

struct xdpw_frame_damage merge_damage(struct xdpw_frame_damage *damage1, struct xdpw_frame_damage *damage2) {
	struct xdpw_frame_damage damage;
	uint32_t x0, y0;
	damage.x = damage1->x < damage2->y ? damage1->x : damage2->x;
	damage.y = damage1->y < damage2->y ? damage1->y : damage2->y;

	x0 = damage1->x + damage1->width < damage2->x + damage2->width ? damage2->x + damage2->width : damage1->x + damage1->width;
	y0 = damage1->y + damage1->height < damage2->y + damage2->height ? damage2->y + damage2->height : damage1->y + damage1->height;
	damage.width = x0 - damage.x;
	damage.height = y0 - damage.y;

	return damage;
}

void xdpw_buffer_constraints_init(struct xdpw_buffer_constraints *constraints) {
	*constraints = (struct xdpw_buffer_constraints){ 0 };
	wl_array_init(&constraints->dmabuf_format_modifier_pairs);
	wl_array_init(&constraints->shm_formats);
}

void xdpw_buffer_constraints_finish(struct xdpw_buffer_constraints *constraints) {
	wl_array_release(&constraints->dmabuf_format_modifier_pairs);
	wl_array_release(&constraints->shm_formats);
	xdpw_gbm_device_unref(constraints->gbm);
	*constraints = (struct xdpw_buffer_constraints){ 0 };
}

bool xdpw_buffer_constraints_move(struct xdpw_buffer_constraints *dst, struct xdpw_buffer_constraints *src) {
	if (!src->dirty) {
		return false;
	}
	int dirty = src->dirty;

	xdpw_buffer_constraints_finish(dst);
	*dst = *src;

	xdpw_buffer_constraints_init(src);
	return dirty;
}

uint32_t xdpw_count_dmabuf_modifiers(struct xdpw_screencast_instance *cast, uint32_t drm_format) {
	struct xdpw_buffer_constraints *constraints = &cast->current_constraints;
	struct gbm_device *gbm = xdpw_get_gbm(cast);

	uint32_t modifiers = 0;
	struct xdpw_format_modifier_pair *fm_pair;
	wl_array_for_each(fm_pair, &constraints->dmabuf_format_modifier_pairs) {
		if (fm_pair->fourcc == drm_format &&
				(fm_pair->modifier == DRM_FORMAT_MOD_INVALID ||
				gbm_device_get_format_modifier_plane_count(gbm, fm_pair->fourcc, fm_pair->modifier) > 0))
			modifiers += 1;
	}

	return modifiers;
}

void xdpw_query_dmabuf_modifiers(struct xdpw_screencast_instance *cast, uint32_t drm_format,
		uint64_t *modifiers, uint32_t num_modifiers) {
	struct xdpw_buffer_constraints *constraints = &cast->current_constraints;
	struct gbm_device *gbm = xdpw_get_gbm(cast);

	uint32_t idx = 0;
	struct xdpw_format_modifier_pair *fm_pair;
	wl_array_for_each(fm_pair, &constraints->dmabuf_format_modifier_pairs) {
		if (fm_pair->fourcc == drm_format &&
				(fm_pair->modifier == DRM_FORMAT_MOD_INVALID ||
				gbm_device_get_format_modifier_plane_count(gbm, fm_pair->fourcc, fm_pair->modifier) > 0)) {
			assert(idx < num_modifiers);
			modifiers[idx] = fm_pair->modifier;
			idx++;
		}
	}
}

