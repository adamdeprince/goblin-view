/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 */

#include "dv.hpp"

#include <errno.h>
#include <stdarg.h>
#include <string.h>

void *
xmalloc(size_t sz)
{
	void	*p;

	if (sz == 0)
		sz = 1;
	p = malloc(sz);
	if (p == NULL)
		dv_fatal("malloc: %s", strerror(errno));
	return (p);
}

void *
xcalloc(size_t n, size_t sz)
{
	void	*p;

	if (n == 0 || sz == 0) {
		n = 1;
		sz = 1;
	}
	p = calloc(n, sz);
	if (p == NULL)
		dv_fatal("calloc: %s", strerror(errno));
	return (p);
}

void *
xrealloc(void *p, size_t sz)
{
	void	*n;

	if (sz == 0)
		sz = 1;
	n = realloc(p, sz);
	if (n == NULL)
		dv_fatal("realloc: %s", strerror(errno));
	return (n);
}

char *
xstrdup(const char *s)
{
	char	*n;
	size_t	 len;

	if (s == NULL)
		s = "";
	len = strlen(s) + 1;
	n = static_cast<char *>(xmalloc(len));
	memcpy(n, s, len);
	return (n);
}

char *
xasprintf(const char *fmt, ...)
{
	va_list	 ap;
	int	 n;
	char	*s;

	va_start(ap, fmt);
	n = vasprintf(&s, fmt, ap);
	va_end(ap);
	if (n < 0)
		dv_fatal("vasprintf: %s", strerror(errno));
	return (s);
}

size_t
dv_strlcpy(char *dst, const char *src, size_t dstsz)
{
	size_t	n;

	if (dstsz == 0)
		return (strlen(src));
	n = 0;
	while (n + 1 < dstsz && src[n] != '\0') {
		dst[n] = src[n];
		n++;
	}
	dst[n] = '\0';
	while (src[n] != '\0')
		n++;
	return (n);
}
