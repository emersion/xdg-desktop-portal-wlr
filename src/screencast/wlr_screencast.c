#include "wlr_screencast.h"

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "hyprland-toplevel-export-v1-client-protocol.h"
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
#include <string.h>

#include "screencast.h"
#include "pipewire_screencast.h"
#include "xdpw.h"
#include "logger.h"
#include "fps_limit.h"
//

struct SToplevelEntry {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char name[256];
    char clazz[256];
	struct wl_list link;
};

void handleTitle(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, const char *title) {
    struct xdpw_screencast_context *ctx = data;

    struct SToplevelEntry *current;
    wl_list_for_each(current, &ctx->toplevel_resource_list, link) {
		if (current->handle == handle) {
			strncpy(current->name, title, 255);
			current->name[255] = '\0';
			break;
		}
	}
}

void handleAppID(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, const char *app_id) {
    struct xdpw_screencast_context *ctx = data;

    struct SToplevelEntry *current;
    wl_list_for_each(current, &ctx->toplevel_resource_list, link) {
		if (current->handle == handle) {
			strncpy(current->clazz, app_id, 255);
			current->name[255] = '\0';
            break;
        }
	}
}

void handleOutputEnter(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_output *output) {
    ;  // noop
}

void handleOutputLeave(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_output *output) {
    ;  // noop
}

void handleState(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct wl_array *state) {
    ;  // noop
}

void handleDone(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    ; // noop
}

void handleClosed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    struct xdpw_screencast_context *ctx = data;

    struct SToplevelEntry *current;
    wl_list_for_each(current, &ctx->toplevel_resource_list, link) {
        if (current->handle == handle) {
            break;
        }
    }

	wl_list_remove(&current->link);
}

void handleParent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle, struct zwlr_foreign_toplevel_handle_v1 *parent) {
    ;  // noop
}

struct zwlr_foreign_toplevel_handle_v1_listener toplevelHandleListener = {
    .title = handleTitle,
    .app_id = handleAppID,
    .output_enter = handleOutputEnter,
    .output_leave = handleOutputLeave,
    .state = handleState,
    .done = handleDone,
    .closed = handleClosed,
    .parent = handleParent,
};

void handleToplevel(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager, struct zwlr_foreign_toplevel_handle_v1 *toplevel) {
    struct xdpw_screencast_context *ctx = data;

	struct SToplevelEntry* entry = malloc(sizeof(struct SToplevelEntry));

	entry->handle = toplevel;

    wl_list_insert(&ctx->toplevel_resource_list, &entry->link);

    zwlr_foreign_toplevel_handle_v1_add_listener(toplevel, &toplevelHandleListener, ctx);
}

void handleFinished(void *data, struct zwlr_foreign_toplevel_manager_v1 *zwlr_foreign_toplevel_manager_v1) {
	; // noop
}

struct zwlr_foreign_toplevel_manager_v1_listener toplevelListener = {
    .toplevel = handleToplevel,
    .finished = handleFinished,
};

struct SToplevelEntry* toplevelEntryFromID(struct xdpw_screencast_context *ctx, uint32_t id) {
    struct SToplevelEntry *current;
    wl_list_for_each(current, &ctx->toplevel_resource_list, link) {
        if (((uint64_t)current->handle & 0xFFFFFFFF) == id) {
            return current;
        }
    }
	return NULL;
}

///

void wlr_frame_free(struct xdpw_screencast_instance *cast) {
	if (!cast->wlr_frame) {
		return;
	}
	zwlr_screencopy_frame_v1_destroy(cast->wlr_frame);
	cast->wlr_frame = NULL;
	logprint(TRACE, "wlroots: frame destroyed");
}

void xdpw_wlr_frame_finish(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "wlroots: finish screencopy");

	wlr_frame_free(cast);

	if (cast->quit || cast->err) {
		// TODO: revisit the exit condition (remove quit?)
		// and clean up sessions that still exist if err
		// is the cause of the instance_destroy call
		xdpw_screencast_instance_destroy(cast);
		return;
	}

	if (!cast->pwr_stream_state) {
		cast->frame_state = XDPW_FRAME_STATE_NONE;
		return;
	}

	if (cast->frame_state == XDPW_FRAME_STATE_RENEG) {
		pwr_update_stream_param(cast);
	}

	if (cast->frame_state == XDPW_FRAME_STATE_FAILED) {
		xdpw_pwr_enqueue_buffer(cast);
	}

	if (cast->frame_state == XDPW_FRAME_STATE_SUCCESS) {
		xdpw_pwr_enqueue_buffer(cast);
		uint64_t delay_ns = fps_limit_measure_end(&cast->fps_limit, cast->framerate);
		if (delay_ns > 0) {
			xdpw_add_timer(cast->ctx->state, delay_ns,
				(xdpw_event_loop_timer_func_t) xdpw_wlr_frame_start, cast);
			return;
		}
	}
	xdpw_wlr_frame_start(cast);
}

void xdpw_wlr_frame_start(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "wlroots: start screencopy");
	if (cast->quit || cast->err) {
		xdpw_screencast_instance_destroy(cast);
		return;
	}

	if (cast->initialized && !cast->pwr_stream_state) {
		cast->frame_state = XDPW_FRAME_STATE_NONE;
		return;
	}

	cast->frame_state = XDPW_FRAME_STATE_STARTED;
	xdpw_wlr_register_cb(cast);
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
	cast->wlr_frame = frame;

	cast->screencopy_frame_info[WL_SHM].width = width;
	cast->screencopy_frame_info[WL_SHM].height = height;
	cast->screencopy_frame_info[WL_SHM].stride = stride;
	cast->screencopy_frame_info[WL_SHM].size = stride * height;
	cast->screencopy_frame_info[WL_SHM].format = xdpw_format_drm_fourcc_from_wl_shm(format);

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

	cast->screencopy_frame_info[DMABUF].width = width;
	cast->screencopy_frame_info[DMABUF].height = height;
	cast->screencopy_frame_info[DMABUF].format = format;
}

static void wlr_frame_buffer_done(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "wlroots: buffer_done event handler");

	if (!cast->initialized) {
		xdpw_wlr_frame_finish(cast);
		return;
	}

	// Check if announced screencopy information is compatible with pipewire meta
	if ((cast->pwr_format.format != xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[cast->buffer_type].format) &&
			cast->pwr_format.format != xdpw_format_pw_strip_alpha(xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[cast->buffer_type].format))) ||
			cast->pwr_format.size.width != cast->screencopy_frame_info[cast->buffer_type].width ||
			cast->pwr_format.size.height != cast->screencopy_frame_info[cast->buffer_type].height) {
		logprint(DEBUG, "wlroots: pipewire and wlroots metadata are incompatible. Renegotiate stream");
		cast->frame_state = XDPW_FRAME_STATE_RENEG;
		xdpw_wlr_frame_finish(cast);
		return;
	}

	if (!cast->current_frame.xdpw_buffer) {
		xdpw_pwr_dequeue_buffer(cast);
	}

	if (!cast->current_frame.xdpw_buffer) {
		logprint(WARN, "wlroots: no current buffer");
		xdpw_wlr_frame_finish(cast);
		return;
	}

	assert(cast->current_frame.xdpw_buffer);

	// Check if dequeued buffer is compatible with announced buffer
	if (( cast->buffer_type == WL_SHM &&
				(cast->current_frame.xdpw_buffer->size[0] != cast->screencopy_frame_info[cast->buffer_type].size ||
				cast->current_frame.xdpw_buffer->stride[0] != cast->screencopy_frame_info[cast->buffer_type].stride)) ||
			cast->current_frame.xdpw_buffer->width != cast->screencopy_frame_info[cast->buffer_type].width ||
			cast->current_frame.xdpw_buffer->height != cast->screencopy_frame_info[cast->buffer_type].height) {
		logprint(DEBUG, "wlroots: pipewire buffer has wrong dimensions");
		cast->frame_state = XDPW_FRAME_STATE_FAILED;
		xdpw_wlr_frame_finish(cast);
		return;
	}

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

	cast->current_frame.damage.x = x;
	cast->current_frame.damage.y = y;
	cast->current_frame.damage.width = width;
	cast->current_frame.damage.height = height;
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
	logprint(TRACE, "wlroots: timestamp %"PRIu64":%"PRIu32, cast->current_frame.tv_sec, cast->current_frame.tv_nsec);

	cast->frame_state = XDPW_FRAME_STATE_SUCCESS;

	xdpw_wlr_frame_finish(cast);
}

static void wlr_frame_failed(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "wlroots: failed event handler");

	cast->frame_state = XDPW_FRAME_STATE_FAILED;

	xdpw_wlr_frame_finish(cast);
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

static void hyprland_frame_buffer_done(void *data,
		struct hyprland_toplevel_export_frame_v1 *frame);

static void hyprland_frame_buffer(void *data, struct hyprland_toplevel_export_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "hyprland: buffer event handler");
	cast->hyprland_frame = frame;

	cast->screencopy_frame_info[WL_SHM].width = width;
	cast->screencopy_frame_info[WL_SHM].height = height;
	cast->screencopy_frame_info[WL_SHM].stride = stride;
	cast->screencopy_frame_info[WL_SHM].size = stride * height;
	cast->screencopy_frame_info[WL_SHM].format = xdpw_format_drm_fourcc_from_wl_shm(format);

	// TODO: am I sure this should be here
	hyprland_frame_buffer_done(cast, frame);
}

static void hyprland_frame_linux_dmabuf(void *data,
		struct hyprland_toplevel_export_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "hyprland: linux_dmabuf event handler");

	cast->screencopy_frame_info[DMABUF].width = width;
	cast->screencopy_frame_info[DMABUF].height = height;
	cast->screencopy_frame_info[DMABUF].format = format;
}

static void hyprland_frame_buffer_done(void *data,
		struct hyprland_toplevel_export_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "hyprland: buffer_done event handler");

	if (!cast->initialized) {
		xdpw_wlr_frame_finish(cast);
		return;
	}

	// Check if announced screencopy information is compatible with pipewire meta
	if ((cast->pwr_format.format != xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[cast->buffer_type].format) &&
			cast->pwr_format.format != xdpw_format_pw_strip_alpha(xdpw_format_pw_from_drm_fourcc(cast->screencopy_frame_info[cast->buffer_type].format))) ||
			cast->pwr_format.size.width != cast->screencopy_frame_info[cast->buffer_type].width ||
			cast->pwr_format.size.height != cast->screencopy_frame_info[cast->buffer_type].height) {
		logprint(DEBUG, "hyprland: pipewire and wlroots metadata are incompatible. Renegotiate stream");
		cast->frame_state = XDPW_FRAME_STATE_RENEG;
		xdpw_wlr_frame_finish(cast);
		return;
	}

	if (!cast->current_frame.xdpw_buffer) {
		xdpw_pwr_dequeue_buffer(cast);
	}

	if (!cast->current_frame.xdpw_buffer) {
		logprint(WARN, "hyprland: no current buffer");
		xdpw_wlr_frame_finish(cast);
		return;
	}

	assert(cast->current_frame.xdpw_buffer);

	// Check if dequeued buffer is compatible with announced buffer
	if (( cast->buffer_type == WL_SHM &&
				(cast->current_frame.xdpw_buffer->size[0] != cast->screencopy_frame_info[cast->buffer_type].size ||
				cast->current_frame.xdpw_buffer->stride[0] != cast->screencopy_frame_info[cast->buffer_type].stride)) ||
			cast->current_frame.xdpw_buffer->width != cast->screencopy_frame_info[cast->buffer_type].width ||
			cast->current_frame.xdpw_buffer->height != cast->screencopy_frame_info[cast->buffer_type].height) {
		logprint(DEBUG, "hyprland: pipewire buffer has wrong dimensions");
		cast->frame_state = XDPW_FRAME_STATE_FAILED;
		xdpw_wlr_frame_finish(cast);
		return;
	}

    hyprland_toplevel_export_frame_v1_copy(frame, cast->current_frame.xdpw_buffer->buffer, 0);
    logprint(TRACE, "hyprland: frame copied");

	fps_limit_measure_start(&cast->fps_limit, cast->framerate);
}

static void hyprland_frame_flags(void *data, struct hyprland_toplevel_export_frame_v1 *frame,
		uint32_t flags) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "hyprland: flags event handler");
	cast->current_frame.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void hyprland_frame_damage(void *data, struct hyprland_toplevel_export_frame_v1 *frame,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "hyprland: damage event handler");

	cast->current_frame.damage.x = x;
	cast->current_frame.damage.y = y;
	cast->current_frame.damage.width = width;
	cast->current_frame.damage.height = height;
}

static void hyprland_frame_ready(void *data, struct hyprland_toplevel_export_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "hyprland: ready event handler");

	cast->current_frame.tv_sec = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo);
	cast->current_frame.tv_nsec = tv_nsec;
	logprint(TRACE, "hyprland: timestamp %"PRIu64":%"PRIu32, cast->current_frame.tv_sec, cast->current_frame.tv_nsec);

	cast->frame_state = XDPW_FRAME_STATE_SUCCESS;

	xdpw_wlr_frame_finish(cast);
}

static void hyprland_frame_failed(void *data,
		struct hyprland_toplevel_export_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;
	if (!frame) {
		return;
	}

	logprint(TRACE, "hyprland: failed event handler");

	cast->frame_state = XDPW_FRAME_STATE_FAILED;

	xdpw_wlr_frame_finish(cast);
}

static const struct hyprland_toplevel_export_frame_v1_listener hyprland_frame_listener = {
	.buffer = hyprland_frame_buffer,
	.buffer_done = hyprland_frame_buffer_done,
	.linux_dmabuf = hyprland_frame_linux_dmabuf,
	.flags = hyprland_frame_flags,
	.ready = hyprland_frame_ready,
	.failed = hyprland_frame_failed,
	.damage = hyprland_frame_damage
};

void xdpw_wlr_register_cb(struct xdpw_screencast_instance *cast) {

	if (cast->target.x != -1 && cast->target.y != -1 && cast->target.w != -1 && cast->target.h != -1 && cast->target.window_handle == -1) {
		// capture region
        cast->frame_callback = zwlr_screencopy_manager_v1_capture_output_region(
			cast->ctx->screencopy_manager, cast->with_cursor, cast->target.output->output, cast->target.x,
			cast->target.y, cast->target.w, cast->target.h);
    } else if (cast->target.window_handle == -1) {
        cast->frame_callback = zwlr_screencopy_manager_v1_capture_output(
            cast->ctx->screencopy_manager, cast->with_cursor, cast->target.output->output);
    } else {
		// share window
		struct SToplevelEntry* entry = toplevelEntryFromID(cast->ctx, cast->target.window_handle);

		if (!entry) {
            logprint(DEBUG, "hyprland: error in getting entry");
			return;
        }

        cast->frame_callback_hyprland = hyprland_toplevel_export_manager_v1_capture_toplevel_with_wlr_toplevel_handle(
			cast->ctx->hyprland_toplevel_manager, cast->with_cursor, entry->handle);

        hyprland_toplevel_export_frame_v1_add_listener(cast->frame_callback_hyprland,
			&hyprland_frame_listener, cast);

        logprint(TRACE, "hyprland: callbacks registered");
        return;
    }

	zwlr_screencopy_frame_v1_add_listener(cast->frame_callback,
		&wlr_frame_listener, cast);
	logprint(TRACE, "wlroots: callbacks registered");
}

static void wlr_output_handle_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t phys_width, int32_t phys_height,
		int32_t subpixel, const char *make, const char *model, int32_t transform) {
	struct xdpw_wlr_output *output = data;
	output->make = strdup(make);
	output->model = strdup(model);
}

static void wlr_output_handle_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		struct xdpw_wlr_output *output = data;
		output->framerate = (float)refresh/1000;
	}
}

static void wlr_output_handle_done(void *data, struct wl_output *wl_output) {
	/* Nothing to do */
}

static void wlr_output_handle_scale(void *data, struct wl_output *wl_output,
		int32_t factor) {
	/* Nothing to do */
}

static const struct wl_output_listener wlr_output_listener = {
	.geometry = wlr_output_handle_geometry,
	.mode = wlr_output_handle_mode,
	.done = wlr_output_handle_done,
	.scale = wlr_output_handle_scale,
};

static void wlr_xdg_output_name(void *data, struct zxdg_output_v1 *xdg_output,
		const char *name) {
	struct xdpw_wlr_output *output = data;

	output->name = strdup(name);
};

static void noop() {
	// This space intentionally left blank
}

static const struct zxdg_output_v1_listener wlr_xdg_output_listener = {
	.logical_position = noop,
	.logical_size = noop,
	.done = NULL, /* Deprecated */
	.description = noop,
	.name = wlr_xdg_output_name,
};

static void wlr_add_xdg_output_listener(struct xdpw_wlr_output *output,
		struct zxdg_output_v1 *xdg_output) {
	output->xdg_output = xdg_output;
	zxdg_output_v1_add_listener(output->xdg_output, &wlr_xdg_output_listener,
		output);
}

static void wlr_init_xdg_output(struct xdpw_screencast_context *ctx,
		struct xdpw_wlr_output *output) {
	struct zxdg_output_v1 *xdg_output =
		zxdg_output_manager_v1_get_xdg_output(ctx->xdg_output_manager,
			output->output);
	wlr_add_xdg_output_listener(output, xdg_output);
}

static void wlr_init_xdg_outputs(struct xdpw_screencast_context *ctx) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &ctx->output_list, link) {
		if (output->xdg_output) {
			continue;
		}
		wlr_init_xdg_output(ctx, output);
	}
}

// stolen from LLVM cuz it wouldnt include lol
static inline int vasprintf(char **strp, const char *fmt, va_list ap) {
    const size_t buff_size = 256;
    if ((*strp = (char *)malloc(buff_size)) == NULL) {
        return -1;
    }

    va_list ap_copy;
    // va_copy may not be provided by the C library in C++ 03 mode.
#if defined(_LIBCPP_CXX03_LANG) && __has_builtin(__builtin_va_copy)
    __builtin_va_copy(ap_copy, ap);
#else
    va_copy(ap_copy, ap);
#endif
    int str_size = vsnprintf(*strp, buff_size, fmt, ap_copy);
    va_end(ap_copy);

    if ((size_t)str_size >= buff_size) {
        if ((*strp = (char *)realloc(*strp, str_size + 1)) == NULL) {
            return -1;
        }
        str_size = vsnprintf(*strp, str_size + 1, fmt, ap);
    }
    return str_size;
}

char *getFormat(const char *fmt, ...) {
    char *outputStr = NULL;

    va_list args;
    va_start(args, fmt);
    vasprintf(&outputStr, fmt, args);
    va_end(args);

    return outputStr;
}

char* buildWindowList(struct xdpw_screencast_context *ctx) {

	char* rolling = calloc(1, 1);

	struct SToplevelEntry* current;
	wl_list_for_each(current, &ctx->toplevel_resource_list, link) {
		
		char* oldRolling = rolling;

		rolling = getFormat("%s%u[HC\011]%s[HT\011]%s[HE\011]", rolling, (uint32_t)(((uint64_t)current->handle) & 0xFFFFFFFF), current->clazz, current->name);

		free(oldRolling);
	}

	for (size_t i = 0; i < strlen(rolling); ++i) {
		if (rolling[i] == '\"')
			rolling[i] = ' ';
	}

	return rolling;
}

struct xdpw_share xdpw_wlr_chooser(struct xdpw_screencast_context *ctx) {
    char result[1024] = {0};
    FILE *fp;
	char buf[1024] = {0};

	const char *WAYLAND_DISPLAY = getenv("WAYLAND_DISPLAY");
	const char *XCURSOR_SIZE = getenv("XCURSOR_SIZE");
    const char *HYPRLAND_INSTANCE_SIGNATURE = getenv("HYPRLAND_INSTANCE_SIGNATURE");

	char* windowList = buildWindowList(ctx);

    char *cmd = getFormat("WAYLAND_DISPLAY=%s QT_QPA_PLATFORM=\"wayland\" XCURSOR_SIZE=%s HYPRLAND_INSTANCE_SIGNATURE=%s XDPH_WINDOW_SHARING_LIST=\"%s\" hyprland-share-picker", WAYLAND_DISPLAY, XCURSOR_SIZE ? XCURSOR_SIZE : "24", HYPRLAND_INSTANCE_SIGNATURE ? HYPRLAND_INSTANCE_SIGNATURE : "0", windowList);

	free(windowList);

    logprint(DEBUG, "Screencast: Picker: Running command \"%s\"", cmd);

    fp = popen(cmd, "r");
    if (fp == NULL) {
        printf("Failed to run command\n");
        exit(1);
    }

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        strcat(result, buf);
    }

    pclose(fp);

	free(cmd);

	// great, let's parse it.

    struct xdpw_share res = {NULL, -1, -1, -1, -1, -1};

	if (strncmp(result, "screen:", 7) == 0) {
		// find output
        logprint(DEBUG, "Screencast: Attempting to find screen for %s", result);

        char* display_name = malloc(strlen(result) - 7);
        strncpy(display_name, result + 7, strlen(result) - 8);
		display_name[strlen(result) - 8] = 0;

		struct xdpw_wlr_output* out;
		bool found = false;
        wl_list_for_each(out, &ctx->output_list, link) {
            if (strcmp(out->name, display_name) == 0) {
				found = true;
				break;
			}
        }

        free(display_name);

		if (!found)
			return res;

		res.output = out;
		return res;
    } else if (strncmp(result, "region:", 7) == 0) {
        // find output
        logprint(DEBUG, "Screencast: Attempting to find region for %s", result);

        int atPos = 7;
		for (int i = 7; i < (int)strlen(result); ++i) {
			if (result[i] == '@'){
				atPos = i;
				break;
			}
		}

        char *display_name = malloc(atPos - 6);
        strncpy(display_name, result + 7, atPos - 7);
        display_name[atPos - 7] = 0;

        struct xdpw_wlr_output *out;
        wl_list_for_each(out, &ctx->output_list, link) {
            if (strcmp(out->name, display_name) == 0) {
                break;
            }
        }

        // then get coords
		int coordno = 0;
		int coords[4] = {-1, -1, -1, -1};
		int coordbegin = 7 + strlen(display_name) + 1;
		for (int i = 7 + strlen(display_name) + 1; i < (int)strlen(result); ++i) {
			if (result[i] == ',' || result[i] == '@' || i + 1 == (int)strlen(result)) {
				char* entire = malloc(i - coordbegin + 1);
				strncpy(entire, result + coordbegin, i - coordbegin);
				entire[i - coordbegin] = 0;
				coords[coordno] = strtol(entire, NULL, 10);
				free(entire);

				coordno++;
				coordbegin = i + 1;
				i++;
			}
		}

		free(display_name);

        struct xdpw_share res2 = {out, coords[0], coords[1], coords[2], coords[3]};
		return res2;
    } else if (strncmp(result, "window:", 7) == 0) {
		if (ctx->hyprland_toplevel_manager == NULL) {
            logprint(DEBUG, "Screencast: Window sharing attempted but the toplevel protocol is not implemented by the compositor!");
            return res;
		}

        logprint(DEBUG, "Screencast: Attempting to find window for %s", result);

        char *display_name = malloc(strlen(result) - 7);
        strncpy(display_name, result + 7, strlen(result) - 8);
        display_name[strlen(result) - 8] = 0;

        res.window_handle = strtol(display_name, NULL, 10);

		free(display_name);
        return res;
    } else {
        logprint(DEBUG, "Screencast: Invalid result from hyprland-share-picker: %s", result);
        return res;
    }

    return res;
}

struct xdpw_wlr_output *xdpw_wlr_output_first(struct wl_list *output_list) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, output_list, link) {
		return output;
	}
	return NULL;
}

struct xdpw_wlr_output *xdpw_wlr_output_find_by_name(struct wl_list *output_list,
		const char *name) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, output_list, link) {
		if (strcmp(output->name, name) == 0) {
			return output;
		}
	}
	return NULL;
}

struct xdpw_wlr_output *xdpw_wlr_output_find(struct xdpw_screencast_context *ctx,
		struct wl_output *out, uint32_t id) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &ctx->output_list, link) {
		if ((output->output == out) || (output->id == id)) {
			return output;
		}
	}
	return NULL;
}

static void wlr_remove_output(struct xdpw_wlr_output *out) {
	free(out->name);
	free(out->make);
	free(out->model);
	zxdg_output_v1_destroy(out->xdg_output);
	wl_output_destroy(out->output);
	wl_list_remove(&out->link);
	free(out);
}

static void wlr_format_modifier_pair_add(struct xdpw_screencast_context *ctx,
		uint32_t format, uint64_t modifier) {
	struct xdpw_format_modifier_pair *fm_pair;
	wl_array_for_each(fm_pair, &ctx->format_modifier_pairs) {
		if (fm_pair->fourcc == format && fm_pair->modifier == modifier) {
			logprint(TRACE, "wlroots: skipping duplicated format %u (%lu)", fm_pair->fourcc, fm_pair->modifier);
			return;
		}
	}

	fm_pair = wl_array_add(&ctx->format_modifier_pairs, sizeof(struct xdpw_format_modifier_pair));
	fm_pair->fourcc = format;
	fm_pair->modifier = modifier;
	logprint(TRACE, "wlroots: format %u (%lu)", fm_pair->fourcc, fm_pair->modifier);
}

static void linux_dmabuf_handle_modifier(void *data,
		struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf_v1,
		uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo) {
	struct xdpw_screencast_context *ctx = data;

	logprint(TRACE, "wlroots: linux_dmabuf_handle_modifier called");

	uint64_t modifier = (((uint64_t)modifier_hi) << 32) | modifier_lo;
	wlr_format_modifier_pair_add(ctx, format, modifier);
}

static const struct zwp_linux_dmabuf_v1_listener linux_dmabuf_listener = {
	.format = noop,
	.modifier = linux_dmabuf_handle_modifier,
};

static void linux_dmabuf_feedback_handle_main_device(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1, struct wl_array *device_arr) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_handle_main_device called");

	assert(ctx->gbm == NULL);

	dev_t device;
	assert(device_arr->size == sizeof(device));
	memcpy(&device, device_arr->data, sizeof(device));

	drmDevice *drmDev;
	if (drmGetDeviceFromDevId(device, /* flags */ 0, &drmDev) != 0) {
		logprint(WARN, "wlroots: unable to open main device");
		ctx->state->config->screencast_conf.force_mod_linear = true;
		return;
	}
	ctx->gbm = xdpw_gbm_device_create(drmDev);
}

static void linux_dmabuf_feedback_format_table(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1, int fd, uint32_t size) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_format_table called");

	wl_array_release(&ctx->format_modifier_pairs);
	wl_array_init(&ctx->format_modifier_pairs);

	ctx->feedback_data.format_table_data = mmap(NULL , size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (ctx->feedback_data.format_table_data == MAP_FAILED) {
		ctx->feedback_data.format_table_data = NULL;
		ctx->feedback_data.format_table_size = 0;
		return;
	}
	ctx->feedback_data.format_table_size = size;
}

static void linux_dmabuf_feedback_handle_done(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_handle_done called");

	if (ctx->feedback_data.format_table_data) {
		munmap(ctx->feedback_data.format_table_data, ctx->feedback_data.format_table_size);
	}
	ctx->feedback_data.format_table_data = NULL;
	ctx->feedback_data.format_table_size = 0;
}

static void linux_dmabuf_feedback_tranche_target_devices(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1, struct wl_array *device_arr) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_tranche_target_devices called");

	dev_t device;
	assert(device_arr->size == sizeof(device));
	memcpy(&device, device_arr->data, sizeof(device));

	drmDevice *drmDev;
	if (drmGetDeviceFromDevId(device, /* flags */ 0, &drmDev) != 0) {
		return;
	}

	if (ctx->gbm) {
		drmDevice *drmDevRenderer = NULL;
		drmGetDevice2(gbm_device_get_fd(ctx->gbm), /* flags */ 0, &drmDevRenderer);
		ctx->feedback_data.device_used = drmDevicesEqual(drmDevRenderer, drmDev);
	} else {
		ctx->gbm = xdpw_gbm_device_create(drmDev);
		ctx->feedback_data.device_used = ctx->gbm != NULL;
	}
}

static void linux_dmabuf_feedback_tranche_flags(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1, uint32_t flags) {
	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_tranche_flags called");
}

static void linux_dmabuf_feedback_tranche_formats(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1, struct wl_array *indices) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_tranche_formats called");

	if (!ctx->feedback_data.device_used || !ctx->feedback_data.format_table_data) {
		return;
	}
	struct fm_entry {
		uint32_t format;
		uint32_t padding;
		uint64_t modifier;
	};
	// An entry in the table has to be 16 bytes long
	assert(sizeof(struct fm_entry) == 16);

	uint32_t n_modifiers = ctx->feedback_data.format_table_size/sizeof(struct fm_entry);
	struct fm_entry *fm_entry = ctx->feedback_data.format_table_data;
	uint16_t *idx;
	wl_array_for_each(idx, indices) {
		if (*idx >= n_modifiers) {
			continue;
		}
		wlr_format_modifier_pair_add(ctx, (fm_entry + *idx)->format, (fm_entry + *idx)->modifier);
	}
}

static void linux_dmabuf_feedback_tranche_done(void *data,
		struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: linux_dmabuf_feedback_tranche_done called");

	ctx->feedback_data.device_used = false;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener linux_dmabuf_listener_feedback = {
	.main_device = linux_dmabuf_feedback_handle_main_device,
	.format_table = linux_dmabuf_feedback_format_table,
	.done = linux_dmabuf_feedback_handle_done,
	.tranche_target_device = linux_dmabuf_feedback_tranche_target_devices,
	.tranche_flags = linux_dmabuf_feedback_tranche_flags,
	.tranche_formats = linux_dmabuf_feedback_tranche_formats,
	.tranche_done = linux_dmabuf_feedback_tranche_done,
};

static void wlr_registry_handle_add(void *data, struct wl_registry *reg,
		uint32_t id, const char *interface, uint32_t ver) {
	struct xdpw_screencast_context *ctx = data;

	logprint(DEBUG, "wlroots: interface to register %s  (Version: %u)",interface, ver);
	if (!strcmp(interface, wl_output_interface.name)) {
		struct xdpw_wlr_output *output = calloc(1, sizeof(*output));

		output->id = id;
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, WL_OUTPUT_VERSION);
		output->output = wl_registry_bind(reg, id, &wl_output_interface, WL_OUTPUT_VERSION);

		wl_output_add_listener(output->output, &wlr_output_listener, output);
		wl_list_insert(&ctx->output_list, &output->link);
		if (ctx->xdg_output_manager) {
			wlr_init_xdg_output(ctx, output);
		}
	}

	if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
		uint32_t version = ver;
		if (SC_MANAGER_VERSION < ver) {
			version = SC_MANAGER_VERSION;
		} else if (ver < SC_MANAGER_VERSION_MIN) {
			version = SC_MANAGER_VERSION_MIN;
		}
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, version);
		ctx->screencopy_manager = wl_registry_bind(
			reg, id, &zwlr_screencopy_manager_v1_interface, version);
	}

    if (!strcmp(interface, hyprland_toplevel_export_manager_v1_interface.name)) {
        uint32_t version = ver;

		logprint(DEBUG, "hyprland: |-- registered to interface %s (Version %u)", interface, version);

        ctx->hyprland_toplevel_manager = wl_registry_bind(reg, id, &hyprland_toplevel_export_manager_v1_interface, version);
    }

	if (!strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
		uint32_t version = ver;

        logprint(DEBUG, "hyprland: |-- registered to interface %s (Version %u)", interface, version);

        ctx->wlroots_toplevel_manager = wl_registry_bind(reg, id, &zwlr_foreign_toplevel_manager_v1_interface, version);
		wl_list_init(&ctx->toplevel_resource_list);

        zwlr_foreign_toplevel_manager_v1_add_listener(ctx->wlroots_toplevel_manager, &toplevelListener, ctx);
    }

    if (strcmp(interface, wl_shm_interface.name) == 0) {
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, WL_SHM_VERSION);
		ctx->shm = wl_registry_bind(reg, id, &wl_shm_interface, WL_SHM_VERSION);
	}

	if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, XDG_OUTPUT_MANAGER_VERSION);
		ctx->xdg_output_manager =
			wl_registry_bind(reg, id, &zxdg_output_manager_v1_interface, XDG_OUTPUT_MANAGER_VERSION);
	}
	if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
		uint32_t version = ver;
		if (LINUX_DMABUF_VERSION < ver) {
			version = LINUX_DMABUF_VERSION;
		} else if (LINUX_DMABUF_VERSION_MIN > ver) {
			logprint(INFO, "wlroots: interface %s (Version %u) is required for DMA-BUF screencast", interface, LINUX_DMABUF_VERSION_MIN);
			return;
		}
		logprint(DEBUG, "wlroots: |-- registered to interface %s (Version %u)", interface, version);
		ctx->linux_dmabuf = wl_registry_bind(reg, id, &zwp_linux_dmabuf_v1_interface, version);

		if (version >= 4) {
			ctx->linux_dmabuf_feedback = zwp_linux_dmabuf_v1_get_default_feedback(ctx->linux_dmabuf);
			zwp_linux_dmabuf_feedback_v1_add_listener(ctx->linux_dmabuf_feedback, &linux_dmabuf_listener_feedback, ctx);
		} else {
			zwp_linux_dmabuf_v1_add_listener(ctx->linux_dmabuf, &linux_dmabuf_listener, ctx);
		}
	}
}

static void wlr_registry_handle_remove(void *data, struct wl_registry *reg,
		uint32_t id) {
	struct xdpw_screencast_context *ctx = data;
	struct xdpw_wlr_output *output = xdpw_wlr_output_find(ctx, NULL, id);
	if (output) {
		logprint(DEBUG, "wlroots: output removed (%s)", output->name);
		struct xdpw_screencast_instance *cast, *tmp;
		wl_list_for_each_safe(cast, tmp, &ctx->screencast_instances, link) {
			if (cast->target.output == output) {
				// screencopy might be in process for this instance
				wlr_frame_free(cast);
				// instance might be waiting for wakeup by the frame limiter
				struct xdpw_timer *timer, *ttmp;
				wl_list_for_each_safe(timer, ttmp, &cast->ctx->state->timers, link) {
					if (timer->user_data == cast) {
						xdpw_destroy_timer(timer);
					}
				}
				cast->teardown = true;
				xdpw_screencast_instance_teardown(cast);
			}
		}
		wlr_remove_output(output);
	}
}

static const struct wl_registry_listener wlr_registry_listener = {
	.global = wlr_registry_handle_add,
	.global_remove = wlr_registry_handle_remove,
};

int xdpw_wlr_screencopy_init(struct xdpw_state *state) {
	struct xdpw_screencast_context *ctx = &state->screencast;

	// initialize a list of outputs
	wl_list_init(&ctx->output_list);

	// initialize a list of active screencast instances
	wl_list_init(&ctx->screencast_instances);

	// initialize a list of format modifier pairs
	wl_array_init(&ctx->format_modifier_pairs);

	// retrieve registry
	ctx->registry = wl_display_get_registry(state->wl_display);
	wl_registry_add_listener(ctx->registry, &wlr_registry_listener, ctx);

	wl_display_roundtrip(state->wl_display);

	logprint(DEBUG, "wayland: registry listeners run");

	// make sure our wlroots supports xdg_output_manager
	if (!ctx->xdg_output_manager) {
		logprint(ERROR, "Compositor doesn't support %s!",
			zxdg_output_manager_v1_interface.name);
		return -1;
	}

	wlr_init_xdg_outputs(ctx);

	wl_display_roundtrip(state->wl_display);

	logprint(DEBUG, "wayland: xdg output listeners run");

	// make sure our wlroots supports shm protocol
	if (!ctx->shm) {
		logprint(ERROR, "Compositor doesn't support %s!", "wl_shm");
		return -1;
	}

	// make sure our wlroots supports screencopy protocol
	if (!ctx->screencopy_manager) {
		logprint(ERROR, "Compositor doesn't support %s!",
			zwlr_screencopy_manager_v1_interface.name);
		return -1;
	}

	// make sure we have a gbm device
	if (ctx->linux_dmabuf && !ctx->gbm) {
		ctx->gbm = xdpw_gbm_device_create(NULL);
		if (!ctx->gbm) {
			logprint(ERROR, "System doesn't support gbm!");
		}
	}

	return 0;
}

void xdpw_wlr_screencopy_finish(struct xdpw_screencast_context *ctx) {
	wl_array_release(&ctx->format_modifier_pairs);

	struct xdpw_wlr_output *output, *tmp_o;
	wl_list_for_each_safe(output, tmp_o, &ctx->output_list, link) {
		wl_list_remove(&output->link);
		zxdg_output_v1_destroy(output->xdg_output);
		wl_output_destroy(output->output);
	}

	struct xdpw_screencast_instance *cast, *tmp_c;
	wl_list_for_each_safe(cast, tmp_c, &ctx->screencast_instances, link) {
		cast->quit = true;
	}

	if (ctx->screencopy_manager) {
		zwlr_screencopy_manager_v1_destroy(ctx->screencopy_manager);
	}
	if (ctx->shm) {
		wl_shm_destroy(ctx->shm);
	}
	if (ctx->xdg_output_manager) {
		zxdg_output_manager_v1_destroy(ctx->xdg_output_manager);
	}
	if (ctx->gbm) {
		int fd = gbm_device_get_fd(ctx->gbm);
		gbm_device_destroy(ctx->gbm);
		close(fd);
	}
	if (ctx->linux_dmabuf_feedback) {
		zwp_linux_dmabuf_feedback_v1_destroy(ctx->linux_dmabuf_feedback);
	}
	if (ctx->linux_dmabuf) {
		zwp_linux_dmabuf_v1_destroy(ctx->linux_dmabuf);
	}
	if (ctx->registry) {
		wl_registry_destroy(ctx->registry);
	}
}
