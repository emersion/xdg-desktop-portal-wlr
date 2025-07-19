#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "string_util.h"

static char *vformat_str(const char *fmt, va_list args) {
	char *str = NULL;
	va_list args_copy;
	va_copy(args_copy, args);

	int len = vsnprintf(NULL, 0, fmt, args);
	if (len < 0) {
		goto out;
	}

	str = malloc(len + 1);
	if (str == NULL) {
		goto out;
	}

	vsnprintf(str, len + 1, fmt, args_copy);

out:
	va_end(args_copy);
	return str;
}

char *format_str(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	char *str = vformat_str(fmt, args);
	va_end(args);
	return str;
}
