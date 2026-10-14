/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Overlapping compositor. Windows are z-ordered rectangles on the
 * desktop; a frame is chrome, not a tmux split. The desktop is
 * composed into cells once per change, then each client gets only the
 * cells that differ from what it saw last.
 */

#include "dv.hpp"

#include <algorithm>
#include <unordered_set>
#include <vector>

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

static int
zcmp(const void *a, const void *b)
{
	const struct dv_window	*wa = *static_cast<struct dv_window *const *>(a);
	const struct dv_window	*wb = *static_cast<struct dv_window *const *>(b);

	return (wa->z - wb->z);
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

static void
paint_window(struct dv_cell *desk, unsigned W, unsigned H, struct dv_window *w)
{
	int		 framed = w->framed && !w->zoomed;
	int		 fx = w->geom.x, fy = w->geom.y;
	unsigned	 fw = dv_frame_cols(&w->geom, framed);
	unsigned	 fh = dv_frame_rows(&w->geom, framed);
	int		 ix, iy;
	unsigned	 x, y;
	int		 focus = (w->session->desktop.focused == w);
	uint32_t	 ffg = focus ? 11 : 6;
	uint32_t	 fbg = 4;

	if (w->hidden || w->aside)
		return;

	ix = framed ? fx + 1 : fx;
	iy = framed ? fy + 1 : fy;

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
		for (ti = 0; ti < tlen && ti + 2 < fw; ti++) {
			unsigned char	tc = (unsigned char)title[ti];
			uint8_t		 a = editing ? (DV_ATTR_BOLD | DV_ATTR_REV)
					    : DV_ATTR_BOLD;

			put_desk(desk, W, H, fx + 1 + (int)ti, fy,
			    tc < 0x80 ? tc : U'?', 15, fbg, a);
		}
		if (editing && tlen + 2 < fw)	/* a block cursor at the end */
			put_desk(desk, W, H, fx + 1 + (int)tlen, fy, U' ', 15,
			    fbg, DV_ATTR_REV);
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
	 * Direct image placements float above the text, and are painted as
	 * placeholder cells clipped exactly as the text is: to the screen,
	 * to the window's viewport, and to the desktop. A window painted
	 * later (higher z) covers them cell by cell, like anything else.
	 */
	std::vector<const dv_kplace *> placements;
	if (w->screen.kpl != nullptr)
		for (const auto &[id, p] : *w->screen.kpl)
			placements.push_back(&p);
	/* Hash iteration must not change which overlapping image is on top. */
	std::sort(placements.begin(), placements.end(), [](auto a, auto b) {
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

static const char *
mode_text(int mode)
{
	switch (mode) {
	case DV_IN_REARRANGE:
		return ("REARRANGE  M move  R resize  P 1-9 snap  H hide  Esc");
	case DV_IN_MOVE:
		return ("MOVE  arrows  Shift=5  Enter done  Esc cancel");
	case DV_IN_RESIZE:
		return ("RESIZE  arrows (top-left stays)  Shift=5  Enter  Esc");
	case DV_IN_POSITION:
		return ("POSITION  1-9 snap  Esc cancel");
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
	struct dv_window	*w, *list[256];
	unsigned		 n = 0, i, W = d->cols, H = d->rows;
	const char		*st;

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
	}
	TAILQ_FOREACH(w, &d->windows, entry) {
		if (n < 256)
			list[n++] = w;
	}
	qsort(list, n, sizeof list[0], zcmp);
	if (s->xcur_map == nullptr || s->xcur_map_n != (size_t)W * H) {
		free(s->xcur_map);
		s->xcur_map = dv_allocn<uint8_t>((size_t)W * H);
		s->xcur_map_n = (size_t)W * H;
	}
	memset(s->xcur_map, 0, s->xcur_map_n);
	s->xcur_any = 0;
	for (i = 0; i < n; i++)
		paint_window(desk, W, H, list[i]);
	st = mode_text(s->input_mode);
	if (st == nullptr && s->broadcast)
		st = "BROADCAST  typing goes to every window   (menu B stops it)";
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
			int	framed = cw->framed && !cw->zoomed;
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
		    "Quit goblin-view and close every window?   y / n");
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
	if (s->menu_open || s->uni_open || s->quit_confirm || s->help_open ||
	    s->xfer_prompt || s->switch_open || s->copy_open)
		return;
	framed = fw->framed && !fw->zoomed;
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

static void
sgr_emit(struct dv_buf *b, const struct dv_cell *c, struct dv_cell *cur)
{
	uint8_t	attr = c->attr & (uint8_t)~DV_ATTR_WIDE;
	uint8_t	ust = (attr & DV_ATTR_UNDER) ? (c->ustyle ? c->ustyle : 1) : 0;
	uint32_t ucol = (attr & DV_ATTR_UNDER) ? c->ucolor : DV_COLOR_DEFAULT;

	if (cur->fg == c->fg && cur->bg == c->bg && cur->attr == attr &&
	    cur->ustyle == ust && cur->ucolor == ucol &&
	    cur->alpha == c->alpha && cur->fg_br == c->fg_br &&
	    cur->bg_br == c->bg_br && cur->fg_gamut == c->fg_gamut &&
	    cur->bg_gamut == c->bg_gamut)
		return;
	dv_buf_append(b, "\033[0", 3);
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

static int
cell_eq(const struct dv_cell *a, const struct dv_cell *b)
{
	return (a->ch == b->ch && a->fg == b->fg && a->bg == b->bg &&
	    a->attr == b->attr && a->irow == b->irow && a->icol == b->icol &&
	    a->alpha == b->alpha && a->fg_br == b->fg_br && a->bg_br == b->bg_br &&
	    a->fg_gamut == b->fg_gamut && a->bg_gamut == b->bg_gamut &&
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

void
dv_view_free(struct dv_view *v)
{
	free(v->xcur_last);
	v->xcur_last = nullptr;
	free(v->last);
	delete v->kimg_gen;
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
	if (v->kimg_gen == nullptr)
		v->kimg_gen = new std::unordered_map<unsigned, unsigned>;

	/* Drop cached images that are no longer on this client's screen. */
	std::erase_if(*v->kimg_gen, [&present, v, b, &changed](const auto &entry) {
		if (present.contains(entry.first))
			return (false);
		if (v->gfx)
			dv_kitty_delete(entry.first, b);
		changed = true;
		return (true);
	});

	/* Transmit placements new to this client, or changed since. */
	for (unsigned id : present) {
		struct dv_window *w = nullptr;
		struct dv_kplace *p = dv_kitty_lookup(s, id, &w);

		if (p == nullptr || !w->kimg->at(p->img).done)
			continue;
		auto it = v->kimg_gen->find(id);
		if (it != v->kimg_gen->end() && it->second == p->gen)
			continue;		/* already up to date */
		if (v->gfx)
			dv_kitty_transmit(w, p, b);
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
		unsigned	syy = sy + (dh > 1 ? Y * (sh - 1) / (dh - 1) : 0);

		if (syy >= ih)
			syy = ih ? ih - 1 : 0;
		for (X = 0; X < dw; X++) {
			unsigned	sxx = sx + (dw > 1 ? X * (sw - 1) / (dw - 1) : 0);
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
		if (cols == 0 || rows == 0 || (size_t)cols * rows > 100000u)
			continue;
		bmW = cols * cw;
		bmH = rows * ch;
		if (bmW == 0 || bmH == 0 || (size_t)bmW * bmH > 4000000u)
			continue;		/* keep the composed sixel bounded */
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
		c->attr = 0;
		return;
	}
	c->fg = DV_COLOR_DEFAULT;
	c->bg = DV_COLOR_DEFAULT;
	c->ucolor = DV_COLOR_DEFAULT;
	c->attr &= (uint8_t)~(DV_ATTR_DIM | DV_ATTR_BLINK | DV_ATTR_REV);
	if (inked != rev)
		c->attr |= DV_ATTR_REV;
}

static size_t compose_frame(struct dv_session *, const struct dv_cell *,
    struct dv_view *, unsigned char **);

size_t
dv_compose_frame(struct dv_session *s, const struct dv_cell *desk,
    struct dv_view *v, unsigned char **out)
{
	struct dv_cell	*mapped;
	size_t		 n, i, cells;

	if (!v->mono)
		return (compose_frame(s, desk, v, out));
	cells = (size_t)s->desktop.cols * s->desktop.rows;
	mapped = dv_allocn<dv_cell>(cells);
	memcpy(mapped, desk, cells * sizeof *mapped);
	for (i = 0; i < cells; i++)
		mono_cell(&mapped[i]);
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

	memset(&b, 0, sizeof b);
	if (v->gfx || v->sixel)
		gchanged = graphics_prelude(s, v, desk, W, vx, vy, vw, vh, &b);
	full = v->full || v->last == nullptr;
	if (gchanged && v->sixel && !v->gfx)
		full = 1;
	if (!full) {
		for (y = 0; y < vh && !changed; y++)
			for (x = 0; x < vw; x++)
				if (!cell_eq(&desk[(vy + y) * W + vx + x],
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

					if ((c->ch == DV_KITTY_PLACEHOLDER ||
					    o->ch == DV_KITTY_PLACEHOLDER) &&
					    !cell_eq(c, o)) {
						full = 1;
						break;
					}
				}
		}
	}

	memset(&cur, 0xff, sizeof cur);
	dv_buf_append(&b, "\033[?2026h\033[?25l", 14);
	if (full)
		dv_buf_append(&b, "\033[0m\033[H\033[2J", 10);

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
				int			 same = cell_eq(c, o);

				if (same && width == 2)
					same = cell_eq(c + 1, o + 1);
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
					last_id = ~0u;
					sgr_emit(&b, &col, &cur);
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
				last_id = ~0u;
				sgr_emit(&b, &col, &cur);
				if (v->gfx && !occ) {
					char	hdr[32];
					int	hn;

					if (r->wparam > 0)
						hn = snprintf(hdr, sizeof hdr,
						    "\033]66;s=%u:w=%u;", r->s,
						    r->wparam);
					else
						hn = snprintf(hdr, sizeof hdr,
						    "\033]66;s=%u;", r->s);
					if (hn > 0)
						dv_buf_append(&b, hdr, (size_t)hn);
					dv_buf_append(&b, r->text, r->len);
					dv_buf_append(&b, "\033\\", 2);
				} else
					dv_buf_append(&b, r->text, r->len);
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
				c = &blank;
			}
			last_id = ~0u;		/* an ordinary cell resets fg */
			sgr_emit(&b, c, &cur);
			utf8_put(&b, c->ch ? c->ch : U' ');
			px = x + width;
			py = y;
			x += width - 1;
		}
	}
	}

	if (v->sixel && !v->gfx && full)
		sixel_overlay(s, desk, W, vx, vy, vw, vh, &b);

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
