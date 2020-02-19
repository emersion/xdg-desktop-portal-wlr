#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include "xdpw.h"

static const char service_name[] = "org.freedesktop.impl.portal.desktop.wlr";

int xdpw_usage(FILE* stream, int rc)
{
	static const char* usage =
"Usage: xdg-desktop-portal-wlr [options]\n"
"\n"
"    -p,--pixelformat=BGRx|RGBx                Force a pixelformat in pipewire\n"
"                                              metadata (performs no conversion).\n"
"    -h,--help                                 Get help (this text).\n"
"\n";

	fprintf(stream, "%s", usage);

	return rc;
}

int main(int argc, char *argv[]) {

	const char* forced_pixelformat = NULL;

	static const char* shortopts = "p:h";
	static const struct option longopts[] = {
		{ "pixelformat", required_argument, NULL, 'p' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	while (1) {
		int c = getopt_long(argc, argv, shortopts, longopts, NULL);
		if (c < 0)
			break;

		switch (c) {
		case 'p':
			forced_pixelformat = optarg;
			break;
		case 'h':
			return xdpw_usage(stdout, 0);
		default:
			return xdpw_usage(stderr, 1);
		}
	}

	int ret = 0;

	sd_bus *bus = NULL;
	ret = sd_bus_open_user(&bus);
	if (ret < 0) {
		fprintf(stderr, "Failed to connect to user bus: %s\n", strerror(-ret));
		goto error;
	}

	init_screenshot(bus);
	init_screencast(bus, forced_pixelformat);

	ret = sd_bus_request_name(bus, service_name, 0);
	if (ret < 0) {
		fprintf(stderr, "Failed to acquire service name: %s\n", strerror(-ret));
		goto error;
	}

	while (1) {
		ret = sd_bus_process(bus, NULL);
		if (ret < 0) {
			fprintf(stderr, "sd_bus_process failed: %s\n", strerror(-ret));
		} else if (ret > 0) {
			// We processed a request, try to process another one, right-away
			continue;
		}

		ret = sd_bus_wait(bus, (uint64_t)-1);
		if (ret < 0) {
			fprintf(stderr, "sd_bus_wait failed: %s\n", strerror(-ret));
			goto error;
		}
	}

	// TODO: cleanup

	return EXIT_SUCCESS;

error:
	sd_bus_unref(bus);
	return EXIT_FAILURE;
}
