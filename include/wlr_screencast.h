#ifndef WLR_SCREENCAST_H
#define WLR_SCREENCAST_H

#include "screencast_common.h"

#define SC_MANAGER_VERSION 2

struct xdpw_state;

int xdpw_wlr_screencopy_init(struct xdpw_state *state);
void xdpw_wlr_screencopy_finish(struct xdpw_screencast_context *ctx);

struct xdpw_wlr_output *xdpw_wlr_output_find_by_name(struct wl_list *output_list,
	const char *name);
struct xdpw_wlr_output *xdpw_wlr_output_first(struct wl_list *output_list);
struct xdpw_wlr_output *xdpw_wlr_output_find(struct xdpw_screencast_context *ctx,
	struct wl_output *out, uint32_t id);

void xdpw_wlr_frame_free(struct xdpw_screencast_instance *cast);
void xdpw_wlr_register_cb(struct xdpw_screencast_instance *cast);

#endif
