#ifndef XDPW_H
#define XDPW_H

#include <wayland-client.h>
#ifdef HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#elif HAVE_ELOGIND
#include <elogind/sd-bus.h>
#endif

#include "screencast_common.h"

struct xdpw_state {
	struct wl_list xdpw_sessions;
	sd_bus *bus;
	struct wl_display *wl_display;
	struct pw_loop *pw_loop;
	struct xdpw_screencast_context screencast;
	uint32_t screencast_source_types; // bitfield of enum source_types
	uint32_t screencast_cursor_modes; // bitfield of enum cursor_modes
	uint32_t screencast_version;
};

struct xdpw_request {
	sd_bus_slot *slot;
};

struct xdpw_session {
	struct wl_list link;
	sd_bus_slot *slot;
	char *session_handle;
	struct xdpw_screencast_instance *screencast_instance;
};

enum {
	PORTAL_RESPONSE_SUCCESS = 0,
	PORTAL_RESPONSE_CANCELLED = 1,
	PORTAL_RESPONSE_ENDED = 2
};

int xdpw_screenshot_init(struct xdpw_state *state);
int xdpw_screencast_init(struct xdpw_state *state, const char *output_name);

struct xdpw_request *xdpw_request_create(sd_bus *bus, const char *object_path);
void xdpw_request_destroy(struct xdpw_request *req);

struct xdpw_session *xdpw_session_create(struct xdpw_state *state, sd_bus *bus, char *object_path);
void xdpw_session_destroy(struct xdpw_session *req);

#endif
