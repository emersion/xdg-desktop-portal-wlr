#ifndef WLR_SCREENCAST_H
#define WLR_SCREENCAST_H

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include <fcntl.h>
#include <limits.h>
#include <png.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "screencast_common.h"

#define SC_MANAGER_VERSION 2

void wlr_frame_free(struct screencast_context *ctx);
int wlr_screencopy_init(struct screencast_context *ctx);
void wlr_screencopy_uninit(struct screencast_context *ctx);
struct wayland_output *wlr_find_output(struct screencast_context *ctx,
																			 struct wl_output *out, uint32_t id);
void wlr_register_cb(struct screencast_context *ctx);

#endif
