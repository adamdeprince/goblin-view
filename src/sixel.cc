/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * A sixel decoder for the window's terminal. A program that draws with
 * sixel (DCS q ... ST) has its picture decoded here into RGBA, which
 * kitty.cc then stores and places like any kitty image, so the host --
 * which must speak kitty graphics -- sees one protocol. Written from the
 * VT330/VT340 description of the format: raster attributes ("), colour
 * definitions and selection (#), repeats (!), graphics carriage return
 * ($), next band (-), and the six-pixel data characters ? to ~.
 */

#include "dv.hpp"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SIXEL_MAX	4096u		/* pixels per side */
#define SIXEL_COLORS	256

struct sx {
	const unsigned char	*d;
	size_t			 n;
	unsigned		 w, h;		/* canvas, on the drawing pass */
	unsigned char		*px;		/* RGBA, or null on the sizing pass */
	uint32_t		 pal[SIXEL_COLORS];	/* 0xRRGGBB */
	unsigned		 color;
	unsigned		 x, y;		/* pen: column, band's top row */
	unsigned		 maxx, maxy;	/* extent touched */
	unsigned		 rw, rh;	/* raster attributes, if given */
};

/* The VT340's default colour map, in 0-100 percent as it documents it. */
static const unsigned char sixel_default[16][3] = {
	{ 0, 0, 0 }, { 20, 20, 80 }, { 80, 13, 13 }, { 20, 80, 20 },
	{ 80, 20, 80 }, { 20, 80, 80 }, { 80, 80, 20 }, { 53, 53, 53 },
	{ 26, 26, 26 }, { 33, 33, 60 }, { 60, 26, 26 }, { 33, 60, 33 },
	{ 60, 33, 60 }, { 33, 60, 60 }, { 60, 60, 33 }, { 80, 80, 80 },
};

static uint32_t
pct_rgb(unsigned r, unsigned g, unsigned b)
{
	if (r > 100)
		r = 100;
	if (g > 100)
		g = 100;
	if (b > 100)
		b = 100;
	return ((r * 255 / 100) << 16) | ((g * 255 / 100) << 8) | (b * 255 / 100);
}

/* Sixel HLS has hue 0 at blue, 120 at red, 240 at green. */
static uint32_t
hls_rgb(unsigned hue, unsigned light, unsigned sat)
{
	double	H = (double)((hue + 240) % 360), L, S, c, x, m, r, g, b;

	L = (light > 100 ? 100 : light) / 100.0;
	S = (sat > 100 ? 100 : sat) / 100.0;
	c = (1.0 - fabs(2.0 * L - 1.0)) * S;
	x = c * (1.0 - fabs(fmod(H / 60.0, 2.0) - 1.0));
	m = L - c / 2.0;
	switch ((int)(H / 60.0)) {
	case 0: r = c; g = x; b = 0; break;
	case 1: r = x; g = c; b = 0; break;
	case 2: r = 0; g = c; b = x; break;
	case 3: r = 0; g = x; b = c; break;
	case 4: r = x; g = 0; b = c; break;
	default: r = c; g = 0; b = x; break;
	}
	return ((uint32_t)((r + m) * 255.0 + 0.5) << 16) |
	    ((uint32_t)((g + m) * 255.0 + 0.5) << 8) |
	    (uint32_t)((b + m) * 255.0 + 0.5);
}

static unsigned
num(struct sx *s, size_t *i)
{
	unsigned	v = 0;

	while (*i < s->n && s->d[*i] >= '0' && s->d[*i] <= '9') {
		if (v < 100000)
			v = v * 10 + (s->d[*i] - '0');
		(*i)++;
	}
	return (v);
}

static void
params(struct sx *s, size_t *i, unsigned *p, int *np, int max)
{
	*np = 0;
	for (;;) {
		unsigned	v = num(s, i);

		if (*np < max)
			p[(*np)++] = v;
		if (*i < s->n && s->d[*i] == ';') {
			(*i)++;
			continue;
		}
		break;
	}
}

/* One data character: six pixels down from the band's top row. */
static void
plot(struct sx *s, unsigned bits)
{
	unsigned	k;

	if (s->x + 1 > s->maxx)
		s->maxx = s->x + 1;
	if (s->y + 6 > s->maxy)
		s->maxy = s->y + 6;
	if (s->px != nullptr && bits != 0 && s->x < s->w) {
		uint32_t	c = s->pal[s->color];

		for (k = 0; k < 6; k++) {
			unsigned	yy = s->y + k;

			if (((bits >> k) & 1) && yy < s->h) {
				unsigned char	*p = s->px +
				    ((size_t)yy * s->w + s->x) * 4;

				p[0] = (unsigned char)(c >> 16);
				p[1] = (unsigned char)(c >> 8);
				p[2] = (unsigned char)c;
				p[3] = 255;
			}
		}
	}
	s->x++;
}

/* Walk the data once: sizing when px is null, drawing otherwise. */
static void
run(struct sx *s)
{
	size_t		i = 0;
	unsigned	p[8];
	int		np, k;

	s->x = s->y = 0;
	s->color = 0;
	s->maxx = s->maxy = 0;
	for (k = 0; k < SIXEL_COLORS; k++)
		s->pal[k] = k < 16 ? pct_rgb(sixel_default[k][0],
		    sixel_default[k][1], sixel_default[k][2]) : 0;
	while (i < s->n) {
		unsigned char	c = s->d[i++];

		if (c >= '?' && c <= '~') {
			plot(s, c - '?');
			continue;
		}
		switch (c) {
		case '"':			/* raster attributes */
			params(s, &i, p, &np, 4);
			if (np >= 4) {
				s->rw = p[2] > SIXEL_MAX ? SIXEL_MAX : p[2];
				s->rh = p[3] > SIXEL_MAX ? SIXEL_MAX : p[3];
			}
			break;
		case '#':			/* define, or select, a colour */
			params(s, &i, p, &np, 5);
			if (np >= 1) {
				unsigned	n = p[0] % SIXEL_COLORS;

				if (np >= 5 && p[1] == 2)
					s->pal[n] = pct_rgb(p[2], p[3], p[4]);
				else if (np >= 5 && p[1] == 1)
					s->pal[n] = hls_rgb(p[2], p[3], p[4]);
				s->color = n;
			}
			break;
		case '!': {			/* repeat */
			unsigned	cnt = num(s, &i);

			if (i < s->n) {
				unsigned char	ch = s->d[i++];

				if (ch >= '?' && ch <= '~') {
					if (cnt == 0)
						cnt = 1;
					if (cnt > SIXEL_MAX)
						cnt = SIXEL_MAX;
					while (cnt-- > 0)
						plot(s, ch - '?');
				}
			}
			break;
		}
		case '$':			/* graphics carriage return */
			s->x = 0;
			break;
		case '-':			/* next band */
			s->x = 0;
			s->y += 6;
			if (s->y > SIXEL_MAX)
				return;
			break;
		default:			/* whitespace, newlines */
			break;
		}
	}
}

/*
 * Decode sixel data (what follows the q) into a malloc'd RGBA bitmap;
 * undrawn pixels are transparent, as a terminal shows its background
 * through them. The size is the raster attributes or the extent drawn,
 * whichever is larger. Returns 0 for nothing to show.
 */
int
dv_sixel_decode(const unsigned char *d, size_t n, unsigned char **rgba,
    unsigned *wpx, unsigned *hpx)
{
	struct sx	s;
	unsigned	w, h;

	memset(&s, 0, sizeof s);
	s.d = d;
	s.n = n;
	run(&s);				/* sizing pass */
	w = s.maxx > s.rw ? s.maxx : s.rw;
	h = s.maxy > s.rh ? s.maxy : s.rh;
	if (w > SIXEL_MAX)
		w = SIXEL_MAX;
	if (h > SIXEL_MAX)
		h = SIXEL_MAX;
	if (w == 0 || h == 0)
		return (0);
	s.px = (unsigned char *)calloc((size_t)w * h, 4);
	if (s.px == nullptr)
		return (0);
	s.w = w;
	s.h = h;
	run(&s);				/* drawing pass */
	*rgba = s.px;
	*wpx = w;
	*hpx = h;
	return (1);
}

/* ---- sixel encoding: RGBA (a sub-rectangle) -> a sixel DCS ---- */

static unsigned
to_pct(unsigned v)
{
	return ((v * 100 + 127) / 255);
}

/*
 * Encode the (x,y,w,h) sub-rectangle of an iw x ih RGBA image as a sixel
 * DCS string appended to `out`. Pixels with alpha < 128 are left unset,
 * so the terminal background shows through (transparent PNGs stay clean).
 * Colours are the image's own when there are <= 256 of them, else mapped
 * to a fixed 6x6x6 cube plus grays -- exact for plots and text, adequate
 * for photos.
 */
void
dv_sixel_encode(const unsigned char *rgba, unsigned iw, unsigned ih,
    unsigned x, unsigned y, unsigned w, unsigned h, struct dv_buf *out)
{
	unsigned	 npix, i, band, col, ci, ncol;
	int		*idx;			/* palette index per pixel, -1 transparent */
	unsigned	 pal[256];		/* 0xRRGGBB */
	int		 hash[1024];
	int		 cube = 0;
	char		 hdr[64];
	int		 nlen;

	if (w == 0 || h == 0 || x >= iw || y >= ih)
		return;
	if (x + w > iw)
		w = iw - x;
	if (y + h > ih)
		h = ih - y;
	npix = w * h;
	idx = (int *)malloc((size_t)npix * sizeof *idx);
	if (idx == nullptr)
		return;
	for (i = 0; i < 1024; i++)
		hash[i] = -1;
	ncol = 0;

	/* First pass: gather an exact palette, or fall back to the cube. */
	for (i = 0; i < npix && !cube; i++) {
		const unsigned char	*p = rgba +
		    ((size_t)(y + i / w) * iw + (x + i % w)) * 4;
		unsigned		 rgb;
		unsigned		 hp;

		if (p[3] < 128) {
			idx[i] = -1;
			continue;
		}
		rgb = ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
		hp = ((rgb * 2654435761u) >> 12) & 1023;
		while (hash[hp] >= 0 && pal[hash[hp]] != rgb)
			hp = (hp + 1) & 1023;
		if (hash[hp] < 0) {
			if (ncol >= 256) {
				cube = 1;
				break;
			}
			hash[hp] = (int)ncol;
			pal[ncol++] = rgb;
		}
		idx[i] = hash[hp];
	}
	if (cube) {			/* too many colours: a fixed palette */
		ncol = 0;
		for (unsigned r = 0; r < 6; r++)
			for (unsigned g = 0; g < 6; g++)
				for (unsigned b = 0; b < 6; b++)
					pal[ncol++] = (r * 51 << 16) |
					    (g * 51 << 8) | (b * 51);
		for (unsigned s = 0; s < 40; s++)		/* grays */
			pal[ncol++] = (s * 255 / 39) * 0x010101u;
		for (i = 0; i < npix; i++) {
			const unsigned char	*p = rgba +
			    ((size_t)(y + i / w) * iw + (x + i % w)) * 4;

			if (p[3] < 128) {
				idx[i] = -1;
				continue;
			}
			idx[i] = (int)(((p[0] + 25) / 51) * 36 +
			    ((p[1] + 25) / 51) * 6 + (p[2] + 25) / 51);
		}
	}

	dv_buf_append(out, "\033Pq", 3);
	nlen = snprintf(hdr, sizeof hdr, "\"1;1;%u;%u", w, h);
	if (nlen > 0)
		dv_buf_append(out, hdr, (size_t)nlen);
	for (ci = 0; ci < ncol; ci++) {
		nlen = snprintf(hdr, sizeof hdr, "#%u;2;%u;%u;%u", ci,
		    to_pct((pal[ci] >> 16) & 0xff), to_pct((pal[ci] >> 8) & 0xff),
		    to_pct(pal[ci] & 0xff));
		if (nlen > 0)
			dv_buf_append(out, hdr, (size_t)nlen);
	}

	/* Six pixel rows per band; one pass per colour used in the band. */
	for (band = 0; band * 6 < h; band++) {
		int	first = 1;

		for (ci = 0; ci < ncol; ci++) {
			int		used = 0, run = -1;
			unsigned	runlen = 0;

			for (col = 0; col < w; col++) {
				unsigned	bits = 0, k;

				for (k = 0; k < 6; k++) {
					unsigned	yy = band * 6 + k;

					if (yy < h &&
					    idx[yy * w + col] == (int)ci)
						bits |= 1u << k;
				}
				if (!used && bits == 0)
					continue;	/* skip leading empties */
				if (!used) {
					if (!first)
						dv_buf_append(out, "$", 1);
					first = 0;
					nlen = snprintf(hdr, sizeof hdr, "#%u",
					    ci);
					if (nlen > 0)
						dv_buf_append(out, hdr,
						    (size_t)nlen);
					used = 1;
				}
				if ((int)bits == run)
					runlen++;
				else {
					if (run >= 0) {
						char	c = (char)(0x3f + run);

						if (runlen > 3) {
							nlen = snprintf(hdr,
							    sizeof hdr, "!%u",
							    runlen);
							if (nlen > 0)
								dv_buf_append(out,
								    hdr, (size_t)nlen);
							dv_buf_append(out, &c, 1);
						} else
							while (runlen-- > 0)
								dv_buf_append(out,
								    &c, 1);
					}
					run = (int)bits;
					runlen = 1;
				}
			}
			if (used && run >= 0) {		/* flush the last run */
				char	c = (char)(0x3f + run);

				if (runlen > 3) {
					nlen = snprintf(hdr, sizeof hdr, "!%u",
					    runlen);
					if (nlen > 0)
						dv_buf_append(out, hdr,
						    (size_t)nlen);
					dv_buf_append(out, &c, 1);
				} else
					while (runlen-- > 0)
						dv_buf_append(out, &c, 1);
			}
			run = -1;
			runlen = 0;
		}
		dv_buf_append(out, "-", 1);	/* next band */
	}
	dv_buf_append(out, "\033\\", 2);
	free(idx);
}
