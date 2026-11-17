/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Multiplexer-side kitty graphics. Original code against the public
 * protocol; the behaviour tracks Adam's tmux kitty-graphics branch
 * (docs/tmux-tracking.md), not its source.
 *
 * A odin-seat window is its own terminal, so it terminates the kitty protocol:
 * it stores the images a program transmits (direct, file or shared
 * memory medium), answers its queries, and turns each display request
 * into a placement on the window's grid -- kitty's unit of display,
 * floating above the text as it does in kitty itself. A redraw of the
 * text beneath leaves it, scrolling carries it, ED / RIS / leaving the
 * alternate screen clear it, and a delete removes it by image id. The
 * program's bytes never reach the host.
 *
 * odin-seat is in turn a kitty CLIENT of the host: each placement is one image
 * on the host, sent under a host-global id with a virtual (U=1)
 * placement of its cell size and source rectangle. The compositor then
 * paints Unicode placeholder cells wherever the placement is visible, so
 * cropping to the window, occlusion by other windows and moving the
 * window are ordinary cell compositing -- no pixels are re-sent for them.
 *
 * A vector image (f=200) is kept as the table of records its specification
 * asks of a relay, so a client that attaches later is sent the scene as it
 * stands, and an edit (a=e) goes to the clients that hold the scene without
 * sending it again. The vector service (a=v) is a question for one terminal.
 * Session fonts live in that terminal under names of our own, which is what
 * keeps the windows apart, and are uploaded to any other that shows them.
 */

#include "dv.hpp"
#include "silkie.hpp"

#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "vector/vector.h"
#include "vtext.hpp"
#include <zlib.h>
#include <limits.h>
#include <stdarg.h>

#define CHUNK		4096		/* kitty's payload chunk limit */

static long
key_val(const char *c, size_t n, char k, long def)
{
	size_t	i;

	for (i = 0; i + 1 < n; i++) {
		if ((i == 0 || c[i - 1] == ',') && c[i] == k && c[i + 1] == '=') {
			long	v = 0;
			int	neg = 0, any = 0;
			size_t	j = i + 2;

			if (j < n && c[j] == '-') {
				neg = 1;
				j++;
			}
			while (j < n && c[j] >= '0' && c[j] <= '9') {
				v = v * 10 + (c[j] - '0');
				j++;
				any = 1;
			}
			return (any ? (neg ? -v : v) : def);
		}
	}
	return (def);
}

static int
key_present(const char *c, size_t n, char k)
{
	size_t	i;

	for (i = 0; i + 1 < n; i++)
		if ((i == 0 || c[i - 1] == ',') && c[i] == k && c[i + 1] == '=')
			return (1);
	return (0);
}

/* The character after k= (the one-letter keys: a, t, d, o). */
static char
key_chr(const char *c, size_t n, char k, char def)
{
	size_t	i;

	for (i = 0; i + 2 < n; i++)
		if ((i == 0 || c[i - 1] == ',') && c[i] == k && c[i + 1] == '=')
			return (c[i + 2]);
	return (def);
}

/*
 * S and O, the size and offset of data read from a file or shared memory:
 * 64-bit unsigned in Silkie (docs/image-tiling.rst). 1 with the value, 0
 * when absent, -1 when it does not fit in 64 bits or is not a number.
 */
static int
key_u64(const char *c, size_t n, char k, uint64_t *out)
{
	for (size_t i = 0; i + 1 < n; i++) {
		if ((i == 0 || c[i - 1] == ',') && c[i] == k && c[i + 1] == '=') {
			uint64_t	v = 0;
			size_t		j = i + 2;
			int		any = 0;

			for (; j < n && c[j] >= '0' && c[j] <= '9'; j++) {
				unsigned	d = (unsigned)(c[j] - '0');

				if (v > (UINT64_MAX - d) / 10)
					return (-1);
				v = v * 10 + d;
				any = 1;
			}
			if (!any)
				return (-1);
			*out = v;
			return (1);
		}
	}
	*out = 0;
	return (0);
}

/*
 * A command whose numbers do not fit their keys is malformed, and a terminal
 * answers nothing to it, as kitty does: every key is 32 bits but S and O,
 * which are 64. The keys whose values are characters are not numbers.
 */
static int
cmd_malformed(const char *c, size_t n)
{
	size_t	at = 0;

	while (at < n) {
		size_t	end = at;

		while (end < n && c[end] != ',')
			end++;
		if (end - at >= 2 && c[at + 1] == '=' &&
		    strchr("atode", c[at]) == nullptr) {
			const char	*v = c + at + 2, *ve = c + end;
			int		 neg = v < ve && *v == '-';
			uint64_t	 x = 0;

			if (neg)
				v++;
			if (c[at] == 'S' || c[at] == 'O') {
				uint64_t	u;

				if (neg || key_u64(c + at, end - at, c[at], &u) < 0)
					return (1);
			} else {
				for (; v < ve && *v >= '0' && *v <= '9'; v++) {
					x = x * 10 + (uint64_t)(*v - '0');
					if (x > (neg ? 2147483648ull : 4294967295ull))
						return (1);
				}
			}
		}
		at = end + 1;
	}
	return (0);
}

/* ---- pixel formats ---- */

/* What a value of f is. */
enum {
	FK_NONE, FK_RGB, FK_RGBA, FK_PNG, FK_IDX8, FK_IDX16, FK_GRAY, FK_565,
	FK_PAL, FK_G16, FK_10A2, FK_16A, FK_F64, FK_VEC
};

static int
fmt_kind(long f)
{
	switch (f) {
	case 24: return (FK_RGB);
	case 32: return (FK_RGBA);
	case 100: return (FK_PNG);
	case 8: return (FK_IDX8);
	case 16: return (FK_IDX16);
	case 301: case 302: case 304: case 308: return (FK_GRAY);
	case 516: return (FK_565);
	case 401: case 402: case 404: case 408: return (FK_PAL);
	case 316: return (FK_G16);
	case 632: return (FK_10A2);
	case 664: return (FK_16A);
	case 64: return (FK_F64);
	case 200: return (FK_VEC);
	}
	return (FK_NONE);
}

/* Bits of a gray sample or a palette index: f less its family's base. */
static int
fmt_bits(long f)
{
	int	k = fmt_kind(f);

	return (k == FK_GRAY ? (int)(f - 300) : k == FK_PAL ? (int)(f - 400) : 0);
}

/* The high precision formats, whose pixels have more than eight bits. */
static int
fmt_deep(long f)
{
	int	k = fmt_kind(f);

	return (k == FK_G16 || k == FK_10A2 || k == FK_16A);
}

int
dv_kitty_fmt_known(long f)
{
	return (fmt_kind(f) != FK_NONE);
}

int
dv_kitty_fmt_bit(long f)
{
	switch (f) {
	case 301: return (DV_FMT_G1);
	case 302: return (DV_FMT_G2);
	case 304: return (DV_FMT_G4);
	case 308: return (DV_FMT_G8);
	case 516: return (DV_FMT_565);
	case 8: return (DV_FMT_I8);
	case 16: return (DV_FMT_I16);
	case 401: return (DV_FMT_P1);
	case 402: return (DV_FMT_P2);
	case 404: return (DV_FMT_P4);
	case 408: return (DV_FMT_P8);
	case 316: return (DV_FMT_G16);
	case 632: return (DV_FMT_10A2);
	case 664: return (DV_FMT_16A);
	}
	return (0);
}

/* G tags the colours of RGB, RGBA, RGB565, the high precision colour
 * formats and the palettes of palette images; gray and indexed colour, PNG,
 * f=64 and f=200 say their own. */
int
dv_kitty_fmt_gamut(long f)
{
	int	k = fmt_kind(f);

	return (k == FK_RGB || k == FK_RGBA || k == FK_565 || k == FK_10A2 ||
	    k == FK_16A || k == FK_PAL);
}

/*
 * A terminal's caps format= list as DV_FMT_* bits. Two older numberings
 * of Silkie's are not this one: before 2026-11-16 it listed its packed
 * formats as 1, 2, 4, 8 and 16 -- so 8 and 16 there are gray and RGB565,
 * not indexed colour -- and for a while after RGB565 was 316, which is now
 * 16-bit gray. A list with 1, 2 or 4 is of the first; 316 without 516 or
 * the other high precision formats of the second. Neither number is taken.
 */
int
dv_kitty_fmt_list(const char *csv)
{
	int		 bits = 0, old_packed = 0, old_565 = 0;
	std::vector<long> nums;

	if (csv == nullptr)
		return (0);
	for (const char *at = csv; *at != '\0'; ) {
		char	*end;
		long	 v = strtol(at, &end, 10);

		if (end != at)
			nums.push_back(v);
		while (*end != '\0' && *end != ',')
			end++;
		at = *end != '\0' ? end + 1 : end;
	}
	for (long v : nums)
		if (v == 1 || v == 2 || v == 4)
			old_packed = 1;
	{
		int	has316 = 0, has_new = 0;

		for (long v : nums) {
			has316 |= v == 316;
			has_new |= v == 516 || v == 632 || v == 664;
		}
		old_565 = has316 && !has_new;
	}
	for (long v : nums) {
		if (old_packed && (v == 8 || v == 16))
			continue;
		if (old_565 && v == 316)
			continue;
		bits |= dv_kitty_fmt_bit(v);
	}
	if (bits & (DV_FMT_G16 | DV_FMT_10A2 | DV_FMT_16A))
		bits |= DV_FMT_E;
	return (bits);
}

/*
 * The bytes a raw format's pixels take at w by h (indexed colour, packed,
 * palette, high precision, and RGB, RGBA and f=64): 0 with *need, -1 when
 * the size does not fit in memory, -2 for a format with no fixed size.
 */
static int
fmt_size(long f, uint64_t w, uint64_t h, uint64_t *need)
{
	uint64_t	bpp = 0, row, pal = 0;
	int		k = fmt_kind(f), bits = fmt_bits(f);

	switch (k) {
	case FK_IDX8: bpp = 1; break;
	case FK_IDX16: case FK_565: case FK_G16: bpp = 2; break;
	case FK_RGB: bpp = 3; break;
	case FK_RGBA: case FK_10A2: bpp = 4; break;
	case FK_16A: case FK_F64: bpp = 8; break;
	case FK_GRAY: case FK_PAL: break;
	default: return (-2);
	}
	if (k == FK_PAL)
		pal = 4ull << bits;
	if (bpp != 0) {
		if (w != 0 && h > UINT64_MAX / w / bpp)
			return (-1);
		row = w * bpp;
	} else {
		if (w > (UINT64_MAX - 7) / (uint64_t)bits)
			return (-1);
		row = (w * (uint64_t)bits + 7) / 8;
	}
	if (row != 0 && h > (UINT64_MAX - pal) / row)
		return (-1);
	*need = pal + row * h;
	if (*need > (uint64_t)SIZE_MAX / 2)	/* and its base64, held too */
		return (-1);
	return (0);
}

/*
 * Is the transfer function E allowed with f (precision-image-protocol)? 0
 * (no key) always; 13, 8, 16 and 18 with RGB, RGBA, gray, packed and the
 * high precision formats; with f=64, which is linear, only 8.
 */
static int
fmt_takes_E(long f, long E)
{
	int	k = fmt_kind(f);

	if (E == 0)
		return (1);
	if (E != 8 && E != 13 && E != 16 && E != 18)
		return (0);
	if (k == FK_F64)
		return (E == 8);
	return (k == FK_RGB || k == FK_RGBA || k == FK_GRAY || k == FK_565 ||
	    fmt_deep(f));
}

/* Copy the transmit keys the host needs to decode the same pixels. E is
 * kept only where it changes the pixels: 13 is the same as none, and 8 on
 * f=64 is what f=64 is. */
static void
capture_fmt(struct dv_kimage *im, const char *c, size_t n)
{
	static const char	keys[] = "fsvT";
	char			*o = im->fmt;
	size_t			 room = sizeof im->fmt;
	int			 first = 1, ki, w;
	long			 f = key_val(c, n, 'f', 32);

	im->fmt[0] = '\0';
	for (ki = 0; keys[ki] != '\0'; ki++) {
		long	v = key_val(c, n, keys[ki], -1);

		if (v < 0)
			continue;
		w = snprintf(o, room, "%s%c=%ld", first ? "" : ",", keys[ki], v);
		if (w <= 0 || (size_t)w >= room)
			dv_fatal("kitty image keys longer than their bound");
		o += w;
		room -= (size_t)w;
		first = 0;
	}
	{
		long	G = key_val(c, n, 'G', -1);
		long	E = key_val(c, n, 'E', 0);

		if (G == 1 || G == 2) {
			w = snprintf(o, room, "%sG=%ld", first ? "" : ",", G);
			if (w <= 0 || (size_t)w >= room)
				dv_fatal("kitty image keys longer than their bound");
			o += w;
			room -= (size_t)w;
			first = 0;
		}
		if (E != 0 && E != 13 && fmt_kind(f) != FK_F64) {
			w = snprintf(o, room, "%sE=%ld", first ? "" : ",", E);
			if (w <= 0 || (size_t)w >= room)
				dv_fatal("kitty image keys longer than their bound");
			o += w;
			room -= (size_t)w;
			first = 0;
		}
	}
	if (key_chr(c, n, 'o', 0) == 'z') {	/* zlib-deflated pixels */
		w = snprintf(o, room, "%so=z", first ? "" : ",");
		if (w <= 0 || (size_t)w >= room)
			dv_fatal("kitty image keys longer than their bound");
	}
}

/* Decode up to `max` bytes of standard base64 into out. */
static size_t
b64_decode(const char *s, size_t n, unsigned char *out, size_t max)
{
	size_t	 o = 0;
	int	 acc = 0, bits = 0;

	for (size_t i = 0; i < n && o < max; i++) {
		int	ch = (unsigned char)s[i], v;

		if (ch >= 'A' && ch <= 'Z')
			v = ch - 'A';
		else if (ch >= 'a' && ch <= 'z')
			v = ch - 'a' + 26;
		else if (ch >= '0' && ch <= '9')
			v = ch - '0' + 52;
		else if (ch == '+')
			v = 62;
		else if (ch == '/')
			v = 63;
		else
			continue;	/* skip '=', newlines, etc. */
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (unsigned char)((acc >> bits) & 0xff);
		}
	}
	return (o);
}

static void
b64_encode(const unsigned char *in, size_t n, struct dv_buf *out)
{
	static const char	t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	    "abcdefghijklmnopqrstuvwxyz0123456789+/";
	char			q[4];
	size_t			i;

	for (i = 0; i < n; i += 3) {
		unsigned	v = (unsigned)in[i] << 16;

		if (i + 1 < n)
			v |= (unsigned)in[i + 1] << 8;
		if (i + 2 < n)
			v |= in[i + 2];
		q[0] = t[(v >> 18) & 63];
		q[1] = t[(v >> 12) & 63];
		q[2] = i + 1 < n ? t[(v >> 6) & 63] : '=';
		q[3] = i + 2 < n ? t[v & 63] : '=';
		dv_buf_append(out, q, 4);
	}
}

/*
 * t=f / t=t / t=s: the payload names a file, a temporary file or a POSIX
 * shared memory object holding the pixels. The endpoint reads it (the
 * host may be a remote terminal that cannot) and keeps the bytes
 * base64-encoded, the form the direct medium arrives in. A temporary
 * file and a shared memory object are removed, as the protocol asks.
 */
/*
 * size and off are 64-bit (S and O): data past 4 GiB, or more than 4 GiB of
 * it, is read as any other. Only the pages that hold it are mapped. An end
 * that does not fit in a file offset is refused before anything is opened.
 * A shared memory name goes to shm_open() and shm_unlink() as it came: it
 * need not start with a slash (a sandboxed macOS program's cannot).
 * NULL when read, otherwise the error to answer.
 */
static const char *
read_medium(struct dv_buf *data, char medium, const char *payload,
    size_t plen, uint64_t size, uint64_t off)
{
	std::string	 name(plen + 1, '\0');
	const char	*path = name.c_str(), *err = "EBADF:could not read medium";
	size_t		 n;
	int		 fd;
	struct stat	 st;
	void		*m;

	if (off > (uint64_t)INT64_MAX || size > (uint64_t)INT64_MAX - off)
		return ("EOVERFLOW:offset and size past a file offset");
	n = b64_decode(payload, plen, (unsigned char *)name.data(), plen);
	name[n] = '\0';
	if (n == 0)
		return (err);
	if (medium == 's')
		fd = shm_open(path, O_RDONLY, 0);
	else
		fd = open(path, O_RDONLY);
	if (fd < 0)
		return (err);
	if (fstat(fd, &st) == 0 && st.st_size >= 0 &&
	    off < (uint64_t)st.st_size) {
		uint64_t	have = (uint64_t)st.st_size - off;
		uint64_t	page = (uint64_t)sysconf(_SC_PAGESIZE);
		uint64_t	start, lead, len;

		if (size == 0 || size > have)
			size = have;
		start = page != 0 ? off / page * page : off;
		lead = off - start;
		len = lead + size;
		if (len > (uint64_t)SIZE_MAX || size > (uint64_t)SIZE_MAX / 2)
			err = "EOVERFLOW:too large for memory";
		else {
			m = mmap(NULL, (size_t)len, PROT_READ, MAP_SHARED, fd,
			    (off_t)start);
			if (m == MAP_FAILED)
				err = errno == ENOMEM ? "ENOMEM:out of memory" :
				    "EBADF:could not map medium";
			else {
				data->len = 0;
				b64_encode((const unsigned char *)m + lead,
				    (size_t)size, data);
				munmap(m, (size_t)len);
				err = nullptr;
			}
		}
	}
	close(fd);
	if (medium == 't' && strstr(path, "tmp") != NULL)
		unlink(path);
	else if (medium == 's')
		shm_unlink(path);
	return (err);
}

/*
 * Inflate a zlib stream of unknown size into a grown buffer. zlib counts in
 * 32 bits, so a stream larger than that is fed and drained a piece at a
 * time; the output is as large as the stream says.
 */
static int
inflate_grow(const unsigned char *in, size_t inlen, unsigned char **out,
    size_t *outlen)
{
	const size_t	piece = UINT_MAX;
	z_stream	zs;
	size_t		cap, used = 0, fed = 0;
	unsigned char	*buf;
	int		rc;

	cap = inlen < (SIZE_MAX - 4096) / 4 ? inlen * 4 + 4096 : SIZE_MAX / 2;
	buf = (unsigned char *)malloc(cap);
	if (buf == nullptr)
		return (0);
	memset(&zs, 0, sizeof zs);
	if (inflateInit(&zs) != Z_OK) {
		free(buf);
		return (0);
	}
	for (;;) {
		size_t	in_now, out_now;

		if (zs.avail_in == 0 && fed < inlen) {
			in_now = inlen - fed < piece ? inlen - fed : piece;
			zs.next_in = (Bytef *)(in + fed);
			zs.avail_in = (uInt)in_now;
			fed += in_now;
		}
		if (used == cap) {
			size_t		 ncap = cap <= SIZE_MAX / 2 ? cap * 2 : SIZE_MAX;
			unsigned char	*nb;

			if (ncap == cap ||
			    (nb = (unsigned char *)realloc(buf, ncap)) == nullptr) {
				inflateEnd(&zs);
				free(buf);
				return (0);
			}
			buf = nb;
			cap = ncap;
		}
		out_now = cap - used < piece ? cap - used : piece;
		zs.next_out = buf + used;
		zs.avail_out = (uInt)out_now;
		rc = inflate(&zs, Z_NO_FLUSH);
		used += out_now - zs.avail_out;
		if (rc == Z_STREAM_END)
			break;
		if (rc == Z_BUF_ERROR && zs.avail_in == 0 && fed == inlen)
			break;			/* no progress, no more input */
		if (rc != Z_OK && rc != Z_BUF_ERROR) {
			inflateEnd(&zs);
			free(buf);
			return (0);
		}
	}
	*outlen = used;
	*out = buf;
	inflateEnd(&zs);
	return (1);
}

static unsigned char *scene_raster(const struct dv_kimage *, unsigned, unsigned);
static unsigned char *f64_rgba(const struct dv_kimage *);
static int	image_raw(const struct dv_kimage *, std::vector<unsigned char> *);

/* ---- colours of kitty's indexed colour formats (f=8, f=16) ---- */

/*
 * The RGB an index names in a window: 0-247 the SGR 256 colours, then the
 * default background and foreground, selection, cursor, cursor text, the
 * URL hover colour, and 255, the foreground. The window's overrides (OSC
 * 4, 10, 11, 12) win over the default theme, which is the one odin-seat
 * answers colour queries with (term.cc). odin-seat draws no selection or
 * URL hover of its own in a program's colours, so those are taken as the
 * protocol takes a colour drawn from the cell: the foreground for 250, 252
 * and 254, the background for 251 and 253.
 */
static uint32_t
idx_rgb(const struct dv_palette *p, unsigned i)
{
	uint32_t	fg = p != nullptr && p->fgset ? p->fg : 0xe5e5e5;
	uint32_t	bg = p != nullptr && p->bgset ? p->bg : 0x000000;

	fg &= 0xffffff;
	bg &= 0xffffff;
	if (i < 248)
		return ((p != nullptr && p->set[i] ? p->idx[i] : dv_index_rgb(i)) &
		    0xffffff);
	switch (i) {
	case 248: case 251: case 253:
		return (bg);
	case 252:
		return (p != nullptr && p->curset ? p->cursor & 0xffffff : fg);
	default:
		return (fg);
	}
}

/* A fingerprint of the colours a window's indices name. */
static uint64_t
pal_stamp(const struct dv_palette *p)
{
	uint64_t	h = 1469598103934665603ull;

	for (unsigned i = 0; i < 256; i++) {
		h ^= idx_rgb(p, i);
		h *= 1099511628211ull;
	}
	return (h);
}

/* ---- transfer functions (precision-image-protocol) ---- */

static float
srgb_to_lin(float c)
{
	return (c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f));
}

static float
lin_to_srgb(float c)
{
	if (!(c > 0))
		return (0);
	if (c >= 1)
		return (1);
	return (c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f);
}

/* SDR white: the reference white of ITU-R BT.2408, as Silkie takes it. */
#define SDR_WHITE_NITS	203.0f

/* SMPTE ST 2084 (PQ): a signal as light, 1.0 being SDR white. */
static float
pq_to_lin(float e)
{
	const float	m1 = 2610.0f / 16384, m2 = 2523.0f / 4096 * 128;
	const float	c1 = 3424.0f / 4096, c2 = 2413.0f / 4096 * 32;
	const float	c3 = 2392.0f / 4096 * 32;
	float		p, n;

	if (!(e > 0))
		return (0);
	p = powf(e, 1 / m2);
	n = p - c1;
	if (n < 0)
		n = 0;
	return (10000.0f * powf(n / (c2 - c3 * p), 1 / m1) / SDR_WHITE_NITS);
}

/*
 * ITU-R BT.2100 hybrid log-gamma, shown as on a display of 1000 nits nominal
 * peak (system gamma 1.2): the inverse OETF, then the OOTF on the luminance
 * of the image's own primaries. A signal of 0.75 comes out at 203 nits.
 */
static void
hlg_to_lin(float c[3], int gamut)
{
	static const float	luma[3][3] = {
		{ 0.2126f, 0.7152f, 0.0722f },		/* sRGB */
		{ 0.2290f, 0.6917f, 0.0793f },		/* Display P3 */
		{ 0.2627f, 0.6780f, 0.0593f },		/* Rec. 2020 */
	};
	const float	a = 0.17883277f, b = 1 - 4 * a;
	const float	cc = 0.5f - a * logf(4 * a);
	const float	*k = luma[gamut == 1 || gamut == 2 ? gamut : 0];
	float		y;

	for (int i = 0; i < 3; i++) {
		float	e = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];

		c[i] = e <= 0.5f ? e * e / 3 : (expf((e - cc) / a) + b) / 12;
	}
	y = k[0] * c[0] + k[1] * c[1] + k[2] * c[2];
	for (int i = 0; i < 3; i++)
		c[i] = 1000.0f * (y > 0 ? powf(y, 0.2f) : 0) * c[i] /
		    SDR_WHITE_NITS;
}

/* ---- decoding the stored pixels ---- */

/*
 * The components of pixel (x, y) of a raw format as they were sent, each
 * 0-1, curve and gamut still theirs: what a program means by the bytes.
 * Packed gray and RGB565 are first the eight-bit values their specification
 * expands them to. The size has been checked (fmt_size).
 */
static void
px_fetch(long f, const unsigned char *raw, size_t w, size_t x, size_t y,
    const struct dv_palette *ipal, float c[4])
{
	const float	k8 = 1 / 255.0f;
	size_t		i = y * w + x;

	switch (fmt_kind(f)) {
	case FK_RGB:
		c[0] = raw[i * 3] * k8;
		c[1] = raw[i * 3 + 1] * k8;
		c[2] = raw[i * 3 + 2] * k8;
		c[3] = 1;
		return;
	case FK_RGBA:
		for (int k = 0; k < 4; k++)
			c[k] = raw[i * 4 + k] * k8;
		return;
	case FK_IDX8:
	case FK_IDX16: {
		int		two = fmt_kind(f) == FK_IDX16;
		uint32_t	rgb = idx_rgb(ipal, raw[i * (two ? 2 : 1)]);

		c[0] = ((rgb >> 16) & 0xff) * k8;
		c[1] = ((rgb >> 8) & 0xff) * k8;
		c[2] = (rgb & 0xff) * k8;
		c[3] = two ? raw[i * 2 + 1] * k8 : 1;
		return;
	}
	case FK_GRAY:
	case FK_PAL: {
		int		bits = fmt_bits(f);
		size_t		pal = fmt_kind(f) == FK_PAL ? (size_t)4 << bits : 0;
		size_t		row = (w * (size_t)bits + 7) / 8;
		size_t		bit = x * (size_t)bits;
		unsigned	mask = (1u << bits) - 1;
		unsigned	v = (raw[pal + y * row + bit / 8] >>
				    (8 - bits - bit % 8)) & mask;

		if (pal != 0) {
			for (int k = 0; k < 4; k++)
				c[k] = raw[v * 4 + k] * k8;
			return;
		}
		c[0] = c[1] = c[2] = (float)(v * (255 / mask)) * k8;
		c[3] = 1;
		return;
	}
	case FK_565: {
		unsigned	v = raw[i * 2] | ((unsigned)raw[i * 2 + 1] << 8);

		c[0] = (float)(((v >> 11) * 255 + 15) / 31) * k8;
		c[1] = (float)((((v >> 5) & 63) * 255 + 31) / 63) * k8;
		c[2] = (float)(((v & 31) * 255 + 15) / 31) * k8;
		c[3] = 1;
		return;
	}
	case FK_G16:
		c[0] = c[1] = c[2] = (float)(raw[i * 2] |
		    ((unsigned)raw[i * 2 + 1] << 8)) / 65535.0f;
		c[3] = 1;
		return;
	case FK_10A2: {
		uint32_t	v = raw[i * 4] | ((uint32_t)raw[i * 4 + 1] << 8) |
				    ((uint32_t)raw[i * 4 + 2] << 16) |
				    ((uint32_t)raw[i * 4 + 3] << 24);

		c[0] = (float)(v & 1023) / 1023.0f;
		c[1] = (float)((v >> 10) & 1023) / 1023.0f;
		c[2] = (float)((v >> 20) & 1023) / 1023.0f;
		c[3] = (float)(v >> 30) / 3.0f;
		return;
	}
	case FK_16A:
		for (int k = 0; k < 4; k++)
			c[k] = (float)(raw[i * 8 + k * 2] |
			    ((unsigned)raw[i * 8 + k * 2 + 1] << 8)) / 65535.0f;
		return;
	}
	c[0] = c[1] = c[2] = c[3] = 0;
}

/* The decoded bytes of a raw image and its keys, the size checked. */
static int
raw_of(const struct dv_kimage *im, long *f, std::vector<unsigned char> *raw)
{
	uint64_t	need;

	*f = key_val(im->fmt, strlen(im->fmt), 'f', 32);
	if (im->gpu || im->wpx == 0 || im->hpx == 0 ||
	    fmt_size(*f, im->wpx, im->hpx, &need) != 0 ||
	    !image_raw(im, raw) || raw->size() < need)
		return (0);
	return (1);
}

/* Is this image's pixels more than eight bits can hold, or another curve? */
static int
image_deep(const struct dv_kimage *im)
{
	long	f = key_val(im->fmt, strlen(im->fmt), 'f', 32);

	return (fmt_deep(f) || key_val(im->fmt, strlen(im->fmt), 'E', 0) != 0);
}

/*
 * A raw image as light: w*h*4 floats, straight alpha, in its own gamut, its
 * curve undone, 1.0 being SDR white. What f=64 holds, and what an image
 * that is deeper than eight bits or has another curve is made into.
 */
static int
image_linear(const struct dv_kimage *im, std::vector<float> *out)
{
	std::vector<unsigned char>	raw;
	long				f, E = key_val(im->fmt, strlen(im->fmt), 'E', 0);
	int				G = (int)key_val(im->fmt, strlen(im->fmt), 'G', 0);

	if (!raw_of(im, &f, &raw) || fmt_kind(f) == FK_F64 ||
	    fmt_kind(f) == FK_PNG || fmt_kind(f) == FK_VEC)
		return (0);
	if (!dv_kitty_fmt_gamut(f))
		G = 0;
	try {
		out->resize((size_t)im->wpx * im->hpx * 4);
	} catch (const std::bad_alloc &) {
		return (0);
	}
	for (size_t y = 0; y < im->hpx; y++)
		for (size_t x = 0; x < im->wpx; x++) {
			float	*c = out->data() + (y * im->wpx + x) * 4;

			px_fetch(f, raw.data(), im->wpx, x, y, im->pal, c);
			switch (E) {
			case 8:
				break;
			case 16:
				for (int k = 0; k < 3; k++)
					c[k] = pq_to_lin(c[k]);
				break;
			case 18:
				hlg_to_lin(c, G);
				break;
			default:
				for (int k = 0; k < 3; k++)
					c[k] = srgb_to_lin(c[k]);
				break;
			}
		}
	return (1);
}

/*
 * Decode a stored image back to RGBA, so it can be re-emitted as sixel
 * to a host that does not take kitty graphics, or expanded for one that
 * does not take a format. Handles every transmit format: f=32 (RGBA), f=24
 * (RGB), f=100 (PNG), kitty's indexed colour f=8 and f=16 (in the window's
 * colours), Silkie's packed gray f=301-308 and RGB565 f=516, its palette
 * images f=401-408 and its high precision f=316, 632 and 664, each
 * optionally zlib-deflated (o=z); a vector scene (f=200), drawn by the
 * rasterizer in src/vector; and light beyond eight bits -- f=64, or any
 * other curve the E key names -- clipped to what eight bits of sRGB show.
 * The colours stay in the image's gamut (G=). Returns a malloc'd w*h*4
 * buffer.
 */
int
dv_kimage_rgba(const struct dv_kimage *im, unsigned char **rgba, unsigned *w,
    unsigned *h)
{
	long				 f = key_val(im->fmt, strlen(im->fmt), 'f', 32);
	unsigned char			*px;
	std::vector<unsigned char>	 raw;
	size_t				 np;

	if (im->vscene != nullptr || f == 64) {
		/* Drawn here, or brought down to eight bits, at its own size. */
		if (im->gpu || im->wpx == 0 || im->hpx == 0)
			return (0);
		px = im->vscene != nullptr ? scene_raster(im, im->wpx, im->hpx) :
		    f64_rgba(im);
		if (px == nullptr)
			return (0);
		*rgba = px;
		*w = im->wpx;
		*h = im->hpx;
		return (1);
	}
	if (f == 100) {			/* PNG */
		if (!image_raw(im, &raw))
			return (0);
		return (dv_png_decode(raw.data(), raw.size(), rgba, w, h));
	}
	if (im->wpx == 0 || im->hpx == 0)
		return (0);
	np = (size_t)im->wpx * im->hpx;
	if (image_deep(im)) {
		std::vector<float>	lin;

		if (!image_linear(im, &lin))
			return (0);
		px = (unsigned char *)malloc(np * 4);
		if (px == nullptr)
			return (0);
		for (size_t i = 0; i < np; i++) {
			for (int k = 0; k < 3; k++)
				px[i * 4 + k] = (unsigned char)(lin_to_srgb(lin[i * 4 + k]) *
				    255.0f + 0.5f);
			px[i * 4 + 3] = (unsigned char)((lin[i * 4 + 3] < 0 ? 0 :
			    lin[i * 4 + 3] > 1 ? 1 : lin[i * 4 + 3]) * 255.0f + 0.5f);
		}
		*rgba = px;
		*w = im->wpx;
		*h = im->hpx;
		return (1);
	}
	if (!raw_of(im, &f, &raw))
		return (0);
	px = (unsigned char *)malloc(np * 4);
	if (px == nullptr)
		return (0);
	if (f == 32)
		memcpy(px, raw.data(), np * 4);
	else if (f == 24)
		for (size_t i = 0; i < np; i++) {
			memcpy(px + i * 4, raw.data() + i * 3, 3);
			px[i * 4 + 3] = 255;
		}
	else
		for (size_t y = 0; y < im->hpx; y++)
			for (size_t x = 0; x < im->wpx; x++) {
				float		 c[4];
				unsigned char	*o = px + (y * im->wpx + x) * 4;

				px_fetch(f, raw.data(), im->wpx, x, y, im->pal, c);
				for (int k = 0; k < 4; k++)
					o[k] = (unsigned char)(c[k] * 255.0f + 0.5f);
			}
	*rgba = px;
	*w = im->wpx;
	*h = im->hpx;
	return (1);
}

/* Pixel size of a stored image: from s=/v=, or a PNG's IHDR. */
static void
image_pixels(struct dv_kimage *im)
{
	long		sv = key_val(im->fmt, strlen(im->fmt), 's', -1);
	long		vv = key_val(im->fmt, strlen(im->fmt), 'v', -1);
	unsigned char	hdr[32];
	size_t		got;

	im->wpx = im->hpx = 0;
	if (im->gpu) {
		if (sv > 0 && vv > 0) {
			im->wpx = (unsigned)sv;
			im->hpx = (unsigned)vv;
		}
		return;
	}
	if (sv > 0 && vv > 0) {		/* raw RGB/RGBA carry their size */
		im->wpx = (unsigned)sv;
		im->hpx = (unsigned)vv;
		return;
	}
	got = b64_decode((const char *)im->data.data,
	    im->data.len < 64 ? im->data.len : 64, hdr, sizeof hdr);
	if (got >= 24 && hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' &&
	    hdr[3] == 'G') {
		im->wpx = ((unsigned)hdr[16] << 24) | ((unsigned)hdr[17] << 16) |
		    ((unsigned)hdr[18] << 8) | hdr[19];
		im->hpx = ((unsigned)hdr[20] << 24) | ((unsigned)hdr[21] << 16) |
		    ((unsigned)hdr[22] << 8) | hdr[23];
	}
}

static void	packs_free(struct dv_kimage *);
static int	patch_changes_root(const std::string &);

/* ---- vector scenes, session fonts ---- */

/*
 * One record of a scene: its line as the terminal will get it, with session
 * font handles and image ids already ours. parent is the group it is in.
 */
struct dv_vrec {
	char			 type = 0;
	int			 has_id = 0;
	int			 dead = 0;
	uint32_t		 id = 0;
	uint32_t		 parent = 0;
	std::string		 line;
	std::vector<unsigned>	 fonts;		/* serials of the fonts it names */
};

struct dv_vscene {
	std::vector<struct dv_vrec>		 recs;	/* in drawing order */
	std::unordered_map<uint32_t, size_t>	 byid;
	size_t					 ndead = 0;
	char					 fmt[96] = "";	/* to restore on a bad resend */
	uint64_t				 serial = 0;	/* which transmission */
	/* The scene as the rasterizer holds it, for a terminal that cannot
	 * draw it itself: made when first wanted, and again once the image's
	 * generation is no longer the one it was made of. */
	VectorDoc				*doc = nullptr;
	unsigned				 doc_gen = 0;
};

/*
 * A font the program uploaded. It is kept while its name resolves to it or
 * any record of a scene uses it, since a scene is shaped again by every
 * client it is sent to. A released name fails for new layouts here, without
 * the terminal having to give up what retained text was shaped with.
 */
struct dv_vfont {
	std::string	 name;
	unsigned	 serial = 0;	/* the terminal knows it as dvf<serial> */
	std::string	 body;		/* the upload, under that name */
	int		 live = 0;	/* the name resolves to this one */
	int		 pending = 0;	/* the terminal has not accepted it yet */
	unsigned	 refs = 0;	/* records that use it */
};

/* Per window: its fonts, and requests whose chunks are still arriving. */
struct dv_vwin {
	std::vector<struct dv_vfont>	 fonts;
	int				 v_active = 0;
	long				 v_id = 0, v_q = 0;
	int				 v_zed = 0;
	struct dv_buf			 v_data = {};
	int				 e_active = 0;
	long				 e_id = 0, e_q = 0;
	int				 e_zed = 0;
	struct dv_buf			 e_data = {};
	/* A palette update's slots (x, w; -1 absent), gamut (G, -1
	 * absent) and whether its data came by the direct medium. */
	long				 e_x = -1, e_w = -1, e_G = -1;
	int				 e_direct = 1;
};

/* A request the terminal owes an answer to, and whose it is. */
struct dv_vreq {
	unsigned	 hid;
	unsigned	 win;
	long		 id;
	long		 q;
	int		 fd;
	unsigned	 serial;	/* an upload: the font it would be */
	int		 fonts_op;
	time_t		 when;
};

/* Past this a program has long since timed out; only the route is dropped. */
#define VREQ_STALE_SECS	600

/* Kitty's chunk size is 4096 and base64 comes in fours. */
#define VCHUNK		4096

static struct dv_window *
vreq_window(struct dv_session *s, unsigned id)
{
	struct dv_window	*w;

	TAILQ_FOREACH(w, &s->desktop.windows, entry)
		if (w->id == id)
			return (w);
	return (nullptr);
}

static struct dv_vwin *
vwin(struct dv_window *w)
{
	if (w->vwin == nullptr)
		w->vwin = new dv_vwin;
	return (w->vwin);
}

static std::string
host_handle(unsigned serial)
{
	return ("dvf" + std::to_string(serial));
}

static struct dv_vfont *
font_serial(struct dv_window *w, unsigned serial)
{
	if (w->vwin != nullptr)
		for (auto &f : w->vwin->fonts)
			if (f.serial == serial)
				return (&f);
	return (nullptr);
}

static struct dv_vfont *
font_live(struct dv_window *w, const std::string &name)
{
	if (w->vwin != nullptr)
		for (auto &f : w->vwin->fonts)
			if (f.live && f.name == name)
				return (&f);
	return (nullptr);
}

/*
 * Fonts leaving a window's list. Each was also given to the text layout
 * here, under its handle, for scenes drawn on this side: it leaves there
 * with it.
 */
template <typename Gone>
static void
fonts_erase(struct dv_vwin *vw, Gone gone)
{
	std::erase_if(vw->fonts, [&gone](const struct dv_vfont &f) {
		if (!gone(f))
			return (false);
		dv_vtext_release(host_handle(f.serial));
		return (true);
	});
}

static void
font_gc(struct dv_window *w)
{
	if (w->vwin != nullptr)
		fonts_erase(w->vwin, [](const struct dv_vfont &f) {
			return (!f.live && !f.pending && f.refs == 0);
		});
}

static void
refs_add(struct dv_window *w, const std::vector<unsigned> &fonts, int delta)
{
	for (unsigned serial : fonts) {
		struct dv_vfont	*f = font_serial(w, serial);

		if (f != nullptr)
			f->refs = (unsigned)((int)f->refs + delta);
	}
}

/* -- JSON, as far as handles go: no tree, no recursion, any depth. -- */

static size_t
json_ws(const std::string &s, size_t i)
{
	while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
	    s[i] == '\r'))
		i++;
	return (i);
}

/* i at the opening quote; the index past the closing one, npos if none. */
static size_t
json_str_end(const std::string &s, size_t i)
{
	for (i++; i < s.size(); i++) {
		if (s[i] == '\\')
			i++;
		else if (s[i] == '"')
			return (i + 1);
	}
	return (std::string::npos);
}

static void
utf8_append(std::string *o, unsigned cp)
{
	if (cp < 0x80)
		*o += (char)cp;
	else if (cp < 0x800) {
		*o += (char)(0xc0 | (cp >> 6));
		*o += (char)(0x80 | (cp & 0x3f));
	} else if (cp < 0x10000) {
		*o += (char)(0xe0 | (cp >> 12));
		*o += (char)(0x80 | ((cp >> 6) & 0x3f));
		*o += (char)(0x80 | (cp & 0x3f));
	} else {
		*o += (char)(0xf0 | (cp >> 18));
		*o += (char)(0x80 | ((cp >> 12) & 0x3f));
		*o += (char)(0x80 | ((cp >> 6) & 0x3f));
		*o += (char)(0x80 | (cp & 0x3f));
	}
}

static int
hex4(const std::string &s, size_t i, unsigned *cp)
{
	unsigned	v = 0;

	if (i + 4 > s.size())
		return (0);
	for (size_t k = i; k < i + 4; k++) {
		char	c = s[k];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (unsigned)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (unsigned)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (unsigned)(c - 'A' + 10);
		else
			return (0);
	}
	*cp = v;
	return (1);
}

/* The string from its opening quote i to end (past the closing quote). */
static std::string
json_str_value(const std::string &s, size_t i, size_t end)
{
	std::string	o;

	for (i++; i + 1 < end; i++) {
		unsigned	cp, lo;

		if (s[i] != '\\') {
			o += s[i];
			continue;
		}
		i++;
		switch (s[i]) {
		case 'b': o += '\b'; break;
		case 'f': o += '\f'; break;
		case 'n': o += '\n'; break;
		case 'r': o += '\r'; break;
		case 't': o += '\t'; break;
		case 'u':
			if (!hex4(s, i + 1, &cp))
				break;
			i += 4;
			if (cp >= 0xd800 && cp < 0xdc00 && i + 2 < end &&
			    s[i + 1] == '\\' && s[i + 2] == 'u' &&
			    hex4(s, i + 3, &lo) && lo >= 0xdc00 && lo < 0xe000) {
				cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
				i += 6;
			}
			utf8_append(&o, cp);
			break;
		default:
			o += s[i];
			break;
		}
	}
	return (o);
}

/* The inside of a string that holds this text. */
static std::string
json_escape(const std::string &v)
{
	std::string	o;

	for (unsigned char c : v) {
		char	t[8];

		if (c == '"' || c == '\\') {
			o += '\\';
			o += (char)c;
		} else if (c < 0x20) {
			snprintf(t, sizeof t, "\\u%04x", c);
			o += t;
		} else
			o += (char)c;
	}
	return (o);
}

/* Past the value at i, of any kind and depth; npos if it does not end. */
static size_t
json_value_end(const std::string &s, size_t i)
{
	size_t	depth = 0;

	i = json_ws(s, i);
	while (i < s.size()) {
		char	c = s[i];

		if (c == '"') {
			i = json_str_end(s, i);
			if (i == std::string::npos)
				return (i);
			if (depth == 0)
				return (i);
			continue;
		}
		if (c == '{' || c == '[') {
			depth++;
			i++;
			continue;
		}
		if (c == '}' || c == ']') {
			if (depth == 0)
				return (std::string::npos);
			i++;
			if (--depth == 0)
				return (i);
			continue;
		}
		if (depth == 0) {
			while (i < s.size() && s[i] != ',' && s[i] != '}' &&
			    s[i] != ']' && s[i] != ' ' && s[i] != '\n' &&
			    s[i] != '\r' && s[i] != '\t')
				i++;
			return (i);
		}
		i++;
	}
	return (std::string::npos);
}

/* Where the top level member's value starts and ends. */
static int
json_top_member(const std::string &s, const char *key, size_t *vs, size_t *ve)
{
	size_t	i = json_ws(s, 0);

	if (i >= s.size() || s[i] != '{')
		return (0);
	for (i = json_ws(s, i + 1); i < s.size() && s[i] == '"';) {
		size_t	ke = json_str_end(s, i), vb, vend;

		if (ke == std::string::npos)
			return (0);
		vb = json_ws(s, ke);
		if (vb >= s.size() || s[vb] != ':')
			return (0);
		vb = json_ws(s, vb + 1);
		vend = json_value_end(s, vb);
		if (vend == std::string::npos)
			return (0);
		if (json_str_value(s, i, ke) == key) {
			*vs = vb;
			*ve = vend;
			return (1);
		}
		i = json_ws(s, vend);
		if (i >= s.size() || s[i] != ',')
			return (0);
		i = json_ws(s, i + 1);
	}
	return (0);
}

static int
json_top_string(const std::string &s, const char *key, std::string *out)
{
	size_t	vs, ve;

	if (!json_top_member(s, key, &vs, &ve) || vs >= s.size() || s[vs] != '"')
		return (0);
	*out = json_str_value(s, vs, ve);
	return (1);
}

/*
 * Copy a layout, or any request that selects fonts, with each "@name" given
 * as a "font" or in a "fallback" array put in the terminal's terms. A name
 * this window has not uploaded is made one no window can have. fonts gets
 * the serials used; unknown says a name did not resolve.
 */
static std::string
json_handles(struct dv_window *w, const std::string &in,
    std::vector<unsigned> *fonts, std::string *unknown)
{
	struct level {
		char	kind;
		int	fallback;
		int	expect_key;
	};
	std::vector<struct level>	 st;
	std::string			 out, key;
	size_t				 i = 0;

	out.reserve(in.size());
	while (i < in.size()) {
		char	c = in[i];

		if (c == '"') {
			size_t		end = json_str_end(in, i);
			struct level	*top = st.empty() ? nullptr : &st.back();

			if (end == std::string::npos)
				break;
			if (top != nullptr && top->kind == '{' && top->expect_key) {
				key = json_str_value(in, i, end);
				top->expect_key = 0;
				out.append(in, i, end - i);
			} else {
				std::string	v = json_str_value(in, i, end);

				if ((key == "font" || (top != nullptr && top->fallback)) &&
				    v.size() > 1 && v[0] == '@') {
					struct dv_vfont	*f = font_live(w, v.substr(1));

					if (f != nullptr) {
						out += "\"@" + host_handle(f->serial) + "\"";
						if (fonts != nullptr)
							fonts->push_back(f->serial);
					} else {
						out += "\"@dvu." + json_escape(v.substr(1)) + "\"";
						if (unknown != nullptr)
							*unknown = v;
					}
				} else
					out.append(in, i, end - i);
				key.clear();
			}
			i = end;
			continue;
		}
		if (c == '{' || c == '[') {
			st.push_back({ c, c == '[' && key == "fallback", c == '{' });
			key.clear();
		} else if (c == '}' || c == ']') {
			if (!st.empty())
				st.pop_back();
			key.clear();
		} else if (c == ',') {
			if (!st.empty() && st.back().kind == '{')
				st.back().expect_key = 1;
			key.clear();
		}
		out += c;
		i++;
	}
	out.append(in, i, std::string::npos);
	return (out);
}

/* A reply in the program's terms: our names for its fonts become its own. */
static std::string
json_unmask(struct dv_window *w, const std::string &in)
{
	std::string	out;
	size_t		i = 0;

	out.reserve(in.size());
	while (i < in.size()) {
		if (in.compare(i, 4, "dvu.") == 0) {
			i += 4;
			continue;
		}
		if (in.compare(i, 3, "dvf") == 0) {
			size_t		k = i + 3;
			unsigned long	v = 0;

			while (k < in.size() && in[k] >= '0' && in[k] <= '9' &&
			    v <= UINT32_MAX)
				v = v * 10 + (unsigned long)(in[k++] - '0');
			if (k > i + 3 && v <= UINT32_MAX) {
				struct dv_vfont	*f = font_serial(w, (unsigned)v);

				if (f != nullptr) {
					out += json_escape(f->name);
					i = k;
					continue;
				}
			}
		}
		out += in[i++];
	}
	return (out);
}

/* The fonts op lists every upload the terminal has: keep this window's. */
static std::string
json_filter_session(struct dv_window *w, const std::string &in)
{
	size_t		vs, ve, i;
	std::string	kept;

	if (!json_top_member(in, "session", &vs, &ve) || in[vs] != '[')
		return (in);
	kept = "[";
	for (i = json_ws(in, vs + 1); i < ve && in[i] != ']';) {
		size_t		end = json_value_end(in, i);
		std::string	el, id;

		if (end == std::string::npos)
			return (in);
		el = in.substr(i, end - i);
		if (json_top_string(el, "id", &id) && id.compare(0, 3, "dvf") == 0) {
			struct dv_vfont	*f = font_serial(w,
			    (unsigned)strtoul(id.c_str() + 3, nullptr, 10));

			if (f != nullptr && f->live) {
				if (kept.size() > 1)
					kept += ',';
				kept += el;
			}
		}
		i = json_ws(in, end);
		if (i < ve && in[i] == ',')
			i = json_ws(in, i + 1);
	}
	kept += ']';
	return (in.substr(0, vs) + kept + in.substr(ve));
}

/* -- records -- */

static std::string
b64_str(const std::string &in)
{
	struct dv_buf	b;
	std::string	o;

	memset(&b, 0, sizeof b);
	b64_encode((const unsigned char *)in.data(), in.size(), &b);
	if (b.len > 0)
		o.assign((const char *)b.data, b.len);
	dv_buf_free(&b);
	return (o);
}

/* A payload in text: base64, and inflated if it was sent deflated. */
static int
payload_text(const unsigned char *d, size_t n, int zed, std::string *out)
{
	std::string	raw(n, '\0');
	size_t		got = b64_decode((const char *)d, n,
	    (unsigned char *)raw.data(), n);

	raw.resize(got);
	if (zed) {
		unsigned char	*dec = nullptr;
		size_t		 declen = 0;

		if (!inflate_grow((const unsigned char *)raw.data(), raw.size(),
		    &dec, &declen))
			return (0);
		out->assign((const char *)dec, declen);
		free(dec);
	} else
		*out = std::move(raw);
	return (1);
}

/* The value of key= in a record, whose fields are split by single spaces. */
static int
rec_field(std::string_view line, const char *key, size_t *vs, size_t *ve)
{
	size_t	kl = strlen(key), i = 1;

	while (i < line.size()) {
		size_t	end;

		i++;
		end = line.find(' ', i);
		if (end == std::string_view::npos)
			end = line.size();
		if (end - i > kl && line.compare(i, kl, key) == 0 && line[i + kl] == '=') {
			*vs = i + kl + 1;
			*ve = end;
			return (1);
		}
		i = end;
	}
	return (0);
}

static int
rec_u32(std::string_view line, const char *key, uint32_t *out)
{
	size_t			vs, ve;
	unsigned long long	v = 0;

	if (!rec_field(line, key, &vs, &ve) || vs == ve)
		return (0);
	for (; vs < ve; vs++) {
		if (line[vs] < '0' || line[vs] > '9' || v > UINT32_MAX)
			return (0);
		v = v * 10 + (unsigned long long)(line[vs] - '0');
	}
	if (v > UINT32_MAX)
		return (0);
	*out = (uint32_t)v;
	return (1);
}

static unsigned res_id(struct dv_window *, unsigned child);

/* A line into a record. 0 and a reason if it is malformed. */
static int
rec_parse(struct dv_window *w, std::string_view line, struct dv_vrec *r,
    std::string *why)
{
	size_t		vs, ve;
	uint32_t	v;

	if (line.size() > 1 && line[1] != ' ') {
		*why = "a record type is one character";
		return (0);
	}
	r->type = line[0];
	r->line = line;
	if (rec_field(line, "id", &vs, &ve)) {
		if (!rec_u32(line, "id", &r->id)) {
			*why = "bad id";
			return (0);
		}
		r->has_id = 1;
	}
	switch (r->type) {
	case 'g':
		if (rec_u32(line, "p", &v))
			r->parent = v;
		[[fallthrough]];
	case 'p': case 's': case 'u': case 'k': case 'l': case 'r':
	case 't': case 'i':
		if (!r->has_id) {
			*why = "an element needs an id";
			return (0);
		}
		break;
	case 'a':
		if (!r->has_id || (!rec_field(line, "d", &vs, &ve) &&
		    !rec_field(line, "pts", &vs, &ve))) {
			*why = "an append needs an id and geometry";
			return (0);
		}
		break;
	case 'x':
		if (!r->has_id && !rec_field(line, "g", &vs, &ve) &&
		    line.find(" all") == std::string_view::npos) {
			*why = "a delete needs an id, a group or all";
			return (0);
		}
		break;
	default:
		break;			/* a later version's: carried as it is */
	}
	if (r->type != 'g' && rec_u32(line, "g", &v))
		r->parent = v;
	if (r->type == 't' && rec_field(line, "text", &vs, &ve)) {
		std::string	layout, unknown, mapped;
		std::string	tail(line.substr(ve));

		if (!payload_text((const unsigned char *)line.data() + vs, ve - vs,
		    0, &layout)) {
			*why = "bad text";
			return (0);
		}
		mapped = json_handles(w, layout, &r->fonts, &unknown);
		if (!unknown.empty()) {
			*why = "unknown font " + unknown;
			return (0);
		}
		if (!r->fonts.empty())
			r->line = std::string(line.substr(0, vs)) + b64_str(mapped) + tail;
	}
	if (r->type == 'i' && rec_u32(line, "img", &v) && v != 0 &&
	    rec_field(line, "img", &vs, &ve))
		r->line = std::string(line.substr(0, vs)) +
		    std::to_string(res_id(w, v)) + std::string(line.substr(ve));
	return (1);
}

/* All the lines of a payload, or none: a=t and a=e are applied whole. */
static int
recs_parse(struct dv_window *w, const std::string &text,
    std::vector<struct dv_vrec> *recs, std::string *err)
{
	size_t	i = 0, lineno = 0;

	while (i <= text.size()) {
		size_t	nl = text.find('\n', i);
		size_t	end = nl == std::string::npos ? text.size() : nl;
		size_t	e = end;

		lineno++;
		if (e > i && text[e - 1] == '\r')
			e--;
		if (e > i) {
			struct dv_vrec	r;
			std::string	why;

			if (!rec_parse(w, std::string_view(text).substr(i, e - i), &r,
			    &why)) {
				*err = "line " + std::to_string(lineno) + ": " + why;
				return (0);
			}
			recs->push_back(std::move(r));
		}
		if (nl == std::string::npos)
			break;
		i = nl + 1;
	}
	return (1);
}

static void
scene_kill(struct dv_window *w, struct dv_vscene *sc, size_t i)
{
	struct dv_vrec	*r = &sc->recs[i];

	if (r->dead)
		return;
	refs_add(w, r->fonts, -1);
	if (r->has_id)
		sc->byid.erase(r->id);
	r->dead = 1;
	r->line.clear();
	r->line.shrink_to_fit();
	r->fonts.clear();
	sc->ndead++;
}

/* Dead records hold a place until they outnumber the living. */
static void
scene_compact(struct dv_vscene *sc)
{
	size_t	o = 0;

	if (sc->ndead < 64 || sc->ndead * 2 < sc->recs.size())
		return;
	sc->byid.clear();
	for (size_t i = 0; i < sc->recs.size(); i++) {
		if (sc->recs[i].dead)
			continue;
		if (o != i)
			sc->recs[o] = std::move(sc->recs[i]);
		if (sc->recs[o].has_id)
			sc->byid[sc->recs[o].id] = o;
		o++;
	}
	sc->recs.resize(o);
	sc->ndead = 0;
}

/* Everything whose chain of parents leads to the group, the group too. */
static void
scene_kill_group(struct dv_window *w, struct dv_vscene *sc, uint32_t group)
{
	std::unordered_map<uint32_t, int>	 known;	/* group: is it inside? */
	std::vector<uint32_t>			 path;

	known[group] = 1;
	for (size_t i = 0; i < sc->recs.size(); i++) {
		struct dv_vrec	*r = &sc->recs[i];
		uint32_t	 at;
		int		 in = 0;

		if (r->dead || (r->has_id && r->id == group && r->type == 'g'))
			continue;
		if (r->type == 'a' || r->type == 's' || r->type == 'l' || r->type == 'r')
			continue;	/* not in a group */
		path.clear();
		for (at = r->parent;;) {
			auto k = known.find(at);

			if (k != known.end()) {
				in = k->second;
				break;
			}
			if (at == 0) {
				in = 0;
				break;
			}
			auto g = sc->byid.find(at);

			if (g == sc->byid.end() || sc->recs[g->second].type != 'g') {
				in = 0;
				break;
			}
			/* A cycle leads nowhere; mark it before walking on. */
			known[at] = 0;
			path.push_back(at);
			at = sc->recs[g->second].parent;
		}
		for (uint32_t id : path)
			known[id] = in;
		/* Before it goes from the index: its own children ask. */
		if (r->type == 'g')
			known[r->id] = in;
		if (in)
			scene_kill(w, sc, i);
	}
	auto g = sc->byid.find(group);

	if (g != sc->byid.end())
		scene_kill(w, sc, g->second);
}

static void
scene_apply(struct dv_window *w, struct dv_vscene *sc, struct dv_vrec &&r)
{
	size_t		vs, ve;
	uint32_t	v;

	if (r.type == 'x') {
		if (r.has_id) {
			auto it = sc->byid.find(r.id);

			if (it != sc->byid.end())
				scene_kill(w, sc, it->second);
		} else if (rec_u32(r.line, "g", &v))
			scene_kill_group(w, sc, v);
		else
			for (size_t i = 0; i < sc->recs.size(); i++)
				scene_kill(w, sc, i);
		scene_compact(sc);
		return;
	}
	if (r.type == 'a') {
		auto it = sc->byid.find(r.id);

		if (it == sc->byid.end())
			return;
		std::string	*to = &sc->recs[it->second].line;
		size_t		 ts, te;

		if (rec_field(r.line, "pts", &vs, &ve) &&
		    rec_field(*to, "pts", &ts, &te)) {
			to->insert(te, "," + r.line.substr(vs, ve - vs));
		} else if (rec_field(r.line, "d", &vs, &ve) &&
		    rec_field(*to, "d", &ts, &te))
			to->insert(te, r.line.substr(vs, ve - vs));
		return;
	}
	refs_add(w, r.fonts, 1);
	if (r.has_id) {
		auto it = sc->byid.find(r.id);

		if (it != sc->byid.end()) {	/* keeps its place */
			refs_add(w, sc->recs[it->second].fonts, -1);
			sc->recs[it->second] = std::move(r);
			return;
		}
		sc->byid[r.id] = sc->recs.size();
	}
	sc->recs.push_back(std::move(r));
}

/* The table as the one a=t that a fresh terminal needs. */
static void
scene_render(const struct dv_vscene *sc, struct dv_buf *out)
{
	std::string	text;

	for (const auto &r : sc->recs) {
		if (r.dead)
			continue;
		text += r.line;
		text += '\n';
	}
	/* An empty scene must still exist there: edits are coming. */
	if (text.empty())
		text = "\n";
	out->len = 0;
	b64_encode((const unsigned char *)text.data(), text.size(), out);
}

static void
scene_free(struct dv_window *w, struct dv_kimage *im)
{
	if (im->vscene != nullptr && im->vscene->doc != nullptr) {
		vector_doc_free(im->vscene->doc);
		im->vscene->doc = nullptr;
	}
	if (im->vscene == nullptr)
		return;
	for (const auto &r : im->vscene->recs)
		if (!r.dead)
			refs_add(w, r.fonts, -1);
	delete im->vscene;
	im->vscene = nullptr;
}

/* How many records the table holds, dead ones too. */
size_t
dv_kitty_scene_slots(const struct dv_kimage *im)
{
	return (im->vscene != nullptr ? im->vscene->recs.size() : 0);
}

/* A completed transmission of f=200 replaces the table. 0 keeps the old. */
static int
scene_load(struct dv_window *w, struct dv_kimage *im, std::string *err)
{
	std::vector<struct dv_vrec>	 recs;
	std::string			 text;
	struct dv_vscene		*sc;
	char				*z = strstr(im->fmt, ",o=z");
	char				 kept[sizeof im->fmt];
	static uint64_t			 serial;

	if (!payload_text(im->data.data, im->data.len, z != nullptr, &text)) {
		*err = "cannot inflate";
		return (0);
	}
	if (!recs_parse(w, text, &recs, err))
		return (0);
	if (z != nullptr)
		*z = '\0';
	dv_strlcpy(kept, im->fmt, sizeof kept);
	scene_free(w, im);
	sc = new dv_vscene;
	sc->serial = ++serial;
	dv_strlcpy(sc->fmt, kept, sizeof sc->fmt);
	for (auto &r : recs)
		scene_apply(w, sc, std::move(r));
	im->vscene = sc;
	im->data.len = 0;		/* rendered when a client needs it */
	font_gc(w);
	return (1);
}

/* -- requests to the terminal -- */

static unsigned
vreq_id(void)
{
	static unsigned	next = DV_VREQ_FIRST;
	unsigned	id = next;

	next = next == DV_VREQ_LAST ? DV_VREQ_FIRST : next + 1;
	return (id);
}

/* a=v for the terminal, in chunks. q=2 says no answer is wanted. */
static void
vreq_frames(const std::string &json, unsigned hid, int quiet, std::string *out)
{
	std::string	b64 = b64_str(json);
	size_t		off = 0;

	do {
		size_t	n = b64.size() - off > VCHUNK ? VCHUNK : b64.size() - off;
		int	more = off + n < b64.size();
		char	hdr[64];

		if (off == 0)
			snprintf(hdr, sizeof hdr, "\033_Ga=v,f=200,i=%u,q=%d,m=%d;", hid,
			    quiet ? 2 : 0, more);
		else
			snprintf(hdr, sizeof hdr, "\033_Gm=%d;", more);
		*out += hdr;
		out->append(b64, off, n);
		*out += "\033\\";
		off += n;
	} while (off < b64.size());
}

static void
v_reply(struct dv_window *w, long id, const std::string &msg)
{
	std::string	r = "\033_Gi=" + std::to_string(id) + ";" + msg + "\033\\";

	dv_window_pty_send(w, r.data(), r.size());
}

static void
v_error(struct dv_window *w, long id, long q, const std::string &what)
{
	if (q < 2)
		v_reply(w, id, "EINVAL:" + b64_str("{\"error\":\"" +
		    json_escape(what) + "\"}"));
}

static void
vreq_purge(struct dv_session *s, time_t now)
{
	if (s->vreqs == nullptr)
		return;
	std::erase_if(*s->vreqs, [s, now](const struct dv_vreq &r) {
		if (now - r.when < VREQ_STALE_SECS)
			return (false);
		if (r.serial != 0) {
			struct dv_window	*w = vreq_window(s, r.win);

			if (w != nullptr && w->vwin != nullptr)
				fonts_erase(w->vwin,
				    [&r](const struct dv_vfont &f) {
					return (f.pending && f.serial == r.serial);
				});
		}
		return (true);
	});
}

/* The whole request has arrived: answer it here, or ask the terminal. */
static void
vreq_done(struct dv_window *w)
{
	struct dv_vwin		*vw = w->vwin;
	struct dv_session	*s = w->session;
	std::string		 json, op, name, body, frames;
	long			 id = vw->v_id, q = vw->v_q;
	unsigned		 serial = 0, hid;
	int			 fd;

	vw->v_active = 0;
	if (!payload_text(vw->v_data.data, vw->v_data.len, vw->v_zed, &json)) {
		vw->v_data.len = 0;
		v_error(w, id, q, "cannot inflate the request");
		return;
	}
	vw->v_data.len = 0;
	/* With no terminal attached that draws scenes itself there is none to
	 * ask: scenes are drawn here, so here is where their text is set and
	 * measured, and the answer is the one that matches what is drawn. */
	const bool	local = s == nullptr || !s->silk_vector;

	(void)json_top_string(json, "op", &op);
	if (op == "release") {
		struct dv_vfont	*f;

		if (!json_top_string(json, "id", &name) ||
		    (f = font_live(w, name)) == nullptr) {
			v_error(w, id, q, "no such font");
			return;
		}
		f->live = 0;
		font_gc(w);
		if (q < 1)
			v_reply(w, id, "OK;" + b64_str("{\"id\":\"" +
			    json_escape(name) + "\"}"));
		return;
	}
	if (op == "upload") {
		static unsigned	 next_serial;
		size_t		 vs, ve;
		struct dv_vfont	 f;

		if (!json_top_string(json, "id", &name) || name.empty() ||
		    !json_top_member(json, "id", &vs, &ve)) {
			v_error(w, id, q, "an upload needs an id");
			return;
		}
		std::string	 info, why;

		serial = ++next_serial;
		/* Kept here as well, whoever else is sent it: a scene set in
		 * it may be drawn on this side for some terminal. With a
		 * terminal to ask, whether it is a font is that terminal's
		 * to say; with none, it is said here. */
		if (!dv_vtext_upload(host_handle(serial), json, &info, &why) && local) {
			v_error(w, id, q, why);
			return;
		}
		f.name = name;
		f.serial = serial;
		f.pending = 1;
		f.body = json.substr(0, vs) + "\"" + host_handle(serial) + "\"" +
		    json.substr(ve);
		body = f.body;
		if (local) {
			struct dv_vfont	*old = font_live(w, name);

			if (old != nullptr)
				old->live = 0;
			f.pending = 0;
			f.live = 1;
			vw->fonts.push_back(std::move(f));
			font_gc(w);
			if (q < 1)
				v_reply(w, id, "OK;" + b64_str("{\"id\":\"" +
				    json_escape(name) + "\"," + info + "}"));
			return;
		}
		vw->fonts.push_back(std::move(f));
	} else
		body = json_handles(w, json, nullptr, nullptr);
	if (local) {
		std::vector<std::pair<std::string, std::string>>	session;
		std::string						result, why;

		for (const auto &f : vw->fonts)
			if (f.live)
				session.push_back({ f.name, host_handle(f.serial) });
		if (!dv_vtext_service(body, session, &result, &why))
			v_error(w, id, q, why);
		else if (q < 1)
			v_reply(w, id, "OK;" + b64_str(result));
		return;
	}
	hid = vreq_id();
	vreq_frames(body, hid, 0, &frames);
	fd = dv_kitty_send_primary(s, frames.data(), frames.size());
	if (fd < 0) {
		if (serial != 0)
			fonts_erase(vw, [serial](const struct dv_vfont &f) {
				return (f.serial == serial);
			});
		v_error(w, id, q, "no attached terminal has vector images");
		return;
	}
	if (s->vreqs == nullptr)
		s->vreqs = new std::vector<struct dv_vreq>;
	vreq_purge(s, time(nullptr));
	s->vreqs->push_back({ hid, w->id, id, q, fd, serial, op == "fonts",
	    time(nullptr) });
}

/* An answer in a window's terms, to the window. */
static void
vreq_answer(struct dv_window *w, const struct dv_vreq *r, const std::string &msg)
{
	std::string	 out;
	size_t		 colon;

	if (msg.compare(0, 2, "OK") == 0) {
		if (r->q >= 1)
			return;
		if (msg.size() > 3 && msg[2] == ';') {
			std::string	text;

			if (!payload_text((const unsigned char *)msg.data() + 3,
			    msg.size() - 3, 0, &text))
				return;
			if (r->fonts_op)
				text = json_filter_session(w, text);
			out = "OK;" + b64_str(json_unmask(w, text));
		} else
			out = "OK";
		v_reply(w, r->id, out);
		return;
	}
	if (r->q >= 2)
		return;
	colon = msg.find(':');
	if (colon != std::string::npos && colon + 1 < msg.size()) {
		std::string	text;

		if (payload_text((const unsigned char *)msg.data() + colon + 1,
		    msg.size() - colon - 1, 0, &text) && !text.empty() &&
		    text[0] == '{') {
			v_reply(w, r->id, msg.substr(0, colon + 1) +
			    b64_str(json_unmask(w, text)));
			return;
		}
	}
	v_reply(w, r->id, msg);
}

void
dv_kitty_host_reply(struct dv_peer *p, const unsigned char *cap, size_t len)
{
	struct dv_session	*s = p->session;
	const char		*c = (const char *)cap;
	size_t			 i;
	long			 hid;

	if (s == nullptr || s->vreqs == nullptr || len < 1 || c[0] != 'G')
		return;
	for (i = 1; i < len && c[i] != ';'; i++)
		;
	hid = key_val(c + 1, i - 1, 'i', 0);
	if (i >= len)
		return;
	std::string	msg(c + i + 1, len - i - 1);

	for (size_t k = 0; k < s->vreqs->size(); k++) {
		struct dv_vreq		 r = (*s->vreqs)[k];
		struct dv_window	*w;

		if ((long)r.hid != hid || r.fd != p->fd)
			continue;
		s->vreqs->erase(s->vreqs->begin() + (long)k);
		w = vreq_window(s, r.win);
		if (r.serial != 0) {
			struct dv_vfont	*f = w != nullptr ? font_serial(w, r.serial) : nullptr;
			int		 ok = msg.compare(0, 2, "OK") == 0;

			/* It is in this terminal now, whether or not its window
			 * is still here: the next sync settles that. */
			if (ok) {
				if (p->view.vfont_sent == nullptr)
					p->view.vfont_sent = new std::unordered_set<unsigned>;
				p->view.vfont_sent->insert(r.serial);
			}
			if (f != nullptr) {
				f->pending = 0;
				if (ok) {
					struct dv_vfont	*old = font_live(w, f->name);

					if (old != nullptr)
						old->live = 0;
					f->live = 1;
				}
			}
			if (w != nullptr) {
				/* Answer first: it needs the name, which a failed
				 * upload loses to the collection below. */
				vreq_answer(w, &r, msg);
				font_gc(w);
			}
			dv_kitty_sync_views(s);
			return;
		}
		if (w != nullptr)
			vreq_answer(w, &r, msg);
		return;
	}
}

/* That terminal will not answer now. A program must not wait for it. */
void
dv_kitty_peer_gone(struct dv_peer *p)
{
	struct dv_session	*s = p->session;

	if (s == nullptr || s->vreqs == nullptr)
		return;
	std::erase_if(*s->vreqs, [s, p](const struct dv_vreq &r) {
		struct dv_window	*w;

		if (r.fd != p->fd)
			return (false);
		w = vreq_window(s, r.win);
		if (w != nullptr) {
			v_error(w, r.id, r.q, "the terminal went away");
			if (r.serial != 0 && w->vwin != nullptr)
				fonts_erase(w->vwin,
				    [&r](const struct dv_vfont &f) {
					return (f.pending && f.serial == r.serial);
				});
		}
		return (true);
	});
}

void
dv_kitty_free_session(struct dv_session *s)
{
	delete s->vreqs;
	s->vreqs = nullptr;
}

/* ---- the window's image table ---- */

static uint64_t
image_find(struct dv_window *w, unsigned child_id)
{
	if (child_id == 0 || w->kimg == nullptr ||
	    !w->kimg->contains(child_id))
		return (0);
	return (child_id);
}

static int
image_referenced(struct dv_window *w, uint64_t img)
{
	if (w->screen.kpl != nullptr)
		for (const auto &[id, p] : *w->screen.kpl)
			if (p.img == img)
				return (1);
	return (0);
}

/* An image shape on the pointer stack follows what happens to its image. */
static void
pointer_refresh(struct dv_window *w)
{
	if (w->session == nullptr || w->session->desktop.focused != w)
		return;
	for (int i = 0; i < w->npshape; i++)
		if (strncmp(w->pshape[i], "image:", 6) == 0) {
			dv_window_apply_pointer(w);
			return;
		}
}

/* Free an image's pixels and remove its placements. */
static void
image_drop(struct dv_window *w, uint64_t img)
{
	if (w->screen.kpl != nullptr)
		std::erase_if(*w->screen.kpl, [img](const auto &entry) {
			return (entry.second.img == img);
		});
	auto it = w->kimg->find(img);
	if (it == w->kimg->end())
		return;
	dv_buf_free(&it->second.data);
	packs_free(&it->second);
	scene_free(w, &it->second);
	w->kimg->erase(it);
	if (w->kreassem == img)
		w->kreassem = 0;
	font_gc(w);
	pointer_refresh(w);
}

/* Named images replace their pixels while retaining placements. Anonymous
 * images get distinct internal keys outside the protocol's 32-bit IDs. */
static uint64_t
image_alloc(struct dv_window *w, unsigned child_id)
{
	if (w->kimg == nullptr)
		w->kimg = new std::unordered_map<uint64_t, dv_kimage>;
	uint64_t img = child_id;
	if (img == 0) {
		if (w->kanon == 0)
			w->kanon = UINT32_MAX;
		img = ++w->kanon;
	}
	auto [it, inserted] = w->kimg->try_emplace(img);
	struct dv_kimage *im = &it->second;
	if (inserted) {
		im->used = 1;
		im->child_id = child_id;
		im->gpu = 0;
	} else {
		im->data.len = 0;
		im->done = 0;
		im->want_place = 0;
		im->fmt[0] = '\0';
		im->reassembling = 0;
		im->gpu = 0;
	}
	return (img);
}

/* ---- placements ---- */

static unsigned
host_id_alloc(void)
{
	/* IDs are independent of window IDs and placement counts. Keep the
	 * scaled-text namespace separate and never reuse a stale host ID. */
	static uint64_t next_id = DV_KITTY_FIRST_ID;

	if (next_id == DV_STEXT_FIRST)
		next_id = 0x30000000u;
	if (next_id == DV_VREQ_FIRST)
		next_id = (uint64_t)DV_VREQ_LAST + 1;
	if (next_id >= DV_FLAT_FIRST)	/* the rest names flattened windows */
		dv_fatal("kitty placement ID space exhausted");
	return ((unsigned)next_id++);
}

/* The host's id for an image used without a placement of its own: by the
 * pointer, or by a record of a scene. Sent by dv_kitty_sync. */
static unsigned
res_id(struct dv_window *w, unsigned child)
{
	if (w->kres == nullptr)
		w->kres = new std::unordered_map<unsigned, unsigned>;
	auto [it, inserted] = w->kres->try_emplace(child, 0u);
	if (inserted)
		it->second = host_id_alloc();
	return (it->second);
}

static struct dv_kplace *
place_alloc(struct dv_screen *s)
{
	if (s->kpl == nullptr)
		s->kpl = new std::unordered_map<unsigned, dv_kplace>;
	unsigned id = host_id_alloc();
	auto [it, inserted] = s->kpl->try_emplace(id);
	it->second.dv_id = id;
	return (&it->second);
}

static void
req_parse(struct dv_kreq *q, const char *c, size_t n)
{
	q->c = key_val(c, n, 'c', 0);
	q->r = key_val(c, n, 'r', 0);
	q->x = key_val(c, n, 'x', 0);
	q->y = key_val(c, n, 'y', 0);
	q->w = key_val(c, n, 'w', 0);
	q->h = key_val(c, n, 'h', 0);
	q->z = key_val(c, n, 'z', 0);
	q->C = key_val(c, n, 'C', 0);
	q->p = key_val(c, n, 'p', 0);
	q->U = key_val(c, n, 'U', 0);
	q->X = key_val(c, n, 'X', 0);
	q->Y = key_val(c, n, 'Y', 0);
}

static void
cell_size(struct dv_window *w, unsigned *cw, unsigned *ch)
{
	*cw = w->session ? w->session->desktop.xpixel : 0;
	*ch = w->session ? w->session->desktop.ypixel : 0;
	if (*cw == 0)
		*cw = DV_DEFAULT_XPIXEL;
	if (*ch == 0)
		*ch = DV_DEFAULT_YPIXEL;
}

/*
 * Size a placement the way kitty does: c and r if the program gave them
 * (a previewer renders extra pixels for sharpness and relies on c/r for
 * the cell size); one of them keeps the source rectangle's aspect ratio;
 * neither means the source rectangle at the host's cell size.
 */
static void
place_geometry(struct dv_window *w, const struct dv_kimage *im,
    const struct dv_kreq *q, struct dv_kplace *p)
{
	unsigned	cw, ch;
	double		sw, sh;

	cell_size(w, &cw, &ch);
	p->sx = q->x > 0 ? (unsigned)q->x : 0;
	p->sy = q->y > 0 ? (unsigned)q->y : 0;
	p->sw = q->w > 0 ? (unsigned)q->w :
	    (im->wpx > p->sx ? im->wpx - p->sx : 0);
	p->sh = q->h > 0 ? (unsigned)q->h :
	    (im->hpx > p->sy ? im->hpx - p->sy : 0);
	p->want_c = q->c > 0 ? (unsigned)q->c : 0;
	p->want_r = q->r > 0 ? (unsigned)q->r : 0;
	p->off_x = q->X > 0 ? (unsigned)q->X : 0;
	p->off_y = q->Y > 0 ? (unsigned)q->Y : 0;
	sw = p->sw ? p->sw : 10.0 * cw;	/* unknown pixels: a modest default */
	sh = p->sh ? p->sh : 5.0 * ch;
	if (q->c > 0 && q->r > 0) {
		p->cols = (unsigned)q->c;
		p->rows = (unsigned)q->r;
	} else if (q->c > 0) {
		p->cols = (unsigned)q->c;
		p->rows = (unsigned)ceil((double)q->c * cw * sh / (sw * ch));
	} else if (q->r > 0) {
		p->rows = (unsigned)q->r;
		p->cols = (unsigned)ceil((double)q->r * ch * sw / (sh * cw));
	} else {
		p->cols = (unsigned)ceil(sw / cw);
		p->rows = (unsigned)ceil(sh / ch);
	}
	if (p->cols < 1)
		p->cols = 1;
	if (p->rows < 1)
		p->rows = 1;
	if (p->cols > 4096)
		p->cols = 4096;
	if (p->rows > 4096)
		p->rows = 4096;
	p->z = (int)q->z;
}

static void
place_begin(struct dv_window *w, struct dv_kplace *p, uint64_t img,
    const struct dv_kreq *q, int virt)
{
	unsigned id = p->dv_id;
	memset(p, 0, sizeof *p);
	p->dv_id = id;
	p->used = 1;
	p->virt = virt;
	p->img = img;
	p->child_id = w->kimg->at(img).child_id;
	p->pid = q->p > 0 ? (unsigned)q->p : 0;
	p->alt = w->screen.alt_on;
	p->depth = w->screen.depth;	/* placements take the pen's depth */
	p->gen = ++w->screen.kgen;
	place_geometry(w, &w->kimg->at(img), q, p);
}

/* Display at the cursor. The same placement id on the same image
 * replaces the earlier placement, as in kitty. */
static struct dv_kplace *
place_direct(struct dv_window *w, uint64_t img, const struct dv_kreq *q)
{
	struct dv_screen	*s = &w->screen;
	struct dv_kplace	*p = nullptr;
	unsigned		 endcol;
	int			 i;

	if (s->cc >= s->cols) {			/* deferred wrap, as for text */
		s->cc = 0;
		dv_screen_linefeed(s);
	}
	if (q->p > 0 && s->kpl != nullptr)
		for (auto &[id, existing] : *s->kpl)
			if (!existing.virt && existing.img == img &&
			    existing.pid == (unsigned)q->p) {
				p = &existing;
				break;
			}
	if (p == nullptr)
		p = place_alloc(s);
	place_begin(w, p, img, q, 0);
	p->row = (int)s->cr;
	p->col = (int)s->cc;
	if (q->C)
		return (p);
	/*
	 * The cursor ends after the image as after a run of text: on its
	 * last row, just past its right edge (pending a wrap if that is off
	 * screen). Moving down the rows scrolls at the bottom, and the
	 * placement rides along with the text -- so a picture taller than
	 * the window shows its bottom, with the prompt below it.
	 */
	for (i = 1; i < (int)p->rows; i++)
		dv_screen_linefeed(s);
	endcol = (unsigned)p->col + p->cols;
	s->cc = endcol < s->cols ? endcol : s->cols;
	return (p);
}

/* Register a virtual placement (U=1): one per image, positioned by the
 * placeholder cells the program writes. Re-registering re-sizes it. */
static void
place_virtual(struct dv_window *w, uint64_t img, const struct dv_kreq *q)
{
	struct dv_screen	*s = &w->screen;
	struct dv_kplace	*p = nullptr;

	struct dv_kimage *im = &w->kimg->at(img);
	if (s->kpl != nullptr) {
		auto it = s->kpl->find(im->virtual_id);
		if (it != s->kpl->end())
			p = &it->second;
	}
	if (p == nullptr)
		p = place_alloc(s);
	place_begin(w, p, img, q, 1);
	im->virtual_id = p->dv_id;
}

static void
place(struct dv_window *w, uint64_t img, const struct dv_kreq *q)
{
	if (q->U)
		place_virtual(w, img, q);
	else
		place_direct(w, img, q);
}

/*
 * a=d. d=i/I: the placements of image i (just placement p, if given);
 * d=a/A: every placement on this screen. Upper case frees the image data
 * too. The positional forms (by cell, column, row, z) are treated as
 * "all": removing too much beats leaving ghosts.
 */
static void
do_delete(struct dv_window *w, const char *c, size_t n)
{
	struct dv_screen	*s = &w->screen;
	char			 what = key_chr(c, n, 'd', 'a');
	int			 free_data = (what >= 'A' && what <= 'Z');
	long			 id = key_val(c, n, 'i', 0);
	long			 pid = key_val(c, n, 'p', 0);

	if ((what | 0x20) == 'i') {
		uint64_t img = image_find(w, (unsigned)id);
		if (img == 0)
			return;
		if (s->kpl != nullptr)
			std::erase_if(*s->kpl, [img, pid](const auto &entry) {
				const auto &p = entry.second;
				return (p.img == img &&
				    (pid == 0 || p.pid == (unsigned)pid));
			});
		if (free_data && !image_referenced(w, img))
			image_drop(w, img);
		return;
	}
	if (s->kpl != nullptr)
		std::erase_if(*s->kpl, [s, free_data](const auto &entry) {
			const auto &p = entry.second;
			return (free_data || (!p.virt && p.alt == s->alt_on));
		});
	if (free_data)
		dv_kitty_free(w);
}

static void
reply(struct dv_window *w, long id, long pid, const char *msg)
{
	std::string	r = "\033_Gi=" + std::to_string(id);

	if (pid > 0)
		r += ",p=" + std::to_string(pid);
	r += ";";
	r += msg;
	r += "\033\\";
	dv_window_pty_send(w, r.data(), r.size());
}

/*
 * Records on to every client that holds a placement of the image. Each
 * placement is its own image there. The peer named has placement own_id
 * up to date already.
 */
/* Records, as the a=e of a placement's image. */
static std::string
scene_frames(unsigned dv_id, const std::string &b64)
{
	std::string	frames;
	size_t		off = 0;

	while (off < b64.size()) {
		size_t	n = b64.size() - off > VCHUNK ? VCHUNK : b64.size() - off;
		int	more = off + n < b64.size();
		char	hdr[64];

		if (off == 0)
			snprintf(hdr, sizeof hdr, "\033_Ga=e,i=%u,q=2,m=%d;", dv_id,
			    more);
		else
			snprintf(hdr, sizeof hdr, "\033_Gm=%d;", more);
		frames += hdr;
		frames.append(b64, off, n);
		frames += "\033\\";
		off += n;
	}
	return (frames);
}

static void
scene_forward(struct dv_window *w, uint64_t img, const std::string &text,
    unsigned own_id, const struct dv_peer *own)
{
	std::string	b64;

	if (w->screen.kpl == nullptr || text.empty())
		return;
	b64 = b64_str(text);
	for (const auto &[pid, p] : *w->screen.kpl) {
		std::string	frames;

		if (p.img != img)
			continue;
		frames = scene_frames(p.dv_id, b64);
		dv_kitty_send_patch(w->session, p.dv_id, p.gen, frames.data(),
		    frames.size(), p.dv_id == own_id ? own : nullptr);
	}
	/*
	 * A terminal showing the window layered holds the image once, whatever
	 * its placements. The caller has just counted the edit in gen: those
	 * that held the scene as it was hold it as it is.
	 */
	const struct dv_kimage	*im = &w->kimg->at(img);

	if (im->hid != 0) {
		std::string	frames = scene_frames(im->hid, b64);

		/* A terminal holding the scene scaled has a root group of its
		 * own: a change to that one reaches it as the whole scene,
		 * scaled again, with its next frame. */
		dv_kitty_send_layer_patch(w->session, im->hid, im->gen - 1, im->gen,
		    frames.data(), frames.size(), own_id != 0 ? own : nullptr,
		    patch_changes_root(text));
	}
}

/* Every placement of an image shows it as it now is. */
static void
image_shown_again(struct dv_window *w, uint64_t img)
{
	if (w->screen.kpl != nullptr)
		for (auto &[id, p] : *w->screen.kpl)
			if (p.img == img)
				p.gen = ++w->screen.kgen;
}

/* An image's pixels, from decoded bytes: kept as base64 without o=z. */
static int
image_set_raw(struct dv_kimage *im, const std::vector<unsigned char> &raw)
{
	std::string	keys = im->fmt;
	size_t		at = keys.find("o=z");

	if (at != std::string::npos) {
		keys.erase(at, 3);
		if (at > 0 && at - 1 < keys.size() && keys[at - 1] == ',')
			keys.erase(at - 1, 1);
		else if (at < keys.size() && keys[at] == ',')
			keys.erase(at, 1);
	}
	if (raw.size() > SIZE_MAX / 2)
		return (0);
	im->data.len = 0;
	b64_encode(raw.data(), raw.size(), &im->data);
	dv_strlcpy(im->fmt, keys.c_str(), sizeof im->fmt);
	return (1);
}

/*
 * a=e for a palette image (palette-image-protocol): slots x to x+w-1 are
 * replaced by the payload, four bytes each. Every placement of the image is
 * then sent again, which for an image a terminal takes as it is means its
 * packed indices and the palette, and for the others the colours expanded.
 */
static void
palette_update(struct dv_window *w, uint64_t img)
{
	struct dv_vwin			*vw = w->vwin;
	struct dv_kimage		*im = &w->kimg->at(img);
	long				 id = vw->e_id, q = vw->e_q;
	long				 f = key_val(im->fmt, strlen(im->fmt), 'f', 32);
	long				 G = key_val(im->fmt, strlen(im->fmt), 'G', 0);
	long				 slots = 1L << fmt_bits(f), x, n;
	std::vector<unsigned char>	 raw, upd;
	unsigned char			*u = nullptr, *dec = nullptr;
	size_t				 ulen, declen;
	const char			*err = nullptr;

	x = vw->e_x < 0 ? 0 : vw->e_x;
	n = vw->e_w < 0 ? slots - x : vw->e_w;
	if (x >= slots || n <= 0 || n > slots - x)
		err = "EINVAL:slots outside the palette";
	else if (vw->e_G >= 0 && vw->e_G != G)
		err = "EINVAL:another gamut than the image's";
	if (err == nullptr) {
		u = (unsigned char *)malloc(vw->e_data.len ? vw->e_data.len : 1);
		if (u == nullptr)
			err = "ENOMEM:out of memory";
	}
	if (err == nullptr) {
		ulen = b64_decode((const char *)vw->e_data.data, vw->e_data.len, u,
		    vw->e_data.len);
		if (vw->e_zed) {
			if (!inflate_grow(u, ulen, &dec, &declen))
				err = "EINVAL:cannot inflate";
			else {
				free(u);
				u = dec;
				ulen = declen;
				if (ulen != (size_t)n * 4)
					err = "EINVAL:does not inflate to the slots";
			}
		}
		if (err == nullptr && ulen < (size_t)n * 4)
			err = "ENODATA:fewer bytes than the slots";
		else if (err == nullptr && ulen > (size_t)n * 4 && vw->e_direct)
			err = "EFBIG:more bytes than the slots";
	}
	vw->e_data.len = 0;
	if (err == nullptr && (!image_raw(im, &raw) ||
	    raw.size() < (size_t)slots * 4))
		err = "ENOMEM:out of memory";
	if (err == nullptr) {
		memcpy(raw.data() + x * 4, u, (size_t)n * 4);
		if (!image_set_raw(im, raw))
			err = "ENOMEM:out of memory";
	}
	free(u);
	if (err != nullptr) {
		if (q < 2)
			reply(w, id, 0, err);
		return;
	}
	im->gen++;
	image_shown_again(w, img);
	pointer_refresh(w);
	if (q < 1)
		reply(w, id, 0, "OK");
}

/*
 * a=e: the records go into the table and on to every client that holds a
 * placement of the image.
 */
static void
edit_done(struct dv_window *w)
{
	struct dv_vwin			*vw = w->vwin;
	long				 id = vw->e_id, q = vw->e_q;
	uint64_t			 img = image_find(w, (unsigned)id);
	struct dv_kimage		*im;
	std::vector<struct dv_vrec>	 recs;
	std::string			 text, err, out;

	vw->e_active = 0;
	if (img == 0 || !w->kimg->at(img).done) {
		vw->e_data.len = 0;
		if (q < 2)
			reply(w, id, 0, "ENOENT:image not found");
		return;
	}
	im = &w->kimg->at(img);
	if (im->vscene == nullptr &&
	    fmt_kind(key_val(im->fmt, strlen(im->fmt), 'f', 32)) == FK_PAL) {
		palette_update(w, img);
		return;
	}
	if (im->vscene == nullptr) {
		vw->e_data.len = 0;
		if (q < 2)
			reply(w, id, 0,
			    "EINVAL:neither a palette nor a vector image");
		return;
	}
	if (!payload_text(vw->e_data.data, vw->e_data.len, vw->e_zed, &text)) {
		vw->e_data.len = 0;
		if (q < 2)
			reply(w, id, 0, "EINVAL:cannot inflate");
		return;
	}
	vw->e_data.len = 0;
	if (!recs_parse(w, text, &recs, &err)) {
		if (q < 2)
			reply(w, id, 0, ("EINVAL:" + err).c_str());
		return;
	}
	for (auto &r : recs) {
		out += r.line;
		out += '\n';
		scene_apply(w, im->vscene, std::move(r));
	}
	im->data.len = 0;
	im->gen++;			/* an image without a placement goes whole */
	font_gc(w);
	scene_forward(w, img, out, 0, nullptr);
	if (q < 1)
		reply(w, id, 0, "OK");
}

/* -- ink: what a terminal applied to its copy of a scene, for the table -- */

/* The image the stroke began on, unless it has been transmitted again since. */
static struct dv_kimage *
ink_image(struct dv_window *w, const struct dv_silk_stroke *st, uint64_t *imgp)
{
	uint64_t		 img = image_find(w, st->image);
	struct dv_kimage	*im;

	if (img == 0)
		return (nullptr);
	im = &w->kimg->at(img);
	if (im->vscene == nullptr || im->vscene->serial != st->scene)
		return (nullptr);
	*imgp = img;
	return (im);
}

/*
 * The placement of an image a brush draws on: the one placeholder cells name if
 * the program made one, or the direct one drawn on top. A terminal holds one
 * brush and each placement is its own image there, so a pen on any other
 * placement is not inked, and the program draws that stroke itself.
 */
unsigned
dv_kitty_ink_target(struct dv_window *w, unsigned child_id)
{
	uint64_t		 img = image_find(w, child_id);
	const struct dv_kplace	*best = nullptr;
	unsigned		 id = dv_kitty_virtual_id(w, child_id);

	if (id != 0 || img == 0 || !w->kimg->at(img).done ||
	    w->screen.kpl == nullptr)
		return (id);
	for (const auto &[pid, p] : *w->screen.kpl)
		if (!p.virt && p.img == img && p.alt == w->screen.alt_on &&
		    (best == nullptr || p.z > best->z ||
		    (p.z == best->z && p.gen > best->gen)))
			best = &p;
	return (best != nullptr ? best->dv_id : 0);
}

/*
 * The placement of an image shown at x, y of the window's screen, 0 if there is
 * none: a terminal can ink only the one whose cell it was given, which is the
 * one compose paints there, whatever else that cell covers.
 */
unsigned
dv_kitty_ink_under(struct dv_window *w, unsigned child_id, int x, int y)
{
	struct dv_screen	*s = &w->screen;
	uint64_t		 img = image_find(w, child_id);
	const struct dv_kplace	*top = nullptr;
	const struct dv_cell	*c;
	unsigned		 cw, ch;
	int			 col, row;

	if (img == 0 || !w->kimg->at(img).done || x < 0 || y < 0)
		return (0);
	cell_size(w, &cw, &ch);
	col = x / (int)(cw * 256);
	row = y / (int)(ch * 256);
	if (s->kpl != nullptr)
		for (const auto &[pid, p] : *s->kpl)
			if (!p.virt && p.alt == s->alt_on &&
			    w->kimg->at(p.img).done && col >= p.col &&
			    col < p.col + (int)p.cols && row >= p.row &&
			    row < p.row + (int)p.rows &&
			    (top == nullptr || p.dv_id > top->dv_id))
				top = &p;
	if (top != nullptr)
		return (top->img == img ? top->dv_id : 0);
	c = dv_screen_disp_at(s, (unsigned)col, (unsigned)row);
	if (c == nullptr || c->ch != DV_KITTY_PLACEHOLDER || c->fg != child_id)
		return (0);
	return (dv_kitty_virtual_id(w, child_id));
}

/* Records for the one terminal whose copy of a placement's image needs them. */
static void
ink_tell(struct dv_window *w, const struct dv_peer *peer, unsigned dv_id,
    const std::string &text)
{
	if (w->screen.kpl == nullptr)
		return;
	auto it = w->screen.kpl->find(dv_id);

	if (it == w->screen.kpl->end())
		return;
	std::string	b64 = b64_str(text);
	std::string	frames = scene_frames(dv_id, b64);

	dv_kitty_send_patch_to(peer, dv_id, it->second.gen, frames.data(),
	    frames.size());
	/* Or its one copy of the image, if it shows the window layered. */
	auto im = w->kimg->find(it->second.img);

	if (im != w->kimg->end() && im->second.hid != 0) {
		frames = scene_frames(im->second.hid, b64);
		dv_kitty_send_layer_patch_to(peer, im->second.hid, im->second.gen,
		    frames.data(), frames.size());
	}
}

/* That terminal's copy has gone wrong, whichever way it holds it. */
static void
ink_stale(struct dv_window *w, const struct dv_silk_stroke *st)
{
	uint64_t	img = image_find(w, st->image);

	dv_kitty_peer_stale(st->peer, st->dv_id);
	if (img != 0 && w->kimg->at(img).hid != 0)
		dv_kitty_peer_stale(st->peer, w->kimg->at(img).hid);
}

/* The element a terminal made under one of our ids goes. */
void
dv_kitty_ink_drop(struct dv_window *w, const struct dv_peer *peer, unsigned dv_id,
    uint32_t token)
{
	ink_tell(w, peer, dv_id, "x id=" + std::to_string(token) + "\n");
}

/*
 * A stroke is over, and the terminal's element becomes the program's: the
 * table's record under the program's id, which later records of the program
 * will name, and which is right whatever that terminal drew. In one a=e, so
 * nothing shows but that. The element ends up last, so if the table has it
 * under something else, or the program has come to use our id for one of its
 * own, the terminal is sent the scene again instead.
 */
void
dv_kitty_ink_settle(struct dv_window *w, const struct dv_silk_stroke *st)
{
	uint64_t		 img;
	struct dv_kimage	*im = ink_image(w, st, &img);
	std::string		 text = "x id=" + std::to_string(st->token) + "\n";

	if (im == nullptr)
		return;
	const struct dv_vscene	*sc = im->vscene;
	auto			 it = sc->byid.find(st->id);

	if (sc->byid.contains(st->token)) {
		ink_stale(w, st);
		return;
	}
	if (it != sc->byid.end()) {
		for (size_t i = it->second + 1; i < sc->recs.size(); i++) {
			const struct dv_vrec	*r = &sc->recs[i];
			int			 drawing = 0;

			if (r->dead)
				continue;
			/* Still under our id there: it will end up after this. */
			for (const auto &other : *w->silk_strokes)
				if (r->has_id && other.id == r->id && other.own &&
				    other.peer == st->peer && other.dv_id == st->dv_id)
					drawing = 1;
			if (!drawing) {
				ink_stale(w, st);
				return;
			}
		}
		text += sc->recs[it->second].line + "\n";
	}
	ink_tell(w, st->peer, st->dv_id, text);
}

/* The placement of the brush's image the pen came down in, at x, y. */
int
dv_kitty_ink_bind(struct dv_window *w, struct dv_silk_stroke *st, int x, int y)
{
	uint64_t	img = image_find(w, st->image);
	unsigned	cw, ch;

	if (img == 0 || w->screen.kpl == nullptr ||
	    w->kimg->at(img).vscene == nullptr)
		return (0);
	auto it = w->screen.kpl->find(st->dv_id);

	if (it == w->screen.kpl->end() || it->second.img != img)
		return (0);
	st->scene = w->kimg->at(img).vscene->serial;
	st->gen = it->second.gen;
	cell_size(w, &cw, &ch);
	st->cx = x >= 0 ? x / (int)(cw * 256) : -1;
	st->cy = y >= 0 ? y / (int)(ch * 256) : -1;
	return (1);
}

/* Are the image and placement still those the stroke began with? */
int
dv_kitty_ink_live(struct dv_window *w, const struct dv_silk_stroke *st)
{
	uint64_t	img;

	if (ink_image(w, st, &img) == nullptr || w->screen.kpl == nullptr)
		return (0);
	auto it = w->screen.kpl->find(st->dv_id);

	return (it != w->screen.kpl->end() && it->second.gen == st->gen);
}

static int
ink_cell_is(struct dv_screen *s, const struct dv_silk_stroke *st, int x, int y,
    int *col0, int *row0)
{
	const struct dv_cell	*c;

	if (x < 0 || y < 0 || (c = dv_screen_disp_at(s, (unsigned)x,
	    (unsigned)y)) == nullptr || c->ch != DV_KITTY_PLACEHOLDER ||
	    c->fg != st->image)
		return (0);
	*col0 = x - (int)c->icol;
	*row0 = y - (int)c->irow;
	return (1);
}

/*
 * Where the placement is shown now: a direct one says, for a U=1 one it is its
 * placeholder cells. The cell last seen usually still is one, and says the new
 * origin if the screen scrolled.
 */
static int
ink_origin(struct dv_window *w, struct dv_silk_stroke *st,
    const struct dv_kplace *p, int *col0, int *row0)
{
	struct dv_screen	*s = &w->screen;

	if (!p->virt) {
		*col0 = p->col;
		*row0 = p->row;
		return (p->alt == s->alt_on);
	}
	if (ink_cell_is(s, st, st->cx, st->cy, col0, row0))
		return (1);
	for (unsigned y = 0; y < s->rows; y++)
		for (unsigned x = 0; x < s->cols; x++)
			if (ink_cell_is(s, st, (int)x, (int)y, col0, row0)) {
				st->cx = (int)x;
				st->cy = (int)y;
				return (1);
			}
	return (0);
}

/* The t of a group record into m: a, b, c, d, e, f. */
static int
ink_group_matrix(std::string_view line, double m[6])
{
	static const double	 identity[6] = { 1, 0, 0, 1, 0, 0 };
	size_t			 vs, ve;

	memcpy(m, identity, sizeof identity);
	if (!rec_field(line, "t", &vs, &ve))
		return (1);

	std::string	 list(line.substr(vs, ve - vs));
	const char	*at = list.c_str();

	for (int i = 0; i < 6; i++) {
		char	*end;

		m[i] = strtod(at, &end);
		if (end == at || (*end != (i == 5 ? '\0' : ',')) || !isfinite(m[i]))
			return (0);
		at = end + 1;
	}
	return (1);
}

/* The product of the transforms from the root down to the group. */
static int
ink_group_transform(const struct dv_vscene *sc, uint32_t group, double m[6])
{
	static const double	 identity[6] = { 1, 0, 0, 1, 0, 0 };
	uint32_t		 at = group;

	memcpy(m, identity, sizeof identity);
	for (size_t steps = 0; steps <= sc->recs.size(); steps++) {
		auto	 it = sc->byid.find(at);
		double	 t[6], o[6];

		if (it == sc->byid.end() || sc->recs[it->second].type != 'g') {
			if (at != 0)
				return (0);
			return (1);	/* the root always exists */
		}
		if (!ink_group_matrix(sc->recs[it->second].line, t))
			return (0);
		memcpy(o, m, sizeof o);
		m[0] = t[0] * o[0] + t[2] * o[1];
		m[1] = t[1] * o[0] + t[3] * o[1];
		m[2] = t[0] * o[2] + t[2] * o[3];
		m[3] = t[1] * o[2] + t[3] * o[3];
		m[4] = t[0] * o[4] + t[2] * o[5] + t[4];
		m[5] = t[1] * o[4] + t[3] * o[5] + t[5];
		if (at == 0)
			return (1);
		at = sc->recs[it->second].parent;
	}
	return (0);		/* a cycle leads nowhere */
}

/* The rectangle of a placement, in the terms of the ink protocol's page. */
struct ink_rect {
	double	L, T, W, H;
};

/* A terminal given U=1 fits the source inside the cells. */
static struct ink_rect
ink_fitted(const struct dv_kplace *p, int col0, int row0, unsigned cw, unsigned ch)
{
	struct ink_rect	r;
	double		bw = (double)p->cols * cw, bh = (double)p->rows * ch;
	double		scale = std::min(bw / p->sw, bh / p->sh);

	r.W = p->sw * scale;
	r.H = p->sh * scale;
	r.L = (double)col0 * cw + (bw - r.W) / 2;
	r.T = (double)row0 * ch + (bh - r.H) / 2;
	return (r);
}

/* What the program works its points out from, by that page's table. */
static struct ink_rect
ink_believed(const struct dv_kplace *p, int col0, int row0, unsigned cw, unsigned ch)
{
	struct ink_rect	r;

	if (p->virt)
		return (ink_fitted(p, col0, row0, cw, ch));
	r.L = (double)col0 * cw + p->off_x;
	r.T = (double)row0 * ch + p->off_y;
	r.W = (double)p->want_c * cw - p->off_x;
	r.H = (double)p->want_r * ch - p->off_y;
	if (p->want_c == 0 && p->want_r == 0) {
		r.W = p->sw;
		r.H = p->sh;
	} else if (p->want_c == 0)
		r.W = r.H * p->sw / p->sh;
	else if (p->want_r == 0)
		r.H = r.W * p->sh / p->sw;
	return (r);
}

/*
 * The placement of a stroke as the terminal with the pen shows it and as the
 * program takes it to be, with that terminal's pixels per pixel of the window.
 */
static int
ink_rects(struct dv_window *w, struct dv_silk_stroke *st, struct ink_rect *drawn,
    struct ink_rect *told, double *xs, double *ys)
{
	const struct dv_kplace	*p = &w->screen.kpl->at(st->dv_id);
	unsigned		 cw, ch, hcw, hch;
	int			 col0, row0;

	if (p->sw == 0 || p->sh == 0)
		return (0);
	if (!ink_origin(w, st, p, &col0, &row0)) {
		st->over = 1;		/* no terminal is showing it */
		return (0);
	}
	cell_size(w, &cw, &ch);
	hcw = st->peer->tty.xpixel ? st->peer->tty.xpixel : DV_DEFAULT_XPIXEL;
	hch = st->peer->tty.ypixel ? st->peer->tty.ypixel : DV_DEFAULT_YPIXEL;
	/* A terminal that shows the window layered was given the placement
	 * as the program made it, and its cells are the program's. */
	if (dv_kitty_layered(w, &st->peer->view) && !p->virt)
		*drawn = ink_believed(p, col0, row0, hcw, hch);
	else
		*drawn = ink_fitted(p, col0, row0, hcw, hch);
	*told = ink_believed(p, col0, row0, cw, ch);
	*xs = (double)hcw / cw;
	*ys = (double)hch / ch;
	return (told->W > 0 && told->H > 0);
}

/*
 * The point a report at x, y of the window adds, by what the terminal with the
 * pen shows: 0 when it adds none. A U=1 placement fits its source rectangle
 * inside its cells, aspect kept, centred.
 */
int
dv_kitty_ink_point(struct dv_window *w, struct dv_silk_stroke *st, int x, int y,
    double *ox, double *oy)
{
	uint64_t		 img;
	struct dv_kimage	*im = ink_image(w, st, &img);
	const struct dv_kplace	*p;
	struct ink_rect		 drawn, told;
	double			 m[6], xs, ys, X, Y, det;

	if (im == nullptr || !ink_rects(w, st, &drawn, &told, &xs, &ys))
		return (0);
	if (!ink_group_transform(im->vscene, st->group, m))
		return (0);
	p = &w->screen.kpl->at(st->dv_id);
	X = p->sx + (x * xs / 256.0 - drawn.L) * p->sw / drawn.W;
	Y = p->sy + (y * ys / 256.0 - drawn.T) * p->sh / drawn.H;
	det = m[0] * m[3] - m[1] * m[2];
	if (det == 0 || !isfinite(det))
		return (0);
	X -= m[4];
	Y -= m[5];
	*ox = (m[3] * X - m[2] * Y) / det;
	*oy = (m[0] * Y - m[1] * X) / det;
	return (isfinite(*ox) && isfinite(*oy));
}

/* A report's position on the axis of one rectangle, in those of the other. */
static int
ink_rescale(int v, double scale, double dL, double dW, double tL, double tW)
{
	double	at = tL + (v * scale / 256.0 - dL) * tW / dW;

	return ((int)llround(std::clamp(at * 256.0, -1073741823.0, 1073741823.0)));
}

/*
 * A window's program works out from a report the same point the terminal
 * applied, by the page's formulas. It can take the placement to be elsewhere:
 * shown in whole cells, or fitted to another shape of cell. So it is told the
 * position that gives that point, which is not always where the pen is.
 */
void
dv_kitty_ink_report(struct dv_window *w, struct dv_silk_stroke *st, int *x, int *y)
{
	struct ink_rect	drawn, told;
	double		xs, ys;

	if (!dv_kitty_ink_live(w, st) || !ink_rects(w, st, &drawn, &told, &xs, &ys))
		return;
	*x = ink_rescale(*x, xs, drawn.L, drawn.W, told.L, told.W);
	*y = ink_rescale(*y, ys, drawn.T, drawn.H, told.T, told.H);
}

/* The table takes what the terminal applied, and the other copies follow. */
void
dv_kitty_ink_record(struct dv_window *w, const struct dv_silk_stroke *st,
    const std::string &line)
{
	uint64_t		 img;
	struct dv_kimage	*im = ink_image(w, st, &img);
	struct dv_vrec		 r;
	std::string		 why;

	if (im == nullptr || !rec_parse(w, line, &r, &why))
		return;
	scene_apply(w, im->vscene, std::move(r));
	im->data.len = 0;
	im->gen++;
	font_gc(w);
	scene_forward(w, img, line + "\n", st->dv_id, st->own ? st->peer : nullptr);
}

/*
 * Checks a transmission that has arrived whole, for the formats whose size
 * their specification fixes: kitty's indexed colour, Silkie's packed,
 * palette and high precision formats. Less data than the size is ENODATA;
 * more, sent directly, EFBIG; compressed data that does not inflate to the
 * size, EINVAL. A palette image is kept decoded, so that its palette can
 * be updated in place. NULL when the image is good.
 */
static const char *
image_check(struct dv_kimage *im)
{
	long				f = key_val(im->fmt, strlen(im->fmt), 'f', 32);
	int				k = fmt_kind(f);
	uint64_t			need;
	std::vector<unsigned char>	raw;
	int				zed = strstr(im->fmt, "o=z") != nullptr;

	if (k != FK_IDX8 && k != FK_IDX16 && k != FK_GRAY && k != FK_565 &&
	    k != FK_PAL && !fmt_deep(f))
		return (nullptr);
	switch (fmt_size(f, im->wpx, im->hpx, &need)) {
	case 0:
		break;
	case -1:
		return ("EOVERFLOW:dimensions too large for memory");
	default:
		return ("EINVAL:no size");
	}
	if (!image_raw(im, &raw))
		return (zed ? "EINVAL:cannot inflate" : "ENOMEM:out of memory");
	if (zed && raw.size() != need)
		return ("EINVAL:does not inflate to the size");
	if (raw.size() < need)
		return ("ENODATA:less data than the format and size need");
	if (raw.size() > need && im->direct &&
	    (k == FK_PAL || k == FK_IDX8 || k == FK_IDX16 || fmt_deep(f)))
		return ("EFBIG:more data than the format and size need");
	if (k == FK_PAL) {
		raw.resize(need);
		if (!image_set_raw(im, raw))
			return ("ENOMEM:out of memory");
	}
	return (nullptr);
}

/* Is this an image animation frames cannot go with: indexed colour, a
 * palette, more than eight bits, or another curve than sRGB? */
static int
frames_refused(long f, long E)
{
	int	k = fmt_kind(f);

	return (k == FK_IDX8 || k == FK_IDX16 || k == FK_PAL || fmt_deep(f) ||
	    (E != 0 && E != 13 && k != FK_F64));
}

/*
 * A feature query (a=q with e=, graphics-protocol.rst "Querying support for
 * protocol features"): the reply names e and every key of the query, with
 * its value, that odin-seat takes for that action -- the others it leaves
 * out, and those at their defaults too. An action it does not take is
 * answered without e. Nothing is done: no image, no chunk, no medium read.
 * What depends on the terminals (a gamut, the GPU surface medium, the vector
 * service) is asked of dv_silkie_gfx_ok, as a transmission would be.
 */
static void
feature_query(struct dv_window *w, const char *c, size_t clen, long id)
{
	char		 act = key_chr(c, clen, 'e', 0);
	long		 f = key_val(c, clen, 'f', 32);
	char		 med = key_chr(c, clen, 't', 'd');
	std::string	 r = "\033_Gi=" + std::to_string(id);
	size_t		 at = 0;
	int		 xmit = act == 't' || act == 'T' || act == 'q';
	int		 place = act == 'T' || act == 'p';
	struct dv_session *s = w->session;

	if (act == 'v' && !(s == nullptr || s->silk_vector))
		act = 0;
	if (act != 't' && act != 'T' && act != 'q' && act != 'p' && act != 'd' &&
	    act != 'e' && act != 'v') {
		r += ";EINVAL:action not supported\033\\";
		dv_window_pty_send(w, r.data(), r.size());
		return;
	}
	r += ",e=";
	r += act;
	while (at < clen) {
		size_t		end = at;
		std::string	key, val;
		int		ok = 0;

		while (end < clen && c[end] != ',')
			end++;
		if (end - at >= 2 && c[at + 1] == '=') {
			key.assign(1, c[at]);
			val.assign(c + at + 2, end - at - 2);
		}
		at = end + 1;
		if (key.empty() || strchr("aiImqe", key[0]) != nullptr)
			continue;
		char		 k = key[0];
		uint64_t	 u64 = 0;
		const char	*vp = val.c_str();
		char		*ve;
		long long	 ll = strtoll(vp, &ve, 10);
		int		 isnum = !val.empty() && *ve == '\0';

		if (strchr("tod", k) == nullptr && k != 'S' && k != 'O' && !isnum)
			continue;
		switch (k) {
		case 'f':
			ok = (xmit || act == 'v') && ll != 32 &&
			    (act != 'v' || ll == 200) &&
			    dv_silkie_gfx_ok(s, (long)ll, 0, 'd');
			break;
		case 't':
			ok = (xmit || act == 'e' || act == 'v') && val.size() == 1 &&
			    val[0] != 'd' &&
			    (val[0] == 'f' || val[0] == 't' || val[0] == 's' ||
			    (val[0] == 'g' && xmit &&
			    dv_silkie_gfx_ok(s, f, 0, 'g')));
			break;
		case 'o':
			ok = (xmit || act == 'e' || act == 'v') && val == "z";
			break;
		case 's':
		case 'v':
			ok = xmit && ll > 0 && ll <= 4294967295ll;
			break;
		case 'S':
		case 'O':
			/* Their size was checked: cmd_malformed. */
			ok = (xmit || act == 'e' || act == 'v') && !val.empty() &&
			    strspn(vp, "0123456789") == val.size();
			for (const char *d = vp; ok && *d != '\0'; d++)
				u64 = u64 * 10 + (uint64_t)(*d - '0');
			ok = ok && u64 != 0;
			break;
		case 'G':
			ok = (xmit || act == 'e') && (ll == 1 || ll == 2) &&
			    (act == 'e' ? dv_silkie_gfx_ok(s, 401, (int)ll, 'd') :
			    dv_kitty_fmt_gamut(f) &&
			    dv_silkie_gfx_ok(s, f, (int)ll, med == 'g' ? 'g' : 'd'));
			break;
		case 'E':
			ok = xmit && ll != 0 && fmt_takes_E(f, ll) && med != 'g';
			break;
		case 'T':
			ok = xmit && ll == 1 && (f == 64 || fmt_deep(f) ||
			    key_val(c, clen, 'E', 0) == 16 ||
			    key_val(c, clen, 'E', 0) == 18);
			break;
		case 'x':
		case 'w':
			ok = (place || act == 'e') && ll > 0 && ll <= 4294967295ll;
			break;
		case 'y':
		case 'h':
		case 'c':
		case 'r':
		case 'X':
		case 'Y':
			ok = place && ll > 0 && ll <= 4294967295ll;
			break;
		case 'z':
			ok = place && ll != 0 && ll >= INT32_MIN && ll <= INT32_MAX;
			break;
		case 'C':
		case 'U':
			ok = place && ll == 1;
			break;
		case 'p':
			ok = (place || act == 'd') && ll > 0 && ll <= 4294967295ll;
			break;
		case 'd':
			ok = act == 'd' && val.size() == 1 && val[0] != 'a' &&
			    strchr("AiI", val[0]) != nullptr;
			break;
		}
		if (!ok)
			continue;
		r += ',';
		r += k;
		r += '=';
		if (strchr("tod", k) != nullptr)
			r += val;
		else if (k == 'S' || k == 'O')
			r += std::to_string(u64);
		else
			r += std::to_string(ll);
	}
	r += ";OK\033\\";
	dv_window_pty_send(w, r.data(), r.size());
}

void
dv_kitty_apc(struct dv_window *w, const unsigned char *cap, size_t len)
{
	struct dv_kimage	*im;
	struct dv_kreq		 rq;
	const char		*c, *data, *err;
	size_t			 clen, dlen, i;
	char			 act, medium;
	long			 id, m, q;
	uint64_t		 img, S = 0, O = 0;

	if (len < 1 || cap[0] != 'G')
		return;
	c = (const char *)cap + 1;
	len--;
	for (i = 0; i < len && c[i] != ';'; i++)
		;
	clen = i;
	data = (i < len) ? c + i + 1 : "";
	dlen = (i < len) ? len - i - 1 : 0;

	/* Numbers too large for their keys: a malformed command, unanswered. */
	if (cmd_malformed(c, clen))
		return;
	(void)key_u64(c, clen, 'S', &S);
	(void)key_u64(c, clen, 'O', &O);
	act = key_chr(c, clen, 'a', 't');
	id = key_val(c, clen, 'i', 0);
	m = key_val(c, clen, 'm', 0);
	q = key_val(c, clen, 'q', 0);

	/* The chunks of a=v and a=e need not say so again. */
	if (w->vwin != nullptr) {
		struct dv_vwin	*vw = w->vwin;

		if (!key_present(c, clen, 'a') && vw->v_active)
			act = 'v';
		else if (!key_present(c, clen, 'a') && vw->e_active)
			act = 'e';
		else if (act != 'v' && act != 'e')
			vw->v_active = vw->e_active = 0;
	}
	if (act == 'v' || act == 'e') {
		struct dv_vwin	*vw = vwin(w);
		int		 service = act == 'v';
		int		*active = service ? &vw->v_active : &vw->e_active;
		long		*aid = service ? &vw->v_id : &vw->e_id;
		long		*aq = service ? &vw->v_q : &vw->e_q;
		int		*azed = service ? &vw->v_zed : &vw->e_zed;
		struct dv_buf	*adata = service ? &vw->v_data : &vw->e_data;

		medium = key_chr(c, clen, 't', 'd');
		if (!*active || (key_present(c, clen, 'i') && id != *aid)) {
			if (service && key_val(c, clen, 'f', 0) != 200) {
				if (q < 2)
					reply(w, id, 0, "EINVAL:a=v is for f=200");
				return;
			}
			*active = 1;
			*aid = id;
			*aq = q;
			*azed = key_chr(c, clen, 'o', 0) == 'z';
			adata->len = 0;
			if (!service) {
				vw->e_x = key_present(c, clen, 'x') ?
				    key_val(c, clen, 'x', 0) : -1;
				vw->e_w = key_present(c, clen, 'w') ?
				    key_val(c, clen, 'w', 0) : -1;
				vw->e_G = key_present(c, clen, 'G') ?
				    key_val(c, clen, 'G', 0) : -1;
				vw->e_direct = medium == 'd';
			}
		}
		if (medium != 'd') {
			if ((err = read_medium(adata, medium, data, dlen, S, O)) !=
			    nullptr) {
				*active = 0;
				if (*aq < 2)
					reply(w, *aid, 0, err);
				return;
			}
			m = 0;
		} else
			dv_buf_append(adata, data, dlen);
		if (m == 1)
			return;
		if (service)
			vreq_done(w);
		else
			edit_done(w);
		return;
	}

	switch (act) {
	case 'q':				/* support query */
		/* Answered only when the host takes kitty graphics: silence
		 * is how a program learns there are none (it pairs the query
		 * with DA1). A Silkie format, medium or gamut the attached
		 * terminals did not list is refused. */
		if (q < 1 && dv_kitty_host_gfx(w)) {
			long	f = key_val(c, clen, 'f', 32);
			int	G = (int)key_val(c, clen, 'G', 0);
			long	E = key_val(c, clen, 'E', 0);
			char	med = key_chr(c, clen, 't', 'd');

			if (key_present(c, clen, 'e')) {
				if (id != 0)
					feature_query(w, c, clen, id);
				return;
			}
			if (!dv_silkie_gfx_ok(w->session, f, G, (int)med) ||
			    !fmt_takes_E(f, E) || (E != 0 && med == 'g'))
				reply(w, id ? id : 1, 0, "EINVAL");
			else
				reply(w, id ? id : 1, 0, "OK");
		}
		return;
	case 'd':
		do_delete(w, c, clen);
		return;
	case 'p':				/* place a stored image */
		img = image_find(w, (unsigned)id);
		if (img == 0 || !w->kimg->at(img).done) {
			if (q < 1)
				reply(w, id, key_val(c, clen, 'p', 0),
				    "ENOENT:image not found");
			return;
		}
		req_parse(&rq, c, clen);
		place(w, img, &rq);
		if (q < 1)
			reply(w, id, rq.p, "OK");
		return;
	case 't':
	case 'T':
		break;
	case 'f':
	case 'c': {
		/*
		 * Frames are not composed here. Those the specifications refuse
		 * -- of an image of indexed colour, a palette, more than eight
		 * bits or another curve, or a frame in such a format -- are
		 * refused as they say; the rest are let be, as before.
		 */
		uint64_t	 at = image_find(w, (unsigned)id);
		long		 ff = key_val(c, clen, 'f', 32);
		long		 E = key_val(c, clen, 'E', 0);
		int		 bad = act == 'f' && frames_refused(ff, E);

		if (at != 0) {
			const struct dv_kimage	*t = &w->kimg->at(at);

			bad |= frames_refused(key_val(t->fmt, strlen(t->fmt), 'f', 32),
			    key_val(t->fmt, strlen(t->fmt), 'E', 0));
		}
		if (bad && q < 2)
			reply(w, id, 0, "EINVAL:no frames for this format");
		return;
	}
	default:				/* animation control */
		return;
	}

	/* A transmission: a fresh one, or the next chunk of the one under
	 * way (continuation chunks carry only m= and q=). */
	if (w->kreassem == 0 || key_present(c, clen, 'f') ||
	    key_present(c, clen, 'i') || key_present(c, clen, 's') ||
	    key_present(c, clen, 't')) {
		long	f = key_val(c, clen, 'f', 32);
		int	G = (int)key_val(c, clen, 'G', 0);
		long	E = key_val(c, clen, 'E', 0);
		char	med = key_chr(c, clen, 't', 'd');
		int	k = fmt_kind(f);

		if (!dv_silkie_gfx_ok(w->session, f, G, (int)med)) {
			if (q < 2)
				reply(w, id, 0, "EINVAL:format not supported");
			return;
		}
		if (!fmt_takes_E(f, E) || (E != 0 && med == 'g')) {
			if (q < 2)
				reply(w, id, 0, "EINVAL:transfer function");
			return;
		}
		/* Raw formats say their size; these must. */
		if ((k == FK_IDX8 || k == FK_IDX16 || k == FK_GRAY ||
		    k == FK_565 || k == FK_PAL || fmt_deep(f)) &&
		    (key_val(c, clen, 's', 0) <= 0 || key_val(c, clen, 'v', 0) <= 0)) {
			if (q < 2)
				reply(w, id, 0, "EINVAL:s and v are required");
			return;
		}
		img = image_alloc(w, (unsigned)id);
		im = &w->kimg->at(img);
		im->gpu = (med == 'g');
		im->direct = (med == 'd');
		im->pal = &w->screen.pal;
		capture_fmt(im, c, clen);
		im->want_place = (act == 'T');
		req_parse(&im->req, c, clen);
		im->reassembling = 1;
		w->kreassem = img;
	} else {
		img = w->kreassem;
		im = &w->kimg->at(img);
	}

	medium = key_chr(c, clen, 't', 'd');
	if (im->gpu) {
		dv_buf_append(&im->data, data, dlen);
		m = 0;
	} else if (medium != 'd') {
		if ((err = read_medium(&im->data, medium, data, dlen, S, O)) !=
		    nullptr) {
			image_drop(w, img);
			w->kreassem = 0;
			if (q < 2)
				reply(w, id, 0, err);
			return;
		}
		m = 0;
	} else
		dv_buf_append(&im->data, data, dlen);
	if (m == 1)
		return;					/* more chunks follow */

	im->reassembling = 0;
	w->kreassem = 0;
	if (key_val(im->fmt, strlen(im->fmt), 'f', 32) == 200) {
		std::string	err;
		long		sv = key_val(im->fmt, strlen(im->fmt), 's', 0);
		long		vv = key_val(im->fmt, strlen(im->fmt), 'v', 0);

		if (sv <= 0 || vv <= 0)
			err = "s and v are required";
		if (!err.empty() || !scene_load(w, im, &err)) {
			/* Nothing of it is kept: the scene before it stands. */
			if (q < 2 && im->child_id != 0)
				reply(w, (long)im->child_id, 0, ("EINVAL:" + err).c_str());
			if (im->vscene != nullptr) {
				dv_strlcpy(im->fmt, im->vscene->fmt, sizeof im->fmt);
				im->data.len = 0;
				im->done = 1;
				im->want_place = 0;
				image_pixels(im);
			} else
				image_drop(w, img);
			return;
		}
	} else {
		const char	*bad;

		scene_free(w, im);
		image_pixels(im);
		if ((bad = image_check(im)) != nullptr) {
			long	cid = (long)im->child_id;

			image_drop(w, img);
			if (q < 2 && cid != 0)
				reply(w, cid, 0, bad);
			return;
		}
	}
	im->done = 1;
	im->gen++;
	image_pixels(im);
	{
		long	k = fmt_kind(key_val(im->fmt, strlen(im->fmt), 'f', 32));

		if (k == FK_IDX8 || k == FK_IDX16)
			im->pal_stamp = pal_stamp(im->pal);
	}
	pointer_refresh(w);
	/* Placements of an image sent again show the new pixels. */
	image_shown_again(w, img);
	if (im->want_place) {
		place(w, img, &im->req);
		im->want_place = 0;
	}
	if (q < 1 && im->child_id != 0)
		reply(w, (long)im->child_id, im->req.p, "OK");
}

void
dv_kitty_free(struct dv_window *w)
{
	if (w->kimg != nullptr)
		for (auto &[id, im] : *w->kimg) {
			dv_buf_free(&im.data);
			packs_free(&im);
			scene_free(w, &im);
		}
	delete w->kimg;
	w->kimg = nullptr;
	if (w->screen.kpl != nullptr)
		w->screen.kpl->clear();
	w->kreassem = 0;
	font_gc(w);
}

/* Deleting images leaves fonts alone; only the window closing frees them. */
void
dv_kitty_close(struct dv_window *w)
{
	if (w->vwin != nullptr) {
		fonts_erase(w->vwin, [](const struct dv_vfont &) { return (true); });
		dv_buf_free(&w->vwin->v_data);
		dv_buf_free(&w->vwin->e_data);
		delete w->vwin;
		w->vwin = nullptr;
	}
	delete w->kres;
	w->kres = nullptr;
}

/* image:<id>[:x[:y]] for the host, if the window has that image now. */
int
dv_kitty_pointer_image(struct dv_window *w, const char *name, char *out,
    size_t cap)
{
	unsigned long	 id, hx = 0, hy = 0;
	char		*end;
	uint64_t	 img;

	if (strncmp(name, "image:", 6) != 0)
		return (0);
	id = strtoul(name + 6, &end, 10);
	if (end == name + 6 || id > UINT32_MAX)
		return (0);
	if (*end == ':') {
		hx = strtoul(end + 1, &end, 10);
		if (*end == ':')
			hy = strtoul(end + 1, &end, 10);
	}
	img = image_find(w, (unsigned)id);
	if (img == 0 || !w->kimg->at(img).done)
		return (0);
	snprintf(out, cap, "image:%u:%lu:%lu", res_id(w, (unsigned)id), hx, hy);
	return (1);
}

/* The host's answer to an OSC 22 query, with the program's image ids. */
size_t
dv_kitty_pointer_reply(struct dv_window *w, const char *in, size_t n,
    struct dv_buf *out)
{
	std::string_view	s(in, n);
	size_t			i = 0;

	while (i < n) {
		size_t	at = s.find("image:", i);

		if (at == std::string_view::npos) {
			dv_buf_append(out, in + i, n - i);
			break;
		}
		at += 6;
		dv_buf_append(out, in + i, at - i);
		i = at;
		unsigned long long	v = 0;
		size_t			k = at;

		while (k < n && in[k] >= '0' && in[k] <= '9' && v <= UINT32_MAX)
			v = v * 10 + (unsigned long long)(in[k++] - '0');
		if (k == at || w->kres == nullptr)
			continue;
		for (const auto &[child, hid] : *w->kres)
			if (hid == v) {
				std::string	c = std::to_string(child);

				dv_buf_append(out, c.data(), c.size());
				i = k;
				break;
			}
	}
	return (out->len);
}

/* The placement behind a host id, and its window. */
struct dv_kplace *
dv_kitty_lookup(struct dv_session *s, unsigned dv_id, struct dv_window **wp)
{
	struct dv_window *w;

	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		if (w->screen.kpl == nullptr)
			continue;
		auto it = w->screen.kpl->find(dv_id);
		if (it != w->screen.kpl->end()) {
			*wp = w;
			return (&it->second);
		}
	}
	return (nullptr);
}

/* The host id behind a placeholder cell the program wrote (U=1 mode),
 * from the image id it encoded in the cell; 0 when there is none. */
unsigned
dv_kitty_virtual_id(struct dv_window *w, unsigned child_id)
{
	uint64_t img = image_find(w, child_id);
	if (img == 0 || !w->kimg->at(img).done || w->screen.kpl == nullptr)
		return (0);
	unsigned id = w->kimg->at(img).virtual_id;
	return (w->screen.kpl->contains(id) ? id : 0);
}

/* Which medium to hand the host, from ODIN_SEAT_KITTY_MEDIUM: shm (the
 * default), tempfile, file, or direct. Shared memory and a temp file are
 * removed by the host after it reads them; a plain file is not. */
static char
emit_medium_choice(void)
{
	const char	*e = getenv("ODIN_SEAT_KITTY_MEDIUM");

	if (e == nullptr || e[0] == '\0')
		return ('s');
	if (e[0] == 'd' || e[0] == 'i')		/* direct / inline */
		return ('d');
	if (e[0] == 'f')			/* file */
		return ('f');
	if (e[0] == 't')			/* temp file */
		return ('t');
	return ('s');				/* shm / shared / anything else */
}

/* A t=f file is not deleted by the host, so unlink ours ourselves once a
 * later transmit shows the host has long since read it. */
static void
defer_unlink(const char *path)
{
	static char	ring[16][256];
	static int	head;

	if (ring[head][0] != '\0')
		unlink(ring[head]);
	snprintf(ring[head], sizeof ring[head], "%s", path);
	head = (head + 1) % 16;
}

/*
 * Write the raw payload to the chosen out-of-band medium and return its
 * name (POSIX shm) or path (file) for the escape's base64 payload, plus
 * the medium char actually used. Returns 'd' -- send the bytes inline --
 * on 'direct', empty input, or any failure, so an image always gets out.
 */
static char
emit_medium(char want, const unsigned char *raw, size_t rawlen, char *name,
    size_t namecap)
{
	static unsigned	ctr;
	int		fd;

	if (want == 'd' || rawlen == 0)
		return ('d');
	if (want == 's') {
		void	*m;

		snprintf(name, namecap, "/gv%u_%u", (unsigned)getpid(), ++ctr);
		fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0600);
		if (fd < 0)
			return ('d');
		if (ftruncate(fd, (off_t)rawlen) < 0) {
			close(fd);
			shm_unlink(name);
			return ('d');
		}
		m = mmap(NULL, rawlen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		close(fd);
		if (m == MAP_FAILED) {
			shm_unlink(name);
			return ('d');
		}
		memcpy(m, raw, rawlen);
		munmap(m, rawlen);
		return ('s');
	}
	{					/* file or temp file */
		const char	*tmp = getenv("TMPDIR");
		ssize_t		 wr;

		if (tmp == nullptr || tmp[0] == '\0')
			tmp = "/tmp";
		snprintf(name, namecap, "%s/gv%u_%u.kitty", tmp,
		    (unsigned)getpid(), ++ctr);
		fd = open(name, O_CREAT | O_WRONLY | O_EXCL, 0600);
		if (fd < 0)
			return ('d');
		wr = write(fd, raw, rawlen);
		close(fd);
		if (wr < 0 || (size_t)wr != rawlen) {
			unlink(name);
			return ('d');
		}
		if (want == 'f')		/* the host will not delete it */
			defer_unlink(name);
		return (want == 'f' ? 'f' : 't');
	}
}

/*
 * Transmit a placement to the host: its image's pixels under the
 * placement's id, then a virtual placement of its cell size and source
 * rectangle. The pixels travel by the medium ODIN_SEAT_KITTY_MEDIUM
 * selects (shared memory by default, so large images do not bloat the
 * escape stream), falling back to inline direct data. The placeholder
 * cells the compositor paints then select which part shows where.
 */
static int
image_emit(struct dv_kimage *im, unsigned hid, struct dv_buf *out)
{
	char			 hdr[200];
	int			 n, sent = 0;
	char			 medium = emit_medium_choice();

	if (im->vscene != nullptr && im->data.len == 0)
		scene_render(im->vscene, &im->data);
	if (!im->done || im->data.len == 0)
		return (0);

	if (im->gpu) {
		n = snprintf(hdr, sizeof hdr, "\033_Ga=t,i=%u,%s%st=g,q=2;",
		    hid, im->fmt, im->fmt[0] ? "," : "");
		if (n > 0)
			dv_buf_append(out, hdr, (size_t)n);
		dv_buf_append(out, im->data.data, im->data.len);
		dv_buf_append(out, "\033\\", 2);
		sent = 1;
	} else if (medium != 'd') {			/* out-of-band: shm / file */
		unsigned char	*raw = (unsigned char *)malloc(im->data.len);

		if (raw != nullptr) {
			size_t	rawlen = b64_decode((const char *)im->data.data,
			    im->data.len, raw, im->data.len);
			char	name[256];
			char	used = rawlen ? emit_medium(medium, raw, rawlen,
			    name, sizeof name) : 'd';

			if (used != 'd') {
				struct dv_buf	nb;

				memset(&nb, 0, sizeof nb);
				b64_encode((const unsigned char *)name,
				    strlen(name), &nb);
				/*
				 * S, the bytes that are the image: a shared
				 * memory object is as large as the pages that
				 * hold it, and what follows the data in the
				 * last of them is not data. Pixels are counted
				 * off by their width and height and never
				 * minded; a vector scene is text to its end,
				 * and was refused for the padding.
				 */
				n = snprintf(hdr, sizeof hdr,
				    "\033_Ga=t,i=%u,%s%st=%c,S=%zu,q=2;", hid,
				    im->fmt, im->fmt[0] ? "," : "", used, rawlen);
				if (n > 0)
					dv_buf_append(out, hdr, (size_t)n);
				dv_buf_append(out, nb.data, nb.len);
				dv_buf_append(out, "\033\\", 2);
				dv_buf_free(&nb);
				sent = 1;
			}
			free(raw);
		}
	}

	if (!sent) {				/* inline direct data, chunked */
		size_t	off = 0;
		int	first = 1;

		do {
			size_t	chunk = im->data.len - off;
			int	last;

			if (chunk > CHUNK)
				chunk = CHUNK;
			last = (off + chunk >= im->data.len);
			if (first)
				n = snprintf(hdr, sizeof hdr,
				    "\033_Ga=t,i=%u,%s%sq=2,m=%d;", hid,
				    im->fmt, im->fmt[0] ? "," : "", last ? 0 : 1);
			else
				n = snprintf(hdr, sizeof hdr, "\033_Gm=%d;",
				    last ? 0 : 1);
			if (n > 0)
				dv_buf_append(out, hdr, (size_t)n);
			dv_buf_append(out, im->data.data + off, chunk);
			dv_buf_append(out, "\033\\", 2);
			off += chunk;
			first = 0;
		} while (off < im->data.len);
	}
	return (1);
}

/* ---- re-packing for the display a client has ---- */

static std::string
strf_kitty(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static std::string
strf_kitty(const char *fmt, ...)
{
	va_list		 ap;
	char		*p = nullptr;
	int		 n;
	std::string	 out;

	va_start(ap, fmt);
	n = vasprintf(&p, fmt, ap);
	va_end(ap);
	if (n < 0)
		dv_fatal("out of memory");
	out.assign(p, (size_t)n);
	free(p);
	return (out);
}

/*
 * What one client can perceive and take: the packed formats it listed and
 * the channel widths its display reported. A client that said nothing of
 * its display is taken to see eight bits a channel, and gets the picture
 * losslessly, only packed tighter where the pixels allow.
 */
struct pack_profile {
	int	packed;		/* dv_view.packed */
	int	gray;		/* one channel: gray or mono */
	int	bits[3];	/* r, g, b, or bits[0] for gray: 1-8 */
	int	wg;		/* it takes G=, pixels in a wider gamut */
	int	vector;		/* it draws vector scenes (f=200) itself */
	int	hdr;		/* it takes HDR pixels (f=64) */
	/* Its cell over the session's, when the picture goes scaled to it;
	 * all 0 when it goes at its own size. */
	unsigned cw, scw, ch, sch;
};

struct dv_kpack {
	unsigned	gen;		/* the image's transmission */
	std::string	key;		/* pack_profile, as text */
	std::string	fmt;		/* keys to send, "" = the image's own */
	struct dv_buf	data;		/* base64, when fmt is not "" */
};

struct dv_kpacks {
	std::vector<struct dv_kpack>	v;
};

static void
packs_free(struct dv_kimage *im)
{
	if (im->packs == nullptr)
		return;
	for (auto &k : im->packs->v)
		dv_buf_free(&k.data);
	delete im->packs;
	im->packs = nullptr;
}

static int
clamp_bits(int b)
{
	return (b <= 0 || b > 8 ? 8 : b);
}

static struct pack_profile
pack_profile_of(const struct dv_view *v, int scaled)
{
	struct pack_profile	pr;

	pr.packed = v->packed;
	pr.wg = v->wg;
	pr.vector = v->vector;
	pr.hdr = v->hdr;
	pr.cw = pr.scw = pr.ch = pr.sch = 0;
	if (scaled && dv_view_scaled(v)) {
		pr.cw = v->cell_w;
		pr.scw = v->scell_w;
		pr.ch = v->cell_h;
		pr.sch = v->scell_h;
	}
	pr.gray = v->mono || v->disp_color != 0;
	if (pr.gray) {
		/* -m paints strict black and white; mono is one bit. */
		pr.bits[0] = v->mono || v->disp_color == 2 ? 1 :
		    clamp_bits(v->disp_bits[3]);
		pr.bits[1] = pr.bits[2] = 0;
	} else
		for (int i = 0; i < 3; i++)
			pr.bits[i] = clamp_bits(v->disp_bits[i]);
	return (pr);
}

/* A channel at b bits, as the eight-bit value it stands for (the expansion
 * the packed-image specification gives for RGB565, applied to any width). */
static unsigned
quant(unsigned c, int b)
{
	unsigned	m = (1u << b) - 1;
	unsigned	q = (c * m + 127) / 255;

	return ((q * 255 + m / 2) / m);
}

static unsigned
luma(const unsigned char *px)
{
	return ((2126u * px[0] + 7152u * px[1] + 722u * px[2] + 5000u) / 10000u);
}

/* ---- one picture for terminals that differ ---- */

/* Is this client sent the pictures of layered windows at another scale? */
int
dv_view_scaled(const struct dv_view *v)
{
	return (v->cell_w != 0 && v->cell_h != 0 && v->scell_w != 0 &&
	    v->scell_h != 0 &&
	    (v->cell_w != v->scell_w || v->cell_h != v->scell_h));
}

/* A length in pixels of a cell `from` wide, in pixels of a cell `to` wide. */
unsigned
dv_scale_len(uint64_t n, unsigned to, unsigned from)
{
	if (from == 0 || to == from)
		return ((unsigned)n);
	return ((unsigned)((n * to + from / 2) / from));
}

/* The same for the size of a picture, which never scales away to nothing. */
unsigned
dv_scale_size(uint64_t n, unsigned to, unsigned from)
{
	unsigned	v = dv_scale_len(n, to, from);

	return (v == 0 && n != 0 ? 1 : v);
}

/*
 * Can a copy of this image be made at another scale? A vector scene can:
 * the same records in a smaller intrinsic size, with the root group scaling
 * everything to it (scene_text_scaled). Of pixels, those that are here to
 * work on, which a GPU surface's are not: it is a token.
 */
int
dv_kitty_scalable(const struct dv_kimage *im)
{
	long	f = key_val(im->fmt, strlen(im->fmt), 'f', 32);

	if (im->gpu)
		return (0);
	if (im->vscene != nullptr || f == 64)
		return (im->wpx != 0 && im->hpx != 0);
	return (dv_kitty_fmt_known(f) && f != 200);
}

/*
 * The root group of a scene (g id=0) with everything in it scaled by fx, fy:
 * its transform, or the identity when it has none, followed by the scaling.
 * A transform that does not read as six numbers is left for the terminal to
 * refuse, as it would have.
 */
static std::string
root_scaled(const std::string &line, double fx, double fy)
{
	size_t		vs, ve;
	double		m[6] = { 1, 0, 0, 1, 0, 0 };
	std::string	t;

	if (rec_field(line, "t", &vs, &ve)) {
		std::string	val = line.substr(vs, ve - vs);
		const char	*at = val.c_str();

		for (int i = 0; i < 6; i++) {
			char	*end;

			m[i] = strtod(at, &end);
			if (end == at || !isfinite(m[i]) ||
			    (i < 5 ? *end != ',' : *end != '\0'))
				return (line);
			at = end + 1;
		}
	}
	t = strf_kitty("%.9g,%.9g,%.9g,%.9g,%.9g,%.9g", fx * m[0], fy * m[1],
	    fx * m[2], fy * m[3], fx * m[4], fy * m[5]);
	if (rec_field(line, "t", &vs, &ve))
		return (line.substr(0, vs) + t + line.substr(ve));
	return (line + " t=" + t);
}

/*
 * A scene's records for a terminal that shows it w2 by h2 where its
 * program made it w by h: the same scene, with its root group scaling user
 * units by the one over the other. Nothing in a scene is in pixels but its
 * intrinsic size, so this is all of scaling it, and the terminal draws it
 * as sharp as any other.
 */
static std::string
scene_text_scaled(const struct dv_vscene *sc, unsigned w, unsigned h,
    unsigned w2, unsigned h2)
{
	double		fx = w ? (double)w2 / w : 1, fy = h ? (double)h2 / h : 1;
	std::string	text;
	bool		rooted = false;

	for (const auto &r : sc->recs) {
		if (r.dead)
			continue;
		if (r.type == 'g' && r.has_id && r.id == 0) {
			text += root_scaled(r.line, fx, fy);
			rooted = true;
		} else
			text += r.line;
		text += '\n';
	}
	if (!rooted)
		text = root_scaled("g id=0", fx, fy) + "\n" + text;
	return (text);
}

/* Do these records change the root group, whose transform a scaled copy of
 * the scene has another of? Deleting everything may put it back, too. */
static int
patch_changes_root(const std::string &text)
{
	size_t	at = 0;

	while (at < text.size()) {
		size_t			end = text.find('\n', at);
		std::string_view	line(text.data() + at,
					    (end == std::string::npos ? text.size() : end) - at);
		uint32_t		id;

		if (line.size() > 1 && line[0] == 'g' && rec_u32(line, "id", &id) && id == 0)
			return (1);
		if (line.size() > 1 && line[0] == 'x' &&
		    line.find(" all") != std::string_view::npos)
			return (1);
		if (end == std::string::npos)
			break;
		at = end + 1;
	}
	return (0);
}

/* An image's keys with another size: s= and v= replaced, the rest kept but
 * o=, which is how the bytes were packed and not what the image is. */
static std::string
fmt_resized(const char *fmt, unsigned w, unsigned h)
{
	std::string	out, in = fmt;
	size_t		at = 0;

	while (at <= in.size()) {
		size_t		end = in.find(',', at);
		std::string	tok = in.substr(at, end == std::string::npos ?
				    std::string::npos : end - at);

		if (tok.rfind("s=", 0) == 0)
			tok = "s=" + std::to_string(w);
		else if (tok.rfind("v=", 0) == 0)
			tok = "v=" + std::to_string(h);
		else if (tok.rfind("o=", 0) == 0)
			tok.clear();
		if (!tok.empty()) {
			if (!out.empty())
				out += ',';
			out += tok;
		}
		if (end == std::string::npos)
			break;
		at = end + 1;
	}
	return (out);
}

/* One pass of the resampler below: n pixels in a row or column, `step`
 * floats apart, to m. Each new pixel is the mean of the stretch of the old
 * ones it covers, a part of a pixel counting for that part. */
static void
scale_line(const float *in, size_t n, size_t in_step, float *out, size_t m,
    size_t out_step)
{
	double	span = (double)n / (double)m;

	for (size_t i = 0; i < m; i++) {
		double	a = (double)i * span, b = a + span;
		size_t	k0 = (size_t)a, k1 = (size_t)b;
		double	acc[4] = { 0, 0, 0, 0 };

		if (k1 >= n)
			k1 = n - 1;
		if (k0 > k1)
			k0 = k1;
		for (size_t k = k0; k <= k1; k++) {
			double	lo = std::max(a, (double)k);
			double	hi = std::min(b, (double)k + 1);
			double	wt = hi > lo ? hi - lo : 0;

			if (k0 == k1)
				wt = span;	/* all of it lies in the one pixel */
			for (int c = 0; c < 4; c++)
				acc[c] += wt * in[k * in_step + c];
		}
		for (int c = 0; c < 4; c++)
			out[i * out_step + c] = (float)(acc[c] / span);
	}
}

/*
 * w x h pixels of four floats, colour already multiplied by alpha, as
 * nw x nh: each new pixel is the mean of the area it covers, so a picture
 * drawn for a large cell loses nothing but detail on a small one. Weighted
 * by alpha that way, a transparent pixel has no colour to lend its
 * neighbours, and an edge keeps its own.
 */
static std::vector<float>
scale_premult(const std::vector<float> &a, unsigned w, unsigned h, unsigned nw,
    unsigned nh)
{
	std::vector<float>	b((size_t)nw * h * 4), c((size_t)nw * nh * 4);

	for (size_t y = 0; y < h; y++)
		scale_line(&a[y * w * 4], w, 4, &b[y * nw * 4], nw, 4);
	for (size_t x = 0; x < nw; x++)
		scale_line(&b[x * 4], h, (size_t)nw * 4, &c[x * 4], nh,
		    (size_t)nw * 4);
	return (c);
}

/* Eight-bit RGBA through it. */
static std::vector<unsigned char>
scale_rgba(const unsigned char *px, unsigned w, unsigned h, unsigned nw,
    unsigned nh)
{
	std::vector<float>		 a((size_t)w * h * 4);
	std::vector<unsigned char>	 out((size_t)nw * nh * 4);

	for (size_t i = 0; i < (size_t)w * h; i++) {
		float	al = px[i * 4 + 3];

		a[i * 4 + 0] = px[i * 4 + 0] * al;
		a[i * 4 + 1] = px[i * 4 + 1] * al;
		a[i * 4 + 2] = px[i * 4 + 2] * al;
		a[i * 4 + 3] = al;
	}
	std::vector<float>	c = scale_premult(a, w, h, nw, nh);

	for (size_t i = 0; i < (size_t)nw * nh; i++) {
		float	al = c[i * 4 + 3];

		for (int k = 0; k < 3; k++) {
			float	v = al > 0 ? c[i * 4 + k] / al : 0;

			out[i * 4 + k] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 :
			    v + 0.5f);
		}
		out[i * 4 + 3] = (unsigned char)(al < 0 ? 0 : al > 255 ? 255 :
		    al + 0.5f);
	}
	return (out);
}

/* IEEE 754 half precision, the numbers of f=64 (Silkie's HDR images). */
static float
half_to_float(unsigned h)
{
	unsigned	e = (h >> 10) & 31, m = h & 1023;
	float		v;

	if (e == 0)
		v = ldexpf((float)m, -24);
	else if (e == 31)
		v = m ? NAN : INFINITY;
	else
		v = ldexpf((float)(m | 1024), (int)e - 25);
	return ((h & 0x8000) ? -v : v);
}

static unsigned
float_to_half(float f)
{
	unsigned	sign = signbit(f) ? 0x8000u : 0;
	int		e;
	float		m;

	if (isnan(f))
		return (0x7e00);
	f = fabsf(f);
	if (f >= 65520.0f)		/* past the largest half: it saturates */
		return (sign | 0x7bff);
	if (f < ldexpf(1.0f, -24) / 2)
		return (sign);
	m = frexpf(f, &e);		/* f = m * 2^e, m in [0.5, 1) */
	if (e < -13) {			/* a subnormal half */
		unsigned	q = (unsigned)lrintf(ldexpf(f, 24));

		return (sign | q);
	}
	unsigned	q = (unsigned)lrintf(ldexpf(m, 11)) - 1024;	/* 0-1024 */
	unsigned	he = (unsigned)(e + 14);

	if (q == 1024) {		/* rounded up to the next power of two */
		q = 0;
		he++;
	}
	if (he >= 31)
		return (sign | 0x7bff);
	return (sign | (he << 10) | q);
}

/* The bytes an image was transmitted as, base64 and deflate undone. */
static int
image_raw(const struct dv_kimage *im, std::vector<unsigned char> *out)
{
	unsigned char	*raw, *dec = nullptr;
	size_t		 rawlen, declen = 0;

	if (im->gpu || im->data.len == 0)
		return (0);
	raw = (unsigned char *)malloc(im->data.len);
	if (raw == nullptr)
		return (0);
	rawlen = b64_decode((const char *)im->data.data, im->data.len, raw,
	    im->data.len);
	if (strstr(im->fmt, "o=z") != nullptr) {
		if (!inflate_grow(raw, rawlen, &dec, &declen)) {
			free(raw);
			return (0);
		}
		free(raw);
		raw = dec;
		rawlen = declen;
	}
	try {
		out->assign(raw, raw + rawlen);
	} catch (const std::bad_alloc &) {
		free(raw);
		return (0);
	}
	free(raw);
	return (1);
}

/*
 * An f=64 image at another size: the same scaling as any other, in its own
 * numbers. They are linear light already, so the mean of an area is the
 * light of that area, and what is brighter than white stays so.
 */
static int
scale_f64(const struct dv_kimage *im, unsigned nw, unsigned nh, std::string *out)
{
	std::vector<unsigned char>	raw;
	size_t				np = (size_t)im->wpx * im->hpx;

	if (im->wpx == 0 || im->hpx == 0 || !image_raw(im, &raw) ||
	    raw.size() / 8 < np)
		return (0);
	std::vector<float>	a(np * 4);

	for (size_t i = 0; i < np; i++) {
		float	c[4];

		for (int k = 0; k < 4; k++) {
			c[k] = half_to_float(raw[i * 8 + k * 2] |
			    ((unsigned)raw[i * 8 + k * 2 + 1] << 8));
			if (!isfinite(c[k]))
				c[k] = 0;
		}
		a[i * 4 + 0] = c[0] * c[3];
		a[i * 4 + 1] = c[1] * c[3];
		a[i * 4 + 2] = c[2] * c[3];
		a[i * 4 + 3] = c[3];
	}
	std::vector<float>	c = scale_premult(a, im->wpx, im->hpx, nw, nh);

	out->resize((size_t)nw * nh * 8);
	for (size_t i = 0; i < (size_t)nw * nh; i++) {
		float	al = c[i * 4 + 3];

		for (int k = 0; k < 4; k++) {
			float		v = k == 3 ? al : al != 0 ? c[i * 4 + k] / al : 0;
			unsigned	h = float_to_half(v);

			(*out)[i * 8 + k * 2] = (char)(h & 0xff);
			(*out)[i * 8 + k * 2 + 1] = (char)(h >> 8);
		}
	}
	return (1);
}

/*
 * An f=64 image as eight-bit RGBA, for a terminal or a path that takes no
 * HDR: its linear light through the sRGB curve, what is brighter than white
 * or outside sRGB clipped to it.
 */
static unsigned char *
f64_rgba(const struct dv_kimage *im)
{
	std::vector<unsigned char>	 raw;
	size_t				 np = (size_t)im->wpx * im->hpx;
	unsigned char			*px;

	if (np == 0 || !image_raw(im, &raw) || raw.size() / 8 < np)
		return (nullptr);
	px = (unsigned char *)malloc(np * 4);
	if (px == nullptr)
		return (nullptr);
	for (size_t i = 0; i < np; i++)
		for (int k = 0; k < 4; k++) {
			float	c = half_to_float(raw[i * 8 + k * 2] |
				    ((unsigned)raw[i * 8 + k * 2 + 1] << 8));

			if (!(c > 0))
				c = 0;		/* negative, or not a number */
			if (c > 1)
				c = 1;
			if (k < 3)
				c = c <= 0.0031308f ? c * 12.92f :
				    1.055f * powf(c, 1 / 2.4f) - 0.055f;
			px[i * 4 + k] = (unsigned char)(c * 255.0f + 0.5f);
		}
	return (px);
}

/* A scene's live records, a line each. */
static std::string
scene_text(const struct dv_vscene *sc)
{
	std::string	text;

	for (const auto &r : sc->recs) {
		if (r.dead)
			continue;
		text += r.line;
		text += '\n';
	}
	return (text);
}

/*
 * A vector scene drawn as w by h straight RGBA pixels, malloc'd: what a
 * terminal that takes f=200 would draw of it at that size. The scene is
 * handed to the rasterizer whole, as the terminal was; if a record in it is
 * one the rasterizer will not have, which fails them all, they go again a
 * line at a time, so that one bad record costs itself and not the picture.
 * Raster images inside a scene (i records) are not drawn: the rasterizer is
 * given none. Text (t records) is not drawn either: see src/vector/vector.c.
 */
static unsigned char *
scene_raster(const struct dv_kimage *im, unsigned w, unsigned h)
{
	struct dv_vscene	*sc = im->vscene;

	if (sc == nullptr || im->wpx == 0 || im->hpx == 0 || w == 0 || h == 0)
		return (nullptr);
	if (sc->doc != nullptr && sc->doc_gen != im->gen) {
		vector_doc_free(sc->doc);
		sc->doc = nullptr;
	}
	if (sc->doc == nullptr) {
		std::string	text = scene_text(sc);

		sc->doc = vector_doc_new(im->wpx, im->hpx);
		if (sc->doc == nullptr)
			return (nullptr);
		if (vector_doc_apply(sc->doc, (const uint8_t *)text.data(),
		    text.size()) != nullptr) {
			size_t	at = 0;

			while (at < text.size()) {
				size_t	end = text.find('\n', at);

				if (end == std::string::npos)
					end = text.size();
				(void)vector_doc_apply(sc->doc,
				    (const uint8_t *)text.data() + at, end - at);
				at = end + 1;
			}
		}
		sc->doc_gen = im->gen;
	}
	return (vector_doc_render(sc->doc, w, h, nullptr, nullptr));
}

/*
 * A colour of Display P3 (gamut 1) or Rec. 2020 (gamut 2) as the nearest
 * sRGB: for a terminal that knows of no other. All three are encoded with
 * the sRGB curve (Silkie's wide gamut colours), so the primaries are
 * exchanged in its linear light and what sRGB cannot show is clipped.
 */
static const double gamut_m[2][3][3] = {
	{ {  1.2249401, -0.2249404,  0.0000000 },
	  { -0.0420569,  1.0420571,  0.0000000 },
	  { -0.0196376, -0.0786361,  1.0982735 } },
	{ {  1.6604910, -0.5876411, -0.0728499 },
	  { -0.1245505,  1.1328999, -0.0083494 },
	  { -0.0181508, -0.1005789,  1.1187297 } },
};

void
dv_gamut_to_srgb(int gamut, unsigned char rgb[3])
{
	double	lin[3], o;

	if (gamut != 1 && gamut != 2)
		return;
	for (int i = 0; i < 3; i++) {
		double	c = rgb[i] / 255.0;

		lin[i] = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
	}
	for (int i = 0; i < 3; i++) {
		o = gamut_m[gamut - 1][i][0] * lin[0] + gamut_m[gamut - 1][i][1] * lin[1] +
		    gamut_m[gamut - 1][i][2] * lin[2];
		o = o < 0 ? 0 : o > 1 ? 1 : o;
		o = o <= 0.0031308 ? o * 12.92 : 1.055 * pow(o, 1 / 2.4) - 0.055;
		rgb[i] = (unsigned char)(o * 255.0 + 0.5);
	}
}

/* Linear light in a gamut as linear extended sRGB: not clipped, so what
 * sRGB cannot show is negative, as f=64 allows. */
static void
gamut_lin_to_srgb(int gamut, float c[3])
{
	double	o[3];

	if (gamut != 1 && gamut != 2)
		return;
	for (int i = 0; i < 3; i++)
		o[i] = gamut_m[gamut - 1][i][0] * c[0] +
		    gamut_m[gamut - 1][i][1] * c[1] + gamut_m[gamut - 1][i][2] * c[2];
	for (int i = 0; i < 3; i++)
		c[i] = (float)o[i];
}

/*
 * An image deeper than eight bits, or with another curve, as f=64 for a
 * terminal that takes HDR pixels but not its own form: its light, in
 * linear extended sRGB, at nw by nh. Half floats keep eleven significant
 * bits, more than the ten of RGB10A2, and PQ and HLG above SDR white.
 */
static int
deep_f64(const struct dv_kimage *im, unsigned nw, unsigned nh, std::string *out)
{
	std::vector<float>	lin;
	long			f = key_val(im->fmt, strlen(im->fmt), 'f', 32);
	int			G = (int)key_val(im->fmt, strlen(im->fmt), 'G', 0);
	size_t			np = (size_t)im->wpx * im->hpx;

	if (!image_linear(im, &lin))
		return (0);
	if (!dv_kitty_fmt_gamut(f))
		G = 0;
	for (size_t i = 0; i < np; i++) {
		float	*c = &lin[i * 4];

		gamut_lin_to_srgb(G, c);
		if (nw != im->wpx || nh != im->hpx)
			for (int k = 0; k < 3; k++)
				c[k] *= c[3];		/* premultiplied, to scale */
	}
	if (nw != im->wpx || nh != im->hpx) {
		lin = scale_premult(lin, im->wpx, im->hpx, nw, nh);
		for (size_t i = 0; i < (size_t)nw * nh; i++)
			for (int k = 0; k < 3; k++)
				lin[i * 4 + k] = lin[i * 4 + 3] > 0 ?
				    lin[i * 4 + k] / lin[i * 4 + 3] : 0;
	}
	try {
		out->resize((size_t)nw * nh * 8);
	} catch (const std::bad_alloc &) {
		return (0);
	}
	for (size_t i = 0; i < (size_t)nw * nh * 4; i++) {
		unsigned	h = float_to_half(lin[i]);

		(*out)[i * 2] = (char)(h & 0xff);
		(*out)[i * 2 + 1] = (char)(h >> 8);
	}
	return (1);
}

/* The bytes of one encoding of w x h RGBA pixels: f is 301, 302, 304 or 308
 * (gray, from the red channel), 516 (RGB565), 24 or 32. */
static std::string
pack_pixels(const unsigned char *px, unsigned w, unsigned h, int f)
{
	std::string	out;
	size_t		np = (size_t)w * h;

	if (f == 32)
		out.assign((const char *)px, np * 4);
	else if (f == 24) {
		out.resize(np * 3);
		for (size_t i = 0; i < np; i++)
			memcpy(&out[i * 3], px + i * 4, 3);
	} else if (f == 516) {
		out.resize(np * 2);
		for (size_t i = 0; i < np; i++) {
			const unsigned char	*p = px + i * 4;
			unsigned		 v = (((p[0] * 31u + 127) / 255) << 11) |
			    (((p[1] * 63u + 127) / 255) << 5) |
			    ((p[2] * 31u + 127) / 255);

			out[i * 2] = (char)(v & 0xff);
			out[i * 2 + 1] = (char)(v >> 8);
		}
	} else {
		int		bits = f - 300;
		size_t		row = ((size_t)w * (size_t)bits + 7) / 8;
		unsigned	m = (1u << bits) - 1;

		out.assign(row * h, '\0');
		for (size_t y = 0; y < h; y++)
			for (size_t x = 0; x < w; x++) {
				unsigned	q = (px[(y * w + x) * 4] * m + 127) / 255;
				size_t		bit = x * (size_t)bits;

				out[y * row + bit / 8] |=
				    (char)(q << (8 - bits - bit % 8));
			}
	}
	return (out);
}

/* A palette image (f=401-408) of pixels with at most 1 << bits colours:
 * the palette, slot by slot as the colours come, then the indices. */
static std::string
pack_palette(const unsigned char *px, unsigned w, unsigned h, int bits)
{
	std::unordered_map<uint32_t, unsigned>	slot;
	size_t					row = ((size_t)w * (size_t)bits + 7) / 8;
	size_t					pal = (size_t)4 << bits;
	std::string				out(pal + row * h, '\0');

	for (size_t y = 0; y < h; y++)
		for (size_t x = 0; x < w; x++) {
			const unsigned char	*p = px + (y * w + x) * 4;
			uint32_t		 c = ((uint32_t)p[0] << 24) |
						    ((uint32_t)p[1] << 16) |
						    ((uint32_t)p[2] << 8) | p[3];
			auto [it, fresh] = slot.try_emplace(c, (unsigned)slot.size());
			size_t			 bit = x * (size_t)bits;

			if (fresh)
				memcpy(&out[it->second * 4], p, 4);
			out[pal + y * row + bit / 8] |=
			    (char)(it->second << (8 - bits - bit % 8));
		}
	return (out);
}

/* zlib, for o=z. Empty when it would not be smaller. */
static std::string
pack_deflate(const std::string &raw)
{
	uLongf		n = compressBound((uLong)raw.size());
	std::string	out((size_t)n, '\0');

	if (raw.size() > (size_t)ULONG_MAX ||
	    compress2((Bytef *)out.data(), &n, (const Bytef *)raw.data(),
	    (uLong)raw.size(), 6) != Z_OK || n >= raw.size())
		return ("");
	out.resize((size_t)n);
	return (out);
}

/*
 * The tightest form of this picture for a profile. Its pixels are first
 * reduced to what the display shows -- gray at its levels, or each channel
 * at its width -- which changes nothing a display of eight bits a channel
 * would show. Then the narrowest format holding them exactly that the
 * client takes -- packed gray, RGB565 or a palette image -- deflated when
 * that is smaller. The image's own form wins when nothing was reduced and
 * it is no bigger, since it is already here.
 *
 * Two more things a client may need first. Pixels in a gamut it knows
 * nothing of (G=) become the nearest sRGB. And a picture of a layered
 * window, for a client whose cell is not the one programs draw for, is
 * scaled by its cell over theirs, so that it covers there the cells it
 * covers everywhere.
 *
 * And formats a terminal may not take. Kitty's indexed colour goes as it is
 * to a terminal that lists it, so that it follows that terminal's colours,
 * unless its window has colours of its own (OSC 4, 10, 11, 12): then it is
 * sent in those. A palette image goes as it is to a terminal that lists
 * it. Pixels of more than eight bits, or with another curve (E=), go as
 * they are to a terminal that lists them, as half floats (f=64) to one that
 * takes HDR pixels, and otherwise as eight bits of sRGB, what is brighter
 * than SDR white clipped.
 */
static void
pack_build(struct dv_kimage *im, const struct pack_profile *pr,
    struct dv_kpack *k)
{
	unsigned char	*px = nullptr;
	unsigned	 w, h;
	long		 f0 = key_val(im->fmt, strlen(im->fmt), 'f', 32);
	long		 G = key_val(im->fmt, strlen(im->fmt), 'G', 0);
	long		 E = key_val(im->fmt, strlen(im->fmt), 'E', 0);
	int		 kind = fmt_kind(f0), bit = dv_kitty_fmt_bit(f0);
	int		 reduced = 0, opaque = 1, allgray = 1, exact565 = 1;
	int		 f, own_ok, whole;
	bool		 have = false, sized = false;
	size_t		 np;
	std::string	 raw, z, keys;

	k->fmt.clear();
	k->data.len = 0;
	own_ok = (bit == 0 || (pr->packed & bit)) &&
	    (E == 0 || (pr->packed & DV_FMT_E)) &&
	    !((kind == FK_IDX8 || kind == FK_IDX16) && im->pal != nullptr &&
	    im->pal->any);
	/* Would it show there as it is, nothing of it lost or changed? */
	whole = !pr->gray && pr->bits[0] >= 8 && pr->bits[1] >= 8 &&
	    pr->bits[2] >= 8 && pr->scw == 0 &&
	    (!dv_kitty_fmt_gamut(f0) || G == 0 || pr->wg);
	if (im->vscene != nullptr && !pr->vector) {
		/*
		 * A terminal that cannot draw a scene is sent it drawn: by
		 * the rasterizer here, at the size it shows it, so it is as
		 * sharp there as its pixels allow.
		 */
		w = pr->scw != 0 ? dv_scale_size(im->wpx, pr->cw, pr->scw) : im->wpx;
		h = pr->scw != 0 ? dv_scale_size(im->hpx, pr->ch, pr->sch) : im->hpx;
		px = w != 0 && h != 0 ? scene_raster(im, w, h) : nullptr;
		if (px == nullptr) {
			k->fmt = "-";
			return;
		}
		have = sized = true;
		reduced = 1;
		G = 0;
		f0 = 32;
	} else if (im->vscene != nullptr && pr->scw != 0 && im->wpx != 0 && im->hpx != 0) {
		/* A scene at another scale: its records, not pixels. */
		unsigned	nw = dv_scale_size(im->wpx, pr->cw, pr->scw);
		unsigned	nh = dv_scale_size(im->hpx, pr->ch, pr->sch);
		std::string	text = scene_text_scaled(im->vscene, im->wpx,
				    im->hpx, nw, nh);

		k->fmt = fmt_resized(im->fmt, nw, nh);
		b64_encode((const unsigned char *)text.data(), text.size(), &k->data);
		return;
	}
	if (f0 == 64 && !pr->hdr && !im->gpu && !have) {
		/* HDR pixels for a terminal that takes none: what of them
		 * eight bits show, then as any other pixels. */
		px = f64_rgba(im);
		if (px == nullptr) {
			k->fmt = "-";
			return;
		}
		w = im->wpx;
		h = im->hpx;
		have = true;
		reduced = 1;
		G = 0;
		f0 = 32;
	}
	if (f0 == 64 && pr->scw != 0 && !im->gpu) {
		unsigned	nw = dv_scale_size(im->wpx, pr->cw, pr->scw);
		unsigned	nh = dv_scale_size(im->hpx, pr->ch, pr->sch);

		if (!scale_f64(im, nw, nh, &raw))
			goto expand;
		z = pack_deflate(raw);
		k->fmt = fmt_resized(im->fmt, nw, nh);
		if (!z.empty()) {
			k->fmt += ",o=z";
			raw.swap(z);
		}
		b64_encode((const unsigned char *)raw.data(), raw.size(), &k->data);
		return;
	}
	if (!have && !im->gpu && image_deep(im)) {
		unsigned	nw = im->wpx, nh = im->hpx;

		if (own_ok && whole)
			return;		/* as it is: it shows all of it */
		if (pr->hdr && !pr->gray) {
			if (pr->scw != 0) {
				nw = dv_scale_size(im->wpx, pr->cw, pr->scw);
				nh = dv_scale_size(im->hpx, pr->ch, pr->sch);
			}
			if (nw != 0 && nh != 0 && deep_f64(im, nw, nh, &raw)) {
				z = pack_deflate(raw);
				k->fmt = strf_kitty("f=64,s=%u,v=%u", nw, nh);
				if (key_val(im->fmt, strlen(im->fmt), 'T', 0) == 1)
					k->fmt += ",T=1";
				if (!z.empty()) {
					k->fmt += ",o=z";
					raw.swap(z);
				}
				b64_encode((const unsigned char *)raw.data(),
				    raw.size(), &k->data);
				return;
			}
		}
		reduced = 1;		/* eight bits of it, below */
		own_ok = 0;
	}
	if (!have && !im->gpu && own_ok && whole &&
	    (kind == FK_IDX8 || kind == FK_IDX16 || kind == FK_PAL))
		return;			/* in the terminal's colours, or its own palette */
	if (!have && (im->gpu || im->vscene != nullptr || f0 == 64 || f0 == 200 ||
	    !dv_kimage_rgba(im, &px, &w, &h)))
		goto expand;		/* not pixels we can work with */
	if (!dv_kitty_fmt_gamut(f0))
		G = 0;			/* the others say their own colours */
	if (G != 0 && !pr->wg && !pr->gray) {
		for (size_t i = 0; i < (size_t)w * h; i++)
			dv_gamut_to_srgb((int)G, px + i * 4);
		G = 0;
		reduced = 1;
	}
	if (pr->scw != 0) {
		unsigned	nw = sized ? w : dv_scale_size(w, pr->cw, pr->scw);
		unsigned	nh = sized ? h : dv_scale_size(h, pr->ch, pr->sch);

		if (nw != w || nh != h) {
			auto		 scaled = scale_rgba(px, w, h, nw, nh);
			unsigned char	*npx = (unsigned char *)realloc(px,
					    scaled.size() ? scaled.size() : 1);

			if (npx == nullptr) {
				free(px);
				dv_fatal("out of memory");
			}
			px = npx;
			memcpy(px, scaled.data(), scaled.size());
			w = nw;
			h = nh;
		}
		reduced = 1;		/* its own form is another size */
	}
	np = (size_t)w * h;
	for (size_t i = 0; i < np; i++) {
		unsigned char	*p = px + i * 4;

		if (pr->gray) {
			unsigned	y = quant(luma(p), pr->bits[0]);

			reduced |= p[0] != y || p[1] != y || p[2] != y;
			p[0] = p[1] = p[2] = (unsigned char)y;
		} else
			for (int c = 0; c < 3; c++) {
				unsigned	q = quant(p[c], pr->bits[c]);

				reduced |= p[c] != q;
				p[c] = (unsigned char)q;
			}
		opaque &= p[3] == 255;
		allgray &= p[0] == p[1] && p[1] == p[2];
		exact565 &= quant(p[0], 5) == p[0] && quant(p[1], 6) == p[1] &&
		    quant(p[2], 5) == p[2];
	}
	f = opaque ? 24 : 32;
	if (opaque && allgray) {
		/* The fewest gray bits that hold every level exactly. */
		for (int g : { 1, 2, 4, 8 }) {
			unsigned	step = 255 / ((1u << g) - 1);
			int		ok = (pr->packed & dv_kitty_fmt_bit(300 + g)) != 0;

			for (size_t i = 0; i < np && ok; i++)
				ok = px[i * 4] % step == 0;
			if (ok) {
				f = 300 + g;
				break;
			}
		}
	}
	if ((f == 24 || f == 32) && opaque && exact565 &&
	    (pr->packed & DV_FMT_565))
		f = 516;
	if (pr->packed & (DV_FMT_P1 | DV_FMT_P2 | DV_FMT_P4 | DV_FMT_P8)) {
		/* Few colours: a palette of them, if that is smaller. */
		std::unordered_set<uint32_t>	colours;
		uint64_t			best;

		for (size_t i = 0; i < np && colours.size() <= 256; i++) {
			const unsigned char	*p = px + i * 4;

			colours.insert(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
			    ((uint32_t)p[2] << 8) | p[3]);
		}
		if (fmt_size(f, w, h, &best) == 0 && colours.size() <= 256)
			for (int b : { 1, 2, 4, 8 }) {
				uint64_t	need;

				if (colours.size() > (1u << b) ||
				    !(pr->packed & dv_kitty_fmt_bit(400 + b)) ||
				    fmt_size(400 + b, w, h, &need) != 0)
					continue;
				if (need < best) {
					f = 400 + b;
					best = need;
				}
				break;
			}
	}
	raw = fmt_kind(f) == FK_PAL ? pack_palette(px, w, h, fmt_bits(f)) :
	    pack_pixels(px, w, h, f);
	free(px);
	z = pack_deflate(raw);
	keys = strf_kitty("f=%d,s=%u,v=%u", f, w, h);
	if (G != 0 && !pr->gray && dv_kitty_fmt_gamut(f))
		keys += strf_kitty(",G=%ld", G);
	if (!z.empty()) {
		keys += ",o=z";
		raw.swap(z);
	}
	if (!reduced && own_ok && im->data.len <= (raw.size() + 2) / 3 * 4)
		return;			/* its own form: no bigger, exact */
	k->fmt = keys;
	b64_encode((const unsigned char *)raw.data(), raw.size(), &k->data);
	return;

expand:
	/* Pixels the client did not list, that did not decode: as they are
	 * only if it can take them. Nor at another scale: a picture at its
	 * own size where a scaled one is placed would be the wrong part of
	 * it, in the wrong place. */
	if (!own_ok || pr->scw != 0)
		k->fmt = "-";
}

/*
 * Send an image as this client should get it: as it is for no client in
 * particular, otherwise packed for what that client's display shows.
 */
static int
image_emit_for(struct dv_kimage *im, unsigned hid, struct dv_buf *out,
    const struct dv_view *v, int scaled = 0)
{
	struct pack_profile	 pr;
	struct dv_kpack		*k = nullptr;
	struct dv_kimage	 ex;
	std::string		 key;
	int			 ok;

	if (v == nullptr || !im->done)
		return (image_emit(im, hid, out));
	pr = pack_profile_of(v, scaled && dv_kitty_scalable(im));
	key = strf_kitty("%x %d %d %d %d %d %d %d %u/%u %u/%u", (unsigned)pr.packed,
	    pr.gray, pr.bits[0], pr.bits[1], pr.bits[2], pr.wg, pr.vector,
	    pr.hdr, pr.cw, pr.scw, pr.ch, pr.sch);
	if (im->packs == nullptr)
		im->packs = new struct dv_kpacks;
	/*
	 * A form made of an earlier transmission is of pixels that are gone.
	 * Dropping those here keeps the list to the forms the terminals of
	 * this transmission asked for; dv_kitty_packs_forget empties it when
	 * the terminals themselves change.
	 */
	for (size_t i = 0; i < im->packs->v.size(); ) {
		if (im->packs->v[i].gen != im->gen && im->packs->v[i].key != key) {
			dv_buf_free(&im->packs->v[i].data);
			im->packs->v.erase(im->packs->v.begin() + (long)i);
		} else
			i++;
	}
	for (auto &c : im->packs->v)
		if (c.key == key)
			k = &c;
	if (k == nullptr) {
		im->packs->v.push_back({ 0, key, "", {} });
		k = &im->packs->v.back();
		k->gen = im->gen + 1;	/* not built yet */
	}
	if (k->gen != im->gen) {
		pack_build(im, &pr, k);
		k->gen = im->gen;
	}
	if (k->fmt.empty())
		return (image_emit(im, hid, out));
	if (k->fmt == "-")
		return (0);
	ex = *im;
	ex.data = k->data;		/* borrowed: image_emit only reads it */
	dv_strlcpy(ex.fmt, k->fmt.c_str(), sizeof ex.fmt);
	ok = image_emit(&ex, hid, out);
	return (ok);
}

/*
 * The virtual placement, at a depth for a terminal that listed depth.1. A
 * placement takes the terminal's depth when it is made, and whatever the
 * terminal was left at is not ours to rely on, so it is said each time.
 */
static void
place_emit(const struct dv_kimage *im, const struct dv_kplace *p,
    struct dv_buf *out, const struct dv_view *v, uint32_t depth)
{
	char	hdr[200];
	int	n;

	if (v != nullptr && v->depth)
		dv_depth_select(depth, out);
	n = snprintf(hdr, sizeof hdr, "\033_Ga=p,U=1,i=%u,p=1,c=%u,r=%u",
	    p->dv_id, p->cols, p->rows);
	if (n > 0)
		dv_buf_append(out, hdr, (size_t)n);
	if (p->sx != 0 || p->sy != 0 || p->sw != im->wpx || p->sh != im->hpx) {
		n = snprintf(hdr, sizeof hdr, ",x=%u,y=%u,w=%u,h=%u", p->sx,
		    p->sy, p->sw, p->sh);
		if (n > 0)
			dv_buf_append(out, hdr, (size_t)n);
	}
	dv_buf_append(out, ",q=2\033\\", 7);
	if (v != nullptr && v->depth && depth != 0)
		dv_depth_select(0, out);
}

void
dv_kitty_transmit(struct dv_window *w, const struct dv_kplace *p,
    struct dv_buf *out, const struct dv_view *v, uint32_t depth)
{
	struct dv_kimage	*im = &w->kimg->at(p->img);

	if (!p->used)
		return;
	if (!image_emit_for(im, p->dv_id, out, v))
		return;
	place_emit(im, p, out, v, depth == UINT32_MAX ? p->depth : depth);
}

void
dv_kitty_replace(struct dv_window *w, const struct dv_kplace *p,
    struct dv_buf *out, const struct dv_view *v, uint32_t depth)
{
	if (!p->used)
		return;
	place_emit(&w->kimg->at(p->img), p, out, v, depth);
}

void
dv_kitty_delete(unsigned dv_id, struct dv_buf *out)
{
	char	d[64];
	int	n = snprintf(d, sizeof d, "\033_Ga=d,d=I,i=%u,q=2\033\\", dv_id);

	if (n > 0)
		dv_buf_append(out, d, (size_t)n);
}

/* ---- layered windows (see dv_kplace) ---- */

/* Does this client show the window's direct placements as real ones? */
int
dv_kitty_layered(const struct dv_window *w, const struct dv_view *v)
{
	return (w->klayered && v->gfx);
}

/*
 * The forms of every image made for one terminal or another are of the
 * terminals there were: their cells, their displays. When those change the
 * forms are made again as they are asked for, and the old ones would only
 * pile up, one per size a terminal's font has ever been.
 */
void
dv_kitty_packs_forget(struct dv_session *s)
{
	struct dv_window	*w;

	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		if (w->kimg == nullptr)
			continue;
		for (auto &[key, im] : *w->kimg)
			packs_free(&im);
	}
}

/* The client's id for a placement's image: the placement's own while each
 * placement is an image of its own there, the image's when it is layered. */
unsigned
dv_kitty_host_image(struct dv_window *w, const struct dv_view *v, unsigned dv_id)
{
	if (!dv_kitty_layered(w, v) || w->screen.kpl == nullptr || w->kimg == nullptr)
		return (dv_id);
	auto it = w->screen.kpl->find(dv_id);

	if (it == w->screen.kpl->end() || it->second.virt ||
	    !w->kimg->contains(it->second.img))
		return (dv_id);
	return (dv_kitty_layer_id(w, it->second.img));
}

/* The host's id for an image of a layered window: one copy there, however
 * many placements show it. */
unsigned
dv_kitty_layer_id(struct dv_window *w, uint64_t img)
{
	struct dv_kimage	*im = &w->kimg->at(img);

	if (im->hid == 0)
		im->hid = host_id_alloc();
	return (im->hid);
}

/* Send a client that copy, as its display takes it. */
int
dv_kitty_layer_send(struct dv_window *w, uint64_t img, struct dv_buf *out,
    struct dv_view *v)
{
	struct dv_kimage	*im = &w->kimg->at(img);
	unsigned		 hid = dv_kitty_layer_id(w, img);

	if (!im->done || !image_emit_for(im, hid, out, v, 1))
		return (0);
	if (v->limg_gen == nullptr)
		v->limg_gen = new std::unordered_map<unsigned, unsigned>;
	(*v->limg_gen)[hid] = im->gen;
	/* A stroke that terminal was drawing on it is in what it was sent. */
	if (w->screen.kpl != nullptr)
		for (const auto &[id, p] : *w->screen.kpl)
			if (p.img == img)
				dv_silkie_ink_resent(w, v, id);
	return (1);
}

/*
 * What a client needs before the placements it is about to be sent, and has
 * not got: session fonts, then images used without a placement. What it
 * holds and nothing needs any more goes.
 */
void
dv_kitty_sync(struct dv_session *s, struct dv_view *v, struct dv_buf *out)
{
	struct dv_window		*w;
	std::unordered_set<unsigned>	 live;

	/*
	 * Kitty's indexed colour is in its window's colours: when a program
	 * changes them (OSC 4, 10, 11, 12, 104, 110-112, a pop of the colour
	 * stack), every placement of such an image is sent again in the new.
	 */
	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		bool		have = false;
		uint64_t	stamp = 0;

		if (w->kimg == nullptr)
			continue;
		for (auto &[img, im] : *w->kimg) {
			int	k = fmt_kind(key_val(im.fmt, strlen(im.fmt), 'f', 32));

			if (!im.done || (k != FK_IDX8 && k != FK_IDX16))
				continue;
			im.pal = &w->screen.pal;
			if (!have) {
				stamp = pal_stamp(&w->screen.pal);
				have = true;
			}
			if (im.pal_stamp == stamp)
				continue;
			im.pal_stamp = stamp;
			im.gen++;
			image_shown_again(w, img);
		}
	}
	if (!v->gfx)
		return;
	if (v->vector) {
		TAILQ_FOREACH(w, &s->desktop.windows, entry) {
			if (w->vwin == nullptr)
				continue;
			for (const auto &f : w->vwin->fonts) {
				if (f.pending)
					continue;
				live.insert(f.serial);
				if (v->vfont_sent != nullptr &&
				    v->vfont_sent->contains(f.serial))
					continue;
				std::string	frames;

				vreq_frames(f.body, vreq_id(), 1, &frames);
				dv_buf_append(out, frames.data(), frames.size());
				if (v->vfont_sent == nullptr)
					v->vfont_sent = new std::unordered_set<unsigned>;
				v->vfont_sent->insert(f.serial);
			}
		}
		if (v->vfont_sent != nullptr)
			std::erase_if(*v->vfont_sent, [&live, out](unsigned serial) {
				std::string	frames;

				if (live.contains(serial))
					return (false);
				vreq_frames("{\"op\":\"release\",\"id\":\"" +
				    host_handle(serial) + "\"}", vreq_id(), 1, &frames);
				dv_buf_append(out, frames.data(), frames.size());
				return (true);
			});
	}
	live.clear();
	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		if (w->kres == nullptr)
			continue;
		for (const auto &[child, hid] : *w->kres) {
			uint64_t	 img = image_find(w, child);
			struct dv_kimage *im;

			if (img == 0 || !w->kimg->at(img).done)
				continue;
			im = &w->kimg->at(img);
			live.insert(hid);
			if (v->kres_gen == nullptr)
				v->kres_gen = new std::unordered_map<unsigned, unsigned>;
			auto it = v->kres_gen->find(hid);

			if (it != v->kres_gen->end() && it->second == im->gen)
				continue;
			if (image_emit_for(im, hid, out, v))
				(*v->kres_gen)[hid] = im->gen;
		}
	}
	if (v->kres_gen != nullptr)
		std::erase_if(*v->kres_gen, [&live, out](const auto &entry) {
			if (live.contains(entry.first))
				return (false);
			dv_kitty_delete(entry.first, out);
			return (true);
		});
}

__attribute__((weak)) int
dv_kitty_send_primary(struct dv_session *, const char *, size_t)
{
	return (-1);
}

__attribute__((weak)) void
dv_kitty_send_patch(struct dv_session *, unsigned, unsigned, const char *, size_t,
    const struct dv_peer *)
{
}

__attribute__((weak)) void
dv_kitty_send_patch_to(const struct dv_peer *, unsigned, unsigned, const char *,
    size_t)
{
}

__attribute__((weak)) void
dv_kitty_peer_stale(const struct dv_peer *, unsigned)
{
}

__attribute__((weak)) void
dv_kitty_send_layer_patch(struct dv_session *, unsigned, unsigned, unsigned,
    const char *, size_t, const struct dv_peer *, int)
{
}

__attribute__((weak)) void
dv_kitty_send_layer_patch_to(const struct dv_peer *, unsigned, unsigned,
    const char *, size_t)
{
}

__attribute__((weak)) void
dv_kitty_sync_views(struct dv_session *)
{
}

/* Does the host this window is shown on take kitty graphics? A window
 * with no session (a unit test) is treated as if it did. */
int
dv_kitty_host_gfx(const struct dv_window *w)
{
	return (w->session == nullptr || w->session->desktop.gfx);
}

/*
 * Store a bitmap the window decoded itself (a sixel) as an image of its
 * own -- zlib-deflated RGBA, a form the host takes -- and place it at
 * the cursor where the sixel would have appeared. The cursor then goes
 * to the start of the line below the picture (sixel scrolling), or to
 * its right if the program asked (mode 8452); at_home (DECSDM) paints
 * it at the top left and leaves the cursor alone.
 */
void
dv_kitty_place_rgba(struct dv_window *w, const unsigned char *rgba,
    unsigned wpx, unsigned hpx, int at_home, int cursor_right)
{
	struct dv_screen	*s = &w->screen;
	struct dv_kimage	*im;
	struct dv_kplace	*p;
	struct dv_kreq		 q;
	uLongf			 zlen;
	unsigned char		*z;
	unsigned		 sr, sc, i;
	uint64_t		 img;

	if (wpx == 0 || hpx == 0)
		return;
	zlen = compressBound((uLong)wpx * hpx * 4);
	z = (unsigned char *)malloc(zlen);
	if (z == nullptr)
		return;
	if (compress2(z, &zlen, rgba, (uLong)wpx * hpx * 4, 6) != Z_OK) {
		free(z);
		return;
	}
	img = image_alloc(w, 0);
	im = &w->kimg->at(img);
	snprintf(im->fmt, sizeof im->fmt, "f=32,s=%u,v=%u,o=z", wpx, hpx);
	im->data.len = 0;
	b64_encode(z, (size_t)zlen, &im->data);
	free(z);
	im->done = 1;
	im->wpx = wpx;
	im->hpx = hpx;
	memset(&q, 0, sizeof q);
	q.C = 1;
	if (at_home) {
		sr = s->cr;
		sc = s->cc;
		s->cr = s->cc = 0;
		place_direct(w, img, &q);
		s->cr = sr;
		s->cc = sc;
		return;
	}
	if (cursor_right) {
		q.C = 0;			/* kitty's rule: right of the last row */
		place_direct(w, img, &q);
		return;
	}
	p = place_direct(w, img, &q);
	unsigned rows = p->rows;
	for (i = 0; i < rows; i++)		/* the line below, scrolling */
		dv_screen_linefeed(s);
	s->cc = 0;
}
