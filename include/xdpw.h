#ifndef XDPW_H
#define XDPW_H

#include <wayland-client.h>
#include <systemd/sd-bus.h>

int init_screenshot(sd_bus *bus);

#endif
