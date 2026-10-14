/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 */

#include "dv.hpp"

#include <stdarg.h>
#include <time.h>

int	dv_verbose;

void
dv_log(const char *fmt, ...)
{
	va_list		 ap;
	struct timespec	 ts;

	if (!dv_verbose)
		return;
	clock_gettime(CLOCK_REALTIME, &ts);
	fprintf(stderr, "goblin-view %lld.%03ld: ",
	    (long long)ts.tv_sec, ts.tv_nsec / 1000000L);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void
dv_fatal(const char *fmt, ...)
{
	va_list	ap;

	fprintf(stderr, "goblin-view: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}
