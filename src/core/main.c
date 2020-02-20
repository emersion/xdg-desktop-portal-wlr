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
"    -l, --loglevel=<loglevel>        Select log level (default is ERROR).\n"
"                                     QUIET, ERROR, WARN, INFO, DEBUG, TRACE\n"
"    -o, --output=<name>              Select output to capture.\n"
"    -p,--pixelformat=BGRx|RGBx       Force a pixelformat in pipewire\n"
"                                     metadata (performs no conversion).\n"
"    -h,--help                        Get help (this text).\n"
"\n";

	fprintf(stream, "%s", usage);

	return rc;
}

int main(int argc, char *argv[]) {

	const char* output_name = NULL;
	const char* forced_pixelformat = NULL;
	enum LOGLEVEL loglevel = ERROR;

	static const char* shortopts = "l:o:p:h";
	static const struct option longopts[] = {
		{ "loglevel", required_argument, NULL, 'l' },
		{ "output", required_argument, NULL, 'o' },
		{ "pixelformat", required_argument, NULL, 'p' },
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
		case 'p':
			forced_pixelformat = optarg;
			break;
		case 'h':
			return xdpw_usage(stdout, 0);
		default:
			return xdpw_usage(stderr, 1);
		}
	}

	init_logger(stderr, loglevel);

	int ret = 0;

	sd_bus *bus = NULL;
	ret = sd_bus_open_user(&bus);
	if (ret < 0) {
		logprint(ERROR, "dbus: failed to connect to user bus: %s", strerror(-ret));
		goto error;
	}

	init_screenshot(bus);
	init_screencast(bus, output_name, forced_pixelformat);

	ret = sd_bus_request_name(bus, service_name, 0);
	if (ret < 0) {
		logprint(ERROR, "dbus: failed to acquire service name: %s", strerror(-ret));
		goto error;
	}

	while (1) {
		ret = sd_bus_process(bus, NULL);
		if (ret < 0) {
			logprint(ERROR, "dbus: sd_bus_process failed: %s", strerror(-ret));
			goto error;
		} else if (ret > 0) {
			// We processed a request, try to process another one, right-away
			continue;
		}

		ret = sd_bus_wait(bus, (uint64_t)-1);
		if (ret < 0) {
			logprint(ERROR, "dbus: sd_bus_wait failed: %s", strerror(-ret));
			goto error;
		}
	}

	// TODO: cleanup

	return EXIT_SUCCESS;

error:
	sd_bus_unref(bus);
	return EXIT_FAILURE;
}
