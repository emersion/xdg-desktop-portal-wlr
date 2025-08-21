#include "wlr_screencast.h"

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include <drm_fourcc.h>
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
#include <assert.h>
#include <wayland-client-protocol.h>
#include <xf86drm.h>

#include "screencast.h"
#include "pipewire_screencast.h"
#include "xdpw.h"
#include "logger.h"
#include "fps_limit.h"

static void wlr_frame_finish(struct xdpw_screencast_instance *cast) {
	if (!cast->wlr_session.wlr_frame) {
		return;
	}
	zwlr_screencopy_frame_v1_destroy(cast->wlr_session.wlr_frame);
	cast->wlr_session.wlr_frame = NULL;
	logprint(TRACE, "wlroots: frame destroyed");
}

static void wlr_frame_buffer_done(void *data,
		struct zwlr_screencopy_frame_v1 *frame);

static void wlr_frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "wlroots: buffer event handler");
	cast->wlr_session.wlr_frame = frame;

	struct xdpw_shm_format *fmt = wl_array_add(&cast->pending_constraints.shm_formats, sizeof(*fmt));
	if (fmt == NULL) {
		logprint(WARN, "ext: allocation for shm format failed");
		return;
	}
	fmt->fourcc = xdpw_format_drm_fourcc_from_wl_shm(format);
	fmt->stride = stride;

	cast->pending_constraints.width = width;
	cast->pending_constraints.height = height;
	cast->pending_constraints.dirty = true;

	if (zwlr_screencopy_manager_v1_get_version(cast->ctx->screencopy_manager) < 3) {
		wlr_frame_buffer_done(cast, frame);
	}
}

static void wlr_frame_linux_dmabuf(void *data,
		struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "wlroots: linux_dmabuf event handler");


	struct xdpw_format_modifier_pair *fm_pair;
	wl_array_for_each(fm_pair, &cast->ctx->format_modifier_pairs) {
		if (fm_pair->fourcc != format) {
			continue;
		}

		struct xdpw_format_modifier_pair *new =
			wl_array_add(&cast->pending_constraints.dmabuf_format_modifier_pairs, sizeof(*new));
		assert(new != NULL);
		new->fourcc = fm_pair->fourcc;
		new->modifier = fm_pair->modifier;
	}

	cast->pending_constraints.width = width;
	cast->pending_constraints.height = height;
	cast->pending_constraints.dirty = true;

}

static bool check_constraints(struct xdpw_buffer_constraints *constraints, struct xdpw_buffer *buffer) {
	if (constraints->width != buffer->width || constraints->height != buffer->height) {
		return false;
	}

	switch (buffer->buffer_type) {
	case DMABUF:;
		struct xdpw_format_modifier_pair *fm_pair;
		wl_array_for_each(fm_pair, &constraints->dmabuf_format_modifier_pairs) {
			if (buffer->format == fm_pair->fourcc && buffer->modifier == fm_pair->modifier) {
				return true;
			}
		}
		return false;
	case WL_SHM:;
		struct xdpw_shm_format *format;
		wl_array_for_each(format, &constraints->shm_formats) {
			if (buffer->format == format->fourcc && buffer->stride[0] == format->stride) {
				return true;
			}
		}
		return false;
	default:
		abort();
	}
}

static void wlr_frame_buffer_done(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	xdpw_buffer_constraints_move(&cast->current_constraints, &cast->pending_constraints);

	logprint(TRACE, "wlroots: buffer_done event handler");

	if (!cast->initialized) {
		wlr_frame_finish(cast);
		return;
	}

	if (!cast->current_frame.xdpw_buffer) {
		logprint(WARN, "wlroots: no current buffer");
		xdpw_pwr_enqueue_buffer(cast);
		wlr_frame_finish(cast);
		return;
	}

	if (!check_constraints(&cast->current_constraints, cast->current_frame.xdpw_buffer)) {
		logprint(DEBUG, "wlroots: buffer constraints changed");
		pwr_update_stream_param(cast);
		xdpw_pwr_enqueue_buffer(cast);
		wlr_frame_finish(cast);
		return;
	}

	cast->current_frame.transformation = cast->target->output->transformation;
	logprint(TRACE, "wlroots: transformation %u", cast->current_frame.transformation);

	struct xdpw_buffer *buffer = cast->current_frame.xdpw_buffer;
	buffer->damage.size = 0;

	zwlr_screencopy_frame_v1_copy_with_damage(frame, cast->current_frame.xdpw_buffer->buffer);
	logprint(TRACE, "wlroots: frame copied");

	fps_limit_measure_start(&cast->fps_limit, cast->framerate);
}

static void wlr_frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t flags) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "wlroots: flags event handler");
	cast->current_frame.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void wlr_frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "wlroots: damage event handler");

	struct xdpw_buffer *buffer = cast->current_frame.xdpw_buffer;
	logprint(TRACE, "wlroots: damage %"PRIu32": %"PRIu32",%"PRIu32"x%"PRIu32",%"PRIu32, buffer->damage.size, x, y, width, height);
	struct xdpw_frame_damage *damage = wl_array_add(&buffer->damage, sizeof(*damage));
	*damage = (struct xdpw_frame_damage){ .x = x, .y = y, .width = width, .height = height };
}

static void wlr_frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "wlroots: ready event handler");

	cast->current_frame.tv_sec = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo);
	cast->current_frame.tv_nsec = tv_nsec;
	cast->current_frame.completed = true;
	logprint(TRACE, "wlroots: timestamp %"PRIu64":%"PRIu32, cast->current_frame.tv_sec, cast->current_frame.tv_nsec);

	xdpw_pwr_enqueue_buffer(cast);
	wlr_frame_finish(cast);
}

static void wlr_frame_failed(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "wlroots: failed event handler");

	xdpw_pwr_enqueue_buffer(cast);
	wlr_frame_finish(cast);
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

static void wlr_register_cb(struct xdpw_screencast_instance *cast) {
	assert(cast->target->type == MONITOR);

	struct zwlr_screencopy_frame_v1 *frame = zwlr_screencopy_manager_v1_capture_output(
		cast->ctx->screencopy_manager, cast->target->with_cursor, cast->target->output->output);
	zwlr_screencopy_frame_v1_add_listener(frame, &wlr_frame_listener, cast);
	logprint(TRACE, "wlroots: callbacks registered");
}

void xdpw_wlr_sc_frame_capture(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "wlroots: start screencopy");
	if (cast->initialized && !cast->pwr_stream_state) {
		return;
	}

	wlr_register_cb(cast);
}

void xdpw_wlr_sc_session_close(struct xdpw_screencast_instance *cast) {
	wlr_frame_finish(cast);
}

int xdpw_wlr_sc_session_init(struct xdpw_screencast_instance *cast) {
	wlr_register_cb(cast);

	// process at least one frame so that we know
	// some of the metadata required for the pipewire
	// remote state connected event
	wl_display_dispatch(cast->ctx->state->wl_display);
	wl_display_roundtrip(cast->ctx->state->wl_display);

	return 0;
}
