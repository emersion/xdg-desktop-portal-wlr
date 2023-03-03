#include "remotedesktop.h"

#include <time.h>

#include "wlr_virtual_pointer.h"
#include "xdpw.h"

static const char object_path[] = "/org/freedesktop/portal/desktop";
static const char interface_name[] = "org.freedesktop.impl.portal.RemoteDesktop";

static uint32_t get_timestamp_ms(struct xdpw_remotedesktop_session_data *remote) {
	struct timespec *t_start, t_stop;

	t_start = &remote->t_start;
	clock_gettime(CLOCK_REALTIME, &t_stop);

	return 1000 * (t_stop.tv_sec - t_start->tv_sec) +
		(t_stop.tv_nsec - t_start->tv_nsec) / 1000000;
}

static int method_remotedesktop_create_session(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *request_handle, *session_handle, *app_id, *key;
	struct xdpw_request *req;
	struct xdpw_session *sess;

	logprint(DEBUG, "remotedesktop: dbus: create session: method invoked");

	ret = sd_bus_message_read(msg, "oos", &request_handle, &session_handle, &app_id);
	if (ret < 0) {
		return ret;
	}

	logprint(DEBUG, "remotedesktop: dbus: create session: request_handle: %s", request_handle);
	logprint(DEBUG, "remotedesktop: dbus: create session: session_handle: %s", session_handle);
	logprint(DEBUG, "remotedesktop: dbus: create session: app_id: %s", app_id);

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
			logprint(DEBUG, "remotedesktop: dbus: create session: session handle token: %s", token);
		} else {
			logprint(WARN, "remotedesktop: dbus: create session: unknown option: %s", key);
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

	req = xdpw_request_create(
		sd_bus_message_get_bus(msg), request_handle);
	if (req == NULL) {
		return -ENOMEM;
	}

	sess = xdpw_session_create(state,
		sd_bus_message_get_bus(msg), strdup(session_handle));
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

	logprint(DEBUG, "remotedesktop: dbus: select devices: method invoked");

	ret = sd_bus_message_read(msg, "oos", &request_handle, &session_handle, &app_id);
	if (ret < 0) {
		return ret;
	}

	logprint(DEBUG, "remotedesktop: dbus: select devices: request_handle: %s", request_handle);
	logprint(DEBUG, "remotedesktop: dbus: select devices: session_handle: %s", session_handle);
	logprint(DEBUG, "remotedesktop: dbus: select devices: app_id: %s", app_id);

	wl_list_for_each_reverse(sess, &state->xdpw_sessions, link) {
		if (strcmp(sess->session_handle, session_handle) == 0) {
			break;
		}
	}
	if (!sess) {
		logprint(WARN, "remotedesktop: dbus: select devices: session not found");
		return -1;
	}
	logprint(DEBUG, "remotedesktop: dbus: select devices: session found");

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
			logprint(DEBUG, "remotedesktop: dbus: select devices: option types: %x", types);
		} else {
			logprint(WARN, "remotedesktop: dbus: select devices: unknown option: %s", key);
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

	logprint(DEBUG, "remotedesktop: dbus: start: method invoked");

	ret = sd_bus_message_read(msg, "oos", &request_handle, &session_handle, &app_id);
	if (ret < 0) {
		return ret;
	}

	logprint(DEBUG, "remotedesktop: dbus: start: request_handle: %s", request_handle);
	logprint(DEBUG, "remotedesktop: dbus: start: session_handle: %s", session_handle);
	logprint(DEBUG, "remotedesktop: dbus: start: app_id: %s", app_id);

	wl_list_for_each_reverse(sess, &state->xdpw_sessions, link) {
		if (strcmp(sess->session_handle, session_handle) == 0) {
			break;
		}
	}
	if (!sess) {
		logprint(WARN, "remotedesktop: dbus: start: session not found");
		return -1;
	}
	logprint(DEBUG, "remotedesktop: dbus: start: session found");

	remote = &sess->remotedesktop_data;
	remote->virtual_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
		state->remotedesktop.virtual_pointer_manager, NULL);
	clock_gettime(CLOCK_REALTIME, &remote->t_start);

	ret = sd_bus_message_read(msg, "s", &parent_window);
	if (ret < 0) {
		return ret;
	}
	logprint(DEBUG, "remotedesktop: dbus: start: parent window: %s", parent_window);

	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		ret = sd_bus_message_read(msg, "s", &key);
		if (ret < 0) {
			return ret;
		}

		logprint(WARN, "remotedesktop: dbus: start: unknown option: %s", key);
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

	ret = sd_bus_reply_method_return(msg, "ua{sv}", PORTAL_RESPONSE_SUCCESS,
		1, "devices", "u", POINTER | KEYBOARD);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int method_remotedesktop_notify_pointer_motion(sd_bus_message *msg,
		void *data, sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *session_handle;
	struct xdpw_session *sess;
	double dx = 0, dy = 0;

	logprint(DEBUG, "remotedesktop: dbus: npm: method invoked");

	ret = sd_bus_message_read(msg, "o", &session_handle);
	if (ret < 0) {
		return ret;
	}
	logprint(DEBUG, "remotedesktop: dbus: npm: session_handle: %s", session_handle);

	wl_list_for_each_reverse(sess, &state->xdpw_sessions, link) {
		if (strcmp(sess->session_handle, session_handle) == 0) {
			break;
		}
	}
	if (!sess) {
		logprint(WARN, "remotedesktop: dbus: npm: session not found");
		return -1;
	}
	logprint(DEBUG, "remotedesktop: dbus: npm: session found");

	ret = sd_bus_message_skip(msg, "a{sv}");
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

	zwlr_virtual_pointer_v1_motion(sess->remotedesktop_data.virtual_pointer,
		get_timestamp_ms(&sess->remotedesktop_data),
		wl_fixed_from_double(dx), wl_fixed_from_double(dy));

	return 0;
}

static int method_remotedesktop_notify_pointer_motion_absolute(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	struct xdpw_state *state = data;

	int ret = 0;
	char *session_handle;
	struct xdpw_session *sess;
	double x = 0, y = 0;

	logprint(DEBUG, "remotedesktop: dbus: npma: method invoked");

	ret = sd_bus_message_read(msg, "o", &session_handle);
	if (ret < 0) {
		return ret;
	}
	logprint(DEBUG, "remotedesktop: dbus: npma: session_handle: %s", session_handle);

	wl_list_for_each_reverse(sess, &state->xdpw_sessions, link) {
		if (strcmp(sess->session_handle, session_handle) == 0) {
			break;
		}
	}
	if (!sess) {
		logprint(WARN, "remotedesktop: dbus: npma: session not found");
		return -1;
	}
	logprint(DEBUG, "remotedesktop: dbus: npma: session found");

	ret = sd_bus_message_skip(msg, "a{sv}");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "d", &x);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_read(msg, "d", &y);
	if (ret < 0) {
		return ret;
	}

	// zwlr_virtual_pointer_v1_motion_absolute(sess->remotedesktop_data.virtual_pointer,
	// 	get_timestamp_ms(&sess->remotedesktop_data),
	// 	wl_fixed_from_double(x), wl_fixed_from_double(y));

	return 0;
}

static int method_remotedesktop_notify_pointer_button(sd_bus_message *msg,
		void *data, sd_bus_error *ret_error) {
	return 0;
}

static int method_remotedesktop_notify_pointer_axis(sd_bus_message *msg,
		void *data, sd_bus_error *ret_error) {
	return 0;
}

static int method_remotedesktop_notify_pointer_axis_discrete(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	return 0;
}

static int method_remotedesktop_notify_keyboard_keycode(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	return 0;
}

static int method_remotedesktop_notify_keyboard_keysym(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	return 0;
}

static int method_remotedesktop_notify_touch_down(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	return 0;
}

static int method_remotedesktop_notify_touch_motion(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	return 0;
}

static int method_remotedesktop_notify_touch_up(
		sd_bus_message *msg, void *data, sd_bus_error *ret_error) {
	return 0;
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
	err = xdpw_wlr_virtual_pointer_init(state);
	if (err) {
		goto fail_virtual_pointer;
	}

	return sd_bus_add_object_vtable(state->bus, &slot, object_path,
		interface_name, remotedesktop_vtable, state);

fail_virtual_pointer:
	xdpw_wlr_virtual_pointer_finish(&state->remotedesktop);

	return err;
}
