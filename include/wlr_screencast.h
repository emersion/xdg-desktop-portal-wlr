#ifndef WLR_SCREENCAST_H
#define WLR_SCREENCAST_H

#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "pipewire_screencast.h"
#include "screencast_common.h"

#define SC_MANAGER_VERSION 2

struct xdpw_state;

void wlr_frame_free(struct xdpw_state *state);
int wlr_screencopy_init(struct xdpw_state *state);
void wlr_screencopy_uninit(struct screencast_context *ctx);

struct wayland_output *wlr_output_find_by_name(struct wl_list *output_list,
	const char *name);
struct wayland_output *wlr_output_find(struct screencast_context *ctx,
	struct wl_output *out, uint32_t id);
struct wayland_output *wlr_output_first(struct wl_list *output_list);

void wlr_register_cb(struct xdpw_state *state);

#endif
