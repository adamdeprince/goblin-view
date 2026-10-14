/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * The Alt-U Unicode picker. A compositor overlay of glyph categories you
 * drive with the arrows and Enter or click with the mouse; the chosen
 * character is typed into the focused window. Categories are backed by
 * whole Unicode blocks (all the math, all the emoji, ...) and paged, so
 * the grid scrolls rather than the list being trimmed. Layout math lives
 * in one place so painting and hit-testing agree.
 */

#include "dv.hpp"

#include <string.h>

struct uni_range {
	uint32_t	lo;
	uint32_t	hi;
};

struct uni_cat {
	const char		*name;
	const struct uni_range	*ranges;
	int			 nr;
};

/* Whole blocks, so a category holds everything in it. Unassigned code
 * points render as the host's missing-glyph box; that is acceptable. */
static const struct uni_range r_arrows[] = {
	{ 0x2190, 0x21FF }, { 0x2B00, 0x2BFF },
};
static const struct uni_range r_math[] = {
	{ 0x2200, 0x22FF },	/* Mathematical Operators */
	{ 0x2A00, 0x2AFF },	/* Supplemental Mathematical Operators */
	{ 0x27C0, 0x27EF },	/* Misc Mathematical Symbols-A */
	{ 0x2980, 0x29FF },	/* Misc Mathematical Symbols-B */
	{ 0x2100, 0x214F },	/* Letterlike Symbols */
	{ 0x00B1, 0x00B1 }, { 0x00D7, 0x00D7 }, { 0x00F7, 0x00F7 },
};
static const struct uni_range r_punct[] = {
	{ 0x2010, 0x2027 }, { 0x2030, 0x205E }, { 0x00A1, 0x00BF },
};
static const struct uni_range r_money[] = {
	{ 0x20A0, 0x20C0 }, { 0x00A2, 0x00A5 },
};
static const struct uni_range r_box[] = {
	{ 0x2500, 0x25FF },	/* Box Drawing, Block Elements, Geometric */
};
static const struct uni_range r_greek[] = {
	{ 0x0370, 0x03FF },
};
static const struct uni_range r_latin[] = {
	{ 0x00C0, 0x024F },	/* Latin-1 letters + Latin Extended-A/B */
};
static const struct uni_range r_emoji[] = {
	{ 0x1F300, 0x1F5FF },	/* Misc Symbols and Pictographs */
	{ 0x1F600, 0x1F64F },	/* Emoticons */
	{ 0x1F680, 0x1F6FF },	/* Transport and Map */
	{ 0x1F900, 0x1F9FF },	/* Supplemental Symbols and Pictographs */
	{ 0x1FA70, 0x1FAFF },	/* Symbols and Pictographs Extended-A */
	{ 0x2600, 0x26FF },	/* Miscellaneous Symbols */
	{ 0x2700, 0x27BF },	/* Dingbats */
};
static const struct uni_range r_hiragana[] = { { 0x3041, 0x3096 } };
static const struct uni_range r_katakana[] = { { 0x30A1, 0x30FA } };
static const struct uni_range r_hangul[] = { { 0xAC00, 0xD7A3 } };
static const struct uni_range r_hanzi[] = { { 0x4E00, 0x9FFF } };

#define CAT(nm, arr) { nm, arr, (int)(sizeof arr / sizeof arr[0]) }
static const struct uni_cat cats[] = {
	CAT("Arrows", r_arrows),
	CAT("Math", r_math),
	CAT("Punct", r_punct),
	CAT("Money", r_money),
	CAT("Box", r_box),
	CAT("Greek", r_greek),
	CAT("Latin", r_latin),
	CAT("Emoji", r_emoji),
	CAT("Hiragana", r_hiragana),
	CAT("Katakana", r_katakana),
	CAT("Hangul", r_hangul),
	CAT("Hanzi", r_hanzi),
};
#define NCATS	((int)(sizeof cats / sizeof cats[0]))

int
dv_uni_cats(void)
{
	return (NCATS);
}

int
dv_uni_count(int cat)
{
	int	i, n = 0;

	if (cat < 0 || cat >= NCATS)
		return (0);
	for (i = 0; i < cats[cat].nr; i++)
		n += (int)(cats[cat].ranges[i].hi - cats[cat].ranges[i].lo + 1);
	return (n);
}

uint32_t
dv_uni_char(int cat, int sel)
{
	int	i;

	if (cat < 0 || cat >= NCATS || sel < 0)
		return (0);
	for (i = 0; i < cats[cat].nr; i++) {
		int	span = (int)(cats[cat].ranges[i].hi -
		    cats[cat].ranges[i].lo + 1);

		if (sel < span)
			return (cats[cat].ranges[i].lo + (uint32_t)sel);
		sel -= span;
	}
	return (0);
}

/* One layout, shared by paint and hit-testing. Tabs wrap within the box. */
struct layout {
	int	bx, by, bw, bh;
	int	gx, gy;		/* first glyph cell */
	int	gcols, grows;	/* glyph grid */
	int	slot;		/* cells per glyph slot */
	int	tabrows;	/* rows the wrapped tab bar occupies */
};

static int
tab_rows(int bw)
{
	int	row = 0, col = 0, i;

	for (i = 0; i < NCATS; i++) {
		int	w = (int)strlen(cats[i].name) + 2;

		if (col > 0 && col + w > bw - 2) {
			row++;
			col = 0;
		}
		col += w;
	}
	return (row + 1);
}

/* Tab i's row (0-based in the tab area), left column, and width. */
static void
tab_pos(int bw, int i, int *trow, int *tcol, int *tw)
{
	int	row = 0, col = 0, k;

	for (k = 0; k <= i; k++) {
		int	w = (int)strlen(cats[k].name) + 2;

		if (col > 0 && col + w > bw - 2) {
			row++;
			col = 0;
		}
		if (k == i) {
			*trow = row;
			*tcol = col;
			*tw = w;
			return;
		}
		col += w;
	}
}

static void
layout(unsigned cols, unsigned rows, struct layout *L)
{
	int	bw = (int)cols - 2, bh = (int)rows - 2;

	if (bw > 64)
		bw = 64;
	if (bh > 24)
		bh = 24;
	if (bw < 20)
		bw = (int)cols;
	if (bh < 10)
		bh = (int)rows;
	L->bw = bw;
	L->bh = bh;
	L->bx = ((int)cols - bw) / 2;
	L->by = ((int)rows - bh) / 2;
	L->slot = 4;
	L->tabrows = tab_rows(bw);
	L->gx = L->bx + 2;
	L->gy = L->by + 1 + L->tabrows + 1;	/* title, tabs, blank */
	L->gcols = (bw - 4) / L->slot;
	if (L->gcols < 1)
		L->gcols = 1;
	L->grows = (L->by + bh - 1) - L->gy;	/* leave a footer row */
	if (L->grows < 1)
		L->grows = 1;
}

void
dv_uni_grid(unsigned cols, unsigned rows, int *gcols, int *grows)
{
	struct layout	L;

	layout(cols, rows, &L);
	*gcols = L.gcols;
	*grows = L.grows;
}

/* Box origin and first glyph cell, for tests and hit-testing callers. */
void
dv_uni_box(unsigned cols, unsigned rows, int *bx, int *by, int *gx, int *gy)
{
	struct layout	L;

	layout(cols, rows, &L);
	*bx = L.bx;
	*by = L.by;
	*gx = L.gx;
	*gy = L.gy;
}

static void
put(struct dv_cell *desk, unsigned W, unsigned H, int x, int y,
    uint32_t ch, uint32_t fg, uint32_t bg, uint8_t attr)
{
	struct dv_cell	*c;

	if (x < 0 || y < 0 || (unsigned)x >= W || (unsigned)y >= H)
		return;
	c = &desk[(unsigned)y * W + (unsigned)x];
	c->ch = ch;
	c->fg = fg;
	c->bg = bg;
	c->attr = attr;
	c->ustyle = 0;
	c->ucolor = DV_COLOR_DEFAULT;
}

static void
puts_at(struct dv_cell *desk, unsigned W, unsigned H, int x, int y,
    const char *s, uint32_t fg, uint32_t bg, uint8_t attr)
{
	for (; *s != '\0'; s++, x++)
		put(desk, W, H, x, y, (unsigned char)*s, fg, bg, attr);
}

#define UBG	4u
#define UBORDER	15u
#define UDIM	7u
#define USEL	11u

void
dv_uni_paint(struct dv_cell *desk, unsigned W, unsigned H, int cat, int sel)
{
	struct layout	L;
	int		x, y, i, page, base, per;

	if (cat < 0 || cat >= NCATS)
		return;
	layout(W, H, &L);

	for (y = 0; y < L.bh; y++)
		for (x = 0; x < L.bw; x++)
			put(desk, W, H, L.bx + x, L.by + y, U' ', UDIM, UBG, 0);
	for (x = 0; x < L.bw; x++) {
		put(desk, W, H, L.bx + x, L.by, 0x2550, UBORDER, UBG, 0);
		put(desk, W, H, L.bx + x, L.by + L.bh - 1, 0x2550, UBORDER,
		    UBG, 0);
	}
	for (y = 0; y < L.bh; y++) {
		put(desk, W, H, L.bx, L.by + y, 0x2551, UBORDER, UBG, 0);
		put(desk, W, H, L.bx + L.bw - 1, L.by + y, 0x2551, UBORDER,
		    UBG, 0);
	}
	put(desk, W, H, L.bx, L.by, 0x2554, UBORDER, UBG, 0);
	put(desk, W, H, L.bx + L.bw - 1, L.by, 0x2557, UBORDER, UBG, 0);
	put(desk, W, H, L.bx, L.by + L.bh - 1, 0x255A, UBORDER, UBG, 0);
	put(desk, W, H, L.bx + L.bw - 1, L.by + L.bh - 1, 0x255D, UBORDER,
	    UBG, 0);
	puts_at(desk, W, H, L.bx + 2, L.by, " Unicode ", UBORDER, UBG,
	    DV_ATTR_BOLD);

	/* Category tabs, wrapped to as many rows as they need. */
	for (i = 0; i < NCATS; i++) {
		int		trow, tcol, tw, on = (i == cat);
		uint32_t	fg = on ? UBG : UDIM;
		uint32_t	bg = on ? USEL : UBG;

		tab_pos(L.bw, i, &trow, &tcol, &tw);
		put(desk, W, H, L.bx + 1 + tcol, L.by + 1 + trow, U' ', fg,
		    bg, 0);
		puts_at(desk, W, H, L.bx + 2 + tcol, L.by + 1 + trow,
		    cats[i].name, fg, bg, on ? DV_ATTR_BOLD : 0);
		put(desk, W, H, L.bx + 1 + tcol + tw - 1, L.by + 1 + trow,
		    U' ', fg, bg, 0);
	}

	/* Glyph grid, paged so the selection is visible. */
	per = L.gcols * L.grows;
	page = per > 0 ? sel / per : 0;
	base = page * per;
	for (i = 0; i < per; i++) {
		int		idx = base + i;
		int		gxp = L.gx + (i % L.gcols) * L.slot;
		int		gyp = L.gy + (i / L.gcols);
		int		on = (idx == sel);
		uint32_t	fg = on ? UBG : UDIM;
		uint32_t	bg = on ? USEL : UBG;
		uint32_t	ch;
		int		k;

		if (idx >= dv_uni_count(cat))
			break;
		ch = dv_uni_char(cat, idx);
		for (k = 0; k < L.slot; k++)
			put(desk, W, H, gxp + k, gyp, U' ', fg, bg, 0);
		put(desk, W, H, gxp + 1, gyp, ch, fg, bg, on ? DV_ATTR_BOLD : 0);
	}

	puts_at(desk, W, H, L.bx + 2, L.by + L.bh - 1,
	    " arrows move  Enter type  Tab category  .,=page  Esc ",
	    UBG, UBORDER, DV_ATTR_BOLD);
}

/*
 * Hit test a click at desktop cell (mx,my), 0-based. Returns 0 for a tab
 * (in *newcat), 1 for a glyph (in *newsel), -1 for outside the box.
 */
int
dv_uni_click(unsigned cols, unsigned rows, int cat, int cursel, int mx,
    int my, int *newcat, int *newsel)
{
	struct layout	L;
	int		i, per, base;

	if (cat < 0 || cat >= NCATS)
		return (-1);
	layout(cols, rows, &L);
	if (mx < L.bx || mx >= L.bx + L.bw || my < L.by || my >= L.by + L.bh)
		return (-1);

	if (my >= L.by + 1 && my < L.by + 1 + L.tabrows) {	/* a tab row */
		for (i = 0; i < NCATS; i++) {
			int	trow, tcol, tw;

			tab_pos(L.bw, i, &trow, &tcol, &tw);
			if (my == L.by + 1 + trow &&
			    mx >= L.bx + 1 + tcol && mx < L.bx + 1 + tcol + tw) {
				*newcat = i;
				return (0);
			}
		}
		return (-1);
	}

	if (my >= L.gy && my < L.gy + L.grows && mx >= L.gx) {
		int	col = (mx - L.gx) / L.slot;
		int	rowi = my - L.gy;
		int	idx;

		if (col < 0 || col >= L.gcols)
			return (-1);
		per = L.gcols * L.grows;
		base = per > 0 ? (cursel / per) * per : 0;	/* the shown page */
		idx = base + rowi * L.gcols + col;
		if (idx < 0 || idx >= dv_uni_count(cat))
			return (-1);
		*newsel = idx;
		return (1);
	}
	(void)newcat;
	return (-1);
}
