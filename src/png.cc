/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * A small PNG decoder, so a program's kitty image (usually PNG) can be
 * turned back into pixels and re-emitted as sixel to a host that speaks
 * sixel but not kitty graphics. Original code from the public PNG
 * specification; zlib does the inflate. Non-interlaced images of colour
 * types 0/2/3/4/6 at bit depths 1/2/4/8/16 are handled -- what the tools
 * that draw into a terminal (gnuplot, matplotlib, ImageMagick, dvipng)
 * emit. Interlaced images are declined (the caller then shows nothing).
 */

#include "dv.hpp"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static unsigned
be32(const unsigned char *p)
{
	return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
	    ((unsigned)p[2] << 8) | p[3];
}

static int
paeth(int a, int b, int c)
{
	int	p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);

	if (pa <= pb && pa <= pc)
		return (a);
	return (pb <= pc ? b : c);
}

/* Inflate the concatenated IDAT stream into a buffer of the known
 * filtered size (rows each prefixed by a filter byte). */
static int
inflate_exact(const unsigned char *in, size_t inlen, unsigned char *out,
    size_t outlen)
{
	uLongf	dlen = (uLongf)outlen;

	return (uncompress(out, &dlen, in, (uLong)inlen) == Z_OK &&
	    dlen == outlen);
}

/* Sample channel c of pixel x on an already-unfiltered 8/16-bit row. */
static unsigned
sample(const unsigned char *row, unsigned x, unsigned chans, unsigned c,
    int bd)
{
	if (bd == 16) {
		const unsigned char	*p = row + ((size_t)x * chans + c) * 2;

		return (p[0]);			/* high byte is enough for RGBA */
	}
	if (bd == 8)
		return (row[(size_t)x * chans + c]);
	/* bd 1/2/4: c is always 0 here (gray or palette index). */
	{
		unsigned	bits = (unsigned)bd, per = 8 / bits;
		unsigned	byte = row[x / per];
		unsigned	shift = (per - 1 - (x % per)) * bits;
		unsigned	mask = (1u << bits) - 1;

		return ((byte >> shift) & mask);
	}
}

int
dv_png_decode(const unsigned char *d, size_t n, unsigned char **rgba,
    unsigned *wpx, unsigned *hpx)
{
	static const unsigned char sig[8] =
	    { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
	unsigned	 w = 0, h = 0, chans, bpp;
	size_t		 stride;
	int		 bd = 0, ct = -1, interlace = 0, ok = 0;
	size_t		 i;
	unsigned char	*idat = nullptr, *filt = nullptr, *out = nullptr;
	size_t		 idat_len = 0, idat_cap = 0, filt_len;
	unsigned char	 plte[256][3];
	unsigned char	 trns[256];
	int		 nplte = 0, ntrns = 0;
	unsigned	 gray_key = 0x10000;	/* >0xffff: no key */
	unsigned	 y;

	if (n < 8 || memcmp(d, sig, 8) != 0)
		return (0);
	memset(trns, 0xff, sizeof trns);
	i = 8;
	while (i + 8 <= n) {
		unsigned	len = be32(d + i);
		const char	*type = (const char *)d + i + 4;
		const unsigned char *cd = d + i + 8;

		if (i + 12 + (size_t)len > n && memcmp(type, "IDAT", 4) != 0)
			break;
		if (i + 12 + (size_t)len > n)
			len = (unsigned)(n - i - 12);
		if (memcmp(type, "IHDR", 4) == 0 && len >= 13) {
			w = be32(cd);
			h = be32(cd + 4);
			bd = cd[8];
			ct = cd[9];
			interlace = cd[12];
		} else if (memcmp(type, "PLTE", 4) == 0) {
			nplte = (int)(len / 3);
			if (nplte > 256)
				nplte = 256;
			memcpy(plte, cd, (size_t)nplte * 3);
		} else if (memcmp(type, "tRNS", 4) == 0) {
			if (ct == 3) {
				ntrns = (int)(len > 256 ? 256 : len);
				memcpy(trns, cd, (size_t)ntrns);
			} else if (ct == 0 && len >= 2) {
				gray_key = ((unsigned)cd[0] << 8) | cd[1];
			}
		} else if (memcmp(type, "IDAT", 4) == 0) {
			if (idat_len + len > idat_cap) {
				unsigned char	*ni;

				idat_cap = (idat_len + len) * 2 + 1;
				ni = (unsigned char *)realloc(idat, idat_cap);
				if (ni == nullptr) {
					free(idat);
					return (0);
				}
				idat = ni;
			}
			memcpy(idat + idat_len, cd, len);
			idat_len += len;
		} else if (memcmp(type, "IEND", 4) == 0)
			break;
		i += 12 + (size_t)len;		/* len + type + data + CRC */
	}
	/* Any size the format allows (2^31 - 1 a side) that memory holds. */
	if (w == 0 || h == 0 || w > 0x7fffffffu || h > 0x7fffffffu ||
	    interlace != 0 || idat == nullptr ||
	    (uint64_t)w * h > (uint64_t)SIZE_MAX / 4 / 2) {
		free(idat);
		return (0);
	}
	switch (ct) {
	case 0: chans = 1; break;		/* gray */
	case 2: chans = 3; break;		/* RGB */
	case 3: chans = 1; break;		/* palette index */
	case 4: chans = 2; break;		/* gray + alpha */
	case 6: chans = 4; break;		/* RGBA */
	default: free(idat); return (0);
	}
	if (bd != 1 && bd != 2 && bd != 4 && bd != 8 && bd != 16) {
		free(idat);
		return (0);
	}
	if ((ct == 2 || ct == 4 || ct == 6) && bd < 8) {
		free(idat);		/* truecolour needs 8 or 16 bits */
		return (0);
	}
	stride = ((size_t)w * chans * (size_t)bd + 7) / 8;
	bpp = (unsigned)((chans * bd + 7) / 8);
	if (bpp < 1)
		bpp = 1;
	if (stride + 1 > SIZE_MAX / h) {
		free(idat);
		return (0);
	}
	filt_len = (stride + 1) * h;
	filt = (unsigned char *)malloc(filt_len);
	out = (unsigned char *)malloc((size_t)w * h * 4);
	if (filt == nullptr || out == nullptr ||
	    !inflate_exact(idat, idat_len, filt, filt_len))
		goto done;

	for (y = 0; y < h; y++) {		/* unfilter in place */
		unsigned char	*row = filt + (size_t)y * (stride + 1);
		unsigned char	 ft = row[0];
		unsigned char	*cur = row + 1;
		unsigned char	*prev = y ? filt + (size_t)(y - 1) *
				    (stride + 1) + 1 : nullptr;
		size_t		 k;

		for (k = 0; k < stride; k++) {
			int	a = k >= bpp ? cur[k - bpp] : 0;
			int	b = prev ? prev[k] : 0;
			int	c = (prev && k >= bpp) ? prev[k - bpp] : 0;
			int	v = cur[k];

			switch (ft) {
			case 1: v += a; break;
			case 2: v += b; break;
			case 3: v += (a + b) / 2; break;
			case 4: v += paeth(a, b, c); break;
			default: break;
			}
			cur[k] = (unsigned char)v;
		}
	}

	for (y = 0; y < h; y++) {		/* to RGBA */
		unsigned char	*row = filt + (size_t)y * (stride + 1) + 1;
		unsigned char	*o = out + (size_t)y * w * 4;
		unsigned	 x;

		for (x = 0; x < w; x++, o += 4) {
			unsigned	r, g, bl, al = 255;

			switch (ct) {
			case 0: {
				unsigned	raw = sample(row, x, 1, 0, bd);
				unsigned	gv = bd >= 8 ? raw :
				    raw * 255 / ((1u << bd) - 1);

				r = g = bl = gv;
				if (bd == 16 && gray_key <= 0xffff) {
					/* key compares full 16-bit; we kept hi */
				}
				if (gray_key <= 0xffff && raw == (bd >= 8 ?
				    (gray_key & 0xff) : gray_key))
					al = 0;
				break;
			}
			case 2:
				r = sample(row, x, 3, 0, bd);
				g = sample(row, x, 3, 1, bd);
				bl = sample(row, x, 3, 2, bd);
				break;
			case 3: {
				unsigned	idx = sample(row, x, 1, 0, bd);

				if ((int)idx >= nplte)
					idx = 0;
				r = plte[idx][0];
				g = plte[idx][1];
				bl = plte[idx][2];
				al = ((int)idx < ntrns) ? trns[idx] : 255;
				break;
			}
			case 4:
				r = g = bl = sample(row, x, 2, 0, bd);
				al = sample(row, x, 2, 1, bd);
				break;
			default:	/* 6 */
				r = sample(row, x, 4, 0, bd);
				g = sample(row, x, 4, 1, bd);
				bl = sample(row, x, 4, 2, bd);
				al = sample(row, x, 4, 3, bd);
				break;
			}
			o[0] = (unsigned char)r;
			o[1] = (unsigned char)g;
			o[2] = (unsigned char)bl;
			o[3] = (unsigned char)al;
		}
	}
	*rgba = out;
	*wpx = w;
	*hpx = h;
	out = nullptr;
	ok = 1;
done:
	free(idat);
	free(filt);
	free(out);
	return (ok);
}
