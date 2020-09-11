#ifndef WLR_SCREENCOPY_H
#define WLR_SCREENCOPY_H

#include "screencast_common.h"

#define SC_MANAGER_VERSION 3

struct xdpw_state;

int xdpw_wlr_screencopy_init(struct xdpw_state *state);
void xdpw_wlr_screencopy_finish(struct xdpw_screencast_context *ctx);

void wlr_registry_handle_add_screencopy(void *data, struct wl_registry *reg,
		uint32_t id, const char *interface, uint32_t ver);

void xdpw_wlr_screencopy_frame_free(struct xdpw_screencast_instance *cast);
void xdpw_wlr_screencopy_register_cb(struct xdpw_screencast_instance *cast);

#endif

