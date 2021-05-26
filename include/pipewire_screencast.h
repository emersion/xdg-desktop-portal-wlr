#ifndef PIPEWIRE_SCREENCAST_H
#define PIPEWIRE_SCREENCAST_H

#include "screencast_common.h"

#define XDPW_PWR_BUFFERS 1
#define XDPW_PWR_ALIGN 16

void pwr_update_stream_param(struct xdpw_screencast_instance *cast);
void xdpw_pwr_stream_create(struct xdpw_screencast_instance *cast);
void xdpw_pwr_stream_destroy(struct xdpw_screencast_instance *cast);
int xdpw_pwr_context_create(struct xdpw_state *state);
void xdpw_pwr_context_destroy(struct xdpw_state *state);

#endif
