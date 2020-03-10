#ifndef PIPEWIRE_SCREENCAST_H
#define PIPEWIRE_SCREENCAST_H

#include <stdio.h>
#include <pipewire/pipewire.h>
#include <spa/param/format-utils.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
#include <spa/support/type-map.h>
#include "wlr_screencast.h"
#include "screencast_common.h"
#include "xdpw.h"

#define BUFFERS 1
#define ALIGN 16

void *pwr_start(struct xdpw_state *state);

#endif
