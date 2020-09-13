#ifndef WLR_SCREENCAST_COMMON_H
#define WLR_SCREENCAST_COMMON_H

#include <screencast_common.h>

void wlr_destroy_shm_buffer(struct wl_buffer *buffer, void *data, int size);
struct wl_buffer *wlr_create_shm_buffer(struct xdpw_screencast_instance *cast,
		enum wl_shm_format fmt, int width, int height, int stride,
		void **data_out);

#endif /* !WLR_SCREENCAST_COMMON_H */
