#include "virtual_input.h"

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include "xdpw.h"
#include "logger.h"

static void wlr_registry_handle_add(void *data, struct wl_registry *reg,
		uint32_t id, const char *interface, uint32_t ver) {
	struct xdpw_remotedesktop_context *ctx = data;

	logprint(DEBUG, "wlroots: interface to register %s  (Version: %u)",
		interface, ver);

	if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name)) {
		uint32_t version = ver;
		if (VIRTUAL_POINTER_VERSION < ver) {
			version = VIRTUAL_POINTER_VERSION;
		} else if (ver < VIRTUAL_POINTER_VERSION_MIN) {
			version = VIRTUAL_POINTER_VERSION_MIN;
		}
		logprint(DEBUG,
			"wlroots: |-- registered to interface %s (Version %u)",
			interface, version);
		ctx->virtual_pointer_manager = wl_registry_bind(reg, id,
			&zwlr_virtual_pointer_manager_v1_interface, version);
	} else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name)) {
		uint32_t version = ver;
		if (VIRTUAL_KEYBOARD_VERSION < ver) {
			version = VIRTUAL_KEYBOARD_VERSION;
		} else if (ver < VIRTUAL_KEYBOARD_VERSION_MIN) {
			version = VIRTUAL_KEYBOARD_VERSION_MIN;
		}
		logprint(DEBUG,
			"wlroots: |-- registered to interface %s (Version %u)",
			interface, version);
		ctx->virtual_keyboard_manager = wl_registry_bind(reg, id,
			&zwp_virtual_keyboard_manager_v1_interface, version);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		ctx->seat = wl_registry_bind(reg, id, &wl_seat_interface, 7);
	}
}


static void wlr_registry_handle_remove(void *data, struct wl_registry *reg,
		uint32_t id) {
	// TODO: handle seat removal?
}

static const struct wl_registry_listener wlr_registry_listener = {
	.global = wlr_registry_handle_add,
	.global_remove = wlr_registry_handle_remove,
};

int xdpw_virtual_input_init(struct xdpw_state *state) {
	struct xdpw_remotedesktop_context *ctx = &state->remotedesktop;

	// retrieve registry
	ctx->registry = wl_display_get_registry(state->wl_display);
	wl_registry_add_listener(ctx->registry, &wlr_registry_listener, ctx);

	wl_display_roundtrip(state->wl_display);

	logprint(DEBUG, "wayland: registry listeners run");
	wl_display_roundtrip(state->wl_display);

	// make sure our wlroots supports virtual-pointer protocol
	if (!ctx->virtual_pointer_manager) {
		logprint(ERROR, "Compositor doesn't support %s!",
			zwlr_virtual_pointer_manager_v1_interface.name);
		return -1;
	}

	// make sure our wlroots supports virtual-keyboard protocol
	if (!ctx->virtual_keyboard_manager) {
		logprint(ERROR, "Compositor doesn't support %s!",
			zwp_virtual_keyboard_manager_v1_interface.name);
		return -1;
	}

	return 0;
}

void xdpw_virtual_input_finish(struct xdpw_remotedesktop_context *ctx) {
	if (ctx->virtual_pointer_manager) {
		zwlr_virtual_pointer_manager_v1_destroy(ctx->virtual_pointer_manager);
	}
}
