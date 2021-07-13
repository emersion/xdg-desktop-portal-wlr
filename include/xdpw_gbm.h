#ifndef XDPW_GBM_H
#define XDPW_GBM_H

#include <gbm.h>

struct gbm_device *create_gbm_device();
void destroy_gbm_device(struct gbm_device *gbm);

#endif
