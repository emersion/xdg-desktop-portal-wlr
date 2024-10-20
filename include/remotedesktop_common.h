#ifndef REMOTEDESKTOP_COMMON_H
#define REMOTEDESKTOP_COMMON_H

#include <stdbool.h>
#include <time.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include <wayland-client-protocol.h>
#include <wayland-util.h>

#include "keyboard.h"

#define XDP_REMOTE_PROTO_VER 1

struct xdpw_remotedesktop_context {
	// xdpw
	struct xdpw_state *state;

	// wlroots
	struct wl_registry *registry;
	struct zwlr_virtual_pointer_manager_v1 *virtual_pointer_manager;
	struct zwp_virtual_keyboard_manager_v1 *virtual_keyboard_manager;
	struct wl_seat *seat;

	// sessions
	struct wl_list remotedesktop_instances;
};

struct xdpw_remotedesktop_session_data {
	struct zwlr_virtual_pointer_v1 *virtual_pointer;
	struct keyboard keyboard;
	struct timespec t_start;
};

enum device_types {
	KEYBOARD = 1,
	POINTER = 2,
	TOUCHSCREEN = 4,
};

#endif
