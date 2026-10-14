/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Snap recipes and clamp rules. Every rectangle is a function of the live
 * desktop size — never a baked 80x25 table.
 */

#include "dv.hpp"

#include <string.h>

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
	unsigned	min_inner_c = DV_MIN_INNER_COLS;
	unsigned	min_inner_r = DV_MIN_INNER_ROWS;
	unsigned	chrome_c = framed ? 2u : 0u;
	unsigned	chrome_r = framed ? 2u : 0u;

	if (desk_cols == 0)
		desk_cols = 1;
	if (desk_rows == 0)
		desk_rows = 1;

	/* Frame is 1 cell of chrome; if the desktop cannot hold it plus a
	 * 1x1 inner, drop the frame rather than overflowing. */
	if (framed && (desk_cols < chrome_c + 1 || desk_rows < chrome_r + 1)) {
		framed = 0;
		chrome_c = 0;
		chrome_r = 0;
	}

	if (g->cols < min_inner_c)
		g->cols = min_inner_c;
	if (g->rows < min_inner_r)
		g->rows = min_inner_r;

	/* Shrink so the whole frame fits: nothing off-screen. */
	if (g->cols + chrome_c > desk_cols)
		g->cols = desk_cols - chrome_c;
	if (g->rows + chrome_r > desk_rows)
		g->rows = desk_rows - chrome_r;
	if (g->cols < 1)
		g->cols = 1;
	if (g->rows < 1)
		g->rows = 1;

	fw = dv_frame_cols(g, framed);
	fh = dv_frame_rows(g, framed);

	if (g->x < 0)
		g->x = 0;
	if (g->y < 0)
		g->y = 0;
	if ((unsigned)g->x + fw > desk_cols)
		g->x = (int)(desk_cols - fw);
	if ((unsigned)g->y + fh > desk_rows)
		g->y = (int)(desk_rows - fh);
	if (g->x < 0)
		g->x = 0;
	if (g->y < 0)
		g->y = 0;
}

void
dv_snap_geom(unsigned desk_cols, unsigned desk_rows, int snap, int framed,
    struct dv_geom *g)
{
	int		x, y;
	unsigned	w, h;
	unsigned	chrome_c = framed ? 2u : 0u;
	unsigned	chrome_r = framed ? 2u : 0u;

	if (snap < 1 || snap > DV_SNAP_COUNT)
		snap = 3;
	recipe_frame(desk_cols, desk_rows, snap, &x, &y, &w, &h);

	g->x = x;
	g->y = y;
	g->cols = w > chrome_c ? w - chrome_c : 1;
	g->rows = h > chrome_r ? h - chrome_r : 1;
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
		else
			dv_clamp_geom(d->cols, d->rows, w->framed, &w->geom);
		if (!w->locked) {
			w->lcols = w->geom.cols;
			w->lrows = w->geom.rows;
		}
	}
}
