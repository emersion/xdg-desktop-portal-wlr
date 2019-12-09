#ifndef XDPW_H
#define XDPW_H

#include <wayland-client.h>
#include <systemd/sd-bus.h>

struct xdpw_request {
	sd_bus_slot *slot;
};

struct xdpw_session {
	sd_bus_slot *slot;
};

enum {
	PORTAL_RESPONSE_SUCCESS = 0,
	PORTAL_RESPONSE_CANCELLED = 1,
	PORTAL_RESPONSE_ENDED = 2
};

int init_screenshot(sd_bus *bus);

struct xdpw_request *request_create(sd_bus *bus, const char *object_path);
void request_destroy(struct xdpw_request *req);

struct xdpw_session *session_create(sd_bus *bus, const char *object_path);
void session_destroy(struct xdpw_session *req);

#endif
