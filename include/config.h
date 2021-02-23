#ifndef CONFIG_H
#define CONFIG_H

#include "logger.h"

struct config_screencast {
	char *output_name;
};

struct xdpw_config {
	struct config_screencast screencast_conf;
};

void print_config(enum LOGLEVEL loglevel, struct xdpw_config *config);
void finish_config(struct xdpw_config *config);
void init_config(const char *configfile, struct xdpw_config *config);

#endif
