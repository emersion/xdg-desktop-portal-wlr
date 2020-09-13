#include "wlr_screencast_common.h"

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "screencast.h"
#include "pipewire_screencast.h"
#include "xdpw.h"
#include "logger.h"

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include <gbm.h>
#include <xf86drm.h>

void wlr_destroy_shm_buffer(struct wl_buffer *buffer, void *data, int size) {
	// Even though this check may be deemed unnecessary,
	// this has been found to cause SEGFAULTs, like this one:
	// https://github.com/emersion/xdg-desktop-portal-wlr/issues/50
	if (data != NULL) {
		munmap(data, size);
		data = NULL;
	}

	if (buffer != NULL) {
		wl_buffer_destroy(buffer);
		buffer = NULL;
	}
}

struct wl_buffer *wlr_create_shm_buffer(struct xdpw_screencast_instance *cast,
		enum wl_shm_format fmt, int width, int height, int stride,
		void **data_out) {
	struct xdpw_screencast_context *ctx = cast->ctx;
	int size = stride * height;

	int fd = anonymous_shm_open();
	if (fd < 0) {
		logprint(ERROR, "wlroots: shm_open failed");
		return NULL;
	}

	int ret;
	while ((ret = ftruncate(fd, size)) == EINTR);

	if (ret < 0) {
		close(fd);
		logprint(ERROR, "wlroots: ftruncate failed");
		return NULL;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		logprint(ERROR, "wlroots: mmap failed: %m");
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(ctx->shm, fd, size);
	close(fd);
	struct wl_buffer *buffer =
		wl_shm_pool_create_buffer(pool, 0, width, height, stride, fmt);
	wl_shm_pool_destroy(pool);

	*data_out = data;
	return buffer;
}

struct wl_buffer *wlr_create_dmabuf_buffer(struct xdpw_screencast_instance *cast,
		int width, int height, uint32_t fourcc, struct gbm_bo **bo_out) {
	struct gbm_bo *bo;
	bo = gbm_bo_create(cast->ctx->gbm, width, height, fourcc,
		GBM_BO_USE_RENDERING);
	if (bo == NULL) {
		logprint(ERROR, "wlroots: failed to create gbm_bo");
		return NULL;
	}

	struct zwp_linux_buffer_params_v1* params;
	params = zwp_linux_dmabuf_v1_create_params(cast->ctx->linux_dmabuf);
	if (!params) {
		logprint(ERROR, "wlroots: failed to create linux_buffer_params");
		gbm_bo_destroy(bo);
		return NULL;
	}

	uint32_t offset = gbm_bo_get_offset(bo, 0);
	uint32_t stride = gbm_bo_get_stride(bo);
	uint64_t mod = gbm_bo_get_modifier(bo);
	int fd = gbm_bo_get_fd(bo);

	if (fd < 0) {
		logprint(ERROR, "wlroots: failed to get file descriptor");
		zwp_linux_buffer_params_v1_destroy(params);
		gbm_bo_destroy(bo);
		return NULL;
	}

	zwp_linux_buffer_params_v1_add(params, fd, 0, offset, stride,
		mod >> 32, mod & 0xffffffff);
	struct wl_buffer *buffer = zwp_linux_buffer_params_v1_create_immed(params,
		width, height, fourcc, /* flags */ 0);
	zwp_linux_buffer_params_v1_destroy(params);
	close(fd);

	if (!buffer) {
		logprint(ERROR, "wlroots: failed to create buffer");
		gbm_bo_destroy(bo);
	}

	*bo_out = bo;
	return buffer;
}

void wlr_destroy_dmabuf_buffer(struct wl_buffer *buffer, struct gbm_bo *bo) {
	if (buffer != NULL) {
		wl_buffer_destroy(buffer);
		buffer = NULL;
	}
	if (bo) {
		gbm_bo_destroy(bo);
	}
}

static char *gbm_find_render_node(size_t maxlen) {
	drmDevice *devices[64];
	char *render_node = NULL;

	int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
	for (int i = 0; i < n; ++i) {
		drmDevice *dev = devices[i];
		if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
			continue;

		render_node = strndup(dev->nodes[DRM_NODE_RENDER], maxlen);
		break;
	}

	drmFreeDevices(devices, n);
	return render_node;
}

struct gbm_device *create_gbm_device(){
	struct gbm_device *gbm;
	char *render_node = NULL;
	size_t render_node_size = 256;

	render_node = gbm_find_render_node(render_node_size);
	if (render_node == NULL) {
		logprint(ERROR, "xdpw: Could not find render node");
		return NULL;
	} else {
		logprint(INFO, "xdpw: Using render node %s",render_node);
	}

	int fd = open(render_node, O_RDWR);
	if (fd < 0) {
		logprint(ERROR, "xdpw: Could not open render node %s",render_node);
		close(fd);
		return NULL;
	}

	free(render_node);
	gbm = gbm_create_device(fd);
	close(fd);
	return gbm;
}

void destroy_gbm_device(struct gbm_device *gbm) {
	gbm_device_destroy(gbm);
}
