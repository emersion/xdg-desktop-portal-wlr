#include "remotedesktop.h"

#include <spa/utils/result.h>
#include <time.h>

#include "config.h"
#include "remotedesktop_common.h"
#include "screencast.h"
#include "virtual_input.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdpw.h"

static const char object_path[] = "/org/freedesktop/portal/desktop";
static const char interface_name[] = "org.freedesktop.impl.portal.RemoteDesktop";

static struct xdpw_session *get_session_from_handle(struct xdpw_state *state, char *session_handle) {
	struct xdpw_session *sess;
	wl_list_for_each_reverse(sess, &state->xdpw_sessions, link) {
		if (strcmp(sess->session_handle, session_handle) == 0) {
			return sess;
		}
	}
	return NULL;
}

static int method_remotedesktop_create_session(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *request_handle, *session_handle, *app_id, *key;
	struct xdpw_request *req;
	struct xdpw_session *sess;

	logprint(DEBUG, "remotedesktop: create session: method invoked");

	ret = sd_bus_message_read(msg, "oos", &request_handle, &session_handle, &app_id);
	if (ret < 0) {
		return ret;
	}

	logprint(DEBUG, "remotedesktop: create session: request_handle: %s", request_handle);
	logprint(DEBUG, "remotedesktop: create session: session_handle: %s", session_handle);
	logprint(DEBUG, "remotedesktop: create session: app_id: %s", app_id);

	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		ret = sd_bus_message_read(msg, "s", &key);
		if (ret < 0) {
			return ret;
		}

		if (strcmp(key, "session_handle_token") == 0) {
			char *token;
			sd_bus_message_read(msg, "v", "s", &token);
			logprint(DEBUG, "remotedesktop: create session: session handle token: %s", token);
		} else {
			logprint(WARN, "remotedesktop: create session: unknown option: %s", key);
			sd_bus_message_skip(msg, "v");
		}

		ret = sd_bus_message_exit_container(msg);
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

	req = xdpw_request_create(sd_bus_message_get_bus(msg), request_handle);
	if (req == NULL) {
		return -ENOMEM;
	}

	sess = xdpw_session_create(state, sd_bus_message_get_bus(msg), strdup(session_handle));
	if (sess == NULL) {
		return -ENOMEM;
	}

	ret = sd_bus_reply_method_return(msg, "ua{sv}", PORTAL_RESPONSE_SUCCESS, 0);
	if (ret < 0) {
		return ret;
	}
	return 0;
}

static int method_remotedesktop_select_devices(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *request_handle, *session_handle, *app_id, *key;
	struct xdpw_session *sess;

	logprint(DEBUG, "remotedesktop: select devices: method invoked");

	ret = sd_bus_message_read(msg, "oos", &request_handle, &session_handle, &app_id);
	if (ret < 0) {
		return ret;
	}

	logprint(DEBUG, "remotedesktop: select devices: request_handle: %s", request_handle);
	logprint(DEBUG, "remotedesktop: select devices: session_handle: %s", session_handle);
	logprint(DEBUG, "remotedesktop: select devices: app_id: %s", app_id);

	sess = get_session_from_handle(state, session_handle);
	if (!sess) {
		logprint(WARN, "remotedesktop: select devices: session not found");
		return -1;
	}
	logprint(DEBUG, "remotedesktop: select devices: session found");

	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		ret = sd_bus_message_read(msg, "s", &key);
		if (ret < 0) {
			return ret;
		}

		if (strcmp(key, "types") == 0) {
			uint32_t types;
			ret = sd_bus_message_read(msg, "v", "u", &types);
			if (ret < 0) {
				return ret;
			}
			logprint(DEBUG, "remotedesktop: select devices: option types: %x", types);
			uint32_t allowed_types = state->config->remotedesktop_conf.allowed_devices;
			if ((types & ~allowed_types) != 0) {
				logprint(DEBUG, "remotedesktop: tried to select not allowed device, "
						"selected types: 0x%x, allowed types: 0x%x.", types, allowed_types);
				types &= allowed_types;
			}
			sess->remotedesktop_data.devices = types;
		} else {
			logprint(WARN, "remotedesktop: select devices: unknown option: %s", key);
			sd_bus_message_skip(msg, "v");
		}

		ret = sd_bus_message_exit_container(msg);
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

	ret = sd_bus_reply_method_return(msg, "ua{sv}", PORTAL_RESPONSE_SUCCESS, 0);
	if (ret < 0) {
		return ret;
	}
	return 0;
}

static int method_remotedesktop_start(sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *request_handle, *session_handle, *app_id, *parent_window, *key;
	struct xdpw_session *sess;
	struct xdpw_remotedesktop_session_data *remote;

	logprint(DEBUG, "remotedesktop: start: method invoked");

	ret = sd_bus_message_read(msg, "oos", &request_handle, &session_handle, &app_id);
	if (ret < 0) {
		return ret;
	}

	logprint(DEBUG, "remotedesktop: start: request_handle: %s", request_handle);
	logprint(DEBUG, "remotedesktop: start: session_handle: %s", session_handle);
	logprint(DEBUG, "remotedesktop: start: app_id: %s", app_id);

	sess = get_session_from_handle(state, session_handle);
	if (!sess) {
		logprint(WARN, "remotedesktop: start: session not found");
		return -1;
	}
	logprint(DEBUG, "remotedesktop: start: session found");
	struct xdpw_screencast_instance *cast = sess->screencast_data.screencast_instance;
	logprint(DEBUG, "remotedesktop: screencast instance %x", cast);

	if (cast) {
		logprint(DEBUG, "remotedesktop: starting screencast");
		if (!cast->initialized) {
			ret = xdpw_screencast_start(cast);
			if (ret < 0) {
				return ret;
			}
		}
		while (cast->node_id == SPA_ID_INVALID) {
			int ret = pw_loop_iterate(state->pw_loop, 0);
			if (ret < 0) {
				logprint(ERROR, "pipewire_loop_iterate failed: %s", spa_strerror(ret));
				return ret;
			}
		}
	}

	remote = &sess->remotedesktop_data;
	remote->virtual_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
		state->remotedesktop.virtual_pointer_manager, NULL);

	// TODO: make this user configureable
	struct xkb_rule_names rule_names = {
		.rules = "evdev",
		.layout = "us",
		.model = "pc105",
		.variant = "",
		.options = "",
	};
	logprint(DEBUG, "Creating virtual keyboard with manager 0x%x",
			state->remotedesktop.virtual_keyboard_manager);
	remote->keyboard.virtual_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
			state->remotedesktop.virtual_keyboard_manager, state->remotedesktop.seat);
	ret = keyboard_init(&remote->keyboard, &rule_names);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_read(msg, "s", &parent_window);
	if (ret < 0) {
		return ret;
	}
	logprint(DEBUG, "remotedesktop: start: parent window: %s", parent_window);

	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		ret = sd_bus_message_read(msg, "s", &key);
		if (ret < 0) {
			return ret;
		}

		logprint(WARN, "remotedesktop: start: unknown option: %s", key);
		sd_bus_message_skip(msg, "v");

		ret = sd_bus_message_exit_container(msg);
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
	ret = sd_bus_message_append(reply, "u", PORTAL_RESPONSE_SUCCESS);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_open_container(reply, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_append(reply, "{sv}",
		"devices", "u", remote->devices);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_open_container(reply, 'e', "sv");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append(reply, "s", "streams");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_open_container(reply, 'v', "a(ua{sv})");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_open_container(reply, 'a', "(ua{sv})");
	if (ret < 0) {
		return ret;
	}
	if (cast) {
		ret = sd_bus_message_open_container(reply, 'r', "ua{sv}");
		if (ret < 0) {
			return ret;
		}
		ret = sd_bus_message_append(reply, "u", cast->node_id);
		if (ret < 0) {
			return ret;
		}
		ret = sd_bus_message_open_container(reply, 'a', "{sv}");
		if (ret < 0) {
			return ret;
		}
		if (cast->target->output->xdg_output) {
			ret = sd_bus_message_append(reply, "{sv}",
				"position", "(ii)", cast->target->output->x, cast->target->output->y);
			if (ret < 0) {
				return ret;
			}
			ret = sd_bus_message_append(reply, "{sv}",
				"size", "(ii)", cast->target->output->width, cast->target->output->height);
			if (ret < 0) {
				return ret;
			}
		}
		ret = sd_bus_message_append(reply, "{sv}", "source_type", "u", MONITOR);
		if (ret < 0) {
			return ret;
		}
		ret = sd_bus_message_close_container(reply);
		if (ret < 0) {
			return ret;
		}
		ret = sd_bus_message_close_container(reply);
		if (ret < 0) {
			return ret;
		}
	}
	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_append(reply, "{sv}",
		"devices", "u", remote->devices);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_close_container(reply);
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

static int method_remotedesktop_notify_pointer_motion(sd_bus_message *msg,
		void *data, sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *session_handle;
	struct xdpw_session *sess;
	double dx = 0, dy = 0;

	logprint(TRACE, "remotedesktop: npm: method invoked");

	ret = sd_bus_message_read(msg, "o", &session_handle);
	if (ret < 0) {
		return ret;
	}
	logprint(TRACE, "remotedesktop: npm: session_handle: %s", session_handle);

	sess = get_session_from_handle(state, session_handle);
	if (!sess) {
		logprint(WARN, "remotedesktop: npm: session not found");
		return -1;
	}
	logprint(TRACE, "remotedesktop: npm: session found");

	if (!(sess->remotedesktop_data.devices & POINTER)) {
		logprint(ERROR, "remotedesktop: npm: called, but pointer not selected!");
		return -1;
	}

	ret = sd_bus_message_skip(msg, "a{sv}");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "dd", &dx, &dy);
	if (ret < 0) {
		return ret;
	}

	zwlr_virtual_pointer_v1_motion(sess->remotedesktop_data.virtual_pointer, get_time_ms(),
		wl_fixed_from_double(dx), wl_fixed_from_double(dy));
	zwlr_virtual_pointer_v1_frame(sess->remotedesktop_data.virtual_pointer);

	return 0;
}

static int method_remotedesktop_notify_pointer_motion_absolute(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *session_handle;
	struct xdpw_session *sess;
	double x = 0, y = 0;

	logprint(TRACE, "remotedesktop: npma: method invoked");

	ret = sd_bus_message_read(msg, "o", &session_handle);
	if (ret < 0) {
		return ret;
	}
	logprint(TRACE, "remotedesktop: npma: session_handle: %s", session_handle);

	sess = get_session_from_handle(state, session_handle);
	if (!sess) {
		logprint(WARN, "remotedesktop: npma: session not found");
		return -1;
	}
	logprint(TRACE, "remotedesktop: npma: session found");

	if (!(sess->remotedesktop_data.devices & POINTER)) {
		logprint(ERROR, "remotedesktop: npma: called, but pointer not selected!");
		return -1;
	}

	ret = sd_bus_message_skip(msg, "a{sv}");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_skip(msg, "u");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "dd", &x, &y);
	if (ret < 0) {
		return ret;
	}

	struct xdpw_wlr_output *output = sess->screencast_data.screencast_instance->target->output;
	zwlr_virtual_pointer_v1_motion_absolute(sess->remotedesktop_data.virtual_pointer,
		get_time_ms(), x, y, output->width, output->height);
	zwlr_virtual_pointer_v1_frame(sess->remotedesktop_data.virtual_pointer);

	return 0;
}

static int method_remotedesktop_notify_pointer_button(sd_bus_message *msg,
		void *data, sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *session_handle;
	struct xdpw_session *sess;
	int32_t button;
	uint32_t btn_state;

	logprint(DEBUG, "remotedesktop: npb: method invoked");

	ret = sd_bus_message_read(msg, "o", &session_handle);
	if (ret < 0) {
		return ret;
	}
	logprint(DEBUG, "remotedesktop: npb: session_handle: %s", session_handle);

	sess = get_session_from_handle(state, session_handle);
	if (!sess) {
		logprint(WARN, "remotedesktop: npb: session not found");
		return -1;
	}
	logprint(DEBUG, "remotedesktop: npb: session found");

	if (!(sess->remotedesktop_data.devices & POINTER)) {
		logprint(ERROR, "remotedesktop: npb: called, but pointer not selected!");
		return -1;
	}

	ret = sd_bus_message_skip(msg, "a{sv}");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "i", &button);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "u", &btn_state);
	if (ret < 0) {
		return ret;
	}

	if (btn_state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (sess->remotedesktop_data.pressed_buttons & 1<<button) {
			logprint(WARN, "remotedesktop: npb: pointer already pressed, ignoring");
			return 0;
		}
		sess->remotedesktop_data.pressed_buttons |= 1<<button;
	} else {
		sess->remotedesktop_data.pressed_buttons &= ~(1<<button);
	}
	zwlr_virtual_pointer_v1_button(sess->remotedesktop_data.virtual_pointer, get_time_ms(),
		button, btn_state);
	zwlr_virtual_pointer_v1_frame(sess->remotedesktop_data.virtual_pointer);
	return 0;
}

static int method_remotedesktop_notify_pointer_axis(sd_bus_message *msg,
		void *data, sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0, finish = 0;
	char *session_handle, *key;
	struct xdpw_session *sess;
	double dx = 0, dy = 0;

	logprint(TRACE, "remotedesktop: npa: method invoked");

	ret = sd_bus_message_read(msg, "o", &session_handle);
	if (ret < 0) {
		return ret;
	}
	logprint(TRACE, "remotedesktop: npa: session_handle: %s", session_handle);

	sess = get_session_from_handle(state, session_handle);
	if (!sess) {
		logprint(WARN, "remotedesktop: npa: session not found");
		return -1;
	}
	logprint(TRACE, "remotedesktop: npa: session found");

	if (!(sess->remotedesktop_data.devices & POINTER)) {
		logprint(DEBUG, "remotedesktop: npa: called, but pointer not selected!");
		return -1;
	}

	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		ret = sd_bus_message_read(msg, "s", &key);
		if (ret < 0) {
			return ret;
		}

		if (strcmp(key, "finish") == 0) {
			sd_bus_message_read(msg, "v", "b", &finish);
			logprint(DEBUG, "remotedesktop: npa: finish: %d", finish);
		} else {
			logprint(WARN, "remotedesktop: npa: unknown option: %s", key);
			sd_bus_message_skip(msg, "v");
		}

		ret = sd_bus_message_exit_container(msg);
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

	ret = sd_bus_message_read(msg, "d", &dx);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "d", &dy);
	if (ret < 0) {
		return ret;
	}

	struct zwlr_virtual_pointer_v1 *pointer = sess->remotedesktop_data.virtual_pointer;
	uint32_t t = get_time_ms();

	zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_CONTINUOUS);
	zwlr_virtual_pointer_v1_axis(pointer, t, WL_POINTER_AXIS_VERTICAL_SCROLL,
			wl_fixed_from_double(dy));
	zwlr_virtual_pointer_v1_axis(pointer, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
			wl_fixed_from_double(dx));

	if (finish) {
		zwlr_virtual_pointer_v1_axis_stop(pointer, t, WL_POINTER_AXIS_VERTICAL_SCROLL);
		zwlr_virtual_pointer_v1_axis_stop(pointer, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
	}

	zwlr_virtual_pointer_v1_frame(pointer);
	return 0;
}

static int method_remotedesktop_notify_pointer_axis_discrete(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *session_handle;
	struct xdpw_session *sess;
	uint32_t axis;
	int32_t steps;

	logprint(DEBUG, "remotedesktop: npad: method invoked");

	ret = sd_bus_message_read(msg, "o", &session_handle);
	if (ret < 0) {
		return ret;
	}
	logprint(DEBUG, "remotedesktop: npad: session_handle: %s", session_handle);

	sess = get_session_from_handle(state, session_handle);
	if (!sess) {
		logprint(WARN, "remotedesktop: npad: session not found");
		return -1;
	}
	logprint(DEBUG, "remotedesktop: npad: session found");

	if (!(sess->remotedesktop_data.devices & POINTER)) {
		logprint(DEBUG, "remotedesktop: npad: called, but pointer not selected!");
		return -1;
	}

	ret = sd_bus_message_skip(msg, "a{sv}");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "u", &axis);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "i", &steps);
	if (ret < 0) {
		return ret;
	}

	zwlr_virtual_pointer_v1_axis_discrete(sess->remotedesktop_data.virtual_pointer,
			get_time_ms(), axis, wl_fixed_from_double(0.1), steps);
	zwlr_virtual_pointer_v1_frame(sess->remotedesktop_data.virtual_pointer);
	return 0;
}

static int method_remotedesktop_notify_keyboard_keycode(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *session_handle;
	struct xdpw_session *sess;
	int32_t keycode;
	uint32_t keystate;

	logprint(DEBUG, "remotedesktop: npb: method invoked");

	ret = sd_bus_message_read(msg, "o", &session_handle);
	if (ret < 0) {
		return ret;
	}
	logprint(DEBUG, "remotedesktop: npb: session_handle: %s", session_handle);

	sess = get_session_from_handle(state, session_handle);
	if (!sess) {
		logprint(WARN, "remotedesktop: npb: session not found");
		return -1;
	}

	if (!(sess->remotedesktop_data.devices & KEYBOARD)) {
		logprint(DEBUG, "remotedesktop: npb: called, but keyboard not selected!");
		return -1;
	}

	ret = sd_bus_message_skip(msg, "a{sv}");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "i", &keycode);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "u", &keystate);
	if (ret < 0) {
		return ret;
	}
	logprint(DEBUG, "remotedesktop: received code %x, state %u", keycode, keystate);
	// The remotedesktop keycodes are evdev keycodes. They are converted to xkb keycodes by a
	// fixed offset of 8.
	keyboard_feed_code(&sess->remotedesktop_data.keyboard, keycode + 8, keystate == 1);
	return 0;
}

static int method_remotedesktop_notify_keyboard_keysym(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *session_handle;
	struct xdpw_session *sess;
	int32_t keysym;
	uint32_t keystate;

	logprint(DEBUG, "remotedesktop: npb: method invoked");

	ret = sd_bus_message_read(msg, "o", &session_handle);
	if (ret < 0) {
		return ret;
	}
	logprint(DEBUG, "remotedesktop: npb: session_handle: %s", session_handle);

	sess = get_session_from_handle(state, session_handle);
	if (!sess) {
		logprint(WARN, "remotedesktop: npb: session not found");
		return -1;
	}

	if (!(sess->remotedesktop_data.devices & KEYBOARD)) {
		logprint(DEBUG, "remotedesktop: npb: called, but keyboard not selected!");
		return -1;
	}

	ret = sd_bus_message_skip(msg, "a{sv}");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "i", &keysym);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "u", &keystate);
	if (ret < 0) {
		return ret;
	}
	logprint(DEBUG, "remotedesktop: received symbol %x, state %u", keysym, keystate);
	keyboard_feed(&sess->remotedesktop_data.keyboard, (xkb_keysym_t)keysym, keystate == 1);
	return 0;
}

static int method_remotedesktop_notify_touch_down(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {

	logprint(ERROR, "remotedesktop: notify_touch_down called, but not supported!");
	return -1;
}

static int method_remotedesktop_notify_touch_motion(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	logprint(ERROR, "remotedesktop: notify_touch_motion called, but not supported!");
	return -1;
}

static int method_remotedesktop_notify_touch_up(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	logprint(ERROR, "remotedesktop: notify_touch_up called, but not supported!");
	return -1;
}

static const sd_bus_vtable remotedesktop_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}",
		method_remotedesktop_create_session, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SelectDevices", "oosa{sv}", "ua{sv}",
		method_remotedesktop_select_devices, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Start", "oossa{sv}", "ua{sv}",
		method_remotedesktop_start, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyPointerMotion", "oa{sv}dd", NULL,
		method_remotedesktop_notify_pointer_motion, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyPointerMotionAbsolute", "oa{sv}udd", NULL,
		method_remotedesktop_notify_pointer_motion_absolute, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyPointerButton", "oa{sv}iu", NULL,
		method_remotedesktop_notify_pointer_button, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyPointerAxis", "oa{sv}dd", NULL,
		method_remotedesktop_notify_pointer_axis, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyPointerAxisDiscrete", "oa{sv}ui", NULL,
		method_remotedesktop_notify_pointer_axis_discrete, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyKeyboardKeycode", "oa{sv}iu", NULL,
		method_remotedesktop_notify_keyboard_keycode, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyKeyboardKeysym", "oa{sv}iu", NULL,
		method_remotedesktop_notify_keyboard_keysym, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyTouchDown", "oa{sv}uudd", NULL,
		method_remotedesktop_notify_touch_down, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyTouchMotion", "oa{sv}uudd", NULL,
		method_remotedesktop_notify_touch_motion, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("NotifyTouchUp", "oa{sv}u", NULL,
		method_remotedesktop_notify_touch_up, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("AvailableDeviceTypes", "u", NULL,
		offsetof(struct xdpw_state, remotedesktop_available_device_types),
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("version", "u", NULL,
		offsetof(struct xdpw_state, remotedesktop_version),
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

int xdpw_remotedesktop_init(struct xdpw_state *state) {
	sd_bus_slot *slot = NULL;

	state->remotedesktop = (struct xdpw_remotedesktop_context) { 0 };
	state->remotedesktop.state = state;

	int err;
	err = xdpw_virtual_input_init(state);
	if (err) {
		goto fail_virtual_input;
	}

	return sd_bus_add_object_vtable(state->bus, &slot, object_path,
		interface_name, remotedesktop_vtable, state);

fail_virtual_input:
	xdpw_virtual_input_finish(&state->remotedesktop);

	return err;
}

void xdpw_remotedesktop_destroy(struct xdpw_remotedesktop_session_data *data) {
	logprint(DEBUG, "remotedesktop: destroy called.");
	if (data->virtual_pointer) {
		zwlr_virtual_pointer_v1_destroy(data->virtual_pointer);
		data->virtual_pointer = NULL;
	}
	if (data->keyboard.virtual_keyboard) {
		zwp_virtual_keyboard_v1_destroy(data->keyboard.virtual_keyboard);
		data->keyboard.virtual_keyboard = NULL;
		keyboard_destroy(&data->keyboard);
	}
}
