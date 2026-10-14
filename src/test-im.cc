/* Input methods: romaji->kana, US-International dead keys, pinyin. */
#include "dv.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failed;

static void
fail(const char *n)
{
	fprintf(stderr, "FAIL %s\n", n);
	failed = 1;
}

/* Feed a string of keys; return the committed UTF-8 in a static buffer. */
static const char *
type(struct dv_im *im, const char *keys)
{
	static char	out[256];
	struct dv_buf	b;

	memset(&b, 0, sizeof b);
	dv_im_reset(im);
	for (; *keys != '\0'; keys++)
		dv_im_key(im, (unsigned char)*keys, &b);
	size_t	n = b.len < sizeof out - 1 ? b.len : sizeof out - 1;
	if (b.data != nullptr)
		memcpy(out, b.data, n);
	out[n] = '\0';
	dv_buf_free(&b);
	return (out);
}


/* The dictionaries are found by the override, else beside the executable
 * or at the compiled-in place: in the build tree, the latter. */
static void
test_datadir(void)
{
	char	probe[1200];

	setenv("GOBLIN_VIEW_IMDATA", "/nonexistent/override", 1);
	if (strcmp(dv_im_datadir(), "/nonexistent/override") != 0)
		fail("GOBLIN_VIEW_IMDATA overrides the data directory");
	unsetenv("GOBLIN_VIEW_IMDATA");
	snprintf(probe, sizeof probe, "%s/pinyin.dict", dv_im_datadir());
	if (access(probe, R_OK) != 0)
		fail("the resolved data directory holds pinyin.dict");
	{
		char	exe[1024];

		if (!dv_exe_path(exe, sizeof exe) || strstr(exe, "test-im") == NULL)
			fail("dv_exe_path names this executable");
	}
}

int
main(void)
{
	test_datadir();
	struct dv_im	im;

	setenv("GOBLIN_VIEW_IMDATA", "share", 0);

	memset(&im, 0, sizeof im);

	im.method = DV_IM_ROMAJI;
	if (strcmp(type(&im, "ka"), "\xe3\x81\x8b") != 0)	/* か */
		fail("romaji ka");
	if (strcmp(type(&im, "shi"), "\xe3\x81\x97") != 0)	/* し */
		fail("romaji shi");
	if (strcmp(type(&im, "kya"), "\xe3\x81\x8d\xe3\x82\x83") != 0) /* きゃ */
		fail("romaji kya");
	if (strcmp(type(&im, "kko"), "\xe3\x81\xa3\xe3\x81\x93") != 0) /* っこ */
		fail("romaji sokuon kko");
	/* nihongo -> にほんご */
	if (strcmp(type(&im, "nihongo"),
	    "\xe3\x81\xab\xe3\x81\xbb\xe3\x82\x93\xe3\x81\x94") != 0)
		fail("romaji nihongo");

	im.method = DV_IM_USINTL;
	if (strcmp(type(&im, "'e"), "\xc3\xa9") != 0)		/* é */
		fail("us-intl acute e");
	if (strcmp(type(&im, "~n"), "\xc3\xb1") != 0)		/* ñ */
		fail("us-intl tilde n");
	if (strcmp(type(&im, "\"u"), "\xc3\xbc") != 0)		/* ü */
		fail("us-intl diaeresis u");

	im.method = DV_IM_PINYIN;
	/* Space commits the top candidate; the table orders 你 first for "ni". */
	if (strcmp(type(&im, "ni "), "\xe4\xbd\xa0") != 0)	/* 你 */
		fail("pinyin ni -> 你 (needs share/pinyin.dict)");
	if (strcmp(type(&im, "hao "), "\xe5\xa5\xbd") != 0)	/* 好 */
		fail("pinyin hao -> 好");
	if (strcmp(type(&im, "wo "), "\xe6\x88\x91") != 0)	/* 我 */
		fail("pinyin wo -> 我");
	/* Enter confirms the top candidate too (not the raw English). */
	if (strcmp(type(&im, "ni\r"), "\xe4\xbd\xa0") != 0)	/* 你 */
		fail("pinyin ni + Enter -> 你 (not \"ni\")");

	im.method = DV_IM_WUBI;
	/* Wubi code "a" -> 工 (top by weight in the Apache table). */
	if (strcmp(type(&im, "a "), "\xe5\xb7\xa5") != 0)	/* 工 */
		fail("wubi a -> 工 (needs share/wubi.dict)");

	/* Method off passes everything through untouched. */
	im.method = DV_IM_OFF;
	{
		struct dv_buf	b;

		memset(&b, 0, sizeof b);
		if (dv_im_key(&im, 'a', &b) != 0 || b.len != 0)
			fail("off is transparent");
		dv_buf_free(&b);
	}

	if (failed) {
		fprintf(stderr, "test-im: FAILED\n");
		return (1);
	}
	printf("test-im: ok\n");
	return (0);
}
