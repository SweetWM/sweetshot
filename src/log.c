#include "log.h"

#include <stdarg.h>
#include <stdio.h>

void log_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fputs("sweetshot: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}
