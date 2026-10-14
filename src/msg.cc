/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 */

#include "dv.hpp"

#include <errno.h>
#include <string.h>
#include <unistd.h>

void
dv_conn_init(struct dv_conn *c, int fd)
{
	memset(c, 0, sizeof *c);
	c->fd = fd;
}

void
dv_conn_free(struct dv_conn *c)
{
	dv_buf_free(&c->in);
	dv_buf_free(&c->out);
	c->fd = -1;
}

int
dv_msg_pack(struct dv_buf *b, int type, const void *payload, size_t len)
{
	unsigned char	hdr[5];

	if (len > DV_MAX_MSG)
		return (-1);
	hdr[0] = (unsigned char)((len >> 24) & 0xff);
	hdr[1] = (unsigned char)((len >> 16) & 0xff);
	hdr[2] = (unsigned char)((len >> 8) & 0xff);
	hdr[3] = (unsigned char)(len & 0xff);
	hdr[4] = (unsigned char)type;
	dv_buf_append(b, hdr, 5);
	if (len != 0)
		dv_buf_append(b, payload, len);
	return (0);
}

int
dv_msg_take(struct dv_buf *b, int *type, unsigned char **payload, size_t *len)
{
	size_t		 n;
	unsigned char	*p;

	if (b->len < 5)
		return (0);
	p = b->data;
	n = ((size_t)p[0] << 24) | ((size_t)p[1] << 16) |
	    ((size_t)p[2] << 8) | (size_t)p[3];
	if (n > DV_MAX_MSG)
		return (-1);
	if (b->len < 5 + n)
		return (0);
	*type = p[4];
	*len = n;
	if (n == 0)
		*payload = NULL;
	else {
		*payload = dv_allocn<unsigned char>(n);
		memcpy(*payload, p + 5, n);
	}
	dv_buf_consume(b, 5 + n);
	return (1);
}

int
dv_conn_send(struct dv_conn *c, int type, const void *payload, size_t len)
{
	return (dv_msg_pack(&c->out, type, payload, len));
}

int
dv_conn_read(struct dv_conn *c)
{
	unsigned char	 buf[8192];
	ssize_t		 n;

	n = read(c->fd, buf, sizeof buf);
	if (n == 0)
		return (0);
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return (1);
		return (-1);
	}
	dv_buf_append(&c->in, buf, (size_t)n);
	return (1);
}

int
dv_conn_flush(struct dv_conn *c)
{
	ssize_t	n;

	while (c->out.len > 0) {
		n = write(c->fd, c->out.data, c->out.len);
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return (1);
			return (-1);
		}
		if (n == 0)
			return (1);
		dv_buf_consume(&c->out, (size_t)n);
	}
	return (1);
}
