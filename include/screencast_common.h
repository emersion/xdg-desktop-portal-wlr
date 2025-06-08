#ifndef SCREENCAST_COMMON_H
#define SCREENCAST_COMMON_H

#include <gbm.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <wayland-client-protocol.h>
#include <xf86drm.h>

#include "fps_limit.h"

// this seems to be right based on
// https://github.com/flatpak/xdg-desktop-portal/blob/309a1fc0cf2fb32cceb91dbc666d20cf0a3202c2/src/screen-cast.c#L955
#define XDP_CAST_PROTO_VER 4
#define XDP_CAST_DATA_VER 1

enum cursor_modes {
  HIDDEN = 1,
  EMBEDDED = 2,
  METADATA = 4,
};

enum source_types {
  MONITOR = 1,
  WINDOW = 2,
};

enum persist_modes {
  PERSIST_NONE = 0,
  PERSIST_TRANSIENT = 1,
  PERSIST_PERMANENT = 2,
};

enum buffer_type {
  WL_SHM = 0,
  DMABUF = 1,
};

enum xdpw_chooser_types {
  XDPW_CHOOSER_DEFAULT,
  XDPW_CHOOSER_NONE,
  XDPW_CHOOSER_SIMPLE,
  XDPW_CHOOSER_DMENU,
  XDPW_CHOOSER_JSON,
};

struct xdpw_output_chooser {
	enum xdpw_chooser_types type;
	char *cmd;
};

struct xdpw_chooser_opts {
	struct wl_list *output_list;

	// XDPW_CHOOSER_JSON
	uint32_t target_mask;
	enum persist_modes persist_mode;

	// XDPW_CHOOSER_NONE
	char *outputname;
};

struct xdpw_frame_damage {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

struct xdpw_frame {
	bool completed;
	bool y_invert;
	uint64_t tv_sec;
	uint32_t tv_nsec;
	uint32_t transformation;
	struct xdpw_buffer *xdpw_buffer;
	struct pw_buffer *pw_buffer;
};

struct xdpw_buffer {
	struct wl_list link;
	enum buffer_type buffer_type;

	uint32_t width;
	uint32_t height;
	uint32_t format;
	int plane_count;
	uint64_t modifier;

	int fd[GBM_MAX_PLANES];
	uint32_t size[GBM_MAX_PLANES];
	uint32_t stride[GBM_MAX_PLANES];
	uint32_t offset[GBM_MAX_PLANES];

	struct wl_array damage;

	struct gbm_bo *bo;

	struct wl_buffer *buffer;
};


struct xdpw_dmabuf_feedback_data {
	void *format_table_data;
	uint32_t format_table_size;
	bool device_used;
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
	struct ext_output_image_capture_source_manager_v1 *ext_output_image_capture_source_manager;
	struct ext_image_copy_capture_manager_v1 *ext_image_copy_capture_manager;
	struct wl_shm *shm;
	struct zwp_linux_dmabuf_v1 *linux_dmabuf;
	struct zwp_linux_dmabuf_feedback_v1 *linux_dmabuf_feedback;
	struct zxdg_output_manager_v1 *xdg_output_manager;
	struct xdpw_dmabuf_feedback_data feedback_data;
	struct wl_array format_modifier_pairs;

	// gbm
	struct gbm_device *gbm;

	// sessions
	struct wl_list screencast_instances;
};

struct xdpw_screencast_target {
	union {
		struct {
			struct xdpw_wlr_output *output;
			bool with_cursor;
		};
	};
};

struct xdpw_screencast_restore_data {
	uint32_t version;
	const char *output_name;
};

struct xdpw_format_modifier_pair {
	uint32_t fourcc;
	uint64_t modifier;
};

struct xdpw_shm_format {
	uint32_t fourcc;
	uint32_t stride;
};

struct xdpw_buffer_constraints {
	struct wl_array dmabuf_format_modifier_pairs;
	struct wl_array shm_formats;
	uint32_t width, height;
	bool dirty;
	struct gbm_device *gbm;
};

struct xdpw_screencast_ext_session {
	struct ext_image_copy_capture_session_v1 *capture_session;
	struct ext_image_copy_capture_frame_v1 *frame;
};

struct xdpw_screencast_wlr_session {
	struct zwlr_screencopy_frame_v1 *frame_callback;
	struct zwlr_screencopy_frame_v1 *wlr_frame;
};

struct xdpw_screencast_instance {
	// list
	struct wl_list link;

	// xdpw
	uint32_t refcount;
	struct xdpw_screencast_context *ctx;
	bool initialized;
	struct xdpw_frame current_frame;
	struct wl_list buffer_list;
	bool avoid_dmabufs;

	// pipewire
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_video_info_raw pwr_format;
	uint32_t seq;
	uint32_t node_id;
	bool pwr_stream_state;
	uint32_t framerate;

	// wlroots
	union {
		struct xdpw_screencast_wlr_session wlr_session;
		struct xdpw_screencast_ext_session ext_session;
	};

	struct xdpw_buffer_constraints current_constraints;
	struct xdpw_buffer_constraints pending_constraints;

	struct xdpw_screencast_target *target;
	uint32_t max_framerate;
	enum buffer_type buffer_type;

	// fps limit
	struct fps_limit_state fps_limit;
};

struct xdpw_screencast_session_data {
	struct xdpw_screencast_instance *screencast_instance;
	uint32_t cursor_mode;
	uint32_t persist_mode;
};

struct xdpw_wlr_output {
	struct wl_list link;
	uint32_t id;
	struct wl_output *output;
	struct zxdg_output_v1 *xdg_output;
	char *make;
	char *model;
	char *name;
	int x;
	int y;
	int width;
	int height;
	float framerate;
	enum wl_output_transform transformation;
};

void randname(char *buf);
struct gbm_device *xdpw_gbm_device_create(drmDevice *device);
struct xdpw_buffer *xdpw_buffer_create(struct xdpw_screencast_instance *cast,
	enum buffer_type buffer_type);
void xdpw_buffer_destroy(struct xdpw_buffer *buffer);

void xdpw_buffer_constraints_init(struct xdpw_buffer_constraints *constraints);
void xdpw_buffer_constraints_finish(struct xdpw_buffer_constraints *constraints);
bool xdpw_buffer_constraints_move(struct xdpw_buffer_constraints *dst, struct xdpw_buffer_constraints *src);
uint32_t xdpw_count_dmabuf_modifiers(struct xdpw_screencast_instance *cast, uint32_t drm_format);
void xdpw_query_dmabuf_modifiers(struct xdpw_screencast_instance *cast, uint32_t drm_format,
		uint64_t *modifiers, uint32_t num_modifiers);

enum wl_shm_format xdpw_format_wl_shm_from_drm_fourcc(uint32_t format);
uint32_t xdpw_format_drm_fourcc_from_wl_shm(enum wl_shm_format format);
uint32_t xdpw_format_drm_fourcc_from_pw_format(enum spa_video_format format);
enum spa_video_format xdpw_format_pw_from_drm_fourcc(uint32_t format);
enum spa_video_format xdpw_format_pw_strip_alpha(enum spa_video_format format);

int xdpw_bpp_from_drm_fourcc(uint32_t format);

enum xdpw_chooser_types get_chooser_type(const char *chooser_type);
const char *chooser_type_str(enum xdpw_chooser_types chooser_type);

struct xdpw_frame_damage merge_damage(struct xdpw_frame_damage *damage1, struct xdpw_frame_damage *damage2);
#endif /* SCREENCAST_COMMON_H */
