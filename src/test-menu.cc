/*
 * The menu: it is a compositor layer. Open it and the box sits on top
 * of the windows; close it and the next frame simply omits it. A
 * desktop too small for the box gets a one-row bar.
 */

#include "dv.hpp"
#include "vtext.hpp"

#include <sys/mman.h>
#include <zlib.h>

#include <stdio.h>
#include <string.h>
#include <string>
#include <string_view>

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

static void
test_tiny_window_resize(void)
{
	for (unsigned cols : {1u, 2u}) {
		for (unsigned rows : {1u, 2u}) {
			struct dv_session s;
			struct dv_window w;

			session_init(&s, &w, 200, 60);
			w.zoomed = 0;
			w.framed = 1;
			w.geom = {150, 50, cols, rows};
			dv_screen_resize(&w.screen, cols, rows);
			dv_vt_write(&w, "X\033[H", 4);
			for (struct dv_size sz : {size_of(40, 12), size_of(1, 1),
			    size_of(1, 2), size_of(2, 1), size_of(3, 3), size_of(80, 24)}) {
				dv_desktop_resize(&s.desktop, sz);
				dv_screen_resize(&w.screen, w.lcols, w.lrows);
				auto desk = dv_allocn<dv_cell>((size_t)sz.cols * sz.rows);
				dv_compose_cells(&s, desk);
				int border = sz.cols >= 3 && sz.rows >= 3;
				unsigned x = w.geom.x + border, y = w.geom.y + border;
				if (x >= sz.cols || y >= sz.rows || desk[y * sz.cols + x].ch != 'X')
					fail("tiny window content remains visible after shrinking and growing");
				int cx, cy, visible;
				dv_compose_cursor(&s, &cx, &cy, &visible);
				if (!visible || cx != (int)x + 1 || cy != (int)y + 1)
					fail("tiny window cursor stays on its content");
				if (!w.framed)
					fail("temporary border suppression preserves the frame preference");
				struct dv_view view = {};
				unsigned char *frame = nullptr;
				size_t n = dv_compose_frame(&s, desk, &view, &frame);
				if (!frame || !memchr(frame, 'X', n))
					fail("tiny window content reaches the host frame");
				free(frame);
				dv_view_free(&view);
				free(desk);
			}
			free(s.xcur_map);
			session_free(&w);
		}
	}
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

template<typename Char>
static int
desk_has(const struct dv_cell *desk, unsigned W, unsigned H, const Char *s)
{
	std::basic_string_view<Char> needle(s);
	if (needle.size() > W)
		return (0);
	for (unsigned y = 0; y < H; y++)
		for (size_t x = 0; x <= W - needle.size(); x++) {
			size_t i = 0;
			while (i < needle.size() && desk[(size_t)y * W + x + i].ch ==
			    static_cast<uint32_t>(needle[i]))
				i++;
			if (i == needle.size())
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
	if (!desk_has(desk, W, H, U"Odin Seat"))
		fail("box has a title");
	if (!desk_has(desk, W, H, "Open Window"))
		fail("box lists Open Window");
	if (!desk_has(desk, W, H, "Screen redraw"))
		fail("box lists Screen redraw");
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
	if (desk_has(desk, W, H, U"Odin Seat"))
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
	if (desk_has(desk, W, H, U"Odin Seat"))
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
	static const char clear[] = "\033[0m\033[H\033[2J";
	if (frame == nullptr || memmem(frame, full, clear, sizeof clear - 1) == nullptr)
		fail("full frame emits a complete clear-screen sequence");
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
 * A cell's font style reaches a client that listed font.1 and no other, is
 * selected again after the SGR 0 every style change starts with, and takes
 * an incremental repaint of its own when only the style changed.
 */
static void
test_font_paint(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	struct dv_view		 vf, vp;
	unsigned char		*frame = nullptr;
	size_t			 n;
	unsigned		 W = 80, H = 24;
	const char		*sel = "\033]777;font;v=1:op=select:role=serif:weight=300:slant=auto\033\\";
	const char		*text = "\033[H\033]777;font;v=1:op=select:role=serif:weight=300\033\\AB"
	    "\033[1mC\033]777;font;v=1:op=reset\033\\D";

	memset(&s, 0, sizeof s);
	dv_desktop_init(&s.desktop, size_of(W, H));
	memset(&w, 0, sizeof w);
	w.id = 1;
	w.session = &s;
	w.pty_fd = -1;
	w.framed = 0;
	w.geom.cols = W;
	w.geom.rows = H;
	dv_screen_init(&w.screen, W, H);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	dv_vt_write(&w, text, strlen(text));
	desk = dv_allocn<dv_cell>((size_t)W * H);
	dv_compose_cells(&s, desk);
	memset(&vf, 0, sizeof vf);
	memset(&vp, 0, sizeof vp);
	vf.font = 1;
	n = dv_compose_frame(&s, desk, &vf, &frame);
	{
		const unsigned char	*a = (const unsigned char *)memmem(frame, n, sel, strlen(sel));
		const unsigned char	*b = a == nullptr ? nullptr :
		    (const unsigned char *)memmem(a + 1, n - (size_t)(a + 1 - frame), sel, strlen(sel));

		if (a == nullptr || memmem(frame, (size_t)(a - frame), "\033[0", 3) == nullptr)
			fail("a font selection follows the SGR that would reset it");
		if (b == nullptr)
			fail("the bold cell's SGR 0 reset it: it is selected again");
		else if (memmem(frame, n, "\033]777;font;v=1:op=reset", 23) == nullptr &&
		    memmem(b, n - (size_t)(b - frame), "\033[0", 3) == nullptr)
			fail("the plain cell gets the default font back");
	}
	free(frame);
	frame = nullptr;
	n = dv_compose_frame(&s, desk, &vp, &frame);
	if (memmem(frame, n, "777;font", 8) != nullptr)
		fail("a client that did not list font.1 is never sent it");
	free(frame);
	frame = nullptr;
	/* Only the style changes: a client that shows styles is repainted. */
	desk[0].font = dv_font_intern("sans", 0, DV_FONT_SLANT_AUTO);
	n = dv_compose_frame(&s, desk, &vf, &frame);
	if (n == 0 || memmem(frame, n, "role=sans", 9) == nullptr)
		fail("a style change alone repaints the cell");
	free(frame);
	/* A box over the window is not in the program's font. */
	{
		const char	*fill = "\033[H\033]777;font;v=1:op=select:role=serif\033\\";

		dv_vt_write(&w, fill, strlen(fill));
		for (unsigned i = 0; i < W * H - 1; i++)
			dv_vt_write(&w, "X", 1);
		s.menu_open = 1;
		dv_compose_cells(&s, desk);
		if (!desk_has(desk, W, H, U"Odin Seat"))
			fail("the box is up");
		for (unsigned i = 0; i < W * H; i++)
			if (desk[i].ch != 'X' && desk[i].ch != 'A' &&
			    desk[i].ch != ' ' && desk[i].font != 0)
				fail("the box is in the default font");
		s.menu_open = 0;
	}
	free(desk);
	dv_view_free(&vf);
	dv_view_free(&vp);
	dv_screen_free(&w.screen);
}

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

	/* More runs than the old table of 32 held, each with its fraction. */
	for (unsigned i = 0; i < 90; i++) {
		char	run[64];

		snprintf(run, sizeof run, "\033[%u;%uH\033]66;n=4:d=5:w=2:v=2;%02u\033\\",
		    i / 15 + 3, (i % 15) * 2 + 1, i);
		dv_vt_write(&w, run, strlen(run));
	}
	dv_compose_cells(&s, desk);
	n = dv_compose_frame(&s, desk, &vg, &frame);
	{
		size_t	runs = 0;

		static const char	head[] = "\033]66;s=1:w=2:n=4:d=5:v=2;";

		for (size_t i = 0; frame != nullptr && i + sizeof head - 1 <= n; i++)
			if (memcmp(frame + i, head, sizeof head - 1) == 0)
				runs++;
		if (runs != 90)
			fail("every sized-text run reaches the host with its fractional scale");
	}
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
	std::vector<unsigned> ids;
	unsigned		 n, x, y, W = 80, H = 24;
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
	if ((ids = dv_switch_list(&s)).size() != 2 || ids[0] != 1 || ids[1] != 2)
		fail("an empty filter lists every window in order");
	dv_strlcpy(s.switch_filter, "be", sizeof s.switch_filter);
	if ((ids = dv_switch_list(&s)).size() != 1 || ids[0] != 2)
		fail("a title prefix filters");
	dv_strlcpy(s.switch_filter, "TOP", sizeof s.switch_filter);
	if ((ids = dv_switch_list(&s)).size() != 1 || ids[0] != 2)
		fail("a command substring filters, case-insensitively");
	dv_strlcpy(s.switch_filter, "1", sizeof s.switch_filter);
	if ((ids = dv_switch_list(&s)).size() != 1 || ids[0] != 1)
		fail("a number filters");
	dv_strlcpy(s.switch_filter, "zz", sizeof s.switch_filter);
	if ((ids = dv_switch_list(&s)).size() != 0)
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
	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
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
	unsetenv("ODIN_SEAT_KITTY_MEDIUM");
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
	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
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

	/*
	 * The newest image wins overlap, independent of hash iteration order.
	 * Two on one cell are more than a cell can say, so the window is
	 * layered and none of the shared desk's cells are its pictures: the
	 * terminal holds each image once and is sent real placements, and of
	 * two at one depth it puts the one with the greater id on top.
	 */
	const char *top = "\033[H\033_Ga=T,f=32,s=1,v=1,i=6000,C=1,q=2;QUJDRA==\033\\";
	dv_vt_write(&w, top, strlen(top));
	dv_compose_cells(&s, desk);
	if (!w.klayered || s.owner == nullptr || count_placeholders(desk, W, H) != 0)
		fail("overlapping placements make their window layered");
	n = dv_compose_frame(&s, desk, &v, &frame);
	if (w.kimg->at(6000).hid <= w.kimg->at(1).hid)
		fail("hash iteration preserves the order of overlapping images");
	if (!v.kimg_gen->empty() || v.limg_gen == nullptr ||
	    v.limg_gen->size() != count + 1)
		fail("a layered window's images are held once each, and no copies for cells");
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
	if (!v.limg_gen->empty() || deletes != count + 1)
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
	unsetenv("ODIN_SEAT_KITTY_MEDIUM");
}

static size_t
count_in(const unsigned char *frame, size_t n, const std::string &needle)
{
	size_t	found = 0;

	for (size_t i = 0; frame != nullptr && i + needle.size() <= n; i++)
		if (memcmp(frame + i, needle.data(), needle.size()) == 0)
			found++;
	return (found);
}

/*
 * A window whose pictures go under its text, part way into cells or over one
 * another is layered: a terminal with the cells the programs were told holds
 * each image once and is sent real placements at their z, cut to where the
 * window shows. Under another frame a placement is several pieces; wholly
 * covered it is none. Another terminal gets placeholder cells.
 */
static void
test_layers(void)
{
	struct dv_session	 s;
	struct dv_window	 w, top;
	struct dv_view		 v{}, plain{};
	constexpr unsigned	 W = 40, H = 12;
	unsigned char		*frame = nullptr;
	size_t			 n;
	const std::string	 holder = "\xf4\x8e\xbb\xae";	/* U+10EEEE */

	session_init(&s, &w, W, H);
	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	v.gfx = 1;
	plain.sixel = 1;
	const int	framed = dv_window_framed(&w);
	const int	ix = w.geom.x + framed, iy = w.geom.y + framed;
	auto at = [ix, iy](unsigned col, unsigned row) {	/* a cell of the window, 0-based */
		return ("\033[" + std::to_string(iy + (int)row + 1) + ";" +
		    std::to_string(ix + (int)col + 1) + "H");
	};
	auto put = [&w](const std::string &t) { dv_vt_write(&w, t.data(), t.size()); };
	auto *desk = dv_allocn<dv_cell>(W * H);
	auto show = [&](struct dv_view *view) {
		free(frame);
		frame = nullptr;
		dv_compose_cells(&s, desk);
		n = dv_compose_frame(&s, desk, view, &frame);
	};

	/* 64 by 64 pixels: four cells by two at 16 by 32. Under the text. */
	put("\033_Ga=t,f=32,s=64,v=64,i=5,q=2;QUJDRA==\033\\");
	put("\033[2;3H\033_Ga=p,i=5,p=1,z=-7,C=1,q=2\033\\\033[2;3Hab");
	show(&v);
	if (!w.klayered || s.owner == nullptr || count_placeholders(desk, W, H) != 0)
		fail("a placement under the text makes its window layered");
	if (desk[(iy + 1) * W + ix + 2].ch != 'a')
		fail("the text over a layered picture is in the cells");
	const unsigned		hid = w.kimg->at(5).hid;
	const std::string	img = "\033_Ga=p,i=" + std::to_string(hid);
	const std::string	whole = at(2, 1) + img +
	    ",p=1,x=0,y=0,w=64,h=64,z=-7,C=1,q=2\033\\";

	if (hid == 0 || count_in(frame, n, "\033_Ga=t,i=" + std::to_string(hid) + ",") != 1 ||
	    count_in(frame, n, "\033_Ga=t") != 1)
		fail("a layered image is sent once, under its own id");
	if (count_in(frame, n, whole) != 1 || count_in(frame, n, "\033_Ga=p") != 1)
		fail("its placement is a real one, at its cell and z");
	if (count_in(frame, n, holder) != 0 || count_in(frame, n, "U=1") != 0)
		fail("a layered placement has no placeholder cells");
	if (v.limg_gen == nullptr || !v.limg_gen->contains(hid))
		fail("the terminal is recorded as holding the image");

	show(&v);
	if (n != 0)
		fail("nothing is sent while nothing changes");

	/* A sprite: another part of the image, part way into its cell. Made
	 * again under its id it moves; nothing is sent again or deleted. */
	put("\033[2;3H\033_Ga=p,i=5,p=1,x=16,y=8,w=20,h=20,X=5,Y=9,z=-7,C=1,q=2\033\\");
	show(&v);
	if (count_in(frame, n, at(2, 1) + img + ",p=1,x=16,y=8,w=20,h=20,X=5,Y=9,z=-7,C=1,q=2\033\\") != 1)
		fail("a placement is made where and as the program made it");
	if (count_in(frame, n, "\033_Ga=t") != 0 || count_in(frame, n, "\033_Ga=d") != 0)
		fail("a moved placement sends no pixels and deletes nothing");

	/* Two of one image at once, each its own placement of the one copy. */
	put("\033[4;1H\033_Ga=p,i=5,p=2,x=0,y=0,w=16,h=32,z=3,C=1,q=2\033\\");
	show(&v);
	if (count_in(frame, n, at(0, 3) + img + ",p=2,x=0,y=0,w=16,h=32,z=3,C=1,q=2\033\\") != 1 ||
	    count_in(frame, n, "\033_Ga=t") != 0)
		fail("a second placement shares the image the terminal holds");
	put("\033_Ga=d,d=i,i=5,p=2,q=2\033\\");
	show(&v);
	if (count_in(frame, n, "\033_Ga=d,d=i,i=" + std::to_string(hid) + ",p=2,q=2\033\\") != 1 ||
	    count_in(frame, n, "\033_Ga=d") != 1)
		fail("a placement deleted goes from the terminal, its image stays");

	/*
	 * Under another window's frame: the picture at the window's corner
	 * shows its whole top row of cells, and the two left cells of the
	 * second. Two pieces, each with its part of the source.
	 */
	put("\033[1;1H\033_Ga=p,i=5,p=1,z=-7,C=1,q=2\033\\");
	memset(&top, 0, sizeof top);
	top.id = 2;
	top.session = &s;
	top.pty_fd = -1;
	top.framed = 1;
	top.z = w.z + 1;
	top.geom = { ix + 2, iy + 1, 10, 5 };
	dv_screen_init(&top.screen, 10, 5);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &top, entry);
	show(&v);
	if (count_in(frame, n, at(0, 0) + img + ",p=1,x=0,y=0,w=64,h=32,z=-7,C=1,q=2\033\\") != 1)
		fail("the part above the frame is one piece");
	if (count_in(frame, n, at(0, 1) + img + ",p=2,x=0,y=32,w=32,h=32,z=-7,C=1,q=2\033\\") != 1)
		fail("the part beside the frame is another, with its part of the source");
	if (count_in(frame, n, "\033_Ga=p") != 2 || count_in(frame, n, "\033_Ga=t") != 0)
		fail("a picture under a frame is cut into the pieces that show");

	/* Fitted to cells (c, r), the pieces are whole cells with their share
	 * of the source: eight cells by two, so a cell is eight pixels wide. */
	put("\033[1;1H\033_Ga=p,i=5,p=1,c=8,r=2,z=-7,C=1,q=2\033\\");
	show(&v);
	if (count_in(frame, n, at(0, 0) + img + ",p=1,x=0,y=0,w=64,h=32,c=8,r=1,z=-7,C=1,q=2\033\\") != 1 ||
	    count_in(frame, n, at(0, 1) + img + ",p=2,x=0,y=32,w=16,h=32,c=2,r=1,z=-7,C=1,q=2\033\\") != 1)
		fail("a fitted placement is cut by the cell, each piece with its share");
	top.geom = { ix + 20, iy + 5, 10, 3 };		/* clear of it */
	show(&v);
	if (count_in(frame, n, at(0, 0) + img + ",p=1,c=8,r=2,z=-7,C=1,q=2\033\\") != 1 ||
	    count_in(frame, n, "\033_Ga=d,d=i,i=" + std::to_string(hid) + ",p=2,q=2\033\\") != 1)
		fail("shown whole again it is one placement, as the program said it");
	put("\033[1;1H\033_Ga=p,i=5,p=1,z=-7,C=1,q=2\033\\");
	show(&v);

	/* Wholly covered: nothing of it is shown, and nothing is sent for it. */
	top.framed = 0;
	top.geom = { 0, 0, W, H };
	dv_screen_resize(&top.screen, W, H);
	show(&v);
	if (count_in(frame, n, "\033_Ga=d,d=i,i=" + std::to_string(hid) + ",p=1,q=2\033\\") != 1 ||
	    count_in(frame, n, "\033_Ga=p") != 0 || count_in(frame, n, "\033_Ga=t") != 0)
		fail("a window wholly covered shows none of its pictures");
	put("\033[1;1H\033_Ga=p,i=5,p=1,x=1,y=1,w=9,h=9,z=-7,C=1,q=2\033\\");
	show(&v);
	if (count_in(frame, n, "\033_G") != 0)
		fail("what a covered window draws sends nothing to the terminal");
	put("\033[1;1H\033_Ga=p,i=5,p=1,z=-7,C=1,q=2\033\\");
	TAILQ_REMOVE(&s.desktop.windows, &top, entry);
	show(&v);
	if (count_in(frame, n, at(0, 0) + img + ",p=1,x=0,y=0,w=64,h=64,z=-7,C=1,q=2\033\\") != 1 ||
	    count_in(frame, n, "\033_Ga=t") != 0)
		fail("uncovered, it is placed again from the image the terminal holds");

	/* A menu over the window takes its cells like a frame does. */
	s.menu_open = 1;
	show(&v);
	s.menu_open = 0;
	{
		bool	cut = false;

		for (unsigned i = 0; i < W * H; i++)
			if ((*s.owner)[i] != w.id)
				cut = true;
		if (!cut)
			fail("a menu takes the cells it covers from the window under it");
	}
	show(&v);

	/* A screen erased loses its placements: all of it goes again. */
	v.full = 1;
	show(&v);
	if (count_in(frame, n, "\033_Ga=t,i=" + std::to_string(hid) + ",") != 1 ||
	    count_in(frame, n, at(0, 0) + img + ",p=1,x=0,y=0,w=64,h=64,z=-7,C=1,q=2\033\\") != 1)
		fail("a full frame sends the image and its placement again");
	{
		const void	*clear = memmem(frame, n, "\033[2J", 4);
		const void	*sent = memmem(frame, n, "\033_Ga=t", 6);

		if (clear == nullptr || sent == nullptr || sent < clear)
			fail("they are sent after the screen is erased");
	}

	/* A terminal that draws sixel has no placements to layer: the picture
	 * is cells for it, and one under the text leaves the cells that have
	 * text (xy now, and the ab written at the start, which it lies under). */
	put("\033[1;1Hxy");
	show(&plain);
	if (count_in(frame, n, "\033_G") != 0 || count_in(frame, n, "xy") != 1 ||
	    count_in(frame, n, "ab") != 1)
		fail("a sixel terminal is shown the text a picture lies under");
	if (plain.limg_gen != nullptr || plain.layers != nullptr)
		fail("it holds no layered images");

	/* Two pictures on one cell, with no z: layered by the overlap alone. */
	put("\033_Ga=d,d=A,q=2\033\\\033[2J");
	show(&v);
	if (w.klayered || s.owner != nullptr ||
	    count_in(frame, n, "\033_Ga=d,d=I,i=" + std::to_string(hid) + ",q=2\033\\") != 1 ||
	    !v.limg_gen->empty())
		fail("with its pictures gone the window is plain, and the image leaves the terminal");
	put("\033_Ga=t,f=32,s=32,v=32,i=8,q=2;QUJDRA==\033\\");
	put("\033_Ga=t,f=32,s=32,v=32,i=9,q=2;QUJDRA==\033\\");
	put("\033[1;1H\033_Ga=p,i=8,C=1,q=2\033\\");
	put("\033[3;1H\033_Ga=p,i=9,C=1,q=2\033\\");
	show(&v);
	if (w.klayered || count_placeholders(desk, W, H) != 4)
		fail("pictures side by side over the text are cells still");
	put("\033[1;2H\033_Ga=p,i=9,p=4,C=1,q=2\033\\");
	show(&v);
	if (!w.klayered || count_placeholders(desk, W, H) != 0 ||
	    count_in(frame, n, "\033_Ga=p,i=") != 3 || count_in(frame, n, holder) != 0)
		fail("pictures that overlap are layered, all of the window's");
	if (count_in(frame, n, "\033_Ga=d,d=I,i=") != 2)
		fail("the copies made for its cells are dropped when it is layered");

	free(frame);
	free(desk);
	dv_view_free(&v);
	dv_view_free(&plain);
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_screen_free(&top.screen);
	dv_buf_free(&w.cap);
	free(s.xcur_map);
	delete s.owner;
	unsetenv("ODIN_SEAT_KITTY_MEDIUM");
}

/* Transmit w x h RGBA pixels as image `id`, in chunks as a program would. */
static void
put_rgba(struct dv_window *w, unsigned id, unsigned iw, unsigned ih,
    const std::vector<unsigned char> &px)
{
	static const char	tab[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string		b64;

	for (size_t i = 0; i < px.size(); i += 3) {
		unsigned	v = (unsigned)px[i] << 16, k = (unsigned)(px.size() - i);

		if (k > 1)
			v |= (unsigned)px[i + 1] << 8;
		if (k > 2)
			v |= px[i + 2];
		b64 += tab[v >> 18];
		b64 += tab[(v >> 12) & 63];
		b64 += k > 1 ? tab[(v >> 6) & 63] : '=';
		b64 += k > 2 ? tab[v & 63] : '=';
	}
	for (size_t at = 0; at < b64.size(); at += 4096) {
		bool		last = at + 4096 >= b64.size();
		std::string	cmd = at == 0 ?
		    "\033_Ga=t,f=32,s=" + std::to_string(iw) + ",v=" + std::to_string(ih) +
		    ",i=" + std::to_string(id) + ",q=2,m=" + (last ? "0" : "1") + ";" :
		    std::string("\033_Gm=") + (last ? "0" : "1") + ";";

		cmd += b64.substr(at, 4096) + "\033\\";
		dv_vt_write(w, cmd.data(), cmd.size());
	}
}

/*
 * Two terminals on one session, one with cells half the size. Programs draw
 * for the larger cell; the smaller is sent each picture scaled to its own,
 * and its placements in its own pixels: the same cells, half the lengths.
 */
static void
test_layers_scaled(void)
{
	struct dv_session	 s;
	struct dv_window	 w, top;
	struct dv_view		 big{}, half{};
	constexpr unsigned	 W = 40, H = 12;
	unsigned char		*frame = nullptr;
	size_t			 n;

	session_init(&s, &w, W, H);
	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	const unsigned	scw = s.desktop.xpixel, sch = s.desktop.ypixel;

	if (scw != 16 || sch != 32)
		fail("the test session's cell is 16 by 32");
	big.gfx = half.gfx = 1;
	big.cell_w = big.scell_w = half.scell_w = scw;
	big.cell_h = big.scell_h = half.scell_h = sch;
	half.cell_w = scw / 2;
	half.cell_h = sch / 2;
	if (dv_view_scaled(&big) || !dv_view_scaled(&half))
		fail("a terminal is scaled when its cell is not the session's");
	const int	framed = dv_window_framed(&w);
	const int	ix = w.geom.x + framed, iy = w.geom.y + framed;
	auto at = [ix, iy](unsigned col, unsigned row) {
		return ("\033[" + std::to_string(iy + (int)row + 1) + ";" +
		    std::to_string(ix + (int)col + 1) + "H");
	};
	auto put = [&w](const std::string &t) { dv_vt_write(&w, t.data(), t.size()); };
	auto *desk = dv_allocn<dv_cell>(W * H);
	auto show = [&](struct dv_view *view) {
		free(frame);
		frame = nullptr;
		dv_compose_cells(&s, desk);
		n = dv_compose_frame(&s, desk, view, &frame);
	};
	std::vector<unsigned char>	px(64 * 64 * 4, 255);

	for (unsigned y = 0; y < 64; y++)
		for (unsigned x = 0; x < 64; x++) {
			px[(y * 64 + x) * 4 + 0] = x < 32 ? 200 : 10;
			px[(y * 64 + x) * 4 + 1] = (unsigned char)(y * 3);
			px[(y * 64 + x) * 4 + 2] = x < 32 ? 10 : 200;
		}
	put_rgba(&w, 5, 64, 64, px);
	put("\033[1;1H\033_Ga=p,i=5,p=1,X=4,Y=8,z=-1,C=1,q=2\033\\");
	show(&big);
	const unsigned		hid = w.kimg->at(5).hid;
	const std::string	img = "\033_Ga=p,i=" + std::to_string(hid);

	if (count_in(frame, n, "s=64,v=64") != 1 ||
	    count_in(frame, n, at(0, 0) + img + ",p=1,x=0,y=0,w=64,h=64,X=4,Y=8,z=-1,C=1,q=2\033\\") != 1)
		fail("the terminal with the session's cell gets the picture as drawn");
	show(&half);
	if (count_in(frame, n, "s=32,v=32") != 1 || count_in(frame, n, "s=64,v=64") != 0)
		fail("a terminal with half the cell is sent the picture at half the size");
	if (count_in(frame, n, at(0, 0) + img + ",p=1,x=0,y=0,w=32,h=32,X=2,Y=4,z=-1,C=1,q=2\033\\") != 1 ||
	    count_in(frame, n, "U=1") != 0)
		fail("and a real placement of it, in its own pixels");

	/* A sprite out of the sheet: each edge of its rectangle scales. */
	put("\033[1;1H\033_Ga=p,i=5,p=1,x=16,y=8,w=20,h=21,z=-1,C=1,q=2\033\\");
	show(&half);
	if (count_in(frame, n, at(0, 0) + img + ",p=1,x=8,y=4,w=10,h=11,z=-1,C=1,q=2\033\\") != 1 ||
	    count_in(frame, n, "\033_Ga=t") != 0)
		fail("a source rectangle is scaled with the picture, which is not sent again");
	show(&big);

	/* Under a frame, the pieces are cut in its pixels and still meet. */
	put("\033[1;1H\033_Ga=p,i=5,p=1,z=-1,C=1,q=2\033\\");
	memset(&top, 0, sizeof top);
	top.id = 2;
	top.session = &s;
	top.pty_fd = -1;
	top.framed = 1;
	top.z = w.z + 1;
	top.geom = { ix + 2, iy + 1, 10, 5 };
	dv_screen_init(&top.screen, 10, 5);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &top, entry);
	show(&half);
	if (count_in(frame, n, at(0, 0) + img + ",p=1,x=0,y=0,w=32,h=16,z=-1,C=1,q=2\033\\") != 1 ||
	    count_in(frame, n, at(0, 1) + img + ",p=2,x=0,y=16,w=16,h=16,z=-1,C=1,q=2\033\\") != 1 ||
	    count_in(frame, n, "\033_Ga=p") != 2)
		fail("a scaled picture under a frame is cut into its own pixels");
	show(&big);
	if (count_in(frame, n, at(0, 1) + img + ",p=2,x=0,y=32,w=32,h=32,z=-1,C=1,q=2\033\\") != 1)
		fail("while the other terminal's pieces are in the session's");

	/* Fitted to cells, the terminal does the scaling: only the source
	 * rectangle follows the picture it holds, and both counts are said. */
	TAILQ_REMOVE(&s.desktop.windows, &top, entry);
	put("\033[1;1H\033_Ga=p,i=5,p=1,c=8,z=-1,C=1,q=2\033\\");
	show(&half);
	if (count_in(frame, n, at(0, 0) + img + ",p=1,c=8,r=" +
	    std::to_string(w.screen.kpl->begin()->second.rows) + ",z=-1,C=1,q=2\033\\") != 1)
		fail("a fitted placement covers the cells it covers everywhere");

	/* Its font changed: what it holds is at the scale that was. */
	half.cell_w = 12;
	half.cell_h = 24;
	half.full = 1;			/* as the server marks it */
	put("\033[1;1H\033_Ga=p,i=5,p=1,z=-1,C=1,q=2\033\\");
	show(&half);
	if (count_in(frame, n, "s=48,v=48") != 1 ||
	    count_in(frame, n, at(0, 0) + img + ",p=1,x=0,y=0,w=48,h=48,z=-1,C=1,q=2\033\\") != 1)
		fail("another cell size sends the picture again at the new scale");
	dv_kitty_packs_forget(&s);
	if (w.kimg->at(5).packs != nullptr)
		fail("forms made for the terminals that were are forgotten");

	/* Pixels that cannot be decoded cannot be scaled: nothing is placed
	 * where a picture of the wrong size would show the wrong part. */
	put("\033_Ga=d,d=A,q=2\033\\");
	put("\033_Ga=t,f=32,s=64,v=64,i=6,q=2;QUJDRA==\033\\");
	put("\033[1;1H\033_Ga=p,i=6,p=1,z=-1,C=1,q=2\033\\");
	show(&half);
	if (count_in(frame, n, "\033_Ga=t") != 0 || count_in(frame, n, "\033_Ga=p") != 0)
		fail("a picture that cannot be scaled is not shown at the wrong size");

	free(frame);
	free(desk);
	dv_view_free(&big);
	dv_view_free(&half);
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_screen_free(&top.screen);
	dv_buf_free(&w.cap);
	free(s.xcur_map);
	delete s.owner;
	unsetenv("ODIN_SEAT_KITTY_MEDIUM");
}

/*
 * A framed window carries its controls on its top edge: four arrows that
 * send it to a half of the desktop and a mark that says whether it is one of
 * the selected. Selected windows show it in their frame, and the status line
 * says who typing reaches while broadcast is on.
 */
static void
test_window_controls(void)
{
	struct dv_session	 s;
	struct dv_window	 w, a, b;
	constexpr unsigned	 W = 80, H = 24;
	auto			*desk = dv_allocn<dv_cell>(W * H);
	auto row_text = [&](unsigned y) {
		std::string	t;

		for (unsigned x = 0; x < W; x++)
			t += desk[y * W + x].ch < 0x80 ? (char)desk[y * W + x].ch : '?';
		return (t);
	};

	session_init(&s, &w, W, H);
	for (auto *it : { &a, &b }) {
		memset(it, 0, sizeof *it);
		it->id = it == &a ? 2 : 3;
		it->session = &s;
		it->pty_fd = -1;
		it->framed = 1;
		it->z = w.z + (int)it->id;
		it->geom = { it == &a ? 2 : 40, 3, 30, 6 };
		dv_strlcpy(it->cmd, "a-rather-long-program-name-here", sizeof it->cmd);
		dv_screen_init(&it->screen, 30, 6);
		TAILQ_INSERT_TAIL(&s.desktop.windows, it, entry);
	}
	s.desktop.focused = &b;
	dv_compose_cells(&s, desk);
	{
		/* a's frame is 32 wide, from column 2: controls at 26-29 and 31. */
		const struct dv_cell	*top = &desk[3 * W + 2];

		if (top[24].ch != 0x2190 || top[25].ch != 0x2191 || top[26].ch != 0x2193 ||
		    top[27].ch != 0x2192 || top[29].ch != 0x25CB)
			fail("a frame shows its four halves and an empty selection mark");
		if (top[23].ch < 0x80 && top[23].ch != ' ' && top[23].ch != 0)
			fail("a long title stops short of the controls");
		if (top[0].fg != 6 || desk[3 * W + 40].fg != 11)
			fail("frames are cyan, the focused one yellow");
	}
	a.selected = 1;
	dv_compose_cells(&s, desk);
	if (desk[3 * W + 2 + 29].ch != 0x25CF || desk[3 * W + 2].fg != 13)
		fail("a selected window fills its mark and colours its frame");

	/* Who typing reaches, on the status line. */
	s.broadcast = 1;
	dv_compose_cells(&s, desk);
	if (row_text(H - 1).find("this window is not selected") == std::string::npos)
		fail("broadcast from outside the selection stays where it is typed");
	b.selected = 1;
	dv_compose_cells(&s, desk);
	if (row_text(H - 1).find("the 2 selected windows") == std::string::npos)
		fail("broadcast inside the selection names how many it reaches");
	a.selected = b.selected = 0;
	dv_compose_cells(&s, desk);
	if (row_text(H - 1).find("every window") == std::string::npos)
		fail("with none selected broadcast is to every window");
	s.broadcast = 0;
	s.input_mode = DV_IN_TILE;
	dv_compose_cells(&s, desk);
	if (row_text(H - 1).find("LAYOUT") == std::string::npos)
		fail("choosing a layout says which letters there are");
	s.input_mode = DV_IN_NORMAL;

	/* A frame too narrow for them has a title and no controls. */
	a.geom.cols = 10;
	dv_screen_resize(&a.screen, 10, 6);
	dv_compose_cells(&s, desk);
	for (unsigned x = 0; x < 12; x++)
		if (desk[3 * W + 2 + x].ch == 0x2190 || desk[3 * W + 2 + x].ch == 0x25CB)
			fail("a narrow frame has no controls");

	free(desk);
	dv_screen_free(&w.screen);
	dv_screen_free(&a.screen);
	dv_screen_free(&b.screen);
	free(s.xcur_map);
	delete s.owner;
}

/*
 * Colours and pixels in a wider gamut reach a terminal that knows of them
 * as they are, and one that does not as the nearest sRGB.
 */
static void
test_gamut_views(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_view		 wide{}, plain{};
	constexpr unsigned	 W = 20, H = 6;
	unsigned char		*frame = nullptr;
	size_t			 n;
	auto			*desk = dv_allocn<dv_cell>(W * H);

	session_init(&s, &w, W, H);
	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	s.silk_any = s.silk_wg = 1;
	dv_strset(&s.silk_format, "24,32,100");
	dv_strset(&s.silk_media, "d,f,t,s");
	wide.gfx = plain.gfx = 1;
	wide.wg = 1;
	auto put = [&w](const std::string &t) { dv_vt_write(&w, t.data(), t.size()); };
	auto show = [&](struct dv_view *view) {
		free(frame);
		frame = nullptr;
		dv_compose_cells(&s, desk);
		n = dv_compose_frame(&s, desk, view, &frame);
	};
	unsigned char	p3[3] = { 100, 220, 60 }, same[3] = { 100, 220, 60 };

	dv_gamut_to_srgb(1, p3);
	if (p3[0] >= 100 || p3[1] <= 220 || memcmp(p3, same, 3) == 0)
		fail("a Display P3 green is a stronger green in sRGB numbers");
	dv_gamut_to_srgb(0, same);
	if (same[0] != 100 || same[1] != 220 || same[2] != 60)
		fail("an sRGB colour is left as it is");
	{
		unsigned char	gray[3] = { 128, 128, 128 }, red[3] = { 255, 0, 0 };

		dv_gamut_to_srgb(2, gray);
		dv_gamut_to_srgb(1, red);
		if (gray[0] != 128 || gray[1] != 128 || gray[2] != 128)
			fail("grays are the same in every gamut");
		if (red[0] != 255 || red[1] != 0 || red[2] != 0)
			fail("what sRGB cannot show is clipped to what it can");
	}

	put("\033[38:2:1:100:220:60mX\033[m");
	show(&wide);
	if (count_in(frame, n, "38:2:1:100:220:60") != 1)
		fail("a terminal that knows gamuts is sent the colour in its own");
	show(&plain);
	if (count_in(frame, n, "38:2:1:") != 0 ||
	    count_in(frame, n, "38;2;" + std::to_string(p3[0]) + ";" +
	    std::to_string(p3[1]) + ";" + std::to_string(p3[2])) != 1)
		fail("one that does not is sent the nearest sRGB colour");

	/* The same for an image's pixels (G=1): 100,220,60 is ZNw8. */
	put("\033[2;1H\033_Ga=T,f=24,s=1,v=1,i=3,G=1,C=1,q=2;ZNw8\033\\");
	wide.full = plain.full = 1;
	show(&wide);
	if (count_in(frame, n, "G=1") != 1 || count_in(frame, n, ";ZNw8\033") != 1)
		fail("pixels in a gamut go as they are to a terminal that takes them");
	show(&plain);
	if (count_in(frame, n, "\033_Ga=t") != 1 || count_in(frame, n, "G=1") != 0 ||
	    count_in(frame, n, ";ZNw8\033") != 0)
		fail("and as sRGB pixels, with no gamut key, to one that does not");

	free(frame);
	free(desk);
	dv_view_free(&wide);
	dv_view_free(&plain);
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);
	free(s.silk_format);
	free(s.silk_media);
	free(s.xcur_map);
	delete s.owner;
	unsetenv("ODIN_SEAT_KITTY_MEDIUM");
}

static std::string
unbase64(const std::string &in)
{
	std::string	out;
	unsigned	acc = 0;
	int		bits = 0;

	for (char c : in) {
		int	v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 :
			    c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;

		if (v < 0)
			continue;
		acc = (acc << 6) | (unsigned)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += (char)((acc >> bits) & 0xff);
		}
	}
	return (out);
}

/*
 * An HDR picture (f=64, half floats in linear light) is scaled for a
 * terminal with a smaller cell in its own numbers: the mean of an area is
 * its light, and what is brighter than white stays so.
 */
static void
test_hdr_scaled(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_view		 half{};
	constexpr unsigned	 W = 20, H = 6;
	unsigned char		*frame = nullptr;
	size_t			 n;
	auto			*desk = dv_allocn<dv_cell>(W * H);
	/* 4 by 2: red at 1 and at 3 times white, then green at 2, twice. */
	static const unsigned char row[32] = {
		0x00, 0x3c, 0, 0, 0, 0, 0x00, 0x3c,	0x00, 0x42, 0, 0, 0, 0, 0x00, 0x3c,
		0, 0, 0x00, 0x40, 0, 0, 0x00, 0x3c,	0, 0, 0x00, 0x40, 0, 0, 0x00, 0x3c,
	};
	static const unsigned char want[16] = {
		0x00, 0x40, 0, 0, 0, 0, 0x00, 0x3c,	0, 0, 0x00, 0x40, 0, 0, 0x00, 0x3c,
	};
	std::vector<unsigned char>	px(row, row + 32);

	px.insert(px.end(), row, row + 32);
	session_init(&s, &w, W, H);
	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	s.silk_any = 1;
	dv_strset(&s.silk_format, "24,32,64,100");
	dv_strset(&s.silk_media, "d,f,t,s");
	half.gfx = 1;
	half.hdr = 1;			/* it takes HDR pixels itself */
	half.scell_w = s.desktop.xpixel;
	half.scell_h = s.desktop.ypixel;
	half.cell_w = half.scell_w / 2;
	half.cell_h = half.scell_h / 2;
	{
		static const char	tab[] =
		    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string		b64;

		for (size_t i = 0; i < px.size(); i += 3) {
			unsigned	v = (unsigned)px[i] << 16, k = (unsigned)(px.size() - i);

			if (k > 1)
				v |= (unsigned)px[i + 1] << 8;
			if (k > 2)
				v |= px[i + 2];
			b64 += tab[v >> 18];
			b64 += tab[(v >> 12) & 63];
			b64 += k > 1 ? tab[(v >> 6) & 63] : '=';
			b64 += k > 2 ? tab[v & 63] : '=';
		}
		std::string	cmd = "\033_Ga=t,f=64,s=4,v=2,i=9,q=2;" + b64 + "\033\\" +
		    "\033[1;1H\033_Ga=p,i=9,p=1,z=-1,C=1,q=2\033\\";

		dv_vt_write(&w, cmd.data(), cmd.size());
	}
	dv_compose_cells(&s, desk);
	n = dv_compose_frame(&s, desk, &half, &frame);
	std::string	out((const char *)frame, n);
	size_t		at = out.find("\033_Ga=t,i=");
	size_t		semi = at == std::string::npos ? at : out.find(';', at);
	size_t		end = semi == std::string::npos ? semi : out.find("\033\\", semi);

	if (end == std::string::npos)
		fail("an HDR picture is sent to a terminal with a smaller cell");
	else {
		std::string	keys = out.substr(at, semi - at);
		std::string	raw = unbase64(out.substr(semi + 1, end - semi - 1));

		if (keys.find("f=64") == std::string::npos ||
		    keys.find("s=2,v=1") == std::string::npos)
			fail("it goes as HDR still, at half the size");
		if (keys.find("o=z") != std::string::npos) {
			unsigned char	plain[64];
			uLongf		len = sizeof plain;

			if (uncompress(plain, &len, (const Bytef *)raw.data(),
			    (uLong)raw.size()) != Z_OK)
				len = 0;
			raw.assign((const char *)plain, len);
		}
		if (raw.size() != sizeof want || memcmp(raw.data(), want, sizeof want) != 0)
			fail("each pixel is the mean of the light it covers, above white included");
	}
	if (count_in(frame, n, ",p=1,x=0,y=0,w=2,h=1,z=-1,C=1,q=2\033\\") != 1)
		fail("and is placed at that size");

	free(frame);
	free(desk);
	dv_view_free(&half);
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);
	free(s.silk_format);
	free(s.silk_media);
	free(s.xcur_map);
	delete s.owner;
	unsetenv("ODIN_SEAT_KITTY_MEDIUM");
}

/*
 * The first image transmitted in a frame: its keys, and its bytes with
 * base64, chunking and deflate undone.
 */
static std::string
sent_image(const unsigned char *frame, size_t n, std::string *keys)
{
	std::string	out((const char *)frame, n), b64, raw;
	size_t		at = out.find("\033_Ga=t,i=");

	keys->clear();
	if (at == std::string::npos)
		return ("");
	size_t	semi = out.find(';', at);

	*keys = out.substr(at, semi - at);
	for (;;) {
		size_t	end = out.find("\033\\", semi);

		if (end == std::string::npos)
			break;
		b64 += out.substr(semi + 1, end - semi - 1);
		if (out.compare(end + 2, 5, "\033_Gm=") != 0)
			break;
		semi = out.find(';', end + 2);
	}
	raw = unbase64(b64);
	if (keys->find("o=z") != std::string::npos) {
		std::string	plain(1 << 22, '\0');
		uLongf		len = plain.size();

		if (uncompress((Bytef *)plain.data(), &len, (const Bytef *)raw.data(),
		    (uLong)raw.size()) != Z_OK)
			len = 0;
		plain.resize(len);
		raw.swap(plain);
	}
	return (raw);
}

/*
 * A vector scene reaches a terminal that draws scenes as its records, and
 * one that does not as pixels: drawn here, at the size that terminal shows
 * it, and drawn again when the scene is edited. HDR pixels reach a terminal
 * that takes none as what eight bits show of them.
 */
static void
test_scene_drawn(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_view		 silkie{}, kitty{}, small{}, sixel{};
	constexpr unsigned	 W = 20, H = 6;
	unsigned char		*frame = nullptr;
	size_t			 n;
	auto			*desk = dv_allocn<dv_cell>(W * H);
	std::string		 keys, px;

	session_init(&s, &w, W, H);
	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	silkie.gfx = kitty.gfx = small.gfx = 1;
	silkie.vector = silkie.hdr = 1;
	sixel.sixel = 1;
	small.scell_w = s.desktop.xpixel;
	small.scell_h = s.desktop.ypixel;
	small.cell_w = small.scell_w / 2;
	small.cell_h = small.scell_h / 2;
	auto put = [&w](const std::string &t) { dv_vt_write(&w, t.data(), t.size()); };
	auto show = [&](struct dv_view *view) {
		free(frame);
		frame = nullptr;
		dv_compose_cells(&s, desk);
		n = dv_compose_frame(&s, desk, view, &frame);
		px = sent_image(frame, n, &keys);
	};
	auto b64 = [](const std::string &in) {
		static const char	tab[] =
		    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string		out;

		for (size_t i = 0; i < in.size(); i += 3) {
			unsigned	v = (unsigned)(unsigned char)in[i] << 16;
			unsigned	k = (unsigned)(in.size() - i);

			if (k > 1)
				v |= (unsigned)(unsigned char)in[i + 1] << 8;
			if (k > 2)
				v |= (unsigned char)in[i + 2];
			out += tab[v >> 18];
			out += tab[(v >> 12) & 63];
			out += k > 1 ? tab[(v >> 6) & 63] : '=';
			out += k > 2 ? tab[v & 63] : '=';
		}
		return (out);
	};
	auto pixel = [&px](unsigned iw, unsigned x, unsigned y) {
		size_t	at = ((size_t)y * iw + x) * 4;

		return (at + 4 <= px.size() ? std::string(px, at, 4) : std::string());
	};
	const std::string	red("\xff\x00\x00\xff", 4), blue("\x00\x00\xff\xff", 4);
	const std::string	clear("\x00\x00\x00\x00", 4);

	/* 32 by 32: the left half red, the right half nothing. Under the text. */
	put("\033_Ga=t,f=200,i=4,s=32,v=32,q=2;" +
	    b64("p id=1 d=M0,0L16,0L16,32L0,32Z fill=#ff0000\n") + "\033\\");
	put("\033[1;1H\033_Ga=p,i=4,p=1,z=-1,C=1,q=2\033\\");
	show(&silkie);
	if (keys.find("f=200") == std::string::npos ||
	    px != "p id=1 d=M0,0L16,0L16,32L0,32Z fill=#ff0000\n")
		fail("a terminal that draws scenes is sent the records");
	show(&kitty);
	if (keys.find("f=200") != std::string::npos || keys.find("f=32") == std::string::npos ||
	    keys.find("s=32,v=32") == std::string::npos || px.size() != 32 * 32 * 4)
		fail("a terminal that does not is sent the scene as pixels");
	if (pixel(32, 4, 16) != red || pixel(32, 28, 16) != clear ||
	    pixel(32, 15, 0) != red || pixel(32, 16, 31) != clear)
		fail("drawn as the scene says: its path filled, the rest left clear");
	if (count_in(frame, n, ",p=1,x=0,y=0,w=32,h=32,z=-1,C=1,q=2\033\\") != 1 ||
	    count_in(frame, n, "\033_Ga=e") != 0)
		fail("and placed like any picture");
	show(&small);
	if (keys.find("s=16,v=16") == std::string::npos || px.size() != 16 * 16 * 4 ||
	    pixel(16, 2, 8) != red || pixel(16, 14, 8) != clear)
		fail("for a smaller cell it is drawn at that size, not drawn and then scaled");
	show(&sixel);
	if (count_in(frame, n, "\033P") == 0 || count_in(frame, n, "\033_G") != 0)
		fail("a sixel terminal is shown the scene drawn, as sixel");
	show(&sixel);
	if (n != 0)
		fail("and nothing more while nothing changes");
	/* Moved three pixels within its cell: no cell is another picture's,
	 * but the pixels in them are others, and they are drawn again. */
	put("\033[1;1H\033_Ga=p,i=4,p=1,X=3,z=-1,C=1,q=2\033\\");
	show(&sixel);
	if (count_in(frame, n, "\033P") == 0)
		fail("a picture moved by pixels is drawn again for a sixel terminal");
	put("\033[1;1H\033_Ga=p,i=4,p=1,z=-1,C=1,q=2\033\\");

	/* An edit: the records to the one, the picture again to the other. */
	show(&silkie);
	show(&kitty);
	put("\033_Ga=e,i=4,q=2;" + b64("p id=1 d=M0,0L16,0L16,32L0,32Z fill=#0000ff\n") + "\033\\");
	show(&kitty);
	if (px.size() != 32 * 32 * 4 || pixel(32, 4, 16) != blue || pixel(32, 28, 16) != clear)
		fail("an edited scene is drawn again for the terminal holding it as pixels");

	/* A record the rasterizer will not have costs itself, not the scene.
	 * The text is set at the scene's corner, above what shows. */
	put("\033_Ga=e,i=4,q=2;" + b64("p id=2 d=M16,0Lbogus fill=#00ff00\n"
	    "t id=3 text=" + b64("{\"text\":\"hi\"}") + "\n") + "\033\\");
	show(&kitty);
	if (px.size() != 32 * 32 * 4 || pixel(32, 4, 16) != blue)
		fail("a record that cannot be drawn leaves the rest of the scene drawn");

	/* Text in a scene is set here, in a font of this machine, and drawn
	 * with the rest: an M in red on the right half, which was clear. A
	 * machine with no fonts at all has nothing to set it in. */
	put("\033_Ga=e,i=4,q=2;" + b64("t id=5 text=" + b64("{\"text\":\"M\",\"size\":28,"
	    "\"x\":17,\"y\":28,\"fill\":\"#ff0000\"}") + "\n") + "\033\\");
	show(&kitty);
	{
		std::string	r, why;
		unsigned	inked = 0;

		for (unsigned y = 0; y < 32 && px.size() == 32 * 32 * 4; y++)
			for (unsigned x = 16; x < 32; x++)
				inked += pixel(32, x, y) == red;
		if (!dv_vtext_outlines()) {
			/* An old HarfBuzz: text is measured, but has no outlines to
			 * be drawn with. The rest of the scene still is. */
			if (pixel(32, 4, 16) != blue)
				fail("a scene whose text cannot be drawn still draws the rest");
		} else if (dv_vtext_service("{\"op\":\"measure\",\"text\":\"M\"}", {}, &r, &why)) {
			if (inked < 20)
				fail("text in a scene is drawn, in a font of this machine");
			if (pixel(32, 4, 16) != blue)
				fail("and the rest of the scene with it");
		} else if (why.find("No fonts are installed") == std::string::npos)
			fail("text is set here unless the machine has no fonts");
	}

	/* HDR pixels: one at twice white and one at half, as eight bits. */
	{
		static const unsigned char hdr[16] = {
			0x00, 0x40, 0x00, 0x40, 0x00, 0x40, 0x00, 0x3c,	/* 2, 2, 2, 1 */
			0x00, 0x38, 0, 0, 0, 0, 0x00, 0x3c,		/* 0.5, 0, 0, 1 */
		};

		put("\033_Ga=d,d=A,q=2\033\\");
		put("\033_Ga=t,f=64,s=2,v=1,i=5,q=2;" +
		    b64(std::string((const char *)hdr, sizeof hdr)) + "\033\\");
		put("\033[1;1H\033_Ga=p,i=5,p=1,z=-1,C=1,q=2\033\\");
		show(&silkie);
		if (keys.find("f=64") == std::string::npos || px != std::string((const char *)hdr, sizeof hdr))
			fail("a terminal that takes HDR pixels is sent them as they are");
		show(&kitty);
		/* Opaque, so three bytes a pixel: white, then linear 0.5 as sRGB 188. */
		if (keys.find("f=64") != std::string::npos || keys.find("f=24") == std::string::npos ||
		    px != std::string("\xff\xff\xff\xbc\x00\x00", 6))
			fail("one that does not is sent what eight bits show of them");
	}

	/*
	 * Handed over in shared memory, which is what a terminal is given
	 * unless told otherwise, an image goes with its size in bytes (S): the
	 * object is as large as the pages holding it, and a scene is text to
	 * its end, so a terminal reading the padding too refuses the scene.
	 */
	{
		struct dv_view		 shared{};
		const std::string	 records = "p id=1 d=M0,0L16,0L16,32L0,32Z fill=#ff0000\n";

		setenv("ODIN_SEAT_KITTY_MEDIUM", "shm", 1);
		shared.gfx = shared.vector = shared.hdr = 1;
		put("\033_Ga=d,d=A,q=2\033\\");
		put("\033_Ga=t,f=200,i=8,s=32,v=32,q=2;" + b64(records) + "\033\\");
		put("\033[1;1H\033_Ga=p,i=8,p=1,z=-1,C=1,q=2\033\\");
		show(&shared);
		if (keys.find("t=s,S=" + std::to_string(records.size()) + ",") == std::string::npos)
			fail("an image in shared memory is sent with the size of its data");
		if (!px.empty() && px[0] == '/')
			shm_unlink(px.c_str());		/* no terminal here to take it */
		else
			fail("and names the object it is in");
		dv_view_free(&shared);
		setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	}

	free(frame);
	free(desk);
	for (auto *v : { &silkie, &kitty, &small, &sixel })
		dv_view_free(v);
	dv_flat_free(&s);
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);
	free(s.xcur_map);
	delete s.owner;
	unsetenv("ODIN_SEAT_KITTY_MEDIUM");
}

/*
 * A terminal that shows no pictures of any kind is drawn them as cells: an
 * upper half block whose foreground is the top half of the cell's part of
 * the picture and whose background is the bottom half. Text over a picture
 * keeps its text, on the picture's colour.
 */
static void
test_cell_pictures(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_view		 plain{}, none{};
	constexpr unsigned	 W = 20, H = 6;
	unsigned char		*frame = nullptr;
	size_t			 n;
	auto			*desk = dv_allocn<dv_cell>(W * H);
	const std::string	 upper = "\xe2\x96\x80", holder = "\xf4\x8e\xbb\xae";

	session_init(&s, &w, W, H);
	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	plain.mosaic = 1;
	auto put = [&w](const std::string &t) { dv_vt_write(&w, t.data(), t.size()); };
	auto show = [&](struct dv_view *view) {
		free(frame);
		frame = nullptr;
		dv_compose_cells(&s, desk);
		n = dv_compose_frame(&s, desk, view, &frame);
	};
	/* Two cells by two (a cell is 16 by 32): the top quarter red, the
	 * rest blue, so the first row of cells is red over blue. */
	std::vector<unsigned char>	px(32 * 64 * 4, 255);

	for (unsigned y = 0; y < 64; y++)
		for (unsigned x = 0; x < 32; x++) {
			px[(y * 32 + x) * 4 + 0] = y < 16 ? 255 : 0;
			px[(y * 32 + x) * 4 + 1] = 0;
			px[(y * 32 + x) * 4 + 2] = y < 16 ? 0 : 255;
		}
	put_rgba(&w, 5, 32, 64, px);
	put("\033[1;1H\033_Ga=p,i=5,p=1,C=1,q=2\033\\");
	show(&plain);
	if (w.klayered || count_in(frame, n, upper) != 4 || count_in(frame, n, holder) != 0 ||
	    count_in(frame, n, "\033_G") != 0 || count_in(frame, n, "\033P") != 0)
		fail("a picture is four cells of half blocks where no pictures show");
	if (count_in(frame, n, "38;2;255;0;0;48;2;0;0;255") != 1 ||
	    count_in(frame, n, "38;2;0;0;255;48;2;0;0;255") != 1)
		fail("each cell is the top and the bottom half of its part of the picture");
	show(&plain);
	if (n != 0)
		fail("nothing is sent while the picture does not change");
	show(&none);
	if (count_in(frame, n, upper) != 0)
		fail("a terminal not drawn pictures as cells gets none");

	/* A scene under the text: the text stays, on the picture's colour. */
	s.silk_any = 1;
	put("\033_Ga=d,d=A,q=2\033\\\033[2J");
	put("\033_Ga=t,f=200,i=6,s=64,v=64,q=2;"
	    "cCBpZD0xIGQ9TTAsMEw2NCwwTDY0LDY0TDAsNjRaIGZpbGw9IzAwZmYwMAo=\033\\");	/* a green square */
	put("\033[1;1H\033_Ga=p,i=6,p=1,z=-1,C=1,q=2\033\\\033[1;2Hab");
	plain.full = 1;
	show(&plain);
	if (!w.klayered || count_in(frame, n, "\033_G") != 0)
		fail("a layered window sends such a terminal no graphics either");
	/* Four cells by two, less the two that have text. */
	if (count_in(frame, n, upper) != 4 * 2 - 2 ||
	    count_in(frame, n, "38;2;0;255;0;48;2;0;255;0") < 1)
		fail("a scene is drawn and shown as cells");
	if (count_in(frame, n, "ab") != 1 || count_in(frame, n, "48;2;0;255;0\x6d\x61\x62") != 1)
		fail("text over a picture is text, on the colour of the picture");

	/* A sprite over the text hides it, as it would in pixels. */
	put_rgba(&w, 5, 32, 64, px);
	put("\033[1;2H\033_Ga=p,i=5,p=2,x=0,y=32,w=32,h=32,z=5,C=1,q=2\033\\");
	show(&plain);
	if (count_in(frame, n, "ab") != 0 || count_in(frame, n, "38;2;0;0;255;48;2;0;0;255") < 1)
		fail("a picture over the text takes its cells");
	{
		const struct dv_cell	*c = &desk[0 * W + 1];	/* the shared desk keeps the text */

		if (c->ch != 'a')
			fail("the cells other terminals are shown are not changed");
	}

	free(frame);
	free(desk);
	dv_view_free(&plain);
	dv_view_free(&none);
	dv_flat_free(&s);
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);
	free(s.xcur_map);
	delete s.owner;
	unsetenv("ODIN_SEAT_KITTY_MEDIUM");
}

/* More windows than the old compositor/switch-list storage could hold. */
static void
test_many_windows(void)
{
    struct dv_session s = {};
    dv_desktop_init(&s.desktop, size_of(80, 24));
    std::vector<dv_window> windows(400);
    for (size_t i = 0; i < windows.size(); i++) {
        auto &w = windows[i];
        w.id = (unsigned)i + 1;
        w.session = &s;
        w.pty_fd = -1;
        w.geom = {0, 0, 1, 1};
        w.z = (int)i;
        dv_screen_init(&w.screen, 1, 1);
        dv_vt_write(&w, i + 1 == windows.size() ? "@" : "x", 1);
        TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
    }
    auto ids = dv_switch_list(&s);
    if (ids.size() != windows.size() || ids.back() != windows.back().id)
        fail("the switch list retains every window beyond 256");
    auto desk = dv_allocn<dv_cell>(80 * 24);
    dv_compose_cells(&s, desk);
    if (desk[0].ch != '@')
        fail("the compositor paints the topmost window beyond 256");
    s.switch_open = 1;
    s.switch_sel = SIZE_MAX;
    dv_compose_cells(&s, desk); /* a stale selection after another client closes windows */
    free(desk);
    for (auto &w : windows)
        dv_screen_free(&w.screen);
    free(s.xcur_map);
}

/*
 * A cell's stereo depth reaches a client that listed depth.1 and no other,
 * follows the SGR 0 that resets it, repaints when only it changed, and the
 * terminal is back on its plane when the frame ends.
 */
static void
test_depth_paint(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	struct dv_view		 vd, vp;
	unsigned char		*frame = nullptr;
	size_t			 n;
	unsigned		 W = 80, H = 24;
	const char		*set = "\033]777;depth;v=1:op=set:d=-150:y=0\033\\";
	const char		*reset = "\033]777;depth;v=1:op=reset\033\\";
	const char		*text = "\033[H\033]777;depth;v=1:op=set:d=-150\033\\if"
	    "\033[1mx\033]777;depth;v=1:op=reset\033\\ y";

	memset(&s, 0, sizeof s);
	dv_desktop_init(&s.desktop, size_of(W, H));
	memset(&w, 0, sizeof w);
	w.id = 1;
	w.session = &s;
	w.pty_fd = -1;
	w.framed = 0;
	w.geom.cols = W;
	w.geom.rows = H;
	dv_screen_init(&w.screen, W, H);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	dv_vt_write(&w, text, strlen(text));
	desk = dv_allocn<dv_cell>((size_t)W * H);
	dv_compose_cells(&s, desk);
	memset(&vd, 0, sizeof vd);
	memset(&vp, 0, sizeof vp);
	vd.depth = 1;
	n = dv_compose_frame(&s, desk, &vd, &frame);
	{
		std::string	f((const char *)frame, n);
		size_t		a = f.find(set), b, c, i, y;

		b = a == std::string::npos ? a : f.find(set, a + 1);
		i = f.find("if");
		y = f.find(" y");
		if (a == std::string::npos || f.rfind("\033[0", a) == std::string::npos ||
		    a > i)
			fail("depth follows the SGR that would reset it, before the text");
		if (b == std::string::npos || b > f.find('x'))
			fail("the bold cell's SGR 0 reset it: it is set again");
		c = f.find(reset, f.find('x'));
		if (c == std::string::npos && f.rfind("\033[0", y) < f.find('x'))
			fail("the flat cells get the plane back");
		if (f.rfind(reset) == std::string::npos ||
		    f.rfind(reset) < f.rfind(set))
			fail("the frame ends on the plane");
	}
	free(frame);
	frame = nullptr;
	n = dv_compose_frame(&s, desk, &vp, &frame);
	if (memmem(frame, n, "777;depth", 9) != nullptr)
		fail("a client that did not list depth.1 is never sent it");
	free(frame);
	frame = nullptr;
	/* Only the depth changes: a client that shows depth is repainted. */
	{
		struct dv_depth_style	st = { DV_DEPTH_PX, 0, 0.5f, 0.0f };

		desk[5].depth = dv_depth_intern(&st);
		n = dv_compose_frame(&s, desk, &vd, &frame);
		if (n == 0 || memmem(frame, n, "op=set:p=0.5:y=0", 16) == nullptr)
			fail("a depth change alone repaints the cell");
		else {
			std::string	f((const char *)frame, n);

			if (f.find(reset, f.find("op=set:p=0.5")) == std::string::npos)
				fail("a frame ending on a deep cell returns to the plane");
		}
		free(frame);
		frame = nullptr;
	}
	/* A box over the window is on the plane. */
	{
		const char	*fill = "\033[H\033]777;depth;v=1:op=set:d=99\033\\";

		dv_vt_write(&w, fill, strlen(fill));
		for (unsigned i = 0; i < W * H - 1; i++)
			dv_vt_write(&w, "X", 1);
		s.menu_open = 1;
		dv_compose_cells(&s, desk);
		for (unsigned i = 0; i < W * H; i++)
			if (desk[i].ch != 'X' && desk[i].depth != 0) {
				fail("the box is on the plane");
				break;
			}
		s.menu_open = 0;
	}
	free(desk);
	dv_view_free(&vd);
	dv_view_free(&vp);
	dv_screen_free(&w.screen);
}

/*
 * --3d-focus: for a client that shows depth, the focused window's text and
 * images are nearer by its pixels, every other window is not, and moving
 * focus repaints the whole screen and places images again without their
 * pixels. A client without the option or without depth sees no change.
 */
static void
test_focus_depth(void)
{
	struct dv_session	 s;
	struct dv_window	 a, b;
	struct dv_cell		*desk;
	struct dv_view		 v, plain;
	unsigned char		*frame = nullptr;
	size_t			 n;
	unsigned		 W = 40, H = 10;
	const char		*near = "\033]777;depth;v=1:op=set:p=-4:y=0\033\\";
	const char		*img = "\033_Ga=T,f=24,s=2,v=2,c=2,r=1,q=2;AAAAAAAAAAAAAAAA\033\\";
	std::string		 f;

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	memset(&s, 0, sizeof s);
	dv_desktop_init(&s.desktop, size_of(W, H));
	for (struct dv_window *w : { &a, &b }) {
		memset(w, 0, sizeof *w);
		w->session = &s;
		w->pty_fd = -1;
		w->framed = 0;
		w->geom.cols = 20;
		w->geom.rows = H;
		dv_screen_init(&w->screen, 20, H);
		TAILQ_INSERT_TAIL(&s.desktop.windows, w, entry);
	}
	a.id = 1; a.z = 1;
	b.id = 2; b.z = 2; b.geom.x = 20;
	dv_vt_write(&a, "XYZ", 3);
	dv_vt_write(&a, img, strlen(img));
	dv_vt_write(&b, "\033]777;depth;v=1:op=set:p=1\033\\BBB", 31);
	s.desktop.focused = &a;
	desk = dv_allocn<dv_cell>((size_t)W * H);
	dv_compose_cells(&s, desk);
	memset(&v, 0, sizeof v);
	memset(&plain, 0, sizeof plain);
	v.depth = 1;
	v.gfx = 1;
	v.focus_px = 4.0f;
	plain.depth = 1;
	plain.gfx = 1;

	n = dv_compose_frame(&s, desk, &v, &frame);
	f.assign((const char *)frame, n);
	if (f.find(near) == std::string::npos || f.find(near) > f.find("XYZ"))
		fail("the focused window's text is nearer");
	if (f.rfind("op=set:p=1:y=0", f.find("BBB")) == std::string::npos ||
	    f.find("p=-3") != std::string::npos)
		fail("another window keeps its own depth");
	{
		size_t	pl = f.find("a=p,U=1");

		if (pl == std::string::npos || f.rfind(near, pl) == std::string::npos ||
		    f.rfind("777;depth", pl) != f.rfind(near, pl) + 2)
			fail("the focused window's image is placed nearer");
	}
	free(frame);
	frame = nullptr;
	n = dv_compose_frame(&s, desk, &plain, &frame);
	if (memmem(frame, n, "p=-4", 4) != nullptr)
		fail("a client without --3d-focus sees no focus depth");
	free(frame);
	frame = nullptr;
	/* Nothing changed: nothing is sent. */
	if (dv_compose_frame(&s, desk, &v, &frame) != 0)
		fail("a still screen is not repainted");
	free(frame);
	frame = nullptr;

	/* Focus moves to b: everything again, b nearer, the image re-placed
	 * flat without its pixels. */
	s.desktop.focused = &b;
	dv_compose_cells(&s, desk);
	n = dv_compose_frame(&s, desk, &v, &frame);
	f.assign((const char *)frame, n);
	if (f.find("\033[2J") == std::string::npos)
		fail("moving focus repaints the whole screen");
	if (f.rfind("op=set:p=-3:y=0", f.find("BBB")) == std::string::npos)
		fail("the newly focused window is nearer, its own depth kept");
	{
		size_t	xyz = f.find("XYZ");

		size_t	set = xyz == std::string::npos ? xyz :
			    f.rfind("777;depth;v=1:op=set", xyz);

		if (xyz == std::string::npos ||
		    (set != std::string::npos && set > f.rfind("\033[0", xyz)))
			fail("the window that lost focus goes back to the plane");
	}
	{
		size_t	pl = f.find("a=p,U=1");

		if (pl == std::string::npos ||
		    f.rfind("777;depth;v=1:op=reset", pl) == std::string::npos ||
		    f.rfind("777;depth", pl) != f.rfind("777;depth;v=1:op=reset", pl))
			fail("the image is placed again at its own depth");
		if (f.find("a=t") != std::string::npos)
			fail("placing again does not resend pixels");
	}
	free(frame);
	frame = nullptr;
	/* A client that does not draw depth is not repainted for focus. */
	{
		struct dv_view	nd;

		memset(&nd, 0, sizeof nd);
		nd.focus_px = 4.0f;
		s.desktop.focused = &a;
		dv_compose_cells(&s, desk);
		n = dv_compose_frame(&s, desk, &nd, &frame);
		free(frame);
		frame = nullptr;
		s.desktop.focused = &b;
		dv_compose_cells(&s, desk);
		n = dv_compose_frame(&s, desk, &nd, &frame);
		if (n != 0 && memmem(frame, n, "\033[2J", 4) != nullptr)
			fail("focus alone repaints only clients that draw it");
		free(frame);
		frame = nullptr;
		dv_view_free(&nd);
	}
	free(desk);
	dv_view_free(&v);
	dv_view_free(&plain);
	dv_kitty_free(&a);
	dv_kitty_free(&b);
	dv_screen_free(&a.screen);
	dv_screen_free(&b.screen);
}


/* Deep colours go to a terminal that listed them, in a sequence after the
 * eight-bit colour; another is sent only the eight-bit colour. And the
 * program status badge is drawn in the frame. */
static void
test_deep_paint(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_cell		*desk;
	struct dv_view		 vd, vp;
	unsigned char		*frame = nullptr;
	size_t			 n;
	unsigned		 W = 80, H = 24;
	const char		*seq = "\033[38:0:1:65535:32768:1;48:0:0:257:0:65535m";

	session_init(&s, &w, W, H);
	w.zoomed = 0;			/* framed, for the status badge */
	w.framed = 1;
	w.geom.x = 2;
	w.geom.y = 2;
	w.geom.cols = 60;
	w.geom.rows = 15;
	dv_vt_write(&w, "\033[38:0:1:65535:32768:1;48:0:0:257:0:65535mDEEP\033[0m", 50);
	desk = dv_allocn<dv_cell>(W * H);
	memset(&vd, 0, sizeof vd);
	memset(&vp, 0, sizeof vp);
	vd.deep = 1;
	vd.wg = vp.wg = 1;
	dv_compose_cells(&s, desk);
	n = dv_compose_frame(&s, desk, &vd, &frame);
	if (frame == nullptr || memmem(frame, n, seq, strlen(seq)) == NULL)
		fail("a deep terminal is sent 38:0 and 48:0");
	else if (memmem(frame, n, "38:2:1:255:128:0", 16) == NULL ||
	    (unsigned char *)memmem(frame, n, "38:2:1:255:128:0", 16) >
	    (unsigned char *)memmem(frame, n, seq, strlen(seq)))
		fail("the eight-bit colour comes first, in a sequence of its own");
	free(frame);
	frame = nullptr;
	n = dv_compose_frame(&s, desk, &vp, &frame);
	if (frame == nullptr || memmem(frame, n, ":0:1:65535", 10) != NULL ||
	    memmem(frame, n, "38:2:1:255:128:0", 16) == NULL)
		fail("another terminal gets only the eight-bit colour");
	free(frame);

	/* A status badge and its message on the frame. */
	dv_vt_write(&w, "\033]7501;state=done:app=make:msg=YnVpbHQ\033\\", 40);
	dv_compose_cells(&s, desk);
	if (!desk_has(desk, W, H, U"✓") || !desk_has(desk, W, H, "make: built"))
		fail("a done record shows a check mark and its message");
	free(desk);
	session_free(&w);
}

int
main(void)
{
	test_deep_paint();
	test_font_paint();
	test_tiny_window_resize();
	test_box_over_desktop();
	test_unicode();
	test_underline_emit();
	test_palette_remap();
	test_xfer_prompt();
	test_xcursor_compose();
	test_mono_frame();
	test_switch_list();
	test_many_windows();
	test_copy_mode_paint();
	test_osc66();
	test_layers();
	test_layers_scaled();
	test_window_controls();
	test_gamut_views();
	test_hdr_scaled();
	test_scene_drawn();
	test_cell_pictures();
	test_gfx_gate();
	test_image_cells();
	test_image_crop();
	test_image_updates();
	test_many_image_cells();
	test_small_desktop_gets_bar();
	test_diff_frames();
	test_depth_paint();
	test_focus_depth();
	if (failed)
		return (1);
	printf("test-menu: ok\n");
	return (0);
}
