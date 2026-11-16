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

static void wlr_frame_finish(struct xdpw_screencast_instance *cast) {
	if (!cast->wlr_session.wlr_frame) {
		return;
	}
	zwlr_screencopy_frame_v1_destroy(cast->wlr_session.wlr_frame);
	cast->wlr_session.wlr_frame = NULL;
	logprint(TRACE, "wlroots: frame destroyed");
}

// Ends the current capture cycle: hand the buffer back to PipeWire and drop
// the wlr frame. We drive the graph, so ask for the next cycle ourselves.
static void wlr_frame_done(struct xdpw_screencast_instance *cast) {
	xdpw_pwr_enqueue_buffer(cast);
	wlr_frame_finish(cast);
	pw_stream_trigger_process(cast->stream);
}

static void wlr_frame_buffer_done(void *data,
		struct zwlr_screencopy_frame_v1 *frame);

/*
 * A transformed output is captured in mode space: the compositor hands us a
 * 1920x1080 frame for a portrait output, and the client is expected to
 * rotate it. The transform can be advertised with SPA_META_VideoTransform,
 * but most consumers (Chromium/Electron) ignore that meta, so the video
 * would come out sideways.
 *
 * When the captured size is the transpose of the output's logical size, we
 * publish the logical size instead and rotate the pixels ourselves.
 */
static void wlr_prepare_rotation(struct xdpw_screencast_instance *cast,
		struct xdpw_buffer_constraints *constraints) {
	struct xdpw_wlr_output *output = cast->target->output;
	if (output == NULL) {
		return;
	}
	const uint32_t transform = output->transformation;
	if (transform != WL_OUTPUT_TRANSFORM_90 && transform != WL_OUTPUT_TRANSFORM_270) {
		return;
	}
	struct xdpw_shm_format *candidate;
	if (cast->needs_rotation) {
		constraints->width = output->width;
		constraints->height = output->height;
		wl_array_for_each(candidate, &constraints->shm_formats) {
			if (xdpw_bpp_from_drm_fourcc(candidate->fourcc) == 4) {
				candidate->stride = constraints->width * 4;
			}
		}
		return;
	}
	/* the compositor delivers the frame in mode space */
	if (constraints->width != (uint32_t) output->height ||
			constraints->height != (uint32_t) output->width) {
		return;
	}
	/* only 4 bytes per pixel can be rotated on the CPU */
	struct xdpw_shm_format *fmt = NULL;
	wl_array_for_each(candidate, &constraints->shm_formats) {
		if (xdpw_bpp_from_drm_fourcc(candidate->fourcc) == 4) {
			fmt = candidate;
			break;
		}
	}
	if (fmt == NULL) {
		logprint(WARN, "wlroots: no 4 bytes per pixel format, not rotating");
		return;
	}

	cast->needs_rotation = true;
	/* a dmabuf can not be rotated on the CPU: use SHM */
	cast->avoid_dmabufs = true;
	/*
	 * The captured frame is the one the compositor presents: the output
	 * transform has already been applied to it. To get the frame back in
	 * the output's logical orientation we have to undo that transform.
	 */
	cast->rotation_degrees = (transform == WL_OUTPUT_TRANSFORM_90) ? 270 : 90;
	cast->capture_width = constraints->width;
	cast->capture_height = constraints->height;
	cast->capture_stride = fmt->stride;
	cast->capture_format = fmt->fourcc;

	/* publish the logical size of the output instead */
	constraints->width = output->width;
	constraints->height = output->height;
	wl_array_for_each(candidate, &constraints->shm_formats) {
		if (xdpw_bpp_from_drm_fourcc(candidate->fourcc) == 4) {
			/*
			 * The stride must be the natural one (width * 4): the
			 * consumers compute the linesize of the frame from the
			 * size of the format, not from the stride of the buffer.
			 * A padded stride makes every row drift a few bytes and
			 * the image comes out sheared.
			 */
			candidate->stride = constraints->width * 4;
		}
	}

	logprint(INFO, "wlroots: rotating the captured frames by %u degrees (%ux%u -> %ux%u)",
		cast->rotation_degrees, cast->capture_width, cast->capture_height,
		constraints->width, constraints->height);
}

/* the damage is reported in the captured (mode) space: move it to the published one */
static void wlr_rotate_damage(struct xdpw_screencast_instance *cast) {
	struct xdpw_frame_damage *dmg;
	wl_array_for_each(dmg, &cast->current_frame.damage) {
		const uint32_t x = dmg->x, y = dmg->y, w = dmg->width, h = dmg->height;
		if (cast->rotation_degrees == 90) {
			dmg->x = y;
			dmg->y = cast->capture_width - (x + w);
			dmg->width = h;
			dmg->height = w;
		} else {
			dmg->x = cast->capture_height - (y + h);
			dmg->y = x;
			dmg->width = h;
			dmg->height = w;
		}
	}
}

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
	wlr_prepare_rotation(cast, &cast->pending_constraints);

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
	wlr_prepare_rotation(cast, &cast->pending_constraints);

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
		wlr_frame_done(cast);
		return;
	}

	if (!check_constraints(&cast->current_constraints, cast->current_frame.xdpw_buffer)) {
		logprint(DEBUG, "wlroots: buffer constraints changed");
		pwr_update_stream_param(cast);
		wlr_frame_done(cast);
		return;
	}

	cast->current_frame.transformation = cast->target->output->transformation;
	if (cast->needs_rotation) {
		/*
		 * The frame is rotated before being published, so it already is in
		 * the output's logical orientation: there is nothing left for the
		 * consumer to do.
		 */
		cast->current_frame.transformation = WL_OUTPUT_TRANSFORM_NORMAL;
	}
	logprint(TRACE, "wlroots: transformation %u", cast->current_frame.transformation);

	cast->current_frame.damage.size = 0;

	if (cast->needs_rotation) {
		/*
		 * The compositor copies the frame in mode space, so it needs a
		 * buffer of the captured size; the published buffer has the
		 * logical size and is filled by rotating that frame.
		 */
		if (cast->rotation_source == NULL ||
				cast->rotation_source->width != cast->capture_width ||
				cast->rotation_source->height != cast->capture_height) {
			if (cast->rotation_source != NULL) {
				xdpw_buffer_destroy(cast->rotation_source);
				cast->rotation_source = NULL;
			}
			cast->rotation_source = xdpw_shm_buffer_create(cast,
				cast->capture_width, cast->capture_height,
				cast->capture_stride, cast->capture_format);
			if (cast->rotation_source == NULL) {
				logprint(ERROR, "wlroots: unable to allocate the rotation source");
				cast->needs_rotation = false;
				wlr_frame_done(cast);
				return;
			}
		}
		zwlr_screencopy_frame_v1_copy_with_damage(frame, cast->rotation_source->buffer);
		logprint(TRACE, "wlroots: frame copied to the rotation source");
		return;
	}

	zwlr_screencopy_frame_v1_copy_with_damage(frame, cast->current_frame.xdpw_buffer->buffer);
	logprint(TRACE, "wlroots: frame copied");
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

	logprint(TRACE, "wlroots: damage %"PRIu32": %"PRIu32",%"PRIu32"x%"PRIu32",%"PRIu32,
			cast->current_frame.damage.size, x, y, width, height);
	struct xdpw_frame_damage *damage = wl_array_add(&cast->current_frame.damage, sizeof(*damage));
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

	if (cast->needs_rotation && cast->rotation_source != NULL &&
			cast->current_frame.xdpw_buffer != NULL) {
		if (xdpw_rotate_buffer(cast->rotation_source, cast->current_frame.xdpw_buffer,
				cast->rotation_degrees)) {
			wlr_rotate_damage(cast);
			logprint(TRACE, "wlroots: frame rotated by %u degrees",
				cast->rotation_degrees);
		} else {
			logprint(ERROR, "wlroots: unable to rotate the captured frame");
		}
	}

	wlr_frame_done(cast);
}

static void wlr_frame_failed(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "wlroots: failed event handler");

	wlr_frame_done(cast);
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

void xdpw_wlr_sc_frame_cancel(struct xdpw_screencast_instance *cast) {
	wlr_frame_finish(cast);
}

void xdpw_wlr_sc_session_close(struct xdpw_screencast_instance *cast) {
	xdpw_wlr_sc_frame_cancel(cast);
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
