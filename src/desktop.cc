/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Snap recipes and clamp rules. Every rectangle is a function of the live
 * desktop size — never a baked 80x25 table.
 */

#include "dv.hpp"

#include <algorithm>
#include <math.h>
#include <string.h>

unsigned
dv_desktop_next_window_id(const struct dv_desktop *d)
{
	std::unordered_set<unsigned> ids;
	const struct dv_window *w;

	TAILQ_FOREACH(w, &d->windows, entry)
		ids.insert(w->id);
	/* Zero is the protocol's no-window sentinel; unsigned wrap means
	 * the actual id space, rather than a window-count quota, ran out. */
	for (unsigned id = 1; id != 0; id++)
		if (ids.find(id) == ids.end())
			return (id);
	return (0);
}

unsigned
dv_frame_cols(const struct dv_geom *g, int framed)
{
	if (!framed)
		return (g->cols);
	return (g->cols + 2);
}

unsigned
dv_frame_rows(const struct dv_geom *g, int framed)
{
	if (!framed)
		return (g->rows);
	return (g->rows + 2);
}

/* Divide one outer rectangle, allowing for a separate frame on each half.
 * Refuse before changing either output when two content cells cannot fit. */
int
dv_split_geom(const struct dv_geom *g, int framed, int below,
    struct dv_geom *first, struct dv_geom *second)
{
	uint64_t chrome = framed ? 2 : 0;
	uint64_t extent = (below ? g->rows : g->cols) + chrome;
	uint64_t half = extent / 2;
	int64_t position = (below ? g->y : g->x) + (int64_t)half;

	if (half <= chrome || extent - half <= chrome ||
	    position > INT32_MAX || position < INT32_MIN)
		return (0);
	*first = *second = *g;
	if (below) {
		first->rows = (unsigned)(half - chrome);
		second->rows = (unsigned)(extent - half - chrome);
		second->y = (int)position;
	} else {
		first->cols = (unsigned)(half - chrome);
		second->cols = (unsigned)(extent - half - chrome);
		second->x = (int)position;
	}
	return (1);
}

static void
recipe_frame(unsigned W, unsigned H, int snap,
    int *x, int *y, unsigned *w, unsigned *h)
{
	unsigned	hw = W / 2;
	unsigned	hh = H / 2;
	unsigned	tw, th;

	if (hw == 0)
		hw = W;
	if (hh == 0)
		hh = H;
	tw = (W * 2) / 3;
	th = (H * 2) / 3;
	if (tw < 4)
		tw = W;
	if (th < 3)
		th = H;

	switch (snap) {
	case 1:	/* top half, full width */
		*x = 0;
		*y = 0;
		*w = W;
		*h = hh;
		break;
	case 2:	/* bottom half, full width */
		*x = 0;
		*y = (int)hh;
		*w = W;
		*h = H - hh;
		break;
	case 3:	/* centred ~2/3 */
		*w = tw;
		*h = th;
		*x = (int)((W - tw) / 2);
		*y = (int)((H - th) / 2);
		break;
	case 4:	/* left half */
		*x = 0;
		*y = 0;
		*w = hw;
		*h = H;
		break;
	case 5:	/* right half */
		*x = (int)hw;
		*y = 0;
		*w = W - hw;
		*h = H;
		break;
	case 6:	/* top-left */
		*x = 0;
		*y = 0;
		*w = hw;
		*h = hh;
		break;
	case 7:	/* top-right */
		*x = (int)hw;
		*y = 0;
		*w = W - hw;
		*h = hh;
		break;
	case 8:	/* bottom-left */
		*x = 0;
		*y = (int)hh;
		*w = hw;
		*h = H - hh;
		break;
	case 9:	/* bottom-right */
		*x = (int)hw;
		*y = (int)hh;
		*w = W - hw;
		*h = H - hh;
		break;
	default:
		*x = 0;
		*y = 0;
		*w = W;
		*h = H;
		break;
	}
}

void
dv_clamp_geom(unsigned desk_cols, unsigned desk_rows, int framed,
    struct dv_geom *g)
{
	unsigned	fw, fh;
	unsigned	chrome;

	if (desk_cols == 0)
		desk_cols = 1;
	if (desk_rows == 0)
		desk_rows = 1;

	framed = framed && dv_frame_fits(desk_cols, desk_rows);
	chrome = framed ? 2u : 0u;

	if (g->cols < DV_MIN_INNER_COLS)
		g->cols = DV_MIN_INNER_COLS;
	if (g->rows < DV_MIN_INNER_ROWS)
		g->rows = DV_MIN_INNER_ROWS;

	/* Cap size before position, even for a wholly off-screen window.
	 * Subtraction avoids overflowing an oversized requested dimension. */
	if (g->cols > desk_cols - chrome)
		g->cols = desk_cols - chrome;
	if (g->rows > desk_rows - chrome)
		g->rows = desk_rows - chrome;

	fw = dv_frame_cols(g, framed);
	fh = dv_frame_rows(g, framed);

	if (g->x < 0)
		g->x = 0;
	if (g->y < 0)
		g->y = 0;
	if ((unsigned)g->x > desk_cols - fw)
		g->x = (int)(desk_cols - fw);
	if ((unsigned)g->y > desk_rows - fh)
		g->y = (int)(desk_rows - fh);
}

void
dv_snap_geom(unsigned desk_cols, unsigned desk_rows, int snap, int framed,
    struct dv_geom *g)
{
	int		x, y;
	unsigned	w, h;
	unsigned	chrome = framed && dv_frame_fits(desk_cols, desk_rows) ? 2u : 0u;

	if (snap < 1 || snap > DV_SNAP_COUNT)
		snap = 3;
	recipe_frame(desk_cols, desk_rows, snap, &x, &y, &w, &h);

	g->x = x;
	g->y = y;
	g->cols = w > chrome ? w - chrome : 1;
	g->rows = h > chrome ? h - chrome : 1;
	dv_clamp_geom(desk_cols, desk_rows, framed, g);
}

void
dv_zoom_geom(unsigned desk_cols, unsigned desk_rows, struct dv_geom *g)
{
	g->x = 0;
	g->y = 0;
	g->cols = desk_cols < 1 ? 1 : desk_cols;
	g->rows = desk_rows < 1 ? 1 : desk_rows;
}

void
dv_desktop_init(struct dv_desktop *d, struct dv_size sz)
{
	memset(d, 0, sizeof *d);
	TAILQ_INIT(&d->windows);
	d->cols = sz.cols ? sz.cols : 1;
	d->rows = sz.rows ? sz.rows : 1;
	d->xpixel = sz.xpixel ? sz.xpixel : DV_DEFAULT_XPIXEL;
	d->ypixel = sz.ypixel ? sz.ypixel : DV_DEFAULT_YPIXEL;
	d->next_snap = 3;	/* centred float — overlap, not a full-width strip */
}

void
dv_desktop_apply_zoom(struct dv_window *w)
{
	struct dv_desktop	*d = &w->session->desktop;

	if (w->zoomed)
		return;
	w->saved = w->geom;
	w->zoomed = 1;
	dv_zoom_geom(d->cols, d->rows, &w->geom);
	w->framed = 0;
	if (!w->locked) {
		w->lcols = w->geom.cols;
		w->lrows = w->geom.rows;
	}
}

void
dv_geom_nudge(struct dv_geom *g, int dx, int dy,
    unsigned desk_cols, unsigned desk_rows, int framed)
{
	g->x += dx;
	g->y += dy;
	dv_clamp_geom(desk_cols, desk_rows, framed, g);
}

void
dv_geom_grow(struct dv_geom *g, int dcols, int drows,
    unsigned desk_cols, unsigned desk_rows, int framed)
{
	int	cols = (int)g->cols + dcols;
	int	rows = (int)g->rows + drows;

	if (cols < (int)DV_MIN_INNER_COLS)
		cols = (int)DV_MIN_INNER_COLS;
	if (rows < (int)DV_MIN_INNER_ROWS)
		rows = (int)DV_MIN_INNER_ROWS;
	g->cols = (unsigned)cols;
	g->rows = (unsigned)rows;
	dv_clamp_geom(desk_cols, desk_rows, framed, g);
}

void
dv_desktop_apply_unzoom(struct dv_window *w)
{
	struct dv_desktop	*d = &w->session->desktop;

	if (!w->zoomed)
		return;
	w->zoomed = 0;
	w->framed = 1;
	w->geom = w->saved;
	dv_clamp_geom(d->cols, d->rows, w->framed, &w->geom);
	if (!w->locked) {
		w->lcols = w->geom.cols;
		w->lrows = w->geom.rows;
	}
}

static int
geom_same(const struct dv_geom *a, const struct dv_geom *b)
{
	return (a->x == b->x && a->y == b->y && a->cols == b->cols &&
	    a->rows == b->rows);
}

void
dv_desktop_resize(struct dv_desktop *d, struct dv_size sz)
{
	struct dv_window	*w;

	if (sz.cols < 1)
		sz.cols = 1;
	if (sz.rows < 1)
		sz.rows = 1;
	if (sz.xpixel == 0)
		sz.xpixel = DV_DEFAULT_XPIXEL;
	if (sz.ypixel == 0)
		sz.ypixel = DV_DEFAULT_YPIXEL;

	d->cols = sz.cols;
	d->rows = sz.rows;
	d->xpixel = sz.xpixel;
	d->ypixel = sz.ypixel;

	TAILQ_FOREACH(w, &d->windows, entry) {
		if (w->zoomed)
			dv_zoom_geom(d->cols, d->rows, &w->geom);
		else {
			/*
			 * A desktop follows its smallest terminal, so one that
			 * attaches for a moment squeezes every window. A window
			 * still where that left it is fitted from the place it
			 * had, so the desktop growing again gives that back. One
			 * moved or sized since is where its user wants it.
			 */
			struct dv_geom	from = w->geom, to;

			if (w->squeezed && geom_same(&w->geom, &w->fitted))
				from = w->want;
			to = from;
			dv_clamp_geom(d->cols, d->rows, w->framed, &to);
			w->squeezed = !geom_same(&to, &from);
			if (w->squeezed) {
				w->want = from;
				w->fitted = to;
			}
			w->geom = to;
		}
		if (!w->locked) {
			w->lcols = w->geom.cols;
			w->lrows = w->geom.rows;
		}
	}
}

/* The snap recipe for an arrow: the half of the desktop on that side. */
int
dv_half_snap(int dx, int dy)
{
	if (dx < 0)
		return (4);
	if (dx > 0)
		return (5);
	if (dy < 0)
		return (1);
	if (dy > 0)
		return (2);
	return (0);
}

int
dv_frame_button(unsigned frame_cols, unsigned x)
{
	if (frame_cols < DV_FRAME_BUTTONS_MIN || x >= frame_cols)
		return (DV_FB_NONE);
	switch (frame_cols - x) {
	case 8:
		return (DV_FB_LEFT);
	case 7:
		return (DV_FB_UP);
	case 6:
		return (DV_FB_DOWN);
	case 5:
		return (DV_FB_RIGHT);
	case 3:
		return (DV_FB_SELECT);
	default:
		return (DV_FB_NONE);
	}
}

/* Where part `i` of `n` equal parts of `extent` starts: the parts differ by
 * a cell at most and leave none over. */
static unsigned
part(unsigned extent, size_t i, size_t n)
{
	return ((unsigned)((uint64_t)extent * i / n));
}

static struct dv_geom
frame_rect(unsigned x0, unsigned y0, unsigned x1, unsigned y1)
{
	struct dv_geom	g;

	g.x = (int)x0;
	g.y = (int)y0;
	g.cols = x1 > x0 ? x1 - x0 : 0;
	g.rows = y1 > y0 ? y1 - y0 : 0;
	return (g);
}

/*
 * The frames of n windows laid out together: outer rectangles, borders
 * included, in the order the windows are given. They share the whole desktop
 * with no cell over and none twice, except DV_TILE_STACK where each has all
 * of it. With more windows than a layout has cells for, the rectangles come
 * out too small to be windows; the caller's clamp then lets them overlap
 * rather than refuse.
 *
 * DV_TILE_GRID picks how many columns by what the windows would look like,
 * which is why it wants the pixels of a cell. A last row with fewer windows
 * shares the width among them, so no part of the screen is empty; of every
 * count of columns it could use, it takes the one whose least square tile
 * on the glass, those wider ones included, is nearest square.
 */
std::vector<struct dv_geom>
dv_tile_frames(unsigned W, unsigned H, unsigned xpixel, unsigned ypixel,
    int layout, size_t n)
{
	std::vector<struct dv_geom>	out;
	size_t				i;

	if (n == 0 || W == 0 || H == 0)
		return (out);
	if (xpixel == 0)
		xpixel = DV_DEFAULT_XPIXEL;
	if (ypixel == 0)
		ypixel = DV_DEFAULT_YPIXEL;
	switch (layout) {
	case DV_TILE_ROWS:
		for (i = 0; i < n; i++)
			out.push_back(frame_rect(0, part(H, i, n), W, part(H, i + 1, n)));
		break;
	case DV_TILE_COLS:
		for (i = 0; i < n; i++)
			out.push_back(frame_rect(part(W, i, n), 0, part(W, i + 1, n), H));
		break;
	case DV_TILE_2COL: {
		size_t	left = (n + 1) / 2, right = n - left;
		unsigned mid = right > 0 ? W / 2 : W;

		for (i = 0; i < left; i++)
			out.push_back(frame_rect(0, part(H, i, left), mid,
			    part(H, i + 1, left)));
		for (i = 0; i < right; i++)
			out.push_back(frame_rect(mid, part(H, i, right), W,
			    part(H, i + 1, right)));
		break;
	}
	case DV_TILE_GRID: {
		size_t	best = 1, c;
		double	best_score = -1;

		for (c = 1; c <= n; c++) {
			size_t	r = (n + c - 1) / c;
			size_t	last = n - c * (r - 1);	/* windows in the last row */
			double	th = (double)H / (double)r * ypixel;
			double	tw = (double)W / (double)c * xpixel;
			double	lw = (double)W / (double)last * xpixel;
			double	score;

			if (tw <= 0 || th <= 0)
				continue;
			/* Its least square tile: those of the last row are wider. */
			score = std::max(fabs(log(tw / th)), fabs(log(lw / th)));
			if (best_score < 0 || score < best_score - 1e-9) {
				best_score = score;
				best = c;
			}
		}
		size_t	rows = (n + best - 1) / best;

		for (i = 0; i < n; i++) {
			size_t	r = i / best, k = i % best;
			size_t	in_row = r + 1 < rows ? best : n - best * (rows - 1);

			out.push_back(frame_rect(part(W, k, in_row), part(H, r, rows),
			    part(W, k + 1, in_row), part(H, r + 1, rows)));
		}
		break;
	}
	case DV_TILE_STACK:
	default:
		for (i = 0; i < n; i++)
			out.push_back(frame_rect(0, 0, W, H));
		break;
	}
	return (out);
}
