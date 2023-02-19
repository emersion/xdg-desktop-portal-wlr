#ifndef WLR_SCREENCAST_H
#define WLR_SCREENCAST_H

#include "screencast_common.h"

#define WL_OUTPUT_VERSION 4

#define SC_MANAGER_VERSION 3
#define SC_MANAGER_VERSION_MIN 2

#define WL_SHM_VERSION 1

#define LINUX_DMABUF_VERSION 4
#define LINUX_DMABUF_VERSION_MIN 3

struct xdpw_state;

int xdpw_wlr_screencopy_init(struct xdpw_state *state);
void xdpw_wlr_screencopy_finish(struct xdpw_screencast_context *ctx);

bool xdpw_wlr_target_chooser(struct xdpw_screencast_context *ctx, struct xdpw_screencast_target *target);

void xdpw_wlr_frame_finish(struct xdpw_screencast_instance *cast);
void xdpw_wlr_frame_start(struct xdpw_screencast_instance *cast);
void xdpw_wlr_register_cb(struct xdpw_screencast_instance *cast);

#endif
