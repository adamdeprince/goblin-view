/*
 * Geometry tests. Sizes include 132x43, 200x60, 40x12, 80x24 so 80x25
 * cannot ossify as an assumption.
 */

#include "dv.hpp"

#include <stdio.h>
#include <string.h>

static int failed;

static void
fail(const char *n)
{
	fprintf(stderr, "FAIL %s\n", n);
	failed = 1;
}

static void
expect_geom(const char *name, struct dv_geom g, int x, int y,
    unsigned cols, unsigned rows)
{
	if (g.x == x && g.y == y && g.cols == cols && g.rows == rows)
		return;
	fprintf(stderr, "FAIL %s: got %d,%d %ux%u want %d,%d %ux%u\n",
	    name, g.x, g.y, g.cols, g.rows, x, y, cols, rows);
	failed = 1;
}

static void
expect_fits(const char *name, unsigned W, unsigned H, int framed,
    struct dv_geom g)
{
	unsigned	fw = dv_frame_cols(&g, framed);
	unsigned	fh = dv_frame_rows(&g, framed);

	if (g.x < 0 || g.y < 0 ||
	    (unsigned)g.x + fw > W || (unsigned)g.y + fh > H ||
	    g.cols < 1 || g.rows < 1) {
		fprintf(stderr,
		    "FAIL %s: %d,%d inner %ux%u frame %ux%u not in %ux%u\n",
		    name, g.x, g.y, g.cols, g.rows, fw, fh, W, H);
		failed = 1;
	}
}

static void
test_snaps(unsigned W, unsigned H)
{
	int		snap, framed;
	struct dv_geom	g;
	char		name[64];

	for (framed = 0; framed <= 1; framed++) {
		if (framed && (W < 4 || H < 3))
			continue;
		for (snap = 1; snap <= 9; snap++) {
			dv_snap_geom(W, H, snap, framed, &g);
			snprintf(name, sizeof name, "snap %ux%u #%d framed=%d",
			    W, H, snap, framed);
			expect_fits(name, W, H, framed, g);
		}
	}
}

static void
test_80x24_unframed(void)
{
	struct dv_geom	g;

	dv_snap_geom(80, 24, 1, 0, &g);
	expect_geom("80x24 snap1", g, 0, 0, 80, 12);
	dv_snap_geom(80, 24, 2, 0, &g);
	expect_geom("80x24 snap2", g, 0, 12, 80, 12);
	dv_snap_geom(80, 24, 4, 0, &g);
	expect_geom("80x24 snap4", g, 0, 0, 40, 24);
	dv_snap_geom(80, 24, 5, 0, &g);
	expect_geom("80x24 snap5", g, 40, 0, 40, 24);
	dv_snap_geom(80, 24, 6, 0, &g);
	expect_geom("80x24 snap6", g, 0, 0, 40, 12);
	dv_snap_geom(80, 24, 9, 0, &g);
	expect_geom("80x24 snap9", g, 40, 12, 40, 12);
}

static void
test_132x43(void)
{
	struct dv_geom	g;

	dv_snap_geom(132, 43, 1, 0, &g);
	expect_geom("132x43 snap1", g, 0, 0, 132, 21);
	dv_snap_geom(132, 43, 2, 0, &g);
	expect_geom("132x43 snap2", g, 0, 21, 132, 22);
	dv_snap_geom(132, 43, 4, 0, &g);
	expect_geom("132x43 snap4", g, 0, 0, 66, 43);
	dv_snap_geom(132, 43, 5, 0, &g);
	expect_geom("132x43 snap5", g, 66, 0, 66, 43);
}

static void
test_200x60(void)
{
	struct dv_geom	g;

	dv_snap_geom(200, 60, 3, 0, &g);
	/* 2/3 = 133 x 40, centred at (33, 10) */
	expect_geom("200x60 snap3", g, 33, 10, 133, 40);
}

static void
test_40x12(void)
{
	struct dv_geom	g;

	dv_snap_geom(40, 12, 1, 1, &g);
	expect_fits("40x12 snap1 framed", 40, 12, 1, g);
	dv_snap_geom(40, 12, 3, 1, &g);
	expect_fits("40x12 snap3 framed", 40, 12, 1, g);
}

static void
test_clamp_after_shrink(void)
{
	struct dv_geom	g;

	g.x = 100;
	g.y = 50;
	g.cols = 80;
	g.rows = 24;
	dv_clamp_geom(40, 12, 0, &g);
	expect_fits("clamp 80x24 onto 40x12", 40, 12, 0, g);
	if (g.x != 0 || g.y != 0) {
		fprintf(stderr, "FAIL clamp origin: %d,%d\n", g.x, g.y);
		failed = 1;
	}
	if (g.cols != 40 || g.rows != 12) {
		fprintf(stderr, "FAIL clamp size: %ux%u\n", g.cols, g.rows);
		failed = 1;
	}

	g.x = 10;
	g.y = 5;
	g.cols = 20;
	g.rows = 8;
	dv_clamp_geom(200, 60, 1, &g);
	expect_geom("clamp no-op on large desktop", g, 10, 5, 20, 8);
}

static void
test_zoom(void)
{
	struct dv_geom	g;

	g.x = 5;
	g.y = 5;
	g.cols = 10;
	g.rows = 5;
	dv_zoom_geom(132, 43, &g);
	expect_geom("zoom 132x43", g, 0, 0, 132, 43);
	dv_zoom_geom(40, 12, &g);
	expect_geom("zoom 40x12", g, 0, 0, 40, 12);
}

static void
test_unzoom_after_host_resize(void)
{
	struct dv_geom	saved, g;

	saved.x = 60;
	saved.y = 20;
	saved.cols = 40;
	saved.rows = 12;
	g = saved;
	dv_clamp_geom(40, 12, 1, &g);
	expect_fits("unzoom onto smaller host", 40, 12, 1, g);
}

int
main(void)
{
	unsigned	sizes[][2] = {
		{80, 24}, {80, 25}, {132, 43}, {200, 60}, {40, 12},
		{10, 5}, {1, 1}, {100, 1}, {3, 3}
	};
	unsigned	i;

	for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
		test_snaps(sizes[i][0], sizes[i][1]);
	test_80x24_unframed();
	test_132x43();
	test_200x60();
	test_40x12();
	test_clamp_after_shrink();
	test_zoom();
	test_unzoom_after_host_resize();

	{
		struct dv_geom	g = { 10, 5, 20, 8 };

		dv_geom_nudge(&g, 3, 2, 80, 24, 1);
		expect_geom("nudge", g, 13, 7, 20, 8);
		dv_geom_nudge(&g, 100, 100, 80, 24, 1);
		expect_fits("nudge clamp", 80, 24, 1, g);
		g = { 0, 0, 10, 5 };
		dv_geom_grow(&g, 5, 3, 80, 24, 1);
		expect_geom("grow", g, 0, 0, 15, 8);
		dv_geom_grow(&g, -100, -100, 80, 24, 1);
		if (g.cols < DV_MIN_INNER_COLS || g.rows < DV_MIN_INNER_ROWS)
			fail("grow min");
	}

	if (failed) {
		fprintf(stderr, "test-desktop: FAILED\n");
		return (1);
	}
	printf("test-desktop: ok\n");
	return (0);
}
