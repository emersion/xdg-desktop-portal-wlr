#include "wlr_screencast.h"

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
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
#include <sys/types.h>

#include "ext_image_copy.h"
#include "screencast.h"
#include "pipewire_screencast.h"
#include "xdpw.h"
#include "logger.h"

static void ext_session_buffer_size(void *data,
		struct ext_image_copy_capture_session_v1 *ext_image_copy_capture_session_v1,
		uint32_t width, uint32_t height) {
	struct xdpw_screencast_instance *cast = data;

	cast->pending_constraints.width = width;
	cast->pending_constraints.height = height;
	cast->pending_constraints.dirty = true;
}

static void ext_session_shm_format(void *data,
		struct ext_image_copy_capture_session_v1 *ext_image_copy_capture_session_v1,
		uint32_t format) {
	struct xdpw_screencast_instance *cast = data;

	uint32_t fourcc = xdpw_format_drm_fourcc_from_wl_shm(format);

	char *fmt_name = drmGetFormatName(fourcc);
	struct xdpw_shm_format *fmt;
	wl_array_for_each(fmt, &cast->pending_constraints.shm_formats) {
		if (fmt->fourcc == fourcc) {
			logprint(TRACE, "ext: skipping duplicated format: %s (%X)", fmt_name, fourcc);
			goto done;
		}
	}
	if (xdpw_bpp_from_drm_fourcc(fourcc) <= 0) {
		logprint(WARN, "ext: unsupported shm format: %s (%X)", fmt_name, fourcc);
		goto done;
	}

	fmt = wl_array_add(&cast->pending_constraints.shm_formats, sizeof(*fmt));
	if (fmt == NULL) {
		logprint(WARN, "ext: allocation for shm format %s (%X) failed", fmt_name, fourcc);
		goto done;
	}
	fmt->fourcc = fourcc;
	// Stride will be calculated when session_done is received
	fmt->stride = 0;
	cast->pending_constraints.dirty = true;
	logprint(TRACE, "ext: shm_format: %s (%X)", fmt_name, fourcc);

done:
	free(fmt_name);
}

static void ext_session_dmabuf_device(void *data,
		struct ext_image_copy_capture_session_v1 *ext_image_copy_capture_session_v1,
		struct wl_array *device_arr) {
	struct xdpw_screencast_instance *cast = data;

	dev_t device;
	assert(device_arr->size == sizeof(device));
	memcpy(&cast->pending_constraints.dmabuf_device, device_arr->data, sizeof(device));
	cast->pending_constraints.dirty = true;
	logprint(TRACE, "ext: dmabuf_device handler");
}

static void ext_session_dmabuf_format(void *data,
		struct ext_image_copy_capture_session_v1 *ext_image_copy_capture_session_v1,
		uint32_t format, struct wl_array *modifiers) {
	struct xdpw_screencast_instance *cast = data;

	char *fmt_name = drmGetFormatName(format);
	uint64_t *modifier;
	wl_array_for_each(modifier, modifiers) {
		struct xdpw_format_modifier_pair *fm_pair;
		bool new = true;
		wl_array_for_each(fm_pair, &cast->pending_constraints.dmabuf_format_modifier_pairs) {
			if (fm_pair->fourcc == format && fm_pair->modifier == *modifier) {
				new = false;
				break;
			}
		}
		if (!new) {
			logprint(TRACE, "ext: skipping duplicated format %s (%X, %lu)", fmt_name, format, *modifier);
			continue;
		}

		fm_pair = wl_array_add(&cast->pending_constraints.dmabuf_format_modifier_pairs, sizeof(*fm_pair));
		fm_pair->fourcc = format;
		fm_pair->modifier = *modifier;

		char *modifier_name = drmGetFormatModifierName(*modifier);
		logprint(TRACE, "ext: dmabuf_format handler: %s (%X), modifier: %s (%X)", fmt_name, format, modifier_name, *modifier);
		free(modifier_name);
	}

	cast->pending_constraints.dirty = true;
	free(fmt_name);
}

static void ext_session_done(void *data,
		struct ext_image_copy_capture_session_v1 *ext_image_copy_capture_session_v1) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "ext: done handler");

	// We can only calculate the stride now we have both formats and width
	struct xdpw_shm_format *fmt;
	wl_array_for_each(fmt, &cast->pending_constraints.shm_formats) {
		int bpp = xdpw_bpp_from_drm_fourcc(fmt->fourcc);
		assert(bpp > 0);
		fmt->stride = bpp * cast->pending_constraints.width;
	}

	if (xdpw_buffer_constraints_move(&cast->current_constraints, &cast->pending_constraints)) {
		logprint(DEBUG, "ext: buffer constraints changed");
		xdpw_gbm_device_update(cast);
		pwr_update_stream_param(cast);
		return;
	}

	if (!cast->initialized) {
		return;
	}
}

static void ext_session_stopped(void *data,
		struct ext_image_copy_capture_session_v1 *ext_image_copy_capture_session_v1) {
	struct xdpw_screencast_instance *cast = data;

	xdpw_screencast_instance_destroy(cast);
	logprint(TRACE, "ext: session_stopped handler");
}

static const struct ext_image_copy_capture_session_v1_listener ext_session_listener = {
	.buffer_size = ext_session_buffer_size,
	.shm_format = ext_session_shm_format,
	.dmabuf_device = ext_session_dmabuf_device,
	.dmabuf_format = ext_session_dmabuf_format,
	.done = ext_session_done,
	.stopped = ext_session_stopped,
};

static void ext_frame_transform(void *data,
		struct ext_image_copy_capture_frame_v1 *ext_image_copy_capture_frame_v1,
		uint32_t transform) {
	struct xdpw_screencast_instance *cast = data;
	logprint(TRACE, "ext: transform handler %u", transform);
	cast->current_frame.transformation = transform;
}

static void ext_frame_damage(void *data,
		struct ext_image_copy_capture_frame_v1 *ext_image_copy_capture_frame_v1,
		int32_t x, int32_t y, int32_t width, int32_t height) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "ext: damage: %"PRId32",%"PRId32"x%"PRId32",%"PRId32, x, y, width, height);

	// Our damage tracking
	struct xdpw_buffer *buffer;
	wl_list_for_each(buffer, &cast->buffer_list, link) {
		struct xdpw_frame_damage *damage = wl_array_add(&buffer->damage, sizeof(*damage));
		*damage = (struct xdpw_frame_damage){ .x = x, .y = y, .width = width, .height = height };
	}
}

static void ext_frame_presentation_time(void *data,
		struct ext_image_copy_capture_frame_v1 *ext_image_copy_capture_frame_v1,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct xdpw_screencast_instance *cast = data;

	cast->current_frame.tv_sec = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo);
	cast->current_frame.tv_nsec = tv_nsec;
	logprint(TRACE, "ext: timestamp %"PRIu64":%"PRIu32, cast->current_frame.tv_sec, cast->current_frame.tv_nsec);
}

static void ext_frame_ready(void *data,
		struct ext_image_copy_capture_frame_v1 *ext_image_copy_capture_frame_v1) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "ext: ready event handler");

	if (cast->ext_session.frame) {
		ext_image_copy_capture_frame_v1_destroy(cast->ext_session.frame);
		cast->ext_session.frame = NULL;
	}

	struct xdpw_buffer *buffer = cast->current_frame.xdpw_buffer;
	cast->current_frame.completed = true;
	xdpw_pwr_enqueue_buffer(cast);
	if (buffer) {
		// Clear damage for the buffer that was just submitted
		buffer->damage.size = 0;
	}
}

static void ext_frame_failed(void *data,
		struct ext_image_copy_capture_frame_v1 *ext_image_copy_capture_frame_v1,
		uint32_t reason) {
	struct xdpw_screencast_instance *cast = data;

	if (cast->ext_session.frame) {
		ext_image_copy_capture_frame_v1_destroy(cast->ext_session.frame);
		cast->ext_session.frame = NULL;
	}

	switch (reason) {
	case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN:
		logprint(ERROR, "ext: frame capture failed: unknown reason");
		xdpw_screencast_instance_destroy(cast);
		return;
	case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
		logprint(ERROR, "ext: frame capture failed: buffer constraint mismatch");
		xdpw_pwr_enqueue_buffer(cast);
		return;
	case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
		logprint(INFO, "ext: frame capture failed: capture session stopped");
		xdpw_screencast_instance_destroy(cast);
		return;
	default:
		abort();
	}
}

static const struct ext_image_copy_capture_frame_v1_listener ext_frame_listener = {
	.transform = ext_frame_transform,
	.damage = ext_frame_damage,
	.presentation_time = ext_frame_presentation_time,
	.ready = ext_frame_ready,
	.failed = ext_frame_failed,
};

static int ext_register_session_cb(struct xdpw_screencast_instance *cast) {
	struct ext_image_capture_source_v1 *source = NULL;
	switch (cast->target->type) {
	case MONITOR:
		if (cast->ctx->ext_output_image_capture_source_manager == NULL) {
			logprint(INFO, "ext: screencast output: unsupported");
			return -1;
		}
		source = ext_output_image_capture_source_manager_v1_create_source(
			cast->ctx->ext_output_image_capture_source_manager,
			cast->target->output->output);
		break;
	case WINDOW:
		if (cast->ctx->ext_foreign_toplevel_image_capture_source_manager == NULL) {
			logprint(INFO, "ext: screencast window: unsupported");
			return -1;
		}
		source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
			cast->ctx->ext_foreign_toplevel_image_capture_source_manager,
			cast->target->toplevel->handle);
		break;
	}
	assert(source != NULL);

	cast->ext_session.capture_session = ext_image_copy_capture_manager_v1_create_session(
			cast->ctx->ext_image_copy_capture_manager, source,
			cast->target->with_cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0);
	ext_image_copy_capture_session_v1_add_listener(cast->ext_session.capture_session,
			&ext_session_listener, cast);
	logprint(TRACE, "ext: session callbacks registered");
	return 0;
}

static void ext_register_frame_cb(struct xdpw_screencast_instance *cast) {
	if (!cast->ext_session.capture_session) {
		if (ext_register_session_cb(cast) != 0) {
			logprint(ERROR, "ext: failed to register session");
			return;
		}
	}
	cast->ext_session.frame = ext_image_copy_capture_session_v1_create_frame(
			cast->ext_session.capture_session);
	ext_image_copy_capture_frame_v1_add_listener(cast->ext_session.frame,
			&ext_frame_listener, cast);

	ext_image_copy_capture_frame_v1_attach_buffer(cast->ext_session.frame,
			cast->current_frame.xdpw_buffer->buffer);
	struct xdpw_buffer *buffer;
	wl_list_for_each(buffer, &cast->buffer_list, link) {
		struct xdpw_frame_damage *damage;
		wl_array_for_each(damage, &buffer->damage) {
			ext_image_copy_capture_frame_v1_damage_buffer(
					cast->ext_session.frame, damage->x, damage->y, damage->width, damage->height);
		}
	}
	ext_image_copy_capture_frame_v1_capture(cast->ext_session.frame);

	logprint(TRACE, "ext: frame callbacks registered");
}

void xdpw_ext_ic_frame_capture(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "ext: start screencopy");
	if (cast->current_frame.xdpw_buffer == NULL) {
		logprint(ERROR, "ext: started frame without buffer");
		return;
	}

	ext_register_frame_cb(cast);
}

void xdpw_ext_ic_session_close(struct xdpw_screencast_instance *cast) {
	if (cast->ext_session.frame) {
		ext_image_copy_capture_frame_v1_destroy(cast->ext_session.frame);
		cast->ext_session.frame = NULL;
	}
	if (cast->ext_session.capture_session) {
		ext_image_copy_capture_session_v1_destroy(cast->ext_session.capture_session);
		cast->ext_session.capture_session = NULL;
	}
}

int xdpw_ext_ic_session_init(struct xdpw_screencast_instance *cast) {
	if (cast->ctx->ext_image_copy_capture_manager == NULL) {
		logprint(INFO, "ext: unsupported");
		return -1;
	}
	if (ext_register_session_cb(cast) != 0) {
		return -1;
	}

	// process at least one frame so that we know
	// some of the metadata required for the pipewire
	// remote state connected event
	return wl_display_roundtrip(cast->ctx->state->wl_display);
}
