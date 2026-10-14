/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Silkie multiplexer behaviour that does not need the server: coordinate
 * translation, contact capture, capability intersection.
 */

#include "dv.hpp"
#include "silkie.hpp"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failed;

static char	hostbuf[8192];
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
	dv_strlcpy(s.silk_ext, "tablet-pointer.1,caps.1", sizeof s.silk_ext);
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
	dv_strlcpy(silk.silkie_blob, blob, sizeof silk.silkie_blob);
	ghost.features = DV_IDENT_KITTY_GFX;
	TAILQ_INIT(&peers);
	TAILQ_INSERT_TAIL(&peers, &silk, entry);
	TAILQ_INSERT_TAIL(&peers, &ghost, entry);
	dv_silkie_recompute(&s, TAILQ_FIRST(&peers));
	if (strcmp(s.silk_ext, "tablet-pointer.1,display.1,caps.1") != 0)
		fail("ext is the selectors any peer listed, plus caps");
	if (strcmp(s.silk_format, "24,32,100") != 0 ||
	    strcmp(s.silk_media, "d,f,t,s") != 0 || s.silk_tablet_caps != 1)
		fail("graphics is the intersection; a bare kitty peer drops the rest");
	if (!pipe_to(&w, &rd)) {
		fail("pipe");
		win_free(&w, rd);
		return;
	}
	dv_silkie_program(&w, q, strlen(q));
	drain(&w, rd, buf, sizeof buf, &n);
	if (!has(buf, "op=cap") || !has(buf, "ext=tablet-pointer.1,display.1,caps.1") ||
	    !has(buf, "format=24,32,100") || !has(buf, "media=d,f,t,s") ||
	    has(buf, "64") || has(buf, "200") || has(buf, "g"))
		fail("a caps query answers with the intersection");
	if (dv_silkie_gfx_ok(&s, 64, 0, 'd') || dv_silkie_gfx_ok(&s, 200, 0, 'd') ||
	    dv_silkie_gfx_ok(&s, 32, 0, 'g') || dv_silkie_gfx_ok(&s, 32, 1, 'd'))
		fail("a format, medium or gamut the intersection dropped is refused");
	if (!dv_silkie_gfx_ok(&s, 24, 0, 'd') || !dv_silkie_gfx_ok(&s, 32, 0, 'f') ||
	    !dv_silkie_gfx_ok(&s, 100, 0, 't') || !dv_silkie_gfx_ok(&s, 32, 0, 's'))
		fail("shared kitty formats and media are accepted");
	ghost.display = 0;
	dv_silkie_recompute(&s, TAILQ_FIRST(&peers));
	if (!dv_silkie_gfx_ok(&s, 32, 1, 'd'))
		fail("wide gamut is accepted when every graphics peer listed formats");
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
	if (failed) {
		fprintf(stderr, "test-silkie: FAILED\n");
		return (1);
	}
	printf("test-silkie: ok\n");
	return (0);
}
