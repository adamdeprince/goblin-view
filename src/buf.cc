/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 */

#include "dv.hpp"

#include <string.h>

void
dv_buf_append(struct dv_buf *b, const void *p, size_t n)
{
	if (n == 0)
		return;
	if (b->len + n > b->space) {
		size_t	sp = b->space ? b->space : 256;

		while (sp < b->len + n)
			sp *= 2;
		b->data = dv_resize(b->data, sp);
		b->space = sp;
	}
	memcpy(b->data + b->len, p, n);
	b->len += n;
}

void
dv_buf_consume(struct dv_buf *b, size_t n)
{
	if (n > b->len)
		n = b->len;
	if (n == 0)
		return;
	memmove(b->data, b->data + n, b->len - n);
	b->len -= n;
}

void
dv_buf_put_utf8(struct dv_buf *b, uint32_t ch)
{
	unsigned char	u[4];
	size_t		n;

	if (ch < 0x80) {
		u[0] = (unsigned char)ch;
		n = 1;
	} else if (ch < 0x800) {
		u[0] = (unsigned char)(0xc0 | (ch >> 6));
		u[1] = (unsigned char)(0x80 | (ch & 0x3f));
		n = 2;
	} else if (ch < 0x10000) {
		u[0] = (unsigned char)(0xe0 | (ch >> 12));
		u[1] = (unsigned char)(0x80 | ((ch >> 6) & 0x3f));
		u[2] = (unsigned char)(0x80 | (ch & 0x3f));
		n = 3;
	} else {
		u[0] = (unsigned char)(0xf0 | (ch >> 18));
		u[1] = (unsigned char)(0x80 | ((ch >> 12) & 0x3f));
		u[2] = (unsigned char)(0x80 | ((ch >> 6) & 0x3f));
		u[3] = (unsigned char)(0x80 | (ch & 0x3f));
		n = 4;
	}
	dv_buf_append(b, u, n);
}

void
dv_row_text(struct dv_buf *b, const struct dv_cell *row, unsigned n)
{
	size_t		start = b->len, trim;
	unsigned	x;

	for (x = 0; x < n; x++) {
		uint32_t	ch = row[x].ch;

		if (ch == 0)			/* the second cell of a wide glyph */
			continue;
		if (ch == DV_KITTY_PLACEHOLDER || ch == DV_STEXT_MARK)
			ch = U' ';
		dv_buf_put_utf8(b, ch);
	}
	trim = b->len;
	while (trim > start && ((char *)b->data)[trim - 1] == ' ')
		trim--;
	b->len = trim;
}

void
dv_buf_free(struct dv_buf *b)
{
	free(b->data);
	b->data = NULL;
	b->len = 0;
	b->space = 0;
}
