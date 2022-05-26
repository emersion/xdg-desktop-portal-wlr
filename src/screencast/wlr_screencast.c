#include "wlr_screencast.h"

#include "linux-dmabuf-unstable-v1-client-protocol.h"
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
#include <assert.h>
#include <wayland-client-protocol.h>
#include <xf86drm.h>

#include "screencast.h"
#include "pipewire_screencast.h"
#include "xdpw.h"
#include "logger.h"
#include "fps_limit.h"

void wlr_frame_free(struct xdpw_screencast_instance *cast) {
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

	logprint(TRACE, "wlroots: linux_dmabuf event handler");

	cast->screencopy_frame_info[DMABUF].width = width;
	cast->screencopy_frame_info[DMABUF].height = height;
	cast->screencopy_frame_info[DMABUF].format = format;
}

static void wlr_frame_buffer_done(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;

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

	logprint(TRACE, "wlroots: flags event handler");
	cast->current_frame.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void wlr_frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: damage event handler");

	cast->current_frame.damage.x = x;
	cast->current_frame.damage.y = y;
	cast->current_frame.damage.width = width;
	cast->current_frame.damage.height = height;
}

static void wlr_frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "wlroots: ready event handler");

	cast->current_frame.tv_sec = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo);
	cast->current_frame.tv_nsec = tv_nsec;

	cast->frame_state = XDPW_FRAME_STATE_SUCCESS;

	xdpw_wlr_frame_finish(cast);
}

static void wlr_frame_failed(void *data,
		struct zwlr_screencopy_frame_v1 *frame) {
	struct xdpw_screencast_instance *cast = data;

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

void xdpw_wlr_register_cb(struct xdpw_screencast_instance *cast) {
	cast->frame_callback = zwlr_screencopy_manager_v1_capture_output(
		cast->ctx->screencopy_manager, cast->with_cursor, cast->target_output->output);

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

static pid_t spawn_chooser(char *cmd, int chooser_in[2], int chooser_out[2]) {
	logprint(TRACE,
			"exec chooser called: cmd %s, pipe chooser_in (%d,%d), pipe chooser_out (%d,%d)",
			cmd, chooser_in[0], chooser_in[1], chooser_out[0], chooser_out[1]);
	pid_t pid = fork();

	if (pid < 0) {
		perror("fork");
		return pid;
	} else if (pid == 0) {
		close(chooser_in[1]);
		close(chooser_out[0]);

		dup2(chooser_in[0], STDIN_FILENO);
		dup2(chooser_out[1], STDOUT_FILENO);
		close(chooser_in[0]);
		close(chooser_out[1]);

		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);

		perror("execl");
		_exit(127);
	}

	close(chooser_in[0]);
	close(chooser_out[1]);

	return pid;
}

static bool wait_chooser(pid_t pid) {
	int status;
	if (waitpid(pid ,&status, 0) != -1 && WIFEXITED(status)) {
		return WEXITSTATUS(status) != 127;
	}
	return false;
}

static bool wlr_output_chooser(struct xdpw_output_chooser *chooser,
		struct wl_list *output_list, struct xdpw_wlr_output **output) {
	logprint(DEBUG, "wlroots: output chooser called");
	struct xdpw_wlr_output *out;
	size_t name_size = 0;
	char *name = NULL;
	*output = NULL;

	int chooser_in[2]; //p -> c
	int chooser_out[2]; //c -> p

	if (pipe(chooser_in) == -1) {
		perror("pipe chooser_in");
		logprint(ERROR, "Failed to open pipe chooser_in");
		goto error_chooser_in;
	}
	if (pipe(chooser_out) == -1) {
		perror("pipe chooser_out");
		logprint(ERROR, "Failed to open pipe chooser_out");
		goto error_chooser_out;
	}

	pid_t pid = spawn_chooser(chooser->cmd, chooser_in, chooser_out);
	if (pid < 0) {
		logprint(ERROR, "Failed to fork chooser");
		goto error_fork;
	}

	switch (chooser->type) {
	case XDPW_CHOOSER_DMENU:;
		FILE *f = fdopen(chooser_in[1], "w");
		if (f == NULL) {
			perror("fdopen pipe chooser_in");
			logprint(ERROR, "Failed to create stream writing to pipe chooser_in");
			goto error_fork;
		}
		wl_list_for_each(out, output_list, link) {
			fprintf(f, "%s\n", out->name);
		}
		fclose(f);
		break;
	default:
		close(chooser_in[1]);
	}

	if (!wait_chooser(pid)) {
		close(chooser_out[0]);
		return false;
	}

	FILE *f = fdopen(chooser_out[0], "r");
	if (f == NULL) {
		perror("fdopen pipe chooser_out");
		logprint(ERROR, "Failed to create stream reading from pipe chooser_out");
		close(chooser_out[0]);
		goto end;
	}

	ssize_t nread = getline(&name, &name_size, f);
	fclose(f);
	if (nread < 0) {
		perror("getline failed");
		goto end;
	}

	//Strip newline
	char *p = strchr(name, '\n');
	if (p != NULL) {
		*p = '\0';
	}

	logprint(TRACE, "wlroots: output chooser %s selects output %s", chooser->cmd, name);
	wl_list_for_each(out, output_list, link) {
		if (strcmp(out->name, name) == 0) {
			*output = out;
			break;
		}
	}
	free(name);

end:
	return true;

error_fork:
	close(chooser_out[0]);
	close(chooser_out[1]);
error_chooser_out:
	close(chooser_in[0]);
	close(chooser_in[1]);
error_chooser_in:
	*output = NULL;
	return false;
}

static struct xdpw_wlr_output *wlr_output_chooser_default(struct wl_list *output_list) {
	logprint(DEBUG, "wlroots: output chooser called");
	struct xdpw_output_chooser default_chooser[] = {
		{XDPW_CHOOSER_SIMPLE, "slurp -f %o -or"},
		{XDPW_CHOOSER_DMENU, "wofi -d -n --prompt='Select the monitor to share:'"},
		{XDPW_CHOOSER_DMENU, "bemenu --prompt='Select the monitor to share:'"},
	};

	size_t N = sizeof(default_chooser)/sizeof(default_chooser[0]);
	struct xdpw_wlr_output *output = NULL;
	bool ret;
	for (size_t i = 0; i<N; i++) {
		ret = wlr_output_chooser(&default_chooser[i], output_list, &output);
		if (!ret) {
			logprint(DEBUG, "wlroots: output chooser %s not found. Trying next one.",
					default_chooser[i].cmd);
			continue;
		}
		if (output != NULL) {
			logprint(DEBUG, "wlroots: output chooser selects %s", output->name);
		} else {
			logprint(DEBUG, "wlroots: output chooser canceled");
		}
		return output;
	}
	return xdpw_wlr_output_first(output_list);
}

struct xdpw_wlr_output *xdpw_wlr_output_chooser(struct xdpw_screencast_context *ctx) {
	switch (ctx->state->config->screencast_conf.chooser_type) {
	case XDPW_CHOOSER_DEFAULT:
		return wlr_output_chooser_default(&ctx->output_list);
	case XDPW_CHOOSER_NONE:
		if (ctx->state->config->screencast_conf.output_name) {
			return xdpw_wlr_output_find_by_name(&ctx->output_list, ctx->state->config->screencast_conf.output_name);
		} else {
			return xdpw_wlr_output_first(&ctx->output_list);
		}
	case XDPW_CHOOSER_DMENU:
	case XDPW_CHOOSER_SIMPLE:;
		struct xdpw_wlr_output *output = NULL;
		if (!ctx->state->config->screencast_conf.chooser_cmd) {
			logprint(ERROR, "wlroots: no output chooser given");
			goto end;
		}
		struct xdpw_output_chooser chooser = {
			ctx->state->config->screencast_conf.chooser_type,
			ctx->state->config->screencast_conf.chooser_cmd
		};
		logprint(DEBUG, "wlroots: output chooser %s (%d)", chooser.cmd, chooser.type);
		bool ret = wlr_output_chooser(&chooser, &ctx->output_list, &output);
		if (!ret) {
			logprint(ERROR, "wlroots: output chooser %s failed", chooser.cmd);
			goto end;
		}
		if (output) {
			logprint(DEBUG, "wlroots: output chooser selects %s", output->name);
		} else {
			logprint(DEBUG, "wlroots: output chooser canceled");
		}
		return output;
	}
end:
	return NULL;
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
	wl_list_for_each(fm_pair, &ctx->format_modifier_pairs, link) {
		if (fm_pair->fourcc == format && fm_pair->modifier == modifier) {
			logprint(TRACE, "wlroots: skipping duplicated format %u (%lu)", fm_pair->fourcc, fm_pair->modifier);
			return;
		}
	}

	fm_pair = calloc(1, sizeof(struct xdpw_format_modifier_pair));
	fm_pair->fourcc = format;
	fm_pair->modifier = modifier;

	logprint(TRACE, "wlroots: format %u (%lu)", fm_pair->fourcc, fm_pair->modifier);

	wl_list_insert(&ctx->format_modifier_pairs, &fm_pair->link);
}

static void wlr_format_modifier_pair_emtpy_list(struct xdpw_screencast_context *ctx) {
	struct xdpw_format_modifier_pair *fm_pair, *tmp;
	wl_list_for_each_safe(fm_pair, tmp, &ctx->format_modifier_pairs, link) {
		wl_list_remove(&fm_pair->link);
		free(fm_pair);
	}
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

	wlr_format_modifier_pair_emtpy_list(ctx);

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
	wl_list_init(&ctx->format_modifier_pairs);

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
	wlr_format_modifier_pair_emtpy_list(ctx);

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
