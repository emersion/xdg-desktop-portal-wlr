#include <sys/wait.h>
#include <unistd.h>

#include "logger.h"
#include "screencast.h"
#include "wlr_screencast.h"
#include "xdpw.h"

static struct xdpw_wlr_output *xdpw_wlr_output_first(struct wl_list *output_list) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, output_list, link) {
		return output;
	}
	return NULL;
}

static pid_t spawn_chooser(const char *cmd, FILE **chooser_in_ptr, FILE **chooser_out_ptr) {
	int chooser_in[2]; // p -> c
	int chooser_out[2]; // c -> p

	if (pipe(chooser_in) == -1) {
		perror("pipe chooser_in");
		logprint(ERROR, "Failed to open pipe chooser_in");
		return -1;
	}
	if (pipe(chooser_out) == -1) {
		perror("pipe chooser_out");
		logprint(ERROR, "Failed to open pipe chooser_out");
		goto error_chooser_in;
	}

	logprint(TRACE,
		"exec chooser called: cmd %s, pipe chooser_in (%d,%d), pipe chooser_out (%d,%d)",
		cmd, chooser_in[0], chooser_in[1], chooser_out[0], chooser_out[1]);

	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		goto error_chooser_out;
	} else if (pid == 0) {
		close(chooser_in[1]);
		close(chooser_out[0]);

		dup2(chooser_in[0], STDIN_FILENO);
		dup2(chooser_out[1], STDOUT_FILENO);
		close(chooser_in[0]);
		close(chooser_out[1]);

		execl("/bin/sh", "/bin/sh", "-c", cmd, NULL);

		perror("execl");
		_exit(127);
	}

	close(chooser_in[0]);
	close(chooser_out[1]);

	FILE *chooser_in_f = fdopen(chooser_in[1], "w");
	if (chooser_in_f == NULL) {
		close(chooser_in[1]);
		close(chooser_out[0]);
		return -1;
	}
	FILE *chooser_out_f = fdopen(chooser_out[0], "r");
	if (chooser_out_f == NULL) {
		fclose(chooser_in_f);
		close(chooser_out[0]);
		return -1;
	}

	*chooser_in_ptr = chooser_in_f;
	*chooser_out_ptr = chooser_out_f;

	return pid;

error_chooser_out:
	close(chooser_out[0]);
	close(chooser_out[1]);
error_chooser_in:
	close(chooser_in[0]);
	close(chooser_in[1]);
	return -1;
}

static bool wait_chooser(pid_t pid) {
	int status;
	if (waitpid(pid ,&status, 0) != -1 && WIFEXITED(status)) {
		return WEXITSTATUS(status) != 127;
	}
	return false;
}

static char *read_chooser_out(FILE *f) {
	char *name = NULL;
	size_t name_size = 0;
	ssize_t nread = getline(&name, &name_size, f);
	if (nread < 0) {
		perror("getline failed");
		return NULL;
	}

	// Strip newline
	char *p = strchr(name, '\n');
	if (p != NULL) {
		*p = '\0';
	}

	return name;
}

static bool wlr_output_chooser(const struct xdpw_output_chooser *chooser,
		struct wl_list *output_list, struct xdpw_wlr_output **output) {
	logprint(DEBUG, "wlroots: output chooser called");
	struct xdpw_wlr_output *out;
	*output = NULL;

	FILE *chooser_in = NULL, *chooser_out = NULL;
	pid_t pid = spawn_chooser(chooser->cmd, &chooser_in, &chooser_out);
	if (pid < 0) {
		logprint(ERROR, "Failed to fork chooser");
		return false;
	}

	switch (chooser->type) {
	case XDPW_CHOOSER_DMENU:;
		wl_list_for_each(out, output_list, link) {
			fprintf(chooser_in, "%s\n", out->name);
		}
		fclose(chooser_in);
		break;
	default:
		fclose(chooser_in);
	}

	if (!wait_chooser(pid)) {
		fclose(chooser_out);
		return false;
	}

	char *name = read_chooser_out(chooser_out);
	fclose(chooser_out);
	if (name == NULL) {
		goto end;
	}

	logprint(TRACE, "wlroots: output chooser %s selects output %s", chooser->cmd, name);
	wl_list_for_each(out, output_list, link) {
		if (strcmp(out->name, name) == 0) {
			*output = out;
			break;
		}
	}
	free(name);

end:
	return true;
}

static struct xdpw_wlr_output *wlr_output_chooser_default(struct wl_list *output_list) {
	logprint(DEBUG, "wlroots: output chooser called");
	const struct xdpw_output_chooser default_chooser[] = {
		{XDPW_CHOOSER_SIMPLE, "slurp -f %o -or"},
		{XDPW_CHOOSER_DMENU, "wmenu -p 'Select the monitor to share:'"},
		{XDPW_CHOOSER_DMENU, "wofi -d -n --prompt='Select the monitor to share:'"},
		{XDPW_CHOOSER_DMENU, "rofi -dmenu -p 'Select the monitor to share:'"},
		{XDPW_CHOOSER_DMENU, "bemenu --prompt='Select the monitor to share:'"},
	};

	size_t N = sizeof(default_chooser)/sizeof(default_chooser[0]);
	struct xdpw_wlr_output *output = NULL;
	bool ret;
	for (size_t i = 0; i<N; i++) {
		ret = wlr_output_chooser(&default_chooser[i], output_list, &output);
		if (!ret) {
			logprint(DEBUG, "wlroots: output chooser %s not found. Trying next one.",
					default_chooser[i].cmd);
			continue;
		}
		if (output != NULL) {
			logprint(DEBUG, "wlroots: output chooser selects %s", output->name);
		} else {
			logprint(DEBUG, "wlroots: output chooser canceled");
		}
		return output;
	}
	return xdpw_wlr_output_first(output_list);
}

static struct xdpw_wlr_output *xdpw_wlr_output_chooser(struct xdpw_screencast_context *ctx) {
	switch (ctx->state->config->screencast_conf.chooser_type) {
	case XDPW_CHOOSER_DEFAULT:
		return wlr_output_chooser_default(&ctx->output_list);
	case XDPW_CHOOSER_NONE:
		if (ctx->state->config->screencast_conf.output_name) {
			return xdpw_wlr_output_find_by_name(&ctx->output_list, ctx->state->config->screencast_conf.output_name);
		} else {
			return xdpw_wlr_output_first(&ctx->output_list);
		}
	case XDPW_CHOOSER_DMENU:
	case XDPW_CHOOSER_SIMPLE:;
		struct xdpw_wlr_output *output = NULL;
		if (!ctx->state->config->screencast_conf.chooser_cmd) {
			logprint(ERROR, "wlroots: no output chooser given");
			goto end;
		}
		struct xdpw_output_chooser chooser = {
			ctx->state->config->screencast_conf.chooser_type,
			ctx->state->config->screencast_conf.chooser_cmd
		};
		logprint(DEBUG, "wlroots: output chooser %s (%d)", chooser.cmd, chooser.type);
		bool ret = wlr_output_chooser(&chooser, &ctx->output_list, &output);
		if (!ret) {
			logprint(ERROR, "wlroots: output chooser %s failed", chooser.cmd);
			goto end;
		}
		if (output) {
			logprint(DEBUG, "wlroots: output chooser selects %s", output->name);
		} else {
			logprint(DEBUG, "wlroots: output chooser canceled");
		}
		return output;
	}
end:
	return NULL;
}

bool xdpw_wlr_target_chooser(struct xdpw_screencast_context *ctx, struct xdpw_screencast_target *target) {
	target->output = xdpw_wlr_output_chooser(ctx);
	return target->output != NULL;
}
