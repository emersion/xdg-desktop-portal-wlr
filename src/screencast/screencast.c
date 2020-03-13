#include "screencast.h"

static const char object_path[] = "/org/freedesktop/portal/desktop";
static const char interface_name[] = "org.freedesktop.impl.portal.ScreenCast";

int setup_outputs(struct xdpw_state *state) {

	struct screencast_context *ctx = &state->screencast;

	struct wayland_output *output, *tmp_o;
	wl_list_for_each_reverse_safe(output, tmp_o, &ctx->output_list, link) {
		logprint(INFO, "wlroots: capturable output: %s model: %s: id: %i name: %s",
			output->make, output->model, output->id, output->name);
	}

	struct wayland_output *out;
	if (ctx->output_name) {
		out = wlr_output_find_by_name(&ctx->output_list, ctx->output_name);
		if (!out) {
			logprint(ERROR, "wlroots: no such output");
			abort();
		}
	} else {
		out = wlr_output_first(&ctx->output_list);
		if (!out) {
			logprint(ERROR, "wlroots: no output found");
			abort();
		}
	}

	ctx->target_output = out;
	ctx->framerate = out->framerate;
	ctx->with_cursor = true;

	logprint(INFO, "wlroots: output: %s", ctx->target_output->name);
	logprint(INFO, "wlroots: wl_display fd: %d", wl_display_get_fd(state->wl_display));

	return 0;

}

void *start_screencast(void *data) {
	struct xdpw_state *state = data;

	wlr_register_cb(state);
	// process at least one frame so that we know
	// some of the metadata required for the pipewire
	// remote state connected event
	wl_display_dispatch(state->wl_display);
	wl_display_roundtrip(state->wl_display);

	pwr_start(state);

	return NULL;
}

static int method_screencast_create_session(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	int ret = 0;

	logprint(INFO, "dbus: create session method invoked");

	char *request_handle, *session_handle, *app_id;
	ret = sd_bus_message_read(msg, "oos", &request_handle, &session_handle, &app_id);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}

	logprint(INFO, "dbus: request_handle: %s", request_handle);
	logprint(INFO, "dbus: session_handle: %s", session_handle);
	logprint(INFO, "dbus: app_id: %s", app_id);

	char* key;
	int innerRet = 0;
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		innerRet = sd_bus_message_read(msg, "s", &key);
		if (innerRet < 0) {
			return innerRet;
		}

		if (strcmp(key, "session_handle_token") == 0) {
			char* token;
			sd_bus_message_read(msg, "v", "s", &token);
			logprint(INFO, "dbus: option token: %s", token);
		} else {
			logprint(WARN, "dbus: unknown option: %s", key);
			sd_bus_message_skip(msg, "v");
		}

		innerRet = sd_bus_message_exit_container(msg);
		if (innerRet < 0) {
			return innerRet;
		}
	}
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_exit_container(msg);
	if (ret < 0) {
		return ret;
	}

	// TODO: cleanup this
	struct xdpw_request *req =
		request_create(sd_bus_message_get_bus(msg), request_handle);
	if (req == NULL) {
		return -ENOMEM;
	}

	// TODO: cleanup this
	struct xdpw_session *sess =
		session_create(sd_bus_message_get_bus(msg), session_handle);
	if (sess == NULL) {
		return -ENOMEM;
	}

	sd_bus_message *reply = NULL;
	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_SUCCESS, 0);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}

	sd_bus_message_unref(reply);
	return 0;
}


static int method_screencast_select_sources(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;

	logprint(INFO, "dbus: select sources method invoked");

	setup_outputs(state);

	char *request_handle, *session_handle, *app_id;
	ret = sd_bus_message_read(msg, "oos", &request_handle, &session_handle, &app_id);
	if (ret < 0) {
		return ret;
	}
	sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}

	logprint(INFO, "dbus: request_handle: %s", request_handle);
	logprint(INFO, "dbus: session_handle: %s", session_handle);
	logprint(INFO, "dbus: app_id: %s", app_id);

	char* key;
	int innerRet = 0;
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		innerRet = sd_bus_message_read(msg, "s", &key);
		if (innerRet < 0) {
			return innerRet;
		}

		if (strcmp(key, "multiple") == 0) {
			bool multiple;
			sd_bus_message_read(msg, "v", "b", &multiple);
			logprint(INFO, "dbus: option multiple: %x", multiple);
		} else if (strcmp(key, "types") == 0) {
			uint32_t mask;
			sd_bus_message_read(msg, "v", "u", &mask);
			logprint(INFO, "dbus: option types:%x", mask);
		} else {
			logprint(WARN, "dbus: unknown option %s", key);
			sd_bus_message_skip(msg, "v");
		}

		innerRet = sd_bus_message_exit_container(msg);
		if (ret < 0) {
			return ret;
		}
	}
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_exit_container(msg);
	if (ret < 0) {
		return ret;
	}


	sd_bus_message *reply = NULL;
	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_SUCCESS, 0);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}

	sd_bus_message_unref(reply);
	return 0;
}

static int method_screencast_start(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct xdpw_state *state = data;
	struct screencast_context *ctx = &state->screencast;

	int ret = 0;

	logprint(INFO, "dbus: start method invoked");

	start_screencast(state);

	char *request_handle, *session_handle, *app_id, *parent_window;
	ret = sd_bus_message_read(msg, "ooss", &request_handle, &session_handle, &app_id, &parent_window);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}

	logprint(INFO, "dbus: request_handle: %s", request_handle);
	logprint(INFO, "dbus: session_handle: %s", session_handle);
	logprint(INFO, "dbus: app_id: %s", app_id);
	logprint(INFO, "dbus: parent_window: %s", parent_window);

	char* key;
	int innerRet = 0;
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		innerRet = sd_bus_message_read(msg, "s", &key);
		if (innerRet < 0) {
			return innerRet;
		}

		logprint(WARN, "dbus: unknown option: %s", key);
		sd_bus_message_skip(msg, "v");

		innerRet = sd_bus_message_exit_container(msg);
		if (innerRet < 0) {
			return innerRet;
		}
	}
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_exit_container(msg);
	if (ret < 0) {
		return ret;
	}

	sd_bus_message *reply = NULL;
	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}

	while (ctx->node_id == 0) {
		int ret = pw_loop_iterate(state->pw_loop, 0);
		if (ret < 0) {
			logprint(ERROR, "pipewire_loop_iterate failed: %s", spa_strerror(ret));
		}
	}

	ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_SUCCESS, 1,
		"streams", "a(ua{sv})", 1,
		ctx->node_id, 2,
		"position", "(ii)", 0, 0,
		"size", "(ii)", ctx->simple_frame.width, ctx->simple_frame.height);

	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}
	sd_bus_message_unref(reply);

	return 0;
}

static const sd_bus_vtable screencast_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}",
		method_screencast_create_session, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SelectSources", "oosa{sv}", "ua{sv}",
		method_screencast_select_sources, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Start", "oossa{sv}", "ua{sv}",
		method_screencast_start, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

int init_screencast(struct xdpw_state *state, const char *output_name, const char *forced_pixelformat) {
	// TODO: cleanup
	sd_bus_slot *slot = NULL;

	state->screencast = (struct screencast_context) { 0 };
	state->screencast.forced_pixelformat = forced_pixelformat;
	state->screencast.output_name = output_name;
	state->screencast.simple_frame = (struct simple_frame) { 0 };
	state->screencast.simple_frame.damage = &(struct damage) { 0 };

	int err = wlr_screencopy_init(state);
	if (err) {
		goto end;
	}

	return sd_bus_add_object_vtable(state->bus, &slot, object_path, interface_name,
		screencast_vtable, state);

end:
	wlr_screencopy_uninit(&state->screencast);
	return err;
}
