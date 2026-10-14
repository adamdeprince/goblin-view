/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Multiplexer-side kitty graphics. Original code against the public
 * protocol; the behaviour tracks Adam's tmux kitty-graphics branch
 * (docs/tmux-tracking.md), not its source.
 *
 * A goblin-view window is its own terminal, so it terminates the kitty protocol:
 * it stores the images a program transmits (direct, file or shared
 * memory medium), answers its queries, and turns each display request
 * into a placement on the window's grid -- kitty's unit of display,
 * floating above the text as it does in kitty itself. A redraw of the
 * text beneath leaves it, scrolling carries it, ED / RIS / leaving the
 * alternate screen clear it, and a delete removes it by image id. The
 * program's bytes never reach the host.
 *
 * goblin-view is in turn a kitty CLIENT of the host: each placement is one image
 * on the host, sent under a host-global id with a virtual (U=1)
 * placement of its cell size and source rectangle. The compositor then
 * paints Unicode placeholder cells wherever the placement is visible, so
 * cropping to the window, occlusion by other windows and moving the
 * window are ordinary cell compositing -- no pixels are re-sent for them.
 */

#include "dv.hpp"
#include "silkie.hpp"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

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

/* Copy the transmit keys the host needs to decode the same pixels. */
static void
capture_fmt(struct dv_kimage *im, const char *c, size_t n)
{
	static const char	keys[] = "fsv";
	char			*o = im->fmt;
	size_t			 room = sizeof im->fmt;
	int			 first = 1, ki, w;

	im->fmt[0] = '\0';
	for (ki = 0; keys[ki] != '\0'; ki++) {
		long	v = key_val(c, n, keys[ki], -1);

		if (v < 0)
			continue;
		w = snprintf(o, room, "%s%c=%ld", first ? "" : ",", keys[ki], v);
		if (w <= 0 || (size_t)w >= room)
			return;
		o += w;
		room -= (size_t)w;
		first = 0;
	}
	{
		long	G = key_val(c, n, 'G', -1);

		if (G > 0) {
			w = snprintf(o, room, "%sG=%ld", first ? "" : ",", G);
			if (w <= 0 || (size_t)w >= room)
				return;
			o += w;
			room -= (size_t)w;
			first = 0;
		}
	}
	if (key_chr(c, n, 'o', 0) == 'z')	/* zlib-deflated pixels */
		snprintf(o, room, "%so=z", first ? "" : ",");
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
static int
read_medium(struct dv_kimage *im, char medium, const char *payload,
    size_t plen, long size, long off)
{
	char		 path[1024];
	size_t		 n;
	int		 fd, ok = 0;
	struct stat	 st;
	void		*m;

	n = b64_decode(payload, plen, (unsigned char *)path, sizeof path - 1);
	path[n] = '\0';
	if (n == 0)
		return (0);
	if (medium == 's')
		fd = shm_open(path, O_RDONLY, 0);
	else
		fd = open(path, O_RDONLY);
	if (fd < 0)
		return (0);
	if (fstat(fd, &st) == 0 && off >= 0 && off < st.st_size) {
		if (size <= 0 || size > st.st_size - off)
			size = st.st_size - off;
		m = mmap(NULL, (size_t)(off + size), PROT_READ, MAP_SHARED,
		    fd, 0);
		if (m != MAP_FAILED) {
			im->data.len = 0;
			b64_encode((const unsigned char *)m + off,
			    (size_t)size, &im->data);
			munmap(m, (size_t)(off + size));
			ok = 1;
		}
	}
	close(fd);
	if (medium == 't' && strstr(path, "tmp") != NULL)
		unlink(path);
	else if (medium == 's')
		shm_unlink(path);
	return (ok);
}

/* Inflate a zlib stream of unknown size into a grown buffer. */
static int
inflate_grow(const unsigned char *in, size_t inlen, unsigned char **out,
    size_t *outlen)
{
	z_stream	zs;
	size_t		cap = inlen * 4 + 4096;
	unsigned char	*buf = (unsigned char *)malloc(cap);
	int		rc;

	if (buf == nullptr)
		return (0);
	memset(&zs, 0, sizeof zs);
	if (inflateInit(&zs) != Z_OK) {
		free(buf);
		return (0);
	}
	zs.next_in = (Bytef *)in;
	zs.avail_in = (uInt)inlen;
	zs.next_out = buf;
	zs.avail_out = (uInt)cap;
	for (;;) {
		rc = inflate(&zs, Z_NO_FLUSH);
		if (rc == Z_STREAM_END)
			break;
		if (rc != Z_OK && rc != Z_BUF_ERROR) {
			inflateEnd(&zs);
			free(buf);
			return (0);
		}
		if (zs.avail_out == 0) {
			size_t	used = cap;
			unsigned char	*nb = (unsigned char *)realloc(buf,
			    cap * 2);

			if (nb == nullptr) {
				inflateEnd(&zs);
				free(buf);
				return (0);
			}
			buf = nb;
			zs.next_out = buf + used;
			zs.avail_out = (uInt)(cap);
			cap *= 2;
		} else if (rc == Z_BUF_ERROR)
			break;			/* no progress, no more input */
	}
	*outlen = zs.total_out;
	*out = buf;
	inflateEnd(&zs);
	return (1);
}

/*
 * Decode a stored image back to RGBA, so it can be re-emitted as sixel
 * to a host that does not take kitty graphics. Handles the transmit
 * formats programs use: f=32 (RGBA), f=24 (RGB), f=100 (PNG), each
 * optionally zlib-deflated (o=z). Returns a malloc'd w*h*4 buffer.
 */
int
dv_kimage_rgba(const struct dv_kimage *im, unsigned char **rgba, unsigned *w,
    unsigned *h)
{
	long		f = key_val(im->fmt, strlen(im->fmt), 'f', 32);
	int		zed = strstr(im->fmt, "o=z") != nullptr;
	unsigned char	*raw, *dec = nullptr, *px;
	size_t		rawlen, declen = 0;

	if (im->gpu || im->data.len == 0)
		return (0);
	raw = (unsigned char *)malloc(im->data.len);
	if (raw == nullptr)
		return (0);
	rawlen = b64_decode((const char *)im->data.data, im->data.len, raw,
	    im->data.len);
	if (zed) {
		if (!inflate_grow(raw, rawlen, &dec, &declen)) {
			free(raw);
			return (0);
		}
		free(raw);
		raw = dec;
		rawlen = declen;
	}
	if (f == 100) {			/* PNG */
		int	ok = dv_png_decode(raw, rawlen, rgba, w, h);

		free(raw);
		return (ok);
	}
	if (im->wpx == 0 || im->hpx == 0) {
		free(raw);
		return (0);
	}
	px = (unsigned char *)malloc((size_t)im->wpx * im->hpx * 4);
	if (px == nullptr) {
		free(raw);
		return (0);
	}
	if (f == 32) {
		size_t	need = (size_t)im->wpx * im->hpx * 4;

		if (rawlen < need) {
			free(raw);
			free(px);
			return (0);
		}
		memcpy(px, raw, need);
	} else if (f == 24) {
		size_t	i, np = (size_t)im->wpx * im->hpx;

		if (rawlen < np * 3) {
			free(raw);
			free(px);
			return (0);
		}
		for (i = 0; i < np; i++) {
			px[i * 4 + 0] = raw[i * 3 + 0];
			px[i * 4 + 1] = raw[i * 3 + 1];
			px[i * 4 + 2] = raw[i * 3 + 2];
			px[i * 4 + 3] = 255;
		}
	} else {
		free(raw);
		free(px);
		return (0);
	}
	free(raw);
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
	w->kimg->erase(it);
	if (w->kreassem == img)
		w->kreassem = 0;
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

static struct dv_kplace *
place_alloc(struct dv_screen *s)
{
	/* IDs are independent of window IDs and placement counts. Keep the
	 * scaled-text namespace separate and never reuse a stale host ID. */
	static uint64_t next_id = DV_KITTY_FIRST_ID;

	if (next_id == DV_STEXT_FIRST)
		next_id = 0x30000000u;
	if (next_id > UINT32_MAX)
		dv_fatal("kitty placement ID space exhausted");
	if (s->kpl == nullptr)
		s->kpl = new std::unordered_map<unsigned, dv_kplace>;
	unsigned id = (unsigned)next_id++;
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
	dv_silkie_ink_placed(w);
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
	char	r[96];
	int	n;

	if (w->pty_fd < 0)
		return;
	if (pid > 0)
		n = snprintf(r, sizeof r, "\033_Gi=%ld,p=%ld;%s\033\\", id, pid,
		    msg);
	else
		n = snprintf(r, sizeof r, "\033_Gi=%ld;%s\033\\", id, msg);
	if (n > 0)
		(void)!write(w->pty_fd, r, (size_t)n);
}

void
dv_kitty_apc(struct dv_window *w, const unsigned char *cap, size_t len)
{
	struct dv_kimage	*im;
	struct dv_kreq		 rq;
	const char		*c, *data;
	size_t			 clen, dlen, i;
	char			 act, medium;
	long			 id, m, q;
	uint64_t		 img;

	if (len < 1 || cap[0] != 'G')
		return;
	c = (const char *)cap + 1;
	len--;
	for (i = 0; i < len && c[i] != ';'; i++)
		;
	clen = i;
	data = (i < len) ? c + i + 1 : "";
	dlen = (i < len) ? len - i - 1 : 0;

	act = key_chr(c, clen, 'a', 't');
	id = key_val(c, clen, 'i', 0);
	m = key_val(c, clen, 'm', 0);
	q = key_val(c, clen, 'q', 0);

	switch (act) {
	case 'q':				/* support query */
		/* Answered only when the host takes kitty graphics: silence
		 * is how a program learns there are none (it pairs the query
		 * with DA1). A Silkie format, medium or gamut the attached
		 * terminals did not list is refused. */
		if (q < 1 && dv_kitty_host_gfx(w)) {
			long	f = key_val(c, clen, 'f', 32);
			int	G = (int)key_val(c, clen, 'G', 0);
			char	med = key_chr(c, clen, 't', 'd');

			if (!dv_silkie_gfx_ok(w->session, f, G, (int)med))
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
	default:				/* frames, animation, compose */
		return;
	}

	/* A transmission: a fresh one, or the next chunk of the one under
	 * way (continuation chunks carry only m= and q=). */
	if (w->kreassem == 0 || key_present(c, clen, 'f') ||
	    key_present(c, clen, 'i') || key_present(c, clen, 's') ||
	    key_present(c, clen, 't')) {
		long	f = key_val(c, clen, 'f', 32);
		int	G = (int)key_val(c, clen, 'G', 0);
		char	med = key_chr(c, clen, 't', 'd');

		if (!dv_silkie_gfx_ok(w->session, f, G, (int)med)) {
			if (q < 1)
				reply(w, id, 0, "EINVAL");
			return;
		}
		img = image_alloc(w, (unsigned)id);
		im = &w->kimg->at(img);
		im->gpu = (med == 'g');
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
		if (!read_medium(im, medium, data, dlen,
		    key_val(c, clen, 'S', 0), key_val(c, clen, 'O', 0))) {
			image_drop(w, img);
			w->kreassem = 0;
			if (q < 1)
				reply(w, id, 0, "EBADF:could not read medium");
			return;
		}
		m = 0;
	} else
		dv_buf_append(&im->data, data, dlen);
	if (m == 1)
		return;					/* more chunks follow */

	im->reassembling = 0;
	im->done = 1;
	w->kreassem = 0;
	image_pixels(im);
	/* Placements of an image sent again show the new pixels. */
	if (w->screen.kpl != nullptr)
		for (auto &[id, p] : *w->screen.kpl)
			if (p.img == img)
				p.gen = ++w->screen.kgen;
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
		for (auto &[id, im] : *w->kimg)
			dv_buf_free(&im.data);
	delete w->kimg;
	w->kimg = nullptr;
	if (w->screen.kpl != nullptr)
		w->screen.kpl->clear();
	w->kreassem = 0;
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

/* Which medium to hand the host, from GOBLIN_VIEW_KITTY_MEDIUM: shm (the
 * default), tempfile, file, or direct. Shared memory and a temp file are
 * removed by the host after it reads them; a plain file is not. */
static char
emit_medium_choice(void)
{
	const char	*e = getenv("GOBLIN_VIEW_KITTY_MEDIUM");

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
 * rectangle. The pixels travel by the medium GOBLIN_VIEW_KITTY_MEDIUM
 * selects (shared memory by default, so large images do not bloat the
 * escape stream), falling back to inline direct data. The placeholder
 * cells the compositor paints then select which part shows where.
 */
void
dv_kitty_transmit(struct dv_window *w, const struct dv_kplace *p,
    struct dv_buf *out)
{
	const struct dv_kimage	*im = &w->kimg->at(p->img);
	char			 hdr[200];
	int			 n, sent = 0;
	char			 medium = emit_medium_choice();

	if (!p->used || !im->done || im->data.len == 0)
		return;

	if (im->gpu) {
		n = snprintf(hdr, sizeof hdr, "\033_Ga=t,i=%u,%s%st=g,q=2;",
		    p->dv_id, im->fmt, im->fmt[0] ? "," : "");
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
				n = snprintf(hdr, sizeof hdr,
				    "\033_Ga=t,i=%u,%s%st=%c,q=2;", p->dv_id,
				    im->fmt, im->fmt[0] ? "," : "", used);
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
				    "\033_Ga=t,i=%u,%s%sq=2,m=%d;", p->dv_id,
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
}
void
dv_kitty_delete(unsigned dv_id, struct dv_buf *out)
{
	char	d[64];
	int	n = snprintf(d, sizeof d, "\033_Ga=d,d=I,i=%u,q=2\033\\", dv_id);

	if (n > 0)
		dv_buf_append(out, d, (size_t)n);
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
