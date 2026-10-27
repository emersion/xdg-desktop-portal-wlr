#include "child.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "logger.h"
#include "xdpw.h"

static void child_destroy(struct xdpw_child *child) {
	wl_list_remove(&child->link);
	if (child->out_fd >= 0) {
		close(child->out_fd);
	}
	free(child->out);
	free(child);
}

struct xdpw_child *xdpw_child_spawn(struct xdpw_state *state, const char *cmd,
		const char *input, xdpw_child_done_func_t done, void *data) {
	struct xdpw_child *child = calloc(1, sizeof(*child));
	if (child == NULL) {
		logprint(ERROR, "child: allocation failed");
		return NULL;
	}

	// Feed the input through a temporary file, so that writing it can't block
	FILE *in = NULL;
	if (input != NULL) {
		in = tmpfile();
		if (in == NULL || fputs(input, in) == EOF || fflush(in) != 0) {
			logprint(ERROR, "child: failed to write the input of %s: %s", cmd,
				strerror(errno));
			goto error_in;
		}
		rewind(in);
	}

	int out[2];
	if (pipe(out) == -1) {
		logprint(ERROR, "child: pipe failed: %s", strerror(errno));
		goto error_in;
	}

	logprint(DEBUG, "child: running %s", cmd);
	pid_t pid = fork();
	if (pid < 0) {
		logprint(ERROR, "child: fork failed: %s", strerror(errno));
		close(out[0]);
		close(out[1]);
		goto error_in;
	} else if (pid == 0) {
		// Own process group, so that cancelling also kills the commands
		// started by the shell (e.g. slurp in a command substitution)
		setpgid(0, 0);

		sigset_t mask;
		sigemptyset(&mask);
		sigprocmask(SIG_SETMASK, &mask, NULL);

		if (in != NULL) {
			dup2(fileno(in), STDIN_FILENO);
		}
		close(out[0]);
		dup2(out[1], STDOUT_FILENO);
		close(out[1]);

		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);
		perror("execl");
		_exit(127);
	}

	// Also set it here to avoid racing against xdpw_child_cancel()
	setpgid(pid, pid);

	if (in != NULL) {
		fclose(in);
	}
	close(out[1]);
	fcntl(out[0], F_SETFD, FD_CLOEXEC);
	fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);

	child->state = state;
	child->pid = pid;
	child->out_fd = out[0];
	child->done = done;
	child->data = data;
	wl_list_insert(&state->children, &child->link);
	return child;

error_in:
	if (in != NULL) {
		fclose(in);
	}
	free(child);
	return NULL;
}

void xdpw_child_cancel(struct xdpw_child *child) {
	logprint(DEBUG, "child: cancelling process group %d", (int)child->pid);
	if (kill(-child->pid, SIGTERM) < 0 && errno != ESRCH) {
		logprint(ERROR, "child: failed to kill process group %d: %s",
			(int)child->pid, strerror(errno));
	}
	child->done = NULL;
	child->data = NULL;
}

static void child_exited(struct xdpw_child *child) {
	int status = 0;
	pid_t ret;
	do {
		ret = waitpid(child->pid, &status, 0);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		logprint(ERROR, "child: waitpid failed: %s", strerror(errno));
		status = 127 << 8;
	}
	logprint(DEBUG, "child: process %d exited with status %d", (int)child->pid,
		WIFEXITED(status) ? WEXITSTATUS(status) : -1);

	if (child->done != NULL) {
		child->done(child, status, child->out ? child->out : "",
			child->out_len, child->data);
	}
	child_destroy(child);
}

void xdpw_child_handle_fd(struct xdpw_state *state, int fd) {
	struct xdpw_child *child = NULL, *iter;
	wl_list_for_each(iter, &state->children, link) {
		if (iter->out_fd == fd) {
			child = iter;
			break;
		}
	}
	if (child == NULL) {
		return;
	}

	while (true) {
		if (child->out_cap - child->out_len < 1024) {
			size_t cap = child->out_cap ? child->out_cap * 2 : 4096;
			char *out = realloc(child->out, cap);
			if (out == NULL) {
				logprint(ERROR, "child: allocation failed");
				return;
			}
			child->out = out;
			child->out_cap = cap;
		}

		ssize_t n = read(fd, child->out + child->out_len,
			child->out_cap - child->out_len - 1);
		if (n > 0) {
			child->out_len += n;
			child->out[child->out_len] = '\0';
		} else if (n == 0) {
			// The shell has exited (or closed its stdout)
			child_exited(child);
			return;
		} else if (errno == EINTR) {
			continue;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return;
		} else {
			logprint(ERROR, "child: read failed: %s", strerror(errno));
			child_exited(child);
			return;
		}
	}
}

void xdpw_child_finish_all(struct xdpw_state *state) {
	struct xdpw_child *child, *tmp;
	wl_list_for_each_safe(child, tmp, &state->children, link) {
		xdpw_child_cancel(child);
		child_destroy(child);
	}
}
