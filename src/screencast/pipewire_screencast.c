#include "pipewire_screencast.h"

static void writeFrameData(void *pwFramePointer, void *wlrFramePointer,
		uint32_t height, uint32_t stride, bool inverted) {
	if (!inverted) {
		memcpy(pwFramePointer, wlrFramePointer, height * stride);
		return;
	}

	for (size_t i = 0; i < (size_t)height; ++i) {
		void *flippedWlrRowPointer = wlrFramePointer + ((height - i - 1) * stride);
		void *pwRowPointer = pwFramePointer + (i * stride);
		memcpy(pwRowPointer, flippedWlrRowPointer, stride);
	}

	return;
}

static void pwr_on_event(void *data, uint64_t expirations) {
	struct xdpw_state *state = data;
	struct screencast_context *ctx = &state->screencast;
	struct pw_buffer *pw_buf;
	struct spa_buffer *spa_buf;
	struct spa_meta_header *h;
	struct spa_data *d;

	logprint(TRACE, "********************");
	logprint(TRACE, "pipewire: event fired");

	if ((pw_buf = pw_stream_dequeue_buffer(ctx->stream)) == NULL) {
		logprint(WARN, "pipewire: out of buffers");
		return;
	}

	spa_buf = pw_buf->buffer;
	d = spa_buf->datas;
	if ((d[0].data) == NULL) {
		logprint(TRACE, "pipewire: data pointer undefined");
		return;
	}
	if ((h = spa_buffer_find_meta_data(spa_buf, SPA_META_Header, sizeof(*h)))) {
		h->pts = -1;
		h->flags = 0;
		h->seq = ctx->seq++;
		h->dts_offset = 0;
	}

	d[0].type = SPA_DATA_MemPtr;
	d[0].maxsize = ctx->simple_frame.size;
	d[0].mapoffset = 0;
	d[0].chunk->size = ctx->simple_frame.size;
	d[0].chunk->stride = ctx->simple_frame.stride;
	d[0].chunk->offset = 0;
	d[0].flags = 0;
	d[0].fd = -1;

	writeFrameData(d[0].data, ctx->simple_frame.data, ctx->simple_frame.height,
		ctx->simple_frame.stride, ctx->simple_frame.y_invert);

	logprint(TRACE, "pipewire: pointer %p", d[0].data);
	logprint(TRACE, "pipewire: size %d", d[0].maxsize);
	logprint(TRACE, "pipewire: stride %d", d[0].chunk->stride);
	logprint(TRACE, "pipewire: width %d", ctx->simple_frame.width);
	logprint(TRACE, "pipewire: height %d", ctx->simple_frame.height);
	logprint(TRACE, "pipewire: y_invert %d", ctx->simple_frame.y_invert);
	logprint(TRACE, "********************");

	pw_stream_queue_buffer(ctx->stream, pw_buf);

	wlr_frame_free(state);
}

static void pwr_handle_stream_state_changed(void *data,
		enum pw_stream_state old, enum pw_stream_state state, const char *error) {
	struct xdpw_state *xdpw_state = data;
	struct screencast_context *ctx = &xdpw_state->screencast;
	ctx->node_id = pw_stream_get_node_id(ctx->stream);

	logprint(INFO, "pipewire: stream state changed to \"%s\"",
		pw_stream_state_as_string(state));
	logprint(INFO, "pipewire: node id is %d", ctx->node_id);

	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		ctx->stream_state = true;
		break;
	default:
		ctx->stream_state = false;
		break;
	}
}

static void pwr_handle_stream_param_changed(void *data, uint32_t id,
		const struct spa_pod *param) {
	struct xdpw_state *xdpw_state = data;
	struct screencast_context *ctx = &xdpw_state->screencast;
	struct pw_stream *stream = ctx->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[2];

	if (!param || id != SPA_PARAM_Format) {
		return;
	}

	spa_format_video_raw_parse(param, &ctx->pwr_format);

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(BUFFERS, 1, 32),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int(ctx->simple_frame.size),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(ctx->simple_frame.stride),
		SPA_PARAM_BUFFERS_align,   SPA_POD_Int(ALIGN));

	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	pw_stream_update_params(stream, params, 2);
}

static const struct pw_stream_events pwr_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = pwr_handle_stream_state_changed,
	.param_changed = pwr_handle_stream_param_changed,
};

void *pwr_start(struct xdpw_state *state) {
	struct screencast_context *ctx = &state->screencast;
	pw_loop_enter(state->pw_loop);

	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	ctx->pwr_context = pw_context_new(state->pw_loop, NULL, 0);
	if (!ctx->pwr_context) {
		logprint(ERROR, "pipewire: failed to create context");
		abort();
	}

	ctx->core = pw_context_connect(ctx->pwr_context, NULL, 0);
	if (!ctx->core) {
		logprint(ERROR, "pipewire: couldn't connect to context");
		abort();
	}

	ctx->stream = pw_stream_new(ctx->core, "xdg-desktop-portal-wlr",
		pw_properties_new(
			PW_KEY_MEDIA_CLASS, "Video/Source",
			NULL));

	if (!ctx->stream) {
		logprint(ERROR, "pipewire: failed to create stream");
		abort();
	}
	ctx->stream_state = false;

	/* make an event to signal frame ready */
	ctx->event =
		pw_loop_add_event(state->pw_loop, pwr_on_event, state);

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format,    SPA_POD_Id(pipewire_from_wl_shm(ctx)),
		SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(
			&SPA_RECTANGLE(ctx->simple_frame.width, ctx->simple_frame.height),
			&SPA_RECTANGLE(1, 1),
			&SPA_RECTANGLE(4096, 4096)),
		// variable framerate
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
		SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(ctx->framerate, 1),
			&SPA_FRACTION(1, 1),
			&SPA_FRACTION(ctx->framerate, 1)));

	 pw_stream_add_listener (ctx->stream, &ctx->stream_listener,
	 	&pwr_stream_events, state);

	pw_stream_connect(ctx->stream,
		PW_DIRECTION_OUTPUT,
		PW_ID_ANY,
		(PW_STREAM_FLAG_DRIVER |
			PW_STREAM_FLAG_MAP_BUFFERS),
		params, 1);

	return NULL;
}
