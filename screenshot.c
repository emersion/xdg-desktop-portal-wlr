#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "xdpw.h"

static const char object_path[] = "/org/freedesktop/portal/desktop";
static const char interface_name[] = "org.freedesktop.impl.portal.Screenshot";

static int method_screenshot(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	int ret = 0;

	char *handle, *app_id, *parent_window;
	ret = sd_bus_message_read(msg, "oss", &handle, &app_id, &parent_window);
	if (ret < 0) {
		return ret;
	}
	// TODO: read options

	// TODO: cleanup this
	struct xdpw_request *req =
		request_create(sd_bus_message_get_bus(msg), handle);
	if (req == NULL) {
		return -ENOMEM;
	}

	sd_bus_message *reply = NULL;
	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_append(reply, "u", 0);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_open_container(reply, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_open_container(reply, 'e', "sv");
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_append_basic(reply, 's', "uri");
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_open_container(reply, 'v', "s");
	if (ret < 0) {
		return ret;
	}

	// TODO: take an actual screenshot
	ret = sd_bus_message_append_basic(reply, 's', "file:///tmp/out.png");
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

static const sd_bus_vtable screenshot_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Screenshot", "ossa{sv}", "ua{sv}", method_screenshot, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

int init_screenshot(sd_bus *bus) {
	// TODO: cleanup
	sd_bus_slot *slot = NULL;
	return sd_bus_add_object_vtable(bus, &slot, object_path, interface_name,
		screenshot_vtable, NULL);
}
