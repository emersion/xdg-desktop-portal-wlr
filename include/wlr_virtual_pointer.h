#ifndef WLR_VIRTUAL_POINTER_H
#define WLR_VIRTUAL_POINTER_H

#define VIRTUAL_POINTER_VERSION 2
#define VIRTUAL_POINTER_VERSION_MIN 1

#include "remotedesktop_common.h"

struct xdpw_state;

int xdpw_wlr_virtual_pointer_init(struct xdpw_state *state);

void xdpw_wlr_virtual_pointer_finish(struct xdpw_remotedesktop_context *ctx);

#endif
