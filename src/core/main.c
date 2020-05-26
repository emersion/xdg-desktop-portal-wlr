#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <poll.h>
#include <pipewire/pipewire.h>
#include <spa/utils/result.h>
#include "xdpw.h"
#include "logger.h"

enum event_loop_fd {
	EVENT_LOOP_DBUS,
	EVENT_LOOP_WAYLAND,
	EVENT_LOOP_PIPEWIRE,
};

static const char service_name[] = "org.freedesktop.impl.portal.desktop.wlr";

static int xdpw_usage(FILE* stream, int rc) {
	static const char* usage =
		"Usage: xdg-desktop-portal-wlr [options]\n"
		"\n"
		"    -l, --loglevel=<loglevel>        Select log level (default is ERROR).\n"
		"                                     QUIET, ERROR, WARN, INFO, DEBUG, TRACE\n"
		"    -o, --output=<name>              Select output to capture.\n"
		"                                     metadata (performs no conversion).\n"
		"    -h, --help                       Get help (this text).\n"
		"\n";

	fprintf(stream, "%s", usage);
	return rc;
}

int main(int argc, char *argv[]) {
	const char* output_name = NULL;
	enum LOGLEVEL loglevel = ERROR;

	static const char* shortopts = "l:o:h";
	static const struct option longopts[] = {
		{ "loglevel", required_argument, NULL, 'l' },
		{ "output", required_argument, NULL, 'o' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	while (1) {
		int c = getopt_long(argc, argv, shortopts, longopts, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'l':
			loglevel = get_loglevel(optarg);
			break;
		case 'o':
			output_name = optarg;
			break;
		case 'h':
			return xdpw_usage(stdout, EXIT_SUCCESS);
		default:
			return xdpw_usage(stderr, EXIT_FAILURE);
		}
	}

	init_logger(stderr, loglevel);

	int ret = 0;

	sd_bus *bus = NULL;
	ret = sd_bus_open_user(&bus);
	if (ret < 0) {
		logprint(ERROR, "dbus: failed to connect to user bus: %s", strerror(-ret));
		return EXIT_FAILURE;
	}
	logprint(DEBUG, "dbus: connected");

	struct wl_display *wl_display = wl_display_connect(NULL);
	if (!wl_display) {
		logprint(ERROR, "wayland: failed to connect to display");
		sd_bus_unref(bus);
		return EXIT_FAILURE;
	}
	logprint(DEBUG, "wlroots: wl_display connected");

	pw_init(NULL, NULL);
	struct pw_loop *pw_loop = pw_loop_new(NULL);
	if (!pw_loop) {
		logprint(ERROR, "pipewire: failed to create loop");
		wl_display_disconnect(wl_display);
		sd_bus_unref(bus);
		return EXIT_FAILURE;
	}
	logprint(DEBUG, "pipewire: pw_loop created");

	struct xdpw_state state = {
		.bus = bus,
		.wl_display = wl_display,
		.pw_loop = pw_loop,
		.screencast_source_types = MONITOR,
		.screencast_cursor_modes = HIDDEN | EMBEDDED,
		.screencast_version = XDP_CAST_PROTO_VER,
	};

	wl_list_init(&state.xdpw_sessions);

	xdpw_screenshot_init(&state);
	ret = xdpw_screencast_init(&state, output_name);
	if (ret < 0) {
		logprint(ERROR, "xdpw: failed to initialize screencast");
		goto error;
	}

	ret = sd_bus_request_name(bus, service_name, 0);
	if (ret < 0) {
		logprint(ERROR, "dbus: failed to acquire service name: %s", strerror(-ret));
		goto error;
	}

	struct pollfd pollfds[] = {
		[EVENT_LOOP_DBUS] = {
			.fd = sd_bus_get_fd(state.bus),
			.events = POLLIN,
		},
		[EVENT_LOOP_WAYLAND] = {
			.fd = wl_display_get_fd(state.wl_display),
			.events = POLLIN,
		},
		[EVENT_LOOP_PIPEWIRE] = {
			.fd = pw_loop_get_fd(state.pw_loop),
			.events = POLLIN,
		},
	};

	while (1) {
		ret = poll(pollfds, sizeof(pollfds) / sizeof(pollfds[0]), -1);
		if (ret < 0) {
			logprint(ERROR, "poll failed: %s", strerror(errno));
			goto error;
		}

		if (pollfds[EVENT_LOOP_DBUS].revents & POLLIN) {
			logprint(TRACE, "event-loop: got dbus event");
			do {
				ret = sd_bus_process(state.bus, NULL);
			} while (ret > 0);
			if (ret < 0) {
				logprint(ERROR, "sd_bus_process failed: %s", strerror(-ret));
				goto error;
			}
		}

		if (pollfds[EVENT_LOOP_WAYLAND].revents & POLLIN) {
			logprint(TRACE, "event-loop: got wayland event");
			ret = wl_display_dispatch(state.wl_display);
			if (ret < 0) {
				logprint(ERROR, "wl_display_dispatch failed: %s", strerror(errno));
				goto error;
			}
		}

		if (pollfds[EVENT_LOOP_PIPEWIRE].revents & POLLIN) {
			logprint(TRACE, "event-loop: got pipewire event");
			ret = pw_loop_iterate(state.pw_loop, 0);
			if (ret < 0) {
				logprint(ERROR, "pw_loop_iterate failed: %s", spa_strerror(ret));
				goto error;
			}
		}

		do {
			ret = wl_display_dispatch_pending(state.wl_display);
			wl_display_flush(state.wl_display);
		} while (ret > 0);

		sd_bus_flush(state.bus);
	}

	// TODO: cleanup

	return EXIT_SUCCESS;

error:
	sd_bus_unref(bus);
	pw_loop_leave(state.pw_loop);
	pw_loop_destroy(state.pw_loop);
	wl_display_disconnect(state.wl_display);
	return EXIT_FAILURE;
}
