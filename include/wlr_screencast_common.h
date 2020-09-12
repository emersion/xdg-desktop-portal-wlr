#ifndef WLR_SCREENCAST_COMMON_H
#define WLR_SCREENCAST_COMMON_H

#include <screencast_common.h>

struct wl_buffer *wlr_create_shm_buffer(struct xdpw_screencast_instance *cast,
		enum wl_shm_format fmt, int width, int height, int stride,
		void **data_out);

#endif /* !WLR_SCREENCAST_COMMON_H */
