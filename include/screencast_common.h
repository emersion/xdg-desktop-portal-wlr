#ifndef SCREENCAST_COMMON_H
#define SCREENCAST_COMMON_H

#include <string.h>
#include <sys/types.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <libdrm/drm_fourcc.h>
#include <wayland-client-protocol.h>
#include "logger.h"

struct damage {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

struct simple_frame {
	uint32_t width;
	uint32_t height;
	uint32_t size;
	uint32_t stride;
	bool y_invert;
	uint64_t tv_sec;
	uint32_t tv_nsec;
	enum wl_shm_format format;
	struct damage *damage;
	struct wl_buffer *buffer;
	void *data;
};

struct screencast_context {
	// pipewire
	struct pw_context *pwr_context;
	struct pw_core *core;
	struct spa_source *event;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_video_info_raw pwr_format;
	uint32_t seq;
	uint32_t node_id;
	bool stream_state;

	// wlroots
	struct wl_display *display;
	struct wl_list output_list;
	struct wl_registry *registry;
	struct zwlr_screencopy_manager_v1 *screencopy_manager;
	struct zxdg_output_manager_v1* xdg_output_manager;
	struct wl_shm *shm;

	// main frame callback
	struct zwlr_screencopy_frame_v1 *frame_callback;

	// target output
	struct wayland_output *target_output;
	uint32_t framerate;
	bool with_cursor;

	// frame
	struct zwlr_screencopy_frame_v1 *wlr_frame;
	struct simple_frame simple_frame;

	// cli options
	const char *output_name;
	const char *forced_pixelformat;

	// if something happens during capture
	int err;
	bool quit;
};

struct wayland_output {
	struct wl_list link;
	uint32_t id;
	struct wl_output *output;
	struct zxdg_output_v1 *xdg_output;
	char *make;
	char *model;
	char *name;
	int width;
	int height;
	float framerate;
};

uint32_t pipewire_from_wl_shm(void *data);

#endif /* SCREENCAST_COMMON_H */
