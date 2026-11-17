/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Overlapping compositor. Windows are z-ordered rectangles on the
 * desktop; a frame is chrome, not a tmux split. The desktop is
 * composed into cells once per change, then each client gets only the
 * cells that differ from what it saw last.
 */

#include "dv.hpp"
#include "pstatus.hpp"

#include <algorithm>
#include <map>
#include <unordered_set>
#include <vector>

#include <math.h>

#include <stdlib.h>
#include <string.h>

#define CP_H	0x2500u
#define CP_V	0x2502u
#define CP_TL	0x250Cu
#define CP_TR	0x2510u
#define CP_BL	0x2514u
#define CP_BR	0x2518u


uint32_t	dv_backdrop_glyph = 0x2591;	/* the backdrop stipple */
static void
utf8_put(struct dv_buf *b, uint32_t cp)
{
	unsigned char	u[4];
	unsigned	n;

	if (cp < 0x80) {
		u[0] = (unsigned char)cp;
		n = 1;
	} else if (cp < 0x800) {
		u[0] = (unsigned char)(0xc0 | (cp >> 6));
		u[1] = (unsigned char)(0x80 | (cp & 0x3f));
		n = 2;
	} else if (cp < 0x10000) {
		u[0] = (unsigned char)(0xe0 | (cp >> 12));
		u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
		u[2] = (unsigned char)(0x80 | (cp & 0x3f));
		n = 3;
	} else {
		u[0] = (unsigned char)(0xf0 | (cp >> 18));
		u[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
		u[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
		u[3] = (unsigned char)(0x80 | (cp & 0x3f));
		n = 4;
	}
	dv_buf_append(b, u, n);
}

static void
put_desk(struct dv_cell *desk, unsigned W, unsigned H,
    int x, int y, uint32_t ch, uint32_t fg, uint32_t bg, uint8_t attr)
{
	struct dv_cell	*c;

	if (x < 0 || y < 0)
		return;
	if ((unsigned)x >= W || (unsigned)y >= H)
		return;
	c = &desk[(unsigned)y * W + (unsigned)x];
	c->ch = ch;
	c->fg = fg;
	c->bg = bg;
	c->attr = attr;
	c->ustyle = 0;
	c->ucolor = DV_COLOR_DEFAULT;
	c->alpha = 100;
	c->fg_br = c->bg_br = 100;
	c->fg_gamut = c->bg_gamut = 0;
	c->fg_x = c->bg_x = c->u_x = 0;
	c->font = 0;
	c->depth = 0;
	c->focus = 0;
}

/* A window's colour overrides (OSC 4/10/11/12/21) applied to one colour:
 * an overridden palette index or default becomes its RGB; all else passes. */
static uint32_t
pal_map(const struct dv_palette *p, uint32_t col, int bg)
{
	if (col == DV_COLOR_DEFAULT)
		return (bg ? (p->bgset ? p->bg : col) : (p->fgset ? p->fg : col));
	if (!(col & DV_COLOR_RGB) && col < 256 && p->set[col])
		return (p->idx[col]);
	return (col);
}

static int	cell_eq(const struct dv_cell *, const struct dv_cell *);

/*
 * Is this window's picture more than placeholder cells can say? A cell holds
 * one image, drawn above its text from the cell's corner. Placements under
 * the text (z below 0), offset within a cell, or overlapping one another need
 * real placements on the terminal: the window is layered, and stays so until
 * its direct placements are gone, so that a sprite alone for a moment is not
 * sent again another way.
 */
static void
window_layering(struct dv_window *w)
{
	struct dv_screen	*s = &w->screen;
	std::vector<uint8_t>	 taken;
	bool			 any = false, need = false;

	if (s->kpl != nullptr)
		for (const auto &[id, p] : *s->kpl) {
			if (!p.used || p.virt || p.alt != s->alt_on)
				continue;
			any = true;
			if (p.z < 0 || p.off_x != 0 || p.off_y != 0) {
				need = true;
				break;
			}
		}
	if (!any) {
		w->klayered = 0;
		return;
	}
	if (w->klayered || need) {
		w->klayered = 1;
		return;
	}
	/* Two on one cell? Marked on a map of the screen, a cell at a time. */
	taken.assign((size_t)s->cols * s->rows, 0);
	for (const auto &[id, p] : *s->kpl) {
		if (!p.used || p.virt || p.alt != s->alt_on)
			continue;
		for (unsigned r = 0; r < p.rows; r++) {
			long	y = (long)p.row + (long)r;

			if (y < 0 || y >= (long)s->rows)
				continue;
			for (unsigned c = 0; c < p.cols; c++) {
				long	x = (long)p.col + (long)c;

				if (x < 0 || x >= (long)s->cols)
					continue;
				uint8_t	*t = &taken[(size_t)y * s->cols + (size_t)x];

				if (*t) {
					w->klayered = 1;
					return;
				}
				*t = 1;
			}
		}
	}
}

/*
 * Direct image placements painted as placeholder cells, clipped exactly as
 * the text is: to the screen, to the window's viewport, and to the desktop.
 * In paint_window a window painted later covers them cell by cell, like
 * anything else. For a layered window shown to a terminal that cannot take
 * its real placements, `own` says which cells are still the window's: the
 * placements go in stacking order, and one under the text leaves the cells
 * that have text alone, which is as near as a cell can come to it.
 */
static void
stamp_placements(struct dv_cell *desk, unsigned W, unsigned H,
    struct dv_window *w, const std::vector<unsigned> *own)
{
	int		 framed = dv_window_framed(w);
	int		 ix = framed ? w->geom.x + 1 : w->geom.x;
	int		 iy = framed ? w->geom.y + 1 : w->geom.y;
	std::vector<const dv_kplace *> placements;

	if (w->screen.kpl != nullptr)
		for (const auto &[id, p] : *w->screen.kpl)
			placements.push_back(&p);
	/* Hash iteration must not change which overlapping image is on top. */
	std::sort(placements.begin(), placements.end(), [own](auto a, auto b) {
		if (own != nullptr && a->z != b->z)
			return (a->z < b->z);
		if (own != nullptr && a->child_id != b->child_id)
			return (a->child_id < b->child_id);
		return (a->dv_id < b->dv_id);
	});
	for (const auto *p : placements) {
		unsigned		 r, c;

		if (!p->used || p->virt || p->alt != w->screen.alt_on ||
		    !w->kimg->at(p->img).done)
			continue;
		for (r = 0; r < p->rows; r++) {
			int	wy = p->row + (int)r;
			int	y = wy - (int)w->oy;

			if (wy < 0 || wy >= (int)w->screen.rows || y < 0 ||
			    (unsigned)y >= w->geom.rows)
				continue;
			for (c = 0; c < p->cols; c++) {
				int		 wx = p->col + (int)c;
				int		 x = wx - (int)w->ox;
				int		 dx = ix + x, dy = iy + y;
				struct dv_cell	*dc;

				if (wx < 0 || wx >= (int)w->screen.cols ||
				    x < 0 || (unsigned)x >= w->geom.cols ||
				    dx < 0 || dy < 0 || (unsigned)dx >= W ||
				    (unsigned)dy >= H)
					continue;
				dc = &desk[(unsigned)dy * W + (unsigned)dx];
				if (own != nullptr) {
					if ((*own)[(size_t)dy * W + (size_t)dx] != w->id)
						continue;
					if (p->z < 0 && dc->ch != DV_KITTY_PLACEHOLDER &&
					    ((dc->ch != U' ' && dc->ch != 0) ||
					    dc->bg != DV_COLOR_DEFAULT ||
					    (dc->attr & DV_ATTR_REV)))
						continue;
				}
				dc->ch = DV_KITTY_PLACEHOLDER;
				dc->fg = p->dv_id;
				dc->bg = DV_COLOR_DEFAULT;
				dc->attr = 0;
				dc->ustyle = 0;
				dc->ucolor = DV_COLOR_DEFAULT;
				dc->irow = (uint16_t)r;
				dc->icol = (uint16_t)c;
			}
		}
	}
}

/*
 * Text the frame shows from a program: what fits in cols columns from
 * (x, y), wide glyphs as two cells. Characters that only steer the text
 * (bidi overrides, joiners, other invisible formatting) are left out, so
 * a program cannot make the frame read as something else. Returns the
 * columns used.
 */
static unsigned
put_text(struct dv_cell *desk, unsigned W, unsigned H, int x, int y,
    unsigned cols, const std::string &utf8, uint32_t fg, uint32_t bg,
    uint8_t attr)
{
	unsigned	used = 0;
	size_t		i = 0;

	while (i < utf8.size() && used < cols) {
		unsigned char	c = (unsigned char)utf8[i];
		uint32_t	cp;
		int		len, wd;

		if (c < 0x80) {
			cp = c;
			len = 1;
		} else if ((c & 0xe0) == 0xc0) {
			cp = c & 0x1f;
			len = 2;
		} else if ((c & 0xf0) == 0xe0) {
			cp = c & 0x0f;
			len = 3;
		} else {
			cp = c & 0x07;
			len = 4;
		}
		if (i + (size_t)len > utf8.size())
			break;
		for (int k = 1; k < len; k++)
			cp = (cp << 6) | ((unsigned char)utf8[i + (size_t)k] & 0x3f);
		i += (size_t)len;
		if ((cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) ||
		    (cp >= 0x2060 && cp <= 0x206f) || cp == 0xfeff || cp == 0x061c ||
		    cp == 0x180e || cp < 0x20 || (cp >= 0x7f && cp <= 0x9f))
			continue;
		wd = dv_wcwidth(cp);
		if (wd <= 0)
			continue;		/* a mark with nothing to sit on here */
		if (wd == 2) {
			if (used + 2 > cols)
				break;
			put_desk(desk, W, H, x + (int)used, y, cp, fg, bg,
			    (uint8_t)(attr | DV_ATTR_WIDE));
			put_desk(desk, W, H, x + (int)used + 1, y, 0, fg, bg, attr);
			used += 2;
		} else {
			put_desk(desk, W, H, x + (int)used, y, cp, fg, bg, attr);
			used++;
		}
	}
	return (used);
}

/*
 * The program status (OSC 7501) of a framed window: a badge after the
 * title for its most urgent record -- the percentage or an ellipsis while
 * working, a check mark when done, a cross when it failed, black on
 * yellow when it is blocked on the user (? for a question), with the
 * number of records in that state when more than one -- and on the
 * bottom border what that record says.
 */
static void
paint_status(struct dv_cell *desk, unsigned W, unsigned H,
    const struct dv_window *w, int fx, int fy, unsigned fw, unsigned fh,
    uint32_t fbg, unsigned after)
{
	struct dv_pstatus_view	 v;
	std::string		 badge;
	uint32_t		 bfg = 15, bbg = fbg;
	unsigned		 reserve = fw >= DV_FRAME_BUTTONS_MIN ?
				    DV_FRAME_BUTTONS_W : 0;

	if (!dv_pstatus_view_of(w, &v))
		return;
	switch (v.state) {
	case DV_PS_WORKING:
		badge = v.progress >= 0 ? std::to_string(v.progress) + "%" :
		    "⋯";
		bfg = 14;
		break;
	case DV_PS_DONE:
		badge = "✓";
		bfg = 10;
		break;
	case DV_PS_ERROR:
		badge = "✗";
		bfg = 9;
		break;
	case DV_PS_BLOCKED:
		badge = v.kind == DV_PS_KIND_QUESTION ? "?" : "!";
		if (v.progress >= 0)
			badge += " " + std::to_string(v.progress) + "%";
		bfg = 0;
		bbg = 11;
		break;
	default:
		return;
	}
	if (v.count > 1)
		badge += "×" + std::to_string(v.count);
	badge = " " + badge + " ";
	/* After the title, where the buttons leave room. */
	if (fw > after + reserve + 2)
		put_text(desk, W, H, fx + (int)after, fy, fw - after - reserve - 2,
		    badge, bfg, bbg, DV_ATTR_BOLD);
	/* What it says, along the bottom. */
	if (fh >= 2 && fw > 4 && !v.text.empty())
		put_text(desk, W, H, fx + 2, fy + (int)fh - 1, fw - 4,
		    " " + v.text + " ", 15, fbg, 0);
}

static void
paint_window(struct dv_cell *desk, unsigned W, unsigned H, struct dv_window *w)
{
	int		 framed = dv_window_framed(w);
	int		 fx = w->geom.x, fy = w->geom.y;
	unsigned	 fw = dv_frame_cols(&w->geom, framed);
	unsigned	 fh = dv_frame_rows(&w->geom, framed);
	int		 ix, iy;
	unsigned	 x, y;
	int		 focus = (w->session->desktop.focused == w);
	/* Yellow with the keyboard, magenta when selected, else cyan. */
	uint32_t	 ffg = focus ? 11 : w->selected ? 13 : 6;
	uint32_t	 fbg = 4;
	std::vector<unsigned> *own = w->session->owner;

	if (w->hidden || w->aside)
		return;

	ix = framed ? fx + 1 : fx;
	iy = framed ? fy + 1 : fy;

	/* Its frame is nobody's own cells; its inside becomes its own below. */
	if (own != nullptr)
		for (y = 0; y < fh; y++)
			for (x = 0; x < fw; x++) {
				int	dx = fx + (int)x, dy = fy + (int)y;

				if (dx >= 0 && dy >= 0 && (unsigned)dx < W &&
				    (unsigned)dy < H)
					(*own)[(size_t)dy * W + (size_t)dx] = 0;
			}

	if (framed) {
		char		 title[80];
		unsigned	 ti, tlen;
		int		 editing = (w->session != nullptr &&
				    w->session->title_edit &&
				    w->session->title_win == w->id);

		if (editing)
			snprintf(title, sizeof title, " %u:%s ", w->id,
			    w->session->title_ed);
		else
			snprintf(title, sizeof title, " %u:%s ", w->id,
			    w->title[0] ? w->title : w->cmd);
		tlen = (unsigned)strlen(title);
		for (x = 0; x < fw; x++) {
			uint32_t	ch;

			if (x == 0)
				ch = CP_TL;
			else if (x + 1 == fw)
				ch = CP_TR;
			else
				ch = CP_H;
			put_desk(desk, W, H, fx + (int)x, fy, ch, ffg, fbg, 0);
			if (x == 0)
				ch = CP_BL;
			else if (x + 1 == fw)
				ch = CP_BR;
			else
				ch = CP_H;
			put_desk(desk, W, H, fx + (int)x, fy + (int)fh - 1,
			    ch, ffg, fbg, 0);
		}
		for (y = 1; y + 1 < fh; y++) {
			put_desk(desk, W, H, fx, fy + (int)y, CP_V, ffg, fbg, 0);
			put_desk(desk, W, H, fx + (int)fw - 1, fy + (int)y,
			    CP_V, ffg, fbg, 0);
		}
		/* Its controls, right of the title: a half of the desktop each
		 * way, and whether it is one of the selected (dv_frame_button). */
		unsigned	 reserve = fw >= DV_FRAME_BUTTONS_MIN ?
				    DV_FRAME_BUTTONS_W : 0;

		for (x = 0; x < fw; x++) {
			uint32_t	glyph = 0;

			switch (dv_frame_button(fw, x)) {
			case DV_FB_LEFT:
				glyph = 0x2190;
				break;
			case DV_FB_UP:
				glyph = 0x2191;
				break;
			case DV_FB_DOWN:
				glyph = 0x2193;
				break;
			case DV_FB_RIGHT:
				glyph = 0x2192;
				break;
			case DV_FB_SELECT:
				glyph = w->selected ? 0x25CF : 0x25CB;
				break;
			}
			if (glyph != 0)
				put_desk(desk, W, H, fx + (int)x, fy, glyph,
				    glyph == 0x25CF ? 13 : 15, fbg, DV_ATTR_BOLD);
		}
		for (ti = 0; ti < tlen && ti + 2 + reserve < fw; ti++) {
			unsigned char	tc = (unsigned char)title[ti];
			uint8_t		 a = editing ? (DV_ATTR_BOLD | DV_ATTR_REV)
					    : DV_ATTR_BOLD;

			put_desk(desk, W, H, fx + 1 + (int)ti, fy,
			    tc < 0x80 ? tc : U'?', 15, fbg, a);
		}
		if (editing && tlen + 2 + reserve < fw)	/* a block cursor at the end */
			put_desk(desk, W, H, fx + 1 + (int)tlen, fy, U' ', 15,
			    fbg, DV_ATTR_REV);
		if (!editing)
			paint_status(desk, W, H, w, fx, fy, fw, fh, fbg,
			    1 + (tlen + 2 + reserve < fw ? tlen : fw - 2 - reserve));
	}

	for (y = 0; y < w->geom.rows; y++) {
		for (x = 0; x < w->geom.cols; x++) {
			unsigned	 sx = x + w->ox;
			unsigned	 sy = y + w->oy;
			struct dv_cell	*c = dv_screen_disp_at(&w->screen, sx, sy);
			int		 dx = ix + (int)x, dy = iy + (int)y;

			if (c == nullptr || dx < 0 || dy < 0 ||
			    (unsigned)dx >= W || (unsigned)dy >= H)
				continue;
			/* Whole-cell copy so image placeholders keep irow/icol. */
			desk[(unsigned)dy * W + (unsigned)dx] = *c;
			if (own != nullptr)
				(*own)[(size_t)dy * W + (size_t)dx] = w->id;
			if (w->screen.pal.any && c->ch != DV_KITTY_PLACEHOLDER &&
			    c->ch != DV_STEXT_MARK) {
				/* This window's own colours, without touching the
				 * host's palette that every other window uses. */
				struct dv_cell	*dc = &desk[(unsigned)dy * W +
						    (unsigned)dx];

				dc->fg = pal_map(&w->screen.pal, dc->fg, 0);
				dc->bg = pal_map(&w->screen.pal, dc->bg, 1);
				if (dc->attr & DV_ATTR_UNDER)
					dc->ucolor = pal_map(&w->screen.pal, dc->ucolor, 0);
			}
			{
				/* An extra cursor here: noted for a host that draws
				 * them, or painted as reverse/underline for one
				 * that cannot. A later window over it wins. */
				struct dv_session	*ss = w->session;
				int			 sh = w->screen.nxcur > 0 ?
				    dv_xcur_at(&w->screen, sx, sy) : 0;

				if (ss != nullptr && ss->xcur_map != nullptr &&
				    (size_t)dy * W + (size_t)dx < ss->xcur_map_n) {
					ss->xcur_map[(size_t)dy * W + (size_t)dx] = (uint8_t)sh;
					if (sh)
						ss->xcur_any = 1;
				}
				if (sh && (ss == nullptr || !ss->desktop.mcursors_host) &&
				    c->ch != DV_KITTY_PLACEHOLDER) {
					struct dv_cell	*dc = &desk[(unsigned)dy * W +
							    (unsigned)dx];

					if (sh == 1 || sh == 29)
						dc->attr |= DV_ATTR_REV;
					else {
						dc->attr |= DV_ATTR_UNDER;
						dc->ustyle = sh == 2 ? 3 : 1;
					}
				}
			}
			if (c->ch == DV_KITTY_PLACEHOLDER) {
				/*
				 * A placeholder the program wrote itself (U=1
				 * mode) names its image by the program's id;
				 * the host knows that image by ours.
				 */
				struct dv_cell	*dc = &desk[(unsigned)dy * W +
						    (unsigned)dx];
				unsigned	 hid = dv_kitty_virtual_id(w, c->fg);

				if (hid != 0)
					dc->fg = hid;
				else {
					dc->ch = U' ';
					dc->fg = DV_COLOR_DEFAULT;
					dc->attr = 0;
					dc->ustyle = 0;
					dc->ucolor = DV_COLOR_DEFAULT;
					dc->irow = dc->icol = 0;
					dc->font = 0;
					dc->depth = 0;
				}
			}
			if (w->session != nullptr &&
			    w->session->sel_win == w->id &&
			    dv_sel_contains(w->session, (int)sx, (int)sy))
				desk[(unsigned)dy * W + (unsigned)dx].attr ^=
				    DV_ATTR_REV;
		}
	}

	/*
	 * Direct image placements float above the text. A layered window's are
	 * not cells at all: each terminal is sent them as it can show them.
	 */
	if (!w->klayered)
		stamp_placements(desk, W, H, w, nullptr);

	/* Everything it covers is the focused window's, or now not. */
	for (y = 0; y < fh; y++)
		for (x = 0; x < fw; x++) {
			int	dx = fx + (int)x, dy = fy + (int)y;

			if (dx >= 0 && dy >= 0 && (unsigned)dx < W && (unsigned)dy < H)
				desk[(unsigned)dy * W + (unsigned)dx].focus =
				    (uint8_t)focus;
		}
}

static const char *
mode_text(int mode)
{
	switch (mode) {
	case DV_IN_REARRANGE:
		return ("REARRANGE  arrow: that half  M move  R resize  P 1-9 snap  H hide  Esc");
	case DV_IN_TILE:
		return ("LAYOUT  F full, stacked  G grid  H rows  V columns  2 two columns  Esc");
	case DV_IN_MOVE:
		return ("MOVE  arrows  Shift=5  Enter done  Esc cancel");
	case DV_IN_RESIZE:
		return ("RESIZE  arrows (top-left stays)  Shift=5  Enter  Esc");
	case DV_IN_POSITION:
		return ("POSITION  1-9 snap  Esc cancel");
	case DV_IN_SPLIT:
		return ("SPLIT  R right  B below  Esc cancel");
	default:
		return (nullptr);
	}
}

static void
paint_status(struct dv_cell *desk, unsigned W, unsigned H, const char *msg)
{
	unsigned	 x, len;
	int		 y;

	if (msg == nullptr || H == 0 || W == 0)
		return;
	y = (int)H - 1;
	len = (unsigned)strlen(msg);
	for (x = 0; x < W; x++) {
		unsigned char	ch = x < len ? (unsigned char)msg[x] : ' ';

		put_desk(desk, W, H, (int)x, y, ch, 0, 6, DV_ATTR_BOLD);
	}
}

/*
 * A wide glyph needs both of its cells. Where a window edge, a frame or
 * the menu took one of them, show a space instead; where both are there,
 * make the second cell a plain continuation so equal desks compare equal.
 */
static void
normalize_wide(struct dv_cell *desk, unsigned W, unsigned H)
{
	unsigned	x, y;

	for (y = 0; y < H; y++) {
		struct dv_cell	*row = desk + y * W;

		for (x = 0; x < W; x++) {
			struct dv_cell	*c = &row[x];

			if (c->attr & DV_ATTR_WIDE) {
				if (x + 1 < W && row[x + 1].ch == 0) {
					row[x + 1].fg = c->fg;
					row[x + 1].bg = c->bg;
					row[x + 1].attr =
					    c->attr & (uint8_t)~DV_ATTR_WIDE;
					x++;
					continue;
				}
				c->ch = U' ';
				c->attr &= (uint8_t)~DV_ATTR_WIDE;
			} else if (c->ch == 0)
				c->ch = U' ';
		}
	}
}

/* The input-method status bar: method, preedit, and numbered candidates. */
static void
paint_im_bar(struct dv_cell *desk, unsigned W, unsigned H,
    struct dv_session *s)
{
	unsigned	 x = 0, col;
	int		 i;
	const char	*name = dv_im_name(s->im.method);
	char		 head[32];
	int		 hn;

	if (W == 0 || H == 0)
		return;
	for (col = 0; col < W; col++)
		put_desk(desk, W, H, (int)col, (int)H - 1, U' ', 0, 6,
		    DV_ATTR_BOLD);
	hn = snprintf(head, sizeof head, " %s: ", name);
	for (i = 0; i < hn && x < W; i++, x++)
		put_desk(desk, W, H, (int)x, (int)H - 1,
		    (unsigned char)head[i], 0, 6, DV_ATTR_BOLD);
	for (i = 0; i < s->im.prelen && x < W; i++, x++)
		put_desk(desk, W, H, (int)x, (int)H - 1,
		    (unsigned char)s->im.pre[i], 15, 6, DV_ATTR_BOLD);
	for (i = 0; i < s->im.ncand && x + 3 < W; i++) {
		char		 num[6];
		int		 k, nn = snprintf(num, sizeof num, " %d.", i + 1);
		const char	*cp = s->im.cand[i];

		for (k = 0; k < nn && x < W; k++, x++)
			put_desk(desk, W, H, (int)x, (int)H - 1,
			    (unsigned char)num[k], 11, 6, DV_ATTR_BOLD);
		while (*cp != '\0' && x < W) {		/* decode UTF-8 */
			uint32_t	ch;
			int		need;

			if ((*cp & 0x80) == 0) { ch = (unsigned char)*cp; need = 0; }
			else if ((*cp & 0xe0) == 0xc0) { ch = *cp & 0x1f; need = 1; }
			else if ((*cp & 0xf0) == 0xe0) { ch = *cp & 0x0f; need = 2; }
			else { ch = *cp & 0x07; need = 3; }
			cp++;
			while (need-- > 0 && (*cp & 0xc0) == 0x80)
				ch = (ch << 6) | (*cp++ & 0x3f);
			put_desk(desk, W, H, (int)x++, (int)H - 1, ch, 0, 6,
			    DV_ATTR_BOLD);
		}
	}
}

void
dv_compose_cells(struct dv_session *s, struct dv_cell *desk)
{
	struct dv_desktop	*d = &s->desktop;
	struct dv_window	*w;
	std::vector<dv_window *> list;
	std::vector<dv_cell>	 under;
	unsigned		 i, W = d->cols, H = d->rows;
	const char		*st;
	bool			 layered = false;

	dv_pstatus_focus(s);	/* a window come back to lets its results go */
	if (W == 0 || H == 0)
		return;
	/* Backdrop: a dim stipple on black, so a floating window reads as
	 * floating and the desktop is never mistaken for a window. */
	for (i = 0; i < W * H; i++) {
		desk[i].ch = dv_backdrop_glyph;	/* light shade ░ unless configured */
		desk[i].fg = 8;		/* bright black (grey) */
		desk[i].bg = 0;		/* black */
		desk[i].attr = 0;
		desk[i].alpha = 100;
		desk[i].fg_br = desk[i].bg_br = 100;
		desk[i].fg_gamut = desk[i].bg_gamut = 0;
		desk[i].fg_x = desk[i].bg_x = desk[i].u_x = 0;
		desk[i].font = 0;
		desk[i].depth = 0;
		desk[i].focus = 0;
	}
	TAILQ_FOREACH(w, &d->windows, entry)
		list.push_back(w);
	std::sort(list.begin(), list.end(), [](const auto *a, const auto *b) {
		return (a->z < b->z);
	});
	if (s->xcur_map == nullptr || s->xcur_map_n != (size_t)W * H) {
		free(s->xcur_map);
		s->xcur_map = dv_allocn<uint8_t>((size_t)W * H);
		s->xcur_map_n = (size_t)W * H;
	}
	memset(s->xcur_map, 0, s->xcur_map_n);
	s->xcur_any = 0;
	for (auto *window : list) {
		window_layering(window);
		if (window->klayered && !window->hidden && !window->aside)
			layered = true;
	}
	if (layered) {
		if (s->owner == nullptr)
			s->owner = new std::vector<unsigned>;
		s->owner->assign((size_t)W * H, 0);
	} else {
		delete s->owner;
		s->owner = nullptr;
	}
	for (auto *window : list)
		paint_window(desk, W, H, window);
	/* What is drawn over the windows from here on takes their cells. */
	if (layered)
		under.assign(desk, desk + (size_t)W * H);
	st = mode_text(s->input_mode);
	if (st == nullptr && s->broadcast) {
		static char	bst[128];
		size_t		nsel = 0;

		for (auto *window : list)
			nsel += window->selected != 0;
		if (nsel == 0)
			st = "BROADCAST  typing goes to every window   (menu B stops it)";
		else if (d->focused != nullptr && d->focused->selected) {
			snprintf(bst, sizeof bst, "BROADCAST  typing goes to the %zu "
			    "selected windows   (menu B stops it)", nsel);
			st = bst;
		} else
			st = "BROADCAST  this window is not selected: typing stays "
			    "here   (menu B stops it)";
	}
	if (s->copy_open) {
		struct dv_window	*cw = nullptr, *it;
		static char		 cst[128];

		TAILQ_FOREACH(it, &s->desktop.windows, entry)
			if (it->id == s->copy_win)
				cw = it;
		if (s->copy_searching)
			snprintf(cst, sizeof cst, "%c%s_", s->copy_searching > 0 ? '/' : '?',
			    s->copy_pattern);
		else
			snprintf(cst, sizeof cst, "COPY  hjkl/arrows move  v select  y copy  "
			    "/ ? search  n N next  g G top/bottom  q quit");
		st = cst;
		if (cw != nullptr && !cw->hidden && !cw->aside) {
			/* the copy cursor: a reverse cell in the window */
			int	framed = dv_window_framed(cw);
			long	top = cw->screen.alt_on ? 0 :
				    (long)cw->screen.sb_len - (long)cw->screen.sb_off;
			long	row = s->copy_y - top - (long)cw->oy;
			long	col = (long)s->copy_x - (long)cw->ox;
			long	dx = cw->geom.x + (framed ? 1 : 0) + col;
			long	dy = cw->geom.y + (framed ? 1 : 0) + row;

			if (row >= 0 && row < (long)cw->geom.rows && col >= 0 &&
			    col < (long)cw->geom.cols && dx >= 0 && dy >= 0 &&
			    dx < (long)W && dy < (long)H)
				desk[(unsigned)dy * W + (unsigned)dx].attr ^= DV_ATTR_REV;
		}
	}
	if (st != nullptr)
		paint_status(desk, W, H, st);
	if (s->menu_open)
		dv_menu_paint(desk, W, H);
	if (s->switch_open)
		dv_switch_paint(desk, W, H, s);
	if (s->uni_open)
		dv_uni_paint(desk, W, H, s->uni_cat, s->uni_sel);
	if (s->quit_confirm)
		dv_confirm_paint(desk, W, H,
		    "Quit odin-seat and close every window?   y / n");
	if (s->kill_confirm) {
		std::string question = "Force-kill window " + std::to_string(s->kill_confirm) +
		    "? Unsaved work is lost.  y / n";

		dv_confirm_paint(desk, W, H,
		    question.c_str());
	}
	if (s->help_open)
		dv_help_paint(desk, W, H);
	if (s->xfer_prompt) {			/* a file transfer asks */
		const char	*lines[4];
		int		 i;

		for (i = 0; i < s->xfer_nlines && i < 4; i++)
			lines[i] = s->xfer_text[i];
		dv_prompt_paint(desk, W, H, lines, (unsigned)i);
	}
	if (s->im_choose)
		dv_confirm_paint(desk, W, H,
		    "Input method:  O off  R romaji  U us-intl  "
		    "P pinyin  Z zhuyin  W wubi");
	else if (s->im.method != DV_IM_OFF)
		paint_im_bar(desk, W, H, s);
	if (layered)
		for (size_t at = 0; at < under.size(); at++)
			if (!cell_eq(&desk[at], &under[at]))
				(*s->owner)[at] = 0;
	normalize_wide(desk, W, H);
}

void
dv_compose_cursor(struct dv_session *s, int *cx, int *cy, int *vis)
{
	struct dv_desktop	*d = &s->desktop;
	struct dv_window	*fw = d->focused;
	unsigned		 W = d->cols, H = d->rows, cc;
	int			 framed;

	*cx = *cy = 1;
	*vis = 0;
	if (fw == nullptr || !fw->screen.visible || fw->hidden || fw->aside)
		return;
	if (fw->screen.sb_off > 0)	/* scrolled back: freeze the cursor */
		return;
	if (s->menu_open || s->uni_open || s->quit_confirm || s->kill_confirm || s->help_open ||
	    s->input_mode == DV_IN_SPLIT ||
	    s->xfer_prompt || s->switch_open || s->copy_open)
		return;
	framed = dv_window_framed(fw);
	cc = fw->screen.cc < fw->screen.cols ? fw->screen.cc :
	    fw->screen.cols - 1;
	*cx = fw->geom.x + (framed ? 1 : 0) + (int)cc - (int)fw->ox + 1;
	*cy = fw->geom.y + (framed ? 1 : 0) + (int)fw->screen.cr -
	    (int)fw->oy + 1;
	if (*cx < 1 || *cy < 1 || (unsigned)*cx > W || (unsigned)*cy > H) {
		*cx = *cy = 1;
		return;
	}
	*vis = 1;
}

static void
color_emit(struct dv_buf *b, uint32_t col, int bg, uint8_t gamut)
{
	char	t[40];
	int	n;

	if ((col & DV_COLOR_RGB) && gamut != 0) {
		n = snprintf(t, sizeof t, ";%u:2:%u:%u:%u:%u", bg ? 48 : 38,
		    gamut, (col >> 16) & 0xff, (col >> 8) & 0xff, col & 0xff);
		if (n > 0)
			dv_buf_append(b, t, (size_t)n);
		return;
	}
	if (col == DV_COLOR_DEFAULT)
		n = snprintf(t, sizeof t, ";%u", bg ? 49 : 39);
	else if (col & DV_COLOR_RGB)
		n = snprintf(t, sizeof t, ";%u;2;%u;%u;%u", bg ? 48 : 38,
		    (col >> 16) & 0xff, (col >> 8) & 0xff, col & 0xff);
	else if (col < 8)
		n = snprintf(t, sizeof t, ";%u", (bg ? 40 : 30) + col);
	else if (col < 16)
		n = snprintf(t, sizeof t, ";%u", (bg ? 100 : 90) + (col - 8));
	else
		n = snprintf(t, sizeof t, ";%u;5;%u", bg ? 48 : 38, col & 0xff);
	if (n > 0)
		dv_buf_append(b, t, (size_t)n);
}

static int
sgr_attrs(struct dv_buf *b, const struct dv_cell *c, struct dv_cell *cur)
{
	uint8_t	attr = c->attr & (uint8_t)~DV_ATTR_WIDE;
	uint8_t	ust = (attr & DV_ATTR_UNDER) ? (c->ustyle ? c->ustyle : 1) : 0;
	uint32_t ucol = (attr & DV_ATTR_UNDER) ? c->ucolor : DV_COLOR_DEFAULT;

	if (cur->fg == c->fg && cur->bg == c->bg && cur->attr == attr &&
	    cur->ustyle == ust && cur->ucolor == ucol &&
	    cur->alpha == c->alpha && cur->fg_br == c->fg_br &&
	    cur->bg_br == c->bg_br && cur->fg_gamut == c->fg_gamut &&
	    cur->bg_gamut == c->bg_gamut && cur->fg_x == c->fg_x &&
	    cur->bg_x == c->bg_x && cur->u_x == (ucol == DV_COLOR_DEFAULT ? 0 : c->u_x))
		return (0);
	dv_buf_append(b, "\033[0", 3);
	cur->font = 0;			/* SGR 0 also resets the font selection */
	cur->depth = 0;			/* and the stereo depth */
	if (attr & DV_ATTR_BOLD)
		dv_buf_append(b, ";1", 2);
	if (attr & DV_ATTR_DIM)
		dv_buf_append(b, ";2", 2);
	if (attr & DV_ATTR_ITALIC)
		dv_buf_append(b, ";3", 2);
	if (attr & DV_ATTR_UNDER) {	/* kitty styled underline: 4:n */
		char	u[8];
		int	un = snprintf(u, sizeof u, ";4:%u", ust);

		if (un > 0)
			dv_buf_append(b, u, (size_t)un);
	}
	if (attr & DV_ATTR_BLINK)
		dv_buf_append(b, ";5", 2);
	if (attr & DV_ATTR_REV)
		dv_buf_append(b, ";7", 2);
	if (attr & DV_ATTR_STRIKE)
		dv_buf_append(b, ";9", 2);
	color_emit(b, c->fg, 0, c->fg_gamut);
	color_emit(b, c->bg, 1, c->bg_gamut);
	if (ucol != DV_COLOR_DEFAULT) {	/* kitty underline colour: 58 */
		char	u[40];
		int	un;

		if (ucol & DV_COLOR_RGB)
			un = snprintf(u, sizeof u, ";58;2;%u;%u;%u",
			    (ucol >> 16) & 0xff, (ucol >> 8) & 0xff, ucol & 0xff);
		else
			un = snprintf(u, sizeof u, ";58;5;%u", ucol & 0xff);
		if (un > 0)
			dv_buf_append(b, u, (size_t)un);
	}
	if (c->fg_br != 100) {
		char	u[16];
		int	un = snprintf(u, sizeof u, ";56:%u", c->fg_br);

		if (un > 0)
			dv_buf_append(b, u, (size_t)un);
	}
	if (c->bg_br != 100) {
		char	u[16];
		int	un = snprintf(u, sizeof u, ";57:%u", c->bg_br);

		if (un > 0)
			dv_buf_append(b, u, (size_t)un);
	}
	if (c->alpha != 100) {
		char	u[16];
		int	un = snprintf(u, sizeof u, ";70:%u", c->alpha);

		if (un > 0)
			dv_buf_append(b, u, (size_t)un);
	}
	dv_buf_append(b, "m", 1);
	cur->fg = c->fg;
	cur->bg = c->bg;
	cur->attr = attr;
	cur->ustyle = ust;
	cur->ucolor = ucol;
	cur->alpha = c->alpha;
	cur->fg_br = c->fg_br;
	cur->bg_br = c->bg_br;
	cur->fg_gamut = c->fg_gamut;
	cur->bg_gamut = c->bg_gamut;
	cur->fg_x = c->fg_x;
	cur->bg_x = c->bg_x;
	cur->u_x = ucol == DV_COLOR_DEFAULT ? 0 : c->u_x;
	return (1);
}

/*
 * The sixteen-bit form of a cell's deep colours, after the eight-bit ones
 * and in a sequence of its own, as Silkie's deep colour specification
 * asks: 38:0:<gamut>:<r>:<g>:<b>, and 48:0 and 58:0 alike.
 */
static void
deep_emit(struct dv_buf *b, const struct dv_cell *c)
{
	std::string	 seq;
	const struct {
		uint32_t	 col, x;
		unsigned	 sgr, gamut;
	} part[3] = {
		{ c->fg, c->fg_x, 38, c->fg_gamut },
		{ c->bg, c->bg_x, 48, c->bg_gamut },
		{ c->ucolor, (c->attr & DV_ATTR_UNDER) ? c->u_x : 0, 58,
		    dv_deep_gamut(c->u_x) },
	};

	for (const auto &d : part) {
		char	t[64];
		int	n;

		if (d.x == 0 || !(d.col & DV_COLOR_RGB) || d.col == DV_COLOR_DEFAULT)
			continue;
		n = snprintf(t, sizeof t, "%s%u:0:%u:%u:%u:%u",
		    seq.empty() ? "\033[" : ";", d.sgr, d.gamut,
		    dv_deep_chan(d.col, d.x, 0), dv_deep_chan(d.col, d.x, 1),
		    dv_deep_chan(d.col, d.x, 2));
		if (n > 0)
			seq.append(t, (size_t)n);
	}
	if (!seq.empty()) {
		seq += 'm';
		dv_buf_append(b, seq.data(), seq.size());
	}
}

/*
 * Where a client with --3d-focus shows the focused window's depth: nearer
 * by its pixels. The same few depths recur, so each is worked out once.
 */
static uint32_t
focus_depth(struct dv_view *v, uint32_t depth)
{
	if (v->focus_px == 0.0f)
		return (depth);
	if (v->focus_depth == nullptr)
		v->focus_depth = new std::unordered_map<uint32_t, uint32_t>;
	auto it = v->focus_depth->find(depth);
	if (it != v->focus_depth->end())
		return (it->second);
	uint32_t near = dv_depth_nearer(depth, v->focus_px);

	v->focus_depth->emplace(depth, near);
	return (near);
}

/* The cell's style, and for a host that listed font.1 or depth.1 its font
 * selection and stereo depth. */
static void
sgr_emit(struct dv_buf *b, const struct dv_cell *c, struct dv_cell *cur,
    struct dv_view *v)
{
	if (sgr_attrs(b, c, cur) && v->deep && !v->mono)
		deep_emit(b, c);
	if (v->font && cur->font != c->font) {
		dv_font_select(c->font, b);
		cur->font = c->font;
	}
	if (v->depth) {
		uint32_t	want = c->focus ? focus_depth(v, c->depth) : c->depth;

		if (cur->depth != want) {
			dv_depth_select(want, b);
			cur->depth = want;
		}
	}
}

/* Is window-screen cell (sx,sy) inside the current linear selection? */
int
dv_sel_contains(const struct dv_session *s, int sx, int sy)
{
	int	ax = s->sel_ax, ay = s->sel_ay, cx = s->sel_cx, cy = s->sel_cy;

	if (!s->sel_active && !s->sel_done)
		return (0);
	if (ay > cy || (ay == cy && ax > cx)) {	/* order the endpoints */
		int	t;
		t = ax; ax = cx; cx = t;
		t = ay; ay = cy; cy = t;
	}
	if (sy < ay || sy > cy)
		return (0);
	if (sy == ay && sx < ax)
		return (0);
	if (sy == cy && sx > cx)
		return (0);
	return (1);
}

/* Equal as this client sees them: focus moves only what a client with
 * --3d-focus and depth draws. */
static int
cell_same_for(const struct dv_view *v, const struct dv_cell *a,
    const struct dv_cell *b)
{
	if (!cell_eq(a, b))
		return (0);
	return (!v->depth || v->focus_px == 0.0f || a->focus == b->focus);
}

static int
cell_eq(const struct dv_cell *a, const struct dv_cell *b)
{
	return (a->ch == b->ch && a->fg == b->fg && a->bg == b->bg &&
	    a->attr == b->attr && a->irow == b->irow && a->icol == b->icol &&
	    a->alpha == b->alpha && a->fg_br == b->fg_br && a->bg_br == b->bg_br &&
	    a->fg_gamut == b->fg_gamut && a->bg_gamut == b->bg_gamut &&
	    a->font == b->font && a->depth == b->depth &&
	    a->fg_x == b->fg_x && a->bg_x == b->bg_x && a->u_x == b->u_x &&
	    (!(a->attr & DV_ATTR_UNDER) ||
	    (a->ustyle == b->ustyle && a->ucolor == b->ucolor)));
}


static void
goto_xy(struct dv_buf *b, unsigned x, unsigned y)
{
	char	t[32];
	int	n;

	n = snprintf(t, sizeof t, "\033[%u;%uH", y + 1, x + 1);
	if (n > 0)
		dv_buf_append(b, t, (size_t)n);
}

/* ---- layered windows: real placements, cut to where the window shows ---- */

/*
 * One placement on a terminal. A placement of a window is as many of these
 * as the rectangles its window shows in: a window under another frame shows
 * in several, and one wholly covered in none, when nothing is sent.
 */
struct layer_piece {
	unsigned	 hid;		/* the terminal's image */
	unsigned	 pid;		/* its placement id there */
	int		 x, y;		/* cell in the viewport */
	unsigned	 X, Y;		/* pixels into that cell */
	int		 src;		/* the source rectangle is said */
	unsigned	 sx, sy, sw, sh;
	unsigned	 c, r;		/* cells to fit, 0 for its own size */
	int		 z;
	uint32_t	 depth;
	int		 gone;		/* the terminal dropped it with its image */
};

static bool
piece_same(const struct layer_piece &a, const struct layer_piece &b)
{
	return (a.hid == b.hid && a.x == b.x && a.y == b.y && a.X == b.X &&
	    a.Y == b.Y && a.src == b.src && a.sx == b.sx && a.sy == b.sy &&
	    a.sw == b.sw && a.sh == b.sh && a.c == b.c && a.r == b.r &&
	    a.z == b.z && a.depth == b.depth);
}

/* What a client shows of the layered windows: by placement, in order. */
struct dv_layers {
	std::map<unsigned, std::vector<layer_piece>> shown;
	std::vector<unsigned>	 spare;		/* placement ids free again */
	unsigned		 next = 1;
};

/* A frame's worth: what to show, and which images it needs first. */
struct layer_plan {
	std::map<unsigned, std::vector<layer_piece>> want;
	std::vector<std::pair<dv_window *, uint64_t>> send;
	std::vector<unsigned>	 drop;
	bool			 changed = false;
};

struct cell_rect {
	int	x0, y0, x1, y1;		/* desk cells, the far edges excluded */
};

/*
 * The cells of a window that show, as rectangles: runs of each row, joined
 * to the run above when it spans the same columns.
 */
static std::vector<cell_rect>
window_rects(const std::vector<unsigned> &own, unsigned W, unsigned id,
    int x0, int y0, int x1, int y1)
{
	std::vector<cell_rect>	 out;
	std::vector<size_t>	 above, row;

	for (int y = y0; y < y1; y++) {
		row.clear();
		for (int x = x0; x < x1; ) {
			if (own[(size_t)y * W + (size_t)x] != id) {
				x++;
				continue;
			}
			int	e = x;

			while (e < x1 && own[(size_t)y * W + (size_t)e] == id)
				e++;
			size_t	at = out.size();

			for (size_t k : above)
				if (out[k].x0 == x && out[k].x1 == e)
					at = k;
			if (at == out.size())
				out.push_back({ x, y, e, y + 1 });
			else
				out[at].y1 = y + 1;
			row.push_back(at);
			x = e;
		}
		above.swap(row);
	}
	return (out);
}

static void
session_cell_px(const struct dv_session *s, unsigned *cw, unsigned *ch)
{
	*cw = s->desktop.xpixel ? s->desktop.xpixel : DV_DEFAULT_XPIXEL;
	*ch = s->desktop.ypixel ? s->desktop.ypixel : DV_DEFAULT_YPIXEL;
}

/*
 * The pieces of one placement. One at its own size is cut by the pixel:
 * each piece starts where the rectangle or the picture does, with the source
 * rectangle that belongs there, so the pieces meet exactly. One fitted to
 * cells (c, r) goes as the program said it while it shows whole; under
 * another frame its pieces are whole cells, each with the share of the
 * source its cells cover, taking the picture to fill the cells it was given.
 */
static void
placement_pieces(const struct dv_window *w, const struct dv_kplace *p,
    unsigned iw, unsigned ih, const std::vector<cell_rect> &rects, int ix,
    int iy, int vx, int vy, unsigned cw, unsigned ch, struct layer_piece proto,
    std::vector<layer_piece> *out)
{
	int64_t		 col0 = (int64_t)ix + p->col - (int64_t)w->ox;
	int64_t		 row0 = (int64_t)iy + p->row - (int64_t)w->oy;
	uint64_t	 sw = p->sw, sh = p->sh;
	int		 sized = sw != 0 && sh != 0;

	if (sized && iw != 0 && ih != 0) {
		/* What is shown is what of the rectangle the image has. */
		if (p->sx >= iw || p->sy >= ih)
			return;
		sw = std::min<uint64_t>(sw, iw - p->sx);
		sh = std::min<uint64_t>(sh, ih - p->sy);
	}
	if (sized && p->want_c == 0 && p->want_r == 0) {
		int64_t	L = col0 * cw + p->off_x, T = row0 * ch + p->off_y;

		for (const auto &r : rects) {
			int64_t	l = std::max<int64_t>(L, (int64_t)r.x0 * cw);
			int64_t	t = std::max<int64_t>(T, (int64_t)r.y0 * ch);
			int64_t	rt = std::min<int64_t>(L + (int64_t)sw, (int64_t)r.x1 * cw);
			int64_t	bt = std::min<int64_t>(T + (int64_t)sh, (int64_t)r.y1 * ch);

			if (l >= rt || t >= bt)
				continue;
			struct layer_piece	q = proto;

			q.x = (int)(l / cw) - vx;
			q.y = (int)(t / ch) - vy;
			q.X = (unsigned)(l % cw);
			q.Y = (unsigned)(t % ch);
			q.src = 1;
			q.sx = p->sx + (unsigned)(l - L);
			q.sy = p->sy + (unsigned)(t - T);
			q.sw = (unsigned)(rt - l);
			q.sh = (unsigned)(bt - t);
			out->push_back(q);
		}
		return;
	}

	int64_t	col1 = col0 + p->cols, row1 = row0 + p->rows;

	for (const auto &r : rects) {
		int64_t	a0 = std::max<int64_t>(col0, r.x0), a1 = std::min<int64_t>(col1, r.x1);
		int64_t	b0 = std::max<int64_t>(row0, r.y0), b1 = std::min<int64_t>(row1, r.y1);

		if (a0 >= a1 || b0 >= b1)
			continue;
		struct layer_piece	q = proto;

		q.x = (int)a0 - vx;
		q.y = (int)b0 - vy;
		if (a0 == col0 && a1 == col1 && b0 == row0 && b1 == row1) {
			q.X = p->off_x;
			q.Y = p->off_y;
			q.c = p->want_c;
			q.r = p->want_r;
			q.src = sized && !(p->sx == 0 && p->sy == 0 &&
			    p->sw == iw && p->sh == ih);
			q.sx = p->sx;
			q.sy = p->sy;
			q.sw = p->sw;
			q.sh = p->sh;
			out->push_back(q);
			return;
		}
		if (!sized)
			continue;	/* nothing says which part of it this is */
		/* The picture's pixels on the page, from the footprint's corner. */
		int64_t	AW = (int64_t)p->cols * cw - p->off_x;
		int64_t	AH = (int64_t)p->rows * ch - p->off_y;
		int64_t	px0 = std::max<int64_t>((a0 - col0) * cw, p->off_x);
		int64_t	px1 = (a1 - col0) * cw;
		int64_t	py0 = std::max<int64_t>((b0 - row0) * ch, p->off_y);
		int64_t	py1 = (b1 - row0) * ch;

		if (AW <= 0 || AH <= 0 || px0 >= px1 || py0 >= py1)
			continue;
		uint64_t s0 = (uint64_t)llround((double)(px0 - p->off_x) * (double)sw / (double)AW);
		uint64_t s1 = (uint64_t)llround((double)(px1 - p->off_x) * (double)sw / (double)AW);
		uint64_t t0 = (uint64_t)llround((double)(py0 - p->off_y) * (double)sh / (double)AH);
		uint64_t t1 = (uint64_t)llround((double)(py1 - p->off_y) * (double)sh / (double)AH);

		s1 = std::min(s1, sw);
		t1 = std::min(t1, sh);
		if (s0 >= s1 || t0 >= t1)
			continue;	/* less than a pixel of it is here */
		q.X = (unsigned)(px0 - (a0 - col0) * cw);
		q.Y = (unsigned)(py0 - (b0 - row0) * ch);
		q.c = (unsigned)(a1 - a0);
		q.r = (unsigned)(b1 - b0);
		q.src = 1;
		q.sx = p->sx + (unsigned)s0;
		q.sy = p->sy + (unsigned)t0;
		q.sw = (unsigned)(s1 - s0);
		q.sh = (unsigned)(t1 - t0);
		out->push_back(q);
	}
}

/* Can this client be sent the real placements of layered windows? */
static bool
layers_capable(const struct dv_session *s, const struct dv_view *v)
{
	return (v->gfx && s->owner != nullptr);
}

/*
 * A placement as a client whose cell is not the session's is to show it,
 * and the size of the image it holds. The cells are the same ones; what
 * was in pixels of the session's cell is in pixels of its own.
 *
 * An image we can scale goes to it already scaled (pack_build: pixels
 * resampled, a vector scene through its root group), so every length scales
 * with it: the offset into the first cell, and the two edges of the source
 * rectangle each on their own, so that neighbours in a sheet of sprites, or
 * the slices a raycaster cuts a scene into, still meet. An image we cannot,
 * a surface on the GPU, is the terminal's to scale: it goes fitted to the
 * cells it covers, which is exact when it filled them and stretched when
 * not.
 */
static struct dv_kplace
placement_for_view(const struct dv_view *v, const struct dv_kplace &p,
    const struct dv_kimage &im, unsigned *iw, unsigned *ih)
{
	struct dv_kplace	q = p;

	*iw = im.wpx;
	*ih = im.hpx;
	if (!dv_view_scaled(v))
		return (q);
	q.off_x = std::min(dv_scale_len(p.off_x, v->cell_w, v->scell_w), v->cell_w - 1);
	q.off_y = std::min(dv_scale_len(p.off_y, v->cell_h, v->scell_h), v->cell_h - 1);
	if (!dv_kitty_scalable(&im)) {
		q.want_c = p.cols;
		q.want_r = p.rows;
		return (q);
	}
	*iw = dv_scale_size(im.wpx, v->cell_w, v->scell_w);
	*ih = dv_scale_size(im.hpx, v->cell_h, v->scell_h);
	if (p.sw != 0 && p.sh != 0 && *iw != 0 && *ih != 0) {
		unsigned	x0 = dv_scale_len(p.sx, v->cell_w, v->scell_w);
		unsigned	x1 = dv_scale_len((uint64_t)p.sx + p.sw, v->cell_w, v->scell_w);
		unsigned	y0 = dv_scale_len(p.sy, v->cell_h, v->scell_h);
		unsigned	y1 = dv_scale_len((uint64_t)p.sy + p.sh, v->cell_h, v->scell_h);

		/* What had pixels keeps one at least, inside the image. */
		x0 = std::min(x0, *iw - 1);
		y0 = std::min(y0, *ih - 1);
		x1 = std::min(std::max(x1, x0 + 1), *iw);
		y1 = std::min(std::max(y1, y0 + 1), *ih);
		q.sx = x0;
		q.sy = y0;
		q.sw = x1 - x0;
		q.sh = y1 - y0;
	}
	if (p.want_c != 0 || p.want_r != 0) {
		/* Rows worked out from columns would follow its cell's shape. */
		q.want_c = p.cols;
		q.want_r = p.rows;
	}
	return (q);
}

/*
 * What this client should show of the layered windows now, against what it
 * shows. `cleared` says its screen is about to be erased, images and all, so
 * nothing it holds counts.
 */
static void
layers_plan(struct dv_session *s, struct dv_view *v, unsigned W, int vx, int vy,
    unsigned vw, unsigned vh, bool cleared, struct layer_plan *plan)
{
	struct dv_window		*w;
	std::unordered_set<unsigned>	 live, sending;
	unsigned			 cw, ch;

	if (cleared) {
		if (v->layers != nullptr) {
			if (!v->layers->shown.empty())
				plan->changed = true;
			v->layers->shown.clear();
			v->layers->spare.clear();
			v->layers->next = 1;
		}
		/* Whatever the erasing leaves of its images goes too: each is
		 * sent again before it is placed. */
		if (v->limg_gen != nullptr)
			for (const auto &[hid, gen] : *v->limg_gen)
				plan->drop.push_back(hid);
	}
	session_cell_px(s, &cw, &ch);
	if (dv_view_scaled(v)) {
		cw = v->cell_w;
		ch = v->cell_h;
	}
	if (layers_capable(s, v))
		TAILQ_FOREACH(w, &s->desktop.windows, entry) {
			int	framed, ix, iy, x0, y0, x1, y1;

			if (!w->klayered || w->kimg == nullptr)
				continue;
			for (const auto &[key, im] : *w->kimg)
				if (im.hid != 0)
					live.insert(im.hid);
			/* Scrolled back, it shows its history: the pictures are
			 * of the screen it is not showing. */
			if (w->hidden || w->aside || w->screen.kpl == nullptr ||
			    w->screen.sb_off > 0)
				continue;
			framed = dv_window_framed(w);
			ix = framed ? w->geom.x + 1 : w->geom.x;
			iy = framed ? w->geom.y + 1 : w->geom.y;
			x0 = std::max(ix, vx);
			y0 = std::max(iy, vy);
			x1 = (int)std::min<int64_t>((int64_t)ix + w->geom.cols, (int64_t)vx + vw);
			y1 = (int)std::min<int64_t>((int64_t)iy + w->geom.rows, (int64_t)vy + vh);
			if (x0 >= x1 || y0 >= y1)
				continue;
			auto rects = window_rects(*s->owner, W, w->id, x0, y0, x1, y1);

			if (rects.empty())
				continue;	/* wholly covered: nothing of it goes */
			/* In the order they were made, so the terminal's ids for
			 * their images do not depend on a hash's order. */
			std::vector<unsigned>	 order;

			for (const auto &[id, p] : *w->screen.kpl)
				order.push_back(id);
			std::sort(order.begin(), order.end());
			for (unsigned id : order) {
				const struct dv_kplace	&p = w->screen.kpl->at(id);
				struct layer_piece	 proto = {};
				std::vector<layer_piece> pieces;

				if (!p.used || p.virt || p.alt != w->screen.alt_on)
					continue;
				auto found = w->kimg->find(p.img);

				if (found == w->kimg->end() || !found->second.done)
					continue;
				proto.z = p.z;
				proto.depth = !v->depth ? 0 : w == s->desktop.focused ?
				    focus_depth(v, p.depth) : p.depth;
				unsigned		 iw, ih;
				struct dv_kplace	 pv = placement_for_view(v, p,
				    found->second, &iw, &ih);

				placement_pieces(w, &pv, iw, ih, rects, ix, iy,
				    vx, vy, cw, ch, proto, &pieces);
				if (pieces.empty())
					continue;
				unsigned hid = dv_kitty_layer_id(w, p.img);

				for (auto &q : pieces)
					q.hid = hid;
				live.insert(hid);
				bool	held = false;

				if (!cleared && v->limg_gen != nullptr) {
					auto have = v->limg_gen->find(hid);

					held = have != v->limg_gen->end() &&
					    have->second == found->second.gen;
				}
				if (!held && sending.insert(hid).second)
					plan->send.push_back({ w, p.img });
				plan->want.emplace(id, std::move(pieces));
			}
		}
	if (!cleared && v->limg_gen != nullptr)
		for (const auto &[hid, gen] : *v->limg_gen)
			if (!live.contains(hid))
				plan->drop.push_back(hid);
	std::sort(plan->drop.begin(), plan->drop.end());
	if (!plan->send.empty() || !plan->drop.empty())
		plan->changed = true;
	if (!plan->changed) {
		const auto	*shown = v->layers != nullptr ? &v->layers->shown : nullptr;
		size_t		 n = shown != nullptr ? shown->size() : 0;

		if (n != plan->want.size())
			plan->changed = true;
		else
			for (const auto &[id, pieces] : plan->want) {
				auto it = shown->find(id);

				if (it == shown->end() || it->second.size() != pieces.size() ||
				    !std::equal(pieces.begin(), pieces.end(),
				    it->second.begin(), [](const auto &a, const auto &b) {
					    return (!b.gone && piece_same(a, b));
				    })) {
					plan->changed = true;
					break;
				}
			}
	}
}

static void
layer_delete(const struct layer_piece &q, struct dv_buf *b)
{
	char	t[80];
	int	n = snprintf(t, sizeof t, "\033_Ga=d,d=i,i=%u,p=%u,q=2\033\\", q.hid, q.pid);

	if (n > 0)
		dv_buf_append(b, t, (size_t)n);
}

static void
layer_place(const struct dv_view *v, const struct layer_piece &q, struct dv_buf *b)
{
	std::string	 cmd = "\033_Ga=p,i=" + std::to_string(q.hid) + ",p=" +
			    std::to_string(q.pid);

	if (q.src)
		cmd += ",x=" + std::to_string(q.sx) + ",y=" + std::to_string(q.sy) +
		    ",w=" + std::to_string(q.sw) + ",h=" + std::to_string(q.sh);
	if (q.X != 0)
		cmd += ",X=" + std::to_string(q.X);
	if (q.Y != 0)
		cmd += ",Y=" + std::to_string(q.Y);
	if (q.c != 0)
		cmd += ",c=" + std::to_string(q.c);
	if (q.r != 0)
		cmd += ",r=" + std::to_string(q.r);
	if (q.z != 0)
		cmd += ",z=" + std::to_string(q.z);
	cmd += ",C=1,q=2\033\\";
	goto_xy(b, (unsigned)q.x, (unsigned)q.y);
	/* A placement takes the terminal's depth when it is made. */
	if (v->depth)
		dv_depth_select(q.depth, b);
	dv_buf_append(b, cmd.data(), cmd.size());
	if (v->depth && q.depth != 0)
		dv_depth_select(0, b);
}

/*
 * The plan, as commands: images that went, images needed, placements that
 * went, then each placement that is new or not as it was. Made again under
 * the id it had, a placement moves where it is without a flicker.
 */
static void
layers_emit(struct dv_view *v, struct layer_plan *plan, struct dv_buf *b)
{
	std::unordered_set<unsigned>	 lost;

	if (v->layers == nullptr) {
		if (plan->want.empty() && plan->send.empty() && plan->drop.empty())
			return;
		v->layers = new struct dv_layers;
	}
	struct dv_layers	*L = v->layers;

	for (unsigned hid : plan->drop) {
		dv_kitty_delete(hid, b);	/* its placements go with it */
		if (v->limg_gen != nullptr)
			v->limg_gen->erase(hid);
		lost.insert(hid);
	}
	for (const auto &[w, img] : plan->send) {
		unsigned	hid = dv_kitty_layer_id(w, img);

		/* Sent again, an image loses the placements it had. */
		if (dv_kitty_layer_send(w, img, b, v))
			lost.insert(hid);
		else
			std::erase_if(plan->want, [hid](const auto &entry) {
				return (entry.second.front().hid == hid);
			});
	}
	for (auto &[id, pieces] : L->shown)
		for (auto &q : pieces)
			if (lost.contains(q.hid))
				q.gone = 1;
	for (auto it = L->shown.begin(); it != L->shown.end(); ) {
		auto	 want = plan->want.find(it->first);
		size_t	 keep = want != plan->want.end() ? want->second.size() : 0;

		for (size_t k = 0; k < it->second.size(); k++) {
			const auto	&q = it->second[k];

			if (k < keep && q.hid == want->second[k].hid)
				continue;
			if (!q.gone)
				layer_delete(q, b);
			if (k >= keep)
				L->spare.push_back(q.pid);
		}
		if (keep == 0)
			it = L->shown.erase(it);
		else {
			if (it->second.size() > keep)
				it->second.resize(keep);
			++it;
		}
	}
	for (auto &[id, pieces] : plan->want) {
		auto	&had = L->shown[id];

		for (size_t k = 0; k < pieces.size(); k++) {
			auto	&q = pieces[k];

			if (k < had.size()) {
				q.pid = had[k].pid;
				if (!had[k].gone && piece_same(q, had[k]))
					continue;
			} else if (!L->spare.empty()) {
				q.pid = L->spare.back();
				L->spare.pop_back();
			} else
				q.pid = L->next++;
			layer_place(v, q, b);
		}
		had = std::move(pieces);
	}
}

void
dv_view_free(struct dv_view *v)
{
	free(v->xcur_last);
	v->xcur_last = nullptr;
	free(v->last);
	delete v->kimg_gen;
	delete v->kres_gen;
	delete v->vfont_sent;
	delete v->focus_depth;
	delete v->kimg_depth;
	delete v->limg_gen;
	delete v->layers;
	memset(v, 0, sizeof *v);
}

/* Collect the visible host IDs once, regardless of how many exist. */
static std::unordered_set<unsigned>
visible_images(const struct dv_cell *desk, unsigned W, int vx, int vy,
    unsigned vw, unsigned vh)
{
	std::unordered_set<unsigned> present;

	for (unsigned y = 0; y < vh; y++) {
		const struct dv_cell *row = desk + (size_t)(vy + y) * W + vx;
		for (unsigned x = 0; x < vw; x++)
			if (row[x].ch == DV_KITTY_PLACEHOLDER)
				present.insert(row[x].fg);
	}
	return (present);
}

/*
 * Before a frame, make sure every placement whose placeholder cells
 * appear in this client's viewport is current, and drop those no longer
 * on screen. Kitty clients get pixels plus virtual placements; sixel
 * clients use the same generation cache to decide when to repaint.
 * This runs before the cell diff: replacing pixels need not change cells.
 */
static bool
graphics_prelude(struct dv_session *s, struct dv_view *v,
    const struct dv_cell *desk, unsigned W, int vx, int vy,
    unsigned vw, unsigned vh, struct dv_buf *b)
{
	auto present = visible_images(desk, W, vx, vy, vw, vh);
	bool changed = false;
	size_t before = b->len;
	if (v->kimg_gen == nullptr)
		v->kimg_gen = new std::unordered_map<unsigned, unsigned>;

	/* What the scenes refer to goes first: session fonts, then images. */
	dv_kitty_sync(s, v, b);
	if (b->len != before)
		changed = true;

	/* Drop cached images that are no longer on this client's screen. */
	std::erase_if(*v->kimg_gen, [&present, v, b, &changed](const auto &entry) {
		if (present.contains(entry.first))
			return (false);
		if (v->gfx)
			dv_kitty_delete(entry.first, b);
		if (v->kimg_depth != nullptr)
			v->kimg_depth->erase(entry.first);
		changed = true;
		return (true);
	});

	/* Transmit placements new to this client, or changed since. */
	for (unsigned id : present) {
		struct dv_window *w = nullptr;
		struct dv_kplace *p = dv_kitty_lookup(s, id, &w);

		if (p == nullptr || !w->kimg->at(p->img).done)
			continue;
		/* The depth it shows at here: nearer in the focused window. */
		uint32_t depth = w == s->desktop.focused ?
		    focus_depth(v, p->depth) : p->depth;
		auto it = v->kimg_gen->find(id);
		if (it != v->kimg_gen->end() && it->second == p->gen) {
			if (!v->gfx || !v->depth || v->kimg_depth == nullptr)
				continue;	/* already up to date */
			auto dd = v->kimg_depth->find(id);
			if (dd == v->kimg_depth->end() || dd->second == depth)
				continue;
			/* Only its depth changed: placed again, no pixels. */
			dv_kitty_replace(w, p, b, v, depth);
			dd->second = depth;
			changed = true;
			continue;
		}
		if (v->gfx) {
			dv_kitty_transmit(w, p, b, v, depth);
			dv_silkie_ink_resent(w, v, id);
			if (v->depth) {
				if (v->kimg_depth == nullptr)
					v->kimg_depth =
					    new std::unordered_map<unsigned, uint32_t>;
				(*v->kimg_depth)[id] = depth;
			}
		}
		(*v->kimg_gen)[id] = p->gen;
		changed = true;
	}
	return (changed);
}

/* Emit one Unicode-placeholder cell: id in the foreground, row/col in
 * diacritics. Invalidates cur so the next ordinary cell re-sets SGR. */
static void
emit_placeholder(struct dv_buf *b, const struct dv_cell *c, struct dv_cell *cur,
    unsigned *last_id)
{
	unsigned	id = c->fg;

	if (*last_id != id) {
		char	t[40];
		/*
		 * Reset first (the leading 0), so the placeholder's background
		 * is the terminal default -- what shows through a transparent
		 * image. Without the reset it inherited the previous cell's
		 * background (e.g. a neighbouring window frame), which bled the
		 * frame colour into transparent pixels as windows moved over.
		 */
		int	n = snprintf(t, sizeof t, "\033[0;38;2;%u;%u;%um",
		    (id >> 16) & 0xff, (id >> 8) & 0xff, id & 0xff);

		if (n > 0)
			dv_buf_append(b, t, (size_t)n);
		*last_id = id;
	}
	utf8_put(b, DV_KITTY_PLACEHOLDER);
	utf8_put(b, dv_placeholder_diacritic(c->irow));
	utf8_put(b, dv_placeholder_diacritic(c->icol));
	if (((id >> 24) & 0xff) != 0)
		utf8_put(b, dv_placeholder_diacritic((id >> 24) & 0xff));
	memset(cur, 0xff, sizeof *cur);	/* next ordinary cell re-emits SGR */
}

/*
 * Build the bytes that take this client's screen from what it last saw
 * to the region of `desk` its viewport covers. The viewport (v->vx,
 * v->vy sized v->cols x v->rows) is set by the caller; a client smaller
 * than the desktop sees a window into it. Returns 0 when there is
 * nothing to send. Wrapped in synchronized output so the host paints
 * the update in one go.
 */
/* Nearest-neighbour scale of a source sub-rectangle into a dw x dh RGBA
 * bitmap: the placement's pixels at the window's cell resolution, which
 * is the resolution the sixel is shown at, so we never build bigger. */
static unsigned char *
scale_rgba(const unsigned char *src, unsigned iw, unsigned ih, unsigned sx,
    unsigned sy, unsigned sw, unsigned sh, unsigned dw, unsigned dh)
{
	unsigned char	*dst = (unsigned char *)malloc((size_t)dw * dh * 4);
	unsigned	 X, Y;

	if (dst == nullptr)
		return (nullptr);
	if (sw == 0 || sh == 0) {
		sx = sy = 0;
		sw = iw;
		sh = ih;
	}
	for (Y = 0; Y < dh; Y++) {
		unsigned	syy = sy + (dh > 1 ? (unsigned)((uint64_t)Y *
				    (sh - 1) / (dh - 1)) : 0);

		if (syy >= ih)
			syy = ih ? ih - 1 : 0;
		for (X = 0; X < dw; X++) {
			unsigned	sxx = sx + (dw > 1 ? (unsigned)((uint64_t)X *
					    (sw - 1) / (dw - 1)) : 0);
			const unsigned char	*sp;
			unsigned char		*dp = dst + ((size_t)Y * dw + X) * 4;

			if (sxx >= iw)
				sxx = iw ? iw - 1 : 0;
			sp = src + ((size_t)syy * iw + sxx) * 4;
			dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = sp[3];
		}
	}
	return (dst);
}

/*
 * Paint image placements as sixel, for a client whose host renders sixel
 * but not kitty graphics. Called after the text pass (which left each
 * image cell blank), and only on a full frame -- a changed image forces
 * one, so sixel never ghosts. Each placement is rasterised at the
 * window's cell-grid resolution (not the child's source size), then the
 * still-visible cells are covered with rectangles so a window on top
 * occludes it exactly as it occludes text.
 */
static void
sixel_overlay(struct dv_session *s, const struct dv_cell *desk, unsigned W,
    int vx, int vy, unsigned vw, unsigned vh, struct dv_buf *b)
{
	auto present = visible_images(desk, W, vx, vy, vw, vh);
	unsigned cw = s->desktop.xpixel ? s->desktop.xpixel : DV_DEFAULT_XPIXEL;
	unsigned ch = s->desktop.ypixel ? s->desktop.ypixel : DV_DEFAULT_YPIXEL;
	unsigned x, y;

	for (unsigned id : present) {
		struct dv_window	*w = nullptr;
		struct dv_kplace	*p = dv_kitty_lookup(s, id, &w);
		unsigned char		*src = nullptr, *bm;
		unsigned		 iw, ih, bmW, bmH, cols, rows, r, c0;
		unsigned char		*grid;
		int			 have_org = 0, ox = 0, oy = 0;

		if (p == nullptr || !w->kimg->at(p->img).done)
			continue;
		cols = p->cols;
		rows = p->rows;
		if (cols == 0 || rows == 0)
			continue;
		/* As large as the placement is: only the arithmetic bounds it. */
		if ((uint64_t)cols * cw > UINT_MAX || (uint64_t)rows * ch > UINT_MAX)
			continue;
		bmW = cols * cw;
		bmH = rows * ch;
		if (bmW == 0 || bmH == 0 ||
		    (uint64_t)bmW * bmH > (uint64_t)SIZE_MAX / 4)
			continue;
		if (!dv_kimage_rgba(&w->kimg->at(p->img), &src, &iw, &ih))
			continue;
		bm = scale_rgba(src, iw, ih, p->sx, p->sy, p->sw, p->sh, bmW, bmH);
		free(src);
		if (bm == nullptr)
			continue;
		grid = (unsigned char *)calloc((size_t)cols * rows, 1);
		if (grid == nullptr) {
			free(bm);
			continue;
		}
		for (y = 0; y < vh; y++)	/* which cells are still visible */
			for (x = 0; x < vw; x++) {
				const struct dv_cell	*c =
				    &desk[(size_t)(vy + y) * W + vx + x];

				if (c->ch != DV_KITTY_PLACEHOLDER || c->fg != id)
					continue;
				if (c->irow < rows && c->icol < cols)
					grid[(unsigned)c->irow * cols + c->icol] = 1;
				ox = (int)(vx + x) - (int)c->icol;
				oy = (int)(vy + y) - (int)c->irow;
				have_org = 1;
			}
		if (!have_org) {
			free(grid);
			free(bm);
			continue;
		}
		for (r = 0; r < rows; r++)
			for (c0 = 0; c0 < cols; c0++) {
				unsigned	c1, r1, cc;
				int		gx, gy;

				if (grid[r * cols + c0] != 1)
					continue;
				for (c1 = c0; c1 + 1 < cols &&
				    grid[r * cols + c1 + 1] == 1; c1++)
					;
				for (r1 = r; r1 + 1 < rows; r1++) {
					int	full_row = 1;

					for (cc = c0; cc <= c1; cc++)
						if (grid[(r1 + 1) * cols + cc] != 1) {
							full_row = 0;
							break;
						}
					if (!full_row)
						break;
				}
				for (r1 = r; r1 < rows; r1++) {	/* recompute + mark */
					int	full_row = 1;

					for (cc = c0; cc <= c1; cc++)
						if (grid[r1 * cols + cc] != 1) {
							full_row = 0;
							break;
						}
					if (!full_row)
						break;
					for (cc = c0; cc <= c1; cc++)
						grid[r1 * cols + cc] = 2;
				}
				r1--;
				gx = ox - vx + (int)c0;
				gy = oy - vy + (int)r;
				if (gx < 0 || gy < 0 || (unsigned)gx >= vw ||
				    (unsigned)gy >= vh)
					continue;
				goto_xy(b, (unsigned)gx, (unsigned)gy);
				dv_sixel_encode(bm, bmW, bmH, c0 * cw, r * ch,
				    (c1 - c0 + 1) * cw, (r1 - r + 1) * ch, b);
			}
		free(grid);
		free(bm);
	}
}

/*
 * The extra cursors visible in a viewport, as the commands a host that
 * draws them needs: one per shape, listing cells 1-based in the viewport.
 * Empty when there are none. Malloc'd.
 */
static char *
xcur_set_string(const struct dv_session *s, unsigned W, int vx, int vy,
    unsigned vw, unsigned vh)
{
	struct dv_buf	xb;
	int		shapes[4] = { 1, 2, 3, 29 }, si;
	char		*r;

	memset(&xb, 0, sizeof xb);
	if (s->xcur_any && s->xcur_map != nullptr) {
		for (si = 0; si < 4; si++) {
			unsigned	x, y;
			int		any = 0;
			char		t[48];

			for (y = 0; y < vh; y++)
				for (x = 0; x < vw; x++) {
					size_t	at = (size_t)(vy + (int)y) * W +
					    (size_t)(vx + (int)x);

					if (at >= s->xcur_map_n || s->xcur_map[at] != shapes[si])
						continue;
					if (!any) {
						snprintf(t, sizeof t, "\033[>%d;2", shapes[si]);
						dv_buf_append(&xb, t, strlen(t));
						any = 1;
					}
					snprintf(t, sizeof t, ":%u:%u", y + 1, x + 1);
					dv_buf_append(&xb, t, strlen(t));
				}
			if (any)
				dv_buf_append(&xb, " q", 2);
		}
	}
	dv_buf_append(&xb, "", 1);
	r = xstrdup((const char *)xb.data);
	dv_buf_free(&xb);
	return (r);
}

/*
 * Strict black and white, for an e-paper terminal: every cell becomes ink
 * on paper (the host's default colours), or paper on ink (reverse video)
 * where the cell had a coloured background -- a highlight, a title bar --
 * or asked for reverse video itself. No shade survives: dim and blink go,
 * underline colours go; bold, italic, underline and strikethrough stay.
 * Cells carrying an image or text-sizing id in their colours are left as
 * they are, since those are not colours.
 */
static unsigned
mono_lum(uint32_t col)
{
	uint32_t	rgb = (col & DV_COLOR_RGB) ? col : dv_index_rgb(col & 0xff);

	return (((rgb >> 16) & 0xff) * 299 + ((rgb >> 8) & 0xff) * 587 +
	    (rgb & 0xff) * 114) / 1000;
}

static void
mono_cell(struct dv_cell *c)
{
	int	inked = (c->bg != DV_COLOR_DEFAULT && mono_lum(c->bg) >= 40);
	int	rev = (c->attr & DV_ATTR_REV) != 0;

	if (c->ch == DV_KITTY_PLACEHOLDER || c->ch == DV_STEXT_MARK)
		return;
	/*
	 * Shaded areas are paper: the desktop backdrop (its glyph, grey on
	 * black), and a shade block a program drew dim on a dark ground. On
	 * e-paper a stipple is noise, not a texture.
	 */
	if ((c->ch == dv_backdrop_glyph && c->fg == 8 && c->bg == 0 && c->attr == 0) ||
	    ((c->ch == 0x2591 || c->ch == 0x2592 || c->ch == 0x2593) && !inked &&
	    (c->fg == DV_COLOR_DEFAULT || mono_lum(c->fg) < 128))) {
		c->ch = U' ';
		c->fg = DV_COLOR_DEFAULT;
		c->bg = DV_COLOR_DEFAULT;
		c->ucolor = DV_COLOR_DEFAULT;
		c->fg_x = c->bg_x = c->u_x = 0;
		c->attr = 0;
		return;
	}
	c->fg = DV_COLOR_DEFAULT;
	c->bg = DV_COLOR_DEFAULT;
	c->ucolor = DV_COLOR_DEFAULT;
	c->fg_x = c->bg_x = c->u_x = 0;
	c->attr &= (uint8_t)~(DV_ATTR_DIM | DV_ATTR_BLINK | DV_ATTR_REV);
	if (inked != rev)
		c->attr |= DV_ATTR_REV;
}

/*
 * A cell for a terminal that knows no gamut but sRGB: a Display P3 or
 * Rec. 2020 colour becomes the nearest sRGB one. Such a terminal skips the
 * id and reads the numbers as sRGB, which is a little off; this is not.
 */
static void
srgb_colour(uint32_t *col, uint8_t *gamut)
{
	unsigned char	rgb[3];

	if (*gamut == 0)
		return;
	if (*col != DV_COLOR_DEFAULT && (*col & DV_COLOR_RGB)) {
		rgb[0] = (unsigned char)(*col >> 16);
		rgb[1] = (unsigned char)(*col >> 8);
		rgb[2] = (unsigned char)*col;
		dv_gamut_to_srgb(*gamut, rgb);
		*col = (*col & ~0xffffffu) | ((uint32_t)rgb[0] << 16) |
		    ((uint32_t)rgb[1] << 8) | rgb[2];
	}
	*gamut = 0;
}

static void
srgb_cell(struct dv_cell *c)
{
	if (c->ch == DV_KITTY_PLACEHOLDER || c->ch == DV_STEXT_MARK)
		return;			/* their colours are ids, not colours */
	/* What was refined is no longer the colour sent: the eight bits are. */
	if (c->fg_gamut != 0)
		c->fg_x = 0;
	if (c->bg_gamut != 0)
		c->bg_x = 0;
	if (dv_deep_gamut(c->u_x) != 0)
		c->u_x = 0;
	srgb_colour(&c->fg, &c->fg_gamut);
	srgb_colour(&c->bg, &c->bg_gamut);
}

/* ---- pictures as cells ----
 *
 * A terminal with neither kitty graphics nor sixel -- most of them -- still
 * has cells of any colour, and a cell drawn as an upper half block has two:
 * its foreground above and its background below. That is a picture of one
 * pixel by two to the cell. Coarse, but a program that draws its screen is
 * usable through it where it could not run at all.
 *
 * A layered window is first flattened: its placements drawn into a canvas
 * of the window's own pixels, each at its offset and source rectangle, in
 * the order of their z, blended as a terminal would blend them. Two
 * canvases, because text lies between: the pictures under it and the
 * pictures over it. Each cell then takes the mean of its two halves. A cell
 * with text keeps its text, over the colour of the picture beneath it.
 *
 * Placeholder cells -- a window that is not layered, or a placement the
 * program positions itself -- name one picture each, and take the means of
 * the part of it that is theirs.
 */
struct flat_image {
	unsigned			gen = 0, w = 0, h = 0;
	uint64_t			pass = 0;
	std::vector<unsigned char>	px;
};

/* Pictures decoded for one frame are wanted for the next: kept here by the
 * id the terminal would know them by, until a frame goes by without them. */
struct flat_window {
	unsigned			w = 0, h = 0;
	uint64_t			pass = 0;
	std::vector<unsigned char>	px;	/* its pictures, over and under as one */
};

struct dv_flat_cache {
	std::unordered_map<unsigned, flat_image>	img;
	/* Layered windows flattened for a sixel terminal, by window id: made
	 * when the frame's cells are, drawn from when its sixels are. */
	std::unordered_map<unsigned, flat_window>	win;
	uint64_t					pass = 0;
};

void
dv_flat_free(struct dv_session *s)
{
	delete s->flat;
	s->flat = nullptr;
}

static const flat_image *
flat_pixels(struct dv_session *s, unsigned key, unsigned gen,
    const struct dv_kimage *im)
{
	if (s->flat == nullptr)
		s->flat = new struct dv_flat_cache;
	auto	&e = s->flat->img[key];

	e.pass = s->flat->pass;
	if (e.px.empty() || e.gen != gen) {
		unsigned char	*px = nullptr;
		unsigned	 w = 0, h = 0;

		e.px.clear();
		e.gen = gen;
		e.w = e.h = 0;
		if (dv_kimage_rgba(im, &px, &w, &h)) {
			e.px.assign(px, px + (size_t)w * h * 4);
			e.w = w;
			e.h = h;
			free(px);
		}
	}
	return (e.px.empty() ? nullptr : &e);
}

/* The mean of a rectangle of straight RGBA pixels: its colour, and how much
 * of the rectangle is covered. What lies outside the picture is clear. */
struct rgba_mean {
	float	r = 0, g = 0, b = 0, a = 0;
};

static rgba_mean
area_mean(const unsigned char *px, unsigned W, unsigned H, int64_t x0,
    int64_t y0, int64_t x1, int64_t y1)
{
	rgba_mean	m;
	double		sum[4] = { 0, 0, 0, 0 };
	int64_t		n = (x1 - x0) * (y1 - y0);

	if (x1 <= x0 || y1 <= y0)
		return (m);
	x0 = std::max<int64_t>(x0, 0);
	y0 = std::max<int64_t>(y0, 0);
	x1 = std::min<int64_t>(x1, W);
	y1 = std::min<int64_t>(y1, H);
	for (int64_t y = y0; y < y1; y++) {
		const unsigned char	*p = px + ((size_t)y * W + (size_t)x0) * 4;

		for (int64_t x = x0; x < x1; x++, p += 4) {
			sum[0] += (double)p[0] * p[3];
			sum[1] += (double)p[1] * p[3];
			sum[2] += (double)p[2] * p[3];
			sum[3] += p[3];
		}
	}
	if (sum[3] > 0) {
		m.r = (float)(sum[0] / sum[3]);
		m.g = (float)(sum[1] / sum[3]);
		m.b = (float)(sum[2] / sum[3]);
		m.a = (float)(sum[3] / (255.0 * (double)n));
	}
	return (m);
}

/* One over another. */
static rgba_mean
mean_over(const rgba_mean &top, const rgba_mean &under)
{
	rgba_mean	m;
	float		ua = under.a * (1 - top.a);

	m.a = top.a + ua;
	if (m.a > 0) {
		m.r = (top.r * top.a + under.r * ua) / m.a;
		m.g = (top.g * top.a + under.g * ua) / m.a;
		m.b = (top.b * top.a + under.b * ua) / m.a;
	}
	return (m);
}

static uint32_t
mean_colour(const rgba_mean &m)
{
	auto	ch = [](float v) {
		return ((uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f));
	};

	return (DV_RGB(ch(m.r), ch(m.g), ch(m.b)));
}

/* A cell as the two halves of a picture. A half the picture does not reach
 * shows what the cell had behind it. */
static void
half_cell(struct dv_cell *c, const rgba_mean &top, const rgba_mean &bot)
{
	bool	t = top.a >= 0.5f, b = bot.a >= 0.5f;
	uint32_t behind = c->ch == DV_KITTY_PLACEHOLDER ? DV_COLOR_DEFAULT : c->bg;

	if (c->ch == DV_KITTY_PLACEHOLDER || t || b) {
		c->attr = 0;
		c->ustyle = 0;
		c->ucolor = DV_COLOR_DEFAULT;
		c->irow = c->icol = 0;
		c->font = 0;
		c->depth = 0;
		c->fg_gamut = c->bg_gamut = 0;
		c->fg_x = c->bg_x = c->u_x = 0;
		c->fg_br = c->bg_br = 100;
		c->alpha = 100;
	}
	if (t && b) {
		c->ch = 0x2580;			/* upper half block */
		c->fg = mean_colour(top);
		c->bg = mean_colour(bot);
	} else if (t) {
		c->ch = 0x2580;
		c->fg = mean_colour(top);
		c->bg = behind;
	} else if (b) {
		c->ch = 0x2584;			/* lower half block */
		c->fg = mean_colour(bot);
		c->bg = behind;
	} else if (c->ch == DV_KITTY_PLACEHOLDER) {
		c->ch = U' ';
		c->fg = c->bg = DV_COLOR_DEFAULT;
	}
}

/* Straight alpha, source over destination. */
static void
blend_over(unsigned char *d, const unsigned char *s)
{
	unsigned	sa = s[3], da = d[3];

	if (sa == 0)
		return;
	if (sa == 255 || da == 0) {
		memcpy(d, s, 4);
		return;
	}
	unsigned	rest = da * (255 - sa);		/* over 255 */
	unsigned	oa = sa * 255 + rest;		/* over 255 */

	for (int k = 0; k < 3; k++)
		d[k] = (unsigned char)((s[k] * sa * 255 + d[k] * rest + oa / 2) / oa);
	d[3] = (unsigned char)((oa + 127) / 255);
}

/* A layered window's pictures, drawn: `under` its text and `over` it, each
 * w by h pixels of the window's inside, or empty when it has none there. */
struct flat_canvas {
	unsigned			w = 0, h = 0;
	std::vector<unsigned char>	under, over;
};

static void
window_flatten(struct dv_session *s, struct dv_window *w, unsigned cw,
    unsigned ch, struct flat_canvas *out)
{
	std::vector<const struct dv_kplace *>	order;

	out->w = w->geom.cols * cw;
	out->h = w->geom.rows * ch;
	out->under.clear();
	out->over.clear();
	if (w->screen.kpl == nullptr || w->kimg == nullptr || w->screen.sb_off > 0 ||
	    out->w == 0 || out->h == 0)
		return;
	for (const auto &[id, p] : *w->screen.kpl)
		if (p.used && !p.virt && p.alt == w->screen.alt_on)
			order.push_back(&p);
	std::sort(order.begin(), order.end(), [](const auto *a, const auto *b) {
		return (a->z != b->z ? a->z < b->z : a->dv_id < b->dv_id);
	});
	for (const auto *p : order) {
		auto	found = w->kimg->find(p->img);

		if (found == w->kimg->end() || !found->second.done)
			continue;
		const flat_image	*im = flat_pixels(s, dv_kitty_layer_id(w, p->img),
					    found->second.gen, &found->second);

		if (im == nullptr)
			continue;
		uint64_t	sx = p->sx, sy = p->sy, sw = p->sw, sh = p->sh;

		if (sw == 0 || sh == 0) {
			sx = sy = 0;
			sw = im->w;
			sh = im->h;
		}
		if (sx >= im->w || sy >= im->h)
			continue;
		sw = std::min<uint64_t>(sw, im->w - sx);
		sh = std::min<uint64_t>(sh, im->h - sy);
		int64_t	X0 = ((int64_t)p->col - (int64_t)w->ox) * cw + p->off_x;
		int64_t	Y0 = ((int64_t)p->row - (int64_t)w->oy) * ch + p->off_y;
		int64_t	dw = (int64_t)sw, dh = (int64_t)sh;

		if (p->want_c != 0 || p->want_r != 0) {
			/* Fitted to its cells, from its offset to their end. */
			dw = (int64_t)p->cols * cw - p->off_x;
			dh = (int64_t)p->rows * ch - p->off_y;
		}
		if (dw <= 0 || dh <= 0)
			continue;
		auto	&canvas = p->z < 0 ? out->under : out->over;

		if (canvas.empty())
			canvas.assign((size_t)out->w * out->h * 4, 0);
		int64_t	x0 = std::max<int64_t>(X0, 0), x1 = std::min<int64_t>(X0 + dw, out->w);
		int64_t	y0 = std::max<int64_t>(Y0, 0), y1 = std::min<int64_t>(Y0 + dh, out->h);

		for (int64_t y = y0; y < y1; y++) {
			uint64_t	 syy = sy + (uint64_t)((y - Y0) * (int64_t)sh / dh);
			unsigned char	*d = &canvas[((size_t)y * out->w + (size_t)x0) * 4];

			for (int64_t x = x0; x < x1; x++, d += 4) {
				uint64_t	sxx = sx + (uint64_t)((x - X0) * (int64_t)sw / dw);

				blend_over(d, &im->px[((size_t)syy * im->w + sxx) * 4]);
			}
		}
	}
}

static rgba_mean
canvas_mean(const std::vector<unsigned char> &c, const struct flat_canvas &f,
    int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
	if (c.empty())
		return (rgba_mean());
	return (area_mean(c.data(), f.w, f.h, x0, y0, x1, y1));
}

/* Every picture in these cells, as cells. */
static void
mosaic_cells(struct dv_session *s, struct dv_cell *desk, unsigned W, unsigned H)
{
	struct dv_window	*w;
	unsigned		 cw, ch;

	session_cell_px(s, &cw, &ch);
	if (s->flat == nullptr)
		s->flat = new struct dv_flat_cache;
	s->flat->pass++;
	if (s->owner != nullptr)
		TAILQ_FOREACH(w, &s->desktop.windows, entry) {
			struct flat_canvas	f;
			int			framed, ix, iy;

			if (!w->klayered || w->hidden || w->aside)
				continue;
			window_flatten(s, w, cw, ch, &f);
			if (f.under.empty() && f.over.empty())
				continue;
			framed = dv_window_framed(w);
			ix = framed ? w->geom.x + 1 : w->geom.x;
			iy = framed ? w->geom.y + 1 : w->geom.y;
			for (unsigned r = 0; r < w->geom.rows; r++)
				for (unsigned c = 0; c < w->geom.cols; c++) {
					int64_t	dx = (int64_t)ix + c, dy = (int64_t)iy + r;

					if (dx < 0 || dy < 0 || dx >= W || dy >= H ||
					    (*s->owner)[(size_t)dy * W + (size_t)dx] != w->id)
						continue;
					struct dv_cell	*cell = &desk[(size_t)dy * W + (size_t)dx];
					int64_t		 px0 = (int64_t)c * cw, py0 = (int64_t)r * ch;
					int64_t		 mid = py0 + ch / 2;
					rgba_mean	 ot = canvas_mean(f.over, f, px0, py0, px0 + cw, mid);
					rgba_mean	 ob = canvas_mean(f.over, f, px0, mid, px0 + cw, py0 + ch);
					rgba_mean	 ut = canvas_mean(f.under, f, px0, py0, px0 + cw, mid);
					rgba_mean	 ub = canvas_mean(f.under, f, px0, mid, px0 + cw, py0 + ch);
					bool		 text = (cell->ch != U' ' && cell->ch != 0) ||
					    (dx > 0 && (desk[(size_t)dy * W + (size_t)dx - 1].attr & DV_ATTR_WIDE));

					if (ot.a >= 0.25f || ob.a >= 0.25f)
						half_cell(cell, mean_over(ot, ut), mean_over(ob, ub));
					else if (!text)
						half_cell(cell, ut, ub);
					else if (cell->bg == DV_COLOR_DEFAULT) {
						/* Its text, on the picture under it. */
						rgba_mean	u = canvas_mean(f.under, f, px0, py0,
								    px0 + cw, py0 + ch);

						if (u.a >= 0.5f)
							cell->bg = mean_colour(u);
					}
				}
		}
	for (unsigned y = 0; y < H; y++)
		for (unsigned x = 0; x < W; x++) {
			struct dv_cell		*cell = &desk[(size_t)y * W + x];
			struct dv_window	*pw = nullptr;
			struct dv_kplace	*p;

			if (cell->ch != DV_KITTY_PLACEHOLDER)
				continue;
			p = dv_kitty_lookup(s, cell->fg, &pw);
			const flat_image	*im = nullptr;

			if (p != nullptr && pw->kimg != nullptr && pw->kimg->contains(p->img) &&
			    pw->kimg->at(p->img).done && p->cols != 0 && p->rows != 0 &&
			    cell->icol < p->cols && cell->irow < p->rows)
				im = flat_pixels(s, p->dv_id, p->gen, &pw->kimg->at(p->img));
			if (im == nullptr) {
				half_cell(cell, rgba_mean(), rgba_mean());
				continue;
			}
			uint64_t	sx = p->sx, sy = p->sy, sw = p->sw, sh = p->sh;

			if (sw == 0 || sh == 0) {
				sx = sy = 0;
				sw = im->w;
				sh = im->h;
			}
			/* Its share of the source: a cell across, half a cell down. */
			int64_t	x0 = (int64_t)(sx + sw * cell->icol / p->cols);
			int64_t	x1 = (int64_t)(sx + sw * (cell->icol + 1) / p->cols);
			int64_t	y0 = (int64_t)(sy + sh * (2 * cell->irow) / (2 * p->rows));
			int64_t	ym = (int64_t)(sy + sh * (2 * cell->irow + 1) / (2 * p->rows));
			int64_t	y1 = (int64_t)(sy + sh * (2 * cell->irow + 2) / (2 * p->rows));

			x1 = std::max(x1, x0 + 1);
			ym = std::max(ym, y0 + 1);
			y1 = std::max(y1, ym + 1);
			half_cell(cell, area_mean(im->px.data(), im->w, im->h, x0, y0, x1, ym),
			    area_mean(im->px.data(), im->w, im->h, x0, ym, x1, y1));
		}
	std::erase_if(s->flat->img, [s](const auto &e) {
		return (e.second.pass != s->flat->pass);
	});
}

/*
 * The same flattening for a terminal that draws sixel. A layered window is
 * more than a picture to the cell can say -- sprites part way into cells,
 * one over another, slices a pixel wide -- so its pictures are drawn into
 * one canvas and that canvas is what the sixels are made from. Its cells in
 * the frame are marked as showing it (DV_FLAT_FIRST), each with a hash of
 * its pixels where a placeholder has a row and column, so that a frame in
 * which any of them changed is one in which the cells changed, and the
 * terminal is repainted as it is for any picture that moved.
 *
 * Sixel is drawn over the cell, so a cell with text keeps its text and
 * shows none of a picture that lies under it; a picture over the text
 * takes the cell.
 */
static void
flat_sixel_cells(struct dv_session *s, struct dv_cell *desk, unsigned W, unsigned H)
{
	struct dv_window	*w;
	unsigned		 cw, ch;

	session_cell_px(s, &cw, &ch);
	if (s->flat == nullptr)
		s->flat = new struct dv_flat_cache;
	s->flat->pass++;
	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		struct flat_canvas	f;
		int			framed, ix, iy;

		if (!w->klayered || w->hidden || w->aside || s->owner == nullptr)
			continue;
		window_flatten(s, w, cw, ch, &f);
		if (f.under.empty() && f.over.empty())
			continue;
		auto	&fw = s->flat->win[w->id];

		fw.w = f.w;
		fw.h = f.h;
		fw.pass = s->flat->pass;
		fw.px = f.under.empty() ? std::vector<unsigned char>((size_t)f.w * f.h * 4, 0) :
		    f.under;
		if (!f.over.empty())
			for (size_t i = 0; i < (size_t)f.w * f.h; i++)
				blend_over(&fw.px[i * 4], &f.over[i * 4]);
		framed = dv_window_framed(w);
		ix = framed ? w->geom.x + 1 : w->geom.x;
		iy = framed ? w->geom.y + 1 : w->geom.y;
		for (unsigned r = 0; r < w->geom.rows; r++)
			for (unsigned c = 0; c < w->geom.cols; c++) {
				int64_t	dx = (int64_t)ix + c, dy = (int64_t)iy + r;

				if (dx < 0 || dy < 0 || dx >= W || dy >= H ||
				    (*s->owner)[(size_t)dy * W + (size_t)dx] != w->id)
					continue;
				struct dv_cell	*cell = &desk[(size_t)dy * W + (size_t)dx];
				int64_t		 px0 = (int64_t)c * cw, py0 = (int64_t)r * ch;
				bool		 text = (cell->ch != U' ' && cell->ch != 0) ||
				    (dx > 0 && (desk[(size_t)dy * W + (size_t)dx - 1].attr & DV_ATTR_WIDE));
				uint32_t	 hash = 2166136261u;
				bool		 any = false;

				if (text && canvas_mean(f.over, f, px0, py0, px0 + cw,
				    py0 + ch).a < 0.25f)
					continue;
				for (unsigned y = 0; y < ch; y++) {
					const unsigned char	*p = &fw.px[((size_t)(py0 + y) *
								    fw.w + (size_t)px0) * 4];

					for (unsigned x = 0; x < cw * 4; x++) {
						hash = (hash ^ p[x]) * 16777619u;
						any |= (x & 3) == 3 && p[x] >= 128;
					}
				}
				if (!any)
					continue;	/* nothing of it shows here */
				cell->ch = DV_KITTY_PLACEHOLDER;
				cell->fg = DV_FLAT_FIRST | (w->id & 0x0fffffffu);
				cell->bg = DV_COLOR_DEFAULT;
				cell->irow = (uint16_t)(hash >> 16);
				cell->icol = (uint16_t)hash;
				cell->attr = 0;
				cell->ustyle = 0;
				cell->ucolor = DV_COLOR_DEFAULT;
				cell->font = 0;
				cell->depth = 0;
			}
	}
	std::erase_if(s->flat->img, [s](const auto &e) {
		return (e.second.pass != s->flat->pass);
	});
	std::erase_if(s->flat->win, [s](const auto &e) {
		return (e.second.pass != s->flat->pass);
	});
}

/*
 * The sixels of those cells that are to be drawn (`redo`, a cell of the
 * viewport each). A run of them in a row, with the same run in the rows
 * under it, is one rectangle of the window's canvas and one sixel: each
 * sixel carries its own palette, so fewer and larger costs far less than a
 * strip to the row.
 */
static void
flat_sixel_emit(struct dv_session *s, const struct dv_cell *desk, unsigned W,
    int vx, int vy, unsigned vw, unsigned vh, const std::vector<uint8_t> &redo,
    struct dv_buf *b)
{
	struct strip {
		unsigned	x, e, y0, y1, fg;	/* cells of the viewport */
	};
	std::vector<strip>	rects;
	std::vector<size_t>	above, row;
	unsigned		cw, ch;

	if (s->flat == nullptr || s->flat->win.empty())
		return;
	session_cell_px(s, &cw, &ch);
	for (unsigned y = 0; y < vh; y++) {
		row.clear();
		for (unsigned x = 0; x < vw; ) {
			const struct dv_cell	*c = &desk[(size_t)(vy + y) * W + vx + x];
			unsigned		 e = x + 1;

			if (c->ch != DV_KITTY_PLACEHOLDER || c->fg < DV_FLAT_FIRST ||
			    !redo[(size_t)y * vw + x]) {
				x++;
				continue;
			}
			while (e < vw && redo[(size_t)y * vw + e] &&
			    desk[(size_t)(vy + y) * W + vx + e].ch == DV_KITTY_PLACEHOLDER &&
			    desk[(size_t)(vy + y) * W + vx + e].fg == c->fg)
				e++;
			size_t	at = rects.size();

			for (size_t k : above)
				if (rects[k].x == x && rects[k].e == e && rects[k].fg == c->fg)
					at = k;
			if (at == rects.size())
				rects.push_back({ x, e, y, y + 1, c->fg });
			else
				rects[at].y1 = y + 1;
			row.push_back(at);
			x = e;
		}
		above.swap(row);
	}
	for (const auto &r : rects) {
		struct dv_window	*w, *found = nullptr;

		TAILQ_FOREACH(w, &s->desktop.windows, entry)
			if ((DV_FLAT_FIRST | (w->id & 0x0fffffffu)) == r.fg)
				found = w;
		auto	it = found != nullptr ? s->flat->win.find(found->id) :
			    s->flat->win.end();

		if (it == s->flat->win.end())
			continue;
		int	framed = dv_window_framed(found);
		int64_t	cx = (int64_t)vx + r.x - (found->geom.x + (framed ? 1 : 0));
		int64_t	cy = (int64_t)vy + r.y0 - (found->geom.y + (framed ? 1 : 0));

		if (cx < 0 || cy < 0 || (uint64_t)(cx + (r.e - r.x)) * cw > it->second.w ||
		    (uint64_t)(cy + (r.y1 - r.y0)) * ch > it->second.h)
			continue;
		goto_xy(b, r.x, r.y0);
		dv_sixel_encode(it->second.px.data(), it->second.w, it->second.h,
		    (unsigned)cx * cw, (unsigned)cy * ch, (r.e - r.x) * cw,
		    (r.y1 - r.y0) * ch, b);
	}
}

static size_t compose_frame(struct dv_session *, const struct dv_cell *,
    struct dv_view *, unsigned char **);

size_t
dv_compose_frame(struct dv_session *s, const struct dv_cell *desk,
    struct dv_view *v, unsigned char **out)
{
	struct dv_cell	*mapped;
	size_t		 n, i, cells;
	/* A layered window on a terminal that draws sixel, which has no
	 * placements to layer: flattened to pixels (flat_sixel_cells). */
	bool		 stamp = s->owner != nullptr && !layers_capable(s, v) &&
			    v->sixel;
	/* Colours in a wider gamut, for a terminal that knows only sRGB. */
	bool		 narrow = false;
	/* Pictures, for a terminal that shows none: as coloured cells. */
	bool		 mosaic = false;

	cells = (size_t)s->desktop.cols * s->desktop.rows;
	if (!v->wg && !v->mono)
		for (i = 0; i < cells && !narrow; i++)
			narrow = desk[i].fg_gamut != 0 || desk[i].bg_gamut != 0;
	if (v->mosaic) {
		mosaic = s->owner != nullptr;
		for (i = 0; i < cells && !mosaic; i++)
			mosaic = desk[i].ch == DV_KITTY_PLACEHOLDER;
	}
	if (!v->mono && !stamp && !narrow && !mosaic)
		return (compose_frame(s, desk, v, out));
	mapped = dv_allocn<dv_cell>(cells);
	memcpy(mapped, desk, cells * sizeof *mapped);
	if (stamp)
		flat_sixel_cells(s, mapped, s->desktop.cols, s->desktop.rows);
	if (mosaic)
		mosaic_cells(s, mapped, s->desktop.cols, s->desktop.rows);
	if (v->mono)
		for (i = 0; i < cells; i++)
			mono_cell(&mapped[i]);
	if (narrow)
		for (i = 0; i < cells; i++)
			srgb_cell(&mapped[i]);
	n = compose_frame(s, mapped, v, out);
	free(mapped);
	return (n);
}

static size_t
compose_frame(struct dv_session *s, const struct dv_cell *desk,
    struct dv_view *v, unsigned char **out)
{
	struct dv_desktop	*d = &s->desktop;
	unsigned		 W = d->cols, H = d->rows;
	unsigned		 vw = v->cols, vh = v->rows;
	int			 vx = v->vx, vy = v->vy;
	struct dv_buf		 b;
	struct dv_cell		 cur, blank;
	unsigned		 x, y;
	int			 cx, cy, vis, full, changed = 0, xchanged = 0;
	char			*xs;
	unsigned		 px = ~0u, py = ~0u;	/* host cursor */
	bool			 gchanged = false;
	struct layer_plan	 plan;
	std::vector<uint8_t>	 redo;		/* flattened cells to draw as sixel */

	*out = nullptr;
	if (W == 0 || H == 0)
		return (0);
	/* No viewport set (a direct caller / test): show the whole desktop. */
	if (vw == 0 || vw > W)
		vw = v->cols = W;
	if (vh == 0 || vh > H)
		vh = v->rows = H;
	if (vx < 0 || vx + (int)vw > (int)W)
		vx = v->vx = 0;
	if (vy < 0 || vy + (int)vh > (int)H)
		vy = v->vy = 0;
	dv_compose_cursor(s, &cx, &cy, &vis);
	/* Desktop cursor to viewport-local, hidden if outside the window. */
	cx -= vx;
	cy -= vy;
	if (vis && (cx < 1 || cy < 1 || (unsigned)cx > vw || (unsigned)cy > vh))
		vis = 0;

	/* A host that draws extra cursors: did the visible set change? */
	xs = v->mcursors ? xcur_set_string(s, W, vx, vy, vw, vh) : nullptr;
	xchanged = xs != nullptr && (v->xcur_last == nullptr ||
	    strcmp(v->xcur_last, xs) != 0);

	/*
	 * A client that shows the focused window nearer repaints all of it when
	 * focus moves: what changed depth is anywhere on its screen.
	 */
	{
		unsigned	fid = d->focused != nullptr ? d->focused->id : 0;

		if (v->depth && v->focus_px != 0.0f && fid != v->focus_win)
			v->full = 1;
		v->focus_win = fid;
	}

	memset(&b, 0, sizeof b);
	if (v->gfx || v->sixel)
		gchanged = graphics_prelude(s, v, desk, W, vx, vy, vw, vh, &b);
	full = v->full || v->last == nullptr;
	/* Layered windows: only a terminal that holds some, or is to. */
	if (layers_capable(s, v) || v->layers != nullptr || v->limg_gen != nullptr) {
		layers_plan(s, v, W, vx, vy, vw, vh, full != 0, &plan);
		if (plan.changed)
			gchanged = true;
	}
	if (gchanged && v->sixel && !v->gfx)
		full = 1;
	if (!full) {
		for (y = 0; y < vh && !changed; y++)
			for (x = 0; x < vw; x++)
				if (!cell_same_for(v, &desk[(vy + y) * W + vx + x],
				    &v->last[y * vw + x])) {
					changed = 1;
					break;
				}
		if (!changed && !xchanged && !gchanged && cx == v->cx && cy == v->cy &&
		    vis == v->vis) {
			free(xs);
			dv_buf_free(&b);
			return (0);
		}
		/*
		 * A sixel host draws pixels, not cells, so a moved or changed
		 * image would ghost under an incremental redraw. When any
		 * image cell changed, repaint the whole viewport (which clears
		 * the screen) and re-emit the sixels.
		 */
		if (changed && v->sixel && !v->gfx) {
			for (y = 0; y < vh && !full; y++)
				for (x = 0; x < vw; x++) {
					const struct dv_cell	*c =
					    &desk[(vy + y) * W + vx + x];
					const struct dv_cell	*o =
					    &v->last[y * vw + x];

					/* Not for a flattened window's cells: what
					 * changed of those is drawn again by the
					 * strip (flat_sixel_emit), over a cell the
					 * frame has just written blank. */
					if (((c->ch == DV_KITTY_PLACEHOLDER &&
					    c->fg < DV_FLAT_FIRST) ||
					    (o->ch == DV_KITTY_PLACEHOLDER &&
					    o->fg < DV_FLAT_FIRST)) &&
					    !cell_eq(c, o)) {
						full = 1;
						break;
					}
				}
		}
	}

	/*
	 * A sixel terminal's flattened windows are drawn from a canvas kept
	 * here, so only what changed need go: the cells whose pixels are not
	 * the ones the terminal was last sent. Each is written blank by the
	 * frame below, which takes the old pixels with it, and then drawn.
	 */
	if (v->sixel && !v->gfx && s->flat != nullptr && !s->flat->win.empty()) {
		redo.assign((size_t)vw * vh, 0);
		for (y = 0; y < vh; y++)
			for (x = 0; x < vw; x++) {
				const struct dv_cell	*c = &desk[(size_t)(vy + y) * W + vx + x];

				if (c->ch != DV_KITTY_PLACEHOLDER || c->fg < DV_FLAT_FIRST)
					continue;
				redo[(size_t)y * vw + x] = full ||
				    !cell_same_for(v, c, &v->last[(size_t)y * vw + x]);
			}
	}

	memset(&cur, 0xff, sizeof cur);
	dv_buf_append(&b, "\033[?2026h\033[?25l", 14);
	if (full) {
		static const char clear[] = "\033[0m\033[H\033[2J";

		dv_buf_append(&b, clear, sizeof clear - 1);
	}

	{
	unsigned	last_id = ~0u;		/* image id in the host's fg */

	for (y = 0; y < vh; y++) {
		const struct dv_cell	*row = desk + (size_t)(vy + y) * W + vx;

		for (x = 0; x < vw; x++) {
			const struct dv_cell	*c = &row[x];
			unsigned		 width = 1;

			if ((c->attr & DV_ATTR_WIDE) && x + 1 < vw)
				width = 2;
			if (!full) {
				const struct dv_cell	*o = v->last + y * vw + x;
				int			 same = cell_same_for(v, c, o);

				if (same && width == 2)
					same = cell_same_for(v, c + 1, o + 1);
				if (same) {
					x += width - 1;
					continue;
				}
			}
			if (px != x || py != y)
				goto_xy(&b, x, y);
			if (c->ch == DV_STEXT_MARK) {
				/*
				 * OSC 66 scaled text. Only the run's origin cell
				 * emits; the rest of its rectangle is covered by
				 * the scaled glyphs, so skip them. A host that
				 * speaks kitty graphics is assumed to speak text
				 * sizing too; elsewhere the run is drawn unscaled
				 * so the text is at least present.
				 */
				struct dv_window	*sw = nullptr;
				struct dv_stext		*r;
				struct dv_cell		 col;
				int			 occ = 0;

				if (c->irow != 0 || c->icol != 0) {
					px = ~0u;
					py = ~0u;
					continue;	/* a covered continuation cell */
				}
				r = dv_stext_lookup(s, c->fg, &sw);
				if (px != x || py != y)
					goto_xy(&b, x, y);
				if (r == nullptr) {	/* run evicted: a blank */
					col = *c;
					col.ch = U' ';
					col.fg = col.bg = DV_COLOR_DEFAULT;
					col.attr = 0;
					col.font = 0;
					col.depth = 0;
					last_id = ~0u;
					sgr_emit(&b, &col, &cur, v);
					utf8_put(&b, U' ');
					px = x + 1;
					py = y;
					continue;
				}
				if (v->gfx) {		/* fully visible? */
					unsigned	rr, cc2;

					for (rr = 0; rr < r->rows && !occ; rr++)
						for (cc2 = 0; cc2 < r->cols; cc2++) {
							int	ddx = vx + (int)x + (int)cc2;
							int	ddy = vy + (int)y + (int)rr;
							const struct dv_cell *bc;

							if (ddx < 0 || ddy < 0 ||
							    (unsigned)ddx >= W ||
							    (unsigned)ddy >= H) {
								occ = 1;
								break;
							}
							bc = &desk[(unsigned)ddy * W +
							    (unsigned)ddx];
							if (bc->ch != DV_STEXT_MARK ||
							    bc->fg != c->fg) {
								occ = 1;
								break;
							}
						}
				}
				col = *c;
				col.ch = U' ';
				col.fg = r->fg;
				col.bg = r->bg;
				col.attr = r->attr;
				col.font = r->font;
				col.depth = r->depth;
				last_id = ~0u;
				sgr_emit(&b, &col, &cur, v);
				if (v->gfx && !occ) {
					std::string	hdr = "\033]66;s=" +
					    std::to_string(r->s);

					if (r->wparam > 0)
						hdr += ":w=" + std::to_string(r->wparam);
					if (r->n > 0) {
						hdr += ":n=" + std::to_string(r->n) +
						    ":d=" + std::to_string(r->d);
						if (r->v > 0)
							hdr += ":v=" + std::to_string(r->v);
						if (r->h > 0)
							hdr += ":h=" + std::to_string(r->h);
					}
					hdr += ';';
					dv_buf_append(&b, hdr.data(), hdr.size());
					dv_buf_append(&b, r->text.data(), r->text.size());
					dv_buf_append(&b, "\033\\", 2);
				} else
					dv_buf_append(&b, r->text.data(), r->text.size());
				px = ~0u;	/* the run moved the host cursor */
				py = ~0u;
				continue;
			}
			if (c->ch == DV_KITTY_PLACEHOLDER) {
				if (v->gfx) {
					emit_placeholder(&b, c, &cur, &last_id);
					px = x + 1;
					py = y;
					continue;
				}
				/* No kitty graphics on this host: the picture's
				 * cells paint blank, the rest is unchanged. */
				blank = *c;
				blank.ch = U' ';
				blank.fg = blank.bg = DV_COLOR_DEFAULT;
				blank.attr = 0;
				blank.alpha = 100;
				blank.fg_br = blank.bg_br = 100;
				blank.fg_gamut = blank.bg_gamut = 0;
				blank.fg_x = blank.bg_x = blank.u_x = 0;
				blank.font = 0;
				blank.depth = 0;
				c = &blank;
			}
			last_id = ~0u;		/* an ordinary cell resets fg */
			sgr_emit(&b, c, &cur, v);
			utf8_put(&b, c->ch ? c->ch : U' ');
			px = x + width;
			py = y;
			x += width - 1;
		}
	}
	}

	/*
	 * Leave the terminal on its plane between frames: what it writes
	 * after a detach, and the next frame's placements, start from there.
	 * (All ones means an SGR 0 already took it there.)
	 */
	if (v->depth && cur.depth != 0 && cur.depth != UINT32_MAX)
		dv_depth_select(0, &b);

	/* After the cells, and after the erasing of a full frame, which takes
	 * a terminal's placements with it; inside the one update, so the text
	 * and the pictures over and under it change together. */
	if (plan.changed)
		layers_emit(v, &plan, &b);

	if (v->sixel && !v->gfx && full)
		sixel_overlay(s, desk, W, vx, vy, vw, vh, &b);
	if (!redo.empty())
		flat_sixel_emit(s, desk, W, vx, vy, vw, vh, redo, &b);

	if (xs != nullptr) {
		/*
		 * A host that draws extra cursors gets the set visible in
		 * this viewport when it changed: none, then these.
		 */
		if (xchanged) {
			dv_buf_append(&b, "\033[>0;4 q", 8);
			dv_buf_append(&b, xs, strlen(xs));
			free(v->xcur_last);
			v->xcur_last = xs;
		} else
			free(xs);
		xs = nullptr;
	}

	{
		char	t[64];
		int	n;

		n = snprintf(t, sizeof t, "\033[%d;%dH\033[%s\033[?2026l",
		    vis ? cy : 1, vis ? cx : 1, vis ? "?25h" : "?25l");
		if (n > 0)
			dv_buf_append(&b, t, (size_t)n);
	}

	if (v->last == nullptr)
		v->last = dv_allocn<dv_cell>((size_t)vw * vh);
	for (y = 0; y < vh; y++)
		memcpy(v->last + y * vw, desk + (size_t)(vy + y) * W + vx,
		    (size_t)vw * sizeof(struct dv_cell));
	v->cx = cx;
	v->cy = cy;
	v->vis = vis;
	v->full = 0;
	*out = b.data;
	return (b.len);
}
