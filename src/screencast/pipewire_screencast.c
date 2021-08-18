#include "pipewire_screencast.h"

#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include <spa/param/props.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <sys/mman.h>
#include <unistd.h>
#include <assert.h>

#include "wlr_screencast.h"
#include "xdpw.h"
#include "logger.h"

static struct spa_pod *build_format(struct spa_pod_builder *b, enum spa_video_format format,
		uint32_t width, uint32_t height, uint32_t framerate) {
	struct spa_pod_frame f[1];

	enum spa_video_format format_without_alpha = xdpw_format_pw_strip_alpha(format);

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	/* format */
	if (format_without_alpha == SPA_VIDEO_FORMAT_UNKNOWN) {
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
	} else {
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format,
				SPA_POD_CHOICE_ENUM_Id(3, format, format, format_without_alpha), 0);
	}
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
		SPA_POD_Rectangle(&SPA_RECTANGLE(width, height)),
		0);
	// variable framerate
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
		SPA_POD_Fraction(&SPA_FRACTION(0, 1)), 0);
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate,
		SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(framerate, 1),
			&SPA_FRACTION(1, 1),
			&SPA_FRACTION(framerate, 1)),
		0);
	return spa_pod_builder_pop(b, &f[0]);
}

static void pwr_handle_stream_state_changed(void *data,
		enum pw_stream_state old, enum pw_stream_state state, const char *error) {
	struct xdpw_screencast_instance *cast = data;
	cast->node_id = pw_stream_get_node_id(cast->stream);

	logprint(INFO, "pipewire: stream state changed to \"%s\"",
		pw_stream_state_as_string(state));
	logprint(INFO, "pipewire: node id is %d", (int)cast->node_id);

	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		cast->pwr_stream_state = true;
		if (!cast->wlr_frame) {
			xdpw_wlr_frame_start(cast);
		}
		break;
	default:
		cast->pwr_stream_state = false;
		break;
	}
}

static void pwr_handle_stream_param_changed(void *data, uint32_t id,
		const struct spa_pod *param) {
	logprint(TRACE, "pipewire: stream parameters changed");
	struct xdpw_screencast_instance *cast = data;
	struct pw_stream *stream = cast->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[2];

	if (!param || id != SPA_PARAM_Format) {
		return;
	}

	spa_format_video_raw_parse(param, &cast->pwr_format);
	cast->framerate = (uint32_t)(cast->pwr_format.max_framerate.num / cast->pwr_format.max_framerate.denom);

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(XDPW_PWR_BUFFERS, 1, 32),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int(cast->screencopy_frame.size),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(cast->screencopy_frame.stride),
		SPA_PARAM_BUFFERS_align,   SPA_POD_Int(XDPW_PWR_ALIGN),
		SPA_PARAM_BUFFERS_dataType,SPA_POD_CHOICE_FLAGS_Int(1<<SPA_DATA_MemFd));

	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	pw_stream_update_params(stream, params, 2);
}

static void pwr_handle_stream_add_buffer(void *data, struct pw_buffer *buffer) {
	struct xdpw_screencast_instance *cast = data;
	struct spa_data *d;

	logprint(TRACE, "pipewire: add buffer event handle");

	d = buffer->buffer->datas;

	// Select buffer type from negotiation result
	if ((d[0].type & (1u << SPA_DATA_MemFd)) > 0) {
		d[0].type = SPA_DATA_MemFd;
	} else {
		logprint(ERROR, "pipewire: unsupported buffer type");
		cast->err = 1;
		return;
	}

	logprint(TRACE, "pipewire: selected buffertype %u", d[0].type);
	// Prepare buffer for choosen type
	if (d[0].type == SPA_DATA_MemFd) {
		d[0].maxsize = cast->screencopy_frame.size;
		d[0].mapoffset = 0;
		d[0].chunk->size = cast->screencopy_frame.size;
		d[0].chunk->stride = cast->screencopy_frame.stride;
		d[0].chunk->offset = 0;
		d[0].flags = 0;
		d[0].fd = anonymous_shm_open();
		d[0].data = NULL;

		if (d[0].fd == -1) {
			logprint(ERROR, "pipewire: unable to create anonymous filedescriptor");
			cast->err = 1;
			return;
		}

		if (ftruncate(d[0].fd, d[0].maxsize) < 0) {
			logprint(ERROR, "pipewire: unable to truncate filedescriptor");
			close(d[0].fd);
			d[0].fd = -1;
			cast->err = 1;
			return;
		}

		// create wl_buffer
		buffer->user_data = import_wl_shm_buffer(cast, d[0].fd, cast->screencopy_frame.format,
			cast->screencopy_frame.width, cast->screencopy_frame.height, cast->screencopy_frame.stride);
	}
}

static void pwr_handle_stream_remove_buffer(void *data, struct pw_buffer *buffer) {
	struct xdpw_screencast_instance *cast = data;

	logprint(TRACE, "pipewire: remove buffer event handle");

	struct spa_data *d = buffer->buffer->datas;
	if (cast->current_frame.current_pw_buffer == buffer) {
		logprint(TRACE, "pipewire: remove buffer currently in use");
		cast->current_frame.current_pw_buffer = NULL;
		cast->current_frame.buffer = NULL;
	}
	switch (d[0].type) {
	case SPA_DATA_MemFd:
		wl_buffer_destroy(buffer->user_data);
		close(d[0].fd);
		break;
	default:
		break;
	}
}

static const struct pw_stream_events pwr_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = pwr_handle_stream_state_changed,
	.param_changed = pwr_handle_stream_param_changed,
	.add_buffer = pwr_handle_stream_add_buffer,
	.remove_buffer = pwr_handle_stream_remove_buffer,
};

void xdpw_pwr_dequeue_buffer(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "pipewire: dequeueing buffer");

	assert(cast->current_frame.current_pw_buffer == NULL);
	if ((cast->current_frame.current_pw_buffer = pw_stream_dequeue_buffer(cast->stream)) == NULL) {
		logprint(WARN, "pipewire: out of buffers");
		cast->current_frame.buffer = NULL;
		return;
	}

	struct spa_buffer *spa_buf = cast->current_frame.current_pw_buffer->buffer;
	struct spa_data *d = spa_buf->datas;
	cast->current_frame.size = d[0].chunk->size;
	cast->current_frame.stride = d[0].chunk->stride;
	cast->current_frame.buffer = cast->current_frame.current_pw_buffer->user_data;
}

void xdpw_pwr_enqueue_buffer(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "pipewire: exporting buffer");

	struct pw_buffer *pw_buf = cast->current_frame.current_pw_buffer;
	bool buffer_corrupt = cast->frame_state != XDPW_FRAME_STATE_SUCCESS;

	assert(pw_buf);

	struct spa_buffer *spa_buf = pw_buf->buffer;
	struct spa_data *d = spa_buf->datas;

	if (cast->current_frame.y_invert) {
		//TODO: Flip buffer or set stride negative
		buffer_corrupt = true;
		cast->err = 1;
	}

	struct spa_meta_header *h;
	if ((h = spa_buffer_find_meta_data(spa_buf, SPA_META_Header, sizeof(*h)))) {
		h->pts = -1;
		h->flags = buffer_corrupt ? SPA_META_HEADER_FLAG_CORRUPTED : 0;
		h->seq = cast->seq++;
		h->dts_offset = 0;
	}

	if (buffer_corrupt) {
		d[0].chunk->flags = SPA_CHUNK_FLAG_CORRUPTED;
	} else {
		d[0].chunk->flags = SPA_CHUNK_FLAG_NONE;
	}

	logprint(TRACE, "********************");
	logprint(TRACE, "pipewire: fd %u", d[0].fd);
	logprint(TRACE, "pipewire: size %d", d[0].maxsize);
	logprint(TRACE, "pipewire: stride %d", d[0].chunk->stride);
	logprint(TRACE, "pipewire: width %d", cast->screencopy_frame.width);
	logprint(TRACE, "pipewire: height %d", cast->screencopy_frame.height);
	logprint(TRACE, "pipewire: y_invert %d", cast->current_frame.y_invert);
	logprint(TRACE, "********************");

	pw_stream_queue_buffer(cast->stream, pw_buf);

	cast->current_frame.current_pw_buffer = NULL;
	cast->current_frame.buffer = NULL;
}

void pwr_update_stream_param(struct xdpw_screencast_instance *cast) {
	logprint(TRACE, "pipewire: stream update parameters");
	struct pw_stream *stream = cast->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[1];

	enum spa_video_format format = xdpw_format_pw_from_wl_shm(cast->screencopy_frame.format);

	params[0] = build_format(&b, format,
			cast->screencopy_frame.width, cast->screencopy_frame.height, cast->framerate);

	pw_stream_update_params(stream, params, 1);
}

void xdpw_pwr_stream_create(struct xdpw_screencast_instance *cast) {
	struct xdpw_screencast_context *ctx = cast->ctx;
	struct xdpw_state *state = ctx->state;

	pw_loop_enter(state->pw_loop);

	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	char name[] = "xdpw-stream-XXXXXX";
	randname(name + strlen(name) - 6);
	cast->stream = pw_stream_new(ctx->core, name,
		pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			NULL));

	if (!cast->stream) {
		logprint(ERROR, "pipewire: failed to create stream");
		abort();
	}
	cast->pwr_stream_state = false;

	enum spa_video_format format = xdpw_format_pw_from_wl_shm(cast->screencopy_frame.format);

	const struct spa_pod *param = build_format(&b, format,
			cast->screencopy_frame.width, cast->screencopy_frame.height, cast->framerate);

	pw_stream_add_listener(cast->stream, &cast->stream_listener,
		&pwr_stream_events, cast);

	pw_stream_connect(cast->stream,
		PW_DIRECTION_OUTPUT,
		PW_ID_ANY,
		(PW_STREAM_FLAG_DRIVER |
			PW_STREAM_FLAG_ALLOC_BUFFERS),
		&param, 1);
}

void xdpw_pwr_stream_destroy(struct xdpw_screencast_instance *cast) {
	if (!cast->stream) {
		return;
	}

	logprint(DEBUG, "pipewire: destroying stream");
	pw_stream_flush(cast->stream, false);
	pw_stream_disconnect(cast->stream);
	pw_stream_destroy(cast->stream);
	cast->stream = NULL;
}

int xdpw_pwr_context_create(struct xdpw_state *state) {
	struct xdpw_screencast_context *ctx = &state->screencast;

	logprint(DEBUG, "pipewire: establishing connection to core");

	if (!ctx->pwr_context) {
		ctx->pwr_context = pw_context_new(state->pw_loop, NULL, 0);
		if (!ctx->pwr_context) {
			logprint(ERROR, "pipewire: failed to create context");
			return -1;
		}
	}

	if (!ctx->core) {
		ctx->core = pw_context_connect(ctx->pwr_context, NULL, 0);
		if (!ctx->core) {
			logprint(ERROR, "pipewire: couldn't connect to context");
			return -1;
		}
	}
	return 0;
}

void xdpw_pwr_context_destroy(struct xdpw_state *state) {
	struct xdpw_screencast_context *ctx = &state->screencast;

	logprint(DEBUG, "pipewire: disconnecting fom core");

	if (ctx->core) {
		pw_core_disconnect(ctx->core);
		ctx->core = NULL;
	}

	if (ctx->pwr_context) {
		pw_context_destroy(ctx->pwr_context);
		ctx->pwr_context = NULL;
	}
}
