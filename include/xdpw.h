#ifndef XDPW_H
#define XDPW_H

#include <wayland-client.h>
#ifdef HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#elif HAVE_ELOGIND
#include <elogind/sd-bus.h>
#endif
#include "logger.h"
#include "screencast.h"

struct xdpw_state {
	sd_bus *bus;
	struct wl_display *wl_display;
	struct pw_loop *pw_loop;

	struct screencast_context screencast;
};

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

int init_screenshot(struct xdpw_state *state);
int init_screencast(struct xdpw_state *state, const char *output_name,
	const char *forced_pixelformat);

struct xdpw_request *request_create(sd_bus *bus, const char *object_path);
void request_destroy(struct xdpw_request *req);

struct xdpw_session *session_create(sd_bus *bus, const char *object_path);
void session_destroy(struct xdpw_session *req);

#endif
