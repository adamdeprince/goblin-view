/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Silkie multiplexer behaviour that does not need the server: coordinate
 * translation, contact capture, capability intersection.
 */

#include "dv.hpp"
#include "silkie.hpp"

#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <zlib.h>
#include <unordered_map>
#include <vector>

static int failed;

static char	hostbuf[1 << 16];
static size_t	hostn;
static unsigned	inj_n, inj_code;

void
dv_host_passthrough(struct dv_session *, const char *s, size_t n)
{
	if (hostn + n >= sizeof hostbuf)
		n = sizeof hostbuf - hostn - 1;
	if (n == 0)
		return;
	memcpy(hostbuf + hostn, s, n);
	hostn += n;
	hostbuf[hostn] = '\0';
}

void
dv_mouse_inject(struct dv_peer *, unsigned, unsigned, unsigned code, int)
{
	inj_n++;
	inj_code = code;
}

/* The vector service's end of the wire: what one terminal was asked. */
static std::string	primary, patched, told_one;
static std::vector<unsigned> patched_except;
static const struct dv_peer *told_peer;
static std::vector<unsigned> stale_ids;
static struct dv_peer	*test_peers;
static std::unordered_map<const struct dv_peer *, std::string> peer_frames;
static int		primary_fd = 7;

int
dv_kitty_send_primary(struct dv_session *, const char *b, size_t n)
{
	if (primary_fd >= 0)
		primary.append(b, n);
	return (primary_fd);
}

void
dv_kitty_send_patch(struct dv_session *, unsigned id, unsigned, const char *b,
    size_t n, const struct dv_peer *except)
{
	patched.append(b, n);
	if (except != nullptr)
		patched_except.push_back(id);
}

void
dv_kitty_send_patch_to(const struct dv_peer *p, unsigned, unsigned, const char *b,
    size_t n)
{
	told_one.append(b, n);
	told_peer = p;
}

void
dv_kitty_peer_stale(const struct dv_peer *, unsigned id)
{
	stale_ids.push_back(id);
}

struct dv_peer *
dv_host_first_peer(void)
{
	return (test_peers);
}

void
dv_host_send_peer(struct dv_peer *p, const char *b, size_t n)
{
	if (hostn + n < sizeof hostbuf) {
		memcpy(hostbuf + hostn, b, n);
		hostn += n;
		hostbuf[hostn] = '\0';
	}
	peer_frames[p].append(b, n);
}

static void
fail(const char *n)
{
	fprintf(stderr, "FAIL %s\n", n);
	failed = 1;
}

static int
has(const char *buf, const char *s)
{
	return (buf != nullptr && strstr(buf, s) != nullptr);
}

static void	pen(struct dv_peer *, int, int, int, int, long = 0, const char * = "",
		    int = 0);

static void
host_clear(void)
{
	hostn = 0;
	hostbuf[0] = '\0';
	inj_n = 0;
	inj_code = 0;
}

/* Read whatever the pane was sent, including a byte the pty would not take. */
static void
drain(struct dv_window *w, int rfd, char *buf, size_t cap, size_t *n)
{
	if (w->pty_out.len > 0 && *n + w->pty_out.len < cap) {
		memcpy(buf + *n, w->pty_out.data, w->pty_out.len);
		*n += w->pty_out.len;
		dv_buf_consume(&w->pty_out, w->pty_out.len);
	}
	for (;;) {
		ssize_t	r;

		if (*n + 1 >= cap)
			break;
		r = read(rfd, buf + *n, cap - *n - 1);
		if (r <= 0)
			break;
		*n += (size_t)r;
	}
	buf[*n] = '\0';
}

static void
peer_init(struct dv_peer *p, struct dv_session *s)
{
	memset(p, 0, sizeof *p);
	p->display = 1;
	p->session = s;
	p->tty.xpixel = 16;
	p->tty.ypixel = 16;
	p->fd = -1;
	p->entry.tqe_next = test_peers;
	test_peers = p;
}

static void
win_init(struct dv_window *w, struct dv_session *s, unsigned id, int x, int y,
    int z)
{
	memset(w, 0, sizeof *w);
	w->id = id;
	w->session = s;
	w->pty_fd = -1;
	w->pipe_fd = -1;
	w->framed = 1;
	w->geom.x = x;
	w->geom.y = y;
	w->geom.cols = 10;
	w->geom.rows = 10;
	w->z = z;
	dv_screen_init(&w->screen, 10, 10);
}

static int
pipe_to(struct dv_window *w, int *rfd)
{
	int	fd[2];

	if (pipe(fd) != 0)
		return (0);
	fcntl(fd[0], F_SETFL, O_NONBLOCK);
	fcntl(fd[1], F_SETFL, O_NONBLOCK);
	w->pty_fd = fd[1];
	*rfd = fd[0];
	return (1);
}

static void
win_free(struct dv_window *w, int rfd)
{
	if (rfd >= 0)
		close(rfd);
	if (w->pty_fd >= 0)
		close(w->pty_fd);
	w->pty_fd = -1;
	dv_buf_free(&w->pty_out);
	dv_buf_free(&w->cap);
	dv_screen_free(&w->screen);
}

static void
session_init(struct dv_session *s)
{
	memset(s, 0, sizeof *s);
	test_peers = nullptr;
	TAILQ_INIT(&s->desktop.windows);
	s->desktop.cols = 80;
	s->desktop.rows = 40;
	s->desktop.xpixel = 16;
	s->desktop.ypixel = 16;
}

/* Framed window at (2,1), cells 16px. Desktop cell (4,3) is one cell
 * into the content, which is 4096/256 of a pixel in both axes. */
static void
test_translate(void)
{
	struct dv_session	s;
	struct dv_window	w;
	struct dv_peer		p;
	int			rd = -1;
	char			buf[2048];
	size_t			n = 0;
	const char	       *ev =
	    "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=3:contact=1:x=16384:y=12288";

	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	w.screen.silk_tablet = 1;
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.focused = &w;
	peer_init(&p, &s);
	if (!pipe_to(&w, &rd)) {
		fail("pipe");
		win_free(&w, rd);
		return;
	}
	dv_silkie_from_host(&p, ev, strlen(ev));
	drain(&w, rd, buf, sizeof buf, &n);
	if (!has(buf, "x=4096") || !has(buf, "y=4096"))
		fail("tablet event at desktop cell (4,3) translates to 4096,4096");
	win_free(&w, rd);
}

static void
test_capture(void)
{
	struct dv_session	s;
	struct dv_window	a, b;
	struct dv_peer		p;
	int			ra = -1, rb = -1;
	char			ba[2048], bb[2048];
	size_t			na = 0, nb = 0;
	const char	       *down =
	    "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=3:contact=1:x=16384:y=12288";
	const char	       *over =
	    "777;tablet-pointer;v=1:op=event:kind=1:phase=2:device=3:contact=1:x=90112:y=12288";
	const char	       *again =
	    "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=3:contact=1:x=90112:y=12288";

	session_init(&s);
	win_init(&a, &s, 1, 2, 1, 1);
	win_init(&b, &s, 2, 20, 1, 2);
	a.screen.silk_tablet = b.screen.silk_tablet = 1;
	TAILQ_INSERT_TAIL(&s.desktop.windows, &a, entry);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &b, entry);
	peer_init(&p, &s);
	if (!pipe_to(&a, &ra) || !pipe_to(&b, &rb)) {
		fail("pipe");
		win_free(&a, ra);
		win_free(&b, rb);
		return;
	}
	dv_silkie_from_host(&p, down, strlen(down));
	drain(&a, ra, ba, sizeof ba, &na);
	drain(&b, rb, bb, sizeof bb, &nb);
	if (!has(ba, "x=4096") || !has(ba, "y=4096") || has(bb, "op=event"))
		fail("a stroke begins on the window under the pointer");
	na = nb = 0;
	ba[0] = bb[0] = '\0';
	dv_silkie_from_host(&p, over, strlen(over));
	drain(&a, ra, ba, sizeof ba, &na);
	drain(&b, rb, bb, sizeof bb, &nb);
	if (!has(ba, "x=77824") || !has(ba, "y=4096") || has(bb, "op=event"))
		fail("a stroke stays with the window where it began");
	dv_silkie_cancel_window(&a);
	na = nb = 0;
	ba[0] = bb[0] = '\0';
	dv_silkie_from_host(&p, again, strlen(again));
	drain(&a, ra, ba, sizeof ba, &na);
	drain(&b, rb, bb, sizeof bb, &nb);
	if (has(ba, "phase=1") || !has(bb, "x=4096") || !has(bb, "y=4096"))
		fail("after cancel a new contact goes to the window under it");
	win_free(&a, ra);
	win_free(&b, rb);
}

static void
test_accidental(void)
{
	struct dv_session	s;
	struct dv_window	w;
	struct dv_peer		p;
	int			rd = -1;
	char			buf[2048];
	size_t			n = 0;
	const char	       *acc =
	    "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=4:contact=2:x=16384:y=12288:conf=0";
	const char	       *real =
	    "777;tablet-pointer;v=1:op=event:kind=1:phase=2:device=4:contact=2:x=16384:y=12288:conf=1";

	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	w.screen.silk_tablet = 1;		/* tool, not hover */
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	peer_init(&p, &s);
	if (!pipe_to(&w, &rd)) {
		fail("pipe");
		win_free(&w, rd);
		return;
	}
	dv_silkie_from_host(&p, acc, strlen(acc));
	drain(&w, rd, buf, sizeof buf, &n);
	if (has(buf, "op=event"))
		fail("an accidental contact is not reported without hover");
	n = 0;
	buf[0] = '\0';
	dv_silkie_from_host(&p, real, strlen(real));
	drain(&w, rd, buf, sizeof buf, &n);
	if (!has(buf, "phase=1") || has(buf, "phase=2") || !has(buf, "x=4096"))
		fail("a contact that stops being accidental begins as a press");
	win_free(&w, rd);
}

static void
test_touchpad(void)
{
	struct dv_session	s;
	struct dv_window	w;
	struct dv_peer		p;
	int			rd = -1;
	char			buf[2048];
	size_t			n = 0;
	const char	       *ev =
	    "777;tablet-pointer;v=1:op=event:kind=1:phase=1:device=5:contact=1:src=1:x=1000:y=2000";

	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	w.screen.silk_tablet = 1;
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.focused = &w;
	peer_init(&p, &s);
	if (!pipe_to(&w, &rd)) {
		fail("pipe");
		win_free(&w, rd);
		return;
	}
	dv_silkie_from_host(&p, ev, strlen(ev));
	drain(&w, rd, buf, sizeof buf, &n);
	if (!has(buf, "x=1000") || !has(buf, "y=2000") || has(buf, "x=-"))
		fail("a touchpad position is not translated");
	win_free(&w, rd);
}

static void
test_scroll(void)
{
	struct dv_session	s;
	struct dv_window	w;
	struct dv_peer		p;
	int			rd = -1;
	char			buf[2048];
	size_t			n = 0;
	const char	       *ev =
	    "777;scroll;v=1:op=event:x=16384:y=12288:dy=1600";

	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	peer_init(&p, &s);
	if (!pipe_to(&w, &rd)) {
		fail("pipe");
		win_free(&w, rd);
		return;
	}
	host_clear();
	dv_silkie_from_host(&p, ev, strlen(ev));
	drain(&w, rd, buf, sizeof buf, &n);
	if (inj_n != 1 || inj_code != 65 || has(buf, "op=event"))
		fail("scroll with no pane mode is a wheel");
	w.screen.silk_scroll = 1;
	host_clear();
	n = 0;
	buf[0] = '\0';
	dv_silkie_from_host(&p, ev, strlen(ev));
	drain(&w, rd, buf, sizeof buf, &n);
	if (inj_n != 0 || !has(buf, "x=4096") || !has(buf, "y=4096"))
		fail("scroll with a pane mode is forwarded, translated");
	win_free(&w, rd);
}

static void
test_reset(void)
{
	struct dv_session	s;
	struct dv_window	a, b;
	const char	       *rst = "777;caps;v=1:op=reset";

	session_init(&s);
	win_init(&a, &s, 1, 2, 1, 1);
	win_init(&b, &s, 2, 20, 1, 2);
	a.screen.silk_tablet = 1;
	b.screen.silk_tablet = 2;
	TAILQ_INSERT_TAIL(&s.desktop.windows, &a, entry);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &b, entry);
	dv_strset(&s.silk_ext, "tablet-pointer.1,caps.1");
	s.silk_any = 1;
	host_clear();
	dv_vt_write(&a, "\033]133;A\033\\", strlen("\033]133;A\033\\"));
	if (a.screen.silk_tablet != 0 || b.screen.silk_tablet != 2 ||
	    !has(hostbuf, "mode=2"))
		fail("OSC 133 A resets one pane; the other pane's mode stays");
	host_clear();
	dv_silkie_program(&b, rst, strlen(rst));
	if (b.screen.silk_tablet != 0 || !has(hostbuf, "mode=0"))
		fail("a caps reset clears that pane and pushes the union");
	win_free(&a, -1);
	win_free(&b, -1);
}

static void
test_query_silent(void)
{
	struct dv_session	s;
	struct dv_window	w;
	int			rd = -1;
	char			buf[2048];
	size_t			n = 0;
	const char	       *q = "777;caps;v=1:op=query:id=7";

	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	if (!pipe_to(&w, &rd)) {
		fail("pipe");
		win_free(&w, rd);
		return;
	}
	dv_silkie_program(&w, q, strlen(q));
	drain(&w, rd, buf, sizeof buf, &n);
	if (has(buf, "op=cap"))
		fail("a query is ignored when no terminal listed extensions");
	win_free(&w, rd);
}

static std::string	said(struct dv_window *, int);

static void
test_recompute(void)
{
	struct dv_session	s;
	struct dv_window	w;
	struct dv_peer		silk, ghost;
	TAILQ_HEAD(silk_peer_head, dv_peer) peers;
	int			rd = -1;
	char			buf[2048];
	size_t			n = 0;
	const char	       *q = "777;caps;v=1:op=query:id=7";
	const char	       *blob =
	    "ext=tablet-pointer.1,display.1\n"
	    "format=24,32,64,100,200\n"
	    "media=d,f,t,s,g\n"
	    "tablet=1\n"
	    "display=kind=lcd:color=srgb:bits=8:gamut=srgb:headroom=0:scale=1:period=0:ppi=110\n";

	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	peer_init(&silk, &s);
	peer_init(&ghost, &s);
	dv_strset(&silk.silkie_blob, blob);
	ghost.features = DV_IDENT_KITTY_GFX;
	TAILQ_INIT(&peers);
	TAILQ_INSERT_TAIL(&peers, &silk, entry);
	TAILQ_INSERT_TAIL(&peers, &ghost, entry);
	dv_silkie_recompute(&s, TAILQ_FIRST(&peers));
	if (strcmp(s.silk_ext, "tablet-pointer.1,display.1,caps.1") != 0)
		fail("ext is the selectors any peer listed, plus caps");
	if (strcmp(s.silk_format, "24,32,100,64,200") != 0 ||
	    strcmp(s.silk_media, "d,f,t,s") != 0 || s.silk_tablet_caps != 1)
		fail("media are the intersection; vector and HDR are offered whatever is attached");
	if (!silk.view.vector || !silk.view.hdr || ghost.view.vector || ghost.view.hdr)
		fail("each terminal is known for what it takes itself");
	{
		/* The caps reply lists the SGR extensions odin-seat keeps. */
		int		rd2 = -1;
		struct dv_window	w2;

		win_init(&w2, &s, 9, 2, 1, 1);
		if (pipe_to(&w2, &rd2)) {
			dv_silkie_program(&w2, q, strlen(q));
			if (said(&w2, rd2).find(":sgr=alpha,deep,gamut,hdr") == std::string::npos)
				fail("the caps reply lists sgr=alpha,deep,gamut,hdr");
		}
		win_free(&w2, rd2);
	}
	if (!pipe_to(&w, &rd)) {
		fail("pipe");
		win_free(&w, rd);
		return;
	}
	dv_silkie_program(&w, q, strlen(q));
	drain(&w, rd, buf, sizeof buf, &n);
	if (!has(buf, "op=cap") || !has(buf, "ext=tablet-pointer.1,display.1,caps.1") ||
	    !has(buf, "format=24,32,100,64,200") || !has(buf, "media=d,f,t,s") ||
	    has(buf, "media=d,f,t,s,g"))
		fail("a caps query answers with what can be shown on every terminal");
	if (dv_silkie_gfx_ok(&s, 32, 0, 'g') || dv_silkie_gfx_ok(&s, 200, 0, 'g'))
		fail("a medium the intersection dropped is refused");
	/* A scene is drawn, and HDR brought down, for the terminal that takes
	 * neither: programs may send both. */
	if (!dv_silkie_gfx_ok(&s, 64, 0, 'd') || !dv_silkie_gfx_ok(&s, 200, 0, 'd'))
		fail("vector scenes and HDR pixels are taken whatever is attached");
	/* A gamut is not dropped: one terminal that takes it is enough, and
	 * the other is sent those pixels as sRGB. */
	if (!dv_silkie_gfx_ok(&s, 32, 1, 'd') || !silk.view.wg || ghost.view.wg)
		fail("pixels in a wider gamut are taken when any terminal shows them");
	if (!dv_silkie_gfx_ok(&s, 24, 0, 'd') || !dv_silkie_gfx_ok(&s, 32, 0, 'f') ||
	    !dv_silkie_gfx_ok(&s, 100, 0, 't') || !dv_silkie_gfx_ok(&s, 32, 0, 's'))
		fail("shared kitty formats and media are accepted");
	ghost.display = 0;
	dv_silkie_recompute(&s, TAILQ_FIRST(&peers));
	if (!dv_silkie_gfx_ok(&s, 32, 1, 'd'))
		fail("wide gamut is accepted when every graphics peer listed formats");
	win_free(&w, rd);
}

/*
 * Several terminals, one display reply: what the best of them shows, one
 * attribute at a time. Each is sent what its own display takes, so programs
 * are told the most there is.
 */
static void
test_display_best(void)
{
	struct dv_session	s;
	struct dv_peer		p3, deep, paper;
	TAILQ_HEAD(disp_peer_head, dv_peer) peers;
	auto has_field = [&s](const char *f) {
		std::string	line = std::string(":") + (s.silk_display ? s.silk_display : "") + ":";

		return (line.find(std::string(":") + f + ":") != std::string::npos);
	};

	session_init(&s);
	peer_init(&p3, &s);
	peer_init(&deep, &s);
	peer_init(&paper, &s);
	dv_strset(&p3.silkie_blob, "ext=display.1\n"
	    "display=kind=lcd:color=rgb:bits=8:gamut=p3:headroom=1:scale=2:period=8333:ppi=254:"
	    "red_bits=8:green_bits=8:blue_bits=8:gray_bits=0\n");
	dv_strset(&deep.silkie_blob, "ext=display.1\n"
	    "sgr=alpha,deep,gamut,hdr\n"
	    "display=kind=oled:color=rgb:bits=10:gamut=srgb:headroom=2.5:scale=1:period=16666:ppi=110:"
	    "red_bits=10:green_bits=10:blue_bits=10:gray_bits=0:output_bits=16:output_type=float:"
	    "output_dither=0\n");
	dv_strset(&paper.silkie_blob, "ext=display.1\n"
	    "display=kind=eink:color=gray:bits=4:gamut=unknown:headroom=1:scale=1:period=100000:ppi=300:"
	    "red_bits=0:green_bits=0:blue_bits=0:gray_bits=4\n");
	TAILQ_INIT(&peers);
	for (auto *q : { &paper, &deep, &p3 })		/* the best one last */
		TAILQ_INSERT_TAIL(&peers, q, entry);
	dv_silkie_recompute(&s, TAILQ_FIRST(&peers));
	if (!has_field("color=rgb") || !has_field("gamut=p3") || !has_field("headroom=2.5"))
		fail("the display is the best colour model, gamut and headroom of them all");
	if (!has_field("red_bits=10") || !has_field("bits=10") || !has_field("gray_bits=0"))
		fail("channel widths are the widest among displays of that colour model");
	if (!has_field("kind=lcd") || !has_field("scale=2") || !has_field("ppi=254"))
		fail("what describes one display is that of the one leading in gamut");
	if (!has_field("output_bits=16"))
		fail("the widest surface a terminal keeps colours on is relayed");
	if (!deep.view.deep || p3.view.deep || paper.view.deep)
		fail("only the terminal that listed deep is sent deep colours");
	if (paper.view.disp_color != 1 || paper.view.disp_bits[3] != 4 ||
	    deep.view.disp_bits[0] != 10 || p3.view.disp_bits[0] != 8)
		fail("each terminal keeps its own display for what it is sent");

	/* In the other order the answer is the same. */
	std::string	first = s.silk_display;

	TAILQ_INIT(&peers);
	for (auto *q : { &p3, &deep, &paper })
		TAILQ_INSERT_TAIL(&peers, q, entry);
	dv_silkie_recompute(&s, TAILQ_FIRST(&peers));
	if (!has_field("gamut=p3") || !has_field("headroom=2.5") || !has_field("red_bits=10") ||
	    !has_field("kind=lcd") || !has_field("color=rgb"))
		fail("the order terminals attached in does not change it");

	/* With the best one gone, what is left leads. */
	p3.display = 0;
	dv_silkie_recompute(&s, TAILQ_FIRST(&peers));
	if (!has_field("gamut=srgb") || !has_field("kind=oled") || !has_field("headroom=2.5"))
		fail("a terminal that leaves takes its display with it");
	for (auto *q : { &p3, &deep, &paper })
		free(q->silkie_blob);
	free(s.silk_display);
	free(s.silk_ext);
	free(s.silk_format);
	free(s.silk_media);
}

/* ---- what silkie added after the first cut of this file ---- */

static std::string
b64(const std::string &in)
{
	static const char	t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	    "abcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string		o;

	for (size_t i = 0; i < in.size(); i += 3) {
		unsigned	v = (unsigned)(unsigned char)in[i] << 16;

		if (i + 1 < in.size())
			v |= (unsigned)(unsigned char)in[i + 1] << 8;
		if (i + 2 < in.size())
			v |= (unsigned char)in[i + 2];
		o += t[(v >> 18) & 63];
		o += t[(v >> 12) & 63];
		o += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
		o += i + 2 < in.size() ? t[v & 63] : '=';
	}
	return (o);
}

static std::string
unb64(const std::string &in)
{
	std::string	o;
	int		acc = 0, bits = 0;

	for (char c : in) {
		int	v;

		if (c >= 'A' && c <= 'Z')
			v = c - 'A';
		else if (c >= 'a' && c <= 'z')
			v = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			v = c - '0' + 52;
		else if (c == '+')
			v = 62;
		else if (c == '/')
			v = 63;
		else
			continue;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			o += (char)((acc >> bits) & 0xff);
		}
	}
	return (o);
}

/* Every payload sent, decoded and joined. */
static std::string
payloads(const std::string &wire)
{
	std::string	o;
	size_t		i = 0;

	while ((i = wire.find("\033_G", i)) != std::string::npos) {
		size_t	semi = wire.find(';', i), end = wire.find("\033\\", i);

		if (semi == std::string::npos || end == std::string::npos)
			break;
		if (semi < end)
			o += unb64(wire.substr(semi + 1, end - semi - 1));
		i = end + 2;
	}
	return (o);
}

static std::string
said(struct dv_window *w, int rd)
{
	static char	buf[1 << 20];
	size_t		n = 0;

	drain(w, rd, buf, sizeof buf, &n);
	return (std::string(buf, n));
}

static void
put(struct dv_window *w, const std::string &bytes)
{
	dv_vt_write(w, bytes.data(), bytes.size());
}

static void
osc(struct dv_window *w, const std::string &body)
{
	put(w, "\033]777;" + body + "\033\\");
}

static void
silk_ready(struct dv_session *s, const char *ext)
{
	dv_strset(&s->silk_ext, ext);
	dv_strset(&s->silk_format, "24,32,64,100,200");
	dv_strset(&s->silk_media, "d,f,t,s");
	s->silk_any = 1;
	s->silk_wg = 1;
	s->silk_vector = 1;		/* it draws scenes, and answers their service */
}

static void
test_frame_length(void)
{
	struct dv_session	s;
	struct dv_window	w;
	std::string		fill(4000, 'a');

	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.focused = &w;
	silk_ready(&s, "haptic.1,caps.1");
	host_clear();
	/* Far past the 127 bytes frames used to be held to, and past every
	 * size this file once kept a frame in. */
	osc(&w, "haptic;v=1:op=play:pattern=click:pad=" + fill);
	if (!has(hostbuf, "op=play") || !has(hostbuf, fill.c_str()))
		fail("a long frame from a program goes through whole");
	{
		std::string	many = "haptic;v=1:op=play:pattern=click";

		for (int i = 0; i < 200; i++)
			many += ":k" + std::to_string(i) + "=1";
		host_clear();
		osc(&w, many);
		if (!has(hostbuf, "k199=1"))
			fail("a frame may have any number of fields");
		host_clear();
		osc(&w, many + ":k7=2");
		if (has(hostbuf, "op=play"))
			fail("a repeated key still rejects the frame");
	}
	win_free(&w, -1);
}

static void
test_contacts_grow(void)
{
	struct dv_session	s;
	struct dv_window	w;
	struct dv_peer		p;
	int			rd = -1;

	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	w.geom.cols = 100;
	w.geom.rows = 100;
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.focused = &w;
	peer_init(&p, &s);
	silk_ready(&s, "tablet-pointer.1,caps.1");
	s.silk_tablet_caps = 7;
	w.screen.silk_tablet = 2;
	if (!pipe_to(&w, &rd))
		return;
	for (int i = 0; i < 80; i++) {
		std::string	f = "777;tablet-pointer;v=1:op=event:kind=2:phase=1:device=3:contact=" +
		    std::to_string(i) + ":x=16384:y=12288";

		dv_silkie_from_host(&p, f.data(), f.size());
	}
	said(&w, rd);
	dv_silkie_cancel_window(&w);
	{
		std::string	out = said(&w, rd);
		size_t		n = 0;

		for (size_t i = 0; (i = out.find("phase=4", i)) != std::string::npos; i += 7)
			n++;
		if (n != 80)
			fail("every contact is held and cancelled, however many");
	}
	win_free(&w, rd);
	dv_silkie_free_window(&w);
	dv_silkie_free_peer(&p);
}

static void
test_font(void)
{
	struct dv_session	s;
	struct dv_window	w;
	struct dv_peer		a, b;
	TAILQ_HEAD(font_peer_head, dv_peer) peers;
	int			rd = -1;
	std::string		out;
	const struct dv_font_style *st;

	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.focused = &w;
	if (!pipe_to(&w, &rd))
		return;

	/* A text attribute: kept with nothing attached, for whoever attaches. */
	osc(&w, "font;v=1:op=select:role=serif:weight=300:slant=-500:italic=1");
	st = dv_font_get(w.screen.font);
	if (st->role != "serif" || st->weight != 300 || st->slant != -500 ||
	    !(w.screen.attr & DV_ATTR_ITALIC))
		fail("select sets role, weight, slant and SGR italic");
	put(&w, "X");
	if (dv_screen_at(&w.screen, 0, 0)->font != w.screen.font)
		fail("a written cell carries the style");
	osc(&w, "font;v=1:op=select:weight=500");
	st = dv_font_get(w.screen.font);
	if (st->role != "serif" || st->weight != 500 || st->slant != -500)
		fail("an omitted property keeps its value");
	said(&w, rd);
	osc(&w, "font;v=1:op=state:id=42");
	out = said(&w, rd);
	if (out.find("op=state:id=42:role=serif:weight=500:slant=-500:italic=1") ==
	    std::string::npos)
		fail("state reads back what was requested");

	/* Validated atomically. */
	{
		uint32_t	before = w.screen.font;

		for (const char *bad : {
		    "font;v=1:op=select:role=serif:weight=950",
		    "font;v=1:op=select:role=sans:weight=50",
		    "font;v=1:op=select:role=sans:slant=1501",
		    "font;v=1:op=select:role=sans:italic=2",
		    "font;v=1:op=select:role=9sans",
		    "font;v=1:op=select:role=sans:role=serif",
		    "font;v=2:op=select:role=sans" }) {
			osc(&w, bad);
			if (w.screen.font != before)
				fail(bad);
		}
		osc(&w, "font;v=1:op=select:role=sans:later=1");
		if (w.screen.font == before)
			fail("an unknown field is ignored, not a rejection");
	}
	/* Saved with the cursor, reset by SGR 0 and op=reset, kept by a caps
	 * reset, which is for modes. */
	osc(&w, "font;v=1:op=select:role=serif:weight=700:slant=auto");
	{
		uint32_t	f = w.screen.font;

		put(&w, "\0337");
		put(&w, "\033[0m");
		if (w.screen.font != 0)
			fail("SGR 0 resets the selection");
		put(&w, "\0338");
		if (w.screen.font != f)
			fail("cursor restore brings the style back");
		silk_ready(&s, "font.1,caps.1");
		osc(&w, "caps;v=1:op=reset");
		if (w.screen.font != f)
			fail("a caps reset leaves the font alone");
		w.screen.attr |= DV_ATTR_ITALIC;
		osc(&w, "font;v=1:op=reset");
		if (w.screen.font != 0 || !(w.screen.attr & DV_ATTR_ITALIC))
			fail("op=reset restores the default and leaves SGR");
		osc(&w, "font;v=1:op=select:role=serif");
		put(&w, "\033c");
		if (w.screen.font != 0)
			fail("RIS resets the selection");
	}
	/* The cells survive being handed to another server. */
	{
		struct dv_cell	in[2], back[2];
		struct dv_buf	bb;

		memset(in, 0, sizeof in);
		memset(back, 0, sizeof back);
		in[0].ch = 'a';
		in[1].ch = 'a';
		in[1].font = dv_font_intern("serif", 300, 0);
		memset(&bb, 0, sizeof bb);
		dv_cells_encode(&bb, in, 2);
		dv_buf_append(&bb, "", 1);
		if (dv_cells_decode((char *)bb.data, back, 2) != 2 ||
		    back[0].font != 0 || back[1].font != in[1].font)
			fail("cells encode their style");
		dv_buf_free(&bb);
		memset(&bb, 0, sizeof bb);
		dv_font_save(&bb);
		dv_buf_append(&bb, "", 1);
		if (strstr((char *)bb.data, " 300 0 serif") == nullptr)
			fail("the style table is saved");
		dv_buf_free(&bb);
		dv_font_restore(4000, "kept", 200, 100);
		if (dv_font_get(4000)->role != "kept" ||
		    dv_font_intern("kept", 200, 100) != 4000)
			fail("a restored style keeps its id");
	}

	/* What is offered: a selector any terminal listed, roles merged, ranges
	 * that every one of them accepts. */
	peer_init(&a, &s);
	peer_init(&b, &s);
	dv_strset(&a.silkie_blob, "ext=font.1,caps.1\n"
	    "font=caps=1:roles=default,sans,serif:weight_min=100:weight_max=900:slant_min=-1500:slant_max=1500:synthetic_slant=1\n");
	dv_strset(&b.silkie_blob, "ext=font.1,caps.1\n"
	    "font=caps=1:roles=default,serif,mono:weight_min=200:weight_max=800:slant_min=-1000:slant_max=1000:synthetic_slant=1\n");
	TAILQ_INIT(&peers);
	TAILQ_INSERT_TAIL(&peers, &a, entry);
	TAILQ_INSERT_TAIL(&peers, &b, entry);
	dv_silkie_recompute(&s, TAILQ_FIRST(&peers));
	if (!a.view.font || !b.view.font)
		fail("a peer that listed font.1 is painted styles");
	said(&w, rd);
	host_clear();
	osc(&w, "font;v=1:op=query:id=9");
	out = said(&w, rd);
	if (out.find("op=cap:id=9:caps=1:roles=default,sans,serif,mono:weight_min=200:weight_max=800:slant_min=-1000:slant_max=1000:synthetic_slant=1") ==
	    std::string::npos)
		fail("the query answers for every attached terminal");
	if (!has(hostbuf, "777;font;v=1:op=query:id=17349532"))
		fail("a query refreshes roles, which change with no notice");
	osc(&w, "font;v=1:op=select:weight=900");
	if (w.screen.font != 0)
		fail("a weight some terminal refuses is out of range here");

	/* SGR 10-19: configuration order, weight and slant kept. */
	osc(&w, "font;v=1:op=select:weight=300:slant=100");
	put(&w, "\033[11m");
	st = dv_font_get(w.screen.font);
	if (st->role != "sans" || st->weight != 300 || st->slant != 100)
		fail("SGR 11 is the first configured role");
	put(&w, "\033[13m");
	if (dv_font_get(w.screen.font)->role != "mono")
		fail("SGR 13 is the third");
	put(&w, "\033[19m");
	if (dv_font_get(w.screen.font)->role != "default")
		fail("an unassigned slot is the default");
	put(&w, "\033[12m\033[10m");
	st = dv_font_get(w.screen.font);
	if (st->role != "default" || st->weight != 300)
		fail("SGR 10 is the default role, weight kept");

	/* A peer without it is not painted it, and nothing is offered if none. */
	dv_strset(&a.silkie_blob, "ext=display.1,caps.1\n");
	dv_strset(&b.silkie_blob, "ext=display.1,caps.1\n");
	dv_silkie_recompute(&s, TAILQ_FIRST(&peers));
	if (a.view.font || b.view.font)
		fail("a peer without font.1 is not painted styles");
	said(&w, rd);
	osc(&w, "font;v=1:op=query:id=10");
	if (said(&w, rd).find("id=10") != std::string::npos)
		fail("the query is ignored when nothing listed font.1");
	put(&w, "\033[11m");
	if (dv_font_get(w.screen.font)->role != "sans")
		fail("the slots survive the terminals going away");
	dv_silkie_free_peer(&a);
	dv_silkie_free_peer(&b);
	dv_silkie_free_session(&s);
	win_free(&w, rd);
}

static void
test_ink(void)
{
	struct dv_session	s;
	struct dv_window	w;
	struct dv_peer		p;
	int			rd = -1;
	std::string		out;
	const char		*good = "ink;v=1:op=brush:i=7:k=1000:n=50:fill=#1a1a80:w0=1:w1=5";

	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	w.geom.cols = 100;
	w.geom.rows = 100;
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.focused = &w;
	peer_init(&p, &s);
	silk_ready(&s, "ink.1,tablet-pointer.1,caps.1");
	s.silk_ink_caps = 1;
	s.silk_tablet_caps = 1;
	if (!pipe_to(&w, &rd))
		return;
	osc(&w, "tablet-pointer;v=1:op=mode:mode=1");
	osc(&w, "ink;v=1:op=mode:mode=1");
	if (w.screen.silk_ink != 0)
		fail("mode=1 does nothing without a brush");
	for (const char *bad : {
	    "ink;v=1:op=brush:k=1:w1=1",
	    "ink;v=1:op=brush:i=7:w1=1",
	    "ink;v=1:op=brush:i=7:k=1",
	    "ink;v=1:op=brush:i=7:k=0:w1=1",
	    "ink;v=1:op=brush:i=7:k=4294967295:n=2:w1=1",
	    "ink;v=1:op=brush:i=7:k=1:w1=-1",
	    "ink;v=1:op=brush:i=7:k=1:w1=inf",
	    "ink;v=1:op=brush:i=7:k=1:w1=nan",
	    "ink;v=1:op=brush:i=7:k=1:w1=1:e=0",
	    "ink;v=1:op=brush:i=7:k=1:w1=1:fill=none",
	    "ink;v=1:op=brush:i=7:k=1:w1=1:tools=0",
	    "ink;v=1:op=brush:i=7:k=1:w1=1:tools=512" }) {
		osc(&w, bad);
		if (w.screen.silk_brush != nullptr)
			fail(bad);
	}
	osc(&w, good);
	if (w.screen.silk_ink != 1)
		fail("a brush turns ink on");
	said(&w, rd);
	osc(&w, "ink;v=1:op=query:id=42");
	out = said(&w, rd);
	if (out.find("op=cap:id=42:caps=1:mode=1:i=7:g=0:k=1000:n=50:fill=#1a1a80:w0=1:w1=5:e=1:tools=58") ==
	    std::string::npos)
		fail("a query carries the brush with its defaults");
	{
		/* Fields of any length, which is what the widths and a gradient
		 * may now be. */
		std::string	wide(300, '1');

		osc(&w, "ink;v=1:op=brush:i=7:k=1:w1=" + wide);
		said(&w, rd);
		osc(&w, "ink;v=1:op=query:id=43");
		if (said(&w, rd).find("w1=" + wide) == std::string::npos)
			fail("a brush is kept whatever its size");
		osc(&w, good);
	}
	/* A stroke the terminal drew uses up the next of the program's ids. */
	{
		put(&w, "\033_Ga=T,U=1,f=200,i=7,s=100,v=100,c=2,r=2,q=2;" +
		    b64("p id=1 d=M0,0L1,1\n") + "\033\\");
		pen(&p, 1, 16384, 12288, 65535, 1);
		out = said(&w, rd);
		if (out.find("ink=1000") == std::string::npos)
			fail("the report has the program's id");
		osc(&w, "ink;v=1:op=query:id=44");
		out = said(&w, rd);
		if (out.find("k=1001:n=49") == std::string::npos)
			fail("k and n are what the program has left");
		pen(&p, 3, 16384, 12288, 0);
		said(&w, rd);
	}
	osc(&w, "ink;v=1:op=mode:mode=0");
	if (w.screen.silk_ink != 0 || w.screen.silk_brush == nullptr)
		fail("mode=0 keeps the brush");
	osc(&w, "ink;v=1:op=mode:mode=1");
	if (w.screen.silk_ink != 1)
		fail("mode=1 turns ink on again with it");
	put(&w, "\033c");
	if (w.screen.silk_ink != 0 || w.screen.silk_brush != nullptr)
		fail("RIS forgets the brush");
	dv_silkie_free_peer(&p);
	win_free(&w, rd);
}

static void
test_label(void)
{
	struct dv_session	s;
	struct dv_window	a, b;
	std::string		text = b64(std::string(5000, 'L'));

	session_init(&s);
	win_init(&a, &s, 1, 2, 1, 1);
	win_init(&b, &s, 2, 20, 1, 2);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &a, entry);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &b, entry);
	s.desktop.focused = &a;
	silk_ready(&s, "tablet-pointer.1,caps.1");
	a.screen.silk_tablet = 4;
	host_clear();
	osc(&b, "tablet-pointer;v=1:op=label:device=5:button=2:text=VW5kbw==");
	if (has(hostbuf, "op=label"))
		fail("a label comes only from the program the pad reports to");
	osc(&a, "tablet-pointer;v=1:op=label:device=5:button=2:text=VW5kbw==");
	if (!has(hostbuf, "op=label:device=5:button=2:text=VW5kbw=="))
		fail("a label goes to the terminal");
	host_clear();
	osc(&a, "tablet-pointer;v=1:op=label:device=5:button=2:text=" + text);
	if (!has(hostbuf, "op=label") || !has(hostbuf, text.substr(0, 4000).c_str()))
		fail("a label has no length of its own");
	host_clear();
	for (const char *bad : {
	    "tablet-pointer;v=1:op=label:button=2",
	    "tablet-pointer;v=1:op=label:device=5",
	    "tablet-pointer;v=1:op=label:device=5:button=2:ring=1",
	    "tablet-pointer;v=1:op=label:device=5:button=256",
	    "tablet-pointer;v=1:op=label:device=5:button=2:text=abc" })
		osc(&a, bad);
	if (has(hostbuf, "op=label"))
		fail("a malformed label is rejected");
	win_free(&a, -1);
	win_free(&b, -1);
}

static void
test_gamut_ignored(void)
{
	struct dv_session	s;

	session_init(&s);
	silk_ready(&s, "caps.1");
	s.silk_wg = 0;
	if (!dv_silkie_gfx_ok(&s, 100, 1, 'd') || !dv_silkie_gfx_ok(&s, 64, 2, 'd') ||
	    !dv_silkie_gfx_ok(&s, 200, 1, 'd'))
		fail("G is ignored for PNG, f=64 and f=200");
	if (dv_silkie_gfx_ok(&s, 32, 1, 'd'))
		fail("G still needs every peer to know it for RGB");
}

static void
test_headroom(void)
{
	struct dv_session	s;
	char			v[32];

	session_init(&s);
	if (dv_silkie_headroom(&s, v, sizeof v))
		fail("no headroom without a display that reports one");
	silk_ready(&s, "display.1,caps.1");
	dv_strset(&s.silk_display, "kind=eink:color=gray:bits=4:gamut=srgb:headroom=2.5:scale=1:period=0:ppi=0");
	if (!dv_silkie_headroom(&s, v, sizeof v) || strcmp(v, "2.5") != 0)
		fail("the XTGETTCAP headroom is the display's");
	dv_strset(&s.silk_display, "kind=lcd:headroom=0");
	if (!dv_silkie_headroom(&s, v, sizeof v) || strcmp(v, "1") != 0)
		fail("no headroom answers 1");
}

/* The window's image key for a child id, or a null scene. */
static struct dv_kimage *
image_of(struct dv_window *w, unsigned id)
{
	if (w->kimg == nullptr || !w->kimg->contains(id))
		return (nullptr);
	return (&w->kimg->at(id));
}

static uint32_t
token_next(const struct dv_peer *p)
{
	const struct dv_silk_lease	*l;

	if (p->silk_ink == nullptr || p->silk_ink->leases.empty())
		return (4000000000u);	/* nobody gave this out */
	l = &p->silk_ink->leases.back();
	return (l->first + l->used);
}

/* A report as the terminal sends it, valid=1 saying it has a pressure. */
static void
pen(struct dv_peer *p, int phase, int x, int y, int pressure, long ink,
    const char *more, int contact)
{
	std::string	f = "777;tablet-pointer;v=1:op=event:kind=1:phase=" +
	    std::to_string(phase) + ":device=3:contact=" + std::to_string(contact) +
	    ":x=" + std::to_string(x) +
	    ":y=" + std::to_string(y) + ":buttons=1:tool=1:valid=1:p=" +
	    std::to_string(pressure);

	/* The terminal gives the next of the ids it holds, whichever the program
	 * will know it by: ink says it is inking, and which that is to be. */
	if (ink != 0)
		f += ":ink=" + std::to_string(token_next(p));
	f += more;
	dv_silkie_from_host(p, f.data(), f.size());
}

/* A placeholder at the screen's row and column for a cell inside the image. */
static std::string
placeholder_cell(unsigned id, int row, int col, unsigned irow, unsigned icol)
{
	auto	mark = [](unsigned n) {
		uint32_t	c = dv_placeholder_diacritic(n);

		return (std::string{(char)(0xc0 | c >> 6), (char)(0x80 | (c & 0x3f))});
	};

	return ("\033[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) +
	    "H\033[38;2;0;0;" + std::to_string(id) + "m\xf4\x8e\xbb\xae" +
	    mark(irow) + mark(icol) + "\033[39m");
}

/* A placeholder for image id at the screen's row and column, its top left. */
static std::string
placeholder(unsigned id, int row, int col)
{
	return ("\033[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) +
	    "H\033[38;2;0;0;" + std::to_string(id) +
	    "m\xf4\x8e\xbb\xae\xcc\x85\xcc\x85\033[39m");
}

static std::string scene_of(struct dv_window *, unsigned);

static std::string
table_of(struct dv_window *w, unsigned id)
{
	return (payloads(scene_of(w, id)));
}

/*
 * The strokes a terminal draws itself: the table learns them from the
 * reports. The worked example of the ink protocol gives the numbers: image 7
 * of 400 by 300 with its corner 30 pixels across and 40 down.
 */
static void
test_ink_mirror(void)
{
	struct dv_session	s;
	struct dv_window	w;
	struct dv_peer		p;
	int			rd = -1;
	const int		x100 = 25600, y200 = 51200;

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	s.desktop.xpixel = 10;
	s.desktop.ypixel = 20;
	win_init(&w, &s, 1, 0, 0, 1);
	w.framed = 0;
	w.geom.cols = 60;
	w.geom.rows = 30;
	dv_screen_resize(&w.screen, 60, 30);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.focused = &w;
	peer_init(&p, &s);
	p.tty.xpixel = 10;
	p.tty.ypixel = 20;
	silk_ready(&s, "ink.1,tablet-pointer.1,caps.1");
	s.silk_ink_caps = 1;
	s.silk_tablet_caps = 1;
	s.desktop.gfx = 1;
	if (!pipe_to(&w, &rd))
		return;
	put(&w, "\033_Ga=T,U=1,f=200,i=7,s=400,v=300,c=40,r=15,q=2;" +
	    b64("p id=1 d=M0,0L1,1\n") + "\033\\");
	put(&w, placeholder(7, 2, 3));
	osc(&w, "tablet-pointer;v=1:op=mode:mode=1");
	osc(&w, "ink;v=1:op=brush:i=7:k=1000:n=50:fill=#1a1a80:w0=1:w1=5");
	said(&w, rd);

	patched.clear();
	patched_except.clear();
	told_one.clear();
	uint32_t	first_token = token_next(&p);

	pen(&p, 1, x100, y200, 13107, 1000);
	pen(&p, 2, 26880, 51712, 39321);
	pen(&p, 3, 27136, 51712, 0);
	if (table_of(&w, 7).find("k id=1000 pts=70,160,1.8,75,162,3.4,76,162,1 fill=#1a1a80") ==
	    std::string::npos)
		fail("the table gets the points the terminal worked out");
	if (payloads(patched).find("k id=1000 pts=70,160,1.8 fill=#1a1a80\n") == std::string::npos ||
	    payloads(patched).find("a id=1000 pts=75,162,3.4\n") == std::string::npos)
		fail("the other copies are told each record");
	if (patched_except.size() != 3)
		fail("the terminal that drew it is not told");
	if (told_peer != &p || payloads(told_one).find("x id=" + std::to_string(first_token) +
	    "\nk id=1000 pts=70,160,1.8,75,162,3.4,76,162,1 fill=#1a1a80\n") == std::string::npos)
		fail("at its end the terminal's element becomes the program's");
	if (w.silk_strokes == nullptr || !w.silk_strokes->empty())
		fail("up ends the stroke");

	/* A move after the up is no part of the stroke. */
	{
		std::string	before = table_of(&w, 7);

		pen(&p, 2, 26880, 51712, 39321);
		if (table_of(&w, 7) != before)
			fail("no stroke, no point");
	}

	/* Into the group, through the transforms from the root down. */
	put(&w, "\033_Ga=e,i=7,q=2;" +
	    b64("g id=5 t=2,0,0,2,10,20\ng id=6 p=5 t=0,1,-1,0,0,0\n") + "\033\\");
	osc(&w, "ink;v=1:op=brush:i=7:g=5:k=2000:n=9:w1=2");
	pen(&p, 1, x100, y200, 0, 2000);
	pen(&p, 3, x100, y200, 0);
	if (table_of(&w, 7).find("k id=2000 g=5 pts=30,70,2,30,70,2 fill=#000000") ==
	    std::string::npos)
		fail("a point is given in its group's coordinates");
	osc(&w, "ink;v=1:op=brush:i=7:g=6:k=2100:n=9:w1=2");
	pen(&p, 1, x100, y200, 0, 2100);
	pen(&p, 3, x100, y200, 0);
	if (table_of(&w, 7).find("k id=2100 g=6 pts=70,-30,2,") == std::string::npos)
		fail("the root's transform is the outermost");

	/* A transform that cannot be inverted adds no point, then one does. */
	put(&w, "\033_Ga=e,i=7,q=2;" + b64("g id=8 t=0,0,0,0,0,0\n") + "\033\\");
	osc(&w, "ink;v=1:op=brush:i=7:g=8:k=2200:n=9:w1=2");
	pen(&p, 1, x100, y200, 0, 2200);
	if (table_of(&w, 7).find("id=2200") != std::string::npos)
		fail("no inverse, no point");
	put(&w, "\033_Ga=e,i=7,q=2;" + b64("g id=8 t=1,0,0,1,0,0\n") + "\033\\");
	pen(&p, 3, x100, y200, 0);
	if (table_of(&w, 7).find("k id=2200 g=8 pts=70,160,2 ") == std::string::npos)
		fail("the first point that can be taken makes the element");

	/* The terminal cancels: it deleted its own, the others follow. */
	osc(&w, "ink;v=1:op=brush:i=7:k=3000:n=9:w1=2");
	pen(&p, 1, x100, y200, 0, 3000);
	patched.clear();
	patched_except.clear();
	pen(&p, 4, x100, y200, 0);
	if (table_of(&w, 7).find("id=3000") != std::string::npos)
		fail("a cancelled stroke never happened");
	if (payloads(patched).find("x id=3000\n") == std::string::npos ||
	    patched_except.size() != 1)
		fail("the terminal that cancelled has nothing to delete");

	/* We cancel, the pane moved: the terminal must delete its ink as well. */
	pen(&p, 1, x100, y200, 0, 3001);
	patched.clear();
	patched_except.clear();
	told_one.clear();
	dv_silkie_cancel_window(&w);
	if (table_of(&w, 7).find("id=3001") != std::string::npos ||
	    payloads(patched).find("x id=3001\n") == std::string::npos ||
	    payloads(told_one).find("x id=" + std::to_string(token_next(&p) - 1) + "\n") ==
	    std::string::npos)
		fail("a stroke we cancel goes from every copy");

	/* Reports after the cancel belong to no stroke. */
	pen(&p, 2, 26880, 51712, 0);
	if (table_of(&w, 7).find("id=3001") != std::string::npos)
		fail("nothing is added to a cancelled stroke");
	pen(&p, 3, 26880, 51712, 0);

	/* Sent again to the terminal at the pen: its stroke is over there. */
	osc(&w, "ink;v=1:op=brush:i=7:k=3100:n=9:w1=2");
	pen(&p, 1, x100, y200, 0, 3100);
	dv_silkie_ink_resent(&w, &p.view, dv_kitty_virtual_id(&w, 7));
	patched_except.clear();
	pen(&p, 2, 26880, 51712, 0);
	if (!patched_except.empty())
		fail("a terminal sent the scene again is told the rest of the stroke");
	pen(&p, 3, 26880, 51712, 0);

	/* The placement moves under the pen: the ink stays under it too. */
	osc(&w, "ink;v=1:op=brush:i=7:k=3200:n=9:w1=2");
	pen(&p, 1, x100, y200, 0, 3200);
	put(&w, "\033[30;1H\n");
	pen(&p, 3, x100, y200, 0);
	if (table_of(&w, 7).find("k id=3200 pts=70,160,2,70,180,2 ") == std::string::npos)
		fail("the origin is where the placement is shown now");

	/* Ink turned off leaves what was drawn and adds no more. */
	osc(&w, "ink;v=1:op=brush:i=7:k=3300:n=9:w1=2");
	pen(&p, 1, x100, y200, 0, 3300);
	osc(&w, "ink;v=1:op=mode:mode=0");
	pen(&p, 2, 26880, 51712, 0);
	osc(&w, "ink;v=1:op=mode:mode=1");
	pen(&p, 3, 26880, 51712, 0);
	if (table_of(&w, 7).find("k id=3300 pts=70,180,2 ") == std::string::npos)
		fail("mode=0 ends the stroke");

	/* A new transmission replaces the image and ends the stroke. */
	osc(&w, "ink;v=1:op=brush:i=7:k=3400:n=9:w1=2");
	pen(&p, 1, x100, y200, 0, 3400);
	put(&w, "\033_Ga=t,f=200,i=7,s=400,v=300,q=2;" + b64("p id=1 d=M0,0L1,1\n") +
	    "\033\\");
	pen(&p, 3, x100, y200, 0);
	if (table_of(&w, 7).find("id=3400") != std::string::npos)
		fail("the stroke does not reach into the new transmission");

	/* The terminal going away keeps what it drew, and the record of it. */
	osc(&w, "ink;v=1:op=brush:i=7:k=3500:n=9:w1=2");
	pen(&p, 1, x100, y200, 0, 3500);
	dv_silkie_ink_peer_gone(&p);
	if (w.silk_strokes == nullptr || !w.silk_strokes->empty() ||
	    table_of(&w, 7).find("k id=3500") == std::string::npos)
		fail("a stroke outlives the terminal that drew it");
	pen(&p, 3, x100, y200, 0);

	/* A U=1 placement fits its cells: 300 high in 600, 150 from the top. */
	put(&w, "\033_Ga=T,U=1,f=200,i=8,s=400,v=300,c=40,r=30,q=2;" +
	    b64("p id=1 d=M0,0L1,1\n") + "\033\\");
	put(&w, placeholder(8, 2, 3));
	osc(&w, "ink;v=1:op=brush:i=8:k=4000:n=2:w1=2");
	pen(&p, 1, x100, y200, 0, 4000);
	if (table_of(&w, 8).find("k id=4000 pts=70,10,2 ") == std::string::npos)
		fail("the image is centred in the cells it is fitted to");
	/* A long stroke is all there. */
	for (int i = 0; i < 20000; i++)
		pen(&p, 2, x100 + i % 256, y200, 0);
	pen(&p, 3, x100, y200, 0);
	{
		std::string	text = table_of(&w, 8);
		size_t		k = text.find("k id=4000"), n = 0;

		for (size_t i = text.find("pts=", k), e = text.find(' ', i); i < e;
		    i = text.find(',', i + 1))
			n++;
		if (n != 3 * 20002)
			fail("every point of a long stroke is kept");
	}
	/* The ids are spent: ink goes off, and a new brush brings it back. */
	if (w.screen.silk_brush == nullptr ||
	    strstr(w.screen.silk_brush, "k=4001:n=1") == nullptr)
		fail("one id is left");
	pen(&p, 1, x100, y200, 0, 4001);
	host_clear();
	if (p.silk_ink->sent != 1 || strstr(hostbuf, "mode=0") != nullptr)
		fail("mode 0 now would stop the stroke the last id is on");
	pen(&p, 2, 26880, y200, 0);
	if (table_of(&w, 8).find("k id=4001 pts=70,10,2,75,10,2 ") == std::string::npos)
		fail("the stroke on the last id goes on");
	pen(&p, 3, x100, y200, 0);
	if (p.silk_ink->sent != 0 || strstr(hostbuf, "mode=0") == nullptr)
		fail("no ids left, no ink");

	dv_silkie_free_window(&w);
	dv_silkie_free_peer(&p);
	win_free(&w, rd);
}

/*
 * Ids of ours between the terminals and the programs: whichever window and
 * terminal a report comes from, it can only mean one thing.
 */
static void
test_ink_leases(void)
{
	struct dv_session	s;
	struct dv_window	a, b;
	struct dv_peer		p, q;
	int			ra = -1, rb = -1;
	std::string		out;
	const std::string	scene = b64("p id=1 d=M0,0L1,1\n");

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	s.desktop.xpixel = 10;
	s.desktop.ypixel = 20;
	win_init(&a, &s, 1, 0, 0, 1);
	win_init(&b, &s, 2, 40, 0, 2);
	a.framed = b.framed = 0;
	a.geom.cols = b.geom.cols = 40;
	a.geom.rows = b.geom.rows = 20;
	dv_screen_resize(&a.screen, 40, 20);
	dv_screen_resize(&b.screen, 40, 20);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &a, entry);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &b, entry);
	s.desktop.focused = &a;
	peer_init(&p, &s);
	peer_init(&q, &s);
	p.tty.xpixel = q.tty.xpixel = 10;
	p.tty.ypixel = q.tty.ypixel = 20;
	silk_ready(&s, "ink.1,tablet-pointer.1,caps.1");
	s.silk_ink_caps = 1;
	s.silk_tablet_caps = 1;
	s.desktop.gfx = 1;
	if (!pipe_to(&a, &ra) || !pipe_to(&b, &rb))
		return;
	osc(&a, "tablet-pointer;v=1:op=mode:mode=1");
	osc(&b, "tablet-pointer;v=1:op=mode:mode=1");
	/* Two programs that chose the same ids, the same image id too. */
	put(&a, "\033[1;1H\033_Ga=T,f=200,i=9,s=100,v=100,c=10,r=5,C=1,q=2;" + scene + "\033\\");
	put(&b, "\033[1;1H\033_Ga=T,f=200,i=9,s=100,v=100,c=10,r=5,C=1,q=2;" + scene + "\033\\");
	osc(&a, "ink;v=1:op=brush:i=9:k=1000:n=1000:w1=2");
	osc(&b, "ink;v=1:op=brush:i=9:k=1000:n=1000:w1=2");
	said(&a, ra);
	said(&b, rb);

	/* Each terminal has its own ids to give, for what its own pen is near. */
	if (p.silk_ink == nullptr || q.silk_ink == nullptr ||
	    p.silk_ink->leases.size() != 1 || p.silk_ink->leases[0].window != a.id ||
	    peer_frames[&p].find("k=1000") != std::string::npos)
		fail("each terminal is given ids of ours");

	/* The focused window's strokes, from two terminals at once: their tokens
	 * mean what they mean to each, and the program's ids are given in turn. */
	pen(&p, 1, 505, 1005, 0, 1);
	pen(&q, 1, 505, 1005, 0, 1);
	out = said(&a, ra);
	if (out.find("ink=1000") == std::string::npos ||
	    out.find("ink=1001") == std::string::npos)
		fail("two terminals drawing at once get two of the program's ids");
	pen(&p, 3, 505, 1005, 0);
	pen(&q, 3, 505, 1005, 0);
	said(&a, ra);

	/* The pen goes over the window not focused: the brush goes with it. */
	host_clear();
	pen(&p, 0, 405 * 256, 25 * 256, 0);
	said(&b, rb);
	{
		const struct dv_silk_lease	*l = &p.silk_ink->leases.back();
		unsigned			 one = dv_kitty_ink_target(&b, 9);

		if (l->window != b.id || l->dv_id != one ||
		    strstr(hostbuf, ("op=brush:i=" + std::to_string(one)).c_str()) == nullptr)
			fail("a window not focused gets the brush when the pen is over it");
		if (q.silk_ink->leases.back().window != a.id)
			fail("another terminal's pen is somewhere else");
		/* And on its own numbers, though the first window spent the same ones. */
		pen(&p, 1, 405 * 256, 25 * 256, 0, 1);
		if (said(&b, rb).find("ink=1000") == std::string::npos)
			fail("each program has its own ids");
		pen(&p, 3, 405 * 256, 25 * 256, 0);
		said(&b, rb);
		if (table_of(&a, 9).find("k id=1000") == std::string::npos ||
		    table_of(&b, 9).find("k id=1000 pts=5,25,2,5,25,2 ") == std::string::npos)
			fail("the same id in two scenes is two strokes");
	}

	/* A second placement of the image: the pen says which it is to be. */
	put(&b, "\033[10;11H\033_Ga=p,i=9,p=2,c=10,r=5,C=1,q=2\033\\");
	{
		unsigned	lower = dv_kitty_ink_under(&b, 9, 5 * 256, 5 * 256);
		unsigned	upper = dv_kitty_ink_under(&b, 9, 105 * 256, 185 * 256);

		if (lower == 0 || upper == 0 || lower == upper)
			fail("two placements, two ids");
		pen(&p, 0, 505 * 256, 185 * 256, 0);
		if (p.silk_ink->leases.back().dv_id != upper)
			fail("the brush is for the placement the pen is over");
		said(&b, rb);
		pen(&p, 1, 505 * 256, 185 * 256, 0, 1);
		pen(&p, 3, 505 * 256, 185 * 256, 0);
		said(&b, rb);
		/* 105 is 10 cells over and 185 is 9 rows down: 5 and 5 into it. */
		if (table_of(&b, 9).find("k id=1001 pts=5,5,2,5,5,2 ") == std::string::npos)
			fail("the point is on the placement the pen is over");
		pen(&p, 0, 405 * 256, 25 * 256, 0);
		if (p.silk_ink->leases.back().dv_id != lower)
			fail("and back to the other");
		said(&b, rb);
		/* Nothing to ink under the pen: it falls back on the focused window. */
		pen(&p, 0, 79 * 10 * 256, 19 * 20 * 256, 0);
		if (p.silk_ink->leases.back().window != a.id)
			fail("nothing under the pen, the focused window's brush");
		said(&b, rb);
		pen(&p, 0, 405 * 256, 25 * 256, 0);
		said(&b, rb);
	}

	/* The program of a window not focused turns ink off during a stroke: the
	 * terminal is told, and goes back to the brush it would hold. */
	pen(&p, 1, 405 * 256, 25 * 256, 0, 1);
	pen(&p, 2, 405 * 256, 25 * 256, 0);
	peer_frames[&p].clear();
	peer_frames[&q].clear();
	osc(&b, "ink;v=1:op=mode:mode=0");
	if (peer_frames[&p].find("op=mode:mode=0") == std::string::npos ||
	    peer_frames[&q].find("op=mode:mode=0") != std::string::npos)
		fail("the terminal drawing the stroke is the one told to stop");
	if (peer_frames[&p].find("op=mode:mode=1") == std::string::npos ||
	    p.silk_ink->sent != 1 || p.silk_ink->leases.back().window != a.id)
		fail("and ink is on again for the window that wants it");
	pen(&p, 2, 415 * 256, 25 * 256, 0);
	pen(&p, 3, 415 * 256, 25 * 256, 0);
	said(&b, rb);
	if (table_of(&b, 9).find("k id=1002 pts=5,25,2,5,25,2 ") == std::string::npos)
		fail("no point after ink was turned off");
	osc(&b, "ink;v=1:op=mode:mode=1");
	said(&b, rb);

	/* A brush lasts as long as the program's ids, not as long as a lease. */
	s.desktop.focused = &a;
	pen(&p, 0, 505, 1005, 0);
	said(&a, ra);
	for (int i = 0; i < 900; i++) {
		pen(&p, 1, 505, 1005, 0, 1);
		out = said(&a, ra);
		if (out.find("ink=" + std::to_string(1002 + i)) == std::string::npos) {
			fail("every id of a long brush is given in turn");
			break;
		}
		pen(&p, 3, 505, 1005, 0);
		said(&a, ra);
	}
	if (p.silk_ink->leases.size() < 2)
		fail("leases are renewed as they run down");
	/* The terminal has seen the last brush: the leases before it can go. */
	{
		const struct dv_silk_lease	*l = &p.silk_ink->leases.back();
		std::string			 f = "777;ink;v=1:op=cap:id=" +
		    std::to_string(0x60000000u + l->sync) + ":caps=1:mode=1";

		dv_silkie_from_host(&p, f.data(), f.size());
		if (p.silk_ink->leases.size() != 1 || said(&a, ra).find("op=cap") != std::string::npos)
			fail("leases the terminal is past are dropped, and it goes no further");
	}

	/* Two tools on an image. The one that began first ends first: as the table. */
	put(&a, "\033_Ga=t,f=200,i=9,s=100,v=100,q=2;" + scene + "\033\\");
	osc(&a, "ink;v=1:op=brush:i=9:k=7000:n=50:w1=2");
	said(&a, ra);
	stale_ids.clear();
	pen(&p, 1, 505, 1005, 0, 1, "", 1);
	pen(&p, 1, 505, 1005, 0, 1, "", 2);
	pen(&p, 3, 505, 1005, 0, 0, "", 1);
	pen(&p, 3, 505, 1005, 0, 0, "", 2);
	if (!stale_ids.empty())
		fail("strokes ending in the order they began end up as the table has them");
	/* The other way round the terminal's order would differ: it is sent the scene. */
	pen(&p, 1, 505, 1005, 0, 1, "", 1);
	pen(&p, 1, 505, 1005, 0, 1, "", 2);
	pen(&p, 3, 505, 1005, 0, 0, "", 2);
	if (!stale_ids.empty())
		fail("the later one can end first");
	pen(&p, 3, 505, 1005, 0, 0, "", 1);
	if (stale_ids.size() != 1)
		fail("a stroke that cannot end up in its place has the scene sent again");
	/* So too if the program draws during a stroke, or takes our id. */
	stale_ids.clear();
	pen(&p, 1, 505, 1005, 0, 1);
	put(&a, "\033_Ga=e,i=9,q=2;" + b64("p id=2 d=M0,0L1,1\n") + "\033\\");
	pen(&p, 3, 505, 1005, 0);
	if (stale_ids.size() != 1)
		fail("a record drawn over the stroke stays over it");
	stale_ids.clear();
	{
		/* Before the stroke, so the stroke would still end up last. */
		uint32_t	token = token_next(&p);

		put(&a, "\033_Ga=e,i=9,q=2;" + b64("p id=" + std::to_string(token) +
		    " d=M0,0L1,1\n") + "\033\\");
		pen(&p, 1, 505, 1005, 0, 1);
		pen(&p, 3, 505, 1005, 0);
		if (stale_ids.size() != 1)
			fail("a program that takes our id does not lose what it drew");
	}
	said(&a, ra);

	/* The last id, for two terminals: the second has no id to be told of. */
	osc(&a, "ink;v=1:op=brush:i=9:k=8000:n=1:w1=2");
	said(&a, ra);
	told_one.clear();
	{
		uint32_t	token = token_next(&q);

		pen(&p, 1, 505, 1005, 0, 1);
		pen(&q, 1, 505, 1005, 0, 1);
		out = said(&a, ra);
		if (out.find("ink=8000") == std::string::npos || out.find("ink=8001") != std::string::npos ||
		    payloads(told_one) != "x id=" + std::to_string(token) + "\n" || told_peer != &q)
			fail("past the program's last id, the ink goes and the program draws");
		pen(&p, 3, 505, 1005, 0);
		pen(&q, 3, 505, 1005, 0);
		said(&a, ra);
	}

	dv_silkie_free_window(&a);
	dv_silkie_free_window(&b);
	dv_silkie_free_peer(&p);
	dv_silkie_free_peer(&q);
	win_free(&a, ra);
	win_free(&b, rb);
}

/*
 * Ink in a window that crops what it shows: the terminal can only ink through
 * the cells it was sent, and what comes back is taken to the window and to the
 * placement as the program knows it. The numbers are worked out from the
 * formulas of the ink protocol's page, by hand, not by the code under test.
 */
static void
test_ink_crop(void)
{
	struct dv_session	s;
	struct dv_window	w, b;
	struct dv_peer		p;
	int			rd = -1, rb = -1;
	std::string		out;

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	s.desktop.xpixel = 10;
	s.desktop.ypixel = 20;
	win_init(&w, &s, 1, 20, 5, 1);
	win_init(&b, &s, 2, 0, 0, 2);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &b, entry);
	s.desktop.focused = &w;
	peer_init(&p, &s);
	p.tty.xpixel = 10;
	p.tty.ypixel = 20;
	silk_ready(&s, "ink.1,tablet-pointer.1,caps.1");
	s.silk_ink_caps = 1;
	s.silk_tablet_caps = 1;
	s.desktop.gfx = 1;
	if (!pipe_to(&w, &rd) || !pipe_to(&b, &rb))
		return;
	osc(&w, "tablet-pointer;v=1:op=mode:mode=1");
	osc(&b, "tablet-pointer;v=1:op=mode:mode=1");
	said(&b, rb);

	/*
	 * A framed window at 20,5 panned 10 across and 3 down its screen, on a
	 * terminal showing the desktop from 2,1. The image's corner, column 3 and
	 * row 2 of the screen, is out of view: the only cell shown is the one at
	 * column 12 and row 5, which is its row 3 and column 9.
	 */
	w.geom.cols = 30;
	w.geom.rows = 10;
	dv_screen_resize(&w.screen, 60, 30);
	w.ox = 10;
	w.oy = 3;
	p.view.vx = 2;
	p.view.vy = 1;
	b.geom.cols = 15;
	b.geom.rows = 20;
	put(&w, "\033_Ga=T,U=1,f=200,i=7,s=400,v=300,c=40,r=15,q=2;" +
	    b64("p id=1 d=M0,0L1,1\n") + "\033\\");
	put(&w, placeholder_cell(7, 5, 12, 3, 9));
	osc(&w, "ink;v=1:op=brush:i=7:k=1000:n=50:w1=2");
	said(&w, rd);
	/* Column 12 is 2 into the view, 21 across the desktop: host pixel 215. */
	pen(&p, 1, 215 * 256, 150 * 256, 0, 1000);
	out = said(&w, rd);
	if (out.find("x=32000:y=28160") == std::string::npos ||
	    out.find("ink=1000") == std::string::npos)
		fail("a report comes back as a place on the window's screen");
	if (table_of(&w, 7).find("k id=1000 pts=95,70,2 ") == std::string::npos)
		fail("a point of a cropped placement is where its image is");
	/* Out over the other window: still this stroke, not clamped to anything. */
	pen(&p, 2, 0, 0, 0);
	pen(&p, 3, 0, 0, 0);
	out = said(&w, rd);
	if (out.find("x=-23040:y=-10240") == std::string::npos)
		fail("a stroke leaving its window is reported where the pen is");
	if (table_of(&w, 7).find("k id=1000 pts=95,70,2,-120,-80,2,-120,-80,2 ") ==
	    std::string::npos)
		fail("the terminal's points outside are in the table, which shows none");
	if (said(&b, rb).find("phase") != std::string::npos)
		fail("the window under the pen gets nothing of another's stroke");

	/*
	 * A cell of another shape than the one agreed, 16 wide. The terminal fits
	 * 400 by 300 into 640 by 600, 640 by 480 and 60 from the top; the program
	 * into 400 by 600, 400 by 300 and 150 from the top. Pixel 320,220 there
	 * is the point 200,100, which the program gets from 200,250. Only at the
	 * middle of the image would the two fits agree.
	 */
	w.framed = 0;
	w.geom.x = w.geom.y = 0;
	w.geom.cols = 60;
	w.geom.rows = 30;
	w.ox = w.oy = 0;
	w.z = 3;
	p.view.vx = p.view.vy = 0;
	p.tty.xpixel = 16;
	put(&w, "\033_Ga=T,U=1,f=200,i=8,s=400,v=300,c=40,r=30,q=2;" +
	    b64("p id=1 d=M0,0L1,1\n") + "\033\\");
	put(&w, placeholder(8, 0, 0));
	osc(&w, "ink;v=1:op=brush:i=8:k=2000:n=9:w1=2");
	said(&w, rd);
	pen(&p, 1, 320 * 256, 220 * 256, 0, 2000);
	pen(&p, 3, 320 * 256, 220 * 256, 0);
	out = said(&w, rd);
	if (table_of(&w, 8).find("k id=2000 pts=200,100,2,200,100,2 ") == std::string::npos)
		fail("the point is by what the terminal at the pen shows");
	if (out.find("x=51200:y=64000") == std::string::npos)
		fail("the program's formulas give the terminal's point");
	p.tty.xpixel = 10;

	/*
	 * A direct placement, 405 by 290 at column 4 and row 2. It takes 41 by 15
	 * cells, 410 by 300, and is fitted to 410 by 293.58, 3.21 down. The
	 * middle, 245,190 there, is the point 202.5,145, and for a program that
	 * takes it to be at 40,40 at its own size, the position 242.5,185.
	 */
	put(&w, "\033[3;5H\033_Ga=T,f=200,i=9,s=405,v=290,q=2;" +
	    b64("p id=1 d=M0,0L1,1\n") + "\033\\");
	osc(&w, "ink;v=1:op=brush:i=9:k=3000:n=9:w1=2");
	said(&w, rd);
	{
		unsigned	id = dv_kitty_ink_target(&w, 9);

		if (id == 0 || dv_kitty_virtual_id(&w, 9) != 0 ||
		    p.silk_ink->sent != 1 || p.silk_ink->leases.back().dv_id != id)
			fail("a direct placement gets the brush");
		/* Higher up, and back when it goes. */
		put(&w, "\033[20;1H\033_Ga=p,i=9,p=2,z=5,C=1,q=2\033\\");
		if (dv_kitty_ink_target(&w, 9) == id ||
		    p.silk_ink->leases.back().dv_id != dv_kitty_ink_target(&w, 9))
			fail("the brush goes to the placement on top");
		put(&w, "\033_Ga=d,d=i,i=9,p=2,q=2\033\\");
		if (dv_kitty_ink_target(&w, 9) != id ||
		    p.silk_ink->leases.back().dv_id != id)
			fail("the brush goes back when that placement is deleted");
	}
	pen(&p, 1, 245 * 256, 190 * 256, 0, 3000);
	pen(&p, 3, 245 * 256, 190 * 256, 0);
	out = said(&w, rd);
	if (table_of(&w, 9).find("k id=3000 pts=202.5,145,2,202.5,145,2 ") ==
	    std::string::npos)
		fail("a direct placement's point is where it is shown");
	if (out.find("x=62080:y=47360") == std::string::npos)
		fail("the program's formulas give the terminal's point on it too");
	/* The screen scrolls one row under the stroke, and it follows. */
	pen(&p, 1, 245 * 256, 190 * 256, 0, 3001);
	put(&w, "\033[30;1H\n");
	pen(&p, 3, 245 * 256, 170 * 256, 0);
	if (table_of(&w, 9).find("k id=3001 pts=202.5,145,2,202.5,145,2 ") ==
	    std::string::npos)
		fail("a direct placement's stroke follows it up the screen");
	/* Over to the other screen: that stroke is over, the terminal's too. */
	pen(&p, 1, 245 * 256, 170 * 256, 0, 3002);
	put(&w, "\033[?1049h");
	pen(&p, 3, 245 * 256, 170 * 256, 0);
	if (table_of(&w, 9).find("k id=3002 pts=202.5,145,2 ") == std::string::npos)
		fail("a stroke ends with the screen its placement is on");
	if (p.silk_ink->sent != 0)
		fail("nothing to draw on, on this screen");
	put(&w, "\033[?1049l");
	said(&w, rd);
	if (p.silk_ink->sent != 1)
		fail("back on the screen with the placement, ink is back");

	/* The report lands on the other window: the ink is on this one's image. */
	b.framed = 0;
	b.geom.x = 70;
	b.geom.cols = 5;
	b.z = 9;
	patched.clear();
	patched_except.clear();
	told_one.clear();
	{
		uint32_t	token = token_next(&p);

		pen(&p, 1, 705 * 256, 10 * 256, 0, 1);
		if (said(&b, rb).find("ink") != std::string::npos ||
		    said(&w, rd).find("phase") != std::string::npos)
			fail("the other window's program is not told of ink it does not hold");
		if (payloads(told_one) != "x id=" + std::to_string(token) + "\n" ||
		    told_peer != &p || !patched.empty())
			fail("the ink goes from the terminal, the only copy that had it");
		if (strstr(w.screen.silk_brush, "k=3003:n=6") == nullptr)
			fail("no program heard of it, so no program's id is spent");
	}
	pen(&p, 3, 705 * 256, 10 * 256, 0);
	said(&b, rb);
	/* An id nobody gave out. */
	pen(&p, 1, 245 * 256, 170 * 256, 0, 0, ":ink=99");
	if (said(&w, rd).find("ink") != std::string::npos)
		fail("a program is not told of an id we did not give");
	pen(&p, 3, 245 * 256, 170 * 256, 0);
	said(&w, rd);

	/* Focus moves as the pen lands, to a window with no ink: the terminal is
	 * told to stop, but the stroke is the program's, and the table has it all. */
	s.desktop.focused = &b;
	dv_silkie_push(&s);
	if (p.silk_ink->sent != 0)
		fail("no brush in the window focused");
	pen(&p, 1, 245 * 256, 170 * 256, 0, 1);
	if (said(&w, rd).find("ink=3003") == std::string::npos)
		fail("a stroke drawn before the terminal heard is told of");
	pen(&p, 3, 245 * 256, 170 * 256, 0);
	if (table_of(&w, 9).find("k id=3003 pts=202.5,145,2,202.5,145,2 ") == std::string::npos)
		fail("and the terminal gets it all when it ends");
	/* Focus moves during a stroke: it goes on, and ink goes off after it. */
	s.desktop.focused = &w;
	dv_silkie_push(&s);
	pen(&p, 1, 245 * 256, 170 * 256, 0, 1);
	host_clear();
	s.desktop.focused = &b;
	dv_silkie_push(&s);
	if (p.silk_ink->sent != 1 || strstr(hostbuf, "mode=0") != nullptr)
		fail("a change of focus does not cut a stroke short");
	pen(&p, 3, 245 * 256, 170 * 256, 0);
	if (table_of(&w, 9).find("k id=3004 pts=202.5,145,2,202.5,145,2 ") == std::string::npos)
		fail("a stroke keeps going when the focus moves");
	if (p.silk_ink->sent != 0)
		fail("and then there is nothing to ink");
	said(&w, rd);
	said(&b, rb);
	s.desktop.focused = &w;
	dv_silkie_push(&s);

	/* The program never hears of an accident: its ink goes at once. */
	patched.clear();
	patched_except.clear();
	told_one.clear();
	{
		uint32_t	token = token_next(&p);

		pen(&p, 1, 245 * 256, 170 * 256, 0, 1, ":conf=0");
		if (said(&w, rd).find("phase") != std::string::npos ||
		    payloads(told_one) != "x id=" + std::to_string(token) + "\n")
			fail("a contact kept back leaves no ink");
	}
	pen(&p, 3, 245 * 256, 170 * 256, 0, 0, ":conf=0");
	said(&w, rd);
	/* A contact that turns into one is cancelled to the program. */
	told_one.clear();
	{
		uint32_t	token = token_next(&p);

		pen(&p, 1, 245 * 256, 170 * 256, 0, 1);
		if (said(&w, rd).find("ink=3005") == std::string::npos)
			fail("the id kept back is the next one given");
		patched.clear();
		patched_except.clear();
		pen(&p, 2, 245 * 256, 170 * 256, 0, 0, ":conf=0");
		if (said(&w, rd).find("phase=4") == std::string::npos ||
		    payloads(patched).find("x id=3005\n") == std::string::npos ||
		    patched_except.size() != 1 ||
		    payloads(told_one) != "x id=" + std::to_string(token) + "\n" ||
		    table_of(&w, 9).find("id=3005") != std::string::npos ||
		    !w.silk_strokes->empty())
			fail("a stroke cancelled for an accident goes from every copy");
	}
	pen(&p, 3, 245 * 256, 170 * 256, 0, 0, ":conf=0");

	dv_silkie_free_window(&w);
	dv_silkie_free_window(&b);
	dv_silkie_free_peer(&p);
	win_free(&w, rd);
	win_free(&b, rb);
}

static std::string
scene_of(struct dv_window *w, unsigned id)
{
	struct dv_buf		b;
	struct dv_kplace	*p = nullptr;
	std::string		out;

	memset(&b, 0, sizeof b);
	if (w->screen.kpl != nullptr)
		for (auto &[pid, pl] : *w->screen.kpl)
			if (pl.img == id)
				p = &pl;
	if (p == nullptr)
		return ("");
	dv_kitty_transmit(w, p, &b);
	if (b.len > 0)
		out.assign((const char *)b.data, b.len);
	dv_buf_free(&b);
	return (out);
}

static void
test_scene(void)
{
	struct dv_session	s;
	struct dv_window	w;
	int			rd = -1;
	std::string		out, wire;

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	silk_ready(&s, "caps.1");
	s.desktop.gfx = 1;
	if (!pipe_to(&w, &rd))
		return;
	put(&w, "\033_Ga=T,f=200,i=1,s=200,v=100,c=4,r=2,T=1,G=1;" +
	    b64("g id=10 box=0,0,50,50\np id=1 g=10 d=M0,0L1,1 fill=p3:1/0/0*250\nk id=2 pts=1,1,1\n") +
	    "\033\\");
	out = said(&w, rd);
	if (out.find("i=1;OK") == std::string::npos)
		fail("a scene is accepted");
	if (image_of(&w, 1) == nullptr || image_of(&w, 1)->vscene == nullptr)
		fail("a scene is kept as records");
	wire = scene_of(&w, 1);
	if (wire.find("f=200,s=200,v=100,T=1") == std::string::npos)
		fail("T=1 reaches the terminal");
	if (payloads(wire).find("box=0,0,50,50") == std::string::npos ||
	    payloads(wire).find("p3:1/0/0*250") == std::string::npos)
		fail("what we do not interpret is carried as it is");

	/* a=e goes into the table, and to whoever holds the placement. */
	patched.clear();
	put(&w, "\033_Ga=e,i=1;" +
	    b64("a id=2 pts=2,2,2\np id=1 g=10 d=M9,9L8,8\nt id=3 text=" + b64("{\"text\":\"hi\"}") + "\n") +
	    "\033\\");
	out = said(&w, rd);
	if (out.find("i=1;OK") == std::string::npos)
		fail("an edit is answered");
	if (patched.find("a=e") == std::string::npos ||
	    payloads(patched).find("a id=2 pts=2,2,2") == std::string::npos)
		fail("an edit is forwarded, not the whole scene");
	{
		std::string	text = payloads(scene_of(&w, 1));

		if (text.find("k id=2 pts=1,1,1,2,2,2") == std::string::npos)
			fail("an append is concatenated to its target");
		if (text.find("p id=1 g=10 d=M9,9L8,8") == std::string::npos ||
		    text.find("M0,0L1,1") != std::string::npos)
			fail("a record with an existing id replaces it");
		if (text.find("p id=1") > text.find("k id=2"))
			fail("a replaced element keeps its place");
		if (text.find("t id=3") == std::string::npos)
			fail("text records are kept");
	}
	/* Sustained edits: many, without the table growing with them. */
	for (int i = 0; i < 3000; i++) {
		put(&w, "\033_Ga=e,i=1,q=2;" + b64("p id=100 d=M0,0\nx id=100\n") + "\033\\");
	}
	if (dv_kitty_scene_slots(image_of(&w, 1)) > 200)
		fail("deleted records do not pile up");
	said(&w, rd);
	/* Whole or not at all. */
	put(&w, "\033_Ga=e,i=1;" + b64("p id=50 d=M0,0\nzz\n") + "\033\\");
	out = said(&w, rd);
	if (out.find("EINVAL:line 2") == std::string::npos)
		fail("a malformed edit says where");
	if (payloads(scene_of(&w, 1)).find("id=50") != std::string::npos)
		fail("a malformed edit changes nothing");
	put(&w, "\033_Ga=e,i=99;" + b64("x all\n") + "\033\\");
	if (said(&w, rd).find("i=99;ENOENT") == std::string::npos)
		fail("an edit of no image is ENOENT");
	put(&w, "\033_Ga=T,f=32,s=1,v=1,i=5,C=1;AAAAAA==\033\\");
	said(&w, rd);
	put(&w, "\033_Ga=e,i=5;" + b64("x all\n") + "\033\\");
	if (said(&w, rd).find("i=5;EINVAL") == std::string::npos)
		fail("an edit of a raster image is EINVAL");
	/* Groups: x g= takes everything whose parents lead to it. */
	put(&w, "\033_Ga=e,i=1,q=2;" + b64("g id=11 p=10\np id=60 g=11 d=M0,0\ng id=20\np id=61 g=20 d=M0,0\nx g=10\n") + "\033\\");
	{
		std::string	text = payloads(scene_of(&w, 1));

		if (text.find("id=60") != std::string::npos ||
		    text.find("id=11") != std::string::npos ||
		    text.find("g id=10") != std::string::npos)
			fail("x g= removes the whole subtree");
		if (text.find("id=61") == std::string::npos ||
		    text.find("k id=2") == std::string::npos)
			fail("x g= leaves the rest");
	}
	/* A bad resend keeps the scene; a new one replaces it; x all empties. */
	put(&w, "\033_Ga=t,f=200,i=1,s=200,v=100;" + b64("p d=M0,0\n") + "\033\\");
	out = said(&w, rd);
	if (out.find("i=1;EINVAL:line 1") == std::string::npos)
		fail("a bad scene is EINVAL with the line");
	if (payloads(scene_of(&w, 1)).find("k id=2") == std::string::npos)
		fail("nothing of a bad transmission is kept");
	put(&w, "\033_Ga=e,i=1,q=2;" + b64("x all\n") + "\033\\");
	if (scene_of(&w, 1).find("a=t") == std::string::npos ||
	    payloads(scene_of(&w, 1)) != "\n")
		fail("an empty scene still exists at the terminal");
	/* Chunks and compression work as for a=t: continuations say only m=. */
	put(&w, "\033_Ga=t,f=200,i=1,s=9,v=9;" + b64("p id=1 d=M0,0\n") + "\033\\");
	said(&w, rd);
	{
		std::string	p = b64("p id=70 d=M0,0\n");

		put(&w, "\033_Ga=e,i=1,m=1;" + p.substr(0, 8) + "\033\\");
		put(&w, "\033_Gm=0;" + p.substr(8) + "\033\\");
		if (said(&w, rd).find("i=1;OK") == std::string::npos ||
		    payloads(scene_of(&w, 1)).find("p id=70") == std::string::npos)
			fail("a chunked edit");
	}
	/* A record's image is one the program sent; the terminal has another id. */
	put(&w, "\033_Ga=t,f=32,s=1,v=1,i=8,q=2;AAAAAA==\033\\");
	put(&w, "\033_Ga=e,i=1,q=2;" + b64("i id=71 img=8\n") + "\033\\");
	{
		std::string	text = payloads(scene_of(&w, 1));
		struct dv_view	v;
		struct dv_buf	b;

		if (text.find("i id=71 img=8\n") != std::string::npos ||
		    w.kres == nullptr || !w.kres->contains(8))
			fail("a scene names an image by the terminal's id");
		memset(&v, 0, sizeof v);
		v.gfx = 1;
		memset(&b, 0, sizeof b);
		dv_kitty_sync(&s, &v, &b);
		dv_buf_append(&b, "", 1);
		if (strstr((char *)b.data, ("i=" + std::to_string(w.kres->at(8))).c_str()) == nullptr)
			fail("that image goes to the terminal without a placement");
		b.len = 0;
		dv_kitty_sync(&s, &v, &b);
		if (b.len != 0)
			fail("and only once");
		delete v.kres_gen;
		delete v.vfont_sent;
		dv_buf_free(&b);
	}
	dv_kitty_free(&w);
	dv_kitty_close(&w);
	win_free(&w, rd);
}

static void
test_service(void)
{
	struct dv_session	s;
	struct dv_window	a, b;
	struct dv_peer		p;
	int			ra = -1, rb = -1;
	std::string		out, req, serial;

	session_init(&s);
	win_init(&a, &s, 1, 2, 1, 1);
	win_init(&b, &s, 2, 20, 1, 2);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &a, entry);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &b, entry);
	peer_init(&p, &s);
	p.fd = 7;
	if (!pipe_to(&a, &ra) || !pipe_to(&b, &rb))
		return;

	/* Nothing attached that draws scenes: they are drawn here, so the
	 * service is answered here, of the fonts of this machine. */
	auto payload_of = [](const std::string &reply) {
		size_t	k = reply.find(";OK;"), e = reply.find("\033\\", k == std::string::npos ? 0 : k);

		return (k == std::string::npos || e == std::string::npos ? std::string() :
		    unb64(reply.substr(k + 4, e - k - 4)));
	};
	put(&a, "\033_Ga=v,f=200,i=77;" + b64("{\"op\":\"fonts\"}") + "\033\\");
	out = said(&a, ra);
	if (out.find("i=77;OK;") == std::string::npos)
		fail("with no terminal to ask, the vector service is answered here");
	/* Measured here too. A machine with no fonts at all says so. */
	put(&a, "\033_Ga=v,f=200,i=76;" + b64("{\"op\":\"measure\",\"text\":\"Hello\",\"size\":24}") + "\033\\");
	out = said(&a, ra);
	if (out.find("i=76;EINVAL:") != std::string::npos) {
		if (unb64(out.substr(out.find("EINVAL:") + 7)).find("No fonts are installed") == std::string::npos)
			fail("a layout is measured here, or there are no fonts to measure it in");
	} else if (out.find("i=76;OK;") == std::string::npos ||
	    payload_of(out).find("\"width\":") == std::string::npos ||
	    payload_of(out).find("\"ink\":[") == std::string::npos ||
	    payload_of(out).find("\"missing\":[]") == std::string::npos)
		fail("a layout is measured here");
	put(&a, "\033_Ga=v,f=200,i=78;" + b64("{\"op\":\"upload\",\"id\":\"x\",\"data\":\"QUJD\"}") + "\033\\");
	if (said(&a, ra).find("i=78;EINVAL:") == std::string::npos)
		fail("and what is not a font is refused here");
	put(&a, "\033_Ga=v,f=200,i=79;" + b64("{\"op\":\"measure\",\"text\":\"x\",\"size\":-2}") + "\033\\");
	if (said(&a, ra).find("i=79;EINVAL:") == std::string::npos)
		fail("as is a layout that is not one");
	silk_ready(&s, "caps.1");

	/* Two windows, one request id: answered each by its own. */
	auto answer = [&p](const std::string &wire, const std::string &json) {
		size_t		i = wire.find("i=", wire.rfind("a=v"));
		std::string	hid, r;

		hid = wire.substr(i + 2, wire.find(',', i) - i - 2);
		r = "Gi=" + hid + ";OK;" + b64(json);
		dv_kitty_host_reply(&p, (const unsigned char *)r.data(), r.size());
	};
	primary.clear();
	put(&a, "\033_Ga=v,f=200,i=77;" + b64("{\"op\":\"upload\",\"id\":\"body\",\"data\":\"QUJD\"}") + "\033\\");
	req = primary;
	if (req.find("a=v,f=200") == std::string::npos ||
	    payloads(req).find("\"id\":\"dvf") == std::string::npos ||
	    payloads(req).find("\"body\"") != std::string::npos)
		fail("an upload reaches the terminal under a name of ours");
	serial = payloads(req).substr(payloads(req).find("dvf"));
	serial = serial.substr(0, serial.find('"'));
	primary.clear();
	put(&b, "\033_Ga=v,f=200,i=77;" + b64("{\"op\":\"upload\",\"id\":\"body\",\"data\":\"REVG\"}") + "\033\\");
	{
		std::string	reqb = primary, other;

		other = payloads(reqb).substr(payloads(reqb).find("dvf"));
		other = other.substr(0, other.find('"'));
		if (other == serial)
			fail("two windows' body are two fonts at the terminal");
		answer(req, "{\"id\":\"" + serial + "\",\"units\":1000}");
		out = said(&a, ra);
		if (out.find("i=77;OK;") == std::string::npos)
			fail("the upload is answered");
		if (unb64(out.substr(out.find("OK;") + 3)).find("\"id\":\"body\"") == std::string::npos)
			fail("the answer is in the program's terms");
		if (said(&b, rb).find("i=77") != std::string::npos)
			fail("the other window was not asking yet");
		answer(reqb, "{\"id\":\"" + other + "\"}");
		said(&b, rb);

		/* A chunked request, with the action said only once. */
		std::string	q = b64("{\"op\":\"measure\",\"font\":\"@body\",\"fallback\":[\"Iosevka\",\"@body\"],\"runs\":[{\"text\":\"@body\",\"font\":\"@nope\"}]}");

		primary.clear();
		put(&a, "\033_Ga=v,f=200,i=78,m=1;" + q.substr(0, 16) + "\033\\");
		if (!primary.empty())
			fail("a request waits for its last chunk");
		put(&a, "\033_Gm=0;" + q.substr(16) + "\033\\");
		std::string	sent = payloads(primary);

		if (sent.find("\"font\":\"@" + serial + "\"") == std::string::npos ||
		    sent.find("[\"Iosevka\",\"@" + serial + "\"]") == std::string::npos)
			fail("a request names this window's font");
		if (sent.find("\"text\":\"@body\"") == std::string::npos)
			fail("text that looks like a handle is left alone");
		if (sent.find("\"font\":\"@dvu.nope\"") == std::string::npos)
			fail("a name it never uploaded cannot find another's");
		/* An error comes back in its terms too. */
		{
			size_t		i = primary.find("i=");
			std::string	hid = primary.substr(i + 2, primary.find(',', i) - i - 2);
			std::string	r = "Gi=" + hid + ";EINVAL:" +
			    b64("{\"error\":\"no font @dvu.nope\"}");

			dv_kitty_host_reply(&p, (const unsigned char *)r.data(), r.size());
			out = said(&a, ra);
			if (out.find("i=78;EINVAL:") == std::string::npos ||
			    unb64(out.substr(out.find("EINVAL:") + 7)).find("@nope") == std::string::npos)
				fail("an error is relayed in the program's terms");
		}

		/* The fonts op shows a window its own uploads. */
		primary.clear();
		put(&a, "\033_Ga=v,f=200,i=79;" + b64("{\"op\":\"fonts\"}") + "\033\\");
		answer(primary, "{\"version\":1,\"features\":[\"x\"],\"session\":[{\"id\":\"" +
		    serial + "\",\"glyphs\":3},{\"id\":\"" + other + "\",\"glyphs\":4}],\"fonts\":[]}");
		out = said(&a, ra);
		{
			std::string	t = unb64(out.substr(out.find("OK;") + 3));

			if (t.find("\"id\":\"body\",\"glyphs\":3") == std::string::npos ||
			    t.find("\"glyphs\":4") != std::string::npos ||
			    t.find("dvf") != std::string::npos ||
			    t.find("\"features\":[\"x\"]") == std::string::npos)
				fail("fonts lists this window's uploads, relaying the rest");
		}

		/* Text in a scene is shaped by every client, so the font stays. */
		std::string	layout = b64("{\"font\":\"@body\",\"text\":\"T\"}");

		put(&a, "\033_Ga=T,f=200,i=1,s=9,v=9,c=2,r=2;" + b64("t id=1 text=" + layout + "\n") + "\033\\");
		out = said(&a, ra);
		if (out.find("i=1;OK") == std::string::npos)
			fail("a scene with session text");
		if (payloads(scene_of(&a, 1)).find(b64("{\"font\":\"@" + serial + "\",\"text\":\"T\"}")) == std::string::npos)
			fail("a text record names the terminal's font");
		put(&a, "\033_Ga=v,f=200,i=80;" + b64("{\"op\":\"release\",\"id\":\"body\"}") + "\033\\");
		out = said(&a, ra);
		if (out.find("i=80;OK") == std::string::npos)
			fail("release is answered");
		put(&a, "\033_Ga=T,f=200,i=2,s=9,v=9;" + b64("t id=1 text=" + layout + "\n") + "\033\\");
		out = said(&a, ra);
		if (out.find("i=2;EINVAL:line 1: unknown font @body") == std::string::npos)
			fail("after a release new layouts fail");
		if (a.vwin == nullptr)
			fail("window state");
		{
			struct dv_view	v;
			struct dv_buf	bb;
			std::string	seen;

			memset(&v, 0, sizeof v);
			v.gfx = 1;
			v.vector = 1;
			memset(&bb, 0, sizeof bb);
			dv_kitty_sync(&s, &v, &bb);
			seen.assign((const char *)bb.data, bb.len);
			if (payloads(seen).find(serial) == std::string::npos ||
			    payloads(seen).find(other) == std::string::npos)
				fail("a released font in use goes to a new client");
			if (seen.find("q=2") == std::string::npos)
				fail("a replay wants no answer");
			bb.len = 0;
			/* Once nothing uses it, it goes. */
			put(&a, "\033_Ga=d,d=I,i=1,q=2\033\\");
			dv_kitty_sync(&s, &v, &bb);
			seen.assign((const char *)bb.data, bb.len);
			if (payloads(seen).find("\"op\":\"release\",\"id\":\"" + serial) == std::string::npos)
				fail("a font nothing uses is released at the terminal");
			delete v.vfont_sent;
			delete v.kres_gen;
			dv_buf_free(&bb);
		}
		/* A terminal that goes away answers what it owed. */
		said(&a, ra);
		primary.clear();
		put(&a, "\033_Ga=v,f=200,i=81;" + b64("{\"op\":\"fonts\"}") + "\033\\");
		dv_kitty_peer_gone(&p);
		if (said(&a, ra).find("i=81;EINVAL:") == std::string::npos)
			fail("a request does not hang on a terminal that left");
		/* A failed upload leaves the name as it was. */
		primary.clear();
		put(&b, "\033_Ga=v,f=200,i=82;" + b64("{\"op\":\"upload\",\"id\":\"body\",\"data\":\"bad\"}") + "\033\\");
		{
			size_t		i = primary.find("i=");
			std::string	hid = primary.substr(i + 2, primary.find(',', i) - i - 2);
			std::string	r = "Gi=" + hid + ";EINVAL:" + b64("{\"error\":\"bad font\"}");

			dv_kitty_host_reply(&p, (const unsigned char *)r.data(), r.size());
		}
		if (said(&b, rb).find("i=82;EINVAL:") == std::string::npos)
			fail("a failed upload is reported");
		put(&b, "\033_Ga=T,f=200,i=3,s=9,v=9,q=1;" + b64("t id=1 text=" + layout + "\n") + "\033\\");
		if (said(&b, rb).find("EINVAL") != std::string::npos)
			fail("the upload before it still stands");
	}
	/* No terminal to ask at all. */
	primary_fd = -1;
	put(&a, "\033_Ga=v,f=200,i=83;" + b64("{\"op\":\"upload\",\"id\":\"x\",\"data\":\"QQ==\"}") + "\033\\");
	if (said(&a, ra).find("i=83;EINVAL:") == std::string::npos)
		fail("an upload with no terminal fails, and leaves nothing");
	primary_fd = 7;
	/* Quiet means quiet. */
	s.silk_any = 0;
	put(&a, "\033_Ga=v,f=200,i=84,q=2;" + b64("{\"op\":\"fonts\"}") + "\033\\");
	if (said(&a, ra).find("i=84") != std::string::npos)
		fail("q=2 silences an error");
	dv_kitty_free(&a);
	dv_kitty_close(&a);
	dv_kitty_free(&b);
	dv_kitty_close(&b);
	dv_kitty_free_session(&s);
	win_free(&a, ra);
	win_free(&b, rb);
}

static void
test_pointer_image(void)
{
	struct dv_session	s;
	struct dv_window	w;
	struct dv_buf		rep;

	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.focused = &w;
	silk_ready(&s, "caps.1");
	s.desktop.gfx = 1;
	/* No such image yet: the shape below it shows. */
	put(&w, "\033]22;>wait,image:9:24:24\033\\");
	if (memmem(w.host_out.data, w.host_out.len, "22;wait", 7) == nullptr)
		fail("an image that does not exist falls back down the stack");
	w.host_out.len = 0;
	put(&w, "\033_Ga=t,f=32,s=1,v=1,i=9,q=2;AAAAAA==\033\\");
	{
		std::string	sent((const char *)w.host_out.data, w.host_out.len);
		std::string	want = "image:" + std::to_string(w.kres != nullptr &&
		    w.kres->contains(9) ? w.kres->at(9) : 0) + ":24:24";

		if (w.kres == nullptr || sent.find(want) == std::string::npos ||
		    sent.find("image:9:") != std::string::npos)
			fail("the shape comes when its image does, under the host's id");
		memset(&rep, 0, sizeof rep);
		std::string	back = "22;" + want;

		dv_kitty_pointer_reply(&w, back.data(), back.size(), &rep);
		if (std::string((const char *)rep.data, rep.len) != "22;image:9:24:24")
			fail("a query reads back the program's id");
		dv_buf_free(&rep);
	}
	w.host_out.len = 0;
	put(&w, "\033_Ga=d,d=I,i=9\033\\");
	if (memmem(w.host_out.data, w.host_out.len, "22;wait", 7) == nullptr)
		fail("deleting the image does not pop the stack, the shape below shows");
	if (w.npshape != 2)
		fail("the stack is as it was");
	dv_buf_free(&w.host_out);
	dv_kitty_free(&w);
	dv_kitty_close(&w);
	win_free(&w, -1);
}

static std::string
strf_test(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static std::string
strf_test(const char *fmt, ...)
{
	va_list		ap;
	char		*p = nullptr;
	int		n;
	std::string	out;

	va_start(ap, fmt);
	n = vasprintf(&p, fmt, ap);
	va_end(ap);
	if (n >= 0)
		out.assign(p, (size_t)n);
	free(p);
	return (out);
}

/* The keys and pixels of the first image a transmission carries, inflated
 * if it was sent deflated. */
static std::string
sent_pixels(const std::string &wire, std::string *keys)
{
	size_t		i = wire.find("\033_Ga=t,"), semi;
	std::string	raw, out;

	keys->clear();
	if (i == std::string::npos ||
	    (semi = wire.find(';', i)) == std::string::npos)
		return ("");
	keys->assign(wire, i + 3, semi - i - 3);
	raw = payloads(wire.substr(i, wire.find("a=p", i) - i));
	if (keys->find("o=z") == std::string::npos)
		return (raw);
	for (uLongf n = raw.size() * 4 + 64;; n *= 2) {
		uLongf	got = n;

		out.assign((size_t)n, '\0');
		int	rc = uncompress((Bytef *)out.data(), &got,
		    (const Bytef *)raw.data(), (uLong)raw.size());

		if (rc == Z_OK) {
			out.resize((size_t)got);
			return (out);
		}
		if (rc != Z_BUF_ERROR)
			return ("");
	}
}

/* The placement of image id, as a view would be sent it. */
static std::string
sent_to(struct dv_window *w, unsigned id, const struct dv_view *v)
{
	struct dv_buf		b;
	struct dv_kplace	*p = nullptr;
	std::string		out;

	memset(&b, 0, sizeof b);
	if (w->screen.kpl != nullptr)
		for (auto &[pid, pl] : *w->screen.kpl)
			if (pl.child_id == id)
				p = &pl;
	if (p == nullptr)
		return ("");
	dv_kitty_transmit(w, p, &b, v);
	if (b.len > 0)
		out.assign((const char *)b.data, b.len);
	dv_buf_free(&b);
	return (out);
}

/*
 * Silkie stereo depth: a text attribute in millidegrees or reference
 * pixels with a vertical phase, validated as a whole, kept with the cells
 * and the placements, and offered by whichever terminal listed depth.1.
 */
static void
test_depth(void)
{
	struct dv_session		 s;
	struct dv_window		 w;
	struct dv_peer			 a, b;
	TAILQ_HEAD(depth_peer_head, dv_peer) peers;
	int				 rd = -1;
	const struct dv_depth_style	*st;
	uint32_t			 before;

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.focused = &w;
	s.desktop.gfx = 1;
	if (!pipe_to(&w, &rd))
		return;

	/* Kept with no terminal attached, as Silkie keeps it while flat. */
	osc(&w, "depth;v=1:op=set:d=-150");
	st = dv_depth_get(w.screen.depth);
	if (st->kind != DV_DEPTH_DEG || st->d != -150 || st->y != 0.0f)
		fail("d sets millidegrees");
	put(&w, "if");
	if (dv_screen_at(&w.screen, 0, 0)->depth != w.screen.depth ||
	    dv_screen_at(&w.screen, 1, 0)->depth != w.screen.depth)
		fail("written cells carry the depth");
	osc(&w, "depth;v=1:op=set:y=0.125");
	st = dv_depth_get(w.screen.depth);
	if (st->kind != DV_DEPTH_DEG || st->d != -150 || st->y != 0.125f)
		fail("y alone keeps the depth");
	osc(&w, "depth;v=1:op=set:p=-0.25");
	st = dv_depth_get(w.screen.depth);
	if (st->kind != DV_DEPTH_PX || st->p != -0.25f || st->y != 0.125f)
		fail("p replaces d and keeps the phase");
	osc(&w, "depth;v=1:op=set:d=250");
	st = dv_depth_get(w.screen.depth);
	if (st->kind != DV_DEPTH_DEG || st->d != 250 || st->p != 0.0f)
		fail("d replaces p");
	osc(&w, "depth;v=1:op=set:d=0");
	st = dv_depth_get(w.screen.depth);
	if (st->kind != DV_DEPTH_FLAT || st->y != 0.125f)
		fail("d=0 resets only the depth");
	osc(&w, "depth;v=1:op=set:y=0");
	if (w.screen.depth != 0)
		fail("y=0 then leaves the plane itself");
	osc(&w, "depth;v=1:op=set:d=2147483647:y=-1e3");
	st = dv_depth_get(w.screen.depth);
	if (st->d != 2147483647 || st->y != -1000.0f)
		fail("the full range and exponents are accepted");

	/* Malformed is rejected as a whole. */
	osc(&w, "depth;v=1:op=set:d=-150:y=0.5");
	before = w.screen.depth;
	for (const char *bad : {
	    "depth;v=1:op=set:d=1:p=1",
	    "depth;v=1:op=set:d=2147483648",
	    "depth;v=1:op=set:d=1.5",
	    "depth;v=1:op=set:d=7:y=nan",
	    "depth;v=1:op=set:p=inf",
	    "depth;v=1:op=set:p=0x10",
	    "depth;v=1:op=set:p=1e39",
	    "depth;v=1:op=set:p=1e-60",
	    "depth;v=1:op=set:p=",
	    "depth;v=1:op=set:p=.",
	    "depth;v=1:op=set:d=3:d=4",
	    "depth;v=2:op=set:d=3" }) {
		osc(&w, bad);
		if (w.screen.depth != before)
			fail(bad);
	}
	osc(&w, "depth;v=1:op=set:d=-150:y=0.5:later=1");
	if (w.screen.depth != before)
		fail("an unknown field is ignored, not a rejection");
	osc(&w, "depth;v=1:op=set:d=-150:y=0.5");
	if (w.screen.depth != before)
		fail("the same depth is the same id");

	/* Saved with the cursor; reset by SGR 0, op=reset, RIS and the
	 * shell-prompt mark; kept by colour changes and a caps reset. */
	{
		uint32_t	d = w.screen.depth;

		put(&w, "\033[31;1m");
		if (w.screen.depth != d)
			fail("a colour change keeps the depth");
		put(&w, "\0337\033[0m");
		if (w.screen.depth != 0)
			fail("SGR 0 resets the depth");
		put(&w, "\0338");
		if (w.screen.depth != d)
			fail("cursor restore brings the depth back");
		put(&w, "\033[m");
		if (w.screen.depth != 0)
			fail("an empty SGR is SGR 0");
		put(&w, "\0338");
		silk_ready(&s, "depth.1,caps.1");
		osc(&w, "caps;v=1:op=reset");
		if (w.screen.depth != d)
			fail("a caps reset, for modes, leaves the depth");
		osc(&w, "depth;v=1:op=reset");
		if (w.screen.depth != 0)
			fail("op=reset returns to the plane");
		put(&w, "\0338");
		put(&w, "\033]133;A\033\\");
		if (w.screen.depth != 0)
			fail("the shell-prompt mark resets the depth");
		put(&w, "\0338");
		put(&w, "\033c");
		if (w.screen.depth != 0)
			fail("RIS resets the depth");
	}

	/* The cells and the table survive being handed to another server. */
	{
		struct dv_cell		 in[2], back[2];
		struct dv_buf		 bb;
		struct dv_depth_style	 px = { DV_DEPTH_PX, 0, -0.1f, 0.3f };
		uint32_t		 id = dv_depth_intern(&px);
		char			 line[80];

		memset(in, 0, sizeof in);
		memset(back, 0, sizeof back);
		in[0].ch = in[1].ch = 'a';
		in[1].depth = id;
		memset(&bb, 0, sizeof bb);
		dv_cells_encode(&bb, in, 2);
		dv_buf_append(&bb, "", 1);
		if (dv_cells_decode((char *)bb.data, back, 2) != 2 ||
		    back[0].depth != 0 || back[1].depth != id)
			fail("cells encode their depth");
		dv_buf_free(&bb);
		/* An older writer's cells have no depth field. */
		if (dv_cells_decode("61,0,0,0,0,0,0,0,64,64,64,0,0,0", back, 1) != 1 ||
		    back[0].depth != 0)
			fail("an older writer's cells are flat");
		memset(&bb, 0, sizeof bb);
		dv_depth_save(&bb);
		dv_buf_append(&bb, "", 1);
		snprintf(line, sizeof line, "dstyle %u ", id);
		if (strstr((char *)bb.data, line) == nullptr)
			fail("the depth table is saved");
		dv_buf_free(&bb);
		snprintf(line, sizeof line, "5000 %d 0 %08x %08x", DV_DEPTH_PX,
		    0xbdcccccdu, 0x3e99999au);
		if (!dv_depth_restore(line) || dv_depth_get(5000)->p != -0.1f ||
		    dv_depth_get(5000)->y != 0.3f || dv_depth_intern(&px) != 5000)
			fail("a restored depth keeps its id and exact floats");
		if (dv_depth_restore("0 1 5 0 0") || dv_depth_restore("7 9 5 0 0") ||
		    dv_depth_restore("7 2 0 7f800000 0"))
			fail("a bad table line is refused");
	}

	/* Placements take the pen's depth and tell a depth.1 terminal it. */
	{
		struct dv_view	 vd, vf;
		std::string	 out;
		size_t		 set, place, reset;

		memset(&vd, 0, sizeof vd);
		memset(&vf, 0, sizeof vf);
		vd.depth = 1;
		osc(&w, "depth;v=1:op=set:d=-150");
		put(&w, "\033_Ga=T,f=24,i=3,s=1,v=1,c=1,r=1,q=2;" + b64(std::string("\xff\x00\x00", 3)) +
		    "\033\\");
		osc(&w, "depth;v=1:op=reset");
		out = sent_to(&w, 3, &vd);
		set = out.find("\033]777;depth;v=1:op=set:d=-150:y=0\033\\");
		place = out.find("a=p,U=1");
		reset = out.find("\033]777;depth;v=1:op=reset\033\\", place);
		if (set == std::string::npos || place == std::string::npos ||
		    set > place)
			fail("a placement is made at its depth");
		if (reset == std::string::npos)
			fail("the terminal is taken back to the plane after it");
		if (sent_to(&w, 3, &vf).find("777;depth") != std::string::npos)
			fail("a terminal without depth.1 is not sent depth");
		put(&w, "\033_Ga=T,f=24,i=5,s=1,v=1,c=1,r=1,q=2;" + b64(std::string("\xff\x00\x00", 3)) +
		    "\033\\");
		out = sent_to(&w, 5, &vd);
		if (out.find("\033]777;depth;v=1:op=reset\033\\") > out.find("a=p,U=1"))
			fail("a flat placement says so: the terminal's depth is not ours");
	}

	/* Vector depth is the scene's own: records pass through untouched. */
	{
		struct dv_view	 vd;
		std::string	 out;

		memset(&vd, 0, sizeof vd);
		vd.depth = 1;
		vd.vector = 1;		/* it draws scenes itself */
		silk_ready(&s, "depth.1,caps.1");
		put(&w, "\033_Ga=T,f=200,i=6,s=100,v=50,c=2,r=1,q=2;" +
		    b64("g id=1 depth=-150\np id=2 g=1 d=M10,10h80v40h-80Z fill=#4488ff\n"
		    "p id=3 g=1 depth_px=-0.25 phase=0.5 d=M0,0L1,1\n") + "\033\\");
		out = payloads(sent_to(&w, 6, &vd));
		if (out.find("g id=1 depth=-150") == std::string::npos ||
		    out.find("depth_px=-0.25 phase=0.5") == std::string::npos)
			fail("vector depth and phase reach the terminal");
	}

	/* --3d-focus moves a depth toward the viewer, phase kept. */
	{
		struct dv_depth_style	 px = { DV_DEPTH_PX, 0, -0.25f, 0.5f };
		struct dv_depth_style	 dg = { DV_DEPTH_DEG, -150, 0.0f, 0.0f };
		const struct dv_depth_style *r;
		float			 f;

		r = dv_depth_get(dv_depth_nearer(0, 4.0f));
		if (r->kind != DV_DEPTH_PX || r->p != -4.0f)
			fail("the plane moves to -4 pixels");
		r = dv_depth_get(dv_depth_nearer(dv_depth_intern(&px), 4.0f));
		if (r->kind != DV_DEPTH_PX || r->p != -4.25f || r->y != 0.5f)
			fail("pixel depth moves exactly, phase kept");
		r = dv_depth_get(dv_depth_nearer(dv_depth_intern(&dg), 4.0f));
		if (r->kind != DV_DEPTH_DEG || r->d != -258)
			fail("angular depth moves by Silkie's default field of view");
		if (dv_depth_nearer(dv_depth_intern(&px), 0.0f) != dv_depth_intern(&px))
			fail("no focus moves nothing");
		if (!dv_parse_px("2.5", &f) || f != 2.5f || dv_parse_px("x", &f))
			fail("--3d-focus takes a number of pixels");
	}

	/* Offered by a terminal that listed depth.1; queried by none. */
	peer_init(&a, &s);
	peer_init(&b, &s);
	dv_strset(&a.silkie_blob, "ext=depth.1,caps.1\n");
	dv_strset(&b.silkie_blob, "ext=caps.1\n");
	TAILQ_INIT(&peers);
	TAILQ_INSERT_TAIL(&peers, &a, entry);
	TAILQ_INSERT_TAIL(&peers, &b, entry);
	dv_silkie_recompute(&s, TAILQ_FIRST(&peers));
	if (!a.view.depth || b.view.depth)
		fail("only the peer that listed depth.1 is painted depth");
	if (!has(s.silk_ext, "depth"))
		fail("depth.1 is listed to programs while a terminal draws it");
	said(&w, rd);
	osc(&w, "caps;v=1:op=query:id=4");
	if (said(&w, rd).find("depth.1") == std::string::npos)
		fail("the caps reply lists depth.1");
	dv_strset(&a.silkie_blob, "ext=caps.1\n");
	dv_silkie_recompute(&s, TAILQ_FIRST(&peers));
	if (a.view.depth || has(s.silk_ext, "depth"))
		fail("nothing offers depth when no terminal listed it");
	osc(&w, "depth;v=1:op=set:d=40");
	if (dv_depth_get(w.screen.depth)->d != 40)
		fail("the attribute is kept with no terminal to draw it");
	dv_silkie_free_peer(&a);
	dv_silkie_free_peer(&b);
	dv_kitty_free(&w);
	dv_kitty_close(&w);
	dv_silkie_free_session(&s);
	win_free(&w, rd);
}

/* Silkie's packed pixels: accepted from programs, passed to a terminal that
 * listed them, and decoded for every other. */
static void
test_packed(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_peer		 a;
	int			 rd = -1;
	std::string		 out;

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.gfx = 1;
	if (!pipe_to(&w, &rd))
		return;
	for (long f : { 301, 302, 304, 308, 516, 8, 16, 401, 402, 404, 408, 316, 632, 664 })
		if (!dv_silkie_gfx_ok(&s, f, 0, 'd') || !dv_silkie_gfx_ok(&s, f, 0, 's') ||
		    dv_silkie_gfx_ok(&s, f, 0, 'g'))
			fail("packed, indexed, palette and deep formats take the ordinary media");
	if (dv_silkie_gfx_ok(&s, 3, 0, 'd') || dv_silkie_gfx_ok(&s, 516, 1, 'd') ||
	    !dv_silkie_gfx_ok(&s, 301, 1, 'd') || !dv_silkie_gfx_ok(&s, 8, 2, 'd') ||
	    dv_silkie_gfx_ok(&s, 632, 2, 'd') || dv_silkie_gfx_ok(&s, 403, 0, 'd'))
		fail("only real formats; a gamut only where it means something");
	/* The numbers of 2026-11-16 and before are no longer packed formats. */
	for (long f : { 1, 2, 4 })
		if (dv_silkie_gfx_ok(&s, f, 0, 'd'))
			fail("f=1, 2 and 4 are refused");

	struct {
		const char	*keys;
		std::string	 data;
		std::string	 want;		/* RGBA */
	} cases[] = {
		/* nine one-bit pixels: two bytes a row, padding ignored */
		{ "f=301,s=9,v=2", std::string("\x55\x80\xaa\x7f", 4),
		  std::string("\0\0\0\xff\xff\xff\xff\xff", 8) },
		{ "f=302,s=4,v=1", std::string("\x1b", 1),
		  std::string("\0\0\0\xff\x55\x55\x55\xff\xaa\xaa\xaa\xff\xff\xff\xff\xff", 16) },
		{ "f=304,s=3,v=2", std::string("\xf0\x00\x12\x30", 4),
		  std::string("\xff\xff\xff\xff\0\0\0\xff", 8) },
		{ "f=308,s=2,v=1", std::string("\x10\xfe", 2),
		  std::string("\x10\x10\x10\xff\xfe\xfe\xfe\xff", 8) },
		{ "f=516,s=3,v=1", std::string("\x00\xf8\xe0\x07\x1f\x00", 6),
		  std::string("\xff\0\0\xff\0\xff\0\xff\0\0\xff\xff", 12) },
	};
	unsigned id = 20;
	for (auto &c : cases) {
		struct dv_kplace	*p = nullptr;
		unsigned char		*rgba = nullptr;
		unsigned		 iw, ih;

		said(&w, rd);
		put(&w, strf_test("\033_Ga=T,%s,i=%u,c=1,r=1;", c.keys, id) + b64(c.data) +
		    "\033\\");
		if (said(&w, rd).find(strf_test("i=%u;OK", id)) == std::string::npos)
			fail(c.keys);
		for (auto &[pid, pl] : *w.screen.kpl)
			if (pl.child_id == id)
				p = &pl;
		if (p == nullptr ||
		    !dv_kimage_rgba(&w.kimg->at(p->img), &rgba, &iw, &ih) ||
		    std::string((char *)rgba, c.want.size()) != c.want)
			fail(c.keys);
		free(rgba);
		id++;
	}
	/* A short payload is refused rather than read past. */
	said(&w, rd);
	put(&w, "\033_Ga=T,f=516,s=4,v=4,i=40,c=1,r=1,q=1;" + b64("ab") + "\033\\");
	if (said(&w, rd).find("i=40;ENODATA") == std::string::npos ||
	    image_of(&w, 40) != nullptr)
		fail("a short packed payload is refused with ENODATA");

	/* The wire: packed to a terminal that listed it, RGBA to the rest. */
	{
		struct dv_view	 vp, vk;

		memset(&vp, 0, sizeof vp);
		memset(&vk, 0, sizeof vk);
		vp.packed = DV_FMT_G1;
		out = sent_to(&w, 20, &vp);
		if (out.find("f=301,s=9,v=2") == std::string::npos ||
		    payloads(out).substr(0, 4) != std::string("\x55\x80\xaa\x7f", 4))
			fail("a terminal that takes f=301 gets the packed bytes");
		std::string	keys, px;

		out = sent_to(&w, 20, &vk);
		px = sent_pixels(out, &keys);
		if (keys.find("f=24,s=9,v=2") == std::string::npos ||
		    px.substr(0, 6) != std::string("\0\0\0\xff\xff\xff", 6) ||
		    px.size() != 9 * 2 * 3)
			fail("another terminal gets the same picture as RGB");
	}

	/* Listed per terminal, and offered to programs whatever is attached. */
	peer_init(&a, &s);
	dv_strset(&a.silkie_blob, "ext=caps.1\nformat=301,302,304,308,516,24,32,100\nmedia=d,f,t,s\n");
	dv_silkie_recompute(&s, &a);
	if (a.view.packed != (DV_FMT_G1 | DV_FMT_G2 | DV_FMT_G4 | DV_FMT_G8 | DV_FMT_565))
		fail("a peer's packed formats are read from its list");
	dv_strset(&a.silkie_blob, "ext=caps.1\nformat=24,32,100,8,16,401,408,316,632,664\n");
	dv_silkie_recompute(&s, &a);
	if (a.view.packed != (DV_FMT_I8 | DV_FMT_I16 | DV_FMT_P1 | DV_FMT_P8 |
	    DV_FMT_G16 | DV_FMT_10A2 | DV_FMT_16A | DV_FMT_E))
		fail("indexed, palette and deep formats are read, and with them E");
	/* Silkie's older numberings: 8 and 16 were gray and RGB565, then 316
	 * was RGB565. Neither is taken for what the numbers are now. */
	dv_strset(&a.silkie_blob, "ext=caps.1\nformat=1,2,4,8,16,24,32,100\n");
	dv_silkie_recompute(&s, &a);
	if (a.view.packed != 0)
		fail("an old list's 8 and 16 are not indexed colour");
	dv_strset(&a.silkie_blob, "ext=caps.1\nformat=24,32,100,301,316\n");
	dv_silkie_recompute(&s, &a);
	if (a.view.packed != DV_FMT_G1)
		fail("an old list's 316 is not 16-bit gray");
	dv_strset(&a.silkie_blob, "ext=caps.1\nformat=24,32,100\nmedia=d,f,t,s\n");
	dv_silkie_recompute(&s, &a);
	if (a.view.packed != 0)
		fail("a peer that did not list them takes none");
	said(&w, rd);
	osc(&w, "caps;v=1:op=query:id=5");
	out = said(&w, rd);
	if (out.find("format=24,32,100,64,200,8,16,301,302,304,308,516,401,402,404,408,"
	    "316,632,664") == std::string::npos)
		fail("programs are offered packed formats, decoded here if need be");
	dv_silkie_free_peer(&a);
	dv_kitty_free(&w);
	dv_kitty_close(&w);
	dv_silkie_free_session(&s);
	win_free(&w, rd);
}

/*
 * Images re-packed for each client: reduced to what its display shows,
 * then the narrowest exact format it takes, deflated when smaller, and the
 * program's own form when that is no bigger and nothing was reduced.
 */
static void
test_repack(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_peer		 a;
	struct dv_view		 v;
	int			 rd = -1;
	unsigned		 id = 60;
	std::string		 keys, px;
	const int		 all = DV_FMT_G1 | DV_FMT_G2 | DV_FMT_G4 | DV_FMT_G8 |
				    DV_FMT_565;

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.gfx = 1;
	if (!pipe_to(&w, &rd))
		return;
	/* n pixels from a function of i, as f=24 or f=32 */
	auto image = [&](int f, unsigned n, auto pixel) {
		std::string	d;

		for (unsigned i = 0; i < n; i++) {
			unsigned	c = pixel(i);

			d += (char)(c >> 24);
			d += (char)(c >> 16);
			d += (char)(c >> 8);
			if (f == 32)
				d += (char)c;
		}
		put(&w, strf_test("\033_Ga=T,f=%d,s=%u,v=1,i=%u,c=1,r=1,q=2;", f, n, id) +
		    b64(d) + "\033\\");
		return (id++);
	};
	auto rgba = [](unsigned r, unsigned g, unsigned b, unsigned a = 255) {
		return ((r << 24) | (g << 16) | (b << 8) | a);
	};
	auto check = [&](unsigned img, const char *want, const std::string &pix,
	    const char *what) {
		px = sent_pixels(sent_to(&w, img, &v), &keys);
		if (keys.find(want) == std::string::npos ||
		    (!pix.empty() && px != pix)) {
			fprintf(stderr, "  sent %s\n", keys.c_str());
			fail(what);
		}
	};

	/* Eight bits a channel, every packed format taken: lossless. */
	memset(&v, 0, sizeof v);
	v.gfx = 1;
	v.packed = all;
	check(image(32, 8, [&](unsigned i) { return rgba(i & 1 ? 255 : 0,
	    i & 1 ? 255 : 0, i & 1 ? 255 : 0); }), "f=301,s=8,v=1",
	    std::string("\x55", 1), "black and white RGBA is one bit");
	check(image(24, 4, [&](unsigned i) { return rgba(i * 85, i * 85,
	    i * 85); }), "f=302,s=4,v=1", std::string("\x1b", 1),
	    "four exact grays are two bits");
	check(image(24, 2, [&](unsigned i) { return rgba(i * 17, i * 17,
	    i * 17); }), "f=304,s=2,v=1", std::string("\x01", 1),
	    "sixteen-level grays are four bits");
	check(image(24, 2, [&](unsigned i) { return rgba(3 + i, 3 + i, 3 + i); }),
	    "f=308,s=2,v=1", std::string("\x03\x04", 2), "other grays are eight bits");
	check(image(24, 3, [&](unsigned i) {
	    return i == 0 ? rgba(255, 0, 0) : i == 1 ? rgba(0, 255, 0) :
	    rgba(0, 0, 255); }), "f=516,s=3,v=1",
	    std::string("\x00\xf8\xe0\x07\x1f\x00", 6), "exact RGB565 colors are 16 bits");
	check(image(32, 2, [&](unsigned i) { return rgba(1 + i, 2, 3); }),
	    "f=24,s=2,v=1", std::string("\x01\x02\x03\x02\x02\x03", 6),
	    "an opaque image loses its alpha, not its colors");
	check(image(32, 2, [&](unsigned i) { return rgba(0, 0, 0, i ? 255 : 7); }),
	    "f=32,s=2,v=1", std::string("\0\0\0\x07\0\0\0\xff", 8),
	    "transparency keeps RGBA");
	check(image(24, 64, [&](unsigned) { return rgba(9, 200, 77); }),
	    "o=z", "", "a picture zlib shrinks goes deflated");
	{
		unsigned	img = image(24, 1, [&](unsigned) { return rgba(1, 2, 3); });

		px = sent_pixels(sent_to(&w, img, &v), &keys);
		if (keys.find("f=24,s=1,v=1,i=") == std::string::npos &&
		    keys.find("f=24,s=1,v=1,q=2") == std::string::npos)
			fail("the program's own form, when no bigger, as it came");
	}
	/* A client that takes no packed formats: RGB at most. */
	v.packed = 0;
	check(image(24, 8, [&](unsigned i) { return rgba(i & 1 ? 255 : 0,
	    i & 1 ? 255 : 0, i & 1 ? 255 : 0); }), "f=24,s=8,v=1", "",
	    "packed formats only to a client that listed them");

	/* Sixteen grays: what it cannot show is not sent. */
	v.packed = all;
	v.disp_color = 1;
	v.disp_bits[3] = 4;
	check(image(24, 2, [&](unsigned i) { return i ? rgba(255, 0, 0) :
	    rgba(250, 250, 250); }), "f=304,s=2,v=1", std::string("\xf3", 1),
	    "a 4-bit gray display is sent 4-bit gray");
	v.packed = 0;
	check(image(24, 1, [&](unsigned) { return rgba(255, 0, 0); }),
	    "f=24,s=1,v=1", std::string("\x33\x33\x33", 3),
	    "gray levels as RGB to a client without packed formats");
	/* Black and white: one bit, as is -m. */
	v.packed = all;
	v.disp_color = 2;
	v.disp_bits[3] = 1;
	check(image(24, 2, [&](unsigned i) { return i ? rgba(200, 200, 200) :
	    rgba(60, 60, 60); }), "f=301,s=2,v=1", std::string("\x40", 1),
	    "a mono display is sent one bit");
	v.disp_color = 0;
	v.disp_bits[3] = 0;
	v.mono = 1;
	check(image(24, 2, [&](unsigned i) { return i ? rgba(200, 200, 200) :
	    rgba(60, 60, 60); }), "f=301,s=2,v=1", std::string("\x40", 1),
	    "-m is sent one bit");
	v.mono = 0;
	/* RGB565: every color in 16 bits. */
	v.disp_bits[0] = 5;
	v.disp_bits[1] = 6;
	v.disp_bits[2] = 5;
	check(image(24, 1, [&](unsigned) { return rgba(200, 100, 50); }),
	    "f=516,s=1,v=1", std::string("\x26\xc3", 2), "an RGB565 display is sent 16 bits");
	check(image(24, 1, [&](unsigned) { return rgba(1, 2, 3); }),
	    "f=301,s=1,v=1", std::string("\x00", 1),
	    "what quantizes to black on it is sent as one bit");
	memset(v.disp_bits, 0, sizeof v.disp_bits);

	/* A retransmission is packed again; the same one is not rebuilt. */
	{
		unsigned	img = image(24, 2, [&](unsigned) { return rgba(0, 0, 0); });

		check(img, "f=301,s=2,v=1", std::string("\x00", 1), "first form");
		id = img;
		image(24, 2, [&](unsigned) { return rgba(255, 255, 255); });
		check(img, "f=301,s=2,v=1", std::string("\xc0", 1),
		    "new pixels under the same id are packed again");
	}

	/* The profile comes from the client's display reply. */
	peer_init(&a, &s);
	dv_strset(&a.silkie_blob, "ext=caps.1,display.1\nformat=301,302,304,308,516,24,32,100\n"
	    "display=kind=eink:color=gray:bits=4:gamut=srgb\n");
	a.view.kimg_gen = new std::unordered_map<unsigned, unsigned>;
	(*a.view.kimg_gen)[1] = 1;
	dv_silkie_recompute(&s, &a);
	if (a.view.disp_color != 1 || a.view.disp_bits[3] != 4 ||
	    !a.view.kimg_gen->empty() || !a.view.full)
		fail("a gray display is read, and its images go again");
	a.view.full = 0;
	(*a.view.kimg_gen)[1] = 1;
	dv_silkie_recompute(&s, &a);
	if (a.view.kimg_gen->empty() || a.view.full)
		fail("the same display sends nothing again");
	dv_strset(&a.silkie_blob, "ext=caps.1,display.1\nformat=24,32,100\n"
	    "display=kind=lcd:color=rgb:bits=0:red_bits=5:green_bits=6:blue_bits=5:gray_bits=0:precision=framebuffer\n");
	dv_silkie_recompute(&s, &a);
	if (a.view.disp_color != 0 || a.view.disp_bits[0] != 5 ||
	    a.view.disp_bits[1] != 6 || a.view.disp_bits[2] != 5)
		fail("per-channel widths are read");
	/* The widths reach programs, which used to lose them. */
	silk_ready(&s, "display.1,caps.1");
	said(&w, rd);
	osc(&w, "display;v=1:op=query:id=8");
	if (said(&w, rd).find("red_bits=5:green_bits=6:blue_bits=5") == std::string::npos)
		fail("a display query relays the channel widths");
	/* A resize asks the terminal again. */
	peer_frames.clear();
	dv_silkie_requery_display(&a);
	if (peer_frames[&a].find("777;display;v=1:op=query:id=17349524") ==
	    std::string::npos)
		fail("a resize queries the client's display");
	dv_strset(&a.silkie_blob, "ext=caps.1\n");
	peer_frames.clear();
	dv_silkie_requery_display(&a);
	if (!peer_frames[&a].empty())
		fail("a terminal without the display protocol is not asked");
	/* A display event from the terminal is its new profile. */
	dv_strset(&a.silkie_blob, "ext=caps.1,display.1\n");
	a.session = &s;
	{
		std::string	ev = "777;display;v=1:op=event:t=1:kind=lcd:color=mono:bits=1:gamut=srgb:headroom=1:scale=1:period=0:ppi=0";

		dv_silkie_from_host(&a, ev.data(), ev.size());
		if (a.view.disp_color != 2)
			fail("a display event changes what the client is sent");
	}
	delete a.view.kimg_gen;
	a.view.kimg_gen = nullptr;
	dv_silkie_free_peer(&a);
	dv_kitty_free(&w);
	dv_kitty_close(&w);
	dv_silkie_free_session(&s);
	win_free(&w, rd);
}

/* The decoded RGBA of image id, "" when it does not decode. */
static std::string
rgba_of(struct dv_window *w, unsigned id)
{
	struct dv_kimage	*im = image_of(w, id);
	unsigned char		*px = nullptr;
	unsigned		 iw, ih;
	std::string		 out;

	if (im == nullptr || !dv_kimage_rgba(im, &px, &iw, &ih))
		return ("");
	out.assign((const char *)px, (size_t)iw * ih * 4);
	free(px);
	return (out);
}

static float
half_test(unsigned h)
{
	unsigned	e = (h >> 10) & 31, m = h & 1023;
	float		v = e == 0 ? ldexpf((float)m, -24) :
			    ldexpf((float)(m | 1024), (int)e - 25);

	return ((h & 0x8000) ? -v : v);
}

/*
 * Kitty's indexed colour, f=8 and f=16: indices into the window's colours,
 * expanded in them, passed as they are to a terminal that lists them while
 * the window has no colours of its own, and sent again when those change.
 */
static void
test_indexed(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_view		 v;
	struct dv_buf		 b;
	int			 rd = -1;
	std::string		 out, keys;
	unsigned		 gen;

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.gfx = 1;
	if (!pipe_to(&w, &rd))
		return;
	put(&w, "\033_Ga=T,f=8,s=3,v=1,i=50,c=1,r=1;" +
	    b64(std::string("\x01\xf9\xf8", 3)) + "\033\\");
	if (said(&w, rd).find("i=50;OK") == std::string::npos)
		fail("f=8 is taken");
	if (rgba_of(&w, 50) != std::string("\xcd\0\0\xff\xe5\xe5\xe5\xff\0\0\0\xff", 12))
		fail("f=8 is the SGR colours, the foreground and the background");
	put(&w, "\033_Ga=T,f=16,s=2,v=1,i=51,c=1,r=1;" +
	    b64(std::string("\x10\x80\xfc\xff", 4)) + "\033\\");
	if (rgba_of(&w, 51) != std::string("\0\0\0\x80\xe5\xe5\xe5\xff", 8))
		fail("f=16 has an alpha byte; 252 is the cursor");
	/* Wrong sizes, a curve and frames are refused. */
	said(&w, rd);
	put(&w, "\033_Ga=T,f=8,s=3,v=1,i=52;" + b64("ab") + "\033\\");
	if (said(&w, rd).find("i=52;ENODATA") == std::string::npos)
		fail("short f=8 is ENODATA");
	put(&w, "\033_Ga=T,f=16,s=1,v=1,i=52;" + b64("abcdef") + "\033\\");
	if (said(&w, rd).find("i=52;EFBIG") == std::string::npos)
		fail("long direct f=16 is EFBIG");
	put(&w, "\033_Ga=T,f=8,s=1,v=1,E=16,i=52;" + b64("a") + "\033\\");
	if (said(&w, rd).find("i=52;EINVAL") == std::string::npos)
		fail("E is refused for indexed colour");
	put(&w, "\033_Ga=f,i=50,f=32,s=1,v=1;" + b64("abcd") + "\033\\");
	if (said(&w, rd).find("i=50;EINVAL") == std::string::npos)
		fail("no frames for indexed colour");

	/* As it is to a terminal that lists it; expanded for another. */
	memset(&v, 0, sizeof v);
	v.gfx = 1;
	v.packed = DV_FMT_I8;
	out = sent_to(&w, 50, &v);
	if (out.find("f=8,s=3,v=1") == std::string::npos ||
	    payloads(out).substr(0, 3) != std::string("\x01\xf9\xf8", 3))
		fail("a terminal that lists f=8 gets the indices, in its own colours");
	v.packed = 0;
	if (sent_pixels(sent_to(&w, 50, &v), &keys) !=
	    std::string("\xcd\0\0\xe5\xe5\xe5\0\0\0", 9) ||
	    keys.find("f=24") == std::string::npos)
		fail("another gets the colours");

	/* The window's colours: expanded in them, and sent again on a change. */
	memset(&b, 0, sizeof b);
	v.packed = DV_FMT_I8;
	dv_kitty_sync(&s, &v, &b);
	gen = image_of(&w, 50)->gen;
	put(&w, "\033]4;1;rgb:00/ff/00\033\\");
	dv_kitty_sync(&s, &v, &b);
	if (image_of(&w, 50)->gen == gen)
		fail("a change of the window's colours makes the picture again");
	if (rgba_of(&w, 50).substr(0, 4) != std::string("\0\xff\0\xff", 4))
		fail("an index the window recoloured is drawn in its colour");
	if (sent_pixels(sent_to(&w, 50, &v), &keys).substr(0, 3) !=
	    std::string("\0\xff\0", 3) || keys.find("f=24") == std::string::npos)
		fail("a window with colours of its own sends them, even where f=8 is taken");
	gen = image_of(&w, 50)->gen;
	dv_kitty_sync(&s, &v, &b);
	if (image_of(&w, 50)->gen != gen)
		fail("the same colours send nothing again");
	dv_buf_free(&b);
	dv_kitty_free(&w);
	dv_kitty_close(&w);
	dv_silkie_free_session(&s);
	win_free(&w, rd);
}

/*
 * Palette images, f=401-408: a palette of RGBA slots then packed indices,
 * the sizes exact, and a=e replacing slots without the pixels.
 */
static void
test_palette(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_view		 v;
	int			 rd = -1;
	std::string		 out, keys;
	unsigned		 gen, pgen = 0;

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.gfx = 1;
	if (!pipe_to(&w, &rd))
		return;
	/* The specification's example: three by two, two bits an index. */
	put(&w, "\033_Ga=T,i=7,f=402,s=3,v=2,c=1,r=1;AAAAAP8AAP8AAP+A/////2w0\033\\");
	if (said(&w, rd).find("i=7;OK") == std::string::npos)
		fail("the f=402 example is taken");
	if (rgba_of(&w, 7) != std::string("\xff\0\0\xff\0\0\xff\x80\xff\xff\xff\xff"
	    "\0\0\0\0\xff\xff\xff\xff\xff\0\0\xff", 24))
		fail("the f=402 example decodes as its table says");
	for (auto &[pid, pl] : *w.screen.kpl)
		if (pl.child_id == 7)
			pgen = pl.gen;
	gen = image_of(&w, 7)->gen;
	put(&w, "\033_Ga=e,i=7,x=1,w=1;AP8A/w==\033\\");
	if (said(&w, rd).find("i=7;OK") == std::string::npos ||
	    rgba_of(&w, 7).substr(0, 4) != std::string("\0\xff\0\xff", 4))
		fail("a palette update makes slot 1 opaque green");
	if (image_of(&w, 7)->gen == gen)
		fail("an update is a new picture");
	for (auto &[pid, pl] : *w.screen.kpl)
		if (pl.child_id == 7 && pl.gen == pgen)
			fail("its placements show the update");
	/* The whole palette, chunked and deflated. */
	{
		std::string	pal(16, '\0');
		uLongf		zn = compressBound(16);
		std::string	z((size_t)zn, '\0');

		for (int i = 0; i < 16; i++)
			pal[(size_t)i] = (char)(0x10 * i);
		compress2((Bytef *)z.data(), &zn, (const Bytef *)pal.data(), 16, 6);
		z.resize((size_t)zn);
		std::string	enc = b64(z);

		put(&w, "\033_Ga=e,i=7,o=z,m=1;" + enc.substr(0, 4) + "\033\\");
		put(&w, "\033_Gm=0;" + enc.substr(4) + "\033\\");
		if (said(&w, rd).find("i=7;OK") == std::string::npos ||
		    rgba_of(&w, 7).substr(0, 4) != pal.substr(4, 4))
			fail("a whole palette, chunked and deflated");
	}
	struct {
		const char	*keys;
		std::string	 data;
		const char	*want;
		const char	*what;
	} bad[] = {
		{ "a=e,i=7,x=3,w=2", std::string(8, 'a'), "i=7;EINVAL", "slots past the palette" },
		{ "a=e,i=7,x=4", std::string(4, 'a'), "i=7;EINVAL", "a first slot past it" },
		{ "a=e,i=7,G=1,x=0,w=1", std::string(4, 'a'), "i=7;EINVAL", "another gamut" },
		{ "a=e,i=7,x=1,w=2", std::string(4, 'a'), "i=7;ENODATA", "too few bytes" },
		{ "a=e,i=7,x=1,w=1", std::string(8, 'a'), "i=7;EFBIG", "too many, direct" },
		{ "a=e,i=999,x=1,w=1", std::string(4, 'a'), "i=999;ENOENT", "no such image" },
		{ "a=T,i=8,f=402,s=3,v=2", std::string(17, 'a'), "i=8;ENODATA", "a short image" },
		{ "a=T,i=8,f=402,s=3,v=2", std::string(19, 'a'), "i=8;EFBIG", "a long image" },
		{ "a=T,i=8,f=403,s=3,v=2", std::string(18, 'a'), "i=8;EINVAL", "f=403" },
		{ "a=T,i=8,f=402,v=2", std::string(18, 'a'), "i=8;EINVAL", "no width" },
		{ "a=T,i=8,f=402,s=3,v=2,o=z", std::string(18, 'a'), "i=8;EINVAL", "not deflated" },
		{ "a=T,i=8,f=402,s=3,v=2,E=8", std::string(18, 'a'), "i=8;EINVAL", "a curve" },
		{ "a=f,i=7,f=32,s=1,v=1", std::string(4, 'a'), "i=7;EINVAL", "a frame" },
		{ "a=c,i=7,r=1,c=2", "", "i=7;EINVAL", "composition" },
	};
	for (auto &c : bad) {
		said(&w, rd);
		put(&w, std::string("\033_G") + c.keys + ";" + b64(c.data) + "\033\\");
		if (said(&w, rd).find(c.want) == std::string::npos)
			fail(c.what);
	}
	put(&w, "\033_Ga=T,f=24,s=1,v=1,i=9;" + b64("abc") + "\033\\");
	said(&w, rd);
	put(&w, "\033_Ga=e,i=9;" + b64("abcd") + "\033\\");
	if (said(&w, rd).find("i=9;EINVAL") == std::string::npos)
		fail("a=e of an image that is neither a palette nor a scene");

	/* As it is to a terminal that lists it, its colours to another. */
	memset(&v, 0, sizeof v);
	v.gfx = 1;
	v.packed = DV_FMT_P2;
	out = sent_to(&w, 7, &v);
	if (out.find("f=402,s=3,v=2") == std::string::npos)
		fail("a terminal that lists f=402 gets the palette image");
	v.packed = 0;
	if (sent_pixels(sent_to(&w, 7, &v), &keys).size() != 24 ||
	    keys.find("f=32") == std::string::npos)
		fail("another gets RGBA");
	/* And a picture of few colours goes as a palette where that is smaller. */
	{
		std::string	px;

		for (int i = 0; i < 64; i++)
			px += i % 3 == 0 ? std::string("\x12\x34\x56\x80", 4) :
			    std::string("\x9a\xbc\xde\xff", 4);
		put(&w, "\033_Ga=T,f=32,s=64,v=1,i=10,c=1,r=1,q=2;" + b64(px) + "\033\\");
		v.packed = DV_FMT_P1 | DV_FMT_P8;
		out = sent_pixels(sent_to(&w, 10, &v), &keys);
		if (keys.find("f=401,s=64,v=1") == std::string::npos ||
		    out.size() != 8 + 8)
			fail("two colours with alpha are a one-bit palette image");
	}
	dv_kitty_free(&w);
	dv_kitty_close(&w);
	dv_silkie_free_session(&s);
	win_free(&w, rd);
}

/*
 * The high precision formats, f=316, 632 and 664, and the transfer key E:
 * decoded to eight bits for sixel and for terminals that take neither, to
 * half floats for one that takes HDR, and as they are to one that lists
 * them.
 */
static void
test_precision(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	struct dv_view		 v;
	int			 rd = -1;
	std::string		 out, keys, px;

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.gfx = 1;
	if (!pipe_to(&w, &rd))
		return;
	/* The specification's RGB10A2 examples, as they are on the wire. */
	put(&w, "\033_Ga=T,f=632,s=4,v=1,i=70,c=1,r=1;" +
	    b64(std::string("\xff\x03\x00\xc0\x00\x02\x08\xe0\xff\xff\xff\xff\0\0\0\0", 16)) +
	    "\033\\");
	if (said(&w, rd).find("i=70;OK") == std::string::npos)
		fail("f=632 is taken");
	if (rgba_of(&w, 70) != std::string("\xff\0\0\xff\x80\x80\x80\xff\xff\xff\xff\xff"
	    "\0\0\0\0", 16))
		fail("RGB10A2 decodes as the examples say");
	put(&w, "\033_Ga=T,f=664,s=1,v=1,i=71,c=1,r=1;" +
	    b64(std::string("\xff\xff\0\0\0\0\0\x80", 8)) + "\033\\");
	if (rgba_of(&w, 71) != std::string("\xff\0\0\x80", 4))
		fail("16-bit RGBA: red at half opacity");
	put(&w, "\033_Ga=T,f=316,s=1,v=1,i=72,c=1,r=1;" +
	    b64(std::string("\x00\x80", 2)) + "\033\\");
	if (rgba_of(&w, 72) != std::string("\x80\x80\x80\xff", 4))
		fail("16-bit gray 0x8000 is middle gray");

	/* E: which values, with which formats. */
	struct {
		const char	*keys;
		std::string	 data;
		const char	*want;
		const char	*what;
	} es[] = {
		{ "f=24,s=1,v=1,E=5", "abc", "EINVAL", "E=5" },
		{ "f=100,E=16", "abc", "EINVAL", "E with PNG" },
		{ "f=64,s=1,v=1,E=16", std::string(8, '\0'), "EINVAL", "f=64 is linear" },
		{ "f=64,s=1,v=1,E=8", std::string(8, '\0'), "OK", "f=64 with E=8" },
		{ "f=632,s=1,v=1,E=13", std::string(4, '\0'), "OK", "E=13 is sRGB" },
		{ "f=304,s=2,v=1,E=18", std::string(1, '\0'), "OK", "packed gray with HLG" },
		{ "f=664,s=1,v=1", std::string(4, '\0'), "ENODATA", "short f=664" },
		{ "f=316,s=1,v=1", std::string(3, '\0'), "EFBIG", "long f=316" },
	};
	for (auto &c : es) {
		said(&w, rd);
		put(&w, std::string("\033_Ga=T,i=73,c=1,r=1,") + c.keys + ";" + b64(c.data) +
		    "\033\\");
		if (said(&w, rd).find(std::string("i=73;") + c.want) == std::string::npos)
			fail(c.what);
	}
	put(&w, "\033_Ga=T,f=632,s=1,v=1,E=13,i=74,c=1,r=1,q=2;" +
	    b64(std::string(4, '\0')) + "\033\\");
	if (strstr(image_of(&w, 74)->fmt, "E=") != nullptr)
		fail("E=13 is not kept: it is no curve but the usual one");
	said(&w, rd);
	put(&w, "\033_Ga=f,i=70,f=32,s=1,v=1;" + b64("abcd") + "\033\\");
	if (said(&w, rd).find("i=70;EINVAL") == std::string::npos)
		fail("no frames for the high precision formats");

	/* HDR10: PQ in Rec. 2020. 594 of 1023 is 203 nits, SDR white. A
	 * gamut needs a terminal that takes one. */
	silk_ready(&s, "caps.1");
	{
		uint32_t	wd = 594u | (594u << 10) | (594u << 20) | (3u << 30);
		uint32_t	w2 = 669u | (669u << 10) | (669u << 20) | (3u << 30);
		std::string	d;

		for (uint32_t x : { wd, w2 })
			for (int k = 0; k < 4; k++)
				d += (char)((x >> (8 * k)) & 0xff);
		put(&w, "\033_Ga=T,f=632,E=16,G=2,s=2,v=1,i=75,c=1,r=1,q=2;" + b64(d) +
		    "\033\\");
	}
	memset(&v, 0, sizeof v);
	v.gfx = 1;
	v.hdr = 1;
	v.wg = 1;
	px = sent_pixels(sent_to(&w, 75, &v), &keys);
	if (keys.find("f=64,s=2,v=1") == std::string::npos || px.size() != 16)
		fail("an HDR terminal gets HDR10 as half floats");
	else {
		float	r0 = half_test((unsigned char)px[0] | ((unsigned)(unsigned char)px[1] << 8));
		float	r1 = half_test((unsigned char)px[8] | ((unsigned)(unsigned char)px[9] << 8));

		if (fabsf(r0 - 1) > 0.02f || fabsf(r1 - 2) > 0.04f) {
			fprintf(stderr, "  %g %g\n", r0, r1);
			fail("PQ is decoded with 203 nits at SDR white");
		}
	}
	v.packed = DV_FMT_10A2 | DV_FMT_E;
	out = sent_to(&w, 75, &v);
	if (out.find("f=632") == std::string::npos || out.find("E=16") == std::string::npos ||
	    out.find("G=2") == std::string::npos)
		fail("a terminal that lists f=632 gets HDR10 as it is");
	v.packed = DV_FMT_10A2;		/* the format, not the key */
	if (sent_to(&w, 75, &v).find("f=64") == std::string::npos)
		fail("without E it gets half floats");
	v.hdr = 0;
	v.packed = 0;
	px = sent_pixels(sent_to(&w, 75, &v), &keys);
	if (keys.find("f=24,s=2,v=1") == std::string::npos ||
	    px != std::string("\xff\xff\xff\xff\xff\xff", 6))
		fail("an SDR terminal gets SDR white and brighter clipped to it");
	/* HLG: a signal of 0.75 is SDR white, the largest 4.93 times it. */
	put(&w, "\033_Ga=T,f=664,E=18,s=2,v=1,i=76,c=1,r=1,q=2;" +
	    b64(std::string("\xff\xbf\xff\xbf\xff\xbf\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", 16)) +
	    "\033\\");
	v.hdr = 1;
	px = sent_pixels(sent_to(&w, 76, &v), &keys);
	if (px.size() != 16)
		fail("HLG goes as half floats");
	else {
		float	r0 = half_test((unsigned char)px[0] | ((unsigned)(unsigned char)px[1] << 8));
		float	r1 = half_test((unsigned char)px[8] | ((unsigned)(unsigned char)px[9] << 8));

		if (fabsf(r0 - 1) > 0.01f || fabsf(r1 - 4.93f) > 0.02f) {
			fprintf(stderr, "  %g %g\n", r0, r1);
			fail("HLG is shown on a 1000 nit display");
		}
	}
	/* A precision image with no curve to an SDR terminal is eight bits. */
	v.hdr = 0;
	if (sent_pixels(sent_to(&w, 70, &v), &keys).size() != 16 ||
	    keys.find("f=32") == std::string::npos)
		fail("f=632 is RGBA to a terminal that takes neither");
	dv_kitty_free(&w);
	dv_kitty_close(&w);
	dv_silkie_free_session(&s);
	win_free(&w, rd);
}

/*
 * Feature queries (a=q with e=): the keys of the query odin-seat takes for
 * that action, with their values; an action it does not take, without e;
 * a number too large for its key, nothing at all.
 */
static void
test_feature_query(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	int			 rd = -1;
	std::string		 out;

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.gfx = 1;
	if (!pipe_to(&w, &rd))
		return;
	struct {
		const char	*q;
		const char	*want;		/* "" = no reply */
		const char	*what;
	} cases[] = {
		{ "a=q,i=31,e=T,U=1,C=1", "\033_Gi=31,e=T,U=1,C=1;OK\033\\", "placeholders and C" },
		{ "a=q,i=31,e=f,r=1,X=1", "\033_Gi=31;EINVAL", "no animation: no e" },
		{ "a=q,i=31,e=t,t=f,O=4294967296", "\033_Gi=31,e=t,t=f,O=4294967296;OK",
		  "64-bit offsets" },
		{ "a=q,i=31,e=t,f=24,s=20000,v=100", "\033_Gi=31,e=t,f=24,s=20000,v=100;OK",
		  "any size" },
		{ "a=q,i=31,e=t,f=403", "\033_Gi=31,e=t;OK", "f=403 is no format" },
		{ "a=q,i=31,e=t,f=301", "\033_Gi=31,e=t,f=301;OK", "packed gray" },
		{ "a=q,i=31,e=t,f=8", "\033_Gi=31,e=t,f=8;OK", "indexed colour" },
		{ "a=q,i=31,e=t,f=1", "\033_Gi=31,e=t;OK", "the old f=1" },
		{ "a=q,i=31,e=e,x=1,w=2", "\033_Gi=31,e=e,x=1,w=2;OK", "palette updates" },
		{ "a=q,i=31,e=t,f=632,E=16,G=2", "\033_Gi=31,e=t,f=632,E=16;OK",
		  "no gamut without a terminal for it" },
		{ "a=q,i=31,e=T,f=64,T=1", "\033_Gi=31,e=T,f=64,T=1;OK", "tone mapping" },
		{ "a=q,i=31,e=t,f=100,E=16", "\033_Gi=31,e=t,f=100;OK", "no E for PNG" },
		{ "a=q,i=31,e=t,t=g", "\033_Gi=31,e=t;OK", "no GPU surface medium" },
		{ "a=q,i=31,e=d,d=I,p=3", "\033_Gi=31,e=d,d=I,p=3;OK", "delete" },
		{ "a=q,i=31,e=t,f=32,s=0", "\033_Gi=31,e=t;OK", "defaults are not named" },
		{ "a=q,i=31,e=t,O=18446744073709551616", "", "a malformed offset" },
		{ "a=q,i=31,e=t,s=4294967296", "", "a malformed width" },
		{ "a=q,e=t,f=24", "", "no i" },
	};
	for (auto &c : cases) {
		said(&w, rd);
		put(&w, std::string("\033_G") + c.q + ";AAAA\033\\");
		out = said(&w, rd);
		if (c.want[0] == '\0' ? !out.empty() : out.find(c.want) != 0) {
			fprintf(stderr, "  got %s\n", out.c_str() + (out.empty() ? 0 : 1));
			fail(c.what);
		}
	}
	silk_ready(&s, "caps.1");
	said(&w, rd);
	put(&w, "\033_Ga=q,i=31,e=t,f=632,E=16,G=2\033\\");
	if (said(&w, rd).find("\033_Gi=31,e=t,f=632,E=16,G=2;OK") != 0)
		fail("HDR10 where a terminal takes the gamut");
	/* A feature query in the middle of a chunked transmission leaves it be. */
	said(&w, rd);
	put(&w, "\033_Ga=T,f=24,s=2,v=1,i=33,c=1,r=1,m=1;" + b64("abc") + "\033\\");
	put(&w, "\033_Ga=q,i=32,e=T,U=1\033\\");
	put(&w, "\033_Gm=0;" + b64("def") + "\033\\");
	out = said(&w, rd);
	if (out.find("i=32,e=T,U=1;OK") == std::string::npos ||
	    out.find("i=33;OK") == std::string::npos || rgba_of(&w, 33).size() != 8)
		fail("a feature query does nothing to a transmission under way");
	dv_kitty_free(&w);
	dv_kitty_close(&w);
	dv_silkie_free_session(&s);
	win_free(&w, rd);
}

/* S and O are 64-bit: data at any offset of a file, the end checked. */
static void
test_medium64(void)
{
	struct dv_session	 s;
	struct dv_window	 w;
	int			 rd = -1, fd;
	char			 path[] = "/tmp/odin-seat-test-medium-XXXXXX";
	std::string		 junk(5000, 'j');

	setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
	session_init(&s);
	win_init(&w, &s, 1, 2, 1, 1);
	TAILQ_INSERT_TAIL(&s.desktop.windows, &w, entry);
	s.desktop.gfx = 1;
	if (!pipe_to(&w, &rd))
		return;
	fd = mkstemp(path);
	if (fd < 0 || write(fd, junk.data(), junk.size()) != (ssize_t)junk.size() ||
	    write(fd, "\x11\x22\x33", 3) != 3) {
		fail("temporary file");
		return;
	}
	close(fd);
	put(&w, "\033_Ga=T,f=24,s=1,v=1,t=f,O=5000,S=3,i=80,c=1,r=1;" + b64(path) +
	    "\033\\");
	if (said(&w, rd).find("i=80;OK") == std::string::npos ||
	    rgba_of(&w, 80) != std::string("\x11\x22\x33\xff", 4))
		fail("data at an offset that is no page");
	put(&w, "\033_Ga=T,f=24,s=1,v=1,t=f,O=9223372036854775807,S=2,i=81;" + b64(path) +
	    "\033\\");
	if (said(&w, rd).find("i=81;EOVERFLOW") == std::string::npos)
		fail("an end past a file offset is EOVERFLOW");
	put(&w, "\033_Ga=T,f=24,s=1,v=1,t=f,S=18446744073709551616,i=82;" + b64(path) +
	    "\033\\");
	if (!said(&w, rd).empty() || image_of(&w, 82) != nullptr)
		fail("a size past 64 bits is malformed: nothing is answered");
	unlink(path);
	dv_kitty_free(&w);
	dv_kitty_close(&w);
	dv_silkie_free_session(&s);
	win_free(&w, rd);
}

int
main(void)
{
	test_translate();
	test_capture();
	test_accidental();
	test_touchpad();
	test_scroll();
	test_reset();
	test_query_silent();
	test_recompute();
	test_display_best();
	test_frame_length();
	test_contacts_grow();
	test_font();
	test_ink();
	test_label();
	test_gamut_ignored();
	test_headroom();
	test_scene();
	test_ink_mirror();
	test_ink_crop();
	test_ink_leases();
	test_service();
	test_pointer_image();
	test_depth();
	test_packed();
	test_repack();
	test_indexed();
	test_palette();
	test_precision();
	test_feature_query();
	test_medium64();
	if (failed) {
		fprintf(stderr, "test-silkie: FAILED\n");
		return (1);
	}
	printf("test-silkie: ok\n");
	return (0);
}
