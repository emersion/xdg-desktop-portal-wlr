#include "wlr_screencopy.h"
#include "wlr_screencast.h"
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

static void wlr_frame_buffer_destroy(struct xdpw_screencast_instance *cast) {
	// Even though this check may be deemed unnecessary,
	// this has been found to cause SEGFAULTs, like this one:
	// https://github.com/emersion/xdg-desktop-portal-wlr/issues/50
	if (cast->xdpw_frames.screencopy_frame.data != NULL) {
		munmap(cast->xdpw_frames.screencopy_frame.data, cast->xdpw_frames.screencopy_frame.size);
		cast->xdpw_frames.screencopy_frame.data = NULL;
	}

	if (cast->xdpw_frames.screencopy_frame.buffer != NULL) {
		wl_buffer_destroy(cast->xdpw_frames.screencopy_frame.buffer);
		cast->xdpw_frames.screencopy_frame.buffer = NULL;
	}
}

void xdpw_wlr_screencopy_frame_free(struct xdpw_screencast_instance *cast) {
	zwlr_screencopy_frame_v1_destroy(cast->wlr_frame);
	cast->wlr_frame = NULL;
	if (cast->quit || cast->err) {
		wlr_frame_buffer_destroy(cast);
		logprint(TRACE, "xdpw: xdpw_frames.screencopy_frame buffer destroyed");
	}
	logprint(TRACE, "wlroots: frame destroyed");
}

static void wlr_frame_buffer_chparam(struct xdpw_screencast_instance *cast,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {

	logprint(DEBUG, "wlroots: reset buffer");
	cast->xdpw_frames.screencopy_frame.width = width;
	cast->xdpw_frames.screencopy_frame.height = height;
	cast->xdpw_frames.screencopy_frame.stride = stride;
	cast->xdpw_frames.screencopy_frame.size = stride * height;
	cast->xdpw_frames.screencopy_frame.format = format;
	wlr_frame_buffer_destroy(cast);
}

static void wlr_frame_linux_dmabuf(void *data,
		struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height) {
	//struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: linux_dmabuf event handler");
}

static void wlr_frame_buffer_done(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: buffer_done event handler");
	zwlr_screencopy_frame_v1_copy_with_damage(frame, cast->xdpw_frames.screencopy_frame.buffer);
	logprint(TRACE, "wlroots: frame copied");
}

static void wlr_frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: buffer event handler");
	cast->wlr_frame = frame;
	if (cast->xdpw_frames.screencopy_frame.width != width ||
			cast->xdpw_frames.screencopy_frame.height != height ||
			cast->xdpw_frames.screencopy_frame.stride != stride ||
			cast->xdpw_frames.screencopy_frame.format != format) {
		logprint(TRACE, "wlroots: buffer properties changed");
		wlr_frame_buffer_chparam(cast, format, width, height, stride);
	}

	if (cast->xdpw_frames.screencopy_frame.buffer == NULL) {
		logprint(DEBUG, "wlroots: create shm buffer");
		cast->xdpw_frames.screencopy_frame.buffer = wlr_create_shm_buffer(cast, format, width, height,
			stride, &cast->xdpw_frames.screencopy_frame.data);
	} else {
		logprint(TRACE,"wlroots: shm buffer exists");
	}

	if (cast->xdpw_frames.screencopy_frame.buffer == NULL) {
		logprint(ERROR, "wlroots: failed to create buffer");
		abort();
	}

	if (zwlr_screencopy_manager_v1_get_version(cast->ctx->screencopy_manager) < 3) {
		wlr_frame_buffer_done(cast,frame);
	}
}

static void wlr_frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t flags) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: flags event handler");
	cast->xdpw_frames.screencopy_frame.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void wlr_frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: ready event handler");

	cast->xdpw_frames.screencopy_frame.tv_sec = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo);
	cast->xdpw_frames.screencopy_frame.tv_nsec = tv_nsec;

	if (!cast->quit && !cast->err && cast->pwr_stream_state) {
		pw_loop_signal_event(cast->ctx->state->pw_loop, cast->event);
		return ;
	}

	xdpw_wlr_frame_free(cast);
}

static void wlr_frame_failed(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: failed event handler");
	cast->err = true;

	xdpw_wlr_frame_free(cast);
}

static void wlr_frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: damage event handler");

	cast->xdpw_frames.screencopy_frame.damage.x = x;
	cast->xdpw_frames.screencopy_frame.damage.y = y;
	cast->xdpw_frames.screencopy_frame.damage.width = width;
	cast->xdpw_frames.screencopy_frame.damage.height = height;
}

static const struct zwlr_screencopy_frame_v1_listener wlr_frame_listener = {
	.buffer = wlr_frame_buffer,
	.buffer_done = wlr_frame_buffer_done,
	.linux_dmabuf = wlr_frame_linux_dmabuf,
	.flags = wlr_frame_flags,
	.ready = wlr_frame_ready,
	.failed = wlr_frame_failed,
	.damage = wlr_frame_damage,
};

void xdpw_wlr_screencopy_register_cb(struct xdpw_screencast_instance *cast) {

	cast->frame_callback = zwlr_screencopy_manager_v1_capture_output(
		cast->ctx->screencopy_manager, cast->with_cursor, cast->target_output->output);

	zwlr_screencopy_frame_v1_add_listener(cast->frame_callback,
		&wlr_frame_listener, cast);
	logprint(TRACE, "wlroots: callbacks registered");
}

void wlr_registry_handle_add_screencopy(void *data, struct wl_registry *reg,
		uint32_t id, const char *interface, uint32_t ver) {
	struct xdpw_screencast_context *ctx = data;

	if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
		uint32_t version = SC_MANAGER_VERSION < ver ? SC_MANAGER_VERSION : ver;
		ctx->state->screencast_version = version;
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, version);
		ctx->screencopy_manager = wl_registry_bind(
			reg, id, &zwlr_screencopy_manager_v1_interface, version);
	}
}

int xdpw_wlr_screencopy_init(struct xdpw_state *state) {
	struct xdpw_screencast_context *ctx = &state->screencast;

	// make sure our wlroots supports screencopy protocol
	if (!ctx->screencopy_manager) {
		logprint(ERROR, "Compositor doesn't support %s!",
			zwlr_screencopy_manager_v1_interface.name);
		return -1;
	}

	return 0;
}

void xdpw_wlr_screencopy_finish(struct xdpw_screencast_context *ctx) {
	if (ctx->screencopy_manager) {
		zwlr_screencopy_manager_v1_destroy(ctx->screencopy_manager);
	}
}
