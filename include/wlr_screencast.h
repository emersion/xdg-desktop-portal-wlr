#ifndef WLR_SCREENCAST_H
#define WLR_SCREENCAST_H

#include "screencast_common.h"

#define WL_OUTPUT_VERSION 4

#define WL_SHM_VERSION 1

#define LINUX_DMABUF_VERSION 4
#define LINUX_DMABUF_VERSION_MIN 3

#define XDG_OUTPUT_VERSION 3
#define XDG_OUTPUT_VERSION_MIN 1

struct xdpw_state;

int xdpw_wlr_screencopy_init(struct xdpw_state *state);
void xdpw_wlr_screencopy_finish(struct xdpw_screencast_context *ctx);

struct xdpw_wlr_output *xdpw_wlr_output_find_by_name(struct wl_list *output_list, const char *name);

struct xdpw_chooser_job;

/** Called with whether the user picked a target. */
typedef void (*xdpw_chooser_done_func_t)(bool selected, void *data);

/**
 * Run the configured chooser (unless it is "none") without waiting for it;
 * done is called once it has exited. Returns NULL if no chooser could be run.
 */
struct xdpw_chooser_job *xdpw_wlr_target_chooser_start(struct xdpw_screencast_context *ctx,
	struct xdpw_screencast_target *target, uint32_t type_mask,
	xdpw_chooser_done_func_t done, void *data);
/** Close the chooser; done won't be called. */
void xdpw_wlr_target_chooser_cancel(struct xdpw_chooser_job *job);
bool xdpw_wlr_target_chooser_none(struct xdpw_screencast_context *ctx,
	struct xdpw_screencast_target *target);
bool xdpw_wlr_target_from_data(struct xdpw_screencast_context *ctx, struct xdpw_screencast_target *target,
		struct xdpw_screencast_restore_data *data);

void xdpw_wlr_frame_capture(struct xdpw_screencast_instance *cast);
int xdpw_wlr_session_init(struct xdpw_screencast_instance *cast);
void xdpw_wlr_session_close(struct xdpw_screencast_instance *cast);

#endif
