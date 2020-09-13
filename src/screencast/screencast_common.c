#include "screencast_common.h"
#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void randname(char *buf) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		assert(buf[i] == 'X');
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

int anonymous_shm_open(void) {
	char name[] = "/xdpw-shm-XXXXXX";
	int retries = 100;

	do {
		randname(name + strlen(name) - 6);

		--retries;
		// shm_open guarantees that O_CLOEXEC is set
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
		if (fd >= 0) {
			shm_unlink(name);
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);

	return -1;
}

enum spa_video_format xdpw_format_pw_from_wl_shm(
		enum wl_shm_format format) {
	switch (format) {
	case WL_SHM_FORMAT_ARGB8888:
		return SPA_VIDEO_FORMAT_BGRA;
	case WL_SHM_FORMAT_XRGB8888:
		return SPA_VIDEO_FORMAT_BGRx;
	case WL_SHM_FORMAT_RGBA8888:
		return SPA_VIDEO_FORMAT_ABGR;
	case WL_SHM_FORMAT_RGBX8888:
		return SPA_VIDEO_FORMAT_xBGR;
	case WL_SHM_FORMAT_ABGR8888:
		return SPA_VIDEO_FORMAT_RGBA;
	case WL_SHM_FORMAT_XBGR8888:
		return SPA_VIDEO_FORMAT_RGBx;
	case WL_SHM_FORMAT_BGRA8888:
		return SPA_VIDEO_FORMAT_ARGB;
	case WL_SHM_FORMAT_BGRX8888:
		return SPA_VIDEO_FORMAT_xRGB;
	case WL_SHM_FORMAT_NV12:
		return SPA_VIDEO_FORMAT_NV12;
	default:
		abort();
	}
}

enum spa_video_format xdpw_format_pw(
		struct xdpw_screencast_instance *cast) {
	switch (cast->type) {
	case XDPW_INSTANCE_SCP_SHM:
		return xdpw_format_pw_from_wl_shm(cast->xdpw_frames.screencopy_frame.format);
	default:
		abort();
	}
}

enum spa_video_format xdpw_format_pw_strip_alpha(enum spa_video_format format) {
	switch (format) {
	case SPA_VIDEO_FORMAT_BGRA:
		return SPA_VIDEO_FORMAT_BGRx;
	case SPA_VIDEO_FORMAT_ABGR:
		return SPA_VIDEO_FORMAT_xBGR;
	case SPA_VIDEO_FORMAT_RGBA:
		return SPA_VIDEO_FORMAT_RGBx;
	case SPA_VIDEO_FORMAT_ARGB:
		return SPA_VIDEO_FORMAT_xRGB;
	default:
		return SPA_VIDEO_FORMAT_UNKNOWN;
	}
}
