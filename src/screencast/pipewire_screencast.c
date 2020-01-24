#include "pipewire_screencast.h"

static inline void init_type(struct pwr_type *type, struct pw_type *map) {
	pw_type_get(map, SPA_TYPE__MediaType, &type->media_type);
	pw_type_get(map, SPA_TYPE__MediaSubtype, &type->media_subtype);
	pw_type_get(map, SPA_TYPE_FORMAT__Video, &type->format_video);
	pw_type_get(map, SPA_TYPE__VideoFormat, &type->video_format);
	pw_type_get(map, SPA_TYPE_META__Cursor, &type->meta_cursor);
};

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
	struct screencast_context *ctx = data;
	struct pw_buffer *pw_buf;
	struct spa_buffer *spa_buf;
	struct spa_meta_header *h;
	struct spa_data *d;

	if (!ctx->stream_state) {
		wlr_frame_free(ctx);
		return;
	}

	logger("pw event fired\n");

	if ((pw_buf = pw_stream_dequeue_buffer(ctx->stream)) == NULL) {
		printf("out of buffers\n");
		return;
	}

	spa_buf = pw_buf->buffer;
	d = spa_buf->datas;
	if ((d[0].data) == NULL)
		return;
	if ((h = spa_buffer_find_meta(spa_buf, ctx->t->meta.Header))) {
		h->pts = -1;
		h->flags = 0;
		h->seq = ctx->seq++;
		h->dts_offset = 0;
	}

	d[0].type = ctx->t->data.MemPtr;
	d[0].maxsize = ctx->simple_frame.size;
	// d[0].data = ctx->simple_frame.data;
	d[0].mapoffset = 0;
	d[0].chunk->size = ctx->simple_frame.size;
	d[0].chunk->stride = ctx->simple_frame.stride;
	d[0].chunk->offset = 0;
	d[0].flags = 0;
	d[0].fd = -1;

	writeFrameData(d[0].data, ctx->simple_frame.data, ctx->simple_frame.height,
								 ctx->simple_frame.stride, ctx->simple_frame.y_invert);

	logger("************** \n");
	logger("pointer: %p\n", d[0].data);
	logger("size: %d\n", d[0].maxsize);
	logger("stride: %d\n", d[0].chunk->stride);
	logger("width: %d\n", ctx->simple_frame.width);
	logger("height: %d\n", ctx->simple_frame.height);
	logger("y_invert: %d\n", ctx->simple_frame.y_invert);
	logger("************** \n");

	pw_stream_queue_buffer(ctx->stream, pw_buf);

	wlr_frame_free(ctx);
}

static void pwr_handle_stream_state_changed(void *data,
																						enum pw_stream_state old,
																						enum pw_stream_state state,
																						const char *error) {
	struct screencast_context *ctx = data;
	ctx->node_id = pw_stream_get_node_id(ctx->stream);

	printf("stream state: \"%s\"\n", pw_stream_state_as_string(state));
	printf("node id: %d\n", ctx->node_id);

	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		ctx->stream_state = false;
		break;
	case PW_STREAM_STATE_STREAMING:
		ctx->stream_state = true;
		break;
	default:
		ctx->stream_state = false;
		break;
	}
}

static void pwr_handle_stream_format_changed(void *data,
																						 const struct spa_pod *format) {
	struct screencast_context *ctx = data;
	struct pw_stream *stream = ctx->stream;
	struct pw_type *t = ctx->t;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b =
			SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[2];

	if (format == NULL) {
		pw_stream_finish_format(stream, 0, NULL, 0);
		return;
	}
	spa_format_video_raw_parse(format, &ctx->pwr_format, &ctx->type.format_video);

	params[0] = spa_pod_builder_object(
			&b, t->param.idBuffers, t->param_buffers.Buffers, ":",
			t->param_buffers.size, "i", ctx->simple_frame.size, ":",
			t->param_buffers.stride, "i", ctx->simple_frame.stride, ":",
			t->param_buffers.buffers, "iru", BUFFERS, SPA_POD_PROP_MIN_MAX(1, 32),
			":", t->param_buffers.align, "i", ALIGN);

	params[1] = spa_pod_builder_object(&b, t->param.idMeta, t->param_meta.Meta,
																		 ":", t->param_meta.type, "I",
																		 t->meta.Header, ":", t->param_meta.size,
																		 "i", sizeof(struct spa_meta_header));

	pw_stream_finish_format(stream, 0, params, 2);
}

static const struct pw_stream_events pwr_stream_events = {
		PW_VERSION_STREAM_EVENTS,
		.state_changed = pwr_handle_stream_state_changed,
		.format_changed = pwr_handle_stream_format_changed,
};

static void pwr_handle_state_changed(void *data, enum pw_remote_state old,
																		 enum pw_remote_state state,
																		 const char *error) {
	struct screencast_context *ctx = data;
	struct pw_remote *remote = ctx->remote;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		printf("remote error: %s\n", error);
		pw_main_loop_quit(ctx->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED: {
		const struct spa_pod *params[1];
		uint8_t buffer[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

		logger("remote state: \"%s\"\n", pw_remote_state_as_string(state));

		ctx->stream = pw_stream_new(
				remote, "wlr_screeencopy",
				pw_properties_new("media.class", "Video/Source", PW_NODE_PROP_MEDIA,
													"Video", PW_NODE_PROP_CATEGORY, "Source",
													PW_NODE_PROP_ROLE, "Screen", NULL));

		params[0] = spa_pod_builder_object(
				&b, ctx->t->param.idEnumFormat, ctx->t->spa_format, "I",
				ctx->type.media_type.video, "I", ctx->type.media_subtype.raw, ":",
				ctx->type.format_video.format, "I", pipewire_from_wl_shm(ctx), ":",
				ctx->type.format_video.size, "Rru",
				&SPA_RECTANGLE(ctx->simple_frame.width, ctx->simple_frame.height),
				SPA_POD_PROP_MIN_MAX(&SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(4096, 4096)),
				":", ctx->type.format_video.framerate, "F",
				&SPA_FRACTION(ctx->framerate, 1));

		pw_stream_add_listener(ctx->stream, &ctx->stream_listener,
													 &pwr_stream_events, ctx);

		pw_stream_connect(ctx->stream, PW_DIRECTION_OUTPUT, NULL,
											PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS,
											params, 1);

		break;
	}
	default:
		logger("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events pwr_remote_events = {
		PW_VERSION_REMOTE_EVENTS,
		.state_changed = pwr_handle_state_changed,
};

void *pwr_start(void *data) {

	struct screencast_context *ctx = data;

	pw_init(NULL, NULL);

	/* create a main loop */
	ctx->loop = pw_main_loop_new(NULL);
	ctx->core = pw_core_new(pw_main_loop_get_loop(ctx->loop), NULL);
	ctx->t = pw_core_get_type(ctx->core);
	ctx->remote = pw_remote_new(ctx->core, NULL, 0);

	init_type(&ctx->type, ctx->t);

	/* make an event to signal frame ready */
	ctx->event =
			pw_loop_add_event(pw_main_loop_get_loop(ctx->loop), pwr_on_event, ctx);

	pw_remote_add_listener(ctx->remote, &ctx->remote_listener, &pwr_remote_events,
												 ctx);
	pw_remote_connect(ctx->remote);

	/* run the loop, this will trigger the callbacks */
	pw_main_loop_run(ctx->loop);

	pw_core_destroy(ctx->core);
	pw_main_loop_destroy(ctx->loop);
	return NULL;
}
