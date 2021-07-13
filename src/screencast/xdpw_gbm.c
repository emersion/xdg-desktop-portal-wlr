#include <gbm.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "xdpw.h"
#include "logger.h"

static char *gbm_find_render_node(size_t maxlen) {
	drmDevice *devices[64];
	char *render_node = NULL;

	int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
	for (int i = 0; i < n; ++i) {
		drmDevice *dev = devices[i];
		if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
			continue;

		render_node = strndup(dev->nodes[DRM_NODE_RENDER], maxlen);
		break;
	}

	drmFreeDevices(devices, n);
	return render_node;
}

struct gbm_device *create_gbm_device(){
	struct gbm_device *gbm;
	char *render_node = NULL;
	size_t render_node_size = 256;

	render_node = gbm_find_render_node(render_node_size);
	if (render_node == NULL) {
		logprint(ERROR, "xdpw: Could not find render node");
		return NULL;
	} else {
		logprint(INFO, "xdpw: Using render node %s",render_node);
	}

	int fd = open(render_node, O_RDWR);
	if (fd < 0) {
		logprint(ERROR, "xdpw: Could not open render node %s",render_node);
		close(fd);
		return NULL;
	}

	free(render_node);
	gbm = gbm_create_device(fd);
	close(fd);
	return gbm;
}

void destroy_gbm_device(struct gbm_device *gbm) {
	gbm_device_destroy(gbm);
}
