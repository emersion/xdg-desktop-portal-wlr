#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xdpw.h"

static const char interface_name[] = "org.freedesktop.impl.portal.Session";

static int method_close(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	// struct xdpw_session *session = data;
	// TODO
	printf("Session.Close\n");
	return 0;
}

static const sd_bus_vtable session_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", method_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END
};

struct xdpw_session *session_create(sd_bus *bus, const char *object_path) {
	struct xdpw_session *req = calloc(1, sizeof(struct xdpw_session));

	if (sd_bus_add_object_vtable(bus, &req->slot, object_path, interface_name,
			session_vtable, NULL) < 0) {
		free(req);
		fprintf(stderr, "sd_bus_add_object_vtable failed: %s\n",
			strerror(-errno));
		return NULL;
	}

	return req;
}

void session_destroy(struct xdpw_session *req) {
	if (req == NULL) {
		return;
	}
	sd_bus_slot_unref(req->slot);
	free(req);
}
