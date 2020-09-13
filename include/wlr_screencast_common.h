#ifndef WLR_SCREENCAST_COMMON_H
#define WLR_SCREENCAST_COMMON_H

#include <screencast_common.h>

#include <gbm.h>

void wlr_destroy_shm_buffer(struct wl_buffer *buffer, void *data, int size);
struct wl_buffer *wlr_create_shm_buffer(struct xdpw_screencast_instance *cast,
		enum wl_shm_format fmt, int width, int height, int stride,
		void **data_out);

struct wl_buffer *wlr_create_dmabuf_buffer(struct xdpw_screencast_instance *cast,
		int width, int height, uint32_t fourcc, struct gbm_bo **bo_out);
void wlr_destroy_dmabuf_buffer(struct wl_buffer *buffer, struct gbm_bo *bo);

struct gbm_device *create_gbm_device();
void destroy_gbm_device(struct gbm_device *gbm);

#endif /* !WLR_SCREENCAST_COMMON_H */
