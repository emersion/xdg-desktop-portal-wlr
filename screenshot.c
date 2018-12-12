#include <stdlib.h>
#include <stdio.h>
#include "xdpw.h"

static const char object_path[] = "/org/freedesktop/portal/desktop";
static const char interface_name[] = "org.freedesktop.impl.portal.Screenshot";

static int method_screenshot(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	// TODO
	printf("Screenshot\n");
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
