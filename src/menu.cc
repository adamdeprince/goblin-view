/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * The menu. It is painted by the compositor as one more layer on
 * the desktop, so it sits on top of the windows
 * and nothing underneath has to be repainted when it closes. The
 * server owns the open/closed state (struct dv_session.menu_open); the
 * client only reports the menu tap.
 */

#include "dv.hpp"

#include <string.h>
#include <strings.h>

/*
 * key == NULL is a blank separator row; key == "" is a full-width note
 * line (no highlighted key); otherwise key is highlighted before label.
 */
static const struct dv_menu_item menu_items[] = {
	{ "O",   "Open Window" },
	{ "C",   "Close Window" },
	{ "Z",   "Zoom (toggle full screen)" },
	{ "R",   "Rearrange (move / resize)" },
	{ "1-9", "Select window  (mouse: any)" },
	{ "U",   "Unicode keyboard" },
	{ "I",   "Input method (pinyin, romaji...)" },
	{ "?",   "Help: keys, mouse, Ghostty setup" },
	{ "D",   "Detach (leave it all running)" },
	{ "M T", "Mark (copy mode) / Transfer the mark" },
	{ "H S", "Hide window / Switch window (a list)" },
	{ "B",   "Broadcast typing to every window" },
	{ "Q",   "Quit goblin-view (close everything)" },
	{ NULL,  NULL },
	{ "",    "Move:   R M, then arrows (Shift=5)" },
	{ "",    "Resize: R R, then arrows, Enter" },
	{ "",    "Mouse:  drag frame=move, corner=resize" },
	{ NULL,  NULL },
	{ "Esc", "Cancel" },
};

#define MENU_NITEMS	((unsigned)(sizeof menu_items / sizeof menu_items[0]))
#define MENU_H		(MENU_NITEMS + 3)	/* top, blank, items, bottom */

#define MENU_FG		7u
#define MENU_BG		4u
#define MENU_BORDER	15u
#define MENU_KEY	11u

const struct dv_menu_item *
dv_menu_items(unsigned *n)
{
	*n = MENU_NITEMS;
	return (menu_items);
}

unsigned
dv_menu_height(void)
{
	return (MENU_H);
}

int
dv_menu_box_fits(unsigned cols, unsigned rows)
{
	return (cols >= DV_MENU_W + 2 && rows >= MENU_H + 1);
}

static void
put(struct dv_cell *desk, unsigned W, unsigned H, unsigned x, unsigned y,
    uint32_t ch, uint32_t fg, uint32_t bg, uint8_t attr)
{
	struct dv_cell	*c;

	if (x >= W || y >= H)
		return;
	c = &desk[y * W + x];
	c->ch = ch;
	c->fg = fg;
	c->bg = bg;
	c->attr = attr;
	c->ustyle = 0;
	c->ucolor = DV_COLOR_DEFAULT;
}

static uint32_t
utf8_next(const char **sp)
{
	const unsigned char	*s = (const unsigned char *)*sp;
	uint32_t		 cp;
	int			 need;

	if (s[0] < 0x80) {
		cp = s[0];
		need = 0;
	} else if ((s[0] & 0xe0) == 0xc0) {
		cp = s[0] & 0x1f;
		need = 1;
	} else if ((s[0] & 0xf0) == 0xe0) {
		cp = s[0] & 0x0f;
		need = 2;
	} else {
		cp = s[0] & 0x07;
		need = 3;
	}
	s++;
	while (need-- > 0 && (*s & 0xc0) == 0x80)
		cp = (cp << 6) | (*s++ & 0x3f);
	*sp = (const char *)s;
	return (cp);
}

/*
 * Paint a string. A byte 0x01 switches to the key colour and 0x02 back,
 * so "\001O\002 Open Window" highlights the O.
 */
static unsigned
text(struct dv_cell *desk, unsigned W, unsigned H, unsigned x, unsigned y,
    const char *s, uint32_t fg, uint32_t bg, uint8_t attr, unsigned max)
{
	unsigned	 n = 0;
	uint32_t	 cur = fg;
	uint8_t		 a = attr;

	while (*s != '\0' && n < max) {
		uint32_t	cp;

		if (*s == '\001') {
			cur = MENU_KEY;
			a = (uint8_t)(attr | DV_ATTR_BOLD);
			s++;
			continue;
		}
		if (*s == '\002') {
			cur = fg;
			a = attr;
			s++;
			continue;
		}
		cp = utf8_next(&s);
		put(desk, W, H, x + n, y, cp, cur, bg, a);
		n++;
	}
	return (n);
}

static void
paint_box(struct dv_cell *desk, unsigned W, unsigned H)
{
	unsigned	x0 = (W - DV_MENU_W) / 2;
	unsigned	y0 = (H - MENU_H) / 2;
	unsigned	x, y, i;
	char		line[96];

	for (y = 0; y < MENU_H; y++)
		for (x = 0; x < DV_MENU_W; x++)
			put(desk, W, H, x0 + x, y0 + y, U' ', MENU_FG, MENU_BG, 0);

	for (x = 1; x + 1 < DV_MENU_W; x++) {
		put(desk, W, H, x0 + x, y0, 0x2550, MENU_BORDER, MENU_BG, 0);
		put(desk, W, H, x0 + x, y0 + MENU_H - 1, 0x2550, MENU_BORDER,
		    MENU_BG, 0);
	}
	for (y = 1; y + 1 < MENU_H; y++) {
		put(desk, W, H, x0, y0 + y, 0x2551, MENU_BORDER, MENU_BG, 0);
		put(desk, W, H, x0 + DV_MENU_W - 1, y0 + y, 0x2551, MENU_BORDER,
		    MENU_BG, 0);
	}
	put(desk, W, H, x0, y0, 0x2554, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0 + DV_MENU_W - 1, y0, 0x2557, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0, y0 + MENU_H - 1, 0x255A, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0 + DV_MENU_W - 1, y0 + MENU_H - 1, 0x255D,
	    MENU_BORDER, MENU_BG, 0);
	text(desk, W, H, x0 + 2, y0, " GoblinView ", MENU_BORDER, MENU_BG,
	    DV_ATTR_BOLD, DV_MENU_W - 4);

	for (i = 0; i < MENU_NITEMS; i++) {
		const struct dv_menu_item	*it = &menu_items[i];

		if (it->key == NULL)
			continue;
		if (it->key[0] == '\0')		/* a note line, dimmed */
			text(desk, W, H, x0 + 3, y0 + 2 + i, it->label, MENU_FG,
			    MENU_BG, DV_ATTR_DIM, DV_MENU_W - 4);
		else {
			snprintf(line, sizeof line, "\001%-3s\002  %s", it->key,
			    it->label);
			text(desk, W, H, x0 + 2, y0 + 2 + i, line, MENU_FG,
			    MENU_BG, 0, DV_MENU_W - 3);
		}
	}
}

static void
paint_bar(struct dv_cell *desk, unsigned W, unsigned H)
{
	unsigned	x, n;

	for (x = 0; x < W; x++)
		put(desk, W, H, x, H - 1, U' ', MENU_FG, MENU_BG, 0);
	n = text(desk, W, H, 0, H - 1, " Menu ", MENU_BG, MENU_BORDER,
	    DV_ATTR_BOLD, W);
	text(desk, W, H, n, H - 1,
	    "  \001O\002pen \001C\002lose \001Z\002oom \001R\002earrange"
	    " \0011-9\002 \001D\002etach \001Q\002uit  (R M/R R + arrows,"
	    " or drag frame)", MENU_FG, MENU_BG, 0, W > n ? W - n : 0);
}

/* A small centred confirm box: a prompt the compositor draws on top. */
/* A centred box of one or more lines, for a question or a notice. */
void
dv_prompt_paint(struct dv_cell *desk, unsigned W, unsigned H,
    const char *const *lines, unsigned n)
{
	unsigned	len = 0, bw, bh, x0, y0, x, y, i;

	if (W == 0 || H == 0 || n == 0)
		return;
	for (i = 0; i < n; i++) {
		unsigned	l = (unsigned)strlen(lines[i]);

		if (l > len)
			len = l;
	}
	bw = len + 4;
	bh = n + 2;
	if (bw > W)
		bw = W;
	if (bh > H)
		bh = H;
	x0 = (W - bw) / 2;
	y0 = H > bh ? (H - bh) / 2 : 0;
	for (y = 0; y < bh; y++)
		for (x = 0; x < bw; x++) {
			uint32_t	ch = U' ';

			if (y == 0 || y == bh - 1)
				ch = 0x2550;
			if (x == 0 || x == bw - 1)
				ch = (y == 0 || y == bh - 1) ? 0x256C : 0x2551;
			put(desk, W, H, x0 + x, y0 + y, ch,
			    ch == U' ' ? MENU_FG : MENU_BORDER, MENU_BG, 0);
		}
	put(desk, W, H, x0, y0, 0x2554, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0 + bw - 1, y0, 0x2557, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0, y0 + bh - 1, 0x255A, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0 + bw - 1, y0 + bh - 1, 0x255D, MENU_BORDER, MENU_BG, 0);
	for (i = 0; i < n && i + 1 < bh - 1 + 1; i++)
		text(desk, W, H, x0 + 2, y0 + 1 + i, lines[i], MENU_FG, MENU_BG,
		    i == 0 || i == n - 1 ? DV_ATTR_BOLD : 0, bw - 3);
}

void
dv_confirm_paint(struct dv_cell *desk, unsigned W, unsigned H, const char *msg)
{
	const char *const	lines[1] = { msg };

	dv_prompt_paint(desk, W, H, lines, 1);
}

/* Does the filter match this window: its number, title or command? */
static int
switch_match(const struct dv_window *w, const char *filter)
{
	char	num[16];
	size_t	i, n = strlen(filter);

	if (n == 0)
		return (1);
	snprintf(num, sizeof num, "%u", w->id);
	if (strncmp(num, filter, n) == 0)
		return (1);
	for (i = 0; i < 2; i++) {
		const char	*hay = i == 0 ? w->title : w->cmd, *h;

		for (h = hay; *h != '\0'; h++)
			if (strncasecmp(h, filter, n) == 0)
				return (1);
	}
	return (0);
}

static const struct dv_window *
switch_find(const struct dv_session *s, unsigned id)
{
	const struct dv_window	*w;

	TAILQ_FOREACH(w, &s->desktop.windows, entry)
		if (w->id == id)
			return (w);
	return (nullptr);
}

unsigned
dv_switch_list(const struct dv_session *s, unsigned *ids, unsigned max)
{
	const struct dv_window	*w;
	unsigned		 n = 0;

	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		if (n >= max)
			break;
		if (switch_match(w, s->switch_filter))
			ids[n++] = w->id;
	}
	return (n);
}

/*
 * The switch-window list: every window of the session, one per row --
 * number, title, command, size, hidden -- with the selected row in
 * reverse video and the filter being typed on the last line.
 */
void
dv_switch_paint(struct dv_cell *desk, unsigned W, unsigned H,
    const struct dv_session *s)
{
	unsigned	 ids[256], n = dv_switch_list(s, ids, 256);
	unsigned	 bw = W < 64 ? W : 64, bh, x0, y0, x, y, i, first = 0;
	unsigned	 rows_avail = H > 6 ? H - 6 : 1, shown;

	if (W < 20 || H < 6)
		return;
	shown = n < rows_avail ? n : rows_avail;
	if ((unsigned)s->switch_sel >= first + shown && shown > 0)
		first = (unsigned)s->switch_sel - shown + 1;
	bh = shown + 4;
	x0 = (W - bw) / 2;
	y0 = (H - bh) / 2;
	for (y = 0; y < bh; y++)
		for (x = 0; x < bw; x++) {
			uint32_t	ch = U' ';

			if (y == 0 || y == bh - 1)
				ch = 0x2550;
			if (x == 0 || x == bw - 1)
				ch = (y == 0 || y == bh - 1) ? 0x256C : 0x2551;
			put(desk, W, H, x0 + x, y0 + y, ch,
			    ch == U' ' ? MENU_FG : MENU_BORDER, MENU_BG, 0);
		}
	put(desk, W, H, x0, y0, 0x2554, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0 + bw - 1, y0, 0x2557, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0, y0 + bh - 1, 0x255A, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0 + bw - 1, y0 + bh - 1, 0x255D, MENU_BORDER, MENU_BG, 0);
	text(desk, W, H, x0 + 2, y0, " Switch window: type to filter, arrows, Enter, Esc ",
	    MENU_BORDER, MENU_BG, DV_ATTR_BOLD, bw - 4);
	for (i = 0; i < shown; i++) {
		const struct dv_window	*w = switch_find(s, ids[first + i]);
		char			 line[160];
		uint8_t			 attr = (first + i == (unsigned)s->switch_sel) ?
					    DV_ATTR_REV : 0;

		if (w == nullptr)
			continue;
		snprintf(line, sizeof line, " %2u  %-24.24s %-14.14s %ux%u%s%s",
		    w->id, w->title[0] ? w->title : w->cmd, w->cmd,
		    w->geom.cols, w->geom.rows, w->hidden ? "  hidden" : "",
		    s->desktop.focused == w ? "  *" : "");
		for (x = 1; x + 1 < bw; x++)
			put(desk, W, H, x0 + x, y0 + 1 + i, U' ', MENU_FG, MENU_BG, attr);
		text(desk, W, H, x0 + 1, y0 + 1 + i, line, MENU_FG, MENU_BG, attr, bw - 2);
	}
	if (n == 0)
		text(desk, W, H, x0 + 2, y0 + 1, "(no window matches)", MENU_FG, MENU_BG,
		    0, bw - 4);
	{
		char	fl[64];

		snprintf(fl, sizeof fl, " Filter: %s_", s->switch_filter);
		text(desk, W, H, x0 + 1, y0 + bh - 2, fl, MENU_KEY, MENU_BG, DV_ATTR_BOLD,
		    bw - 2);
	}
}

void
dv_menu_paint(struct dv_cell *desk, unsigned W, unsigned H)
{
	if (W == 0 || H == 0)
		return;
	if (dv_menu_box_fits(W, H))
		paint_box(desk, W, H);
	else
		paint_bar(desk, W, H);
}

/*
 * The ? help: the keys, the mouse, and what to set in Ghostty, in one
 * box on top of everything. Any key closes it.
 */
void
dv_help_paint(struct dv_cell *desk, unsigned W, unsigned H)
{
	static const char *const lines[] = {
	    "The menu key is a tap of Alt (Option on a Mac): press, let go,",
	    "or press Ctrl-\\.  Then a letter; Esc cancels.",
	    "",
	    "O open   C close   Z zoom   R rearrange   1-9 select   D detach   Q quit",
	    "U unicode keyboard   I input method (pinyin, wubi, zhuyin, romaji...)",
	    "Move: R M, then arrows (Shift = 5), Enter.   Resize: R R, arrows, Enter.",
	    "Mouse: drag a frame to move, its corner to resize, the body to select",
	    "text (copied on release).  Double-click a title to rename the window.",
	    "",
	    "Ghostty: in ~/.config/ghostty/config add",
	    "    macos-option-as-alt = true",
	    "so an Option tap opens the menu (Cmd+Shift+, reloads the config).",
	    "Images: programs may draw with kitty graphics or sixel when the",
	    "terminal you attach from speaks kitty graphics (Ghostty, kitty).",
	    "Elsewhere goblin-view is text-only and tells programs so.",
	    "",
	    "Any key closes this help.",
	};
	unsigned	n = sizeof lines / sizeof lines[0], i, bw = 0, bh = n + 2;
	unsigned	x0, y0, x, y;

	if (W == 0 || H == 0)
		return;
	for (i = 0; i < n; i++)
		if (strlen(lines[i]) + 4 > bw)
			bw = (unsigned)strlen(lines[i]) + 4;
	if (bw > W)
		bw = W;
	if (bh > H)
		bh = H;
	x0 = (W - bw) / 2;
	y0 = (H - bh) / 2;
	for (y = 0; y < bh; y++)
		for (x = 0; x < bw; x++)
			put(desk, W, H, x0 + x, y0 + y, U' ', MENU_FG, MENU_BG, 0);
	for (x = 0; x < bw; x++) {
		put(desk, W, H, x0 + x, y0, 0x2550, MENU_BORDER, MENU_BG, 0);
		put(desk, W, H, x0 + x, y0 + bh - 1, 0x2550, MENU_BORDER, MENU_BG,
		    0);
	}
	for (y = 0; y < bh; y++) {
		put(desk, W, H, x0, y0 + y, 0x2551, MENU_BORDER, MENU_BG, 0);
		put(desk, W, H, x0 + bw - 1, y0 + y, 0x2551, MENU_BORDER, MENU_BG,
		    0);
	}
	put(desk, W, H, x0, y0, 0x2554, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0 + bw - 1, y0, 0x2557, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0, y0 + bh - 1, 0x255A, MENU_BORDER, MENU_BG, 0);
	put(desk, W, H, x0 + bw - 1, y0 + bh - 1, 0x255D, MENU_BORDER, MENU_BG,
	    0);
	text(desk, W, H, x0 + 2, y0, " GoblinView help ", MENU_BORDER, MENU_BG,
	    DV_ATTR_BOLD, bw - 3);
	for (i = 0; i < n && i + 2 < bh; i++)
		text(desk, W, H, x0 + 2, y0 + 1 + i, lines[i], MENU_FG, MENU_BG,
		    i == 0 ? DV_ATTR_BOLD : 0, bw - 4);
}
