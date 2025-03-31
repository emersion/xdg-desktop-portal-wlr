#ifndef EXT_IMAGE_COPY_H
#define EXT_IMAGE_COPY_H

#include "screencast_common.h"

void xdpw_ext_ic_frame_capture(struct xdpw_screencast_instance *cast);
int xdpw_ext_ic_session_init(struct xdpw_screencast_instance *cast);
void xdpw_ext_ic_session_close(struct xdpw_screencast_instance *cast);

#endif
