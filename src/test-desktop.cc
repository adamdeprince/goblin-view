/*
 * Geometry tests. Sizes include 132x43, 200x60, 40x12, 80x24 so 80x25
 * cannot ossify as an assumption.
 */

#include "dv.hpp"

#include <climits>
#include <stdio.h>
#include <string.h>
#include <vector>

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
	/* A border needs room for two edges and at least one content cell. */
	framed = framed && W >= 3 && H >= 3;
	unsigned	fw = dv_frame_cols(&g, framed);
	unsigned	fh = dv_frame_rows(&g, framed);

	if (g.cols > W || g.rows > H || g.x < 0 || g.y < 0 ||
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
test_tiny_windows(void)
{
	for (int framed : {0, 1}) {
		for (unsigned cols : {1u, 2u}) {
			for (unsigned rows : {1u, 2u}) {
				struct dv_geom g = {100, 50, cols, rows};

				dv_clamp_geom(40, 12, framed, &g);
				expect_geom("tiny off-screen window", g,
				    40 - cols - 2 * framed, 12 - rows - 2 * framed,
				    cols, rows);
				dv_geom_grow(&g, -100, -100, 40, 12, framed);
				if (g.cols != 1 || g.rows != 1)
					fail("shrink stops at exactly 1x1");
			}
		}
		for (unsigned W : {0u, 1u, 2u, 3u, 40u}) {
			for (unsigned H : {0u, 1u, 2u, 3u, 12u}) {
				for (struct dv_geom g : {
				    dv_geom{INT_MAX, INT_MAX, UINT_MAX, UINT_MAX},
				    dv_geom{100, 50, 80, 24},
				    dv_geom{-100, -50, 0, 0}}) {
					dv_clamp_geom(W, H, framed, &g);
					expect_fits("clamp onto tiny desktop",
					    W ? W : 1, H ? H : 1, framed, g);
				}
			}
		}
	}
}

static void
test_desktop_resize(void)
{
	struct dv_session s = {};
	struct dv_window normal = {}, zoomed = {}, hidden = {}, aside = {}, locked = {};

	dv_desktop_init(&s.desktop, {200, 60, 0, 0});
	for (auto w : {&normal, &zoomed, &hidden, &aside, &locked}) {
		w->session = &s;
		w->framed = 1;
		w->geom = {150, 50, 20, 8};
		w->lcols = 20;
		w->lrows = 8;
		TAILQ_INSERT_TAIL(&s.desktop.windows, w, entry);
	}
	s.desktop.focused = &normal;
	hidden.hidden = 1;
	aside.aside = 1;
	locked.locked = 1;
	dv_desktop_apply_zoom(&zoomed);
	for (struct dv_size sz : {dv_size{40, 12, 0, 0}, {1, 1, 0, 0},
	    {1, 2, 0, 0}, {2, 1, 0, 0}, {0, 0, 0, 0}, {200, 60, 0, 0}}) {
		dv_desktop_resize(&s.desktop, sz);
		for (auto w : {&normal, &zoomed, &hidden, &aside, &locked}) {
			expect_fits("desktop resize keeps every window on-screen",
			    s.desktop.cols, s.desktop.rows, w->framed, w->geom);
			if (!w->locked && (w->lcols != w->geom.cols ||
			    w->lrows != w->geom.rows))
				fail("logical size follows geometry");
		}
		if (locked.lcols != 20 || locked.lrows != 8)
			fail("locked logical size survives a desktop shrink");
		if (s.desktop.focused != &normal || !hidden.hidden || !aside.aside)
			fail("resize preserves focus and visibility flags");
		dv_desktop_apply_unzoom(&zoomed);
		expect_fits("unzoom after desktop resize", s.desktop.cols,
		    s.desktop.rows, zoomed.framed, zoomed.geom);
		dv_desktop_apply_zoom(&zoomed);
	}
}

/*
 * A smaller terminal attaching squeezes the windows; when it leaves, each
 * window it squeezed and nobody has touched since has its own place back.
 */
static void
test_resize_gives_back(void)
{
	struct dv_session s = {};
	struct dv_window kept = {}, moved = {}, small = {};

	dv_desktop_init(&s.desktop, {200, 60, 0, 0});
	for (auto w : {&kept, &moved, &small}) {
		w->session = &s;
		w->framed = 1;
		w->geom = {100, 30, 90, 25};
		TAILQ_INSERT_TAIL(&s.desktop.windows, w, entry);
	}
	small.geom = {2, 2, 20, 8};
	dv_desktop_resize(&s.desktop, {80, 24, 0, 0});
	expect_fits("a smaller desktop squeezes a window into it", 80, 24, 1, kept.geom);
	expect_geom("a window that already fits is left alone", small.geom, 2, 2, 20, 8);
	if (!kept.squeezed || small.squeezed)
		fail("only a window that had to change is remembered");
	/* Smaller still, then larger: still from the place it first had. */
	dv_desktop_resize(&s.desktop, {40, 12, 0, 0});
	dv_desktop_resize(&s.desktop, {120, 40, 0, 0});
	expect_fits("part of the way back fits the desktop there is", 120, 40, 1, kept.geom);
	if (kept.geom.cols != 90 || kept.geom.rows != 25)
		fail("part of the way back it has the size it had, where that fits");
	moved.geom.x = 3;			/* its user moves it */
	dv_desktop_resize(&s.desktop, {200, 60, 0, 0});
	expect_geom("the desktop back, an untouched window is where it was",
	    kept.geom, 100, 30, 90, 25);
	if (moved.geom.x != 3 || moved.squeezed)
		fail("a window moved since is where its user put it");
	if (kept.squeezed)
		fail("a window back in its place has nothing left to give back");
	expect_geom("and the small one never moved", small.geom, 2, 2, 20, 8);
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

static void
test_splits(void)
{
	for (int framed : {0, 1}) {
		for (int below : {0, 1}) {
			for (unsigned width = 1; width < 80; width++) {
				for (unsigned height = 1; height < 30; height++) {
					struct dv_geom original = {7, 9, width, height};
					struct dv_geom first = {}, second = {};
					unsigned chrome = framed ? 2 : 0;
					unsigned extent = (below ? height : width) + chrome;
					int ok = dv_split_geom(&original, framed, below, &first, &second);

					if (ok != (extent / 2 > chrome))
						fail("split refuses only when two content cells cannot fit");
					if (!ok)
						continue;
					if (first.x != original.x || first.y != original.y ||
					    (below ? first.cols != width || second.cols != width ||
					    second.x != first.x || second.y != first.y + (int)first.rows + (int)chrome ||
					    first.rows + second.rows + 2 * chrome != height + chrome :
					    first.rows != height || second.rows != height ||
					    second.y != first.y || second.x != first.x + (int)first.cols + (int)chrome ||
					    first.cols + second.cols + 2 * chrome != width + chrome))
						fail("split halves exactly cover the original rectangle without overlap");
				}
			}
		}
	}
	struct dv_geom original = {INT_MAX - 1, 0, 100, 10}, first = {}, second = {};
	if (dv_split_geom(&original, 1, 0, &first, &second))
		fail("split rejects coordinate overflow");
}

static void
test_window_ids(void)
{
	struct dv_desktop desktop = {};
	TAILQ_INIT(&desktop.windows);
	std::vector<dv_window> windows(400);
	for (size_t i = 0; i < windows.size(); i++) {
		windows[i].id = (unsigned)i + 1;
		TAILQ_INSERT_TAIL(&desktop.windows, &windows[i], entry);
	}
	if (dv_desktop_next_window_id(&desktop) != windows.size() + 1)
		fail("window ids continue beyond 255");
	TAILQ_REMOVE(&desktop.windows, &windows[17], entry);
	if (dv_desktop_next_window_id(&desktop) != 18)
		fail("closed window ids are reused");
}

/*
 * Layouts for several windows at once. Whatever the desktop and however
 * many windows, the frames of every layout but the stack share the desktop
 * exactly: each cell is in one frame and no frame is empty while there is
 * room for it.
 */
static void
test_tiles(void)
{
	static const struct { unsigned W, H; } desks[] = {
		{ 80, 24 }, { 132, 43 }, { 200, 60 }, { 40, 12 }, { 211, 57 }, { 7, 5 }
	};
	static const int layouts[] = {
		DV_TILE_GRID, DV_TILE_ROWS, DV_TILE_COLS, DV_TILE_2COL
	};

	for (const auto &dk : desks)
		for (int layout : layouts)
			for (size_t n = 1; n <= 13; n++) {
				auto	f = dv_tile_frames(dk.W, dk.H, 10, 20, layout, n);
				std::vector<unsigned> seen((size_t)dk.W * dk.H, 0);
				bool	bad = f.size() != n;

				for (const auto &g : f)
					for (unsigned y = 0; y < g.rows; y++)
						for (unsigned x = 0; x < g.cols; x++) {
							size_t at = (size_t)(g.y + (int)y) * dk.W +
							    (size_t)(g.x + (int)x);

							if (g.x < 0 || g.y < 0 || g.x + (int)g.cols > (int)dk.W ||
							    g.y + (int)g.rows > (int)dk.H) {
								bad = true;
								continue;
							}
							seen[at]++;
						}
				for (unsigned c : seen)
					bad |= c != 1;
				if (bad) {
					fprintf(stderr, "FAIL layout %d of %zu on %ux%u shares the desktop exactly\n",
					    layout, n, dk.W, dk.H);
					failed = 1;
				}
			}

	auto rows = dv_tile_frames(80, 24, 10, 20, DV_TILE_ROWS, 3);
	expect_geom("horizontal bars: the first", rows[0], 0, 0, 80, 8);
	expect_geom("horizontal bars: the last", rows[2], 0, 16, 80, 8);
	auto cols = dv_tile_frames(80, 24, 10, 20, DV_TILE_COLS, 3);
	expect_geom("vertical bars: the first", cols[0], 0, 0, 26, 24);
	expect_geom("vertical bars: the last", cols[2], 53, 0, 27, 24);
	auto two = dv_tile_frames(80, 24, 10, 20, DV_TILE_2COL, 5);
	expect_geom("two columns: three on the left", two[2], 0, 16, 40, 8);
	expect_geom("two columns: two on the right", two[4], 40, 12, 40, 12);
	auto one = dv_tile_frames(80, 24, 10, 20, DV_TILE_2COL, 1);
	expect_geom("two columns of one window is the desktop", one[0], 0, 0, 80, 24);
	auto stack = dv_tile_frames(80, 24, 10, 20, DV_TILE_STACK, 4);
	if (stack.size() != 4)
		fail("a stack has a frame for every window");
	for (const auto &g : stack)
		expect_geom("stacked windows are each the whole desktop", g, 0, 0, 80, 24);

	/*
	 * The grid is as square as the glass allows. Four windows on a
	 * desktop of 80 by 24 cells of 10 by 20 pixels -- 800 by 480 -- are
	 * two by two: 400 by 240 each, where four in a row would be 200 by
	 * 480. On cells twice as wide the same desktop is 1600 by 480 and
	 * four in a row are square.
	 */
	auto grid = dv_tile_frames(80, 24, 10, 20, DV_TILE_GRID, 4);
	expect_geom("a grid of four is two by two", grid[3], 40, 12, 40, 12);
	auto wide = dv_tile_frames(80, 24, 20, 20, DV_TILE_GRID, 4);
	expect_geom("on wider cells four are side by side", wide[3], 60, 0, 20, 24);
	/* A last row with fewer windows shares the whole width. */
	auto five = dv_tile_frames(120, 40, 10, 20, DV_TILE_GRID, 5);
	if (five[4].x + (int)five[4].cols != 120 || five[3].y != five[4].y ||
	    five[3].x != 0 || five[0].cols != 40)
		fail("a short last row of the grid fills the width");
	if (!dv_tile_frames(80, 24, 10, 20, DV_TILE_GRID, 0).empty() ||
	    !dv_tile_frames(0, 24, 10, 20, DV_TILE_ROWS, 3).empty())
		fail("no windows, or no desktop, is no frames");

	/* The controls on a frame's top edge, and the half each arrow means. */
	if (dv_frame_button(40, 32) != DV_FB_LEFT || dv_frame_button(40, 33) != DV_FB_UP ||
	    dv_frame_button(40, 34) != DV_FB_DOWN || dv_frame_button(40, 35) != DV_FB_RIGHT ||
	    dv_frame_button(40, 37) != DV_FB_SELECT || dv_frame_button(40, 36) != DV_FB_NONE ||
	    dv_frame_button(40, 39) != DV_FB_NONE || dv_frame_button(40, 5) != DV_FB_NONE)
		fail("a frame's controls sit left of its top right corner");
	if (dv_frame_button(DV_FRAME_BUTTONS_MIN - 1, DV_FRAME_BUTTONS_MIN - 4) != DV_FB_NONE ||
	    dv_frame_button(DV_FRAME_BUTTONS_MIN, DV_FRAME_BUTTONS_MIN - 3) != DV_FB_SELECT ||
	    dv_frame_button(40, 40) != DV_FB_NONE)
		fail("a narrow frame has no controls");
	{
		struct dv_geom	g = { 3, 3, 10, 5 };

		dv_snap_geom(80, 24, dv_half_snap(-1, 0), 1, &g);
		expect_geom("left half", g, 0, 0, 38, 22);
		dv_snap_geom(80, 24, dv_half_snap(1, 0), 1, &g);
		expect_geom("right half", g, 40, 0, 38, 22);
		dv_snap_geom(80, 24, dv_half_snap(0, -1), 1, &g);
		expect_geom("top half", g, 0, 0, 78, 10);
		dv_snap_geom(80, 24, dv_half_snap(0, 1), 1, &g);
		expect_geom("bottom half", g, 0, 12, 78, 10);
		if (dv_half_snap(0, 0) != 0)
			fail("no direction is no half");
	}
}

int
main(void)
{
	unsigned	sizes[][2] = {
		{80, 24}, {80, 25}, {132, 43}, {200, 60}, {40, 12},
		{10, 5}, {1, 1}, {1, 2}, {2, 1}, {2, 2}, {100, 1}, {3, 3}
	};
	unsigned	i;

	for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
		test_snaps(sizes[i][0], sizes[i][1]);
	test_80x24_unframed();
	test_132x43();
	test_200x60();
	test_40x12();
	test_clamp_after_shrink();
	test_tiny_windows();
	test_desktop_resize();
	test_zoom();
	test_unzoom_after_host_resize();
	test_splits();
	test_window_ids();
	test_tiles();
	test_resize_gives_back();

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
