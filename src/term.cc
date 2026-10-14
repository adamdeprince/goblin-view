/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Per-window screen and a VT parser written from public DEC/xterm
 * behaviour, not from another multiplexer. Parser state lives in the
 * screen because a pty hands sequences over in pieces.
 */

#include "dv.hpp"
#include "silkie.hpp"

#include <climits>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

enum { ST_GROUND, ST_ESC, ST_ESC2, ST_CSI, ST_OSC, ST_STR };

int	dv_scrollback_lines = 1000;	/* lines of scrollback for new screens */

static void xcur_clear(struct dv_screen *);
static void xcur_csi(struct dv_window *);

/* Which string we are collecting in ST_OSC / ST_STR. */
enum { STR_OSC = 1, STR_DCS, STR_APC, STR_PM, STR_SOS };

static void
cell_blank(struct dv_cell *c, uint32_t fg, uint32_t bg)
{
	c->ch = U' ';
	c->fg = fg;
	c->bg = bg;
	c->attr = 0;
	c->ustyle = 0;
	c->ucolor = DV_COLOR_DEFAULT;
	c->irow = c->icol = 0;
	c->alpha = 100;
	c->fg_br = c->bg_br = 100;
	c->fg_gamut = c->bg_gamut = 0;
}

static struct dv_cell *
slot(struct dv_screen *s, unsigned x, unsigned y)
{
	if (x >= s->cols || y >= s->rows)
		return (nullptr);
	return (s->cells + y * s->cols + x);
}

struct dv_cell *
dv_screen_at(struct dv_screen *s, unsigned x, unsigned y)
{
	return (slot(s, x, y));
}

static void
fill_blank(struct dv_cell *p, unsigned n, uint32_t fg, uint32_t bg)
{
	unsigned	i;

	for (i = 0; i < n; i++)
		cell_blank(&p[i], fg, bg);
}

/* Erased cells take the current background (bce) and no attributes. */
static void
erase(struct dv_screen *s, unsigned x0, unsigned y, unsigned n)
{
	struct dv_cell	*c;

	if (y >= s->rows || x0 >= s->cols)
		return;
	if (x0 + n > s->cols)
		n = s->cols - x0;
	c = slot(s, x0, y);
	fill_blank(c, n, s->fg, s->bg);
}

void
dv_screen_init(struct dv_screen *s, unsigned cols, unsigned rows)
{
	if (cols < 1)
		cols = 1;
	if (rows < 1)
		rows = 1;
	memset(s, 0, sizeof *s);
	s->cols = cols;
	s->rows = rows;
	s->cells = dv_allocn<dv_cell>((size_t)cols * rows);
	s->mt = 0;
	s->mb = rows - 1;
	s->visible = 1;
	s->wrap = 1;
	s->alt_scroll = 1;	/* wheel scrolls alt-screen apps by default */
	s->sb_cap = dv_scrollback_lines > 0 ? (unsigned)dv_scrollback_lines : 1;
	s->sb = dv_allocn<dv_cell *>(s->sb_cap);
	s->sb_w = dv_allocn<unsigned>(s->sb_cap);
	s->fg = DV_COLOR_DEFAULT;
	s->bg = DV_COLOR_DEFAULT;
	s->ucolor = DV_COLOR_DEFAULT;
	s->alpha = 100;
	s->fg_br = s->bg_br = 100;
	s->val = -1;
	s->ph_n = -1;
	fill_blank(s->cells, cols * rows, s->fg, s->bg);
}

void
dv_screen_free(struct dv_screen *s)
{
	unsigned	i;

	for (i = 0; i < s->sb_len; i++)
		free(s->sb[(s->sb_head + i) % s->sb_cap]);
	free(s->sb);
	free(s->sb_w);
	for (i = 0; i < DV_STEXTS; i++)
		free(s->stext[i].text);
	free(s->cells);
	free(s->alt);
	free(s->params);
	free(s->sub);
	delete s->kpl;
	memset(s, 0, sizeof *s);
}

static struct dv_cell *
grid_resize(struct dv_cell *old, unsigned ocols, unsigned orows,
    unsigned cols, unsigned rows)
{
	struct dv_cell	*n;
	unsigned	 x, y, cc, rr;

	n = dv_allocn<dv_cell>((size_t)cols * rows);
	fill_blank(n, cols * rows, DV_COLOR_DEFAULT, DV_COLOR_DEFAULT);
	cc = cols < ocols ? cols : ocols;
	rr = rows < orows ? rows : orows;
	for (y = 0; y < rr; y++)
		for (x = 0; x < cc; x++)
			n[y * cols + x] = old[y * ocols + x];
	free(old);
	return (n);
}

static void
clamp_save(struct dv_cursor_save *sv, unsigned cols, unsigned rows)
{
	if (sv->cr >= rows)
		sv->cr = rows - 1;
	if (sv->cc >= cols)
		sv->cc = cols - 1;
}

/*
 * Scroll the viewer's window into the primary-screen scrollback by delta
 * lines (positive = back into history). Returns 1 if the position moved.
 * No effect on the alternate screen, which keeps no history.
 */
int
dv_screen_scroll(struct dv_screen *s, int delta)
{
	long	off = (long)s->sb_off + delta;

	if (s->alt_on)
		return (0);
	if (off < 0)
		off = 0;
	if (off > (long)s->sb_len)
		off = (long)s->sb_len;
	if ((unsigned)off == s->sb_off)
		return (0);
	s->sb_off = (unsigned)off;
	return (1);
}

/*
 * The cell to display at window position (x,y), accounting for how far
 * the viewer has scrolled back. With sb_off == 0 this is the live cell;
 * otherwise the top rows come from the scrollback ring. A padded blank
 * fills a stored line narrower than the window.
 */
struct dv_cell *
dv_screen_disp_at(struct dv_screen *s, unsigned x, unsigned y)
{
	static struct dv_cell	blank;
	unsigned		vi, first;

	if (s->sb_off == 0 || s->alt_on)
		return (slot(s, x, y));
	blank.ch = U' ';
	blank.fg = blank.bg = DV_COLOR_DEFAULT;
	blank.attr = 0;
	blank.irow = blank.icol = 0;
	blank.alpha = 100;
	blank.fg_br = blank.bg_br = 100;
	blank.fg_gamut = blank.bg_gamut = 0;
	first = s->sb_len - s->sb_off;		/* virtual index of the top row */
	vi = first + y;
	if (vi < s->sb_len) {			/* a scrollback line */
		struct dv_cell	*line = s->sb[(s->sb_head + vi) % s->sb_cap];

		if (x < s->sb_w[(s->sb_head + vi) % s->sb_cap])
			return (&line[x]);
		return (&blank);
	}
	{
		unsigned	sr = vi - s->sb_len;	/* a live screen row */

		if (sr >= s->rows)
			sr = s->rows - 1;
		return (slot(s, x, sr));
	}
}

void
dv_screen_resize(struct dv_screen *s, unsigned cols, unsigned rows)
{
	if (cols < 1)
		cols = 1;
	if (rows < 1)
		rows = 1;
	if (cols == s->cols && rows == s->rows)
		return;
	s->cells = grid_resize(s->cells, s->cols, s->rows, cols, rows);
	if (s->alt != nullptr)
		s->alt = grid_resize(s->alt, s->cols, s->rows, cols, rows);
	s->cols = cols;
	s->rows = rows;
	if (s->sb_off > s->sb_len)
		s->sb_off = s->sb_len;
	s->mt = 0;
	s->mb = rows - 1;
	if (s->cr >= rows)
		s->cr = rows - 1;
	if (s->cc >= cols)
		s->cc = cols - 1;
	clamp_save(&s->saved, cols, rows);
	clamp_save(&s->saved_alt, cols, rows);
}

/*
 * Kitty placements scroll with the text they sit on: those anchored in
 * the scrolled region move with it, and one carried wholly out of the
 * region is gone, as in kitty. Virtual (U=1) placements are positioned
 * by placeholder cells, which scroll as text on their own.
 */
static void
places_scroll(struct dv_screen *s, unsigned top, unsigned bot, int dy)
{
	if (s->kpl == nullptr)
		return;
	for (auto it = s->kpl->begin(); it != s->kpl->end(); ) {
		auto &p = it->second;
		/* Any overlap with the region moves it (its top may already
		 * be above the screen from an earlier scroll). */
		if (p.virt || p.alt != s->alt_on ||
		    p.row + (int)p.rows - 1 < (int)top || p.row > (int)bot) {
			++it;
			continue;
		}
		p.row += dy;
		if ((dy < 0 && p.row + (int)p.rows <= (int)top) ||
		    (dy > 0 && p.row > (int)bot))
			it = s->kpl->erase(it);
		else
			++it;
	}
}

/* Drop placements: every one (RIS), or the direct ones on this screen
 * (ED 2, leaving the alternate screen). */
static void
places_clear(struct dv_screen *s, int all)
{
	if (s->kpl != nullptr)
		std::erase_if(*s->kpl, [s, all](const auto &entry) {
			const auto &p = entry.second;
			return (all || (!p.virt && p.alt == s->alt_on));
		});
}

/* Push one screen row into the scrollback ring, evicting the oldest when
 * full. The row is copied at the current width. */
static void
sb_push(struct dv_screen *s, const struct dv_cell *row)
{
	unsigned	idx;

	if (s->sb == nullptr || s->sb_cap == 0)
		return;
	if (s->sb_len == s->sb_cap) {		/* full: drop the oldest */
		free(s->sb[s->sb_head]);
		s->sb_head = (s->sb_head + 1) % s->sb_cap;
		s->sb_len--;
	}
	idx = (s->sb_head + s->sb_len) % s->sb_cap;
	s->sb[idx] = dv_allocn<dv_cell>(s->cols);
	memcpy(s->sb[idx], row, (size_t)s->cols * sizeof(struct dv_cell));
	s->sb_w[idx] = s->cols;
	s->sb_len++;
}

/* A scrollback line from outside (a handed-over state): width cells,
 * stored as they are. */
void
dv_screen_sb_push(struct dv_screen *s, const struct dv_cell *row, unsigned width)
{
	unsigned	idx;

	if (s->sb == nullptr || s->sb_cap == 0)
		return;
	if (s->sb_len == s->sb_cap) {
		free(s->sb[s->sb_head]);
		s->sb_head = (s->sb_head + 1) % s->sb_cap;
		s->sb_len--;
	}
	idx = (s->sb_head + s->sb_len) % s->sb_cap;
	s->sb[idx] = dv_allocn<dv_cell>(width > 0 ? width : 1);
	memcpy(s->sb[idx], row, (size_t)width * sizeof(struct dv_cell));
	s->sb_w[idx] = width;
	s->sb_len++;
}

/*
 * Cells as text, for a state handed to another server: each cell's
 * fields in hex, joined by commas, identical neighbours run-length
 * folded with *count, cells separated by spaces.
 */
static int
cell_same(const struct dv_cell *a, const struct dv_cell *b)
{
	return (a->ch == b->ch && a->fg == b->fg && a->bg == b->bg && a->attr == b->attr &&
	    a->ustyle == b->ustyle && a->ucolor == b->ucolor && a->irow == b->irow &&
	    a->icol == b->icol && a->alpha == b->alpha && a->fg_br == b->fg_br &&
	    a->bg_br == b->bg_br && a->fg_gamut == b->fg_gamut &&
	    a->bg_gamut == b->bg_gamut);
}

void
dv_cells_encode(struct dv_buf *b, const struct dv_cell *cells, unsigned n)
{
	unsigned	i = 0;
	char		t[160];

	while (i < n) {
		unsigned	run = 1;
		int		l;

		while (i + run < n && cell_same(&cells[i], &cells[i + run]))
			run++;
		l = snprintf(t, sizeof t, "%s%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x",
		    i ? " " : "",
		    cells[i].ch, cells[i].fg, cells[i].bg, cells[i].attr, cells[i].ustyle,
		    cells[i].ucolor, cells[i].irow, cells[i].icol, cells[i].alpha,
		    cells[i].fg_br, cells[i].bg_br, cells[i].fg_gamut, cells[i].bg_gamut);
		if (run > 1)
			l += snprintf(t + l, sizeof t - (size_t)l, "*%u", run);
		dv_buf_append(b, t, (size_t)l);
		i += run;
	}
}

unsigned
dv_cells_decode(const char *text, struct dv_cell *cells, unsigned n)
{
	unsigned	i = 0;
	const char	*p = text;

	while (i < n && *p != '\0') {
		unsigned	v[13], run = 1, k;
		char		*end;

		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		v[8] = 100;
		v[9] = v[10] = 100;
		v[11] = v[12] = 0;
		for (k = 0; k < 8; k++) {
			v[k] = (unsigned)strtoul(p, &end, 16);
			if (end == p)
				return (i);
			p = end;
			if (k < 7) {
				if (*p != ',')
					return (i);
				p++;
			}
		}
		for (k = 8; k < 13 && *p == ','; k++) {
			p++;
			v[k] = (unsigned)strtoul(p, &end, 16);
			if (end == p)
				return (i);
			p = end;
		}
		if (*p == '*') {
			run = (unsigned)strtoul(p + 1, &end, 10);
			p = end;
		}
		for (k = 0; k < run && i < n; k++, i++) {
			cells[i].ch = v[0];
			cells[i].fg = v[1];
			cells[i].bg = v[2];
			cells[i].attr = (uint8_t)v[3];
			cells[i].ustyle = (uint8_t)v[4];
			cells[i].ucolor = v[5];
			cells[i].irow = (uint16_t)v[6];
			cells[i].icol = (uint16_t)v[7];
			cells[i].alpha = (uint8_t)v[8];
			cells[i].fg_br = (uint16_t)v[9];
			cells[i].bg_br = (uint16_t)v[10];
			cells[i].fg_gamut = (uint8_t)v[11];
			cells[i].bg_gamut = (uint8_t)v[12];
		}
	}
	return (i);
}

/* Scroll rows top..bot (inclusive) up by n, blanking at the bottom. */
static void
scroll_up(struct dv_screen *s, unsigned top, unsigned bot, unsigned n)
{
	unsigned	 y, w = s->cols;

	if (bot >= s->rows)
		bot = s->rows - 1;
	if (top > bot)
		return;
	if (n > bot - top + 1)
		n = bot - top + 1;
	if (n == 0)
		return;
	/* Lines leaving the top of the primary screen enter scrollback. */
	if (top == 0 && !s->alt_on)
		for (y = 0; y < n; y++)
			sb_push(s, s->cells + y * w);
	for (y = top; y + n <= bot; y++)
		memcpy(s->cells + y * w, s->cells + (y + n) * w,
		    w * sizeof(struct dv_cell));
	for (y = bot + 1 - n; y <= bot; y++)
		fill_blank(s->cells + y * w, w, s->fg, s->bg);
	places_scroll(s, top, bot, -(int)n);
}

static void
scroll_down(struct dv_screen *s, unsigned top, unsigned bot, unsigned n)
{
	unsigned	 y, w = s->cols;

	if (bot >= s->rows)
		bot = s->rows - 1;
	if (top > bot)
		return;
	if (n > bot - top + 1)
		n = bot - top + 1;
	if (n == 0)
		return;
	for (y = bot; y >= top + n; y--)
		memcpy(s->cells + y * w, s->cells + (y - n) * w,
		    w * sizeof(struct dv_cell));
	for (y = top; y < top + n; y++)
		fill_blank(s->cells + y * w, w, s->fg, s->bg);
	places_scroll(s, top, bot, (int)n);
}

/*
 * Character width. Zero for combining marks and format characters,
 * two for East Asian wide/fullwidth and the emoji blocks that every
 * modern terminal renders double width. Not a full Unicode table, but
 * enough that a prompt with a glyph in it stays aligned.
 */
struct range {
	uint32_t	lo;
	uint32_t	hi;
};

static const struct range zero_width[] = {
	{ 0x0300, 0x036F }, { 0x0483, 0x0489 }, { 0x0591, 0x05BD },
	{ 0x05BF, 0x05BF }, { 0x05C1, 0x05C2 }, { 0x05C4, 0x05C5 },
	{ 0x05C7, 0x05C7 }, { 0x0610, 0x061A }, { 0x064B, 0x065F },
	{ 0x0670, 0x0670 }, { 0x06D6, 0x06DC }, { 0x06DF, 0x06E4 },
	{ 0x06E7, 0x06E8 }, { 0x06EA, 0x06ED }, { 0x0900, 0x0902 },
	{ 0x093A, 0x093A }, { 0x093C, 0x093C }, { 0x0941, 0x0948 },
	{ 0x094D, 0x094D }, { 0x0951, 0x0957 }, { 0x0962, 0x0963 },
	{ 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0E47, 0x0E4E },
	{ 0x1AB0, 0x1AFF }, { 0x1DC0, 0x1DFF }, { 0x200B, 0x200F },
	{ 0x2028, 0x202E }, { 0x2060, 0x2064 }, { 0x20D0, 0x20FF },
	{ 0xFE00, 0xFE0F }, { 0xFE20, 0xFE2F }, { 0xFEFF, 0xFEFF },
	{ 0x1F3FB, 0x1F3FF }, { 0xE0100, 0xE01EF },
};

static const struct range wide[] = {
	{ 0x1100, 0x115F }, { 0x231A, 0x231B }, { 0x2329, 0x232A },
	{ 0x23E9, 0x23EC }, { 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 },
	{ 0x25FD, 0x25FE }, { 0x2614, 0x2615 }, { 0x2648, 0x2653 },
	{ 0x267F, 0x267F }, { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 },
	{ 0x26AA, 0x26AB }, { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 },
	{ 0x26CE, 0x26CE }, { 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA },
	{ 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 }, { 0x26FA, 0x26FA },
	{ 0x26FD, 0x26FD }, { 0x2705, 0x2705 }, { 0x270A, 0x270B },
	{ 0x2728, 0x2728 }, { 0x274C, 0x274C }, { 0x274E, 0x274E },
	{ 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 },
	{ 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF }, { 0x2B1B, 0x2B1C },
	{ 0x2B50, 0x2B50 }, { 0x2B55, 0x2B55 }, { 0x2E80, 0x303E },
	{ 0x3041, 0x33FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF },
	{ 0xA000, 0xA4CF }, { 0xA960, 0xA97F }, { 0xAC00, 0xD7A3 },
	{ 0xF900, 0xFAFF }, { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE6F },
	{ 0xFF00, 0xFF60 }, { 0xFFE0, 0xFFE6 }, { 0x1F004, 0x1F004 },
	{ 0x1F0CF, 0x1F0CF }, { 0x1F18E, 0x1F18E }, { 0x1F191, 0x1F19A },
	{ 0x1F200, 0x1F251 }, { 0x1F300, 0x1F64F }, { 0x1F680, 0x1F6FF },
	{ 0x1F7E0, 0x1F7EB }, { 0x1F900, 0x1F9FF }, { 0x1FA70, 0x1FAFF },
	{ 0x20000, 0x2FFFD }, { 0x30000, 0x3FFFD },
};

static int
in_ranges(uint32_t cp, const struct range *r, size_t n)
{
	size_t	lo = 0, hi = n;

	while (lo < hi) {
		size_t	mid = (lo + hi) / 2;

		if (cp < r[mid].lo)
			hi = mid;
		else if (cp > r[mid].hi)
			lo = mid + 1;
		else
			return (1);
	}
	return (0);
}

int
dv_wcwidth(uint32_t cp)
{
	if (cp == 0)
		return (0);
	if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0))
		return (0);
	if (cp < 0x300)
		return (1);
	if (in_ranges(cp, zero_width, sizeof zero_width / sizeof zero_width[0]))
		return (0);
	if (in_ranges(cp, wide, sizeof wide / sizeof wide[0]))
		return (2);
	return (1);
}

/*
 * DEC special graphics, for `ESC ( 0` line drawing. Index is ch - 0x5f.
 */
static const uint32_t dec_graphics[] = {
	U' ',      0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
	0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
	0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
	0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

static uint32_t
map_charset(struct dv_screen *s, uint32_t cp)
{
	if (s->charset[s->shift] == 0 || cp >= 0x80)
		return (cp);
	if (cp >= 0x5f && cp <= 0x7e)
		return (dec_graphics[cp - 0x5f]);
	switch (cp) {
	case '+':
		return (0x2192);
	case ',':
		return (0x2190);
	case '-':
		return (0x2191);
	case '.':
		return (0x2193);
	case '0':
		return (0x2588);
	default:
		return (cp);
	}
}

/* Writing into half of a wide glyph blanks the other half. */
static void
split_wide(struct dv_screen *s, unsigned x, unsigned y)
{
	struct dv_cell	*c = slot(s, x, y), *o;

	if (c == nullptr)
		return;
	if (c->ch == 0 && x > 0) {
		o = slot(s, x - 1, y);
		if (o != nullptr && (o->attr & DV_ATTR_WIDE)) {
			o->ch = U' ';
			o->attr &= (uint8_t)~DV_ATTR_WIDE;
		}
	}
	if ((c->attr & DV_ATTR_WIDE) && x + 1 < s->cols) {
		o = slot(s, x + 1, y);
		if (o != nullptr && o->ch == 0)
			o->ch = U' ';
	}
}

static void
index_down(struct dv_screen *s)
{
	if (s->cr == s->mb)
		scroll_up(s, s->mt, s->mb, 1);
	else if (s->cr + 1 < s->rows)
		s->cr++;
}

static void
reverse_index(struct dv_screen *s)
{
	if (s->cr == s->mt)
		scroll_down(s, s->mt, s->mb, 1);
	else if (s->cr > 0)
		s->cr--;
}

static void
insert_cells(struct dv_screen *s, unsigned x, unsigned y, unsigned n)
{
	struct dv_cell	*row;
	unsigned	 i;

	if (y >= s->rows || x >= s->cols)
		return;
	if (n > s->cols - x)
		n = s->cols - x;
	row = s->cells + y * s->cols;
	for (i = s->cols - 1; i >= x + n; i--)
		row[i] = row[i - n];
	fill_blank(row + x, n, s->fg, s->bg);
}

static void
delete_cells(struct dv_screen *s, unsigned x, unsigned y, unsigned n)
{
	struct dv_cell	*row;
	unsigned	 i;

	if (y >= s->rows || x >= s->cols)
		return;
	if (n > s->cols - x)
		n = s->cols - x;
	row = s->cells + y * s->cols;
	for (i = x; i + n < s->cols; i++)
		row[i] = row[i + n];
	fill_blank(row + s->cols - n, n, s->fg, s->bg);
}

/* The image id a program encodes in a placeholder cell's foreground:
 * the RGB triple as 24 bits, or a palette index. */
static uint32_t
fg_image_id(uint32_t fg)
{
	if (fg & DV_COLOR_RGB)
		return (fg & 0xffffffu);
	if (fg == DV_COLOR_DEFAULT)
		return (0);
	return (fg & 0xffu);
}

/*
 * A U+10EEEE placeholder cell (kitty's Unicode placeholder mode, which a
 * program uses inside tmux): the cell names its image by the foreground
 * colour and its row/column by the diacritics that follow. Without
 * diacritics it continues the row of the placeholder before it.
 */
static void
placeholder_begin(struct dv_screen *s, struct dv_cell *c)
{
	struct dv_cell	*prev = s->cc > 0 ? slot(s, s->cc - 1, s->cr) : nullptr;

	c->fg = fg_image_id(s->fg);
	c->irow = c->icol = 0;
	if (prev != nullptr && prev->ch == DV_KITTY_PLACEHOLDER &&
	    (prev->fg & 0xffffffu) == c->fg) {
		c->fg = prev->fg;		/* including the id's high byte */
		c->irow = prev->irow;
		c->icol = (uint16_t)(prev->icol + 1);
	}
	s->ph_x = s->cc;
	s->ph_y = s->cr;
	s->ph_n = 0;
}

static void
placeholder_mark(struct dv_screen *s, uint32_t cp)
{
	struct dv_cell	*c;
	int		 idx;

	if (s->ph_n < 0 || s->ph_n > 2 || s->cr != s->ph_y ||
	    s->cc != s->ph_x + 1)
		return;
	idx = dv_placeholder_index(cp);
	c = slot(s, s->ph_x, s->ph_y);
	if (idx < 0 || c == nullptr || c->ch != DV_KITTY_PLACEHOLDER)
		return;
	switch (s->ph_n++) {
	case 0:
		c->irow = (uint16_t)idx;
		break;
	case 1:
		c->icol = (uint16_t)idx;
		break;
	default:
		c->fg = (c->fg & 0xffffffu) | ((uint32_t)idx << 24);
		break;
	}
}

static void
put_ch(struct dv_screen *s, uint32_t cp)
{
	struct dv_cell	*c;
	int		 width;

	cp = map_charset(s, cp);
	width = cp == DV_KITTY_PLACEHOLDER ? 1 : dv_wcwidth(cp);
	if (width == 0) {
		placeholder_mark(s, cp);
		return;		/* other combining marks: not modelled yet */
	}

	/* Deferred wrap: the cursor sits past the last column until then. */
	if (s->cc >= s->cols) {
		if (s->wrap) {
			s->cc = 0;
			index_down(s);
		} else
			s->cc = s->cols - 1;
	}
	if (width == 2 && s->cc + 1 >= s->cols) {
		if (!s->wrap)
			return;
		erase(s, s->cc, s->cr, 1);
		s->cc = 0;
		index_down(s);
		if (s->cols < 2)
			return;
	}
	if (s->insert)
		insert_cells(s, s->cc, s->cr, (unsigned)width);

	split_wide(s, s->cc, s->cr);
	c = slot(s, s->cc, s->cr);
	if (c == nullptr)
		return;
	c->ch = cp;
	c->fg = s->fg;
	c->bg = s->bg;
	c->attr = s->attr;
	c->ustyle = s->ustyle;
	c->ucolor = s->ucolor;
	c->alpha = s->alpha;
	c->fg_br = s->fg_br;
	c->bg_br = s->bg_br;
	c->fg_gamut = s->fg_gamut;
	c->bg_gamut = s->bg_gamut;
	c->irow = c->icol = 0;
	if (cp == DV_KITTY_PLACEHOLDER)
		placeholder_begin(s, c);
	else
		s->ph_n = -1;
	if (width == 2) {
		c->attr |= DV_ATTR_WIDE;
		split_wide(s, s->cc + 1, s->cr);
		c = slot(s, s->cc + 1, s->cr);
		if (c != nullptr) {
			c->ch = 0;
			c->fg = s->fg;
			c->bg = s->bg;
			c->attr = s->attr;
			c->ustyle = s->ustyle;
			c->ucolor = s->ucolor;
			c->alpha = s->alpha;
			c->fg_br = s->fg_br;
			c->bg_br = s->bg_br;
			c->fg_gamut = s->fg_gamut;
			c->bg_gamut = s->bg_gamut;
		}
	}
	s->cc += (unsigned)width;
	s->last_ch = cp;
}

static void
cursor_set(struct dv_screen *s, int r, int c)
{
	if (r < 0)
		r = 0;
	if (c < 0)
		c = 0;
	if ((unsigned)r >= s->rows)
		r = (int)s->rows - 1;
	if ((unsigned)c >= s->cols)
		c = (int)s->cols - 1;
	s->cr = (unsigned)r;
	s->cc = (unsigned)c;
}

static void
save_cursor(struct dv_screen *s, struct dv_cursor_save *sv)
{
	sv->cr = s->cr;
	sv->cc = s->cc < s->cols ? s->cc : s->cols - 1;
	sv->fg = s->fg;
	sv->bg = s->bg;
	sv->attr = s->attr;
	sv->ustyle = s->ustyle;
	sv->ucolor = s->ucolor;
	sv->alpha = s->alpha;
	sv->fg_br = s->fg_br;
	sv->bg_br = s->bg_br;
	sv->fg_gamut = s->fg_gamut;
	sv->bg_gamut = s->bg_gamut;
	sv->valid = 1;
}

static void
restore_cursor(struct dv_screen *s, const struct dv_cursor_save *sv)
{
	if (!sv->valid) {
		cursor_set(s, 0, 0);
		return;
	}
	cursor_set(s, (int)sv->cr, (int)sv->cc);
	s->fg = sv->fg;
	s->bg = sv->bg;
	s->attr = sv->attr;
	s->ustyle = sv->ustyle;
	s->ucolor = sv->ucolor;
	s->alpha = sv->alpha;
	s->fg_br = sv->fg_br;
	s->bg_br = sv->bg_br;
	s->fg_gamut = sv->fg_gamut;
	s->bg_gamut = sv->bg_gamut;
}

static void
swap_alt(struct dv_screen *s, int on)
{
	struct dv_cell	*t;

	if (on != s->alt_on)
		xcur_clear(s);		/* extra cursors do not survive the switch */
	if (on && !s->alt_on)
		s->sb_off = 0;		/* the alt screen has no scrollback */
	if (on && !s->alt_on) {
		if (s->alt == nullptr)
			s->alt = dv_allocn<dv_cell>((size_t)s->cols * s->rows);
		fill_blank(s->alt, s->cols * s->rows, DV_COLOR_DEFAULT,
		    DV_COLOR_DEFAULT);
		t = s->cells;
		s->cells = s->alt;
		s->alt = t;
		s->alt_on = 1;
	} else if (!on && s->alt_on) {
		places_clear(s, 0);		/* the alt screen's images go */
		t = s->cells;
		s->cells = s->alt;
		s->alt = t;
		s->alt_on = 0;
	}
}

static void
reset(struct dv_screen *s)
{
	swap_alt(s, 0);
	places_clear(s, 1);
	{
		unsigned	i;

		for (i = 0; i < DV_STEXTS; i++) {
			free(s->stext[i].text);
			s->stext[i].text = nullptr;
			s->stext[i].used = 0;
		}
	}
	fill_blank(s->cells, s->cols * s->rows, DV_COLOR_DEFAULT,
	    DV_COLOR_DEFAULT);
	s->cr = s->cc = 0;
	s->mt = 0;
	s->mb = s->rows - 1;
	s->wrap = 1;
	s->alt_scroll = 1;
	s->bracketed = 0;
	s->pointer_mode = 0;
	s->visible = 1;
	s->app_cursor = 0;
	s->insert = 0;
	memset(s->nkbd, 0, sizeof s->nkbd);
	xcur_clear(s);
	s->xcur_tcs = s->xcur_ccs = 0;
	s->fg = s->bg = DV_COLOR_DEFAULT;
	s->ucolor = DV_COLOR_DEFAULT;
	s->attr = 0;
	s->ustyle = 0;
	s->alpha = 100;
	s->fg_br = s->bg_br = 100;
	s->fg_gamut = s->bg_gamut = 0;
	s->silk_tablet = s->silk_scroll = s->silk_gesture = 0;
	s->silk_frame = s->silk_latency = s->silk_display = s->silk_ink = 0;
	s->silk_frame_seq = 0;
	s->silk_frame_dirty = 0;
	s->silk_brush[0] = '\0';
	s->silk_shown[0] = '\0';
	s->sace = 0;
	s->nsmode = 0;
	memset(&s->saved, 0, sizeof s->saved);
	memset(&s->saved_alt, 0, sizeof s->saved_alt);
	s->charset[0] = s->charset[1] = 0;
	s->shift = 0;
}

/*
 * Bytes for the program: written now if the pty takes them, else queued
 * and drained as it drains (POLLOUT in the server), so a big reply never
 * blocks the server and a key never lands inside a half-written escape.
 */
void
dv_window_pty_flush(struct dv_window *w)
{
	while (w->pty_out.len > 0 && w->pty_fd >= 0) {
		size_t	n = w->pty_out.len > 4096 ? 4096 : w->pty_out.len;
		ssize_t	r = write(w->pty_fd, w->pty_out.data, n);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return;			/* EAGAIN: POLLOUT calls again */
		}
		if (r == 0)
			return;
		dv_buf_consume(&w->pty_out, (size_t)r);
	}
}

void
dv_window_pty_send(struct dv_window *w, const void *data, size_t n)
{
	if (w->pty_fd < 0 || n == 0)
		return;
	dv_buf_append(&w->pty_out, data, n);
	dv_window_pty_flush(w);
}

static void
vt_reply(struct dv_window *w, const char *str)
{
	dv_window_pty_send(w, str, strlen(str));
}

/*
 * The kitty keyboard flag stacks: one for the main screen, one for the
 * alternate, each holding the flags in force. The top is what the
 * server encodes keys with; an empty stack means legacy keys. A push
 * onto a full stack evicts the oldest entry; a pop that empties the
 * stack resets every flag; set edits the top (a base entry if none).
 */
int
dv_screen_kbd_flags(const struct dv_screen *s)
{
	int	a = s->alt_on ? 1 : 0;

	return (s->nkbd[a] > 0 ? s->kbd[a][s->nkbd[a] - 1] : 0);
}

static void
kbd_push(struct dv_screen *s, int flags)
{
	int	a = s->alt_on ? 1 : 0;

	if (s->nkbd[a] >= 8) {
		memmove(&s->kbd[a][0], &s->kbd[a][1], sizeof(int) * 7);
		s->nkbd[a] = 7;
	}
	s->kbd[a][s->nkbd[a]++] = flags & 31;
}

static void
kbd_pop(struct dv_screen *s, int n)
{
	int	a = s->alt_on ? 1 : 0;

	if (n < 1)
		n = 1;
	if (n >= s->nkbd[a])
		s->nkbd[a] = 0;
	else
		s->nkbd[a] -= n;
}

static void
kbd_set(struct dv_screen *s, int flags, int mode)
{
	int	a = s->alt_on ? 1 : 0, cur;

	if (s->nkbd[a] == 0)
		kbd_push(s, 0);
	cur = s->kbd[a][s->nkbd[a] - 1];
	if (mode == 2)
		cur |= flags;
	else if (mode == 3)
		cur &= ~flags;
	else
		cur = flags;
	s->kbd[a][s->nkbd[a] - 1] = cur & 31;
}

static int
param(const struct dv_screen *s, int i, int def)
{
	if (i >= s->np || s->params[i] == 0)
		return (def);
	return (s->params[i]);
}

const struct dv_cell *
dv_screen_history_row(struct dv_screen *s, unsigned vi, unsigned *width)
{
	if (vi >= s->sb_len)
		return (nullptr);
	*width = s->sb_w[(s->sb_head + vi) % s->sb_cap];
	return (s->sb[(s->sb_head + vi) % s->sb_cap]);
}

/* Subparameters that follow params[i] via ':'. */
static int
subcount(const struct dv_screen *s, int i)
{
	int	k = 0;

	while (i + 1 + k < s->np && s->sub[i + 1 + k])
		k++;
	return (k);
}

static uint32_t
color_256(int n)
{
	if (n < 0)
		n = 0;
	if (n > 255)
		n = 255;
	return ((uint32_t)n);
}

static uint32_t
color_rgb(int r, int g, int b)
{
	return (DV_RGB(r & 0xff, g & 0xff, b & 0xff));
}

/*
 * Extended colour: 38/48 followed by 5;n or 2;r;g;b, either with
 * semicolons or with colons (where an empty colour-space id may sit
 * between the 2 and the r). Returns the parameters consumed after i.
 */
static uint8_t
gamut_id(int id)
{
	if (id < 0 || id > 2)
		return (0);
	return ((uint8_t)id);
}

/* Brightness above SDR white, in quarter steps, clamped. 100 and below
 * is the default. The percent is only a colon sub-parameter. */
static uint16_t
sgr_brightness(const struct dv_screen *s, int i)
{
	int	v;

	if (subcount(s, i) < 1)
		return (100);
	v = s->params[i + 1];
	if (v <= 100)
		return (100);
	if (v > 1675)
		v = 1675;
	v = ((v + 12) / 25) * 25;
	if (v > 1675)
		v = 1675;
	if (v <= 100)
		return (100);
	return ((uint16_t)v);
}

/* Background opacity. 100 and above is opaque. Even percentages only. */
static uint8_t
sgr_alpha(const struct dv_screen *s, int i)
{
	int	v;

	if (subcount(s, i) < 1)
		return (100);
	v = s->params[i + 1];
	if (v >= 100)
		return (100);
	if (v < 0)
		v = 0;
	return ((uint8_t)(v & ~1));
}

static int
sgr_extended(struct dv_screen *s, int i, uint32_t *out, uint8_t *gamut)
{
	int	k = subcount(s, i);

	if (gamut != nullptr)
		*gamut = 0;
	if (k > 0) {
		if (s->params[i + 1] == 5 && k >= 2) {
			*out = color_256(s->params[i + 2]);
			return (k);
		}
		if (s->params[i + 1] == 2 && k >= 4) {
			if (k >= 5) {
				*out = color_rgb(s->params[i + 3],
				    s->params[i + 4], s->params[i + 5]);
				if (gamut != nullptr)
					*gamut = gamut_id(s->params[i + 2]);
			} else
				*out = color_rgb(s->params[i + 2],
				    s->params[i + 3], s->params[i + 4]);
			return (k);
		}
		return (k);
	}
	if (i + 2 < s->np && s->params[i + 1] == 5) {
		*out = color_256(s->params[i + 2]);
		return (2);
	}
	if (i + 4 < s->np && s->params[i + 1] == 2) {
		*out = color_rgb(s->params[i + 2], s->params[i + 3],
		    s->params[i + 4]);
		return (4);
	}
	return (s->np - i - 1);
}

/* The style a run of SGR parameters acts on: the cursor's, or a cell's
 * (DECCARA). Underline style and colour ride with it (kitty extension). */
struct sgr_st {
	uint32_t	fg, bg, ucolor;
	uint8_t		attr, ustyle;
	uint8_t		alpha;
	uint16_t	fg_br, bg_br;
	uint8_t		fg_gamut, bg_gamut;
};

static void
sgr_underline(struct sgr_st *t, int style)
{
	t->ustyle = (uint8_t)style;
	if (style)
		t->attr |= DV_ATTR_UNDER;
	else
		t->attr &= (uint8_t)~DV_ATTR_UNDER;
}

/*
 * Apply params[from..np) as SGR to a style. Kitty's extensions: 4:n
 * picks an underline style (1 straight, 2 double, 3 curly, 4 dotted, 5
 * dashed; 0 none), 58/59 set/reset the underline colour, 221/222 reset
 * bold and faint independently.
 */
static void
sgr_apply(struct dv_screen *s, int from, struct sgr_st *t)
{
	int	i;

	for (i = from; i < s->np; i++) {
		int	v = s->params[i];

		if (s->sub[i])
			continue;	/* a stray subparameter */
		switch (v) {
		case 0:
			t->fg = t->bg = DV_COLOR_DEFAULT;
			t->ucolor = DV_COLOR_DEFAULT;
			t->attr = 0;
			t->ustyle = 0;
			t->alpha = 100;
			t->fg_br = t->bg_br = 100;
			t->fg_gamut = t->bg_gamut = 0;
			break;
		case 1:
			t->attr |= DV_ATTR_BOLD;
			break;
		case 2:
			t->attr |= DV_ATTR_DIM;
			break;
		case 3:
			t->attr |= DV_ATTR_ITALIC;
			break;
		case 4:
			if (subcount(s, i) > 0) {
				int	st = s->params[i + 1];

				sgr_underline(t, (st >= 0 && st <= 5) ? st : 1);
			} else
				sgr_underline(t, 1);
			break;
		case 5:
		case 6:
			t->attr |= DV_ATTR_BLINK;
			break;
		case 7:
			t->attr |= DV_ATTR_REV;
			break;
		case 9:
			t->attr |= DV_ATTR_STRIKE;
			break;
		case 21:
			sgr_underline(t, 2);	/* double underline */
			break;
		case 22:
			t->attr &= (uint8_t)~(DV_ATTR_BOLD | DV_ATTR_DIM);
			break;
		case 23:
			t->attr &= (uint8_t)~DV_ATTR_ITALIC;
			break;
		case 24:
			sgr_underline(t, 0);
			break;
		case 25:
			t->attr &= (uint8_t)~DV_ATTR_BLINK;
			break;
		case 27:
			t->attr &= (uint8_t)~DV_ATTR_REV;
			break;
		case 29:
			t->attr &= (uint8_t)~DV_ATTR_STRIKE;
			break;
		case 38:
			i += sgr_extended(s, i, &t->fg, &t->fg_gamut);
			break;
		case 39:
			t->fg = DV_COLOR_DEFAULT;
			t->fg_gamut = 0;
			break;
		case 48:
			i += sgr_extended(s, i, &t->bg, &t->bg_gamut);
			break;
		case 49:
			t->bg = DV_COLOR_DEFAULT;
			t->bg_gamut = 0;
			break;
		case 56:
			t->fg_br = sgr_brightness(s, i);
			break;
		case 57:
			t->bg_br = sgr_brightness(s, i);
			break;
		case 58:
			i += sgr_extended(s, i, &t->ucolor, nullptr);
			break;
		case 59:
			t->ucolor = DV_COLOR_DEFAULT;
			break;
		case 70:
			t->alpha = sgr_alpha(s, i);
			break;
		case 221:
			t->attr &= (uint8_t)~DV_ATTR_BOLD;
			break;
		case 222:
			t->attr &= (uint8_t)~DV_ATTR_DIM;
			break;
		default:
			if (v >= 30 && v <= 37) {
				t->fg = (uint32_t)(v - 30);
				t->fg_gamut = 0;
			} else if (v >= 40 && v <= 47) {
				t->bg = (uint32_t)(v - 40);
				t->bg_gamut = 0;
			} else if (v >= 90 && v <= 97) {
				t->fg = (uint32_t)(v - 90 + 8);
				t->fg_gamut = 0;
			} else if (v >= 100 && v <= 107) {
				t->bg = (uint32_t)(v - 100 + 8);
				t->bg_gamut = 0;
			}
			break;
		}
	}
}

static void
sgr(struct dv_screen *s)
{
	struct sgr_st	t;

	if (s->np == 0) {
		s->fg = s->bg = DV_COLOR_DEFAULT;
		s->ucolor = DV_COLOR_DEFAULT;
		s->attr = 0;
		s->ustyle = 0;
		s->alpha = 100;
		s->fg_br = s->bg_br = 100;
		s->fg_gamut = s->bg_gamut = 0;
		return;
	}
	t.fg = s->fg;
	t.bg = s->bg;
	t.ucolor = s->ucolor;
	t.attr = s->attr;
	t.ustyle = s->ustyle;
	t.alpha = s->alpha;
	t.fg_br = s->fg_br;
	t.bg_br = s->bg_br;
	t.fg_gamut = s->fg_gamut;
	t.bg_gamut = s->bg_gamut;
	sgr_apply(s, 0, &t);
	s->fg = t.fg;
	s->bg = t.bg;
	s->ucolor = t.ucolor;
	s->attr = t.attr;
	s->ustyle = t.ustyle;
	s->alpha = t.alpha;
	s->fg_br = t.fg_br;
	s->bg_br = t.bg_br;
	s->fg_gamut = t.fg_gamut;
	s->bg_gamut = t.bg_gamut;
}

/*
 * DECCARA (CSI Pt ; Pl ; Pb ; Pr $ r, then SGR parameters): apply the
 * attributes to a screen region -- a rectangle under DECSACE 2, else a
 * stream from (Pt,Pl) to (Pb,Pr). Kitty allows every SGR attribute here,
 * colours included. No attributes means reset.
 */
static void
deccara(struct dv_screen *s)
{
	int		pt = param(s, 0, 1), pl = param(s, 1, 1);
	int		pb = param(s, 2, (int)s->rows), pr = param(s, 3, (int)s->cols);
	unsigned	y, x;

	if (pt < 1)
		pt = 1;
	if (pl < 1)
		pl = 1;
	if (pb > (int)s->rows)
		pb = (int)s->rows;
	if (pr > (int)s->cols)
		pr = (int)s->cols;
	if (pb < pt)
		return;
	/* A stream may end at an earlier column on a later row; only a
	 * rectangle (or a single-row run) needs Pr >= Pl. */
	if ((s->sace == 2 || pt == pb) && pr < pl)
		return;
	for (y = (unsigned)pt - 1; y < (unsigned)pb; y++) {
		unsigned	x0 = 0, x1 = s->cols;

		if (s->sace == 2 || pt == pb) {	/* rectangle, or a single row */
			x0 = (unsigned)pl - 1;
			x1 = (unsigned)pr;
		} else {				/* stream */
			if (y == (unsigned)pt - 1)
				x0 = (unsigned)pl - 1;
			if (y == (unsigned)pb - 1)
				x1 = (unsigned)pr;
		}
		for (x = x0; x < x1; x++) {
			struct dv_cell	*c = slot(s, x, y);
			struct sgr_st	 t;

			if (c == nullptr)
				continue;
			t.fg = c->fg;
			t.bg = c->bg;
			t.ucolor = c->ucolor;
			t.attr = c->attr & (uint8_t)~DV_ATTR_WIDE;
			t.ustyle = c->ustyle;
			t.alpha = c->alpha;
			t.fg_br = c->fg_br;
			t.bg_br = c->bg_br;
			t.fg_gamut = c->fg_gamut;
			t.bg_gamut = c->bg_gamut;
			if (s->np <= 4) {
				t.fg = t.bg = DV_COLOR_DEFAULT;
				t.ucolor = DV_COLOR_DEFAULT;
				t.attr = 0;
				t.ustyle = 0;
				t.alpha = 100;
				t.fg_br = t.bg_br = 100;
				t.fg_gamut = t.bg_gamut = 0;
			} else
				sgr_apply(s, 4, &t);
			c->fg = t.fg;
			c->bg = t.bg;
			c->ucolor = t.ucolor;
			c->attr = t.attr | (c->attr & DV_ATTR_WIDE);
			c->ustyle = t.ustyle;
			c->alpha = t.alpha;
			c->fg_br = t.fg_br;
			c->bg_br = t.bg_br;
			c->fg_gamut = t.fg_gamut;
			c->bg_gamut = t.bg_gamut;
		}
	}
}

/*
 * Unscroll (CSI n + T): scroll the screen down n lines, filling the new
 * top lines from the newest scrollback rather than leaving them blank.
 * On the alternate screen, or with no history, the lines are blank.
 */
static void
unscroll(struct dv_screen *s, unsigned n)
{
	unsigned	i;

	if (n == 0)
		n = 1;
	if (n > s->rows)
		n = s->rows;
	for (i = 0; i < n; i++) {
		scroll_down(s, 0, s->rows - 1, 1);
		if (!s->alt_on && s->sb_len > 0) {
			unsigned	idx = (s->sb_head + s->sb_len - 1) % s->sb_cap;
			struct dv_cell	*line = s->sb[idx];
			unsigned	w = s->sb_w[idx], k;

			for (k = 0; k < w && k < s->cols; k++)
				s->cells[k] = line[k];
			free(line);
			s->sb[idx] = nullptr;
			s->sb_len--;
			if (s->sb_off > s->sb_len)
				s->sb_off = s->sb_len;
		}
	}
}

static void
ed(struct dv_screen *s, int mode)
{
	unsigned	 y;
	unsigned	 cc = s->cc < s->cols ? s->cc : s->cols - 1;

	switch (mode) {
	case 0:
		erase(s, cc, s->cr, s->cols - cc);
		for (y = s->cr + 1; y < s->rows; y++)
			erase(s, 0, y, s->cols);
		break;
	case 1:
		for (y = 0; y < s->cr; y++)
			erase(s, 0, y, s->cols);
		erase(s, 0, s->cr, cc + 1);
		break;
	case 2:
		for (y = 0; y < s->rows; y++)
			erase(s, 0, y, s->cols);
		places_clear(s, 0);	/* kitty clears the images with it */
		xcur_clear(s);		/* ...and the extra cursors */
		break;
	case 3:
		xcur_clear(s);		/* scrollback we do not keep; cursors go */
		break;
	case 22:
		/* kitty: everything on screen goes into scrollback, then clear. */
		if (!s->alt_on)
			for (y = 0; y < s->rows; y++)
				sb_push(s, s->cells + y * s->cols);
		for (y = 0; y < s->rows; y++)
			erase(s, 0, y, s->cols);
		places_clear(s, 0);
		xcur_clear(s);
		break;
	default:
		break;
	}
}

static void
el(struct dv_screen *s, int mode)
{
	unsigned	 cc = s->cc < s->cols ? s->cc : s->cols - 1;

	switch (mode) {
	case 0:
		erase(s, cc, s->cr, s->cols - cc);
		break;
	case 1:
		erase(s, 0, s->cr, cc + 1);
		break;
	case 2:
		erase(s, 0, s->cr, s->cols);
		break;
	default:
		break;
	}
}

static void
set_mode(struct dv_screen *s, int priv, int mode, int on)
{
	if (!priv) {
		if (mode == 4)
			s->insert = on;
		return;
	}
	switch (mode) {
	case 1:
		s->app_cursor = on;
		break;
	case 7:
		s->wrap = on;
		break;
	case 25:
		s->visible = on;
		break;
	case 80:	/* DECSDM: a sixel at the top left, cursor untouched */
		s->sixel_dm = on;
		break;
	case 8452:	/* a sixel leaves the cursor to its right */
		s->sixel_right = on;
		break;
	case 1000:
	case 1002:
	case 1003:
		/* One tracking level at a time; resetting a level you are not
		 * in leaves the current one alone (so XTRESTORE can replay). */
		if (on)
			s->mouse = mode;
		else if (s->mouse == mode)
			s->mouse = 0;
		break;
	case 1004:
		break;	/* focus reporting: goblin-view owns the host focus */
	case 1007:	/* alternate scroll: wheel becomes arrows (default on) */
		s->alt_scroll = on;
		break;
	case 2004:	/* bracketed paste: the app wants ESC[200~/201~ wrappers */
		s->bracketed = on;
		break;
	case 1005:
	case 1006:
	case 1015:
	case 1016:	/* SGR with pixel coordinates (kitty/xterm) */
		if (on)
			s->mouse_sgr = mode == 1006 ? 1 : mode == 1016 ? 2 : 0;
		else if ((mode == 1006 && s->mouse_sgr == 1) ||
		    (mode == 1016 && s->mouse_sgr == 2) || mode == 1005 ||
		    mode == 1015)
			s->mouse_sgr = 0;
		break;
	case 47:
	case 1047:
		swap_alt(s, on);
		break;
	case 1048:
		if (on)
			save_cursor(s, &s->saved);
		else
			restore_cursor(s, &s->saved);
		break;
	case 1049:
		if (on) {
			save_cursor(s, &s->saved);
			swap_alt(s, 1);
			s->cr = s->cc = 0;
		} else {
			swap_alt(s, 0);
			restore_cursor(s, &s->saved);
		}
		break;
	default:
		break;
	}
}

/* The host's cell size, from the session's desktop; a default until a
 * client has measured. */
static void
cell_pixels(const struct dv_window *w, unsigned *xp, unsigned *yp)
{
	*xp = w->session ? w->session->desktop.xpixel : 0;
	*yp = w->session ? w->session->desktop.ypixel : 0;
	if (*xp == 0)
		*xp = DV_DEFAULT_XPIXEL;
	if (*yp == 0)
		*yp = DV_DEFAULT_YPIXEL;
}

/* Private modes safe to save and restore wholesale (XTSAVE/XTRESTORE
 * with no parameters -- kitty's extension): nothing that moves the cursor
 * or the screen contents (no DECCOLM, no alternate screen). */
static const int	free_modes[] = {
	1, 7, 25, 1000, 1002, 1003, 1006, 1016, 1007, 2004, 80, 8452,
};

static int
mode_get(const struct dv_screen *s, int mode)
{
	switch (mode) {
	case 1: return (s->app_cursor);
	case 7: return (s->wrap);
	case 25: return (s->visible);
	case 1000: case 1002: case 1003: return (s->mouse == mode);
	case 1006: return (s->mouse_sgr == 1);
	case 1016: return (s->mouse_sgr == 2);
	case 1007: return (s->alt_scroll);
	case 2004: return (s->bracketed);
	case 80: return (s->sixel_dm);
	case 8452: return (s->sixel_right);
	default: return (-1);
	}
}

static void
smode_record(struct dv_screen *s, int mode)
{
	int	v = mode_get(s, mode), k;

	if (v < 0)
		return;
	for (k = 0; k < s->nsmode; k++)
		if (s->smode_num[k] == mode) {
			s->smode_val[k] = v;
			return;
		}
	if (s->nsmode < 16) {
		s->smode_num[s->nsmode] = mode;
		s->smode_val[s->nsmode] = v;
		s->nsmode++;
	}
}

static void
xtsave(struct dv_screen *s)
{
	int	i;

	if (s->np == 0) {
		for (i = 0; i < (int)(sizeof free_modes / sizeof free_modes[0]); i++)
			smode_record(s, free_modes[i]);
		return;
	}
	for (i = 0; i < s->np; i++)
		smode_record(s, s->params[i]);
}

static void
xtrestore(struct dv_screen *s)
{
	int	i, k;

	if (s->np == 0) {
		for (k = 0; k < s->nsmode; k++)
			set_mode(s, 1, s->smode_num[k], s->smode_val[k]);
		return;
	}
	for (i = 0; i < s->np; i++)
		for (k = 0; k < s->nsmode; k++)
			if (s->smode_num[k] == s->params[i])
				set_mode(s, 1, s->params[i], s->smode_val[k]);
}

static void
do_csi(struct dv_window *w, int final)
{
	struct dv_screen	*s = &w->screen;
	char			 rep[64];
	int			 a, b, i;
	unsigned		 cc;

	switch (final) {
	case 'A':
		cursor_set(s, (int)s->cr - param(s, 0, 1), (int)s->cc);
		break;
	case 'B':
	case 'e':
		cursor_set(s, (int)s->cr + param(s, 0, 1), (int)s->cc);
		break;
	case 'C':
	case 'a':
		cursor_set(s, (int)s->cr, (int)s->cc + param(s, 0, 1));
		break;
	case 'D':
		cc = s->cc < s->cols ? s->cc : s->cols - 1;
		cursor_set(s, (int)s->cr, (int)cc - param(s, 0, 1));
		break;
	case 'E':
		cursor_set(s, (int)s->cr + param(s, 0, 1), 0);
		break;
	case 'F':
		cursor_set(s, (int)s->cr - param(s, 0, 1), 0);
		break;
	case 'G':
	case '`':
		cursor_set(s, (int)s->cr, param(s, 0, 1) - 1);
		break;
	case 'H':
	case 'f':
		cursor_set(s, param(s, 0, 1) - 1, param(s, 1, 1) - 1);
		break;
	case 'd':
		cursor_set(s, param(s, 0, 1) - 1, (int)s->cc);
		break;
	case 'I':
		for (a = param(s, 0, 1); a > 0 && s->cc + 1 < s->cols; a--) {
			s->cc = (s->cc + 8) & ~7u;
			if (s->cc >= s->cols)
				s->cc = s->cols - 1;
		}
		break;
	case 'Z':
		for (a = param(s, 0, 1); a > 0; a--) {
			if (s->cc == 0)
				break;
			s->cc = (s->cc - 1) & ~7u;
		}
		break;
	case 'J':
		ed(s, param(s, 0, 0));
		break;
	case 'K':
		el(s, param(s, 0, 0));
		break;
	case 'L':
		if (s->cr >= s->mt && s->cr <= s->mb)
			scroll_down(s, s->cr, s->mb, (unsigned)param(s, 0, 1));
		break;
	case 'M':
		if (s->cr >= s->mt && s->cr <= s->mb)
			scroll_up(s, s->cr, s->mb, (unsigned)param(s, 0, 1));
		break;
	case '@':
		insert_cells(s, s->cc, s->cr, (unsigned)param(s, 0, 1));
		break;
	case 'P':
		delete_cells(s, s->cc, s->cr, (unsigned)param(s, 0, 1));
		break;
	case 'X':
		erase(s, s->cc, s->cr, (unsigned)param(s, 0, 1));
		break;
	case 'S':
		if (s->prefix == 0)
			scroll_up(s, s->mt, s->mb, (unsigned)param(s, 0, 1));
		else if (s->prefix == '?') {
			/*
			 * XTSMGRAPHICS: a program asks what sixel can do here.
			 * Colour registers and the geometry, at the host's cell
			 * size; a failure status when the host has no graphics.
			 */
			int		pi = param(s, 0, 0);
			unsigned	xp = 0, yp = 0;

			cell_pixels(w, &xp, &yp);
			if (!dv_kitty_host_gfx(w))
				snprintf(rep, sizeof rep, "\033[?%d;3;0S", pi);
			else if (pi == 1)
				snprintf(rep, sizeof rep, "\033[?1;0;256S");
			else if (pi == 2)
				snprintf(rep, sizeof rep, "\033[?2;0;%u;%uS",
				    s->cols * xp, s->rows * yp);
			else
				snprintf(rep, sizeof rep, "\033[?%d;1;0S", pi);
			vt_reply(w, rep);
		}
		break;
	case 'T':
		if (s->inter == '+')		/* kitty unscroll */
			unscroll(s, (unsigned)param(s, 0, 1));
		else if (s->prefix == 0)
			scroll_down(s, s->mt, s->mb, (unsigned)param(s, 0, 1));
		break;
	case 'b':
		if (s->last_ch != 0)
			for (a = param(s, 0, 1); a > 0; a--)
				put_ch(s, s->last_ch);
		break;
	case 'm':
		if (s->prefix == 0)
			sgr(s);
		break;
	case 'r':
		if (s->prefix == '?') {		/* XTRESTORE private modes */
			xtrestore(s);
			break;
		}
		if (s->inter == '$') {		/* DECCARA */
			deccara(s);
			break;
		}
		a = param(s, 0, 1);
		b = param(s, 1, (int)s->rows);
		if (a < 1)
			a = 1;
		if (b > (int)s->rows)
			b = (int)s->rows;
		if (b <= a)
			break;
		s->mt = (unsigned)(a - 1);
		s->mb = (unsigned)(b - 1);
		cursor_set(s, 0, 0);
		break;
	case 's':
		if (s->prefix == '?')		/* XTSAVE private modes */
			xtsave(s);
		else if (s->prefix == 0)
			save_cursor(s, &s->saved);
		break;
	case 'x':
		if (s->inter == '*')		/* DECSACE: attribute change extent */
			s->sace = param(s, 0, 0);
		break;
	case 'u':
		if (s->prefix == 0)
			restore_cursor(s, &s->saved);
		else if (s->prefix == '?') {	/* kitty keyboard: query */
			snprintf(rep, sizeof rep, "\033[?%du", dv_screen_kbd_flags(s));
			vt_reply(w, rep);
		} else if (s->prefix == '>')
			kbd_push(s, param(s, 0, 0));
		else if (s->prefix == '<')
			kbd_pop(s, param(s, 0, 1));
		else if (s->prefix == '=')
			kbd_set(s, param(s, 0, 0), param(s, 1, 1));
		break;
	case 'h':
	case 'l':
		for (i = 0; i < s->np; i++)
			set_mode(s, s->prefix == '?', s->params[i],
			    final == 'h');
		break;
	case 'n':
		if (s->prefix != 0 && s->prefix != '?')
			break;
		if (param(s, 0, 0) == 5)
			vt_reply(w, "\033[0n");
		else if (param(s, 0, 0) == 6) {
			cc = s->cc < s->cols ? s->cc : s->cols - 1;
			snprintf(rep, sizeof rep, "\033[%u;%uR", s->cr + 1,
			    cc + 1);
			vt_reply(w, rep);
		}
		break;
	case 'c':
		/* DA1 claims sixel (4) only when the host takes kitty
		 * graphics, which is what a sixel here becomes. */
		if (s->prefix == 0 && param(s, 0, 0) == 0)
			vt_reply(w, dv_kitty_host_gfx(w) ? "\033[?62;4;22c" :
			    "\033[?62;22c");
		else if (s->prefix == '>' && param(s, 0, 0) == 0)
			vt_reply(w, "\033[>1;10;0c");
		break;
	case 'q':
		if (s->prefix == '>' && s->inter == ' ')	/* extra cursors */
			xcur_csi(w);
		else if (s->prefix == '>')	/* XTVERSION: name ourselves */
			vt_reply(w, "\033P>|GoblinView " DV_VERSION "\033\\");
		break;
	case 'p':
		/*
		 * DECRQM: report a private mode as "not recognised" (0), so a
		 * program that probes support (sync 2026, bracketed paste, ...)
		 * gets an answer and does not block waiting for one.
		 */
		if (s->prefix == '?') {
			snprintf(rep, sizeof rep, "\033[?%d;0$y", param(s, 0, 0));
			vt_reply(w, rep);
		}
		break;
	case 't':
		/*
		 * XTWINOPS size queries (14/16/18 are the report opcodes, not
		 * assumed sizes). The cell-pixel VALUE is the host terminal's,
		 * measured by the client (CSI 16 t / in-band resize) and kept
		 * live in the session's desktop -- this window relays the
		 * parent's cell size, plus its own cols/rows. A program placing
		 * a kitty image (a LaTeX previewer) reads it to size the image.
		 * The default only stands in until a client has measured.
		 */
		if (s->prefix == 0 && param(s, 0, 0) == 8) {
			/*
			 * Resize request: CSI 8 ; rows ; cols t. Record it;
			 * the server resizes the window after this read (it
			 * owns the geometry, clamping and pty resize). A zero
			 * dimension means "as large as fits".
			 */
			w->resize_req = 1;
			w->req_rows = (unsigned)param(s, 1, 0);
			w->req_cols = (unsigned)param(s, 2, 0);
			break;
		}
		if (s->prefix == 0) {
			unsigned	xp, yp;

			cell_pixels(w, &xp, &yp);
			if (param(s, 0, 0) == 14)	/* text area, pixels */
				snprintf(rep, sizeof rep, "\033[4;%u;%ut",
				    s->rows * yp, s->cols * xp);
			else if (param(s, 0, 0) == 16)	/* cell size, pixels */
				snprintf(rep, sizeof rep, "\033[6;%u;%ut", yp, xp);
			else if (param(s, 0, 0) == 18)	/* text area, cells */
				snprintf(rep, sizeof rep, "\033[8;%u;%ut",
				    s->rows, s->cols);
			else
				rep[0] = '\0';
			if (rep[0] != '\0')
				vt_reply(w, rep);
		}
		break;
	default:
		break;
	}
}

static void
do_esc(struct dv_screen *s, unsigned char b)
{
	switch (b) {
	case '7':
		save_cursor(s, s->alt_on ? &s->saved_alt : &s->saved);
		break;
	case '8':
		restore_cursor(s, s->alt_on ? &s->saved_alt : &s->saved);
		break;
	case 'D':
		index_down(s);
		break;
	case 'E':
		s->cc = 0;
		index_down(s);
		break;
	case 'M':
		reverse_index(s);
		break;
	case 'c':
		reset(s);
		break;
	default:
		break;
	}
}

void
dv_pointer_send(struct dv_window *w, const goblin::pointer::Message &m)
{
    namespace gp = goblin::pointer;
    int at = -1;
    for (int i=0;i<w->npointer;++i) {
        const auto &a=w->pointer_active[i];
        if (a.kind==m.kind && a.device==m.device && a.contact==m.contact) { at=i; break; }
    }
    if (m.phase==gp::Down || m.phase==gp::Move) {
        if (at<0) { if (w->npointer>=32) return; at=w->npointer++; }
        w->pointer_active[at]=m;
    } else if (m.phase==gp::Up || m.phase==gp::Cancel) {
        if (at<0) return;
        w->pointer_active[at]=w->pointer_active[--w->npointer];
    }
    const auto bytes=gp::encode(m); dv_window_pty_send(w,bytes.data(),bytes.size());
}
void
dv_pointer_cancel(struct dv_window *w)
{
    while (w->npointer>0) {
        auto m=w->pointer_active[w->npointer-1];
        m.phase=goblin::pointer::Cancel; m.buttons=m.p=0;
        dv_pointer_send(w,m);
    }
    ++w->pointer_epoch;
}

/* An integer metadata value for OSC 66: key= in a colon-separated list. */
static int
osc66_int(const char *m, size_t n, char key, int def)
{
	size_t	i;

	for (i = 0; i + 1 < n; i++)
		if ((i == 0 || m[i - 1] == ':') && m[i] == key && m[i + 1] == '=') {
			int	v = 0, any = 0;
			size_t	j = i + 2;

			while (j < n && m[j] >= '0' && m[j] <= '9') {
				v = v * 10 + (m[j] - '0');
				j++;
				any = 1;
			}
			return (any ? v : def);
		}
	return (def);
}

/*
 * OSC 66 text sizing: ESC ] 66 ; s=..:w=.. ; text ST. Lay the text as a
 * run occupying (base width * scale) columns by (scale) rows, mark those
 * cells so the compositor can reproduce it, and advance the cursor by the
 * scaled width on the same row -- which is also how a program detects
 * that we support the protocol (its CPR probe sees the cursor move).
 */
static void
osc66(struct dv_window *w, const char *t, size_t n)
{
	struct dv_screen	*s = &w->screen;
	const char		*meta, *text;
	size_t			 i, mlen, tlen;
	int			 sc, wp, slotn = -1, q;
	unsigned		 bw, cols, rows, rr, cc, id, sr, sco;
	struct dv_stext		*r;

	for (i = 0; i < n && t[i] != ';'; i++)	/* past "66" */
		;
	if (i >= n)
		return;
	meta = t + i + 1;
	for (i = i + 1; i < n && t[i] != ';'; i++)
		;
	mlen = (size_t)(t + i - meta);
	text = (i < n) ? t + i + 1 : "";
	tlen = (i < n) ? n - i - 1 : 0;
	if (tlen == 0)
		return;

	sc = osc66_int(meta, mlen, 's', 1);
	wp = osc66_int(meta, mlen, 'w', 0);
	if (sc < 1)
		sc = 1;
	if (sc > 7)
		sc = 7;
	if (wp < 0)
		wp = 0;
	if (wp > 7)
		wp = 7;

	if (wp > 0)
		bw = (unsigned)wp;
	else {					/* natural width in cells */
		uint32_t	cp = 0;
		int		need = 0;

		bw = 0;
		for (i = 0; i < tlen; i++) {
			unsigned char	b = (unsigned char)text[i];

			if (need > 0) {
				if ((b & 0xc0) == 0x80) {
					cp = (cp << 6) | (b & 0x3f);
					if (--need == 0)
						bw += (unsigned)dv_wcwidth(cp);
				} else
					need = 0;
				continue;
			}
			if (b < 0x80) {
				bw += (unsigned)dv_wcwidth(b);
			} else if ((b & 0xe0) == 0xc0) {
				cp = b & 0x1f;
				need = 1;
			} else if ((b & 0xf0) == 0xe0) {
				cp = b & 0x0f;
				need = 2;
			} else if ((b & 0xf8) == 0xf0) {
				cp = b & 0x07;
				need = 3;
			}
		}
		if (bw == 0)
			bw = 1;
	}
	cols = bw * (unsigned)sc;
	rows = (unsigned)sc;
	if (cols == 0)
		return;

	for (q = 0; q < DV_STEXTS; q++)		/* a free slot, else the oldest */
		if (!s->stext[q].used) {
			slotn = q;
			break;
		}
	if (slotn < 0) {
		int	best = 0;

		for (q = 1; q < DV_STEXTS; q++)
			if (s->stext[q].gen < s->stext[best].gen)
				best = q;
		free(s->stext[best].text);
		slotn = best;
	}
	r = &s->stext[slotn];
	memset(r, 0, sizeof *r);
	r->used = 1;
	r->gen = ++s->stext_gen;
	r->s = (uint8_t)sc;
	r->wparam = (uint8_t)wp;
	r->bw = (uint16_t)bw;
	r->cols = (uint16_t)cols;
	r->rows = (uint16_t)rows;
	r->fg = s->fg;
	r->bg = s->bg;
	r->attr = s->attr;
	r->len = (uint16_t)(tlen > 4096 ? 4096 : tlen);
	r->text = (char *)malloc(r->len);
	if (r->text == nullptr) {
		r->used = 0;
		return;
	}
	memcpy(r->text, text, r->len);

	id = DV_STEXT_FIRST + ((unsigned)w->id << 8) + (unsigned)slotn;
	sr = s->cr;
	sco = s->cc >= s->cols ? s->cols - 1 : s->cc;
	for (rr = 0; rr < rows; rr++)
		for (cc = 0; cc < cols; cc++) {
			struct dv_cell	*cell = slot(s, sco + cc, sr + rr);

			if (cell == nullptr)
				continue;
			cell->ch = DV_STEXT_MARK;
			cell->fg = id;
			cell->bg = r->bg;
			cell->attr = 0;
			cell->ustyle = 0;
			cell->ucolor = DV_COLOR_DEFAULT;
			cell->irow = (uint16_t)rr;
			cell->icol = (uint16_t)cc;
		}
	s->cc = (sco + cols <= s->cols) ? sco + cols : s->cols;
	s->last_ch = 0;
}

/* The scaled-text run behind a host-global id, and its window. */
struct dv_stext *
dv_stext_lookup(struct dv_session *s, unsigned id, struct dv_window **wp)
{
	struct dv_window	*w;
	unsigned		 win, slotn;

	if (id < DV_STEXT_FIRST)
		return (nullptr);
	win = (id - DV_STEXT_FIRST) >> 8;
	slotn = (id - DV_STEXT_FIRST) & 0xff;
	if (slotn >= DV_STEXTS)
		return (nullptr);
	TAILQ_FOREACH(w, &s->desktop.windows, entry)
		if (w->id == win) {
			struct dv_stext	*r = &w->screen.stext[slotn];

			if (!r->used)
				return (nullptr);
			*wp = w;
			return (r);
		}
	return (nullptr);
}

void (*dv_dnd_app_hook)(struct dv_window *, const char *, size_t) = nullptr;

/* ---- extra cursors (kitty multiple cursors protocol) ---- */

int
dv_xcur_at(const struct dv_screen *s, unsigned col, unsigned row)
{
	int	i;

	for (i = 0; i < s->nxcur; i++)
		if (s->xcur[i].col == col && s->xcur[i].row == row)
			return (s->xcur[i].shape);
	return (0);
}

static void
xcur_clear(struct dv_screen *s)
{
	s->nxcur = 0;
}

/* Set (or, shape 0, remove) the cursor in one cell. */
static void
xcur_put(struct dv_screen *s, unsigned col, unsigned row, int shape)
{
	int	i;

	if (col >= s->cols || row >= s->rows)
		return;
	for (i = 0; i < s->nxcur; i++)
		if (s->xcur[i].col == col && s->xcur[i].row == row) {
			if (shape == 0) {
				s->xcur[i] = s->xcur[s->nxcur - 1];
				s->nxcur--;
			} else
				s->xcur[i].shape = (uint8_t)shape;
			return;
		}
	if (shape == 0 || s->nxcur >= DV_XCURSORS)
		return;
	s->xcur[s->nxcur].col = (uint16_t)col;
	s->xcur[s->nxcur].row = (uint16_t)row;
	s->xcur[s->nxcur].shape = (uint8_t)shape;
	s->nxcur++;
}

static void
fmt_colour(char *out, size_t cap, unsigned which, uint8_t cs, uint32_t col)
{
	if (cs == 2)
		snprintf(out, cap, "%u:2:%u:%u:%u", which, (col >> 16) & 0xff,
		    (col >> 8) & 0xff, col & 0xff);
	else if (cs == 5)
		snprintf(out, cap, "%u:5:%u", which, col & 0xff);
	else
		snprintf(out, cap, "%u:%u", which, cs);
}

/*
 * CSI > SHAPE ; TYPE : COORDS ; ... SP q. Shapes 0-3 and 29 place (or
 * clear) cursors: at the main cursor (type 0), at y:x points (type 2),
 * over top:left:bottom:right rectangles (type 4). 30 and 40 set the
 * text and cursor colours; 100 and 101 query; no shape asks what we do.
 */
static void
xcur_csi(struct dv_window *w)
{
	struct dv_screen	*s = &w->screen;
	int			 shape = param(s, 0, 0), i;
	char			 rep[8192];

	if (s->np == 0) {			/* CSI > SP q: what do we support? */
		vt_reply(w, "\033[>1;2;3;29;30;40;100;101 q");
		return;
	}
	if (shape == 100) {
		size_t	n = 0;
		int	first = 1;

		n += (size_t)snprintf(rep + n, sizeof rep - n, "\033[>100");
		for (i = 0; i < s->nxcur && n + 40 < sizeof rep; i++) {
			n += (size_t)snprintf(rep + n, sizeof rep - n, "%s%u:2:%u:%u",
			    first ? ";" : ";", s->xcur[i].shape, s->xcur[i].row + 1,
			    s->xcur[i].col + 1);
			first = 0;
		}
		snprintf(rep + n, sizeof rep - n, " q");
		vt_reply(w, rep);
		return;
	}
	if (shape == 101) {
		char	a[64], b[64];

		fmt_colour(a, sizeof a, 30, s->xcur_tcs, s->xcur_tcol);
		fmt_colour(b, sizeof b, 40, s->xcur_ccs, s->xcur_ccol);
		snprintf(rep, sizeof rep, "\033[>101;%s;%s q", a, b);
		vt_reply(w, rep);
		return;
	}
	if (shape == 30 || shape == 40) {
		int		cs = s->np > 1 ? s->params[1] : 0;
		uint32_t	col = 0;

		if (cs == 2 && s->np > 4)
			col = ((uint32_t)(s->params[2] & 0xff) << 16) |
			    ((uint32_t)(s->params[3] & 0xff) << 8) | (uint32_t)(s->params[4] & 0xff);
		else if (cs == 5 && s->np > 2)
			col = (uint32_t)(s->params[2] & 0xff);
		else if (cs != 0 && cs != 1)
			return;
		if (shape == 30) {
			s->xcur_tcs = (uint8_t)cs;
			s->xcur_tcol = col;
		} else {
			s->xcur_ccs = (uint8_t)cs;
			s->xcur_ccol = col;
		}
		return;
	}
	if (shape != 0 && shape != 1 && shape != 2 && shape != 3 && shape != 29)
		return;
	if (s->np == 1) {			/* no coordinates: the main cursor */
		xcur_put(s, s->cc, s->cr, shape);
		return;
	}
	for (i = 1; i < s->np; ) {
		int	type = s->params[i], j = i + 1, k;

		while (j < s->np && s->sub[j])
			j++;
		if (type == 0)
			xcur_put(s, s->cc, s->cr, shape);
		else if (type == 2)
			for (k = i + 1; k + 1 < j; k += 2)
				xcur_put(s, (unsigned)(s->params[k + 1] > 0 ? s->params[k + 1] - 1 : 0),
				    (unsigned)(s->params[k] > 0 ? s->params[k] - 1 : 0), shape);
		else if (type == 4) {
			if (j == i + 1) {		/* no rectangle: the whole screen */
				if (shape == 0)
					xcur_clear(s);
				else {
					unsigned	x, y;

					for (y = 0; y < s->rows; y++)
						for (x = 0; x < s->cols; x++)
							xcur_put(s, x, y, shape);
				}
			}
			for (k = i + 1; k + 3 < j; k += 4) {
				unsigned	top = s->params[k] > 0 ? (unsigned)s->params[k] - 1 : 0;
				unsigned	left = s->params[k + 1] > 0 ? (unsigned)s->params[k + 1] - 1 : 0;
				unsigned	bot = s->params[k + 2] > 0 ? (unsigned)s->params[k + 2] - 1 : s->rows - 1;
				unsigned	right = s->params[k + 3] > 0 ? (unsigned)s->params[k + 3] - 1 : s->cols - 1;
				unsigned	x, y;

				for (y = top; y <= bot && y < s->rows; y++)
					for (x = left; x <= right && x < s->cols; x++)
						xcur_put(s, x, y, shape);
			}
		}
		i = j;
	}
}

/* ---- forwarding to the host, and routing its replies back ---- */

/* Append this OSC verbatim, terminated with ST, to the bytes bound for
 * the host; the server flushes them to the clients. */
static void
forward_osc(struct dv_window *w)
{
	dv_buf_append(&w->host_out, "\033]", 2);
	dv_buf_append(&w->host_out, w->cap.data, w->cap.len);
	dv_buf_append(&w->host_out, "\033\\", 2);
}

/* Remember that the host's reply for (osc, id) belongs to this window. */
void
dv_route_expect(struct dv_window *w, unsigned osc, const char *id, size_t idlen)
{
	struct dv_session	*s = w->session;
	struct dv_osc_route	*r;
	unsigned		 k, oldest = 0;

	if (s == nullptr)
		return;
	if (idlen >= sizeof s->routes[0].id)
		idlen = sizeof s->routes[0].id - 1;
	for (k = 0; k < DV_OSC_ROUTES; k++) {
		r = &s->routes[k];
		if (r->osc == osc && strlen(r->id) == idlen &&
		    memcmp(r->id, id, idlen) == 0) {
			r->win = w->id;
			r->seq = ++s->route_seq;
			return;
		}
	}
	for (k = 0; k < DV_OSC_ROUTES; k++) {
		if (s->routes[k].osc == 0) {
			oldest = k;
			break;
		}
		if (s->routes[k].seq < s->routes[oldest].seq)
			oldest = k;
	}
	r = &s->routes[oldest];
	r->osc = osc;
	memcpy(r->id, id, idlen);
	r->id[idlen] = '\0';
	r->win = w->id;
	r->seq = ++s->route_seq;
}

/* The value of key= in colon- or semicolon-separated metadata. Returns
 * its length, 0 when absent (out is then empty). */
size_t
dv_meta_val(const char *m, size_t n, const char *key, char *out, size_t cap)
{
	size_t	kl = strlen(key), i, o = 0;

	if (cap == 0)
		return (0);
	out[0] = '\0';
	for (i = 0; i + kl < n; i++) {
		if ((i == 0 || m[i - 1] == ':' || m[i - 1] == ';') &&
		    memcmp(m + i, key, kl) == 0 && m[i + kl] == '=') {
			for (i += kl + 1; i < n && m[i] != ':' && m[i] != ';' &&
			    o + 1 < cap; i++)
				out[o++] = m[i];
			out[o] = '\0';
			return (o);
		}
	}
	return (0);
}

/* ---- colours: per-window palette overrides (OSC 4/10/11/12/21) ---- */

static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return (c - '0');
	if (c >= 'a' && c <= 'f')
		return (c - 'a' + 10);
	if (c >= 'A' && c <= 'F')
		return (c - 'A' + 10);
	return (-1);
}

/* A hex component of 1-4 digits scaled to 8 bits. */
static int
hex_component(const char *p, size_t n, unsigned *out)
{
	unsigned	v = 0;
	size_t		i;

	if (n < 1 || n > 4)
		return (0);
	for (i = 0; i < n; i++) {
		int	h = hexval(p[i]);

		if (h < 0)
			return (0);
		v = v * 16 + (unsigned)h;
	}
	switch (n) {
	case 1: *out = v * 17; break;
	case 2: *out = v; break;
	case 3: *out = v >> 4; break;
	default: *out = v >> 8; break;
	}
	return (1);
}

/* Parse an X11/kitty colour: rgb:R/G/B, rgbi:r/g/b, #RGB.., or a basic
 * name; an @alpha suffix is ignored. Returns 0 when not understood. */
static int
parse_color(const char *v, size_t n, uint32_t *out)
{
	static const struct { const char *nm; uint32_t rgb; } names[] = {
		/* X11 colour names, as XParseColor resolves them */
		{ "black", 0x000000 }, { "red", 0xff0000 }, { "green", 0x00ff00 },
		{ "yellow", 0xffff00 }, { "blue", 0x0000ff }, { "magenta", 0xff00ff },
		{ "cyan", 0x00ffff }, { "white", 0xffffff }, { "gray", 0xbebebe },
		{ "grey", 0xbebebe }, { "orange", 0xffa500 }, { "purple", 0xa020f0 },
		{ "brown", 0xa52a2a }, { "pink", 0xffc0cb },
	};
	unsigned	r, g, b;
	size_t		i;

	for (i = 0; i < n; i++)
		if (v[i] == '@') {
			n = i;
			break;
		}
	if (n >= 4 && memcmp(v, "rgb:", 4) == 0) {
		const char	*p = v + 4, *e = v + n, *q;
		unsigned	 c[3];
		int		 k;

		for (k = 0; k < 3; k++) {
			for (q = p; q < e && *q != '/'; q++)
				;
			if (!hex_component(p, (size_t)(q - p), &c[k]))
				return (0);
			if (k < 2 && q >= e)
				return (0);
			p = q + 1;
		}
		*out = DV_RGB(c[0], c[1], c[2]);
		return (1);
	}
	if (n >= 5 && memcmp(v, "rgbi:", 5) == 0) {
		double	f[3];
		char	tmp[64];
		int	k;
		char	*p, *q;

		if (n - 5 >= sizeof tmp)
			return (0);
		memcpy(tmp, v + 5, n - 5);
		tmp[n - 5] = '\0';
		p = tmp;
		for (k = 0; k < 3; k++) {
			f[k] = strtod(p, &q);
			if (q == p || f[k] < 0 || f[k] > 1)
				return (0);
			if (k < 2 && *q != '/')
				return (0);
			p = q + 1;
		}
		*out = DV_RGB((unsigned)(f[0] * 255 + 0.5),
		    (unsigned)(f[1] * 255 + 0.5), (unsigned)(f[2] * 255 + 0.5));
		return (1);
	}
	if (n >= 4 && v[0] == '#') {
		size_t	d = (n - 1) / 3;

		if ((n - 1) % 3 != 0 || d < 1 || d > 4)
			return (0);
		if (!hex_component(v + 1, d, &r) || !hex_component(v + 1 + d, d, &g) ||
		    !hex_component(v + 1 + 2 * d, d, &b))
			return (0);
		*out = DV_RGB(r, g, b);
		return (1);
	}
	for (i = 0; i < sizeof names / sizeof names[0]; i++)
		if (strlen(names[i].nm) == n && strncasecmp(v, names[i].nm, n) == 0) {
			*out = DV_COLOR_RGB | names[i].rgb;
			return (1);
		}
	return (0);
}

/* xterm's default 256-colour palette, for answering an unset index. */
uint32_t
dv_index_rgb(unsigned n)
{
	static const uint32_t base16[16] = {
		0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd,
		0x00cdcd, 0xe5e5e5, 0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
		0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
	};
	static const unsigned lv[6] = { 0, 95, 135, 175, 215, 255 };

	if (n < 16)
		return (DV_COLOR_RGB | base16[n]);
	if (n < 232) {
		unsigned	i = n - 16;

		return (DV_RGB(lv[i / 36], lv[(i / 6) % 6], lv[i % 6]));
	}
	{
		unsigned	g = 8 + 10 * (n - 232);

		return (DV_RGB(g, g, g));
	}
}

static void
pal_recompute(struct dv_palette *p)
{
	unsigned	i;

	p->any = p->fgset || p->bgset || p->curset;
	for (i = 0; i < 256 && !p->any; i++)
		if (p->set[i])
			p->any = 1;
}

/* "rgb:rrrr/gggg/bbbb" for a colour, the form xterm replies with. */
static void
fmt_rgb(uint32_t col, char *buf, size_t cap)
{
	unsigned	r = (col >> 16) & 0xff, g = (col >> 8) & 0xff, b = col & 0xff;

	snprintf(buf, cap, "rgb:%02x%02x/%02x%02x/%02x%02x", r, r, g, g, b, b);
}

static uint32_t
pal_index(const struct dv_screen *s, unsigned n)
{
	return (s->pal.set[n] ? s->pal.idx[n] : dv_index_rgb(n));
}

static uint32_t
pal_dynamic(const struct dv_screen *s, unsigned which)	/* 10 fg 11 bg 12 cursor */
{
	if (which == 10)
		return (s->pal.fgset ? s->pal.fg : (DV_COLOR_RGB | 0xe5e5e5));
	if (which == 11)
		return (s->pal.bgset ? s->pal.bg : (DV_COLOR_RGB | 0x000000));
	return (s->pal.curset ? s->pal.cursor : (DV_COLOR_RGB | 0xe5e5e5));
}

/* The host's cursor colour follows the focused window's override. */
void
dv_window_apply_cursor_color(struct dv_window *w)
{
	char	buf[64], c[32];
	int	n;

	if (w->screen.pal.curset) {
		fmt_rgb(w->screen.pal.cursor, c, sizeof c);
		n = snprintf(buf, sizeof buf, "\033]12;%s\033\\", c);
	} else
		n = snprintf(buf, sizeof buf, "\033]112\033\\");
	if (n > 0)
		dv_buf_append(&w->host_out, buf, (size_t)n);
}

/* OSC 4 ; N ; spec [; N ; spec ...]: set or query palette entries. */
static void
osc4(struct dv_window *w, const char *b, size_t n)
{
	struct dv_screen	*s = &w->screen;
	size_t			 i = 0;

	while (i < n) {
		unsigned	idx = 0;
		size_t		st;
		int		any = 0;

		while (i < n && b[i] >= '0' && b[i] <= '9') {
			idx = idx * 10 + (unsigned)(b[i] - '0');
			i++;
			any = 1;
		}
		if (!any || i >= n || b[i] != ';')
			return;
		i++;
		st = i;
		while (i < n && b[i] != ';')
			i++;
		if (idx < 256) {
			if (i - st == 1 && b[st] == '?') {
				char	c[32], rep[64];
				int	rn;

				fmt_rgb(pal_index(s, idx), c, sizeof c);
				rn = snprintf(rep, sizeof rep, "\033]4;%u;%s\033\\", idx, c);
				if (rn > 0)
					vt_reply(w, rep);
			} else {
				uint32_t	col;

				if (parse_color(b + st, i - st, &col)) {
					s->pal.idx[idx] = col;
					s->pal.set[idx] = 1;
					s->pal.any = 1;
				}
			}
		}
		if (i < n)
			i++;
	}
}

/* OSC 104 [; N ...]: reset palette entries (all when none given). */
static void
osc104(struct dv_window *w, const char *b, size_t n)
{
	struct dv_screen	*s = &w->screen;
	size_t			 i = 0;

	if (n == 0) {
		memset(s->pal.set, 0, sizeof s->pal.set);
		pal_recompute(&s->pal);
		return;
	}
	while (i < n) {
		unsigned	idx = 0;
		int		any = 0;

		while (i < n && b[i] >= '0' && b[i] <= '9') {
			idx = idx * 10 + (unsigned)(b[i] - '0');
			i++;
			any = 1;
		}
		if (any && idx < 256)
			s->pal.set[idx] = 0;
		while (i < n && b[i] != ';')
			i++;
		if (i < n)
			i++;
	}
	pal_recompute(&s->pal);
}

/* OSC 10/11/12 ; spec : set or query the default fg / bg / cursor. */
static void
osc_dyncolor(struct dv_window *w, unsigned which, const char *b, size_t n)
{
	struct dv_screen	*s = &w->screen;
	uint32_t		 col;

	if (n == 1 && b[0] == '?') {
		char	c[32], rep[64];
		int	rn;

		fmt_rgb(pal_dynamic(s, which), c, sizeof c);
		rn = snprintf(rep, sizeof rep, "\033]%u;%s\033\\", which, c);
		if (rn > 0)
			vt_reply(w, rep);
		return;
	}
	if (!parse_color(b, n, &col))
		return;
	if (which == 10) {
		s->pal.fg = col;
		s->pal.fgset = 1;
	} else if (which == 11) {
		s->pal.bg = col;
		s->pal.bgset = 1;
	} else {
		s->pal.cursor = col;
		s->pal.curset = 1;
		if (w->session != nullptr && w->session->desktop.focused == w)
			dv_window_apply_cursor_color(w);
	}
	s->pal.any = 1;
}

static void
osc_dyncolor_reset(struct dv_window *w, unsigned which)
{
	struct dv_screen	*s = &w->screen;

	if (which == 110)
		s->pal.fgset = 0;
	else if (which == 111)
		s->pal.bgset = 0;
	else {
		s->pal.curset = 0;
		if (w->session != nullptr && w->session->desktop.focused == w)
			dv_window_apply_cursor_color(w);
	}
	pal_recompute(&s->pal);
}

/*
 * OSC 21 (kitty colour control): key=value pairs separated by ';'. A
 * value of ? queries, an empty or missing value resets, anything else
 * sets. Keys: 0-255, foreground, background, cursor; other keys are
 * accepted and answered empty.
 */
static void
osc21(struct dv_window *w, const char *b, size_t n)
{
	struct dv_screen	*s = &w->screen;
	struct dv_buf		 rep;
	size_t			 i = 0;
	int			 nq = 0;

	memset(&rep, 0, sizeof rep);
	dv_buf_append(&rep, "\033]21", 4);
	while (i < n) {
		size_t		ks = i, ke, vs, ve;
		unsigned	idx = 0;
		int		isnum = 1, have_val = 0;

		while (i < n && b[i] != ';' && b[i] != '=')
			i++;
		ke = i;
		if (i < n && b[i] == '=') {
			have_val = 1;
			i++;
		}
		vs = i;
		while (i < n && b[i] != ';')
			i++;
		ve = i;
		if (i < n)
			i++;
		if (ke == ks)
			continue;
		for (size_t k = ks; k < ke; k++)
			if (b[k] < '0' || b[k] > '9')
				isnum = 0;
			else
				idx = idx * 10 + (unsigned)(b[k] - '0');
		if (have_val && ve - vs == 1 && b[vs] == '?') {	/* query */
			char	c[32];

			dv_buf_append(&rep, ";", 1);
			dv_buf_append(&rep, b + ks, ke - ks);
			dv_buf_append(&rep, "=", 1);
			if (isnum && idx < 256)
				fmt_rgb(pal_index(s, idx), c, sizeof c);
			else if (ke - ks == 10 && memcmp(b + ks, "foreground", 10) == 0)
				fmt_rgb(pal_dynamic(s, 10), c, sizeof c);
			else if (ke - ks == 10 && memcmp(b + ks, "background", 10) == 0)
				fmt_rgb(pal_dynamic(s, 11), c, sizeof c);
			else if (ke - ks == 6 && memcmp(b + ks, "cursor", 6) == 0)
				fmt_rgb(pal_dynamic(s, 12), c, sizeof c);
			else
				c[0] = '\0';
			dv_buf_append(&rep, c, strlen(c));
			nq++;
			continue;
		}
		if (!have_val || ve == vs) {			/* reset */
			if (isnum && idx < 256)
				s->pal.set[idx] = 0;
			else if (ke - ks == 10 && memcmp(b + ks, "foreground", 10) == 0)
				s->pal.fgset = 0;
			else if (ke - ks == 10 && memcmp(b + ks, "background", 10) == 0)
				s->pal.bgset = 0;
			else if (ke - ks == 6 && memcmp(b + ks, "cursor", 6) == 0)
				osc_dyncolor_reset(w, 112);
			pal_recompute(&s->pal);
			continue;
		}
		{						/* set */
			uint32_t	col;

			if (!parse_color(b + vs, ve - vs, &col))
				continue;
			if (isnum && idx < 256) {
				s->pal.idx[idx] = col;
				s->pal.set[idx] = 1;
				s->pal.any = 1;
			} else if (ke - ks == 10 && memcmp(b + ks, "foreground", 10) == 0)
				osc_dyncolor(w, 10, b + vs, ve - vs);
			else if (ke - ks == 10 && memcmp(b + ks, "background", 10) == 0)
				osc_dyncolor(w, 11, b + vs, ve - vs);
			else if (ke - ks == 6 && memcmp(b + ks, "cursor", 6) == 0)
				osc_dyncolor(w, 12, b + vs, ve - vs);
		}
	}
	if (nq > 0) {
		dv_buf_append(&rep, "\033\\", 2);
		dv_window_pty_send(w, rep.data, rep.len);
	}
	dv_buf_free(&rep);
}

/* ---- OSC 22 mouse pointer shapes: a per-window stack ---- */

/* Tell the host this window's current pointer shape (default if none). */
void
dv_window_apply_pointer(struct dv_window *w)
{
	char	buf[80];
	int	n;

	if (w->npshape > 0)
		n = snprintf(buf, sizeof buf, "\033]22;%s\033\\",
		    w->pshape[w->npshape - 1]);
	else
		n = snprintf(buf, sizeof buf, "\033]22;\033\\");
	if (n > 0)
		dv_buf_append(&w->host_out, buf, (size_t)n);
}

static void
pshape_push(struct dv_window *w, const char *nm, size_t n)
{
	if (n == 0 || n >= sizeof w->pshape[0])
		return;
	if (w->npshape >= 16) {			/* full: drop the oldest */
		memmove(w->pshape[0], w->pshape[1], sizeof w->pshape[0] * 15);
		w->npshape = 15;
	}
	memcpy(w->pshape[w->npshape], nm, n);
	w->pshape[w->npshape][n] = '\0';
	w->npshape++;
}

static void
osc22(struct dv_window *w, const char *b, size_t n)
{
	char	c = n ? b[0] : 0;

	if (c == '?') {					/* query: the host knows */
		forward_osc(w);
		dv_route_expect(w, 22, "", 0);
		return;
	}
	if (c == '<') {					/* pop */
		if (w->npshape > 0)
			w->npshape--;
	} else if (c == '>') {				/* push a comma list */
		size_t	i, st = 1;

		for (i = 1; i <= n; i++)
			if (i == n || b[i] == ',') {
				pshape_push(w, b + st, i - st);
				st = i + 1;
			}
	} else {					/* set, or reset when empty */
		if (c == '=') {
			b++;
			n--;
		}
		if (n == 0)
			w->npshape = 0;
		else {
			if (w->npshape == 0)
				w->npshape = 1;
			if (n >= sizeof w->pshape[0])
				n = sizeof w->pshape[0] - 1;
			memcpy(w->pshape[w->npshape - 1], b, n);
			w->pshape[w->npshape - 1][n] = '\0';
		}
	}
	if (w->session != nullptr && w->session->desktop.focused == w)
		dv_window_apply_pointer(w);
}

/*
 * A complete OSC from the program. The number selects: window title
 * (0/2), colours (4/104/10-12/110-112/21, per-window), pointer shape
 * (22), clipboard (52 and kitty's 5522), text sizing (66), notifications
 * (99), file transfer (5113, and goblin-download 777), colour push/pop
 * (30001/30101). Anything that only the real terminal can do is forwarded
 * verbatim and its reply routed back to this window.
 */
static void
osc_done(struct dv_window *w)
{
	const char	*t = (const char *)w->cap.data, *body;
	size_t		 n = w->cap.len, k = 0, bn, ml;
	unsigned	 num = 0;
	char		 id[64];
	size_t		 idl;

	if (n == 0)
		return;
	while (k < n && t[k] >= '0' && t[k] <= '9') {
		num = num * 10 + (unsigned)(t[k] - '0');
		k++;
	}
	body = (k < n && t[k] == ';') ? t + k + 1 : t + k;
	bn = (size_t)(t + n - body);
	for (ml = 0; ml < bn && body[ml] != ';'; ml++)	/* metadata segment */
		;

	switch (num) {
	case 0:
	case 2:					/* window title */
		if (k == 1 && n >= 2 && t[1] == ';') {
			char	buf[64];
			size_t	l = n - 2 < sizeof buf - 1 ? n - 2 : sizeof buf - 1;

			if (w->title_locked)
				return;
			memcpy(buf, t + 2, l);
			buf[l] = '\0';
			dv_strlcpy(w->title, buf, sizeof w->title);
		}
		return;
	case 4:
		osc4(w, body, bn);
		return;
	case 104:
		osc104(w, body, bn);
		return;
	case 10:
	case 11:
	case 12:
		osc_dyncolor(w, num, body, bn);
		return;
	case 110:
	case 111:
	case 112:
		osc_dyncolor_reset(w, num);
		return;
	case 21:
		osc21(w, body, bn);
		return;
	case 22:
		osc22(w, body, bn);
		return;
	case 30001:				/* push the colour state */
		if (w->screen.npal < 4)
			w->screen.palstack[w->screen.npal++] = w->screen.pal;
		return;
	case 30101:				/* pop it */
		if (w->screen.npal > 0) {
			w->screen.pal = w->screen.palstack[--w->screen.npal];
			if (w->session != nullptr && w->session->desktop.focused == w)
				dv_window_apply_cursor_color(w);
		}
		return;
	case 52: {
		/*
		 * OSC 52 (clipboard). Hand it to the host verbatim, so the real
		 * terminal -- or an outer multiplexer we are nested in -- sets
		 * the system clipboard. "52;<sel>;?" is a READ the host answers.
		 */
		size_t	j;

		for (j = 3; j < n && t[j] != ';'; j++)
			;
		if (j + 1 < n && t[j + 1] == '?')
			w->clip_query = 1;
		forward_osc(w);
		return;
	}
	case 66:
		osc66(w, t, n);
		return;
	case 99:				/* desktop notification: the host shows it */
		idl = dv_meta_val(body, ml, "i", id, sizeof id);
		forward_osc(w);
		dv_route_expect(w, 99, id, idl);
		return;
	case 5522:				/* kitty clipboard: the host's clipboard */
		idl = dv_meta_val(body, ml, "id", id, sizeof id);
		forward_osc(w);
		dv_route_expect(w, 5522, id, idl);
		return;
	case 133:				/* shell prompt mark: A resets this pane */
		if (bn > 0 && body[0] == 'A')
			dv_silkie_reset_pane(w);
		return;
    case 777: { /* namespaced Goblin extensions */
        if (dv_silkie_program(w, t, n))
            return;
        auto *s = &w->screen;
        goblin::pointer::Message m;
        const auto body_view = std::string_view(reinterpret_cast<const char *>(t), n);
        if (body_view.substr(0,goblin::pointer::prefix.size()) == goblin::pointer::prefix) {
            if (!goblin::pointer::parse(body_view,m)) return;
            if (m.op == goblin::pointer::Op::Mode) {
                if (s->pointer_mode != m.mode) dv_pointer_cancel(w);
                s->pointer_mode = m.mode;
            }
            else if (m.op == goblin::pointer::Op::Query) {
                unsigned cw, ch; cell_pixels(w,&cw,&ch);
                m.op = goblin::pointer::Op::Cap;
                m.caps = w->session ? w->session->pointer_caps : 0;
                m.cw = (int)cw; m.ch = (int)ch;
                m.w = (int)std::min(65535u,s->cols*cw); m.h = (int)std::min(65535u,s->rows*ch);
                const auto reply = goblin::pointer::encode(m);
                dv_window_pty_send(w,reply.data(),reply.size());
            }
            return;
        }
        dv_xfer_osc(w,num,body,bn); return;
    }
	case 5113:				/* kitty file transfer */
		dv_xfer_osc(w, num, body, bn);	/* a host that speaks it, or us */
		return;
	case 72: {				/* drag and drop */
		char	tv[8];

		dv_meta_val(body, ml, "t", tv, sizeof tv);
		if (tv[0] == 'a') {		/* the program takes drops */
			size_t	l = bn > ml + 1 ? bn - ml - 1 : 0;

			if (l >= sizeof w->dnd_types)
				l = sizeof w->dnd_types - 1;
			memcpy(w->dnd_types, body + ml + 1, l);
			w->dnd_types[l] = '\0';
			w->dnd_accept = 1;
		} else if (tv[0] == 'A') {
			w->dnd_accept = 0;
			w->dnd_types[0] = '\0';
		}
		if (dv_dnd_app_hook != nullptr)	/* the server relays the rest */
			dv_dnd_app_hook(w, body, bn);
		return;
	}
	default:
		return;				/* not ours: dropped, not relayed */
	}
}

/*
 * A kitty graphics APC (ESC _ G ... ST) from the inner program. We keep
 * the raw bytes so the compositor can replay them to the host at the
 * window's position -- the multiplexer does not decode the image. A
 * control part carrying "f=" starts a new transmission (reset); chunk
 * continuations (m=1) append.
 */
static void
apc_done(struct dv_window *w)
{
	/*
	 * The window is a kitty endpoint: hand the APC to the graphics
	 * store, which reassembles it and answers the program. The bytes
	 * are not forwarded to the host -- goblin-view re-emits its own (kitty.cc).
	 */
	if (w->cap.len >= 1 && w->cap.data[0] == 'G')
		dv_kitty_apc(w, w->cap.data, w->cap.len);
}

static void
execute_c0(struct dv_screen *s, unsigned char b)
{
	switch (b) {
	case '\r':
		s->cc = 0;
		break;
	case '\n':
	case 0x0b:
	case 0x0c:
		index_down(s);
		break;
	case '\b':
		if (s->cc >= s->cols)
			s->cc = s->cols - 1;
		if (s->cc > 0)
			s->cc--;
		break;
	case '\t':
		s->cc = (s->cc + 8) & ~7u;
		if (s->cc >= s->cols)
			s->cc = s->cols - 1;
		break;
	case 0x0e:
		s->shift = 1;
		break;
	case 0x0f:
		s->shift = 0;
		break;
	default:
		break;
	}
}

static int
csi_grow(struct dv_screen *s)
{
	unsigned	 cap = s->param_cap ? s->param_cap * 2u : 16u;
	int		*params;
	uint8_t		*sub;

	if (cap < (unsigned)s->np + 1)
		cap = (unsigned)s->np + 1;
	params = static_cast<int *>(realloc(s->params, cap * sizeof *params));
	if (params == nullptr)
		return (0);
	s->params = params;
	sub = static_cast<uint8_t *>(realloc(s->sub, cap));
	if (sub == nullptr)
		return (0);
	memset(sub + s->param_cap, 0, cap - s->param_cap);
	s->sub = sub;
	s->param_cap = cap;
	return (1);
}

static void
csi_start(struct dv_screen *s)
{
	s->st = ST_CSI;
	s->prefix = 0;
	s->inter = 0;
	s->np = 0;
	s->val = -1;
	s->csi_bytes = 0;
	s->csi_drop = 0;
	s->next_sub = 0;
	if (s->sub != nullptr && s->param_cap)
		memset(s->sub, 0, s->param_cap);
}

static void
csi_push(struct dv_screen *s, int is_sub)
{
	if (!s->csi_drop) {
		if ((unsigned)s->np >= s->param_cap && !csi_grow(s))
			s->csi_drop = 1;
		else {
			s->params[s->np] = s->val < 0 ? 0 : s->val;
			s->sub[s->np] = (uint8_t)is_sub;
			s->np++;
		}
	}
	s->val = -1;
}

/* A terminfo capability a window's program can count on. */
static const char *
tcap_value(const char *name, int *is_bool)
{
	static const struct { const char *nm, *val; int b; } caps[] = {
		{ "Tc", "", 1 },		/* true colour */
		{ "RGB", "", 1 },
		{ "Su", "", 1 },		/* styled underlines */
		{ "colors", "256", 0 },
		{ "Co", "256", 0 },
		{ "setrgbf", "\033[38:2::%p1%d:%p2%d:%p3%dm", 0 },
		{ "setrgbb", "\033[48:2::%p1%d:%p2%d:%p3%dm", 0 },
		{ "Smulx", "\033[4:%p1%dm", 0 },
		{ "Setulc", "\033[58:2::%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d:"
		    "%p1%{255}%&%dm", 0 },
		{ "Ms", "\033]52;%p1%s;%p2%s\033\\", 0 },
	};
	size_t	i;

	*is_bool = 0;
	if (strcmp(name, "TN") == 0 || strcmp(name, "name") == 0) {
		const char	*term = getenv("GOBLIN_VIEW_TERM");

		return (term != nullptr && term[0] != '\0' ? term : "xterm-256color");
	}
	for (i = 0; i < sizeof caps / sizeof caps[0]; i++)
		if (strcmp(caps[i].nm, name) == 0) {
			*is_bool = caps[i].b;
			return (caps[i].val);
		}
	return (nullptr);
}

static int
hexnib(char c)
{
	if (c >= '0' && c <= '9')
		return (c - '0');
	if (c >= 'a' && c <= 'f')
		return (c - 'a' + 10);
	if (c >= 'A' && c <= 'F')
		return (c - 'A' + 10);
	return (-1);
}

static void
xtgettcap(struct dv_window *w, const char *q, size_t n)
{
	size_t	i = 0;

	while (i <= n) {
		size_t		st = i, j, k;
		char		name[64], hexname[130], rep[640];
		const char	*val;
		int		is_bool, bad = 0;

		while (i < n && q[i] != ';')
			i++;
		if (i - st == 0 || i - st > sizeof hexname - 2 || (i - st) % 2) {
			i++;
			continue;
		}
		memcpy(hexname, q + st, i - st);
		hexname[i - st] = '\0';
		for (j = st, k = 0; j + 1 < i && k + 1 < sizeof name; j += 2) {
			int	hi = hexnib(q[j]), lo = hexnib(q[j + 1]);

			if (hi < 0 || lo < 0)
				bad = 1;
			name[k++] = (char)(hi * 16 + lo);
		}
		name[k] = '\0';
		val = bad ? nullptr : tcap_value(name, &is_bool);
		if (val == nullptr)
			snprintf(rep, sizeof rep, "\033P0+r%s\033\\", hexname);
		else if (is_bool)
			snprintf(rep, sizeof rep, "\033P1+r%s\033\\", hexname);
		else {
			size_t	o = 0, m;
			char	hexval[520];

			for (m = 0; val[m] != '\0' && o + 2 < sizeof hexval; m++)
				o += (size_t)snprintf(hexval + o, sizeof hexval - o, "%02x",
				    (unsigned char)val[m]);
			snprintf(rep, sizeof rep, "\033P1+r%s=%s\033\\", hexname, hexval);
		}
		vt_reply(w, rep);
		i++;
	}
}

static void
dcs_done(struct dv_window *w)
{
	const char	*t = (const char *)w->cap.data;
	size_t		 i, n = w->cap.len;

	/* XTGETTCAP (ESC P + q hexname ; hexname ... ST): what a program
	 * may rely on here, one reply per name; unknown names are said to
	 * be unknown so a prompt probing terminfo never blocks. */
	if (n >= 2 && t[0] == '+' && t[1] == 'q') {
		xtgettcap(w, t + 2, n - 2);
		return;
	}
	/*
	 * Sixel: DCS P1 ; P2 ; P3 q data. The window decodes it and shows
	 * it as an image of its own -- only when the host takes kitty
	 * graphics, which is also the only time DA1 offered sixel.
	 */
	for (i = 0; i < n && ((t[i] >= '0' && t[i] <= '9') || t[i] == ';'); i++)
		;
	if (i < n && t[i] == 'q' && dv_kitty_host_gfx(w)) {
		unsigned char	*rgba = nullptr;
		unsigned	 wpx = 0, hpx = 0;

		if (dv_sixel_decode((const unsigned char *)t + i + 1, n - i - 1,
		    &rgba, &wpx, &hpx)) {
			dv_kitty_place_rgba(w, rgba, wpx, hpx, w->screen.sixel_dm,
			    w->screen.sixel_right);
			free(rgba);
		}
	}
}

static void
str_start(struct dv_window *w, int kind)
{
	struct dv_screen	*s = &w->screen;

	s->st = kind == STR_OSC ? ST_OSC : ST_STR;
	s->str_kind = kind;
	s->slen = 0;
	s->str_esc = 0;
	s->str_drop = 0;
	w->cap.len = 0;
}

static void
str_dispatch(struct dv_window *w)
{
	struct dv_screen	*s = &w->screen;

	if (!s->str_drop) {
		if (s->st == ST_OSC)
			osc_done(w);
		else if (s->str_kind == STR_APC)
			apc_done(w);
		else if (s->str_kind == STR_DCS)
			dcs_done(w);
	}
	s->str_drop = 0;
}

/* A line feed with the screen's scrolling, for a placement's cursor
 * advance (kitty.cc). */
void
dv_screen_linefeed(struct dv_screen *s)
{
	index_down(s);
}

/*
 * Kitty's row/column diacritics: the Nth code point encodes the value N
 * on a Unicode-placeholder cell. Published table (rowcolumn_diacritics).
 */
static const uint32_t placeholder_diacritics[] = {
	0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F,
	0x0346, 0x034A, 0x034B, 0x034C, 0x0350, 0x0351, 0x0352, 0x0357,
	0x035B, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369,
	0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x0483, 0x0484,
	0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597,
	0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1,
	0x05A8, 0x05A9, 0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610, 0x0611,
	0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658,
	0x0659, 0x065A, 0x065B, 0x065D, 0x065E, 0x06D6, 0x06D7, 0x06D8,
	0x06D9, 0x06DA, 0x06DB, 0x06DC, 0x06DF, 0x06E0, 0x06E1, 0x06E2,
	0x06E4, 0x06E7, 0x06E8, 0x06EB, 0x06EC, 0x0730, 0x0732, 0x0733,
	0x0735, 0x0736, 0x073A, 0x073D, 0x073F, 0x0740, 0x0741, 0x0743,
	0x0745, 0x0747, 0x0749, 0x074A, 0x07EB, 0x07EC, 0x07ED, 0x07EE,
	0x07EF, 0x07F0, 0x07F1, 0x07F3, 0x0816, 0x0817, 0x0818, 0x0819,
	0x081B, 0x081C, 0x081D, 0x081E, 0x081F, 0x0820, 0x0821, 0x0822,
	0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082A, 0x082B, 0x082C,
	0x082D, 0x0951, 0x0953, 0x0954, 0x0F82, 0x0F83, 0x0F86, 0x0F87,
	0x135D, 0x135E, 0x135F, 0x17DD, 0x193A, 0x1A17, 0x1A75, 0x1A76,
	0x1A77, 0x1A78, 0x1A79, 0x1A7A, 0x1A7B, 0x1A7C, 0x1B6B, 0x1B6D,
	0x1B6E, 0x1B6F, 0x1B70, 0x1B71, 0x1B72, 0x1B73, 0x1CD0, 0x1CD1,
	0x1CD2, 0x1CDA, 0x1CDB, 0x1CE0, 0x1DC0, 0x1DC1, 0x1DC3, 0x1DC4,
	0x1DC5, 0x1DC6, 0x1DC7, 0x1DC8, 0x1DC9, 0x1DCB, 0x1DCC, 0x1DD1,
	0x1DD2, 0x1DD3, 0x1DD4, 0x1DD5, 0x1DD6, 0x1DD7, 0x1DD8, 0x1DD9,
	0x1DDA, 0x1DDB, 0x1DDC, 0x1DDD, 0x1DDE, 0x1DDF, 0x1DE0, 0x1DE1,
	0x1DE2, 0x1DE3, 0x1DE4, 0x1DE5, 0x1DE6, 0x1DFE, 0x20D0, 0x20D1,
	0x20D4, 0x20D5, 0x20D6, 0x20D7, 0x20DB, 0x20DC, 0x20E1, 0x20E7,
	0x20E9, 0x20F0, 0x2CEF, 0x2CF0, 0x2CF1, 0x2DE0, 0x2DE1, 0x2DE2,
	0x2DE3, 0x2DE4, 0x2DE5, 0x2DE6, 0x2DE7, 0x2DE8, 0x2DE9, 0x2DEA,
	0x2DEB, 0x2DEC, 0x2DED, 0x2DEE, 0x2DEF, 0x2DF0, 0x2DF1, 0x2DF2,
	0x2DF3, 0x2DF4, 0x2DF5, 0x2DF6, 0x2DF7, 0x2DF8, 0x2DF9, 0x2DFA,
	0x2DFB, 0x2DFC, 0x2DFD, 0x2DFE, 0x2DFF, 0xA66F, 0xA67C, 0xA67D,
	0xA6F0, 0xA6F1, 0xA8E0, 0xA8E1, 0xA8E2, 0xA8E3, 0xA8E4, 0xA8E5,
	0xA8E6, 0xA8E7, 0xA8E8, 0xA8E9, 0xA8EA, 0xA8EB, 0xA8EC, 0xA8ED,
	0xA8EE, 0xA8EF, 0xA8F0, 0xA8F1, 0xAAB0, 0xAAB2, 0xAAB3, 0xAAB7,
	0xAAB8, 0xAABE, 0xAABF, 0xAAC1, 0xFE20, 0xFE21, 0xFE22, 0xFE23,
	0xFE24, 0xFE25, 0xFE26, 0x10A0F, 0x10A38, 0x1D185, 0x1D186,
	0x1D187, 0x1D188, 0x1D189, 0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD,
	0x1D242, 0x1D243, 0x1D244,
};
#define N_DIACRITICS	((unsigned)(sizeof placeholder_diacritics / \
			    sizeof placeholder_diacritics[0]))

uint32_t
dv_placeholder_diacritic(unsigned n)
{
	if (n >= N_DIACRITICS)
		n = N_DIACRITICS - 1;
	return (placeholder_diacritics[n]);
}

int
dv_placeholder_index(uint32_t cp)
{
	unsigned	i;

	for (i = 0; i < N_DIACRITICS; i++)
		if (placeholder_diacritics[i] == cp)
			return ((int)i);
	return (-1);
}

void
dv_vt_write(struct dv_window *w, const void *buf, size_t n)
{
	struct dv_screen	*s = &w->screen;
	const unsigned char	*p = static_cast<const unsigned char *>(buf);
	size_t			 i;

	if (s->cells == nullptr)
		return;
	s->sb_off = 0;			/* new output jumps back to the bottom */
	for (i = 0; i < n; i++) {
		unsigned char	b = p[i];

		/* A multibyte character in progress. */
		if (s->uneed > 0) {
			if ((b & 0xc0) == 0x80) {
				s->ucp = (s->ucp << 6) | (b & 0x3f);
				if (--s->uneed == 0 && s->st == ST_GROUND)
					put_ch(s, s->ucp);
				continue;
			}
			s->uneed = 0;	/* broken sequence: reprocess b */
		}

		if (s->st == ST_OSC || s->st == ST_STR) {
			if (s->str_esc) {
				s->str_esc = 0;
				if (b == '\\') {
					str_dispatch(w);
					s->st = ST_GROUND;
					continue;
				}
				/* ESC + other: the string ended abnormally */
				str_dispatch(w);
				s->st = ST_ESC;
				/* fall through to the ESC handler below */
			} else if (b == 0x1b) {
				s->str_esc = 1;
				continue;
			} else if (b == 0x07) {
				str_dispatch(w);
				s->st = ST_GROUND;
				continue;
			} else {
				/* OSC, APC and DCS. escape-limit 0 keeps every byte.
				 * Past a configured limit the rest is discarded and
				 * the sequence is not acted on. */
				if (s->st == ST_OSC || s->str_kind == STR_APC ||
				    s->str_kind == STR_DCS) {
					if (dv_escape_limit &&
					    w->cap.len >= dv_escape_limit)
						s->str_drop = 1;
					if (!s->str_drop)
						dv_buf_append(&w->cap, &b, 1);
				}
				continue;
			}
		}

		if (b == 0x1b) {
			s->st = ST_ESC;
			continue;
		}
		if (b < 0x20) {
			execute_c0(s, b);
			continue;
		}
		if (b == 0x7f)
			continue;

		switch (s->st) {
		case ST_GROUND:
			if (b < 0x80) {
				put_ch(s, b);
			} else if ((b & 0xe0) == 0xc0) {
				s->ucp = b & 0x1f;
				s->uneed = 1;
			} else if ((b & 0xf0) == 0xe0) {
				s->ucp = b & 0x0f;
				s->uneed = 2;
			} else if ((b & 0xf8) == 0xf0) {
				s->ucp = b & 0x07;
				s->uneed = 3;
			}
			break;
		case ST_ESC:
			switch (b) {
			case '[':
				csi_start(s);
				break;
			case ']':
				str_start(w, STR_OSC);
				break;
			case 'P':
				str_start(w, STR_DCS);
				break;
			case '_':
				str_start(w, STR_APC);
				break;
			case '^':
				str_start(w, STR_PM);
				break;
			case 'X':
				str_start(w, STR_SOS);
				break;
			case '(':
			case ')':
			case '*':
			case '+':
			case '#':
			case '%':
			case ' ':
				s->prefix = b;
				s->st = ST_ESC2;
				break;
			default:
				if (b == 'c') {
					dv_pointer_cancel(w);
					dv_silkie_cancel_window(w);
				}
				do_esc(s, b);
				if (b == 'c')
					dv_silkie_push(w->session);
				s->st = ST_GROUND;
				break;
			}
			break;
		case ST_ESC2:
			if (s->prefix == '(')
				s->charset[0] = (b == '0');
			else if (s->prefix == ')')
				s->charset[1] = (b == '0');
			else if (s->prefix == '#' && b == '8') {
				unsigned	x, y;

				for (y = 0; y < s->rows; y++)
					for (x = 0; x < s->cols; x++) {
						struct dv_cell *c = slot(s, x, y);

						cell_blank(c, DV_COLOR_DEFAULT,
						    DV_COLOR_DEFAULT);
						c->ch = U'E';
					}
			}
			s->st = ST_GROUND;
			break;
		case ST_CSI:
			s->csi_bytes++;
			if (dv_csi_limit && s->csi_bytes > dv_csi_limit)
				s->csi_drop = 1;
			if (b >= '0' && b <= '9') {
				if (!s->csi_drop) {
					if (s->val < 0)
						s->val = 0;
					if (s->val > (INT_MAX - 9) / 10)
						s->val = INT_MAX;
					else
						s->val = s->val * 10 + (b - '0');
				}
			} else if (b == ';' || b == ':') {
				csi_push(s, s->next_sub);
				s->next_sub = (b == ':');
			} else if (b == '?' || b == '>' || b == '<' || b == '=') {
				if (s->np == 0 && s->val < 0 && !s->csi_drop)
					s->prefix = b;
			} else if (b >= 0x20 && b <= 0x2f) {
				/* intermediate: $ * + SP ! ... (DECSTR "!p",
				 * DECCARA "$r", DECSACE "*x", unscroll "+T"). */
				if (!s->csi_drop) {
					s->inter = b;
					if (s->prefix == 0)
						s->prefix = b;
				}
			} else if (b >= 0x40 && b <= 0x7e) {
				if (!s->csi_drop) {
					if (s->val >= 0 || s->np > 0)
						csi_push(s, s->next_sub);
					if (!s->csi_drop)
						do_csi(w, b);
				}
				s->next_sub = 0;
				s->csi_drop = 0;
				s->st = ST_GROUND;
			} else {
				s->csi_drop = 0;
				s->st = ST_GROUND;
			}
			break;
		default:
			s->st = ST_GROUND;
			break;
		}
	}
	dv_silkie_output(w);
}
