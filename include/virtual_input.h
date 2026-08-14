#ifndef VIRTUAL_INPUT_H
#define VIRTUAL_INPUT_H

#define VIRTUAL_POINTER_VERSION 2
#define VIRTUAL_POINTER_VERSION_MIN 1

#define VIRTUAL_KEYBOARD_VERSION 1
#define VIRTUAL_KEYBOARD_VERSION_MIN 1

#include "remotedesktop_common.h"

struct xdpw_state;

int xdpw_virtual_input_init(struct xdpw_state *state);

void xdpw_virtual_input_finish(struct xdpw_remotedesktop_context *ctx);

#endif
