/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * The image codecs: PNG decode (a known 2x2 RGBA and a 2x1 grayscale
 * PNG), the RGBA-from-stored-image helper, and the sixel encoder.
 */

#include "dv.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int	failed;

static void
fail(const char *n)
{
	fprintf(stderr, "FAIL %s\n", n);
	failed = 1;
}

/* A 2x2 RGBA PNG: red, green / blue, white. */
static const unsigned char png_rgba[] = {
	137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,6,0,0,
	0,114,182,13,36,0,0,0,18,73,68,65,84,120,156,99,248,207,192,240,31,12,
	129,52,24,0,0,73,200,9,247,249,171,182,13,0,0,0,0,73,69,78,68,174,66,
	96,130
};

/* A 2x1 grayscale PNG: black, white. */
static const unsigned char png_gray[] = {
	137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,1,8,0,0,0,
	0,209,73,32,86,0,0,0,11,73,68,65,84,120,156,99,96,248,15,0,1,2,1,0,66,
	190,188,104,0,0,0,0,73,69,78,68,174,66,96,130
};

static void
test_png(void)
{
	unsigned char	*rgba = nullptr;
	unsigned	 w = 0, h = 0;

	if (!dv_png_decode(png_rgba, sizeof png_rgba, &rgba, &w, &h))
		fail("decode the 2x2 RGBA PNG");
	else {
		if (w != 2 || h != 2)
			fail("RGBA PNG dimensions");
		/* red, green / blue, white */
		if (rgba[0] != 255 || rgba[1] != 0 || rgba[2] != 0)
			fail("pixel(0,0) is red");
		if (rgba[4] != 0 || rgba[5] != 255 || rgba[6] != 0)
			fail("pixel(1,0) is green");
		if (rgba[8] != 0 || rgba[9] != 0 || rgba[10] != 255)
			fail("pixel(0,1) is blue");
		if (rgba[12] != 255 || rgba[13] != 255 || rgba[14] != 255)
			fail("pixel(1,1) is white");
		if (rgba[3] != 255)
			fail("RGBA alpha is opaque");
		free(rgba);
	}

	rgba = nullptr;
	if (!dv_png_decode(png_gray, sizeof png_gray, &rgba, &w, &h))
		fail("decode the 2x1 grayscale PNG");
	else {
		if (w != 2 || h != 1)
			fail("grayscale PNG dimensions");
		if (rgba[0] != 0 || rgba[1] != 0 || rgba[2] != 0)
			fail("gray pixel 0 is black");
		if (rgba[4] != 255 || rgba[5] != 255 || rgba[6] != 255)
			fail("gray pixel 1 is white");
		free(rgba);
	}

	/* Not a PNG: refused, not crashed. */
	if (dv_png_decode((const unsigned char *)"not a png xxxxxxxx", 18,
	    &rgba, &w, &h))
		fail("garbage is rejected");
}

/* base64 without pulling in kitty.cc internals. */
static void
b64(const unsigned char *in, size_t n, char *out)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	    "abcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t	i, o = 0;

	for (i = 0; i < n; i += 3) {
		unsigned	v = (unsigned)in[i] << 16;

		if (i + 1 < n)
			v |= (unsigned)in[i + 1] << 8;
		if (i + 2 < n)
			v |= in[i + 2];
		out[o++] = t[(v >> 18) & 63];
		out[o++] = t[(v >> 12) & 63];
		out[o++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < n ? t[v & 63] : '=';
	}
	out[o] = '\0';
}

static void
test_kimage_rgba(void)
{
	struct dv_kimage	im;
	unsigned char		*rgba = nullptr;
	unsigned		 w = 0, h = 0;
	const unsigned char	 rgb[3] = { 10, 20, 30 };	/* one f=24 pixel */

	/* f=24 raw: a 1x1 RGB pixel becomes opaque RGBA. */
	memset(&im, 0, sizeof im);
	im.used = im.done = 1;
	im.wpx = im.hpx = 1;
	strcpy(im.fmt, "f=24,s=1,v=1");
	{
		char	enc[16];

		b64(rgb, 3, enc);
		dv_buf_append(&im.data, enc, strlen(enc));
	}
	if (!dv_kimage_rgba(&im, &rgba, &w, &h))
		fail("f=24 image decodes to RGBA");
	else {
		if (w != 1 || h != 1 || rgba[0] != 10 || rgba[1] != 20 ||
		    rgba[2] != 30 || rgba[3] != 255)
			fail("f=24 pixel expands with opaque alpha");
		free(rgba);
	}
	dv_buf_free(&im.data);

	/* f=100 PNG: the helper runs it through the PNG decoder. */
	memset(&im, 0, sizeof im);
	im.used = im.done = 1;
	strcpy(im.fmt, "f=100");
	{
		char	*enc = (char *)malloc(sizeof png_rgba * 2 + 4);

		b64(png_rgba, sizeof png_rgba, enc);
		dv_buf_append(&im.data, enc, strlen(enc));
		free(enc);
	}
	if (!dv_kimage_rgba(&im, &rgba, &w, &h))
		fail("f=100 image decodes via PNG");
	else {
		if (w != 2 || h != 2 || rgba[0] != 255 || rgba[1] != 0)
			fail("f=100 pixels match the PNG");
		free(rgba);
	}
	dv_buf_free(&im.data);
}

static void
test_sixel_encode(void)
{
	unsigned char	rgba[2 * 2 * 4];
	struct dv_buf	b;
	unsigned	i;

	for (i = 0; i < 4; i++) {	/* four opaque colours */
		rgba[i * 4 + 0] = (i == 0) ? 255 : 0;
		rgba[i * 4 + 1] = (i == 1) ? 255 : 0;
		rgba[i * 4 + 2] = (i == 2) ? 255 : 0;
		rgba[i * 4 + 3] = 255;
	}
	memset(&b, 0, sizeof b);
	dv_sixel_encode(rgba, 2, 2, 0, 0, 2, 2, &b);
	if (b.len < 8)
		fail("sixel output is produced");
	if (memcmp(b.data, "\033Pq", 3) != 0)
		fail("sixel starts with the DCS q introducer");
	if (b.len < 2 || memcmp(b.data + b.len - 2, "\033\\", 2) != 0)
		fail("sixel ends with ST");
	if (memmem(b.data, b.len, "#0;2;", 5) == NULL)
		fail("sixel defines colour registers as RGB percentages");
	dv_buf_free(&b);

	/* A fully transparent image emits a valid, empty-ish sixel, no crash. */
	memset(rgba, 0, sizeof rgba);
	memset(&b, 0, sizeof b);
	dv_sixel_encode(rgba, 2, 2, 0, 0, 2, 2, &b);
	if (b.len < 5 || memcmp(b.data, "\033Pq", 3) != 0)
		fail("transparent sixel is still well formed");
	dv_buf_free(&b);
}

int
main(void)
{
	test_png();
	test_kimage_rgba();
	test_sixel_encode();
	if (failed) {
		fprintf(stderr, "test-codec: FAILED\n");
		return (1);
	}
	printf("test-codec: ok\n");
	return (0);
}
