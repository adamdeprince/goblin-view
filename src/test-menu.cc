/*
 * The menu: it is a compositor layer. Open it and the box sits on top
 * of the windows; close it and the next frame simply omits it. A
 * desktop too small for the box gets a one-row bar.
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

static struct dv_size
size_of(unsigned cols, unsigned rows)
{
	struct dv_size	sz;

	memset(&sz, 0, sizeof sz);
	sz.cols = cols;
	sz.rows = rows;
	sz.xpixel = DV_DEFAULT_XPIXEL;
	sz.ypixel = DV_DEFAULT_YPIXEL;
	return (sz);
}

/* A session with one zoomed shell window, no pty behind it. */
static void
session_init(struct dv_session *s, struct dv_window *w, unsigned cols,
    unsigned rows)
{
	memset(s, 0, sizeof *s);
	memset(w, 0, sizeof *w);
	dv_desktop_init(&s->desktop, size_of(cols, rows));
	w->id = 1;
	w->session = s;
	w->pty_fd = -1;
	w->zoomed = 1;
	dv_strlcpy(w->cmd, "shell", sizeof w->cmd);
	dv_zoom_geom(cols, rows, &w->geom);
	dv_screen_init(&w->screen, cols, rows);
	TAILQ_INSERT_TAIL(&s->desktop.windows, w, entry);
	s->desktop.focused = w;
}

static void
session_free(struct dv_window *w)
{
	dv_screen_free(&w->screen);
}

/* The desk row as plain ASCII, non-ASCII as '#'. */
static void
row_text(const struct dv_cell *desk, unsigned W, unsigned y, char *out)
{
	unsigned	x;

	for (x = 0; x < W; x++) {
		uint32_t	ch = desk[y * W + x].ch;

		out[x] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '#';
	}
	out[W] = '\0';
}

static int
desk_has(const struct dv_cell *desk, unsigned W, unsigned H, const char *s)
{
	char		line[1024];
	unsigned	y;

	for (y = 0; y < H; y++) {
		row_text(desk, W, y, line);
		if (strstr(line, s) != NULL)
			return (1);
	}
	return (0);
}

static void
test_box_over_desktop(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	struct dv_view		 v;
	unsigned char		*frame = nullptr;
	size_t			 n;
	unsigned		 W = 80, H = 24;

	session_init(&s, &w, W, H);
	dv_vt_write(&w, "$ make\r\n", 8);
	desk = dv_allocn<dv_cell>(W * H);
	memset(&v, 0, sizeof v);

	s.menu_open = 1;
	dv_compose_cells(&s, desk);
	if (!desk_has(desk, W, H, "GoblinView"))
		fail("box has a title");
	if (!desk_has(desk, W, H, "Open Window"))
		fail("box lists Open Window");
	if (!desk_has(desk, W, H, "$ make"))
		fail("the window is still visible around the box");

	n = dv_compose_frame(&s, desk, &v, &frame);
	if (n == 0)
		fail("first frame is not empty");
	if (frame == nullptr || memmem(frame, n, "\033[?1049", 7) != NULL)
		fail("the menu must not switch the host's screen");
	if (frame == nullptr || memmem(frame, n, "\033[?25l", 6) == NULL ||
	    memmem(frame, n, "\033[?25h", 6) != NULL)
		fail("cursor is hidden while the menu is up");
	free(frame);

	s.menu_open = 0;
	dv_compose_cells(&s, desk);
	if (desk_has(desk, W, H, "GoblinView"))
		fail("closing removes the box");
	n = dv_compose_frame(&s, desk, &v, &frame);
	if (n == 0 || frame == nullptr)
		fail("closing repaints what the box covered");
	else if (memmem(frame, n, "\033[?25h", 6) == NULL)
		fail("closing shows the cursor again");
	free(frame);

	free(desk);
	session_free(&w);
}

static void
test_small_desktop_gets_bar(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	char			 line[64];
	unsigned		 W = 20, H = 6;

	session_init(&s, &w, W, H);
	desk = dv_allocn<dv_cell>(W * H);
	s.menu_open = 1;
	dv_compose_cells(&s, desk);
	if (desk_has(desk, W, H, "GoblinView"))
		fail("a desktop too small for the box must not get one");
	row_text(desk, W, H - 1, line);
	if (strstr(line, "Menu") == NULL)
		fail("bar goes on the last row");
	free(desk);
	session_free(&w);
}

/* A second frame with nothing new is empty; a change is a small diff. */
static void
test_diff_frames(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	struct dv_view		 v;
	unsigned char		*frame = nullptr;
	size_t			 n, full;
	unsigned		 W = 132, H = 43;

	session_init(&s, &w, W, H);
	desk = dv_allocn<dv_cell>(W * H);
	memset(&v, 0, sizeof v);

	dv_compose_cells(&s, desk);
	full = dv_compose_frame(&s, desk, &v, &frame);
	if (full == 0)
		fail("first frame paints everything");
	free(frame);
	frame = nullptr;

	n = dv_compose_frame(&s, desk, &v, &frame);
	if (n != 0 || frame != nullptr)
		fail("unchanged desktop sends nothing");
	free(frame);
	frame = nullptr;

	dv_vt_write(&w, "x", 1);
	dv_compose_cells(&s, desk);
	n = dv_compose_frame(&s, desk, &v, &frame);
	if (n == 0 || n >= full / 4)
		fail("one changed cell is a small frame");
	if (frame == nullptr || memmem(frame, n, "\033[?2026h", 8) == NULL ||
	    memmem(frame, n, "\033[?2026l", 8) == NULL)
		fail("frames are wrapped in synchronized output");
	free(frame);

	free(desk);
	session_free(&w);
	dv_view_free(&v);
}

static void
test_unicode(void)
{
	struct dv_cell	*desk;
	unsigned	 W = 90, H = 30;
	int		 gc, gr, nc = 0, ns = 0, rc, cat, sel;

	if (dv_uni_cats() < 4)
		fail("several unicode categories");
	if (dv_uni_char(0, 0) == 0)
		fail("category 0 has glyphs");
	dv_uni_grid(W, H, &gc, &gr);
	if (gc < 2 || gr < 2)
		fail("a usable glyph grid");

	desk = dv_allocn<dv_cell>(W * H);
	dv_uni_paint(desk, W, H, 0, 0);
	/* the first category name appears on the tab row */
	{
		char		line[256];
		unsigned	x, y;
		int		found = 0;

		for (y = 0; y < H; y++) {
			for (x = 0; x < W; x++) {
				uint32_t ch = desk[y * W + x].ch;
				line[x] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : ' ';
			}
			line[W] = '\0';
			if (strstr(line, "Arrows") != NULL)
				found = 1;
		}
		if (!found)
			fail("picker shows the category tabs");
	}
	free(desk);

	/* Clicking the first glyph cell selects index 0 of the shown page. */
	cat = 0;
	sel = 0;
	{
		/* Ask the picker where its box and glyph grid landed. */
		int	bx, by, gx, gy;

		dv_uni_box(W, H, &bx, &by, &gx, &gy);
		rc = dv_uni_click(W, H, cat, sel, gx + 1, gy, &nc, &ns);
		if (rc != 1 || ns != 0)
			fail("clicking the first glyph selects it");
		/* Clicking a tab on the tab row switches category. */
		rc = dv_uni_click(W, H, cat, sel, bx + 2, by + 1, &nc, &ns);
		if (rc != 0 || nc != 0)
			fail("clicking a tab selects a category");
		/* Clicking outside the box reports nothing. */
		rc = dv_uni_click(W, H, cat, sel, 0, 0, &nc, &ns);
		if (rc != -1)
			fail("clicking outside the picker is a miss");
	}
}

static unsigned count_placeholders(const struct dv_cell *, unsigned, unsigned);

/*
 * A client is painted image cells only when its host takes kitty
 * graphics. The same desktop, composed for a graphics client, carries
 * the image (an APC transmit and U+10EEEE placeholder cells); composed
 * for a text client it carries neither -- the picture's cells paint
 * blank -- so reconnecting from a terminal without kitty graphics is
 * safe. This is the per-client gate that makes a status change on
 * reconnect correct.
 */
static void
test_gfx_gate(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	struct dv_view		 vg, vt;
	unsigned char		*frame = nullptr;
	size_t			 n;
	unsigned		 W = 40, H = 12;
	const char		*img = "\033[H\033_Ga=T,f=32,s=64,v=64,C=1;QUJD\033\\";
	static const unsigned char ph[] = { 0xf4, 0x8e, 0xbb, 0xae };

	memset(&s, 0, sizeof s);
	dv_desktop_init(&s.desktop, size_of(W, H));
	s.desktop.gfx = 1;
	memset(&w, 0, sizeof w);
	w.id = 1; w.session = &s; w.pty_fd = -1; w.framed = 1;
	w.geom.x = 0; w.geom.y = 0; w.geom.cols = 20; w.geom.rows = 8;
	dv_screen_init(&w.screen, 20, 8);
	dv_vt_write(&w, img, strlen(img));
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	w.z = 1;
	s.desktop.focused = &w;
	desk = dv_allocn<dv_cell>(W * H);
	dv_compose_cells(&s, desk);
	if (count_placeholders(desk, W, H) == 0)
		fail("the image is on the desktop as placeholder cells");

	/* A graphics client: transmit and placeholder cells. */
	memset(&vg, 0, sizeof vg);
	vg.gfx = 1;
	n = dv_compose_frame(&s, desk, &vg, &frame);
	if (frame == nullptr || memmem(frame, n, "\033_Ga=t", 6) == NULL)
		fail("a graphics client gets the image transmitted");
	if (frame == nullptr || memmem(frame, n, ph, 4) == NULL)
		fail("a graphics client gets placeholder cells");
	free(frame);
	frame = nullptr;

	/* A text client (host without kitty graphics): neither. */
	memset(&vt, 0, sizeof vt);
	vt.gfx = 0;
	n = dv_compose_frame(&s, desk, &vt, &frame);
	if (frame != nullptr && memmem(frame, n, "\033_G", 3) != NULL)
		fail("a text client is sent no graphics escapes");
	if (frame != nullptr && memmem(frame, n, ph, 4) != NULL)
		fail("a text client is sent no placeholder cells");
	free(frame);

	dv_view_free(&vg);
	dv_view_free(&vt);
	free(desk);
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
}

/*
 * OSC 66 scaled text is re-emitted to a host that speaks it (a kitty
 * graphics host, by proxy) and drawn unscaled otherwise, so the text is
 * present either way.
 */
static void
test_osc66(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	struct dv_view		 vg, vt;
	unsigned char		*frame = nullptr;
	size_t			 n;
	unsigned		 W = 40, H = 12;

	memset(&s, 0, sizeof s);
	dv_desktop_init(&s.desktop, size_of(W, H));
	memset(&w, 0, sizeof w);
	w.id = 1; w.session = &s; w.pty_fd = -1; w.framed = 1;
	w.geom.x = 0; w.geom.y = 0; w.geom.cols = 30; w.geom.rows = 8;
	dv_screen_init(&w.screen, 30, 8);
	dv_vt_write(&w, "\033[H\033]66;s=2;Hi\a", 15);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	w.z = 1;
	s.desktop.focused = &w;
	desk = dv_allocn<dv_cell>(W * H);
	dv_compose_cells(&s, desk);

	memset(&vg, 0, sizeof vg);
	vg.gfx = 1;
	n = dv_compose_frame(&s, desk, &vg, &frame);
	if (frame == nullptr || memmem(frame, n, "\033]66;s=2;Hi", 11) == NULL)
		fail("a text-sizing host is sent the OSC 66 run");
	free(frame);
	frame = nullptr;

	memset(&vt, 0, sizeof vt);
	vt.gfx = 0;
	n = dv_compose_frame(&s, desk, &vt, &frame);
	if (frame == nullptr || memmem(frame, n, "\033]66", 4) != NULL)
		fail("a plain host is not sent OSC 66");
	if (frame == nullptr || memmem(frame, n, "Hi", 2) == NULL)
		fail("a plain host still gets the text, unscaled");
	free(frame);

	dv_view_free(&vg);
	dv_view_free(&vt);
	free(desk);
	dv_screen_free(&w.screen);
}

/* Styled, coloured underlines reach the host as SGR 4:n and 58. */
static void
test_underline_emit(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	struct dv_view		 v;
	unsigned char		*frame = nullptr;
	size_t			 n;
	unsigned		 W = 40, H = 10;

	session_init(&s, &w, W, H);
	dv_vt_write(&w, "\033[4:3;58;2;255;0;0mU\033[0m", 24);
	desk = dv_allocn<dv_cell>(W * H);
	memset(&v, 0, sizeof v);
	dv_compose_cells(&s, desk);
	n = dv_compose_frame(&s, desk, &v, &frame);
	if (frame == nullptr || memmem(frame, n, ";4:3", 4) == NULL)
		fail("the frame carries the underline style 4:3");
	if (frame == nullptr || memmem(frame, n, ";58;2;255;0;0", 13) == NULL)
		fail("the frame carries the underline colour 58;2;r;g;b");
	free(frame);
	dv_view_free(&v);
	free(desk);
	session_free(&w);
}

/* A window's colour overrides are applied when its cells are painted:
 * palette index 1 redefined by OSC 4 is emitted as that RGB, and a
 * default-background cell takes the OSC 11 background. Another window is
 * untouched, since the host's palette is never changed. */
static void
test_palette_remap(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	struct dv_view		 v;
	unsigned char		*frame = nullptr;
	size_t			 n;
	unsigned		 W = 40, H = 10;

	session_init(&s, &w, W, H);
	{
		const char	*a = "\033]4;1;rgb:12/34/56\033\\\033]11;#0000ff\033\\";
		const char	*b = "\033[31mR\033[0m";

		dv_vt_write(&w, a, strlen(a));
		dv_vt_write(&w, b, strlen(b));
	}
	desk = dv_allocn<dv_cell>(W * H);
	memset(&v, 0, sizeof v);
	dv_compose_cells(&s, desk);
	{
		struct dv_cell	*c = &desk[0];		/* zoomed: no frame */

		if (c->ch != 'R' || c->fg != (DV_COLOR_RGB | 0x123456))
			fail("a redefined palette index is painted as its RGB");
		if (c->bg != (DV_COLOR_RGB | 0x0000ff))
			fail("the window's default background is painted as its RGB");
		if (desk[1].bg != (DV_COLOR_RGB | 0x0000ff))
			fail("blank cells of the window take its background too");
	}
	n = dv_compose_frame(&s, desk, &v, &frame);
	if (frame == nullptr || memmem(frame, n, "38;2;18;52;86", 13) == NULL)
		fail("the frame carries the remapped foreground");
	if (frame == nullptr || memmem(frame, n, "48;2;0;0;255", 12) == NULL)
		fail("the frame carries the remapped background");
	free(frame);
	dv_view_free(&v);
	free(desk);
	session_free(&w);
}

/* A file transfer's question is a box over the desktop, and the cursor
 * hides while it is up. */
static void
test_xfer_prompt(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	unsigned		 W = 80, H = 24, y;
	int			 seen = 0, cx, cy, vis;

	session_init(&s, &w, W, H);
	s.xfer_prompt = 1;
	s.xfer_nlines = 2;
	dv_strlcpy(s.xfer_text[0], "Window 1 (shell) wants to send you files.",
	    sizeof s.xfer_text[0]);
	dv_strlcpy(s.xfer_text[1], "Save them to ~/Downloads?   y / n",
	    sizeof s.xfer_text[1]);
	desk = dv_allocn<dv_cell>(W * H);
	dv_compose_cells(&s, desk);
	for (y = 0; y < H; y++) {
		char	row[128];

		row_text(desk, W, y, row);
		if (strstr(row, "wants to send you files") != NULL)
			seen |= 1;
		if (strstr(row, "Save them to ~/Downloads?") != NULL)
			seen |= 2;
	}
	if (seen != 3)
		fail("the transfer box shows both lines");
	dv_compose_cursor(&s, &cx, &cy, &vis);
	if (vis)
		fail("the cursor hides under the transfer box");
	free(desk);
	session_free(&w);
}

/* Extra cursors: painted as reverse video for a host that cannot draw
 * them, sent as the protocol to one that can, and only when they change. */
static void
test_xcursor_compose(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	struct dv_view		 v;
	unsigned char		*frame = nullptr;
	size_t			 n;
	unsigned		 W = 40, H = 10;

	session_init(&s, &w, W, H);
	{
		const char	*seq = "\033[>1;2:2:3 q";	/* row 2, column 3 */

		dv_vt_write(&w, seq, strlen(seq));
	}
	desk = dv_allocn<dv_cell>(W * H);
	memset(&v, 0, sizeof v);
	dv_compose_cells(&s, desk);
	if (!(desk[1 * W + 2].attr & DV_ATTR_REV))
		fail("without a capable host the cursor cell is reverse video");
	n = dv_compose_frame(&s, desk, &v, &frame);
	if (frame != nullptr && memmem(frame, n, "\033[>", 3) != NULL)
		fail("nothing is sent to a host that cannot draw extra cursors");
	free(frame);
	frame = nullptr;

	s.desktop.mcursors_host = 1;
	v.mcursors = 1;
	dv_compose_cells(&s, desk);
	if (desk[1 * W + 2].attr & DV_ATTR_REV)
		fail("with a capable host the cell is left alone");
	n = dv_compose_frame(&s, desk, &v, &frame);
	if (frame == nullptr || memmem(frame, n, "\033[>0;4 q\033[>1;2:2:3 q",
	    strlen("\033[>0;4 q\033[>1;2:2:3 q")) == NULL)
		fail("a capable host is sent the cursor set (clear, then set)");
	free(frame);
	frame = nullptr;
	n = dv_compose_frame(&s, desk, &v, &frame);
	if (frame != nullptr && memmem(frame, n, "\033[>0;4 q", 8) != NULL)
		fail("an unchanged set is not sent again");
	free(frame);
	dv_view_free(&v);
	free(desk);
	free(s.xcur_map);
	session_free(&w);
}

/* A black-and-white client: only default colours and reverse video are
 * ever emitted. A coloured background (the title bar, a highlight) becomes
 * reverse video; coloured text on the default background becomes ink. */
static void
test_mono_frame(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	struct dv_view		 v;
	unsigned char		*frame = nullptr;
	size_t			 n, i;
	unsigned		 W = 60, H = 12;
	int			 colours = 0, reverse = 0;

	session_init(&s, &w, W, H);
	w.zoomed = 0;				/* framed: a coloured title bar */
	w.framed = 1;
	w.geom.x = 2;
	w.geom.y = 1;
	w.geom.cols = 30;
	w.geom.rows = 6;
	dv_screen_resize(&w.screen, 30, 6);
	{
		const char	*seq = "\033[31;44mR\033[0m \033[2;33mD\033[0m \033[7mV\033[0m "
		    "\033[38;2;10;200;30;58;2;255;0;0;4:3mU\033[0m";

		dv_vt_write(&w, seq, strlen(seq));
	}
	desk = dv_allocn<dv_cell>(W * H);
	memset(&v, 0, sizeof v);
	v.mono = 1;
	dv_compose_cells(&s, desk);
	n = dv_compose_frame(&s, desk, &v, &frame);
	if (frame == nullptr || n == 0)
		fail("a mono frame is composed");
	/* every SGR parameter must be one of 0 1 3 4[:n] 7 9 39 49 */
	for (i = 0; frame != nullptr && i + 1 < n; i++) {
		if (frame[i] != 0x1b || frame[i + 1] != '[')
			continue;
		size_t	j = i + 2;
		while (j < n && ((frame[j] >= '0' && frame[j] <= '9') ||
		    frame[j] == ';' || frame[j] == ':'))
			j++;
		if (j >= n || frame[j] != 'm')
			continue;		/* not an SGR */
		{
			char	params[64];
			size_t	pl = j - (i + 2) < sizeof params - 1 ? j - (i + 2) : sizeof params - 1;
			char	*tok, *save;

			memcpy(params, frame + i + 2, pl);
			params[pl] = '\0';
			for (tok = strtok_r(params, ";", &save); tok != NULL;
			    tok = strtok_r(NULL, ";", &save)) {
				int	p = atoi(tok);

				if (p == 7)
					reverse++;
				if (!(p == 0 || p == 1 || p == 3 || p == 4 || p == 7 ||
				    p == 9 || p == 39 || p == 49))
					colours++;
			}
		}
	}
	if (colours != 0)
		fail("a mono frame carries no colour, dim or blink parameters");
	if (reverse == 0)
		fail("the coloured title bar is painted as reverse video");
	if (frame != nullptr && memmem(frame, n, "\xe2\x96\x91", 3) != NULL)
		fail("the backdrop stipple is plain paper on a mono client");
	{
		/* window cell 0,0 (red on blue) is reverse; cell 2,0 (dim yellow
		 * on default) is plain ink; 4,0 (reverse) stays reverse */
		unsigned	ox = (unsigned)w.geom.x + 1, oy = (unsigned)w.geom.y + 1;
		struct dv_cell	c0 = desk[oy * W + ox], c2 = desk[oy * W + ox + 2],
				c4 = desk[oy * W + ox + 4];

		(void)c0; (void)c2; (void)c4;	/* the desk keeps colour: mapping is per view */
		if (c0.bg == DV_COLOR_DEFAULT)
			fail("the desk itself is untouched by a mono view");
	}
	free(frame);
	dv_view_free(&v);
	free(desk);
	session_free(&w);
}

/* The switch-window list: the filter matches numbers, titles and commands;
 * the overlay lists the matches with the selected row in reverse video. */
static void
test_switch_list(void)
{
	struct dv_session	 s;
	struct dv_window	 w1, w2;
	struct dv_cell		*desk;
	unsigned		 ids[8], n, x, y, W = 80, H = 24;
	int			 seen_beta = 0, seen_alpha = 0, rev_on_beta = 0;

	session_init(&s, &w1, W, H);
	dv_strlcpy(w1.title, "alpha", sizeof w1.title);
	dv_strlcpy(w1.cmd, "bash", sizeof w1.cmd);
	memset(&w2, 0, sizeof w2);
	w2.id = 2; w2.session = &s; w2.pty_fd = -1; w2.framed = 1;
	w2.geom.x = 8; w2.geom.y = 4; w2.geom.cols = 20; w2.geom.rows = 8;
	dv_screen_init(&w2.screen, 20, 8);
	dv_strlcpy(w2.title, "beta", sizeof w2.title);
	dv_strlcpy(w2.cmd, "htop", sizeof w2.cmd);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w2, entry);

	s.switch_filter[0] = '\0';
	if (dv_switch_list(&s, ids, 8) != 2 || ids[0] != 1 || ids[1] != 2)
		fail("an empty filter lists every window in order");
	dv_strlcpy(s.switch_filter, "be", sizeof s.switch_filter);
	if (dv_switch_list(&s, ids, 8) != 1 || ids[0] != 2)
		fail("a title prefix filters");
	dv_strlcpy(s.switch_filter, "TOP", sizeof s.switch_filter);
	if (dv_switch_list(&s, ids, 8) != 1 || ids[0] != 2)
		fail("a command substring filters, case-insensitively");
	dv_strlcpy(s.switch_filter, "1", sizeof s.switch_filter);
	if (dv_switch_list(&s, ids, 8) != 1 || ids[0] != 1)
		fail("a number filters");
	dv_strlcpy(s.switch_filter, "zz", sizeof s.switch_filter);
	if (dv_switch_list(&s, ids, 8) != 0)
		fail("no match is empty");

	s.switch_filter[0] = '\0';
	s.switch_open = 1;
	s.switch_sel = 1;
	desk = dv_allocn<dv_cell>(W * H);
	dv_compose_cells(&s, desk);
	for (y = 0; y < H; y++) {
		char	row[128];

		row_text(desk, W, y, row);
		if (strstr(row, "alpha") != NULL && strstr(row, "bash") != NULL)
			seen_alpha = 1;
		if (strstr(row, "beta") != NULL && strstr(row, "htop") != NULL) {
			seen_beta = 1;
			for (x = 0; x < W; x++)
				if (desk[y * W + x].attr & DV_ATTR_REV)
					rev_on_beta = 1;
		}
		if (strstr(row, "Switch window") != NULL)
			n = y;
	}
	(void)n;
	if (!seen_alpha || !seen_beta)
		fail("the list shows both windows with their commands");
	if (!rev_on_beta)
		fail("the selected row is reverse video");
	free(desk);
	dv_screen_free(&w2.screen);
	session_free(&w1);
}

/* Copy mode paints its cursor as a reverse cell and a status line, and
 * hides the terminal cursor. */
static void
test_copy_mode_paint(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	unsigned		 W = 60, H = 12, y;
	int			 cx, cy, vis, seen = 0;

	session_init(&s, &w, W, H);
	dv_vt_write(&w, "hello world", 11);
	s.copy_open = 1;
	s.copy_win = 1;
	s.copy_x = 6;
	s.copy_y = 0;
	desk = dv_allocn<dv_cell>(W * H);
	dv_compose_cells(&s, desk);
	if (!(desk[0 * W + 6].attr & DV_ATTR_REV) || desk[6].ch != 'w')
		fail("the copy cursor is a reverse cell on the window's text");
	for (y = 0; y < H; y++) {
		char	row[128];

		row_text(desk, W, y, row);
		if (strstr(row, "COPY") != NULL)
			seen = 1;
	}
	if (!seen)
		fail("copy mode shows its status line");
	dv_compose_cursor(&s, &cx, &cy, &vis);
	if (vis)
		fail("the terminal cursor hides in copy mode");
	free(desk);
	session_free(&w);
}

/* Placeholder cells in a composed desktop. */
static unsigned
count_placeholders(const struct dv_cell *desk, unsigned W, unsigned H)
{
	unsigned	n = 0, i;

	for (i = 0; i < W * H; i++)
		if (desk[i].ch == DV_KITTY_PLACEHOLDER)
			n++;
	return (n);
}

/* A window's kitty image is painted as placeholder cells: clipped to the
 * window and occluded by a window on top, exactly like text. */
static void
test_image_cells(void)
{
	struct dv_session	 s;
	struct dv_window	 w1, w2;
	struct dv_cell		*desk;
	unsigned		 W = 60, H = 20, x, y, ph = 0, covered_ph = 0;
	const char		*img = "\033_Ga=T,f=32,s=192,v=192,C=1;QUJD\033\\";

	memset(&s, 0, sizeof s);
	dv_desktop_init(&s.desktop, size_of(W, H));

	/* Window 1: framed at 1,1, roomy; a 12x6-cell image at its origin. */
	memset(&w1, 0, sizeof w1);
	w1.id = 1; w1.session = &s; w1.pty_fd = -1; w1.framed = 1;
	w1.geom.x = 1; w1.geom.y = 1; w1.geom.cols = 30; w1.geom.rows = 12;
	dv_screen_init(&w1.screen, 30, 12);
	dv_vt_write(&w1, img, strlen(img));
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w1, entry);
	w1.z = 1;

	/* Window 2 on top, overlapping the image's right/bottom. */
	memset(&w2, 0, sizeof w2);
	w2.id = 2; w2.session = &s; w2.pty_fd = -1; w2.framed = 1;
	w2.geom.x = 8; w2.geom.y = 4; w2.geom.cols = 20; w2.geom.rows = 8;
	dv_screen_init(&w2.screen, 20, 8);
	dv_vt_write(&w2, "hello", 5);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w2, entry);
	w2.z = 2;
	s.desktop.focused = &w2;

	desk = dv_allocn<dv_cell>(W * H);
	dv_compose_cells(&s, desk);

	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			struct dv_cell	*c = &desk[y * W + x];
			int		 in2 = ((int)x >= w2.geom.x &&
			    (int)x < w2.geom.x + (int)w2.geom.cols + 2 &&
			    (int)y >= w2.geom.y &&
			    (int)y < w2.geom.y + (int)w2.geom.rows + 2);

			if (c->ch == DV_KITTY_PLACEHOLDER) {
				ph++;
				if (in2)
					covered_ph++;
				if (c->fg != w1.screen.kpl->begin()->second.dv_id)
					fail("placeholder cells name the placement's host id");
			}
		}
	if (ph == 0)
		fail("the image shows as placeholder cells");
	if (ph >= 12 * 6)
		fail("window 2 must occlude part of the image");
	if (covered_ph != 0)
		fail("no image cell survives under the covering window");

	free(desk);
	dv_screen_free(&w1.screen);
	dv_screen_free(&w2.screen);
	dv_kitty_free(&w1);
	dv_kitty_free(&w2);
}

/* An image larger than its window is clipped to the window's cells. The
 * placement is kept whole, so a shrink crops it and growing back
 * restores it. */
static void
test_image_crop(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	unsigned		 W = 40, H = 20;
	const char		*img = "\033_Ga=T,f=32,s=320,v=320,C=1;QUJD\033\\";

	memset(&s, 0, sizeof s);
	dv_desktop_init(&s.desktop, size_of(W, H));
	memset(&w, 0, sizeof w);
	w.id = 3; w.session = &s; w.pty_fd = -1; w.framed = 1;
	w.geom.x = 1; w.geom.y = 1; w.geom.cols = 6; w.geom.rows = 3;
	dv_screen_init(&w.screen, 6, 3);		/* small window */
	dv_vt_write(&w, img, strlen(img));		/* 20x10 cells */
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	w.z = 1;
	s.desktop.focused = &w;

	desk = dv_allocn<dv_cell>(W * H);
	dv_compose_cells(&s, desk);
	if (count_placeholders(desk, W, H) != 6 * 3)
		fail("image clipped to the window's cells (got wrong count)");

	w.geom.cols = 12; w.geom.rows = 6;		/* grow: more shows */
	dv_screen_resize(&w.screen, 12, 6);
	dv_compose_cells(&s, desk);
	if (count_placeholders(desk, W, H) != 12 * 6)
		fail("growing the window shows more of the image");

	w.geom.cols = 4; w.geom.rows = 2;		/* shrink: crops */
	dv_screen_resize(&w.screen, 4, 2);
	dv_compose_cells(&s, desk);
	if (count_placeholders(desk, W, H) != 4 * 2)
		fail("shrinking the window crops the image");

	free(desk);
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
}

/* Remote desktops replace tile pixels without moving their placeholders. */
static void
test_image_updates(void)
{
	struct dv_session s;
	struct dv_window w;
	struct dv_view views[3]{};
	constexpr unsigned W = 8, H = 4;
	unsigned char *frame = nullptr;

	session_init(&s, &w, W, H);
	views[0].gfx = views[1].gfx = 1;
	views[2].sixel = 1;
	setenv("GOBLIN_VIEW_KITTY_MEDIUM", "direct", 1);
	auto *desk = dv_allocn<dv_cell>(W * H);
	auto paint = [&](const char *pixels) {
		char cmd[256];
		snprintf(cmd, sizeof cmd,
		    "\033[H\033_Ga=T,t=d,f=24,s=1,v=1,i=1,p=1,c=1,r=1,C=1,q=2;%s\033\\",
		    pixels);
		dv_vt_write(&w, cmd, strlen(cmd));
		dv_compose_cells(&s, desk);
	};
	paint("/wAA");
	for (auto &v : views) {
		dv_compose_frame(&s, desk, &v, &frame);
		free(frame);
		frame = nullptr;
	}
	paint("AP8A");
	for (auto &v : views) {
		size_t n = dv_compose_frame(&s, desk, &v, &frame);
		if (v.gfx) {
			if (frame == nullptr || memmem(frame, n, ";AP8A\033\\", 7) == nullptr)
				fail("each kitty client receives replaced tile pixels without cell changes");
		} else if (frame == nullptr || memmem(frame, n, "\033Pq", 3) == nullptr)
			fail("a sixel client repaints replaced tile pixels without cell changes");
		free(frame);
		frame = nullptr;
		if (dv_compose_frame(&s, desk, &v, &frame) != 0)
			fail("an unchanged graphics frame produces no output");
		free(frame);
		frame = nullptr;
		dv_view_free(&v);
	}
	free(desk);
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);
	unsetenv("GOBLIN_VIEW_KITTY_MEDIUM");
}

/* A frame must transmit and cache every visible image beyond 4096. */
static void
test_many_image_cells(void)
{
	struct dv_session s;
	struct dv_window w, other;
	struct dv_view v{};
	constexpr unsigned W = 100, H = 60, count = 5000;
	char cmd[192];
	unsigned char *frame = nullptr;

	session_init(&s, &w, W, H);
	v.gfx = 1;
	setenv("GOBLIN_VIEW_KITTY_MEDIUM", "direct", 1);
	for (unsigned i = 0; i < count; i++) {
		snprintf(cmd, sizeof cmd,
		    "\033[%u;%uH\033_Ga=T,f=32,s=1,v=1,i=%u,C=1,q=2;QUJDRA==\033\\",
		    i / W + 1, i % W + 1, i + 1);
		dv_vt_write(&w, cmd, strlen(cmd));
	}
	memset(&other, 0, sizeof other);
	other.id = 2; other.session = &s; other.pty_fd = -1;
	dv_screen_init(&other.screen, 1, 1);
	const char *img = "\033_Ga=T,f=32,s=1,v=1,C=1,q=2;QUJDRA==\033\\";
	dv_vt_write(&other, img, strlen(img));
	TAILQ_INSERT_TAIL(&s.desktop.windows, &other, entry);
	unsigned other_id = other.screen.kpl->begin()->first;
	struct dv_window *found = nullptr;
	if (w.screen.kpl->contains(other_id) ||
	    dv_kitty_lookup(&s, other_id, &found) == nullptr || found != &other)
		fail("host placement IDs and lookups remain distinct across windows");

	auto *desk = dv_allocn<dv_cell>(W * H);
	dv_compose_cells(&s, desk);
	if (count_placeholders(desk, W, H) != count)
		fail("the compositor paints more than 4096 distinct images");
	size_t n = dv_compose_frame(&s, desk, &v, &frame);
	size_t transmits = 0;
	for (size_t i = 0; i + 6 <= n; i++)
		if (memcmp(frame + i, "\033_Ga=t", 6) == 0)
			transmits++;
	if (transmits != count || v.kimg_gen->size() != count)
		fail("a frame transmits and caches every visible image beyond 4096");
	free(frame);
	frame = nullptr;

	/* An unchanged frame must not re-upload images outside the old cap. */
	n = dv_compose_frame(&s, desk, &v, &frame);
	if (frame != nullptr && memmem(frame, n, "\033_Ga=t", 6) != nullptr)
		fail("all cached images remain current on an unchanged frame");
	free(frame);
	frame = nullptr;

	/* The newest image wins overlap, independent of hash iteration order. */
	const char *top = "\033[H\033_Ga=T,f=32,s=1,v=1,i=6000,C=1,q=2;QUJDRA==\033\\";
	dv_vt_write(&w, top, strlen(top));
	dv_compose_cells(&s, desk);
	bool top_visible = false;
	for (unsigned i = 0; i < W * H; i++)
		if (desk[i].ch == DV_KITTY_PLACEHOLDER &&
		    w.screen.kpl->at(desk[i].fg).child_id == 6000)
			top_visible = true;
	if (!top_visible)
		fail("hash iteration preserves the order of overlapping images");
	dv_compose_frame(&s, desk, &v, &frame);
	if (v.kimg_gen->size() != count)
		fail("occluded images leave the client cache");
	free(frame);
	frame = nullptr;

	const char *del = "\033_Ga=d,d=A,q=2\033\\";
	dv_vt_write(&w, del, strlen(del));
	dv_compose_cells(&s, desk);
	n = dv_compose_frame(&s, desk, &v, &frame);
	size_t deletes = 0;
	for (size_t i = 0; i + 10 <= n; i++)
		if (memcmp(frame + i, "\033_Ga=d,d=I", 10) == 0)
			deletes++;
	if (!v.kimg_gen->empty() || deletes != count)
		fail("all hidden images are deleted from the host and cache");
	free(frame);
	free(desk);
	dv_view_free(&v);
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_screen_free(&other.screen);
	dv_kitty_free(&other);
	dv_buf_free(&w.cap);
	dv_buf_free(&other.cap);
	unsetenv("GOBLIN_VIEW_KITTY_MEDIUM");
}

int
main(void)
{
	test_box_over_desktop();
	test_unicode();
	test_underline_emit();
	test_palette_remap();
	test_xfer_prompt();
	test_xcursor_compose();
	test_mono_frame();
	test_switch_list();
	test_copy_mode_paint();
	test_osc66();
	test_gfx_gate();
	test_image_cells();
	test_image_crop();
	test_image_updates();
	test_many_image_cells();
	test_small_desktop_gets_bar();
	test_diff_frames();
	if (failed)
		return (1);
	printf("test-menu: ok\n");
	return (0);
}
