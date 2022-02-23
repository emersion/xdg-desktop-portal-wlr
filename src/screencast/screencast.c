#include "screencast.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <drm_fourcc.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <spa/utils/result.h>

#include "pipewire_screencast.h"
#include "wlr_screencast.h"
#include "xdpw.h"
#include "logger.h"

static const char object_path[] = "/org/freedesktop/portal/desktop";
static const char interface_name[] = "org.freedesktop.impl.portal.ScreenCast";

void exec_with_shell(char *command) {
	pid_t pid1 = fork();
	if (pid1 < 0) {
		perror("fork");
		return;
	} else if (pid1 == 0) {
		pid_t pid2 = fork();
		if (pid2 < 0) {
			perror("fork");
		} else if (pid2 == 0) {
			char *const argv[] = {
				"sh",
				"-c",
				command,
				NULL,
			};
			execvp("sh", argv);
			perror("execvp");
			_exit(127);
		}
		_exit(0);
	}
	int stat;
	if (waitpid(pid1, &stat, 0) < 0) {
		perror("waitpid");
	}
}

void xdpw_screencast_instance_init(struct xdpw_screencast_context *ctx,
		struct xdpw_screencast_instance *cast, struct xdpw_wlr_output *out, bool with_cursor) {

	// only run exec_before if there's no other instance running that already ran it
	if (wl_list_empty(&ctx->screencast_instances)) {
		char *exec_before = ctx->state->config->screencast_conf.exec_before;
		if (exec_before) {
			logprint(INFO, "xdpw: executing %s before screencast", exec_before);
			exec_with_shell(exec_before);
		}
	}

	cast->ctx = ctx;
	cast->target_output = out;
	if (ctx->state->config->screencast_conf.max_fps > 0) {
		cast->max_framerate = ctx->state->config->screencast_conf.max_fps < (uint32_t)out->framerate ?
			ctx->state->config->screencast_conf.max_fps : (uint32_t)out->framerate;
	} else {
		cast->max_framerate = (uint32_t)out->framerate;
	}
	cast->framerate = cast->max_framerate;
	cast->with_cursor = with_cursor;
	cast->refcount = 1;
	cast->node_id = SPA_ID_INVALID;
	cast->avoid_dmabufs = false;
	cast->teardown = false;
	wl_list_init(&cast->buffer_list);
	logprint(INFO, "xdpw: screencast instance %p has %d references", cast, cast->refcount);
	wl_list_insert(&ctx->screencast_instances, &cast->link);
	logprint(INFO, "xdpw: %d active screencast instances",
		wl_list_length(&ctx->screencast_instances));
}

void xdpw_screencast_instance_destroy(struct xdpw_screencast_instance *cast) {
	assert(cast->refcount == 0); // Fails assert if called by screencast_finish
	logprint(DEBUG, "xdpw: destroying cast instance");

	if (cast->ctx->ext_screencopy_manager) {
		xdpw_wlr_ext_screencopy_surface_destroy(cast);
	}

	// make sure this is the last running instance that is being destroyed
	if (wl_list_length(&cast->link) == 1) {
		char *exec_after = cast->ctx->state->config->screencast_conf.exec_after;
		if (exec_after) {
			logprint(INFO, "xdpw: executing %s after screencast", exec_after);
			exec_with_shell(exec_after);
		}
	}

	wl_list_remove(&cast->link);
	xdpw_pwr_stream_destroy(cast);
	assert(wl_list_length(&cast->buffer_list) == 0);
	free(cast);
}

void xdpw_screencast_instance_teardown(struct xdpw_screencast_instance *cast) {
	struct xdpw_session *sess, *tmp;
	wl_list_for_each_safe(sess, tmp, &cast->ctx->state->xdpw_sessions, link) {
		if (sess->screencast_instance == cast) {
			xdpw_session_destroy(sess);
		}
	}
}

bool setup_outputs(struct xdpw_screencast_context *ctx, struct xdpw_session *sess, bool with_cursor) {

	struct xdpw_wlr_output *output, *tmp_o;
	wl_list_for_each_reverse_safe(output, tmp_o, &ctx->output_list, link) {
		logprint(INFO, "wlroots: capturable output: %s model: %s: id: %i name: %s",
			output->make, output->model, output->id, output->name);
	}

	struct xdpw_wlr_output *out;
	out = xdpw_wlr_output_chooser(ctx);
	if (!out) {
		logprint(ERROR, "wlroots: no output found");
		return false;
	}

	// Disable screencast sharing to avoid sharing between dmabuf and shm capable clients
	/*
	struct xdpw_screencast_instance *cast, *tmp_c;
	wl_list_for_each_reverse_safe(cast, tmp_c, &ctx->screencast_instances, link) {
		logprint(INFO, "xdpw: existing screencast instance: %d %s cursor",
			cast->target_output->id,
			cast->with_cursor ? "with" : "without");

		if (cast->target_output->id == out->id && cast->with_cursor == with_cursor) {
			if (cast->refcount == 0) {
				logprint(DEBUG,
					"xdpw: matching cast instance found, "
					"but is already scheduled for destruction, skipping");
			}
			else {
				sess->screencast_instance = cast;
				++cast->refcount;
			}
			logprint(INFO, "xdpw: screencast instance %p now has %d references",
				cast, cast->refcount);
		}
	}
	*/

	if (!sess->screencast_instance) {
		sess->screencast_instance = calloc(1, sizeof(struct xdpw_screencast_instance));
		xdpw_screencast_instance_init(ctx, sess->screencast_instance,
			out, with_cursor);
	}
	logprint(INFO, "wlroots: output: %s",
		sess->screencast_instance->target_output->name);

	return true;

}

static int start_screencast(struct xdpw_screencast_instance *cast) {
	if (cast->ctx->ext_screencopy_manager) {
		xdpw_wlr_ext_screencopy_surface_create(cast);

		// process at least one frame so that we know
		// some of the metadata required for the pipewire
		// remote state connected event
		wl_display_dispatch(cast->ctx->state->wl_display);
		wl_display_roundtrip(cast->ctx->state->wl_display);
	} else {
		xdpw_wlr_register_cb(cast);

		// process at least one frame so that we know
		// some of the metadata required for the pipewire
		// remote state connected event
		wl_display_dispatch(cast->ctx->state->wl_display);
		wl_display_roundtrip(cast->ctx->state->wl_display);

		if (cast->screencopy_frame_info[WL_SHM].format == DRM_FORMAT_INVALID ||
				(cast->ctx->state->screencast_version >= 3 &&
				 cast->screencopy_frame_info[DMABUF].format == DRM_FORMAT_INVALID)) {
			logprint(INFO, "wlroots: unable to receive a valid format from wlr_screencopy");
			return -1;
		}

		xdpw_pwr_stream_create(cast);
	}

	cast->initialized = true;
	return 0;
}

static int method_screencast_create_session(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

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

	char *key;
	int innerRet = 0;
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		innerRet = sd_bus_message_read(msg, "s", &key);
		if (innerRet < 0) {
			return innerRet;
		}

		if (strcmp(key, "session_handle_token") == 0) {
			char *token;
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

	struct xdpw_request *req =
		xdpw_request_create(sd_bus_message_get_bus(msg), request_handle);
	if (req == NULL) {
		return -ENOMEM;
	}

	struct xdpw_session *sess =
		xdpw_session_create(state, sd_bus_message_get_bus(msg), strdup(session_handle));
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
	struct xdpw_screencast_context *ctx = &state->screencast;

	int ret = 0;
	struct xdpw_session *sess, *tmp_s;
	sd_bus_message *reply = NULL;

	logprint(INFO, "dbus: select sources method invoked");

	// default to embedded cursor mode if not specified
	bool cursor_embedded = true;

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

	char *key;
	int innerRet = 0;
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		innerRet = sd_bus_message_read(msg, "s", &key);
		if (innerRet < 0) {
			return innerRet;
		}

		if (strcmp(key, "multiple") == 0) {
			int multiple;
			sd_bus_message_read(msg, "v", "b", &multiple);
			logprint(INFO, "dbus: option multiple: %d", multiple);
		} else if (strcmp(key, "types") == 0) {
			uint32_t mask;
			sd_bus_message_read(msg, "v", "u", &mask);
			if (mask & (1<<WINDOW)) {
				logprint(INFO, "dbus: non-monitor cast requested, not replying");
				return -1;
			}
			logprint(INFO, "dbus: option types:%x", mask);
		} else if (strcmp(key, "cursor_mode") == 0) {
			uint32_t cursor_mode;
			sd_bus_message_read(msg, "v", "u", &cursor_mode);
			if (cursor_mode & HIDDEN) {
				cursor_embedded = false;
			}
			if (cursor_mode & METADATA) {
				logprint(ERROR, "dbus: unsupported cursor mode requested, cancelling");
				goto error;
			}
			logprint(INFO, "dbus: option cursor_mode:%x", cursor_mode);
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

	bool output_selection_canceled = 1;
	wl_list_for_each_reverse_safe(sess, tmp_s, &state->xdpw_sessions, link) {
		if (strcmp(sess->session_handle, session_handle) == 0) {
				logprint(DEBUG, "dbus: select sources: found matching session %s", sess->session_handle);
				output_selection_canceled = !setup_outputs(ctx, sess, cursor_embedded);
		}
	}

	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}
	if (output_selection_canceled) {
		ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_CANCELLED, 0);
	} else {
		ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_SUCCESS, 0);
	}
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}
	sd_bus_message_unref(reply);
	return 0;

error:
	wl_list_for_each_reverse_safe(sess, tmp_s, &state->xdpw_sessions, link) {
		if (strcmp(sess->session_handle, session_handle) == 0) {
				logprint(DEBUG, "dbus: select sources error: destroying matching session %s", sess->session_handle);
				xdpw_session_destroy(sess);
		}
	}

	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_CANCELLED, 0);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}
	sd_bus_message_unref(reply);
	return -1;
}

static int method_screencast_start(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;

	logprint(INFO, "dbus: start method invoked");

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

	char *key;
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

	struct xdpw_screencast_instance *cast = NULL;
	struct xdpw_session *sess, *tmp_s;
	wl_list_for_each_reverse_safe(sess, tmp_s, &state->xdpw_sessions, link) {
		if (strcmp(sess->session_handle, session_handle) == 0) {
				logprint(DEBUG, "dbus: start: found matching session %s", sess->session_handle);
				cast = sess->screencast_instance;
		}
	}
	if (!cast) {
		return -1;
	}

	if (!cast->initialized) {
		ret = start_screencast(cast);
	}
	if (ret < 0) {
		return ret;
	}

	while (cast->node_id == SPA_ID_INVALID) {
		int ret = pw_loop_iterate(state->pw_loop, 0);
		if (ret < 0) {
			logprint(ERROR, "pipewire_loop_iterate failed: %s", spa_strerror(ret));
			return ret;
		}
	}

	sd_bus_message *reply = NULL;
	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}

	logprint(DEBUG, "dbus: start: returning node %d", (int)cast->node_id);
	ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_SUCCESS, 1,
		"streams", "a(ua{sv})", 1,
		cast->node_id, 2,
		"position", "(ii)", 0, 0,
		"size", "(ii)", cast->screencopy_frame_info[WL_SHM].width, cast->screencopy_frame_info[WL_SHM].height);

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
	SD_BUS_PROPERTY("AvailableSourceTypes", "u", NULL,
		offsetof(struct xdpw_state, screencast_source_types),
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("AvailableCursorModes", "u", NULL,
		offsetof(struct xdpw_state, screencast_cursor_modes),
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("version", "u", NULL,
		offsetof(struct xdpw_state, screencast_version),
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

int xdpw_screencast_init(struct xdpw_state *state) {
	sd_bus_slot *slot = NULL;

	state->screencast = (struct xdpw_screencast_context) { 0 };
	state->screencast.state = state;

	int err;
	err = xdpw_pwr_context_create(state);
	if (err) {
		goto fail_pipewire;
	}

	err = xdpw_wlr_screencopy_init(state);
	if (err) {
		goto fail_screencopy;
	}

	return sd_bus_add_object_vtable(state->bus, &slot, object_path, interface_name,
		screencast_vtable, state);

fail_screencopy:
	xdpw_wlr_screencopy_finish(&state->screencast);

fail_pipewire:
	xdpw_pwr_context_destroy(state);

	return err;
}
