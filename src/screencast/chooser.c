#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "child.h"
#include "logger.h"
#include "screencast.h"
#include "string_util.h"
#include "wlr_screencast.h"
#include "xdpw.h"

static struct xdpw_wlr_output *xdpw_wlr_output_first(struct wl_list *output_list) {
	struct xdpw_wlr_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, output_list, link) {
		return output;
	}
	return NULL;
}

static char *get_output_label(struct xdpw_wlr_output *output, enum xdpw_chooser_types chooser_type) {
	if (chooser_type == XDPW_CHOOSER_DMENU) {
		return format_str("Monitor: %s %s", output->name, output->description);
	} else {
		return format_str("Monitor: %s", output->name);
	}
}

static char *get_toplevel_label(struct xdpw_toplevel *toplevel, enum xdpw_chooser_types chooser_type) {
	if (chooser_type == XDPW_CHOOSER_DMENU) {
		return format_str("Window: %s (%s)", toplevel->title, toplevel->identifier);
	} else {
		return format_str("Window: %s", toplevel->identifier);
	}
}

struct xdpw_chooser_job {
	struct xdpw_screencast_context *ctx;
	struct xdpw_screencast_target *target;
	uint32_t type_mask;
	// Choosers to try in order, until one of them can be run
	const struct xdpw_chooser *choosers;
	size_t n_choosers, index;
	struct xdpw_chooser configured;
	struct xdpw_child *child;
	xdpw_chooser_done_func_t done;
	void *data;
};

static const struct xdpw_chooser default_choosers[] = {
	{XDPW_CHOOSER_SIMPLE, "slurp -f 'Monitor: %o' -or"},
	{XDPW_CHOOSER_DMENU, "wmenu -p 'Select a source to share:' -l 10"},
	{XDPW_CHOOSER_DMENU, "wofi -d -n --prompt='Select a source to share:'"},
	{XDPW_CHOOSER_DMENU, "rofi -dmenu -p 'Select a source to share:'"},
	{XDPW_CHOOSER_DMENU, "bemenu --prompt='Select a source to share:'"},
	{XDPW_CHOOSER_DMENU, "mew -l 10 -p 'Select a source to share:'"},
	{XDPW_CHOOSER_DMENU, "fuzzel -d -l 10 -p 'Select a source to share:'"},
};

static char *get_chooser_input(struct xdpw_chooser_job *job,
		const struct xdpw_chooser *chooser) {
	struct xdpw_screencast_context *ctx = job->ctx;
	char *input = NULL;
	size_t input_size = 0;
	FILE *f = open_memstream(&input, &input_size);
	if (f == NULL) {
		return NULL;
	}

	if (chooser->type == XDPW_CHOOSER_DMENU) {
		if (job->type_mask & MONITOR) {
			struct xdpw_wlr_output *out;
			wl_list_for_each(out, &ctx->output_list, link) {
				char *label = get_output_label(out, chooser->type);
				fprintf(f, "%s\n", label);
				free(label);
			}
		}
		if (job->type_mask & WINDOW) {
			struct xdpw_toplevel *toplevel;
			wl_list_for_each(toplevel, &ctx->toplevels, link) {
				char *label = get_toplevel_label(toplevel, chooser->type);
				fprintf(f, "%s\n", label);
				free(label);
			}
		}
	}

	if (fclose(f) != 0) {
		free(input);
		return NULL;
	}
	return input;
}

static void select_label(struct xdpw_chooser_job *job,
		const struct xdpw_chooser *chooser, const char *selected_label) {
	struct xdpw_screencast_context *ctx = job->ctx;
	struct xdpw_screencast_target *target = job->target;

	logprint(TRACE, "wlroots: chooser %s selects %s", chooser->cmd, selected_label);

	bool found = false;
	struct xdpw_wlr_output *out;
	wl_list_for_each(out, &ctx->output_list, link) {
		char *label = get_output_label(out, chooser->type);
		found = strcmp(selected_label, label) == 0;
		free(label);
		if (!found && chooser->type == XDPW_CHOOSER_SIMPLE) {
			// Compatibility with xdg-desktop-portal-wlr < v0.8.0
			found = strcmp(selected_label, out->name) == 0;
		}
		if (found) {
			target->type = MONITOR;
			target->output = out;
			break;
		}
	}

	struct xdpw_toplevel *toplevel;
	wl_list_for_each(toplevel, &ctx->toplevels, link) {
		if (found) {
			break;
		}
		char *label = get_toplevel_label(toplevel, chooser->type);
		found = strcmp(selected_label, label) == 0;
		free(label);
		if (found) {
			target->type = WINDOW;
			target->toplevel = toplevel;
			break;
		}
	}

	if (!found) {
		logprint(ERROR, "wlroots: chooser %s selected unknown target: %s", chooser->cmd, selected_label);
	}
}

static bool chooser_job_run_next(struct xdpw_chooser_job *job);

static void chooser_job_finish(struct xdpw_chooser_job *job, bool ran) {
	xdpw_chooser_done_func_t done = job->done;
	void *data = job->data;
	struct xdpw_screencast_target *target = job->target;
	free(job);
	done(ran && (target->output || target->toplevel), data);
}

static void chooser_done(struct xdpw_child *child, int status,
		const char *out, size_t out_len, void *data) {
	struct xdpw_chooser_job *job = data;
	const struct xdpw_chooser *chooser = &job->choosers[job->index];
	job->child = NULL;

	// 127: the shell couldn't find the chooser
	if (!WIFEXITED(status) || WEXITSTATUS(status) == 127) {
		logprint(DEBUG, "wlroots: chooser %s failed", chooser->cmd);
		job->index++;
		if (!chooser_job_run_next(job)) {
			chooser_job_finish(job, false);
		}
		return;
	}

	// Only the first line matters
	size_t label_len = strcspn(out, "\n");
	if (label_len > 0 || out_len > 0) {
		char *selected_label = strndup(out, label_len);
		if (selected_label != NULL) {
			select_label(job, chooser, selected_label);
			free(selected_label);
		}
	}

	chooser_job_finish(job, true);
}

static bool chooser_job_run_next(struct xdpw_chooser_job *job) {
	for (; job->index < job->n_choosers; job->index++) {
		const struct xdpw_chooser *chooser = &job->choosers[job->index];
		if (chooser->type == XDPW_CHOOSER_SIMPLE && job->choosers == default_choosers &&
				job->type_mask != MONITOR) {
			continue;
		}

		logprint(DEBUG, "wlroots: chooser %s (%d)", chooser->cmd, chooser->type);
		char *input = get_chooser_input(job, chooser);
		job->child = xdpw_child_spawn(job->ctx->state, chooser->cmd, input,
			chooser_done, job);
		free(input);
		if (job->child != NULL) {
			return true;
		}
		logprint(ERROR, "Failed to fork chooser");
	}
	return false;
}

struct xdpw_chooser_job *xdpw_wlr_target_chooser_start(struct xdpw_screencast_context *ctx,
		struct xdpw_screencast_target *target, uint32_t type_mask,
		xdpw_chooser_done_func_t done, void *data) {
	struct xdpw_chooser_job *job = calloc(1, sizeof(*job));
	if (job == NULL) {
		return NULL;
	}
	job->ctx = ctx;
	job->target = target;
	job->type_mask = type_mask;
	job->done = done;
	job->data = data;

	switch (ctx->state->config->screencast_conf.chooser_type) {
	case XDPW_CHOOSER_DEFAULT:
		job->choosers = default_choosers;
		job->n_choosers = sizeof(default_choosers) / sizeof(default_choosers[0]);
		break;
	case XDPW_CHOOSER_DMENU:
	case XDPW_CHOOSER_SIMPLE:
		if (!ctx->state->config->screencast_conf.chooser_cmd) {
			logprint(ERROR, "wlroots: no chooser given");
			goto error;
		}
		job->configured = (struct xdpw_chooser){
			ctx->state->config->screencast_conf.chooser_type,
			ctx->state->config->screencast_conf.chooser_cmd
		};
		job->choosers = &job->configured;
		job->n_choosers = 1;
		break;
	case XDPW_CHOOSER_NONE:
		// Use xdpw_wlr_target_chooser_none() instead
		goto error;
	}

	logprint(DEBUG, "wlroots: chooser called");
	if (!chooser_job_run_next(job)) {
		logprint(ERROR, "wlroots: no chooser could be run");
		goto error;
	}
	return job;

error:
	free(job);
	return NULL;
}

void xdpw_wlr_target_chooser_cancel(struct xdpw_chooser_job *job) {
	if (job->child != NULL) {
		xdpw_child_cancel(job->child);
	}
	free(job);
}

bool xdpw_wlr_target_chooser_none(struct xdpw_screencast_context *ctx,
		struct xdpw_screencast_target *target) {
	target->type = MONITOR;
	if (ctx->state->config->screencast_conf.output_name) {
		target->output = xdpw_wlr_output_find_by_name(&ctx->output_list, ctx->state->config->screencast_conf.output_name);
	} else {
		target->output = xdpw_wlr_output_first(&ctx->output_list);
	}
	return target->output != NULL;
}
