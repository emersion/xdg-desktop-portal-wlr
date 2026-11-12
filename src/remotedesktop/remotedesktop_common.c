#include <time.h>

#include "remotedesktop_common.h"

uint32_t get_time_ms() {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec * 1e-6;
}

