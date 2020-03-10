#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "xdpw.h"

static const char object_path[] = "/org/freedesktop/portal/desktop";
static const char interface_name[] = "org.freedesktop.impl.portal.Screenshot";

static bool exec_screenshooter(const char *path) {
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		return false;
	} else if (pid == 0) {
		char *const argv[] = {
			"grim",
			"--",
			(char *)path,
			NULL,
		};
		execvp("grim", argv);

		perror("execvp");
		exit(127);
	}

	int stat;
	if (waitpid(pid, &stat, 0) < 0) {
		perror("waitpid");
		return false;
	}

	return stat == 0;
}

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

	// TODO: choose a better path
	const char path[] = "/tmp/out.png";
	if (!exec_screenshooter(path)) {
		return -1;
	}

	const char uri_prefix[] = "file://";
	char uri[strlen(path) + strlen(uri_prefix) + 1];
	snprintf(uri, sizeof(uri), "%s%s", uri_prefix, path);

	sd_bus_message *reply = NULL;
	ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_append(reply, "ua{sv}", PORTAL_RESPONSE_SUCCESS, 1, "uri", "s", uri);
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

int init_screenshot(struct xdpw_state *state) {
	// TODO: cleanup
	sd_bus_slot *slot = NULL;
	return sd_bus_add_object_vtable(state->bus, &slot, object_path, interface_name,
		screenshot_vtable, NULL);
}
