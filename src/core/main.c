#include <stdlib.h>
#include <stdio.h>
#include "xdpw.h"

static const char service_name[] = "org.freedesktop.impl.portal.desktop.wlr";

int main(int argc, char *argv[]) {
	int ret = 0;

	sd_bus *bus = NULL;
	ret = sd_bus_open_user(&bus);
	if (ret < 0) {
		fprintf(stderr, "Failed to connect to user bus: %s\n", strerror(-ret));
		goto error;
	}

	init_screenshot(bus);
	init_screencast(bus);

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
