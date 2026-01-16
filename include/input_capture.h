#pragma once

#include "xdpw.h"

#include <systemd/sd-bus.h>
#include <libei-1.0/libeis.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"

#include <stdbool.h>

struct xdpw_input_capture_barrier {
  uint32_t id;
  int32_t x1, y1, x2, y2;
  struct xdpw_input_capture_barrier *next;
};

struct xdpw_input_capture_output {
  struct wl_list link;
  struct xdpw_input_capture_data *data;
  uint32_t name;
  struct wl_output *wl_output;
  int32_t x, y, width, height;
  bool ready;
};

struct xdpw_input_capture_data {
  struct xdpw_state *xdpw_state;

  uint32_t capabilities;
  uint32_t version;
  sd_bus *bus;
  struct eis *eis_context;

  struct wl_display *wl_display;
  struct wl_registry *wl_registry;
  struct wl_compositor *wl_compositor;
  struct zwlr_layer_shell_v1 *wl_layer_shell;

  struct wl_seat* wl_seat;
  struct zwp_pointer_constraints_v1 *wl_pointer_constraints;
  struct zwp_keyboard_shortcuts_inhibit_manager_v1 *wl_keyboard_shortcuts_manager;
  
  struct xkb_context *xkb_context;  // global XKB context

  struct xdpw_session *active_session;  // pointer to the currently capturing session

  struct wl_list output_list;
};

int xdpw_input_capture_init(struct xdpw_state *);
void xdpw_input_capture_destroy(void);
void xdpw_input_capture_dispatch_eis(struct xdpw_state *);
