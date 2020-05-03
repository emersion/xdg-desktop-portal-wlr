#ifndef SCREENCAST_COMMON_H
#define SCREENCAST_COMMON_H

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <wayland-client-protocol.h>

// this seems to be right based on
// https://github.com/flatpak/xdg-desktop-portal/blob/309a1fc0cf2fb32cceb91dbc666d20cf0a3202c2/src/screen-cast.c#L955
#define XDP_CAST_PROTO_VER 2

enum cursor_modes {
  HIDDEN = 1,
  EMBEDDED = 2,
  METADATA = 4,
};

enum source_types {
  MONITOR = 1,
  WINDOW = 2,
};

struct xdpw_frame_damage {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

struct xdpw_frame {
	uint32_t width;
	uint32_t height;
	uint32_t size;
	uint32_t stride;
	bool y_invert;
	uint64_t tv_sec;
	uint32_t tv_nsec;
	enum wl_shm_format format;
	struct xdpw_frame_damage damage;
	struct wl_buffer *buffer;
	void *data;
};

struct xdpw_screencast_context {

	// xdpw
	struct xdpw_state *state;

	// pipewire
	struct pw_context *pwr_context;
	struct pw_core *core;

	// wlroots
	struct wl_list output_list;
	struct wl_registry *registry;
	struct zwlr_screencopy_manager_v1 *screencopy_manager;
	struct zxdg_output_manager_v1* xdg_output_manager;
	struct wl_shm *shm;

	// cli options
	const char *output_name;

	// sessions
	struct wl_list screencast_instances;
};

struct xdpw_screencast_instance {
	// list
	struct wl_list link;

	// xdpw
	uint32_t refcount;
	struct xdpw_screencast_context *ctx;
	bool initialized;

	// pipewire
	struct spa_source *event;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_video_info_raw pwr_format;
	uint32_t seq;
	uint32_t node_id;
	bool pwr_stream_state;

	// wlroots
	struct zwlr_screencopy_frame_v1 *frame_callback;
	struct xdpw_wlr_output *target_output;
	uint32_t framerate;
	struct zwlr_screencopy_frame_v1 *wlr_frame;
	struct xdpw_frame simple_frame;
	bool with_cursor;
	int err;
	bool quit;
};

struct xdpw_wlr_output {
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

void randname(char *buf);
enum spa_video_format xdpw_format_pw_from_wl_shm(
	struct xdpw_screencast_instance *cast);
enum spa_video_format xdpw_format_pw_strip_alpha(enum spa_video_format format);

#endif /* SCREENCAST_COMMON_H */
