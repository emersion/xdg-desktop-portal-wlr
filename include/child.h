#ifndef CHILD_H
#define CHILD_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <wayland-util.h>

struct xdpw_state;
struct xdpw_child;

/**
 * Called once the child has exited. status is the waitpid() status, out
 * holds everything the child wrote to its standard output (NUL-terminated,
 * out_len bytes). The child is destroyed after the callback returns.
 */
typedef void (*xdpw_child_done_func_t)(struct xdpw_child *child, int status,
	const char *out, size_t out_len, void *data);

struct xdpw_child {
	struct xdpw_state *state;
	pid_t pid;
	int out_fd;
	char *out;
	size_t out_len, out_cap;
	xdpw_child_done_func_t done;
	void *data;
	struct wl_list link; // xdpw_state::children
};

/**
 * Run cmd with /bin/sh -c in its own process group, without waiting for it.
 * If input is not NULL, it is fed to the child's standard input.
 */
struct xdpw_child *xdpw_child_spawn(struct xdpw_state *state, const char *cmd,
	const char *input, xdpw_child_done_func_t done, void *data);

/**
 * Terminate the child's whole process group. The done callback won't be
 * called; the child is reaped in the background.
 */
void xdpw_child_cancel(struct xdpw_child *child);

/** Read pending output of the child owning fd, reap it on EOF. */
void xdpw_child_handle_fd(struct xdpw_state *state, int fd);

void xdpw_child_finish_all(struct xdpw_state *state);

#endif
