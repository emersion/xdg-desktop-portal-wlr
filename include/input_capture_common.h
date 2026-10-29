#ifndef INPUT_CAPTURE_COMMON_H
#define INPUT_CAPTURE_COMMON_H

#include "input_capture.h"

struct xdpw_input_capture_session_data {
  
  uint32_t capabilities;
  bool enabled;
  struct eis_device *device;
  
  uint32_t activation_id;
  
  uint32_t zone_set_id;
  struct xdpw_input_capture_barrier *barriers; // Linked list of active barriers
  
  struct wl_surface *wl_surface;
  struct zwlr_layer_surface_v1 *wl_layer_surface; 
  struct wl_pointer *wl_pointer;
  struct wl_keyboard *wl_keyboard;
  struct zwp_locked_pointer_v1 *wl_locked_pointer;
  struct zwp_keyboard_shortcuts_inhibitor_v1 *wl_keyboard_inhibitor;
  
  // pointer position for delta calculation
  wl_fixed_t last_pointer_x;
  wl_fixed_t last_pointer_y;
  
  // xkb state
  struct xkb_keymap *xkb_keymap;
  struct xkb_state *xkb_state;
};


void xdpw_input_capture_session_data_free(struct xdpw_input_capture_session_data *);

#endif