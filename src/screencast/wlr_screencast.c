#include "wlr_screencast.h"

void wlr_frame_free(struct screencast_context *ctx) {

	zwlr_screencopy_frame_v1_destroy(ctx->wlr_frame);
	munmap(ctx->simple_frame.data, ctx->simple_frame.size);
	wl_buffer_destroy(ctx->simple_frame.buffer);
	logger("wlr frame destroyed\n");
	pthread_mutex_unlock(&ctx->lock);

}

static struct wl_buffer *create_shm_buffer(struct screencast_context *ctx,
																					 enum wl_shm_format fmt, int width,
																					 int height, int stride,
																					 void **data_out) {
	int size = stride * height;

	const char shm_name[] = "/wlroots-screencopy";
	int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		fprintf(stderr, "shm_open failed\n");
		return NULL;
	}
	shm_unlink(shm_name);

	int ret;
	while ((ret = ftruncate(fd, size)) == EINTR) {
		// No-op
	}
	if (ret < 0) {
		close(fd);
		fprintf(stderr, "ftruncate failed\n");
		return NULL;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
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
														 uint32_t format, uint32_t width, uint32_t height,
														 uint32_t stride) {
	struct screencast_context *ctx = data;

	pthread_mutex_lock(&ctx->lock);

	logger("wlr buffer event handler\n");
	ctx->wlr_frame = frame;
	ctx->simple_frame.width = width;
	ctx->simple_frame.height = height;
	ctx->simple_frame.stride = stride;
	ctx->simple_frame.size = stride * height;
	ctx->simple_frame.format = format;
	ctx->simple_frame.buffer = create_shm_buffer(ctx, format, width, height,
																							 stride, &ctx->simple_frame.data);
	if (ctx->simple_frame.buffer == NULL) {
		fprintf(stderr, "failed to create buffer\n");
		exit(EXIT_FAILURE);
	}

	// zwlr_screencopy_frame_v1_copy(frame, ctx->simple_frame.buffer);
	zwlr_screencopy_frame_v1_copy_with_damage(frame, ctx->simple_frame.buffer);
	pthread_mutex_unlock(&ctx->lock);
}

static void wlr_frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
														uint32_t flags) {
	struct screencast_context *ctx = data;

	pthread_mutex_lock(&ctx->lock);

	logger("wlr flags event handler\n");
	ctx->simple_frame.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;

	pthread_mutex_unlock(&ctx->lock);
}

static void wlr_frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
														uint32_t tv_sec_hi, uint32_t tv_sec_lo,
														uint32_t tv_nsec) {
	struct screencast_context *ctx = data;

	pthread_mutex_lock(&ctx->lock);

	logger("wlr ready event handler\n");

	ctx->simple_frame.tv_sec = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo);
	ctx->simple_frame.tv_nsec = tv_nsec;

	if (!ctx->quit && !ctx->err) {
		pw_loop_signal_event(pw_main_loop_get_loop(ctx->loop), ctx->event);
		// sleep(1);
		wlr_register_cb(ctx);
	}
}

static void wlr_frame_failed(void *data,
														 struct zwlr_screencopy_frame_v1 *frame) {
	struct screencast_context *ctx = data;

	logger("wlr failed event handler\n");

	wlr_frame_free(ctx);
	ctx->err = true;
}

static void wlr_frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
														 uint32_t x, uint32_t y, uint32_t width,
														 uint32_t height) {
	struct screencast_context *ctx = data;

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

void wlr_register_cb(struct screencast_context *ctx) {
	ctx->frame_callback = zwlr_screencopy_manager_v1_capture_output(
			ctx->screencopy_manager, ctx->with_cursor, ctx->target_output->output);

	zwlr_screencopy_frame_v1_add_listener(ctx->frame_callback,
																				&wlr_frame_listener, ctx);
	logger("wlr callbacks registered\n");
}

static void wlr_output_handle_geometry(void *data, struct wl_output *wl_output,
																			 int32_t x, int32_t y, int32_t phys_width,
																			 int32_t phys_height, int32_t subpixel,
																			 const char *make, const char *model,
																			 int32_t transform) {
	struct wayland_output *output = data;
	output->make = strdup(make);
	output->model = strdup(model);
}

static void wlr_output_handle_mode(void *data, struct wl_output *wl_output,
																	 uint32_t flags, int32_t width,
																	 int32_t height, int32_t refresh) {
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		struct wayland_output *output = data;
		output->framerate = (float)refresh * 1000;
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

struct wayland_output *wlr_find_output(struct screencast_context *ctx,
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
																		uint32_t id, const char *interface,
																		uint32_t ver) {
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
}

static void wlr_registry_handle_remove(void *data, struct wl_registry *reg,
																			 uint32_t id) {
	wlr_remove_output(
			wlr_find_output((struct screencast_context *)data, NULL, id));
}

static const struct wl_registry_listener wlr_registry_listener = {
		.global = wlr_registry_handle_add,
		.global_remove = wlr_registry_handle_remove,
};

int wlr_screencopy_init(struct screencast_context *ctx) {
	// connect to wayland display WAYLAND_DISPLAY or 'wayland-0' if not set
	ctx->display = wl_display_connect(NULL);
	if (!ctx->display) {
		printf("Failed to connect to display!\n");
		return -1;
	}

	// retrieve list of outputs
	wl_list_init(&ctx->output_list);

	// retrieve registry
	ctx->registry = wl_display_get_registry(ctx->display);
	wl_registry_add_listener(ctx->registry, &wlr_registry_listener, ctx);

	wl_display_roundtrip(ctx->display);
	wl_display_dispatch(ctx->display);

	// make sure our wlroots supports screencopy protocol
	if (!ctx->shm) {
		printf("Compositor doesn't support %s!\n", "wl_shm");
		return -1;
	}

	// make sure our wlroots supports screencopy protocol
	if (!ctx->screencopy_manager) {
		printf("Compositor doesn't support %s!\n",
					 zwlr_screencopy_manager_v1_interface.name);
		return -1;
	}

	return 0;
}

void wlr_screencopy_uninit(struct screencast_context *ctx) {
	struct wayland_output *output, *tmp_o;
	wl_list_for_each_safe(output, tmp_o, &ctx->output_list, link) {
		wl_list_remove(&output->link);
	}

	if (ctx->screencopy_manager) {
		zwlr_screencopy_manager_v1_destroy(ctx->screencopy_manager);
	}
}
