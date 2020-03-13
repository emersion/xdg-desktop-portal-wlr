#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum LOGLEVEL { QUIET, ERROR, WARN, INFO, DEBUG, TRACE };

struct logger_properties {
	enum LOGLEVEL level;
	FILE *dst;
};

void init_logger(FILE *dst, enum LOGLEVEL level);
enum LOGLEVEL get_loglevel(const char *level);
void logprint(enum LOGLEVEL level, char *msg, ...);

#endif
