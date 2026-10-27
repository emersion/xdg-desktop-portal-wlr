#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "child.h"
#include "logger.h"
#include "xdpw.h"
#include "screenshot.h"

static const char object_path[] = "/org/freedesktop/portal/desktop";
static const char interface_name[] = "org.freedesktop.impl.portal.Screenshot";

struct screenshot_job {
	struct xdpw_request *req;
	sd_bus_message *msg; // pending method call
	struct xdpw_child *child;
};

static void screenshot_job_destroy(struct screenshot_job *job) {
	sd_bus_message_unref(job->msg);
	free(job);
}

static void screenshot_job_finish(struct screenshot_job *job) {
	xdpw_request_destroy(job->req);
	screenshot_job_destroy(job);
}

static void screenshot_job_close(struct xdpw_request *req) {
	struct screenshot_job *job = req->data;
	logprint(DEBUG, "screenshot: request closed, cancelling");

	xdpw_child_cancel(job->child);

	int ret = sd_bus_reply_method_return(job->msg, "ua{sv}", PORTAL_RESPONSE_ENDED, 0);
	if (ret < 0) {
		logprint(ERROR, "dbus: failed to reply: %s", strerror(-ret));
	}

	req->close = NULL;
	req->data = NULL;
	// The request itself is destroyed by the Close handler
	screenshot_job_destroy(job);
}

static struct screenshot_job *screenshot_job_start(sd_bus_message *msg,
		const char *handle, struct xdpw_state *state, const char *cmd,
		xdpw_child_done_func_t done) {
	struct screenshot_job *job = calloc(1, sizeof(*job));
	if (job == NULL) {
		return NULL;
	}

	job->req = xdpw_request_create(sd_bus_message_get_bus(msg), handle);
	if (job->req == NULL) {
		free(job);
		return NULL;
	}

	job->child = xdpw_child_spawn(state, cmd, NULL, done, job);
	if (job->child == NULL) {
		xdpw_request_destroy(job->req);
		free(job);
		return NULL;
	}

	job->msg = sd_bus_message_ref(msg);
	job->req->close = screenshot_job_close;
	job->req->data = job;
	return job;
}

static bool child_succeeded(int status) {
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// TODO: choose a better path
static const char screenshot_path[] = "/tmp/out.png";
static const char screenshot_uri[] = "file:///tmp/out.png";

static void screenshot_done(struct xdpw_child *child, int status,
		const char *out, size_t out_len, void *data) {
	struct screenshot_job *job = data;
	int ret;

	if (child_succeeded(status)) {
		ret = sd_bus_reply_method_return(job->msg, "ua{sv}",
			PORTAL_RESPONSE_SUCCESS, 1, "uri", "s", screenshot_uri);
	} else {
		logprint(ERROR, "screenshot: grim failed");
		ret = sd_bus_reply_method_errorf(job->msg, SD_BUS_ERROR_FAILED,
			"Screenshot failed");
	}
	if (ret < 0) {
		logprint(ERROR, "dbus: failed to reply: %s", strerror(-ret));
	}

	screenshot_job_finish(job);
}

static int method_screenshot(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	int ret = 0;

	bool interactive = false;

	char *handle, *app_id, *parent_window;
	ret = sd_bus_message_read(msg, "oss", &handle, &app_id, &parent_window);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_enter_container(msg, 'a', "{sv}");
	if (ret < 0) {
		return ret;
	}
	char *key;
	int inner_ret = 0;
	while ((ret = sd_bus_message_enter_container(msg, 'e', "sv")) > 0) {
		inner_ret = sd_bus_message_read(msg, "s", &key);
		if (inner_ret < 0) {
			return inner_ret;
		}

		if (strcmp(key, "interactive") == 0) {
			int mode;
			sd_bus_message_read(msg, "v", "b", &mode);
			logprint(DEBUG, "dbus: option interactive: %d", mode);
			interactive = mode;
		} else if (strcmp(key, "modal") == 0) {
			int modal;
			sd_bus_message_read(msg, "v", "b", &modal);
			logprint(DEBUG, "dbus: option modal: %d", modal);
		} else {
			logprint(WARN, "dbus: unknown option %s", key);
			sd_bus_message_skip(msg, "v");
		}

		inner_ret = sd_bus_message_exit_container(msg);
		if (inner_ret < 0) {
			return inner_ret;
		}
	}
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_exit_container(msg);
	if (ret < 0) {
		return ret;
	}

	char cmd[128];
	if (interactive) {
		snprintf(cmd, sizeof(cmd), "grim -g \"$(slurp)\" -- %s", screenshot_path);
	} else {
		snprintf(cmd, sizeof(cmd), "grim -- %s", screenshot_path);
	}

	// The reply is sent once grim (and slurp) are done, or the request is
	// closed. Keep serving other requests meanwhile.
	if (screenshot_job_start(msg, handle, data, cmd, screenshot_done) == NULL) {
		return -ENOMEM;
	}

	return 1;
}

static bool parse_ppm_pixel(const char *out, size_t out_len,
		struct xdpw_ppm_pixel *pixel) {
	int width, height;
	int header_len = 0;
	// The header ends with a single whitespace character after maxval
	if (sscanf(out, "P6 %d %d %d%n", &width, &height,
			&pixel->max_color_value, &header_len) != 3 ||
			!isspace((unsigned char)out[header_len])) {
		return false;
	}
	header_len++;
	if (width != 1 || height != 1 || pixel->max_color_value <= 0 ||
			pixel->max_color_value > 255 || out_len < (size_t)header_len + 3) {
		return false;
	}

	const unsigned char *pixels = (const unsigned char *)out + header_len;
	pixel->red = pixels[0];
	pixel->green = pixels[1];
	pixel->blue = pixels[2];
	return true;
}

static void pick_color_done(struct xdpw_child *child, int status,
		const char *out, size_t out_len, void *data) {
	struct screenshot_job *job = data;
	struct xdpw_ppm_pixel pixel = {0};
	int ret;

	if (!child_succeeded(status)) {
		logprint(ERROR, "Selection failed");
		ret = sd_bus_reply_method_errorf(job->msg, SD_BUS_ERROR_FAILED,
			"Selection failed");
	} else if (!parse_ppm_pixel(out, out_len, &pixel)) {
		logprint(WARN, "Invalid image format or size");
		ret = sd_bus_reply_method_errorf(job->msg, SD_BUS_ERROR_FAILED,
			"Invalid image format or size");
	} else {
		double red = pixel.red / (pixel.max_color_value * 1.0);
		double green = pixel.green / (pixel.max_color_value * 1.0);
		double blue = pixel.blue / (pixel.max_color_value * 1.0);
		ret = sd_bus_reply_method_return(job->msg, "ua{sv}",
			PORTAL_RESPONSE_SUCCESS, 1, "color", "(ddd)", red, green, blue);
	}
	if (ret < 0) {
		logprint(ERROR, "dbus: failed to reply: %s", strerror(-ret));
	}

	screenshot_job_finish(job);
}

static int method_pick_color(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {

	int ret = 0;

	char *handle, *app_id, *parent_window;
	ret = sd_bus_message_read(msg, "oss", &handle, &app_id, &parent_window);
	if (ret < 0) {
		return ret;
	}

	const char cmd[] = "grim -s 1 -g \"$(slurp -p)\" -t ppm -";
	if (screenshot_job_start(msg, handle, data, cmd, pick_color_done) == NULL) {
		return -ENOMEM;
	}

	return 1;
}

static const sd_bus_vtable screenshot_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Screenshot", "ossa{sv}", "ua{sv}", method_screenshot, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("PickColor", "ossa{sv}", "ua{sv}", method_pick_color, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("version", "u", NULL,
		offsetof(struct xdpw_state, screenshot_version),
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

int xdpw_screenshot_init(struct xdpw_state *state) {
	// TODO: cleanup
	sd_bus_slot *slot = NULL;
	return sd_bus_add_object_vtable(state->bus, &slot, object_path, interface_name,
		screenshot_vtable, state);
}
