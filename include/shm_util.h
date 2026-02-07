#ifndef SHM_H
#define SHM_H

#include <unistd.h>

void randname(char *buf);
int anonymous_shm_open(void);

#endif
