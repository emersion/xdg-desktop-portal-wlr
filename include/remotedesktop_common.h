#ifndef REMOTEDESKTOP_COMMON_H
#define REMOTEDESKTOP_COMMON_H

#include <stdbool.h>

#include <wayland-client-protocol.h>

#define XDP_REMOTE_PROTO_VER 1

struct xdpw_remotedesktop_context {
	// xdpw
	struct xdpw_state *state;

	// sessions
	struct wl_list remotedesktop_instances;
};

struct xdpw_remotedesktop_session_data {
	uint32_t devices;
};

enum device_types {
	KEYBOARD = 1,
	POINTER = 2,
	TOUCHSCREEN = 4,
};

#endif
