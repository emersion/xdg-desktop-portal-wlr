#define _POSIX_C_SOURCE 200809L

#include "wlr_screencast.h"
#include "xdpw.h"

void wlr_frame_free(struct xdpw_state *state) {
	zwlr_screencopy_frame_v1_destroy(state->screencast.wlr_frame);
	munmap(state->screencast.simple_frame.data, state->screencast.simple_frame.size);
	wl_buffer_destroy(state->screencast.simple_frame.buffer);
	logprint(TRACE, "wlroots: frame destroyed");

	if (!state->screencast.quit && !state->screencast.err) {
		wlr_register_cb(state);
	}
}

static struct wl_buffer *create_shm_buffer(struct screencast_context *ctx,
		enum wl_shm_format fmt, int width, int height, int stride,
		void **data_out) {
	int size = stride * height;

	const char shm_name[] = "/wlroots-screencopy";
	int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		logprint(ERROR, "wlroots: shm_open failed");
		return NULL;
	}
	shm_unlink(shm_name);

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

static void wlr_frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
	struct xdpw_state *state = data;
	struct screencast_context *ctx = &state->screencast;

	logprint(TRACE, "wlroots: buffer event handler");
	ctx->wlr_frame = frame;
	ctx->simple_frame.width = width;
	ctx->simple_frame.height = height;
	ctx->simple_frame.stride = stride;
	ctx->simple_frame.size = stride * height;
	ctx->simple_frame.format = format;
	ctx->simple_frame.buffer = create_shm_buffer(ctx, format, width, height,
		stride, &ctx->simple_frame.data);

	if (ctx->simple_frame.buffer == NULL) {
		logprint(ERROR, "wlroots: failed to create buffer");
		abort();
	}

	zwlr_screencopy_frame_v1_copy_with_damage(frame, ctx->simple_frame.buffer);
	logprint(TRACE, "wlroots: frame copied");
}

static void wlr_frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t flags) {
	struct xdpw_state *state = data;
	struct screencast_context *ctx = &state->screencast;

	logprint(TRACE, "wlroots: flags event handler");
	ctx->simple_frame.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void wlr_frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct xdpw_state *state = data;
	struct screencast_context *ctx = &state->screencast;

	logprint(TRACE, "wlroots: ready event handler");

	ctx->simple_frame.tv_sec = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo);
	ctx->simple_frame.tv_nsec = tv_nsec;

	if (!ctx->quit && !ctx->err && ctx->stream_state) {
		pw_loop_signal_event(state->pw_loop, ctx->event);
		return ;
	}

	wlr_frame_free(state);
}

static void wlr_frame_failed(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_state *state = data;
	struct screencast_context *ctx = &state->screencast;

	logprint(TRACE, "wlroots: failed event handler");
	ctx->err = true;

	wlr_frame_free(state);
}

static void wlr_frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
	struct xdpw_state *state = data;
	struct screencast_context *ctx = &state->screencast;

	logprint(TRACE, "wlroots: damage event handler");

	ctx->simple_frame.damage->x = x;
	ctx->simple_frame.damage->y = y;
	ctx->simple_frame.damage->width = width;
	ctx->simple_frame.damage->height = height;
}

static const struct zwlr_screencopy_frame_v1_listener wlr_frame_listener = {
	.buffer = wlr_frame_buffer,
	.flags = wlr_frame_flags,
	.ready = wlr_frame_ready,
	.failed = wlr_frame_failed,
	.damage = wlr_frame_damage,
};

void wlr_register_cb(struct xdpw_state *state) {
	struct screencast_context *ctx = &state->screencast;

	ctx->frame_callback = zwlr_screencopy_manager_v1_capture_output(
		ctx->screencopy_manager, ctx->with_cursor, ctx->target_output->output);

	zwlr_screencopy_frame_v1_add_listener(ctx->frame_callback,
		&wlr_frame_listener, state);
	logprint(TRACE, "wlroots: callbacks registered");
}

static void wlr_output_handle_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t phys_width, int32_t phys_height,
		int32_t subpixel, const char *make, const char *model, int32_t transform) {
	struct wayland_output *output = data;
	output->make = strdup(make);
	output->model = strdup(model);
}

static void wlr_output_handle_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		struct wayland_output *output = data;
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

static void wlr_xdg_output_name(void* data, struct zxdg_output_v1* xdg_output,
		const char* name) {
	struct wayland_output *output = data;

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

void wlr_add_xdg_output_listener(struct wayland_output *output,
		struct zxdg_output_v1* xdg_output) {
	output->xdg_output = xdg_output;
	zxdg_output_v1_add_listener(output->xdg_output, &wlr_xdg_output_listener,
		output);
}

static void wlr_init_xdg_outputs(struct screencast_context *ctx) {
	struct wayland_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &ctx->output_list, link) {
		struct zxdg_output_v1 *xdg_output =
			zxdg_output_manager_v1_get_xdg_output( ctx->xdg_output_manager,
				output->output);
		wlr_add_xdg_output_listener(output, xdg_output);
	}
}

struct wayland_output *wlr_output_first(struct wl_list *output_list) {
	struct wayland_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, output_list, link) {
		return output;
	}
	return NULL;
}

struct wayland_output *wlr_output_find_by_name(struct wl_list *output_list,
		const char* name) {
	struct wayland_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, output_list, link) {
		if (strcmp(output->name, name) == 0) {
			return output;
		}
	}
	return NULL;
}

struct wayland_output *wlr_output_find(struct screencast_context *ctx,
		struct wl_output *out, uint32_t id) {
	struct wayland_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &ctx->output_list, link) {
		if ((output->output == out) || (output->id == id)) {
			return output;
		}
	}
	return NULL;
}

static void wlr_remove_output(struct wayland_output *out) {
	wl_list_remove(&out->link);
}

static void wlr_registry_handle_add(void *data, struct wl_registry *reg,
		uint32_t id, const char *interface, uint32_t ver) {
	struct screencast_context *ctx = data;

	if (!strcmp(interface, wl_output_interface.name)) {
		struct wayland_output *output = malloc(sizeof(*output));

		output->id = id;
		output->output = wl_registry_bind(reg, id, &wl_output_interface, 1);

		wl_output_add_listener(output->output, &wlr_output_listener, output);
		wl_list_insert(&ctx->output_list, &output->link);
	}

	if (!strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
		ctx->screencopy_manager = wl_registry_bind(
			reg, id, &zwlr_screencopy_manager_v1_interface, SC_MANAGER_VERSION);
	}

	if (strcmp(interface, wl_shm_interface.name) == 0) {
		ctx->shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
	}

	if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		ctx->xdg_output_manager =
			wl_registry_bind(reg, id, &zxdg_output_manager_v1_interface, 3);
	}
}

static void wlr_registry_handle_remove(void *data, struct wl_registry *reg,
		uint32_t id) {
	wlr_remove_output(
		wlr_output_find((struct screencast_context *)data, NULL, id));
}

static const struct wl_registry_listener wlr_registry_listener = {
	.global = wlr_registry_handle_add,
	.global_remove = wlr_registry_handle_remove,
};

int wlr_screencopy_init(struct xdpw_state *state) {
	struct screencast_context *ctx = &state->screencast;

	// retrieve list of outputs
	wl_list_init(&ctx->output_list);

	// retrieve registry
	ctx->registry = wl_display_get_registry(state->wl_display);
	wl_registry_add_listener(ctx->registry, &wlr_registry_listener, ctx);

	wl_display_dispatch(state->wl_display);
	wl_display_roundtrip(state->wl_display);

	logprint(TRACE, "wayland: registry listeners run");

	wlr_init_xdg_outputs(ctx);

	wl_display_dispatch(state->wl_display);
	wl_display_roundtrip(state->wl_display);

	logprint(TRACE, "wayland: xdg output listeners run");

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

	return 0;
}

void wlr_screencopy_uninit(struct screencast_context *ctx) {
	struct wayland_output *output, *tmp_o;
	wl_list_for_each_safe(output, tmp_o, &ctx->output_list, link) {
		wl_list_remove(&output->link);
		zxdg_output_v1_destroy(output->xdg_output);
		wl_output_destroy(output->output);
	}

	if (ctx->screencopy_manager) {
		zwlr_screencopy_manager_v1_destroy(ctx->screencopy_manager);
	}
}
