#ifndef WAYLAND_DISPATCH_H
#define WAYLAND_DISPATCH_H

struct wl_display;

// Read available data after poll reports readability, without waiting for a
// live event. The portal dispatches Wayland from a single thread.
int xdpw_wl_dispatch(struct wl_display *display);

#endif
