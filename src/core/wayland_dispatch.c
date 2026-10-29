#include <wayland-client.h>
#include "wayland_dispatch.h"

int xdpw_wl_dispatch(struct wl_display *display) {
	while (wl_display_prepare_read(display) != 0) {
		if (wl_display_dispatch_pending(display) < 0) {
			return -1;
		}
	}
	if (wl_display_read_events(display) < 0) {
		return -1;
	}
	return wl_display_dispatch_pending(display);
}
