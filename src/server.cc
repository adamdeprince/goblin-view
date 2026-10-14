/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 */

#include "dv.hpp"
#include "pointer_coordinates.hpp"
#include "silkie.hpp"

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct dv_server	*dv_srv;
static int		 dv_quit;

static void	compose_send(struct dv_session *);
static void	uni_insert(struct dv_session *, uint32_t);
static void	win_local(struct dv_window *, int, int, int *, int *);
static void	copy_selection(struct dv_session *);
static const char *key_arrow(const unsigned char *, size_t, int *);
static void	flush_paste(struct dv_session *, struct dv_window *);

/* Something on the desktop changed: repaint clients at the end of the loop. */
static void
redraw(struct dv_session *s)
{
	s->dirty = 1;
}

static long long
now_ms(void)
{
	struct timespec	ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void
on_sigchld(int sig)
{
	(void)sig;
}

static void
on_sigterm(int sig)
{
	(void)sig;
	dv_quit = 1;
}

static unsigned
get_u16(const unsigned char **p, const unsigned char *end, int *err)
{
	unsigned	v;

	if (*p + 2 > end) {
		*err = 1;
		return (0);
	}
	v = ((unsigned)(*p)[0] << 8) | (unsigned)(*p)[1];
	*p += 2;
	return (v);
}

static void	refresh_gfx(struct dv_session *);
static void pointer_cancel_peer(struct dv_peer *);
static void handle_pointer(struct dv_peer *, const goblin::pointer::Message &);

static void
peer_close(struct dv_peer *p)
{
	pointer_cancel_peer(p);
	if (p->session != NULL && p->display) {
		p->display = 0;
		refresh_gfx(p->session);
	}
	TAILQ_REMOVE(&dv_srv->peers, p, entry);
	dv_view_free(&p->view);
	dv_conn_free(&p->conn);
	if (p->fd >= 0)
		close(p->fd);
	free(p);
}

static void
recalculate_size(struct dv_session *s)
{
	struct dv_peer	*p;
	struct dv_size	 sz;
	int		 any = 0;

	memset(&sz, 0, sizeof sz);
	s->pointer_caps = 0;
	TAILQ_FOREACH(p, &dv_srv->peers, entry) {
		if (!p->display || p->session != s)
			continue;
		s->pointer_caps |= p->pointer_caps;
		if (!any) {
			sz = p->tty;
			any = 1;
			continue;
		}
		/* Size to the LARGEST client; smaller ones scroll a viewport. */
		if (p->tty.cols > sz.cols)
			sz.cols = p->tty.cols;
		if (p->tty.rows > sz.rows)
			sz.rows = p->tty.rows;
		if (p->tty.xpixel != 0)
			sz.xpixel = p->tty.xpixel;
		if (p->tty.ypixel != 0)
			sz.ypixel = p->tty.ypixel;
	}
	if (!any)
		return;
	if (sz.cols < 2)
		sz.cols = 80;
	if (sz.rows < 2)
		sz.rows = 24;
	if (sz.xpixel == 0)
		sz.xpixel = DV_DEFAULT_XPIXEL;
	if (sz.ypixel == 0)
		sz.ypixel = DV_DEFAULT_YPIXEL;
	dv_desktop_resize(&s->desktop, sz);
	{
		struct dv_window	*w;

		TAILQ_FOREACH(w, &s->desktop.windows, entry)
			dv_window_set_pty_size(w);
	}
	redraw(s);
}

/* Raw bytes a window wants written to the host tty (OSC-52 clipboard). */
static void
flush_host_out(struct dv_window *w)
{
	struct dv_peer	*p;

	int		 asked = 0;

	if (w->host_out.len == 0)
		return;
	TAILQ_FOREACH(p, &dv_srv->peers, entry) {
		if (!p->display || p->session != w->session)
			continue;
		dv_conn_send(&p->conn, DV_MSG_PASSTHROUGH, w->host_out.data,
		    w->host_out.len);
		dv_conn_flush(&p->conn);
		/*
		 * A clipboard READ is answered by the host; ask one client
		 * only, or every attached terminal would reply, and remember
		 * which window is waiting so the answer goes back to it.
		 */
		if (w->clip_query) {
			w->session->clip_query_win = w->id;
			asked = 1;
			break;
		}
	}
	/*
	 * A READ with nobody attached has no terminal to answer it. Reply
	 * with an empty clipboard for the same selection rather than leave
	 * the program blocked on a reply that will never come.
	 */
	if (w->clip_query && !asked && w->pty_fd >= 0) {
		const unsigned char	*d = w->host_out.data;
		size_t			 n = w->host_out.len, i, j;
		char			 rep[80];
		int			 rn;

		/* d is ESC ] 5 2 ; <sel> ; ? ... : pull out <sel>. */
		for (i = 0; i < n && d[i] != ';'; i++)
			;
		for (j = i + 1; j < n && d[j] != ';' && j - i < 16; j++)
			;
		rn = snprintf(rep, sizeof rep, "\033]52;%.*s;\033\\",
		    (int)(j > i + 1 ? j - i - 1 : 0),
		    (const char *)d + i + 1);
		if (rn > 0)
			dv_window_pty_send(w, rep, (size_t)rn);
	}
	w->clip_query = 0;
	w->host_out.len = 0;
}

/*
 * A host OSC reply relayed by a client: deliver it to the window that
 * asked. OSC 52 uses the legacy single slot; everything else -- kitty
 * clipboard (5522), notifications (99), file transfer (5113 and 777),
 * colour and pointer queries -- is routed by OSC number and request id.
 * One-shot replies free their route; a clipboard read frees it on its
 * final status; notification and transfer routes stay until evicted.
 */
static struct dv_window *window_at(struct dv_session *s, int x, int y);
static void win_local(struct dv_window *w, int x, int y, int *sx, int *sy);
static void flush_paste(struct dv_session *s, struct dv_window *w);

/*
 * Drag and drop (kitty OSC 72). The host reports the drag by cell; the
 * window under it is found and the event translated into that window's
 * own cells and handed to its program if it said it takes drops (t=a).
 * Otherwise goblin-view takes the drop on the window's behalf: it asks
 * the host for the text (file paths, or plain text), and pastes it into
 * that window -- the one the drop landed on, at its location, not merely
 * the focused one.
 */
static void
bput_str(struct dv_buf *b, const char *s)
{
	dv_buf_append(b, s, strlen(s));
}

static void
dnd_to_host(struct dv_session *s, const char *text)
{
	struct dv_peer	*q;

	TAILQ_FOREACH(q, &dv_srv->peers, entry) {
		if (!q->display || q->session != s || q->fd != s->dnd_peer_fd)
			continue;
		dv_conn_send(&q->conn, DV_MSG_PASSTHROUGH, text, strlen(text));
		dv_conn_flush(&q->conn);
	}
}

static void
dnd_to_app(struct dv_window *w, const char *text)
{
	dv_window_pty_send(w, text, strlen(text));
}

/* Tell every host what the windows take: the union of their lists. */
static void
dnd_advertise(struct dv_session *s)
{
	struct dv_window	*w;
	struct dv_peer		*q;
	struct dv_buf		 b;

	memset(&b, 0, sizeof b);
	bput_str(&b, "\033]72;t=a;text/uri-list text/plain");
	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		if (!w->dnd_accept || w->dnd_types[0] == '\0')
			continue;
		dv_buf_append(&b, " ", 1);
		dv_buf_append(&b, w->dnd_types, strlen(w->dnd_types));
	}
	dv_buf_append(&b, "\033\\", 2);
	TAILQ_FOREACH(q, &dv_srv->peers, entry) {
		if (!q->display || q->session != s)
			continue;
		dv_conn_send(&q->conn, DV_MSG_PASSTHROUGH, b.data, b.len);
		dv_conn_flush(&q->conn);
	}
	dv_buf_free(&b);
}

/* The 1-based index of the MIME type we would take, 0 for none. */
static int
dnd_pick(const char *mimes)
{
	const char	*p = mimes;
	int		 i = 0, plain = 0;

	while (*p != '\0') {
		const char	*e = strchr(p, ' ');
		size_t		 l = e ? (size_t)(e - p) : strlen(p);

		i++;
		if (l == 13 && memcmp(p, "text/uri-list", 13) == 0)
			return (i);
		if (plain == 0 && l >= 10 && memcmp(p, "text/plain", 10) == 0)
			plain = i;
		p += l;
		while (*p == ' ')
			p++;
	}
	return (plain);
}

/* A text/uri-list becomes paths: file:// URIs decoded, quoted for a
 * shell when they need it, one per line becoming space-separated. */
static void
dnd_uris_to_paths(const unsigned char *d, size_t n, struct dv_buf *out)
{
	size_t	i = 0;
	int	first = 1;

	while (i < n) {
		size_t	st = i, e, k;
		char	path[DV_PATH_MAX];
		size_t	pl = 0;
		int	need_q = 0;

		while (i < n && d[i] != '\n' && d[i] != '\r')
			i++;
		e = i;
		while (i < n && (d[i] == '\n' || d[i] == '\r'))
			i++;
		if (e == st || d[st] == '#')
			continue;
		k = st;
		if (e - st > 7 && memcmp(d + st, "file://", 7) == 0) {
			k = st + 7;
			while (k < e && d[k] != '/')	/* a host name: skipped */
				k++;
		}
		for (; k < e && pl + 1 < sizeof path; k++) {
			unsigned char	c = d[k];

			if (c == '%' && k + 2 < e) {
				char	h[3] = { (char)d[k + 1], (char)d[k + 2], 0 };

				c = (unsigned char)strtoul(h, NULL, 16);
				k += 2;
			}
			path[pl++] = (char)c;
		}
		path[pl] = '\0';
		for (k = 0; k < pl; k++) {
			unsigned char	c = (unsigned char)path[k];

			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			    (c >= '0' && c <= '9') || c == '/' || c == '.' ||
			    c == '_' || c == '-' || c == '+' || c == ':' || c == '@' ||
			    c == ',' || c >= 0x80))
				need_q = 1;
		}
		if (!first)
			dv_buf_append(out, " ", 1);
		first = 0;
		if (!need_q) {
			dv_buf_append(out, path, pl);
			continue;
		}
		dv_buf_append(out, "'", 1);
		for (k = 0; k < pl; k++) {
			if (path[k] == '\'')
				dv_buf_append(out, "'\\''", 4);
			else
				dv_buf_append(out, path + k, 1);
		}
		dv_buf_append(out, "'", 1);
	}
}

/* The dropped data is all here: paste it into the window it fell on. */
static void
dnd_deliver(struct dv_session *s)
{
	struct dv_window	*w = dv_window_find(s, s->dnd_target);
	struct dv_buf		 text;

	if (w == nullptr)
		w = s->desktop.focused;
	memset(&text, 0, sizeof text);
	if (s->dnd_idx > 0 && s->dnd_idx == dnd_pick(s->dnd_mimes) &&
	    strstr(s->dnd_mimes, "text/uri-list") != NULL) {
		/* was it the uri-list we picked? the pick prefers it */
		const char	*p = s->dnd_mimes;
		int		 i = 0, is_uri = 0;

		while (*p != '\0' && ++i <= s->dnd_idx) {
			const char	*e = strchr(p, ' ');
			size_t		 l = e ? (size_t)(e - p) : strlen(p);

			if (i == s->dnd_idx)
				is_uri = (l == 13 && memcmp(p, "text/uri-list", 13) == 0);
			p += l;
			while (*p == ' ')
				p++;
		}
		if (is_uri)
			dnd_uris_to_paths(s->dnd_data.data, s->dnd_data.len, &text);
		else
			dv_buf_append(&text, s->dnd_data.data, s->dnd_data.len);
	} else
		dv_buf_append(&text, s->dnd_data.data, s->dnd_data.len);
	if (w != nullptr && text.len > 0) {
		s->paste.len = 0;
		dv_buf_append(&s->paste, text.data, text.len);
		flush_paste(s, w);
	}
	dv_buf_free(&text);
	s->dnd_data.len = 0;
	s->dnd_self = 0;
	dnd_to_host(s, "\033]72;t=r:o=1\033\\");	/* done: it was a copy */
}

/* An event from the host: t=m motion (x=-1 leaves), t=M drop, t=r data. */
static void
dnd_from_host(struct dv_peer *p, const char *body, size_t bn, size_t ml)
{
	struct dv_session	*s = p->session;
	struct dv_window	*w;
	char			 tv[8], xs[16], ys[16], os[8], ms[8], out[1024];
	int			 x, y, px = 0, py = 0;
	const char		*mimes = bn > ml ? body + ml + 1 : "";
	size_t			 mn = bn > ml ? bn - ml - 1 : 0;

	s->dnd_peer_fd = p->fd;
	dv_meta_val(body, ml, "t", tv, sizeof tv);
	if (tv[0] == 'r' || tv[0] == 'R') {	/* data, for us or for a program */
		if (s->dnd_self) {
			unsigned char	*raw = dv_allocn<unsigned char>(mn + 3);
			long		 rn = dv_b64_decode(mimes, mn, raw, mn + 3);

			if (rn > 0)
				dv_buf_append(&s->dnd_data, raw, (size_t)rn);
			free(raw);
			dv_meta_val(body, ml, "m", ms, sizeof ms);
			if (tv[0] == 'R' || ms[0] != '1')
				dnd_deliver(s);
			return;
		}
		w = dv_window_find(s, s->dnd_target);
		if (w != nullptr && w->dnd_accept) {
			struct dv_buf	b;

			memset(&b, 0, sizeof b);
			dv_buf_append(&b, "\033]72;", 5);
			dv_buf_append(&b, body, bn);
			dv_buf_append(&b, "\033\\", 2);
			dv_window_pty_send(w, b.data, b.len);
			dv_buf_free(&b);
		}
		return;
	}
	if (tv[0] != 'm' && tv[0] != 'M')
		return;
	dv_meta_val(body, ml, "x", xs, sizeof xs);
	dv_meta_val(body, ml, "y", ys, sizeof ys);
	dv_meta_val(body, ml, "o", os, sizeof os);
	x = atoi(xs);
	y = atoi(ys);
	{
		char	t[16];

		dv_meta_val(body, ml, "X", t, sizeof t);
		px = atoi(t);
		dv_meta_val(body, ml, "Y", t, sizeof t);
		py = atoi(t);
	}
	if (mn > 0) {
		if (mn >= sizeof s->dnd_mimes)
			mn = sizeof s->dnd_mimes - 1;
		memcpy(s->dnd_mimes, mimes, mn);
		s->dnd_mimes[mn] = '\0';
	}
	w = (x < 0 || y < 0) ? nullptr :
	    window_at(s, x + p->view.vx, y + p->view.vy);
	if (w == nullptr && x >= 0 && y >= 0)
		w = s->desktop.focused;		/* the backdrop: the focused window */
	if ((w != nullptr ? w->id : 0) != s->dnd_win) {	/* crossed a window */
		struct dv_window	*old = dv_window_find(s, s->dnd_win);

		if (old != nullptr && old->dnd_accept)
			dnd_to_app(old, "\033]72;t=m:x=-1:y=-1:X=-1:Y=-1:o=0;\033\\");
		s->dnd_win = w != nullptr ? w->id : 0;
	}
	if (w == nullptr)
		return;					/* left the terminal */
	if (w->dnd_accept) {
		/* The program takes drops: the event in its own cells; its
		 * answers go back to the host as they come. */
		struct dv_desktop	*d = &s->desktop;
		unsigned		 cw = d->xpixel ? d->xpixel : DV_DEFAULT_XPIXEL;
		unsigned		 ch = d->ypixel ? d->ypixel : DV_DEFAULT_YPIXEL;
		int			 lx, ly, lpx, lpy;

		win_local(w, x + p->view.vx, y + p->view.vy, &lx, &ly);
		lpx = lx * (int)cw + (px - x * (int)cw);
		lpy = ly * (int)ch + (py - y * (int)ch);
		if (lpx < 0)
			lpx = lx * (int)cw;
		if (lpy < 0)
			lpy = ly * (int)ch;
		snprintf(out, sizeof out, "\033]72;t=%c:x=%d:y=%d:X=%d:Y=%d:o=%s;%s\033\\",
		    tv[0], lx, ly, lpx, lpy, os[0] ? os : "1",
		    tv[0] == 'M' || mn > 0 ? s->dnd_mimes : "");
		dnd_to_app(w, out);
		s->dnd_target = w->id;
		s->dnd_self = 0;
		return;
	}
	/* Ours to take, as text: files as their paths, or plain text. */
	if (dnd_pick(s->dnd_mimes) == 0) {
		dnd_to_host(s, "\033]72;t=m:o=0;\033\\");
		return;
	}
	if (tv[0] == 'm') {
		dnd_to_host(s, "\033]72;t=m:o=1;text/uri-list text/plain\033\\");
		return;
	}
	s->dnd_target = w->id;
	s->dnd_self = 1;
	s->dnd_idx = dnd_pick(s->dnd_mimes);
	s->dnd_data.len = 0;
	snprintf(out, sizeof out, "\033]72;t=r:x=%d\033\\", s->dnd_idx);
	dnd_to_host(s, out);
}

/* From a program: t=a/t=A change what we advertise; answers to motion,
 * data requests and completion go to the host; t=q asks if drops can
 * come at all. */
static void
srv_dnd_from_app(struct dv_window *w, const char *body, size_t n)
{
	struct dv_session	*s = w->session;
	char			 tv[8];
	size_t			 ml;

	if (s == nullptr)
		return;
	for (ml = 0; ml < n && body[ml] != ';'; ml++)
		;
	dv_meta_val(body, ml, "t", tv, sizeof tv);
	if (tv[0] == 'a' || tv[0] == 'A') {
		dnd_advertise(s);
		return;
	}
	if (tv[0] == 'q') {			/* a program asks: only if a host can */
		struct dv_peer	*q;
		char		 id[64], rep[160];

		TAILQ_FOREACH(q, &dv_srv->peers, entry)
			if (q->display && q->session == s && (q->features & DV_IDENT_DND))
				break;
		if (q == NULL)
			return;
		dv_meta_val(body, ml, "i", id, sizeof id);
		snprintf(rep, sizeof rep, "\033]72;t=q:i=%s;\033\\", id);
		dv_window_pty_send(w, rep, strlen(rep));
		return;
	}
	if (s->dnd_target == w->id && !s->dnd_self) {	/* its drag: relay */
		struct dv_buf	b;
		struct dv_peer	*q;

		memset(&b, 0, sizeof b);
		dv_buf_append(&b, "\033]72;", 5);
		dv_buf_append(&b, body, n);
		dv_buf_append(&b, "\033\\", 2);
		TAILQ_FOREACH(q, &dv_srv->peers, entry) {
			if (!q->display || q->session != s || q->fd != s->dnd_peer_fd)
				continue;
			dv_conn_send(&q->conn, DV_MSG_PASSTHROUGH, b.data, b.len);
			dv_conn_flush(&q->conn);
		}
		dv_buf_free(&b);
	}
}

static struct dv_window *
route_take(struct dv_session *s, unsigned osc, const char *id, size_t idl,
    int *slot)
{
	int	best = -1;
	unsigned k;

	for (k = 0; k < DV_OSC_ROUTES; k++) {
		struct dv_osc_route	*r = &s->routes[k];

		if (r->osc != osc)
			continue;
		if (idl > 0 && strlen(r->id) == idl && memcmp(r->id, id, idl) == 0) {
			best = (int)k;
			break;
		}
		if (best < 0 || r->seq > s->routes[best].seq)
			best = (int)k;
	}
	*slot = best;
	return (best < 0 ? nullptr : dv_window_find(s, s->routes[best].win));
}

static void
handle_osc_reply(struct dv_peer *p, unsigned char *payload, size_t len)
{
	struct dv_session	*s = p->session;
	struct dv_window	*w = nullptr;
	const char		*t = (const char *)payload, *body;
	size_t			 n = len, k = 0, bn, ml, idl = 0;
	unsigned		 num = 0;
	char			 id[64] = "";
	int			 slot = -1;

    if (s == nullptr || len == 0) return;
    goblin::pointer::Message pointer;
    if (goblin::pointer::parse(std::string_view(reinterpret_cast<const char *>(payload),len),pointer)) {
        if (pointer.op==goblin::pointer::Op::Event) handle_pointer(p,pointer);
        return;
    }
	if (dv_silkie_from_host(p, (const char *)payload, len))
		return;
	if (n >= 2 && t[0] == 0x1b && t[1] == ']') {	/* framed */
		t += 2;
		n -= 2;
	}
	while (k < n && t[k] >= '0' && t[k] <= '9') {
		num = num * 10 + (unsigned)(t[k] - '0');
		k++;
	}
	body = (k < n && t[k] == ';') ? t + k + 1 : t + k;
	bn = (size_t)(t + n - body);
	/* The host's string keeps its terminator; parsing does not want it. */
	if (bn >= 2 && body[bn - 2] == 0x1b && body[bn - 1] == '\\')
		bn -= 2;
	else if (bn >= 1 && body[bn - 1] == 0x07)
		bn -= 1;
	for (ml = 0; ml < bn && body[ml] != ';'; ml++)
		;
	if (num == 99)
		idl = dv_meta_val(body, ml, "i", id, sizeof id);
	else if (num == 5522)
		idl = dv_meta_val(body, ml, "id", id, sizeof id);
	else if (num == 5113 || num == 777)
		idl = dv_meta_val(body, bn, "id", id, sizeof id);

	if (num == 72) {			/* a drag: routed by position */
		dnd_from_host(p, body, bn, ml);
		return;
	}
	if (num == 52 && s->clip_query_win != 0) {
		w = dv_window_find(s, s->clip_query_win);
		s->clip_query_win = 0;
	}
	if (w == nullptr && num != 0)
		w = route_take(s, num, id, idl, &slot);
	if (slot >= 0) {
		int	done = 0;

		switch (num) {
		case 4: case 10: case 11: case 12: case 21: case 22: case 52:
			done = 1;
			break;
		case 5522: {
			char	st[32];

			dv_meta_val(body, ml, "status", st, sizeof st);
			done = !(strcmp(st, "DATA") == 0 || strcmp(st, "OK") == 0);
			break;
		}
		default:
			break;
		}
		if (done)
			s->routes[slot].osc = 0;
	}
	if (w == nullptr)
		w = s->desktop.focused;	/* unsolicited: best guess */
	if (w == nullptr || w->pty_fd < 0)
		return;
	dv_window_pty_send(w, payload, len);
}

static void
broadcast_pty(struct dv_window *w, const void *buf, size_t n)
{
	dv_vt_write(w, buf, n);
	flush_host_out(w);
	if (w->resize_req) {		/* the program asked for a new size */
		w->resize_req = 0;
		dv_window_resize_to(w, w->req_cols, w->req_rows);
	}
	redraw(w->session);
}

static void
compute_viewport(struct dv_session *s, struct dv_peer *p)
{
	unsigned	 W = s->desktop.cols, H = s->desktop.rows;
	unsigned	 vw = p->tty.cols < W ? p->tty.cols : W;
	unsigned	 vh = p->tty.rows < H ? p->tty.rows : H;
	int		 vx = p->view.vx, vy = p->view.vy;
	struct dv_window *f = s->desktop.focused;

	if (vw < 1)
		vw = 1;
	if (vh < 1)
		vh = 1;
	if (f != nullptr && !f->hidden && !f->aside) {
		int	framed = f->framed && !f->zoomed;
		int	fx = f->geom.x, fy = f->geom.y;
		int	fw = (int)dv_frame_cols(&f->geom, framed);
		int	fh = (int)dv_frame_rows(&f->geom, framed);

		if (fx < vx)
			vx = fx;
		if (fx + fw > vx + (int)vw)
			vx = fx + fw - (int)vw;
		if (fy < vy)
			vy = fy;
		if (fy + fh > vy + (int)vh)
			vy = fy + fh - (int)vh;
	}
	if (vx > (int)(W - vw))
		vx = (int)(W - vw);
	if (vy > (int)(H - vh))
		vy = (int)(H - vh);
	if (vx < 0)
		vx = 0;
	if (vy < 0)
		vy = 0;
	if (p->view.cols != vw || p->view.rows != vh) {
		free(p->view.last);
		p->view.last = nullptr;
		p->view.cols = vw;
		p->view.rows = vh;
		p->view.full = 1;
	}
	if (p->view.vx != vx || p->view.vy != vy) {
		p->view.vx = vx;
		p->view.vy = vy;
		p->view.full = 1;
	}
}

static void
compose_send(struct dv_session *s)
{
	struct dv_peer	*p;
	struct dv_cell	*desk;
	unsigned	 W = s->desktop.cols, H = s->desktop.rows;

	if (W == 0 || H == 0)
		return;
	desk = dv_allocn<dv_cell>((size_t)W * H);
	dv_compose_cells(s, desk);
	TAILQ_FOREACH(p, &dv_srv->peers, entry) {
		unsigned char	*frame = nullptr;
		size_t		 n;

		if (!p->display || p->session != s)
			continue;
		if (p->conn.out.len > 0) {
			s->dirty = 1;
			continue;
		}
		compute_viewport(s, p);
		p->view.gfx = (p->features & DV_IDENT_KITTY_GFX) != 0;
		p->view.sixel = (p->features & DV_IDENT_SIXEL) != 0;
		p->view.mcursors = (p->features & DV_IDENT_MCURSORS) != 0;
		p->view.mono = (p->features & DV_IDENT_MONO) != 0;
		n = dv_compose_frame(s, desk, &p->view, &frame);
		if (n > 0) {
			dv_conn_send(&p->conn, DV_MSG_STDOUT, frame, n);
			dv_conn_flush(&p->conn);
		}
		free(frame);
	}
	free(desk);
}

/*
 * Focus and raise a window. A zoomed window that is NOT the target yields
 * its zoom, so selecting another window never leaves one masking the rest.
 */
static void
focus_window(struct dv_session *s, struct dv_window *w)
{
	struct dv_window	*it;

	if (w == nullptr)
		return;
	TAILQ_FOREACH(it, &s->desktop.windows, entry) {
		if (it != w && it->zoomed) {
			dv_desktop_apply_unzoom(it);
			dv_window_set_pty_size(it);
		}
	}
	dv_window_focus(w);
	/* The host's pointer shape and cursor colour follow focus. */
	dv_window_apply_pointer(w);
	dv_window_apply_cursor_color(w);
	flush_host_out(w);
	redraw(s);
}

/*
 * Can any client showing this session display an image, by kitty graphics
 * or by sixel? Only then do the windows advertise graphics to their
 * programs (kitty query, sixel in DA1), since we convert to whatever each
 * client's host speaks. With no client attached the last answer stands.
 */
static void
refresh_gfx(struct dv_session *s)
{
	struct dv_peer	*q;
	int		 any = 0, gfx = 0;

	TAILQ_FOREACH(q, &dv_srv->peers, entry) {
		if (!q->display || q->session != s)
			continue;
		any = 1;
		/* Any client that can show an image by either protocol: we
		 * convert to whatever each client's host takes, so a program
		 * may draw graphics if any attached terminal can display. */
		if (q->features & (DV_IDENT_KITTY_GFX | DV_IDENT_SIXEL))
			gfx = 1;
	}
	if (any)
		s->desktop.gfx = gfx;
	/* Extra cursors are drawn by the host when one attached can. */
	s->desktop.mcursors_host = 0;
	TAILQ_FOREACH(q, &dv_srv->peers, entry)
		if (q->display && q->session == s &&
		    (q->features & DV_IDENT_MCURSORS))
			s->desktop.mcursors_host = 1;
}

static void
attach_peer(struct dv_peer *p, struct dv_session *s)
{
	p->session = s;
	p->display = 1;
	refresh_gfx(s);
	dv_silkie_attach(p);
	p->view.full = 1;
	p->view.vx = 0;
	p->view.vy = 0;
	recalculate_size(s);
	redraw(s);
}

/* Type a Unicode codepoint into the focused window's pty. */
static void
uni_insert(struct dv_session *s, uint32_t cp)
{
	struct dv_window	*w = s->desktop.focused;
	unsigned char		u[4];
	size_t			n;

	if (w == nullptr || w->pty_fd < 0 || cp == 0)
		return;
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
	dv_window_pty_send(w, u, n);
}

/* Topmost visible window whose frame rectangle covers desktop cell x,y. */
static struct dv_window *
window_at(struct dv_session *s, int x, int y)
{
	struct dv_window	*w, *best = nullptr;

	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		int	framed = w->framed && !w->zoomed;
		int	fx = w->geom.x, fy = w->geom.y;
		int	fw = (int)dv_frame_cols(&w->geom, framed);
		int	fh = (int)dv_frame_rows(&w->geom, framed);

		if (w->hidden || w->aside)
			continue;
		if (x < fx || x >= fx + fw || y < fy || y >= fy + fh)
			continue;
		if (best == nullptr || w->z > best->z)
			best = w;
	}
	return (best);
}

/* Re-encode a mouse event for the inner app's coordinate origin. */
static void
forward_mouse(struct dv_window *w, int x, int y, unsigned code, int press)
{
	int	framed = w->framed && !w->zoomed;
	int	ix = w->geom.x + (framed ? 1 : 0);
	int	iy = w->geom.y + (framed ? 1 : 0);
	int	lx = x - ix + 1 + (int)w->ox;
	int	ly = y - iy + 1 + (int)w->oy;
	char	buf[64];
	int	n;

	if (!w->screen.mouse || w->pty_fd < 0)
		return;
	if (lx < 1)
		lx = 1;
	if (ly < 1)
		ly = 1;
	if ((unsigned)lx > w->screen.cols)
		lx = (int)w->screen.cols;
	if ((unsigned)ly > w->screen.rows)
		ly = (int)w->screen.rows;
	if (w->screen.mouse_sgr == 2) {	/* SGR pixels (1016): cell centres */
		struct dv_desktop	*d = &w->session->desktop;
		unsigned		 cw = d->xpixel ? d->xpixel : DV_DEFAULT_XPIXEL;
		unsigned		 chh = d->ypixel ? d->ypixel : DV_DEFAULT_YPIXEL;

		n = snprintf(buf, sizeof buf, "\033[<%u;%u;%u%c", code,
		    (unsigned)(lx - 1) * cw + cw / 2,
		    (unsigned)(ly - 1) * chh + chh / 2, press ? 'M' : 'm');
	} else if (w->screen.mouse_sgr)
		n = snprintf(buf, sizeof buf, "\033[<%u;%d;%d%c", code, lx, ly,
		    press ? 'M' : 'm');
	else {
		unsigned	cb = press ? (code & 0xffu) : 3u;

		if (cb > 223 || lx > 223 || ly > 223)
			return;
		n = snprintf(buf, sizeof buf, "\033[M%c%c%c",
		    (char)(32 + cb), (char)(32 + lx), (char)(32 + ly));
	}
	if (n > 0)
		dv_window_pty_send(w, buf, (size_t)n);
}

/*
 * The window under the pointer. Leaving a window that tracks the mouse
 * in pixel mode gets kitty's leave event (button 288: motion with bit
 * 256), so a program can drop its hover state; the host's pointer shape
 * follows the window under it, the focused one over the backdrop.
 */
static void
hover_set(struct dv_session *s, struct dv_window *w)
{
	unsigned		 id = w != nullptr ? w->id : 0;
	struct dv_window	*old, *pw;

	if (id == s->hover_win)
		return;
	old = s->hover_win ? dv_window_find(s, s->hover_win) : nullptr;
	if (old != nullptr && old->screen.mouse && old->screen.mouse_sgr == 2)
		dv_window_pty_send(old, "\033[<288;1;1M", 11);
	s->hover_win = id;
	pw = w != nullptr ? w : s->desktop.focused;
	if (pw != nullptr) {
		dv_window_apply_pointer(pw);
		flush_host_out(pw);
	}
}

/*
 * Wheel over a window with no mouse tracking: do what a native terminal
 * does. On the alternate screen (a pager, an editor) translate each notch
 * into a few arrow keys -- "alternate scroll" -- honouring DECCKM and the
 * app's DECSET 1007. On the primary screen there is nothing to scroll
 * (goblin-view keeps no scrollback), so it is left alone.
 */
static int
scroll_window(struct dv_window *w, int up)
{
	const char	*seq;
	int		 i;

	if (w == nullptr || w->pty_fd < 0)
		return (0);
	if (w->screen.alt_on) {
		/* A pager or editor: alternate scroll, wheel becomes arrows. */
		if (!w->screen.alt_scroll)
			return (0);
		if (w->screen.app_cursor)
			seq = up ? "\033OA" : "\033OB";
		else
			seq = up ? "\033[A" : "\033[B";
		for (i = 0; i < 3; i++)
			dv_window_pty_send(w, seq, strlen(seq));
		return (0);		/* the app's output triggers the redraw */
	}
	/* The primary screen: scroll our own history, as a native terminal
	 * does at a shell prompt. */
	return (dv_screen_scroll(&w->screen, up ? 3 : -3));
}

/*
 * A mouse event in the peer's viewport-local 1-based coordinates. Left
 * drag on the frame moves, on the corner resizes, in the body selects
 * and (if the app asked for the mouse) forwards. The picker takes clicks
 * while it is up. code is the raw SGR button (motion 32, wheel 64).
 */
static void
handle_mouse(struct dv_peer *p, struct dv_session *s, unsigned vx,
    unsigned vy, unsigned code, int press)
{
	struct dv_window	*w;
	int			 x, y, motion, wheel, button;

	if (s == nullptr)
		return;
	x = (int)vx - 1 + p->view.vx;
	y = (int)vy - 1 + p->view.vy;
	motion = (code & 32) != 0;
	wheel = (code & 64) != 0;
	button = (int)(code & 3);

	if (s->uni_open) {
		int	nc = 0, ns = 0, rc;

		if (!press || motion || wheel)
			return;
		rc = dv_uni_click(s->desktop.cols, s->desktop.rows, s->uni_cat,
		    s->uni_sel, x, y, &nc, &ns);
		if (rc < 0)
			s->uni_open = 0;
		else if (rc == 0) {
			s->uni_cat = nc;
			s->uni_sel = 0;
		} else {
			s->uni_sel = ns;
			uni_insert(s, dv_uni_char(s->uni_cat, ns));
		}
		redraw(s);
		return;
	}
	if (s->menu_open || s->quit_confirm || s->help_open || s->xfer_prompt ||
	    s->switch_open || s->copy_open)
		return;			/* overlays own the screen */

	if (wheel) {
		/*
		 * Scroll the window under the pointer, or the focused one when
		 * the pointer is over the backdrop. A mouse-tracking app gets
		 * the wheel event; anything else scrolls natively (arrows on
		 * the alternate screen). Scrolling never raises or refocuses.
		 */
		w = window_at(s, x, y);
		if (w == nullptr)
			w = s->desktop.focused;
		if (w == nullptr)
			return;
		if (w->screen.mouse)
			forward_mouse(w, x, y, code, 1);
		else if ((code & 3) < 2 &&	/* 64 = up, 65 = down (not horizontal) */
		    scroll_window(w, (code & 3) == 0))
			redraw(s);
		return;
	}

	if (s->drag != 0 && (motion || !press)) {
		w = dv_window_find(s, s->drag_win);
		if (w == nullptr) {
			s->drag = 0;
			return;
		}
		if (!press) {			/* button up ends the drag */
			if (s->drag == DV_DRAG_FORWARD)
				forward_mouse(w, x, y, code, 0);
			else if (s->drag == DV_DRAG_SELECT) {
				s->sel_active = 0;
				if (s->sel_ax == s->sel_cx &&
				    s->sel_ay == s->sel_cy)
					s->sel_done = 0;	/* a click, not a drag */
				else {
					s->sel_done = 1;
					copy_selection(s);
				}
				redraw(s);
			}
			s->drag = 0;
			return;
		}
		if (s->drag == DV_IN_MOVE) {
			int	framed = w->framed && !w->zoomed;
			int	chrome = framed ? 2 : 0;
			int	ox = w->geom.x, oy = w->geom.y;
			int	nx = x - s->grab_dx, ny = y - s->grab_dy;
			int	shrank = 0;

			if (nx < 0)
				nx = 0;
			if (ny < 0)
				ny = 0;
			w->geom.x = nx;
			w->geom.y = ny;
			if (nx != ox || ny != oy)
				dv_silkie_cancel_window(w);
			/*
			 * Pushed past the right/bottom edge: instead of the
			 * window stopping, shrink it so its far edge stays at
			 * the desktop boundary while the grab point keeps up.
			 */
			if (nx + (int)w->geom.cols + chrome > (int)s->desktop.cols) {
				int	nc = (int)s->desktop.cols - nx - chrome;

				if (nc < (int)DV_MIN_INNER_COLS)
					nc = (int)DV_MIN_INNER_COLS;
				if ((unsigned)nc != w->geom.cols) {
					w->geom.cols = (unsigned)nc;
					shrank = 1;
				}
			}
			if (ny + (int)w->geom.rows + chrome > (int)s->desktop.rows) {
				int	nr = (int)s->desktop.rows - ny - chrome;

				if (nr < (int)DV_MIN_INNER_ROWS)
					nr = (int)DV_MIN_INNER_ROWS;
				if ((unsigned)nr != w->geom.rows) {
					w->geom.rows = (unsigned)nr;
					shrank = 1;
				}
			}
			dv_clamp_geom(s->desktop.cols, s->desktop.rows, framed,
			    &w->geom);
			if (shrank)
				dv_window_set_pty_size(w);
			redraw(s);
		} else if (s->drag == DV_IN_RESIZE) {
			int	framed = w->framed && !w->zoomed;
			int	chrome = framed ? 2 : 0;
			int	nc = x - w->geom.x + 1 - chrome;
			int	nr = y - w->geom.y + 1 - chrome;

			if (nc < (int)DV_MIN_INNER_COLS)
				nc = (int)DV_MIN_INNER_COLS;
			if (nr < (int)DV_MIN_INNER_ROWS)
				nr = (int)DV_MIN_INNER_ROWS;
			w->geom.cols = (unsigned)nc;
			w->geom.rows = (unsigned)nr;
			dv_clamp_geom(s->desktop.cols, s->desktop.rows, framed,
			    &w->geom);
			dv_window_set_pty_size(w);
			redraw(s);
		} else if (s->drag == DV_DRAG_SELECT) {
			win_local(w, x, y, &s->sel_cx, &s->sel_cy);
			redraw(s);
		} else if (s->drag == DV_DRAG_FORWARD)
			forward_mouse(w, x, y, code, 1);
		return;
	}

	if (code & 256) {		/* kitty: the pointer left the terminal */
		hover_set(s, nullptr);
		return;
	}
	if (motion && button == 3) {	/* hover: motion with no button held */
		w = window_at(s, x, y);
		hover_set(s, w);
		if (w != nullptr && w->screen.mouse == 1003)
			forward_mouse(w, x, y, code, 1);
		return;
	}
	if (motion || !press)
		return;

	w = window_at(s, x, y);
	if (w == nullptr)
		return;			/* backdrop */
	if (dv_cfg.click_raises)
		focus_window(s, w);
	else {				/* click-raises = no: focus, keep the stacking */
		struct dv_window	*it;

		TAILQ_FOREACH(it, &s->desktop.windows, entry)
			if (it != w && it->zoomed) {
				dv_desktop_apply_unzoom(it);
				dv_window_set_pty_size(it);
			}
		dv_window_focus_noraise(w);
		dv_window_apply_pointer(w);
		dv_window_apply_cursor_color(w);
		flush_host_out(w);
		redraw(s);
	}

	if (button == 0) {
		int		framed = w->framed && !w->zoomed;
		int		fx = w->geom.x, fy = w->geom.y;
		int		fw = (int)dv_frame_cols(&w->geom, framed);
		int		fh = (int)dv_frame_rows(&w->geom, framed);
		int		corner = (x == fx + fw - 1 && y == fy + fh - 1);
		int		border = framed && (x == fx || x == fx + fw - 1 ||
				    y == fy || y == fy + fh - 1);
		int		on_title = framed && y == fy && !corner;
		long long	now = now_ms();
		int		dbl = on_title && s->last_click_win == w->id &&
				    now - s->last_click_ms < 400 &&
				    s->last_click_y == y &&
				    (x - s->last_click_x) < 3 &&
				    (s->last_click_x - x) < 3;

		s->last_click_ms = now;
		s->last_click_win = w->id;
		s->last_click_x = x;
		s->last_click_y = y;
		if (dbl) {			/* double-click the title: rename */
			s->title_edit = 1;
			s->title_win = w->id;
			s->title_ed[0] = '\0';	/* start fresh */
			s->title_edlen = 0;
			s->drag = 0;
			redraw(s);
			return;
		}

		if (corner) {
			s->drag = DV_IN_RESIZE;
			s->drag_win = w->id;
		} else if (border) {
			s->drag = DV_IN_MOVE;
			s->drag_win = w->id;
			s->grab_dx = x - fx;
			s->grab_dy = y - fy;
		} else if (w->screen.mouse && !(code & 4)) {
			s->drag = DV_DRAG_FORWARD;	/* app wants the mouse */
			s->drag_win = w->id;
			forward_mouse(w, x, y, code, 1);
		} else {
			/* Body drag selects text (Shift forces it over an app). */
			int	sx, sy;

			win_local(w, x, y, &sx, &sy);
			s->drag = DV_DRAG_SELECT;
			s->drag_win = w->id;
			s->sel_win = w->id;
			s->sel_ax = s->sel_cx = sx;
			s->sel_ay = s->sel_cy = sy;
			s->sel_active = 1;
			s->sel_done = 0;
			redraw(s);
		}
		return;
	}
	if (w->screen.mouse)
		forward_mouse(w, x, y, code, 1);
}

void
dv_host_passthrough(struct dv_session *s, const char *bytes, size_t n)
{
	struct dv_peer	*p;

	if (s == nullptr || dv_srv == nullptr || bytes == nullptr || n == 0)
		return;
	TAILQ_FOREACH(p, &dv_srv->peers, entry) {
		if (!p->display || p->session != s)
			continue;
		dv_conn_send(&p->conn, DV_MSG_PASSTHROUGH, bytes, n);
		dv_conn_flush(&p->conn);
	}
}

void
dv_mouse_inject(struct dv_peer *p, unsigned vx, unsigned vy, unsigned code,
    int press)
{
	if (p == nullptr)
		return;
	handle_mouse(p, p->session, vx, vy, code, press);
}

void
dv_silkie_peers_changed(struct dv_session *s)
{
	if (s == nullptr || dv_srv == nullptr)
		return;
	dv_silkie_recompute(s, TAILQ_FIRST(&dv_srv->peers));
	dv_silkie_push(s);
}

/* Map a desktop cell to a window's own screen cell (with scroll offset). */
static void
win_local(struct dv_window *w, int x, int y, int *sx, int *sy)
{
	int	framed = w->framed && !w->zoomed;

	*sx = x - (w->geom.x + (framed ? 1 : 0)) + (int)w->ox;
	*sy = y - (w->geom.y + (framed ? 1 : 0)) + (int)w->oy;
}

static void
b64_encode(const unsigned char *in, size_t n, struct dv_buf *out)
{
	static const char	*a =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t	i;

	for (i = 0; i + 2 < n; i += 3) {
		char	q[4];

		q[0] = a[in[i] >> 2];
		q[1] = a[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
		q[2] = a[((in[i + 1] & 0xf) << 2) | (in[i + 2] >> 6)];
		q[3] = a[in[i + 2] & 0x3f];
		dv_buf_append(out, q, 4);
	}
	if (n - i == 1) {
		char	q[4];

		q[0] = a[in[i] >> 2];
		q[1] = a[(in[i] & 3) << 4];
		q[2] = '=';
		q[3] = '=';
		dv_buf_append(out, q, 4);
	} else if (n - i == 2) {
		char	q[4];

		q[0] = a[in[i] >> 2];
		q[1] = a[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
		q[2] = a[(in[i + 1] & 0xf) << 2];
		q[3] = '=';
		dv_buf_append(out, q, 4);
	}
}

/*
 * Copy the current selection from its window to the system clipboard,
 * as OSC-52 handed to the host. Cells are read in reading order, trailing
 * blanks per line are dropped, and rows are joined with newlines.
 */
namespace gp = goblin::pointer;
static int pointer_device(struct dv_peer *p, int id)
{
    static int next = 1;
    for (int i=0;i<p->pointer_device_count;++i) if (p->pointer_host_device[i]==id) return p->pointer_app_device[i];
    if (p->pointer_device_count==64 || next==0x7fffffff) return -1;
    const int at=p->pointer_device_count++;
    p->pointer_host_device[at]=id; p->pointer_app_device[at]=next++;
    return p->pointer_app_device[at];
}
static gp::Message pointer_local(struct dv_peer *p, struct dv_window *w, gp::Message m)
{
    m.device=pointer_device(p,m.device);
    const bool frame=w->framed && !w->zoomed;
    const unsigned cw=p->tty.xpixel ? p->tty.xpixel : DV_DEFAULT_XPIXEL;
    const unsigned ch=p->tty.ypixel ? p->tty.ypixel : DV_DEFAULT_YPIXEL;
    const unsigned aw=w->session->desktop.xpixel ? w->session->desktop.xpixel : DV_DEFAULT_XPIXEL;
    const unsigned ah=w->session->desktop.ypixel ? w->session->desktop.ypixel : DV_DEFAULT_YPIXEL;
    m.x=gp::window_coordinate(m.x,p->view.vx,w->geom.x+int(frame),int(w->ox),cw,aw);
    m.y=gp::window_coordinate(m.y,p->view.vy,w->geom.y+int(frame),int(w->oy),ch,ah);
    // Gesture translation is a vector: scale it but do not translate its origin.
    m.dx=gp::window_coordinate(m.dx,0,0,0,cw,aw);
    m.dy=gp::window_coordinate(m.dy,0,0,0,ch,ah);
    return m;
}
static void pointer_mouse(struct dv_peer *p, struct dv_window *w, const gp::Message &event, int buttons)
{
    unsigned code=(event.tool==2 || (buttons&2) ? 2 : buttons&4 ? 1 : 0) | ((event.mods&1)?4:0) | ((event.mods&2)?8:0) | ((event.mods&4)?16:0);
    if (event.phase==gp::Move || event.phase==gp::Hover) code|=32;
    if (event.phase==gp::Hover) code|=3;
    const bool motion=event.phase==gp::Move || event.phase==gp::Hover;
    if (!w->screen.mouse || (motion && w->screen.mouse!=1003 && !(event.phase==gp::Move && w->screen.mouse==1002))) return;
    const bool press=event.phase!=gp::Up && event.phase!=gp::Cancel;
    if (w->screen.mouse_sgr==2) {
        const auto local=pointer_local(p,w,event);
        const unsigned aw=w->session->desktop.xpixel ? w->session->desktop.xpixel : DV_DEFAULT_XPIXEL;
        const unsigned ah=w->session->desktop.ypixel ? w->session->desktop.ypixel : DV_DEFAULT_YPIXEL;
        const int px=std::clamp(local.x/gp::unit+1,1,int(w->screen.cols*aw));
        const int py=std::clamp(local.y/gp::unit+1,1,int(w->screen.rows*ah));
        char buf[80];const int n=snprintf(buf,sizeof buf,"\033[<%u;%d;%d%c",code,px,py,press?'M':'m');
        dv_window_pty_send(w,buf,size_t(n));
    } else {
        const unsigned cw=p->tty.xpixel ? p->tty.xpixel : DV_DEFAULT_XPIXEL;
        const unsigned ch=p->tty.ypixel ? p->tty.ypixel : DV_DEFAULT_YPIXEL;
        forward_mouse(w,gp::cell_at(event.x,cw)+p->view.vx,gp::cell_at(event.y,ch)+p->view.vy,code,press);
    }
}
static void pointer_cancel_peer(struct dv_peer *p)
{
    for (auto &c:p->pointer_contacts) if (c.used) {
        auto *w=p->session ? dv_window_find(p->session,c.window) : nullptr;
        if (w && c.epoch==w->pointer_epoch) {
            if (c.legacy==2) {
                if (p->session->drag_win==w->id) { p->session->drag=0; p->session->sel_active=0; redraw(p->session); }
            } else if (c.legacy) {
                auto m=c.event; m.phase=gp::Cancel;
                pointer_mouse(p,w,m,c.event.buttons);
            } else {
                auto m=pointer_local(p,w,c.event); m.phase=gp::Cancel; m.buttons=m.p=0;
                dv_pointer_send(w,m);
            }
        }
        c.used=0;
    }
}
static void handle_pointer(struct dv_peer *p, const gp::Message &event)
{
    auto *s=p->session; if (!s || !p->display || pointer_device(p,event.device)<0) return;
    const unsigned cw=p->tty.xpixel ? p->tty.xpixel : DV_DEFAULT_XPIXEL;
    const unsigned ch=p->tty.ypixel ? p->tty.ypixel : DV_DEFAULT_YPIXEL;
    const int x=gp::cell_at(event.x,cw)+p->view.vx, y=gp::cell_at(event.y,ch)+p->view.vy;
    dv_pointer_capture *capture=nullptr, *unused=nullptr;
    for (auto &c:p->pointer_contacts) {
        if (!c.used) { if (!unused) unused=&c; continue; }
        if (c.event.device==event.device && c.event.contact==event.contact && c.event.kind==event.kind) capture=&c;
    }
    auto *w=capture ? dv_window_find(s,capture->window) : window_at(s,x,y);
    if (capture && (!w || capture->epoch!=w->pointer_epoch)) { capture->used=0; return; }
    if (s->menu_open || s->quit_confirm || s->help_open || s->xfer_prompt || s->switch_open || s->copy_open || s->uni_open) {
        pointer_cancel_peer(p); return;
    }
    if (event.kind==gp::Gesture) {
        dv_window *first=nullptr,*second=nullptr;
        for (auto &c:p->pointer_contacts) if (c.used && !c.legacy && c.event.kind==gp::Touch && c.event.device==event.device) {
            if (c.event.contact==event.contact) first=dv_window_find(s,c.window);
            if (c.event.contact==event.peer) second=dv_window_find(s,c.window);
        }
        if (!capture && (!first || first!=second)) return;
        if (!capture) w=first;
    }
    if (!w) return;
    const bool framed=w->framed && !w->zoomed;
    const bool content=x>=w->geom.x+int(framed) && y>=w->geom.y+int(framed)
        && x<w->geom.x+int(framed)+int(w->geom.cols) && y<w->geom.y+int(framed)+int(w->geom.rows);
    if (capture && capture->legacy==2) {
        unsigned code=(event.mods&1 ? 4 : 0) | (event.phase==gp::Move ? 32 : 0);
        handle_mouse(p,s,unsigned(std::max(1,gp::cell_at(event.x,cw)+1)),unsigned(std::max(1,gp::cell_at(event.y,ch)+1)),code,
                     event.phase!=gp::Up && event.phase!=gp::Cancel);
        capture->event=event;
        if (event.phase==gp::Up || event.phase==gp::Cancel) capture->used=0;
        return;
    }
    if (!capture && (event.phase==gp::Up || event.phase==gp::Cancel || event.phase==gp::Move)) return;
    if (!capture && (!content || (event.mods&1) || (!w->screen.pointer_mode && !w->screen.mouse))) {
        if (event.kind!=gp::Gesture) {
            unsigned code=(event.buttons&2 ? 2 : event.buttons&4 ? 1 : 0) | ((event.mods&1)?4:0) | ((event.mods&2)?8:0) | ((event.mods&4)?16:0);
            if (event.phase==gp::Hover) code|=35;
            if (event.phase==gp::Down && unused) {
                unused->used=1; unused->legacy=2; unused->window=w->id; unused->epoch=w->pointer_epoch; unused->event=event;
            }
            handle_mouse(p,s,unsigned(std::max(1,gp::cell_at(event.x,cw)+1)),unsigned(std::max(1,gp::cell_at(event.y,ch)+1)),code,1);
        }
        return;
    }
    if (event.phase==gp::Down) {
        // A legacy mouse stream has one contact per touch device. Secondary
        // fingers cannot introduce unmatched extra presses in the child app.
        if (event.kind==gp::Touch && !w->screen.pointer_mode)
            for (const auto &c:p->pointer_contacts)
                if (c.used && c.legacy==1 && c.window==w->id && c.event.kind==gp::Touch
                    && c.event.device==event.device && c.event.contact!=event.contact) return;
        if (!capture) capture=unused;
        if (!capture) return;
        capture->used=1; capture->window=w->id; capture->epoch=w->pointer_epoch;
        capture->legacy=!w->screen.pointer_mode;
        if (event.kind!=gp::Gesture && s->desktop.focused!=w) { focus_window(s,w); redraw(s); }
    }
    const int buttons=(capture && (event.phase==gp::Up || event.phase==gp::Cancel)) ? capture->event.buttons : event.buttons;
    if (capture) capture->event=event;
    if ((capture && capture->legacy) || !w->screen.pointer_mode) {
        if (event.kind==gp::Gesture) return;
        pointer_mouse(p,w,event,buttons);
    } else dv_pointer_send(w,pointer_local(p,w,event));
    if (capture && (event.phase==gp::Up || event.phase==gp::Cancel)) capture->used=0;
}

/* Text to the host clipboard (OSC 52 through this window's host path),
 * and kept as the mark that Transfer pastes. */
static void
clipboard_set(struct dv_session *s, struct dv_window *w, const void *text,
    size_t len)
{
	struct dv_buf	b64, msg;

	s->mark.len = 0;
	dv_buf_append(&s->mark, text, len);
	memset(&b64, 0, sizeof b64);
	b64_encode((const unsigned char *)text, len, &b64);
	memset(&msg, 0, sizeof msg);
	dv_buf_append(&msg, "\033]52;c;", 7);
	dv_buf_append(&msg, b64.data, b64.len);
	dv_buf_append(&msg, "\033\\", 2);
	dv_buf_append(&w->host_out, msg.data, msg.len);
	flush_host_out(w);		/* to the host clipboard */
	dv_buf_free(&b64);
	dv_buf_free(&msg);
}

/* ---- copy mode: a cursor over a window's scrollback and screen ---- */

/* Rows a window has to move over: its scrollback lines, then its rows. */
static long
copy_rows(const struct dv_window *w)
{
	return ((w->screen.alt_on ? 0 : (long)w->screen.sb_len) + (long)w->screen.rows);
}

/* The cells of virtual row vy and how many there are. */
static const struct dv_cell *
copy_row(struct dv_window *w, long vy, unsigned *n)
{
	struct dv_screen	*sc = &w->screen;
	long			 sb = sc->alt_on ? 0 : (long)sc->sb_len;

	if (vy < 0 || vy >= copy_rows(w)) {
		*n = 0;
		return (nullptr);
	}
	if (vy < sb)
		return (dv_screen_history_row(sc, (unsigned)vy, n));
	*n = sc->cols;
	return (dv_screen_at(sc, 0, (unsigned)(vy - sb)));
}

/* Keep the copy cursor on screen (by scrolling the window) and mirror the
 * selection into the displayed coordinates the painter highlights. */
static void
copy_show(struct dv_session *s, struct dv_window *w)
{
	struct dv_screen	*sc = &w->screen;
	long			 sb = sc->alt_on ? 0 : (long)sc->sb_len, top;

	if (s->copy_y < 0)
		s->copy_y = 0;
	if (s->copy_y >= copy_rows(w))
		s->copy_y = copy_rows(w) - 1;
	if (s->copy_x < 0)
		s->copy_x = 0;
	if (s->copy_x >= (int)sc->cols)
		s->copy_x = (int)sc->cols - 1;
	if (!sc->alt_on) {
		top = sb - (long)sc->sb_off;
		if (s->copy_y < top)
			sc->sb_off = (unsigned)(sb - s->copy_y);
		else if (s->copy_y >= top + (long)sc->rows)
			sc->sb_off = (unsigned)(sb - (s->copy_y - (long)sc->rows + 1));
		if (sc->sb_off > sc->sb_len)
			sc->sb_off = sc->sb_len;
	}
	top = sb - (long)sc->sb_off;
	s->sel_win = w->id;
	s->sel_active = 0;
	s->sel_done = s->copy_sel;
	s->sel_ax = s->copy_ax;
	s->sel_ay = (int)(s->copy_ay - top);
	s->sel_cx = s->copy_x;
	s->sel_cy = (int)(s->copy_y - top);
}

/* The selected text: a linear selection between the anchor and the cursor. */
static void
copy_text(struct dv_session *s, struct dv_window *w, struct dv_buf *out)
{
	long	ay = s->copy_ay, cy = s->copy_y, vy;
	int	ax = s->copy_ax, cx = s->copy_x;

	if (!s->copy_sel) {		/* no selection: the cursor's line */
		ay = cy;
		ax = 0;
		cx = (int)w->screen.cols - 1;
	}
	if (ay > cy || (ay == cy && ax > cx)) {
		long	ty = ay; ay = cy; cy = ty;
		int	tx = ax; ax = cx; cx = tx;
	}
	for (vy = ay; vy <= cy; vy++) {
		unsigned		 n;
		const struct dv_cell	*row = copy_row(w, vy, &n);
		int			 x0 = vy == ay ? ax : 0;
		int			 x1 = vy == cy ? cx : (int)n - 1;

		if (row != nullptr && x0 < (int)n) {
			if (x1 >= (int)n)
				x1 = (int)n - 1;
			if (x1 >= x0)
				dv_row_text(out, row + x0, (unsigned)(x1 - x0 + 1));
		}
		if (vy < cy)
			dv_buf_append(out, "\n", 1);
	}
}

/* Search from the cursor in direction dir for the pattern, wrapping; the
 * match is case-insensitive when the pattern has no capitals. */
static int
copy_search(struct dv_session *s, struct dv_window *w, int dir)
{
	long		 rows = copy_rows(w), i, vy;
	const char	*pat = s->copy_pattern;
	size_t		 pl = strlen(pat), k;
	int		 fold = 1;

	if (pl == 0 || rows == 0)
		return (0);
	for (k = 0; k < pl; k++)
		if (isupper((unsigned char)pat[k]))
			fold = 0;
	for (i = 0; i <= rows; i++) {
		struct dv_buf		 line;
		unsigned		 n;
		const struct dv_cell	*row;
		long			 best = -1;

		vy = s->copy_y + (dir > 0 ? i : -i);
		vy = ((vy % rows) + rows) % rows;
		row = copy_row(w, vy, &n);
		if (row == nullptr)
			continue;
		memset(&line, 0, sizeof line);
		{
			/* cell columns, not UTF-8 bytes: one char per cell */
			unsigned	x;

			for (x = 0; x < n; x++) {
				uint32_t	ch = row[x].ch;

				if (ch == 0)
					ch = U' ';
				if (ch > 0x7f)
					ch = 0x80;	/* a non-ASCII cell never matches ASCII */
				dv_buf_append(&line, &ch, 1);
			}
			dv_buf_append(&line, "", 1);
		}
		{
			const char	*t = (const char *)line.data;
			long		 from = (i == 0) ? (dir > 0 ? s->copy_x + 1 : s->copy_x - 1) :
					    (dir > 0 ? 0 : (long)n - 1);
			long		 x;

			if (dir > 0) {
				for (x = from < 0 ? 0 : from; x + (long)pl <= (long)n; x++)
					if ((fold ? strncasecmp(t + x, pat, pl) : strncmp(t + x, pat, pl)) == 0) {
						best = x;
						break;
					}
			} else {
				for (x = from > (long)n - (long)pl ? (long)n - (long)pl : from; x >= 0; x--)
					if ((fold ? strncasecmp(t + x, pat, pl) : strncmp(t + x, pat, pl)) == 0) {
						best = x;
						break;
					}
			}
		}
		dv_buf_free(&line);
		if (best >= 0) {
			s->copy_y = vy;
			s->copy_x = (int)best;
			copy_show(s, w);
			return (1);
		}
	}
	return (0);
}

static void
copy_leave(struct dv_session *s, struct dv_window *w)
{
	s->copy_open = 0;
	s->copy_sel = 0;
	s->copy_searching = 0;
	s->sel_done = 0;
	s->sel_active = 0;
	if (w != nullptr)
		w->screen.sb_off = 0;
	redraw(s);
}

/* Keys in copy mode, as the bytes the host sent. */
static void
copy_key(struct dv_session *s, const unsigned char *d, size_t len)
{
	struct dv_window	*w = dv_window_find(s, s->copy_win);
	const char		*arrow;
	int			 step = 1;
	unsigned char		 c = len > 0 ? d[0] : 0;

	if (w == nullptr) {
		copy_leave(s, nullptr);
		return;
	}
	if (s->copy_searching) {
		size_t	pl = strlen(s->copy_pattern);

		if (len == 1 && (c == '\r' || c == '\n')) {
			s->copy_dir = s->copy_searching;
			s->copy_searching = 0;
			copy_search(s, w, s->copy_dir);
		} else if (len == 1 && c == 0x1b)
			s->copy_searching = 0;
		else if (len == 1 && (c == 0x7f || c == 0x08)) {
			if (pl > 0)
				s->copy_pattern[pl - 1] = '\0';
		} else if (len == 1 && c >= 0x20 && c < 0x7f && pl + 1 < sizeof s->copy_pattern) {
			s->copy_pattern[pl] = (char)c;
			s->copy_pattern[pl + 1] = '\0';
		}
		redraw(s);
		return;
	}
	arrow = key_arrow(d, len, &step);
	if (arrow != nullptr) {
		if (strcmp(arrow, "up") == 0)
			s->copy_y -= step;
		else if (strcmp(arrow, "down") == 0)
			s->copy_y += step;
		else if (strcmp(arrow, "left") == 0)
			s->copy_x -= step;
		else
			s->copy_x += step;
	} else if (len == 4 && d[0] == 0x1b && d[1] == '[' && d[3] == '~' && (d[2] == '5' || d[2] == '6')) {
		s->copy_y += d[2] == '5' ? -(long)w->screen.rows / 2 : (long)w->screen.rows / 2;
	} else if (len != 1) {
		return;				/* some other sequence: ignored */
	} else switch (c) {
	case 0x1b: case 'q':
		copy_leave(s, w);
		return;
	case 'h': s->copy_x--; break;
	case 'j': s->copy_y++; break;
	case 'k': s->copy_y--; break;
	case 'l': s->copy_x++; break;
	case '0': s->copy_x = 0; break;
	case '$': {
		unsigned		 n;
		const struct dv_cell	*row = copy_row(w, s->copy_y, &n);

		s->copy_x = (int)n - 1;
		while (row != nullptr && s->copy_x > 0 &&
		    (row[s->copy_x].ch == U' ' || row[s->copy_x].ch == 0))
			s->copy_x--;
		break;
	}
	case 'g': s->copy_y = 0; break;
	case 'G': s->copy_y = copy_rows(w) - 1; break;
	case 0x15: s->copy_y -= (long)w->screen.rows / 2; break;	/* Ctrl-U */
	case 0x04: s->copy_y += (long)w->screen.rows / 2; break;	/* Ctrl-D */
	case 'v': case ' ':
		s->copy_sel = 1;
		s->copy_ax = s->copy_x;
		s->copy_ay = s->copy_y;
		break;
	case 'y': case '\r': case '\n': {
		struct dv_buf	text;

		memset(&text, 0, sizeof text);
		copy_text(s, w, &text);
		if (text.len > 0)
			clipboard_set(s, w, text.data, text.len);
		dv_buf_free(&text);
		copy_leave(s, w);
		return;
	}
	case '/': case '?':
		s->copy_searching = c == '/' ? 1 : -1;
		s->copy_pattern[0] = '\0';
		break;
	case 'n':
		copy_search(s, w, s->copy_dir ? s->copy_dir : 1);
		break;
	case 'N':
		copy_search(s, w, s->copy_dir ? -s->copy_dir : -1);
		break;
	default:
		break;
	}
	copy_show(s, w);
	redraw(s);
}

/* A copy-mode key named by a menu word (from a script). */
static void
copy_word(struct dv_session *s, const char *word)
{
	static const struct { const char *w, *bytes; } map[] = {
		{ "up", "\033[A" }, { "down", "\033[B" }, { "right", "\033[C" },
		{ "left", "\033[D" }, { "enter", "\r" }, { "esc", "\033" },
		{ "backspace", "\177" }, { "space", " " }, { "pageup", "\033[5~" },
		{ "pagedown", "\033[6~" }, { "menu", "\033" },
	};
	size_t	i;

	for (i = 0; i < sizeof map / sizeof map[0]; i++)
		if (strcmp(word, map[i].w) == 0) {
			copy_key(s, (const unsigned char *)map[i].bytes, strlen(map[i].bytes));
			return;
		}
	copy_key(s, (const unsigned char *)word, strlen(word));
}

static void
copy_selection(struct dv_session *s)
{
	struct dv_window	*w = dv_window_find(s, s->sel_win);
	struct dv_buf		 text;
	unsigned		 y, x;

	if (w == nullptr || !s->sel_done)
		return;
	memset(&text, 0, sizeof text);
	for (y = 0; y < w->screen.rows; y++) {
		size_t	line_start = text.len, trim;

		for (x = 0; x < w->screen.cols; x++) {
			struct dv_cell	*c;
			unsigned char	u[4];
			uint32_t	ch;
			size_t		k, un;

			if (!dv_sel_contains(s, (int)x, (int)y))
				continue;
			c = dv_screen_at(&w->screen, x, y);
			ch = (c == nullptr || c->ch == 0) ? U' ' : c->ch;
			if (ch == DV_KITTY_PLACEHOLDER)
				ch = U' ';
			/* utf8 */
			if (ch < 0x80) {
				u[0] = (unsigned char)ch; un = 1;
			} else if (ch < 0x800) {
				u[0] = 0xc0 | (ch >> 6);
				u[1] = 0x80 | (ch & 0x3f); un = 2;
			} else if (ch < 0x10000) {
				u[0] = 0xe0 | (ch >> 12);
				u[1] = 0x80 | ((ch >> 6) & 0x3f);
				u[2] = 0x80 | (ch & 0x3f); un = 3;
			} else {
				u[0] = 0xf0 | (ch >> 18);
				u[1] = 0x80 | ((ch >> 12) & 0x3f);
				u[2] = 0x80 | ((ch >> 6) & 0x3f);
				u[3] = 0x80 | (ch & 0x3f); un = 4;
			}
			for (k = 0; k < un; k++)
				dv_buf_append(&text, &u[k], 1);
		}
		/* Trim trailing spaces on this line. */
		trim = text.len;
		while (trim > line_start && text.data[trim - 1] == ' ')
			trim--;
		text.len = trim;
		if (y + 1 < w->screen.rows && dv_sel_contains(s, 0, (int)(y + 1)))
			dv_buf_append(&text, "\n", 1);
	}
	if (text.len == 0) {
		dv_buf_free(&text);
		return;
	}
	clipboard_set(s, w, text.data, text.len);
	dv_buf_free(&text);
}

/* The Unicode picker's own key handling. */
static int
uni_key(struct dv_session *s, const char *arg)
{
	int	gc, gr, cnt = dv_uni_count(s->uni_cat);

	dv_uni_grid(s->desktop.cols, s->desktop.rows, &gc, &gr);
	if (strcmp(arg, "menu") == 0 || strcmp(arg, "esc") == 0) {
		s->uni_open = 0;
	} else if (strcmp(arg, "left") == 0) {
		if (s->uni_sel > 0)
			s->uni_sel--;
	} else if (strcmp(arg, "right") == 0) {
		if (s->uni_sel + 1 < cnt)
			s->uni_sel++;
	} else if (strcmp(arg, "up") == 0) {
		if (s->uni_sel - gc >= 0)
			s->uni_sel -= gc;
	} else if (strcmp(arg, "down") == 0) {
		if (s->uni_sel + gc < cnt)
			s->uni_sel += gc;
	} else if (strcmp(arg, "tab") == 0) {
		s->uni_cat = (s->uni_cat + 1) % dv_uni_cats();
		s->uni_sel = 0;
	} else if (strcmp(arg, "enter") == 0) {
		uni_insert(s, dv_uni_char(s->uni_cat, s->uni_sel));
	}
	redraw(s);
	return (1);
}

static int
split_argv(unsigned char *payload, size_t len, char ***argvp, int *argcp)
{
	size_t	 i;
	int	 n = 0, k = 0;
	char	**av;

	if (len == 0)
		return (-1);
	for (i = 0; i < len; i++) {
		if (payload[i] == '\0')
			n++;
	}
	if (payload[len - 1] != '\0')
		return (-1);
	av = dv_allocn<char *>((size_t)n + 1);
	av[k++] = (char *)payload;
	for (i = 0; i + 1 < len; i++) {
		if (payload[i] == '\0')
			av[k++] = (char *)payload + i + 1;
	}
	if (av[n - 1][0] == '\0')
		n--;
	av[n] = NULL;
	*argvp = av;
	*argcp = n;
	return (0);
}

static int
send_reply(struct dv_peer *p, int status, const char *text)
{
	size_t		 n = text ? strlen(text) : 0;
	unsigned char	*buf = dv_allocn<unsigned char>(4 + n);

	buf[0] = (unsigned char)((status >> 24) & 0xff);
	buf[1] = (unsigned char)((status >> 16) & 0xff);
	buf[2] = (unsigned char)((status >> 8) & 0xff);
	buf[3] = (unsigned char)(status & 0xff);
	if (n != 0)
		memcpy(buf + 4, text, n);
	dv_conn_send(&p->conn, DV_MSG_REPLY, buf, 4 + n);
	free(buf);
	return (status);
}

static struct dv_size
size_from_args(int argc, char **argv, struct dv_size ident)
{
	int	i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-x") == 0 && i + 1 < argc)
			ident.cols = (unsigned)strtoul(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "-y") == 0 && i + 1 < argc)
			ident.rows = (unsigned)strtoul(argv[++i], NULL, 10);
	}
	if (ident.cols < 1)
		ident.cols = 80;
	if (ident.rows < 1)
		ident.rows = 24;
	if (ident.xpixel == 0)
		ident.xpixel = DV_DEFAULT_XPIXEL;
	if (ident.ypixel == 0)
		ident.ypixel = DV_DEFAULT_YPIXEL;
	return (ident);
}

static const char *
session_name_from_args(int argc, char **argv, const char *def)
{
	int	i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			return (argv[i + 1]);
	}
	return (def);
}

static int
cmd_is_detached(int argc, char **argv)
{
	int	i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0)
			return (1);
	}
	return (0);
}

static int
cmd_skip_to_exec(int argc, char **argv)
{
	int	i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0)
			return (i + 1);
		if (argv[i][0] != '-')
			return (i);
		if ((strcmp(argv[i], "-x") == 0 ||
		    strcmp(argv[i], "-y") == 0 ||
		    strcmp(argv[i], "-s") == 0) && i + 1 < argc)
			i++;
	}
	return (argc);
}

/*
 * send-keys, capture-window and pipe-window: a window addressed by
 * -s session and -w number (default: the focused window of the default
 * session). Options come first; what follows is the command's own.
 */
static int
window_command(struct dv_peer *p, int argc, char **argv)
{
	struct dv_session	*s = NULL;
	struct dv_window	*w = NULL;
	const char		*sname = NULL;
	unsigned		 wid = 0;
	int			 i = 1, literal = 0, history = 0;

	for (; i < argc; i++) {
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			sname = argv[++i];
		else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
			wid = (unsigned)strtoul(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "-l") == 0)
			literal = 1;
		else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
			i++;
			history = strcmp(argv[i], "-") == 0 ? -1 : atoi(argv[i]);
		} else if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		} else
			break;
	}
	s = sname != NULL ? dv_session_find(sname) : dv_session_default();
	if (s == NULL)
		return (send_reply(p, 1, "no session\n"));
	w = wid != 0 ? dv_window_find(s, wid) : s->desktop.focused;
	if (w == NULL)
		return (send_reply(p, 1, "no such window\n"));

	if (strcmp(argv[0], "send-keys") == 0) {
		struct dv_buf	b;
		int		k;

		memset(&b, 0, sizeof b);
		for (k = i; k < argc; k++) {
			const char	*t = argv[k];

			if (k > i)
				dv_buf_append(&b, " ", 1);
			if (literal) {
				dv_buf_append(&b, t, strlen(t));
				continue;
			}
			while (*t != '\0') {	/* C escapes: \n \r \t \e \a \b \f \v \0 \xHH \\ */
				unsigned char	c = (unsigned char)*t++;

				if (c != '\\' || *t == '\0') {
					dv_buf_append(&b, &c, 1);
					continue;
				}
				switch (*t++) {
				case 'n': c = '\n'; break;
				case 'r': c = '\r'; break;
				case 't': c = '\t'; break;
				case 'e': c = 0x1b; break;
				case 'a': c = 0x07; break;
				case 'b': c = 0x08; break;
				case 'f': c = 0x0c; break;
				case 'v': c = 0x0b; break;
				case '0': c = 0; break;
				case '\\': c = '\\'; break;
				case 'x': {
					char	h[3];

					h[0] = t[0];
					h[1] = t[0] ? t[1] : '\0';
					h[2] = '\0';

					if (isxdigit((unsigned char)h[0]) && isxdigit((unsigned char)h[1])) {
						c = (unsigned char)strtoul(h, NULL, 16);
						t += 2;
					} else
						c = 'x';
					break;
				}
				default:
					c = (unsigned char)t[-1];
					break;
				}
				dv_buf_append(&b, &c, 1);
			}
		}
		if (w->pty_fd < 0) {
			dv_buf_free(&b);
			return (send_reply(p, 1, "window has no program\n"));
		}
		if (w->screen.sb_off > 0) {
			w->screen.sb_off = 0;
			redraw(s);
		}
		dv_window_pty_send(w, b.data, b.len);
		dv_buf_free(&b);
		return (send_reply(p, 0, ""));
	}

	if (strcmp(argv[0], "capture-window") == 0) {
		struct dv_buf		 b;
		struct dv_screen	*sc = &w->screen;
		unsigned		 y, from = 0, width;
		int			 rc;

		memset(&b, 0, sizeof b);
		if (history != 0 && !sc->alt_on) {
			if (history > 0 && (unsigned)history < sc->sb_len)
				from = sc->sb_len - (unsigned)history;
			for (y = from; y < sc->sb_len; y++) {
				const struct dv_cell	*row = dv_screen_history_row(sc, y, &width);

				if (row != NULL)
					dv_row_text(&b, row, width);
				dv_buf_append(&b, "\n", 1);
			}
		}
		for (y = 0; y < sc->rows; y++) {
			dv_row_text(&b, dv_screen_at(sc, 0, y), sc->cols);
			dv_buf_append(&b, "\n", 1);
		}
		dv_buf_append(&b, "", 1);
		rc = send_reply(p, 0, (const char *)b.data);
		dv_buf_free(&b);
		return (rc);
	}

	/* pipe-window: start (or replace) a log of the window's raw output,
	 * or stop it when no path is given. */
	if (w->pipe_fd >= 0) {
		close(w->pipe_fd);
		w->pipe_fd = -1;
	}
	if (i < argc) {
		w->pipe_fd = open(argv[i], O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
		if (w->pipe_fd < 0) {
			char	msg[DV_PATH_MAX + 64];

			snprintf(msg, sizeof msg, "%s: %s\n", argv[i], strerror(errno));
			return (send_reply(p, 1, msg));
		}
	}
	return (send_reply(p, 0, ""));
}

static char *
list_windows_text(struct dv_session *s)
{
	struct dv_window	*w;
	struct dv_buf		 b;
	char			 line[256];
	int			 n;

	memset(&b, 0, sizeof b);
	if (s == NULL) {
		dv_buf_append(&b, "", 1);
		return ((char *)b.data);
	}
	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		n = snprintf(line, sizeof line,
		    "%s:%u: %s (%ux%u) %d,%d z=%d%s%s\n",
		    s->name, w->id, w->title[0] ? w->title : w->cmd,
		    w->geom.cols, w->geom.rows,
		    w->geom.x, w->geom.y, w->z,
		    w->zoomed ? " zoomed" : "",
		    s->desktop.focused == w ? " focused" : "");
		if (n > 0 && w->hidden && (size_t)n < sizeof line) {
			n--;			/* replace the newline */
			n += snprintf(line + n, sizeof line - (size_t)n, " hidden\n");
		}
		if (n > 0)
			dv_buf_append(&b, line, (size_t)n);
	}
	dv_buf_append(&b, "", 1);
	return ((char *)b.data);
}

static void
begin_place(struct dv_session *s, int mode)
{
	struct dv_window	*w = s->desktop.focused;

	if (w == NULL)
		return;
	if (w->zoomed) {
		dv_desktop_apply_unzoom(w);
		dv_window_set_pty_size(w);
	}
	s->mode_saved = w->geom;
	s->input_mode = mode;
	redraw(s);
}

static int
dir_from_word(const char *word, int *dx, int *dy)
{
	*dx = *dy = 0;
	if (strcmp(word, "up") == 0) {
		*dy = -1;
		return (1);
	}
	if (strcmp(word, "down") == 0) {
		*dy = 1;
		return (1);
	}
	if (strcmp(word, "left") == 0) {
		*dx = -1;
		return (1);
	}
	if (strcmp(word, "right") == 0) {
		*dx = 1;
		return (1);
	}
	return (0);
}

static void
nudge_or_grow(struct dv_session *s, int dx, int dy, int grow)
{
	struct dv_window	*w = s->desktop.focused;
	struct dv_desktop	*d;

	if (w == NULL)
		return;
	d = &s->desktop;
	if (grow)
		dv_geom_grow(&w->geom, dx, dy, d->cols, d->rows, w->framed);
	else
		dv_geom_nudge(&w->geom, dx, dy, d->cols, d->rows, w->framed);
	if (grow)
		dv_window_set_pty_size(w);
	redraw(s);
}

/* Keys while the switch-window list is up. */
static void
switch_key(struct dv_session *s, const char *arg)
{
	unsigned	ids[256], n = dv_switch_list(s, ids, 256);
	size_t		fl = strlen(s->switch_filter);

	if (strcmp(arg, "esc") == 0 || strcmp(arg, "menu") == 0) {
		s->switch_open = 0;
	} else if (strcmp(arg, "enter") == 0) {
		struct dv_window	*w = n > 0 && (unsigned)s->switch_sel < n ?
		    dv_window_find(s, ids[s->switch_sel]) : nullptr;

		s->switch_open = 0;
		if (w != nullptr) {
			w->hidden = 0;
			focus_window(s, w);
		}
	} else if (strcmp(arg, "up") == 0) {
		if (s->switch_sel > 0)
			s->switch_sel--;
	} else if (strcmp(arg, "down") == 0) {
		if (n > 0 && (unsigned)s->switch_sel + 1 < n)
			s->switch_sel++;
	} else if (strcmp(arg, "backspace") == 0) {
		if (fl > 0)
			s->switch_filter[fl - 1] = '\0';
		s->switch_sel = 0;
	} else if (strlen(arg) == 1 && fl + 1 < sizeof s->switch_filter) {
		s->switch_filter[fl] = arg[0];
		s->switch_filter[fl + 1] = '\0';
		s->switch_sel = 0;
	}
	redraw(s);
}

static
void
dv_menu_cmd(struct dv_session *s, const char *arg, int step, struct dv_peer *p)
{
	struct dv_window	*w = s->desktop.focused;
	struct dv_desktop	*d = &s->desktop;
	int			 dx, dy;
	char			 letter;
	unsigned		 id;

	if (arg == nullptr)
		arg = "";

	if (s->title_edit) {	/* a menu action ends title editing */
		s->title_edit = 0;
		redraw(s);
	}

	/* The help box closes on any key. */
	if (s->help_open) {
		s->help_open = 0;
		redraw(s);
		return;
	}

	/* A file transfer's yes/no box takes the key. */
	if (s->xfer_prompt) {
		char	f = arg[0];

		if (f == 'y' || f == 'Y')
			dv_xfer_answer(s, 1);
		else if (f == 'n' || f == 'N' || strcmp(arg, "esc") == 0)
			dv_xfer_answer(s, 0);
		redraw(s);
		return;
	}

	/* Quit confirmation swallows input until answered. */
	if (s->quit_confirm) {
		char	f = arg[0];

		if (f == 'y' || f == 'Y') {
			struct dv_peer	*q;

			TAILQ_FOREACH(q, &dv_srv->peers, entry)
				dv_conn_send(&q->conn, DV_MSG_EXIT, NULL, 0);
			dv_quit = 1;
		} else {
			s->quit_confirm = 0;
			redraw(s);
		}
		return;
	}

	/* Copy mode takes every key. */
	if (s->copy_open) {
		copy_word(s, arg);
		return;
	}

	/* The switch-window list, if open, takes the keys. */
	if (s->switch_open) {
		switch_key(s, arg);
		return;
	}

	/* The Unicode picker, if open, takes the keys. */
	if (s->uni_open) {
		uni_key(s, arg);
		return;
	}

	/* The input-method chooser: a letter picks the method. */
	if (s->im_choose) {
		char	f = arg[0];

		if (strcmp(arg, "esc") == 0 || strcmp(arg, "menu") == 0)
			;			/* just close */
		else if (f == 'o')
			s->im.method = DV_IM_OFF;
		else if (f == 'r')
			s->im.method = DV_IM_ROMAJI;
		else if (f == 'u')
			s->im.method = DV_IM_USINTL;
		else if (f == 'p')
			s->im.method = DV_IM_PINYIN;
		else if (f == 'z')
			s->im.method = DV_IM_ZHUYIN;
		else if (f == 'w')
			s->im.method = DV_IM_WUBI;
		dv_im_reset(&s->im);
		s->im_choose = 0;
		redraw(s);
		return;
	}

	if (strcmp(arg, "menu") == 0) {
		if (s->menu_open)
			s->menu_open = 0;
		else {
			if (s->input_mode != DV_IN_NORMAL)
				dv_menu_cmd(s, "esc", 1, p);
			s->menu_open = 1;
		}
		redraw(s);
		return;
	}
	if (dir_from_word(arg, &dx, &dy)) {
		dx *= step;
		dy *= step;
		if (s->input_mode == DV_IN_MOVE)
			nudge_or_grow(s, dx, dy, 0);
		else if (s->input_mode == DV_IN_RESIZE)
			nudge_or_grow(s, dx, dy, 1);
		return;
	}
	if (strcmp(arg, "enter") == 0) {
		s->input_mode = DV_IN_NORMAL;
		redraw(s);
		return;
	}
	if (strcmp(arg, "esc") == 0) {
		if ((s->input_mode == DV_IN_MOVE ||
		    s->input_mode == DV_IN_RESIZE) && w != nullptr) {
			w->geom = s->mode_saved;
			dv_window_set_pty_size(w);
		}
		s->input_mode = DV_IN_NORMAL;
		s->menu_open = 0;
		redraw(s);
		return;
	}

	letter = arg[0];
	if (letter >= 'A' && letter <= 'Z')
		letter = (char)(letter - 'A' + 'a');
	if (letter == 0)
		return;
	/* A letter is the answer to the menu; it goes away. */
	if (s->menu_open) {
		s->menu_open = 0;
		redraw(s);
	}

	if (s->input_mode == DV_IN_REARRANGE ||
	    s->input_mode == DV_IN_POSITION) {
		if (letter == 'm') {
			begin_place(s, DV_IN_MOVE);
			return;
		}
		if (letter == 'r') {
			begin_place(s, DV_IN_RESIZE);
			return;
		}
		if (letter == 'p') {
			s->input_mode = DV_IN_POSITION;
			redraw(s);
			return;
		}
		if (letter == 'h' && w != nullptr) {
			w->hidden = !w->hidden;
			s->input_mode = DV_IN_NORMAL;
			redraw(s);
			return;
		}
		if (letter >= '1' && letter <= '9' && w != nullptr) {
			if (w->zoomed) {
				dv_desktop_apply_unzoom(w);
			}
			dv_snap_geom(d->cols, d->rows, letter - '0', 1,
			    &w->geom);
			w->framed = 1;
			w->zoomed = 0;
			dv_window_set_pty_size(w);
			s->input_mode = DV_IN_NORMAL;
			redraw(s);
			return;
		}
		if (letter == 0)
			return;
		/* unknown rearrange key: drop back to normal and fall through */
		s->input_mode = DV_IN_NORMAL;
	}

	switch (letter) {
	case 'o':
		if (dv_window_open(s, NULL, NULL, (int)d->next_snap,
		    p != nullptr ? p->cwd : nullptr) != NULL)
			redraw(s);
		break;
	case 'c':
		if (w != nullptr) {
			dv_window_close(w);
			redraw(s);
		}
		break;
	case 'z':
		if (w == NULL)
			break;
		if (w->zoomed)
			dv_desktop_apply_unzoom(w);
		else
			dv_desktop_apply_zoom(w);
		dv_window_set_pty_size(w);
		redraw(s);
		break;
	case 'r':
		s->input_mode = DV_IN_REARRANGE;
		redraw(s);
		break;
	case 'u':
		s->uni_open = 1;
		s->uni_cat = 0;
		s->uni_sel = 0;
		redraw(s);
		break;
	case 'm':	/* copy mode on the focused window */
		if (w != nullptr && w->pty_fd >= 0) {
			s->copy_open = 1;
			s->copy_win = w->id;
			s->copy_sel = 0;
			s->copy_searching = 0;
			s->copy_x = (int)w->screen.cc;
			s->copy_y = (w->screen.alt_on ? 0 : (long)w->screen.sb_len) +
			    (long)w->screen.cr;
			copy_show(s, w);
			redraw(s);
		}
		break;
	case 't':	/* transfer: paste the mark into the focused window */
		if (w != nullptr && w->pty_fd >= 0 && s->mark.len > 0) {
			s->paste.len = 0;
			dv_buf_append(&s->paste, s->mark.data, s->mark.len);
			flush_paste(s, w);
		}
		break;
	case 'h':	/* hide the focused window; the next one on top takes focus */
		if (w != nullptr) {
			struct dv_window	*it, *next = nullptr;

			w->hidden = 1;
			TAILQ_FOREACH(it, &d->windows, entry)
				if (!it->hidden && !it->aside &&
				    (next == nullptr || it->z > next->z))
					next = it;
			d->focused = next;
			if (next != nullptr) {
				dv_window_apply_pointer(next);
				dv_window_apply_cursor_color(next);
				flush_host_out(next);
			}
			redraw(s);
		}
		break;
	case 's':	/* the switch-window list */
		s->switch_open = 1;
		s->switch_sel = 0;
		s->switch_filter[0] = '\0';
		redraw(s);
		break;
	case 'b':	/* broadcast: typing goes to every window */
		s->broadcast = !s->broadcast;
		redraw(s);
		break;
	case 'i':
		s->im_choose = 1;
		redraw(s);
		break;
	case 'd': {	/* detach, leaving the session running */
		/*
		 * From the key: this terminal. From a script (a peer that is
		 * not a display), every terminal attached to the session --
		 * the script cannot mean itself.
		 */
		struct dv_peer	*q, *q1;
		int		 any = 0;

		TAILQ_FOREACH_SAFE(q, &dv_srv->peers, entry, q1) {
			if (!q->display || q->session != s)
				continue;
			if (p != nullptr && p->display && q != p)
				continue;
			dv_conn_send(&q->conn, DV_MSG_EXIT, NULL, 0);
			q->display = 0;
			q->session = NULL;
			any = 1;
		}
		if (any)
			refresh_gfx(s);
		break;
	}
	case '?':	/* help: the keys, the mouse, the Ghostty setup */
		s->help_open = 1;
		redraw(s);
		break;
	case 'q':	/* quit goblin-view: ask first, then tear the server down */
		s->quit_confirm = 1;
		redraw(s);
		break;
	case '1': case '2': case '3': case '4': case '5':
	case '6': case '7': case '8': case '9':
		id = (unsigned)(letter - '0');
		w = dv_window_find(s, id);
		if (w != nullptr) {
			w->hidden = 0;		/* a hidden window comes back */
			focus_window(s, w);
		}
		break;
	default:
		break;
	}
}

static int
handle_command(struct dv_peer *p, unsigned char *payload, size_t len)
{
	char		**argv = NULL;
	int		 argc = 0, exec_i, detached;
	const char	*sname;
	struct dv_session *s;
	struct dv_size	 sz;
	char		*text;
	char		*const *cmdv;
	int		 rc = 0;

	if (split_argv(payload, len, &argv, &argc) != 0 || argc < 1) {
		free(argv);
		return (send_reply(p, 1, "empty command\n"));
	}

	if (strcmp(argv[0], "kill-server") == 0) {
		send_reply(p, 0, "");
		dv_quit = 1;
		free(argv);
		return (0);
	}

	if (strcmp(argv[0], "list-sessions") == 0) {
		struct dv_buf	 b;
		char		 line[128];

		memset(&b, 0, sizeof b);
		TAILQ_FOREACH(s, &dv_srv->sessions, entry) {
			int n = snprintf(line, sizeof line, "%s: %ux%u\n",
			    s->name, s->desktop.cols, s->desktop.rows);
			if (n > 0)
				dv_buf_append(&b, line, (size_t)n);
		}
		dv_buf_append(&b, "", 1);
		rc = send_reply(p, 0, b.data ? (char *)b.data : "");
		dv_buf_free(&b);
		free(argv);
		return (rc);
	}

	if (strcmp(argv[0], "list-clients") == 0) {
		struct dv_buf	 b;
		struct dv_peer	*q;

		memset(&b, 0, sizeof b);
		TAILQ_FOREACH(q, &dv_srv->peers, entry) {
			char	line[320];
			int	n;

			if (!q->display || !q->identified)
				continue;
			n = snprintf(line, sizeof line,
			    "%s: %ux%u term %s host %s%s%s%s\n",
			    q->session != NULL ? q->session->name : "-",
			    q->tty.cols, q->tty.rows, q->term,
			    q->host[0] ? q->host : "?",
			    (q->features & DV_IDENT_KITTY_GFX) ? " kitty-gfx" : "",
			    (q->features & DV_IDENT_SIXEL) ? " sixel" : "",
			    (q->features & DV_IDENT_CELLSIZE) ? " cellsize" : "");
			if (n > 0)
				dv_buf_append(&b, line, (size_t)n);
		}
		dv_buf_append(&b, "", 1);
		free(argv);
		rc = send_reply(p, 0, (const char *)b.data);
		dv_buf_free(&b);
		return (rc);
	}

	if (strcmp(argv[0], "upgrade") == 0) {
		char		 exe[DV_PATH_MAX], state[DV_PATH_MAX], msg[DV_PATH_MAX + 80];
		const char	*path = argc > 1 ? argv[1] : nullptr, *tmpdir = getenv("TMPDIR");
		struct dv_peer	*q;
		char		*const av[] = { const_cast<char *>("goblin-view"),
		    const_cast<char *>("-S"), dv_srv->sock_path,
		    const_cast<char *>(dv_srv->debug ? "-D" : "-v"),
		    const_cast<char *>("takeover"), state, NULL };

		if (path == nullptr) {
			if (!dv_exe_path(exe, sizeof exe)) {
				free(argv);
				return (send_reply(p, 1, "cannot tell my own path; give one\n"));
			}
			path = exe;
		}
		if (access(path, X_OK) != 0) {
			snprintf(msg, sizeof msg, "%s: %s\n", path, strerror(errno));
			free(argv);
			return (send_reply(p, 1, msg));
		}
		snprintf(state, sizeof state, "%s/goblin-view-%ld.state",
		    tmpdir != NULL && tmpdir[0] == '/' ? tmpdir : "/tmp", (long)getpid());
		/* What is queued for a client goes out first, so nothing is
		 * mid-message when the new program takes the sockets over. */
		TAILQ_FOREACH(q, &dv_srv->peers, entry) {
			int	tries = 20;

			while (q->conn.out.len > 0 && tries-- > 0) {
				struct pollfd	pf = { q->fd, POLLOUT, 0 };

				if (poll(&pf, 1, 25) <= 0 || dv_conn_flush(&q->conn) < 0)
					break;
			}
		}
		TAILQ_FOREACH(q, &dv_srv->peers, entry) pointer_cancel_peer(q);
		if (!dv_state_write(state, p)) {
			snprintf(msg, sizeof msg, "cannot write the state to %s: %s\n", state,
			    strerror(errno));
			free(argv);
			return (send_reply(p, 1, msg));
		}
		dv_log("upgrade: exec %s with state %s", path, state);
		dv_state_inherit_fds(1);
		execv(path, av);
		/* still here: it failed; carry on as we were */
		dv_state_inherit_fds(0);
		unlink(state);
		snprintf(msg, sizeof msg, "exec %s: %s\n", path, strerror(errno));
		free(argv);
		return (send_reply(p, 1, msg));
	}

	if (strcmp(argv[0], "save") == 0 || strcmp(argv[0], "restore") == 0) {
		struct dv_buf	report;
		const char	*file = dv_saved_path();
		int		 i, ok;

		for (i = 1; i + 1 < argc; i++)
			if (strcmp(argv[i], "-f") == 0)
				file = argv[i + 1];
		memset(&report, 0, sizeof report);
		if (strcmp(argv[0], "save") == 0)
			ok = dv_sessions_save(file, &report);
		else
			ok = dv_sessions_restore(file, &report);
		{
			int	is_restore = strcmp(argv[0], "restore") == 0;

			dv_buf_append(&report, "", 1);
			rc = send_reply(p, ok ? 0 : 1, (const char *)report.data);
			dv_buf_free(&report);
			free(argv);
			if (is_restore)
				TAILQ_FOREACH(s, &dv_srv->sessions, entry)
					redraw(s);
		}
		return (rc);
	}

	if (strcmp(argv[0], "send-keys") == 0 || strcmp(argv[0], "capture-window") == 0 ||
	    strcmp(argv[0], "pipe-window") == 0) {
		rc = window_command(p, argc, argv);
		free(argv);
		return (rc);
	}

	if (strcmp(argv[0], "list-windows") == 0) {
		sname = session_name_from_args(argc, argv, NULL);
		s = sname ? dv_session_find(sname) : dv_session_default();
		text = list_windows_text(s);
		rc = send_reply(p, s == NULL ? 1 : 0, text);
		free(text);
		free(argv);
		return (rc);
	}

	if (strcmp(argv[0], "kill-session") == 0) {
		sname = session_name_from_args(argc, argv, "0");
		if (argc > 1 && argv[1][0] != '-')
			sname = argv[1];
		s = dv_session_find(sname);
		if (s == NULL) {
			free(argv);
			return (send_reply(p, 1, "no session\n"));
		}
		dv_session_destroy(s);
		free(argv);
		return (send_reply(p, 0, ""));
	}

	if (strcmp(argv[0], "new-session") == 0) {
		sname = session_name_from_args(argc, argv, "0");
		if (dv_session_find(sname) != NULL) {
			free(argv);
			return (send_reply(p, 1, "session exists\n"));
		}
		sz = size_from_args(argc, argv, p->tty);
		s = dv_session_create(sname, sz);
		exec_i = cmd_skip_to_exec(argc, argv);
		cmdv = exec_i < argc ? argv + exec_i : NULL;
		if (dv_window_open(s, NULL, cmdv, 0, p->cwd) == NULL) {
			dv_session_destroy(s);
			free(argv);
			return (send_reply(p, 1, "failed to spawn\n"));
		}
		detached = cmd_is_detached(argc, argv);
		if (!detached && (p->identified && p->tty.cols)) {
			attach_peer(p, s);
			free(argv);
			return (0);	/* no reply; stream */
		}
		free(argv);
		return (send_reply(p, 0, ""));
	}

	if (strcmp(argv[0], "mouse") == 0) {
		unsigned	mx, my, code;
		int		press, i, np2 = 0;
		char		*pos[5];

		sname = session_name_from_args(argc, argv, NULL);
		s = sname != NULL ? dv_session_find(sname) :
		    (p->session != NULL ? p->session : dv_session_default());
		for (i = 1; i < argc && np2 < 5; i++) {
			if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
				i++;
				continue;
			}
			pos[np2++] = argv[i];
		}
		if (s == NULL || np2 < 3) {
			free(argv);
			return (send_reply(p, 1, "usage: mouse [-s name] "
			    "press|drag|release x y\n"));
		}
		mx = (unsigned)strtoul(pos[1], NULL, 10);
		my = (unsigned)strtoul(pos[2], NULL, 10);
		if (strcmp(pos[0], "drag") == 0) {
			code = 32;
			press = 1;
		} else if (strcmp(pos[0], "release") == 0) {
			code = 0;
			press = 0;
		} else if (strcmp(pos[0], "wheelup") == 0) {
			code = 64;
			press = 1;
		} else if (strcmp(pos[0], "wheeldown") == 0) {
			code = 65;
			press = 1;
		} else {		/* press, optional button in pos[3] */
			code = np2 > 3 ? (unsigned)strtoul(pos[3], NULL, 10) : 0;
			press = 1;
		}
		handle_mouse(p, s, mx, my, code, press);
		free(argv);
		if (!p->display)
			return (send_reply(p, 0, ""));
		return (0);
	}

	if (strcmp(argv[0], "new-window") == 0) {
		int	ex;

		sname = session_name_from_args(argc, argv, NULL);
		s = sname ? dv_session_find(sname) : dv_session_default();
		if (s == NULL) {
			free(argv);
			return (send_reply(p, 1, "no session\n"));
		}
		ex = cmd_skip_to_exec(argc, argv);
		cmdv = ex < argc ? argv + ex : NULL;
		if (dv_window_open(s, NULL, cmdv, 0, p->cwd) == NULL) {
			free(argv);
			return (send_reply(p, 1, "failed to spawn\n"));
		}
		redraw(s);
		free(argv);
		return (send_reply(p, 0, ""));
	}

	if (strcmp(argv[0], "refresh") == 0) {
		p->view.full = 1;
		if (p->session != NULL)
			redraw(p->session);
		free(argv);
		if (!p->display)
			return (send_reply(p, 0, ""));
		return (0);
	}

	if (strcmp(argv[0], "menu") == 0) {
		int		 was_display = p->display;
		int		 step = 1, i = 1;
		const char	*key = "", *mname = NULL;

		if (argc > 2 && strcmp(argv[1], "-s") == 0) {	/* menu -s name key [5] */
			mname = argv[2];
			i = 3;
		}
		if (mname != NULL)
			s = dv_session_find(mname);
		else
			s = p->session != NULL ? p->session : dv_session_default();
		if (s == NULL) {
			free(argv);
			if (!was_display)
				return (send_reply(p, 1, "no session\n"));
			return (0);
		}
		if (i < argc)
			key = argv[i++];
		if (i < argc && argv[i][0] == '5')
			step = 5;
		dv_menu_cmd(s, key, step, p);
		free(argv);
		if (!was_display)
			return (send_reply(p, 0, ""));
		return (0);
	}

	if (strcmp(argv[0], "attach") == 0) {
		sname = session_name_from_args(argc, argv, NULL);
		if (argc > 1 && argv[1][0] != '-')
			sname = argv[1];
		s = sname ? dv_session_find(sname) : dv_session_default();
		if (s == NULL) {
			sz = p->tty;
			if (sz.cols < 1)
				sz.cols = 80;
			if (sz.rows < 1)
				sz.rows = 24;
			if (sz.xpixel == 0)
				sz.xpixel = DV_DEFAULT_XPIXEL;
			if (sz.ypixel == 0)
				sz.ypixel = DV_DEFAULT_YPIXEL;
			s = dv_session_create(sname ? sname : "0", sz);
			if (dv_window_open(s, NULL, NULL, 0, p->cwd) == NULL) {
				dv_session_destroy(s);
				free(argv);
				return (send_reply(p, 1, "failed to spawn\n"));
			}
		}
		attach_peer(p, s);
		free(argv);
		return (0);
	}

	rc = send_reply(p, 1, "unknown command\n");
	free(argv);
	return (rc);
}

/* Who we are, so a client of another build can tell at once. (The test
 * knobs make this server look like one too old to say, or another one.) */
static void
send_hello(struct dv_peer *p)
{
	unsigned char	hello[2 + sizeof DV_VERSION];
	unsigned	proto = DV_PROTOCOL;
	const char	*tp = getenv("GOBLIN_VIEW_TEST_PROTO");

	if (getenv("GOBLIN_VIEW_TEST_NOHELLO") != NULL)
		return;
	if (tp != NULL && tp[0] != '\0')
		proto = (unsigned)strtoul(tp, NULL, 10);
	hello[0] = (unsigned char)(proto >> 8);
	hello[1] = (unsigned char)(proto & 0xff);
	memcpy(hello + 2, DV_VERSION, sizeof DV_VERSION - 1);
	dv_conn_send(&p->conn, DV_MSG_HELLO, hello, 2 + sizeof DV_VERSION - 1);
}

static void
handle_identify(struct dv_peer *p, unsigned char *payload, size_t len)
{
	const unsigned char	*q = payload, *end = payload + len;
	int			 err = 0;
	unsigned		 tlen;

	p->tty.cols = get_u16(&q, end, &err);
	p->tty.rows = get_u16(&q, end, &err);
	p->tty.xpixel = get_u16(&q, end, &err);
	p->tty.ypixel = get_u16(&q, end, &err);
	if (err || q >= end)
		return;
	p->features = *q++;
	if (q >= end)
		return;
	tlen = *q++;
	if (q + tlen > end)
		tlen = (unsigned)(end - q);
	if (tlen >= sizeof p->term)
		tlen = sizeof p->term - 1;
	memcpy(p->term, q, tlen);
	p->term[tlen] = '\0';
	q += tlen;

	/* cwd is optional: an older client simply does not send one. */
	p->cwd[0] = '\0';
	if (q + 2 <= end) {
		unsigned	clen = get_u16(&q, end, &err);

		if (!err) {
			if (q + clen > end)
				clen = (unsigned)(end - q);
			if (clen >= sizeof p->cwd)
				clen = sizeof p->cwd - 1;
			memcpy(p->cwd, q, clen);
			p->cwd[clen] = '\0';
			q += clen;
		}
	}
	/* So is the terminal's name (XTVERSION). */
	p->host[0] = '\0';
	if (q + 2 <= end) {
		unsigned	hl = get_u16(&q, end, &err);

		if (!err) {
			if (q + hl > end)
				hl = (unsigned)(end - q);
			if (hl >= sizeof p->host)
				hl = sizeof p->host - 1;
			memcpy(p->host, q, hl);
			p->host[hl] = '\0';
			q += hl;
		}
	}
	if (q < end) p->pointer_caps = *q++ & 7;
	p->silkie_blob[0] = '\0';
	if (q + 2 <= end) {
		unsigned	bl = get_u16(&q, end, &err);

		if (!err) {
			if (q + bl > end)
				bl = (unsigned)(end - q);
			if (bl >= sizeof p->silkie_blob)
				bl = (unsigned)sizeof p->silkie_blob - 1;
			memcpy(p->silkie_blob, q, bl);
			p->silkie_blob[bl] = '\0';
		}
	}
	if (p->session != nullptr)
		dv_silkie_attach(p);
	p->identified = 1;
	dv_log("identify %ux%u cell %ux%u term %s host %s cwd %s", p->tty.cols,
	    p->tty.rows, p->tty.xpixel, p->tty.ypixel, p->term, p->host, p->cwd);

	send_hello(p);
}

static void
handle_resize(struct dv_peer *p, unsigned char *payload, size_t len)
{
	pointer_cancel_peer(p);
	const unsigned char	*q = payload, *end = payload + len;
	int			 err = 0;

	p->tty.cols = get_u16(&q, end, &err);
	p->tty.rows = get_u16(&q, end, &err);
	p->tty.xpixel = get_u16(&q, end, &err);
	p->tty.ypixel = get_u16(&q, end, &err);
	if (err)
		return;
	if (p->session != NULL && p->display)
		recalculate_size(p->session);
}

/* Is this payload one keystroke of the given shape? */
static int
key_is_esc(const unsigned char *d, size_t n)
{
	return (n == 1 && d[0] == 0x1b);
}

static int
key_is_enter(const unsigned char *d, size_t n)
{
	return (n == 1 && (d[0] == '\r' || d[0] == '\n'));
}

static const char *
key_arrow(const unsigned char *d, size_t n, int *step)
{
	unsigned char	fin;

	*step = 1;
	if (n < 3 || d[0] != 0x1b || (d[1] != '[' && d[1] != 'O'))
		return (nullptr);
	fin = d[n - 1];
	if (n >= 6 && d[3] == ';')
		*step = (d[4] == '2') ? 5 : 1;
	switch (fin) {
	case 'A':
		return ("up");
	case 'B':
		return ("down");
	case 'C':
		return ("right");
	case 'D':
		return ("left");
	default:
		return (nullptr);
	}
}

static int
key_letter(const unsigned char *d, size_t n)
{
	unsigned char	c;

	if (n == 0)
		return (0);
	c = d[0];
	if (c >= 'A' && c <= 'Z')
		return (c - 'A' + 'a');
	if ((c >= 'a' && c <= 'z') || (c >= '1' && c <= '9') || c == '?')
		return (c);
	return (0);
}

/*
 * Keys from a display client. While the menu or a placement mode is
 * up they answer it; otherwise they go to the focused window's pty,
 * re-encoded for DECCKM if the application asked for it.
 */
/*
 * A drag-and-drop (or Ctrl-V) arrives from the host as a bracketed paste,
 * ESC[200~ ... ESC[201~, because the client enables DECSET 2004 on the
 * host. It has no drop coordinates, so it goes to the focused window --
 * the one taking input. We translate it to that window's own paste mode:
 * wrapped if the program enabled bracketed paste, the bare content if not
 * (as a real terminal delivers a paste to a program that did not ask for
 * the wrappers). This keeps the markers from leaking into such programs.
 */
static const unsigned char PASTE_START[6] = { 0x1b, '[', '2', '0', '0', '~' };
static const unsigned char PASTE_END[6]   = { 0x1b, '[', '2', '0', '1', '~' };

static void
flush_paste(struct dv_session *s, struct dv_window *w)
{
	if (w->pty_fd < 0)
		return;
	if (w->screen.bracketed)
		dv_window_pty_send(w, PASTE_START, sizeof PASTE_START);
	if (s->paste.len > 0)
		dv_window_pty_send(w, s->paste.data, s->paste.len);
	if (w->screen.bracketed)
		dv_window_pty_send(w, PASTE_END, sizeof PASTE_END);
	s->paste.len = 0;
}

/*
 * Forward host input to the focused window, pulling any bracketed paste
 * out of the byte stream and re-emitting it per the window's paste mode.
 * Bytes outside a paste (ordinary typing) are written through unchanged.
 * The paste content is capped so a pathological stream cannot grow it
 * without bound. Returns nothing; consumes the whole payload.
 */
static void
forward_input(struct dv_session *s, struct dv_window *w,
    const unsigned char *buf, size_t len)
{
	size_t	i = 0;

	if (w->pty_fd < 0)
		return;
	while (i < len) {
		if (!s->in_paste) {
			const unsigned char	*at = (const unsigned char *)
			    memmem(buf + i, len - i, PASTE_START,
			    sizeof PASTE_START);

			if (at == NULL) {		/* no paste: plain typing */
				dv_window_pty_send(w, buf + i, len - i);
				return;
			}
			if (at > buf + i)
				dv_window_pty_send(w, buf + i,
				    (size_t)(at - (buf + i)));
			i = (size_t)(at - buf) + sizeof PASTE_START;
			s->in_paste = 1;
			s->paste.len = 0;
		} else {
			const unsigned char	*at = (const unsigned char *)
			    memmem(buf + i, len - i, PASTE_END,
			    sizeof PASTE_END);
			size_t			 take = (at != NULL) ?
			    (size_t)(at - (buf + i)) : len - i;

			if (s->paste.len + take <= 4u * 1024 * 1024)
				dv_buf_append(&s->paste, buf + i, take);
			if (at == NULL)
				return;		/* more paste in a later chunk */
			i += take + sizeof PASTE_END;
			s->in_paste = 0;
			flush_paste(s, w);
		}
	}
}

static void
handle_stdin_msg(struct dv_peer *p, unsigned char *payload, size_t len)
{
	struct dv_window	*w;
	struct dv_session	*s;
	const char		*word;
	int			 step, letter;
	char			 one[2];

	if (p->session == NULL || !p->display)
		return;
	s = p->session;
	w = s->desktop.focused;
	if (len == 0)
		return;

	/* Inline title editing takes every key until Enter or Esc. */
	if (s->title_edit) {
		unsigned char	b = payload[0];

		if (b == 0x1b && len == 1) {		/* Esc: cancel */
			s->title_edit = 0;
			redraw(s);
			return;
		}
		if (b == '\r' || b == '\n') {		/* Enter: keep the name */
			struct dv_window	*tw = dv_window_find(s, s->title_win);

			if (tw != nullptr) {
				s->title_ed[s->title_edlen] = '\0';
				dv_strlcpy(tw->title, s->title_ed,
				    sizeof tw->title);
				tw->title_locked = 1;	/* app OSC no longer wins */
			}
			s->title_edit = 0;
			redraw(s);
			return;
		}
		if (b == 0x7f || b == 0x08) {		/* backspace one UTF-8 char */
			while (s->title_edlen > 0 &&
			    (s->title_ed[s->title_edlen - 1] & 0xc0) == 0x80)
				s->title_edlen--;
			if (s->title_edlen > 0)
				s->title_edlen--;
			s->title_ed[s->title_edlen] = '\0';
			redraw(s);
			return;
		}
		/* Append typed bytes (printable ASCII or any UTF-8). */
		{
			size_t	i;

			for (i = 0; i < len; i++) {
				unsigned char	c = payload[i];

				if (c < 0x20 || c == 0x7f)
					continue;
				if (s->title_edlen < (int)sizeof s->title_ed - 1)
					s->title_ed[s->title_edlen++] = (char)c;
			}
			s->title_ed[s->title_edlen] = '\0';
		}
		redraw(s);
		return;
	}

	if (s->copy_open) {			/* copy mode: every key is its own */
		copy_key(s, payload, len);
		return;
	}

	if (s->menu_open || s->uni_open || s->quit_confirm || s->im_choose ||
	    s->help_open || s->xfer_prompt || s->switch_open ||
	    s->input_mode == DV_IN_REARRANGE || s->input_mode == DV_IN_POSITION) {
		if (len == 1 && (payload[0] == 0x7f || payload[0] == 0x08)) {
			dv_menu_cmd(s, "backspace", 1, p);
			return;
		}
		/*
		 * A paste is not a decision: pasted text cannot answer a box
		 * (approve a file, quit), so everything between the host's
		 * paste markers is swallowed while one is up.
		 */
		if (s->in_paste || memmem(payload, len, PASTE_START,
		    sizeof PASTE_START) != NULL) {
			if (memmem(payload, len, PASTE_END, sizeof PASTE_END) != NULL)
				s->in_paste = 0;
			else
				s->in_paste = 1;
			return;
		}
		if (key_is_esc(payload, len)) {
			dv_menu_cmd(s, "esc", 1, p);
			return;
		}
		if (key_is_enter(payload, len)) {
			dv_menu_cmd(s, "enter", 1, p);
			return;
		}
		if (len == 1 && payload[0] == '\t') {
			dv_menu_cmd(s, "tab", 1, p);
			return;
		}
		if (len == 1 && payload[0] == ' ') {
			dv_menu_cmd(s, "enter", 1, p);	/* space inserts */
			return;
		}
		word = key_arrow(payload, len, &step);
		if (word != nullptr) {
			dv_menu_cmd(s, word, step, p);
			return;
		}
		letter = key_letter(payload, len);
		if (letter != 0) {
			one[0] = (char)letter;
			one[1] = '\0';
			dv_menu_cmd(s, one, 1, p);
		}
		return;		/* swallow other keys while it is up */
	}

	if (s->input_mode == DV_IN_MOVE || s->input_mode == DV_IN_RESIZE) {
		if (key_is_enter(payload, len)) {
			dv_menu_cmd(s, "enter", 1, p);
			return;
		}
		if (key_is_esc(payload, len)) {
			dv_menu_cmd(s, "esc", 1, p);
			return;
		}
		word = key_arrow(payload, len, &step);
		if (word != nullptr)
			dv_menu_cmd(s, word, step, p);
		return;		/* swallow other keys while placing */
	}

	if (w == NULL || w->pty_fd < 0)
		return;

	if (w->screen.sb_off > 0) {	/* a keypress returns to the live bottom */
		w->screen.sb_off = 0;
		redraw(s);
	}

	/*
	 * An active input method composes ordinary typing into Unicode
	 * before it reaches the program. A run of typed bytes is fed key by
	 * key; a byte the method does not consume passes through in place.
	 * An escape sequence (arrows, function keys) is left untouched.
	 */
	if (s->im.method != DV_IM_OFF && len > 0) {
		size_t		i;
		int		all_typed = 1;

		for (i = 0; i < len; i++) {
			unsigned char	b = payload[i];

			if (!((b >= 0x20 && b < 0x7f) || b == 0x7f || b == 0x08 ||
			    b == 0x1b || b == '\r' || b == '\n')) {
				all_typed = 0;
				break;
			}
		}
		if (all_typed) {
			struct dv_buf	out;

			memset(&out, 0, sizeof out);
			for (i = 0; i < len; i++) {
				if (!dv_im_key(&s->im, payload[i], &out))
					dv_buf_append(&out, &payload[i], 1);
			}
			dv_window_pty_send(w, out.data, out.len);
			dv_buf_free(&out);
			redraw(s);
			return;
		}
	}

	if (s->broadcast) {		/* every other window gets it too */
		struct dv_window	*it;

		TAILQ_FOREACH(it, &s->desktop.windows, entry)
			if (it != w && it->pty_fd >= 0)
				dv_window_pty_send(it, payload, len);
	}
	if (w->screen.app_cursor && len == 3 && payload[0] == 0x1b &&
	    payload[1] == '[' && strchr("ABCDHF", payload[2]) != NULL)
		payload[1] = 'O';
	forward_input(s, w, payload, len);
}

/* Is an overlay or a placing mode taking the keys? */
static int
modal_up(const struct dv_session *s)
{
	return (s->menu_open || s->uni_open || s->quit_confirm || s->im_choose ||
	    s->help_open || s->xfer_prompt || s->title_edit || s->switch_open ||
	    s->copy_open || s->input_mode != 0);
}

/*
 * A structured key from a kitty-keyboard host. The focused window gets
 * it encoded for the keyboard flags its program pushed; a window with
 * none, or an overlay, gets the legacy bytes exactly as typed input.
 */
static void
handle_key_msg(struct dv_peer *p, unsigned char *payload, size_t len)
{
	struct dv_session	*s = p->session;
	struct dv_window	*w;
	struct dv_key		 k;
	unsigned char		 buf[160];
	size_t			 n;
	int			 flags = 0;

	if (!dv_key_unpack(payload, len, &k) || s == NULL || !p->display)
		return;
	w = s->desktop.focused;
	if (w != NULL && !modal_up(s) && s->im.method == DV_IM_OFF)
		flags = dv_screen_kbd_flags(&w->screen);
	if (flags == 0) {
		if (k.nlegacy > 0)
			handle_stdin_msg(p, k.legacy, k.nlegacy);
		return;
	}
	if (w->screen.sb_off > 0) {		/* typing returns to the bottom */
		w->screen.sb_off = 0;
		redraw(s);
	}
	n = dv_kbd_encode(&k, flags, buf, sizeof buf);
	if (n > 0)
		dv_window_pty_send(w, buf, n);
	if (s->broadcast) {			/* the other windows, each in its own encoding */
		struct dv_window	*it;

		TAILQ_FOREACH(it, &s->desktop.windows, entry) {
			int	f;

			if (it == w || it->pty_fd < 0)
				continue;
			f = dv_screen_kbd_flags(&it->screen);
			n = f ? dv_kbd_encode(&k, f, buf, sizeof buf) : k.nlegacy;
			if (f == 0)
				memcpy(buf, k.legacy, k.nlegacy);
			if (n > 0)
				dv_window_pty_send(it, buf, n);
		}
	}
}

static int
handle_msg(struct dv_peer *p, int type, unsigned char *payload, size_t len)
{
	switch (type) {
	case DV_MSG_IDENTIFY:
		handle_identify(p, payload, len);
		return (1);
	case DV_MSG_COMMAND:
		handle_command(p, payload, len);
		return (1);
	case DV_MSG_STDIN:
		handle_stdin_msg(p, payload, len);
		return (1);
	case DV_MSG_OSC:
		if (p->display)
			handle_osc_reply(p, payload, len);
		return (1);
	case DV_MSG_MOUSE:
		if (len >= 7 && p->display) {
			unsigned	mx = ((unsigned)payload[0] << 8) | payload[1];
			unsigned	my = ((unsigned)payload[2] << 8) | payload[3];
			unsigned	mc = ((unsigned)payload[4] << 8) | payload[5];

			handle_mouse(p, p->session, mx, my, mc, payload[6]);
		} else if (len >= 6 && p->display) {	/* an older client */
			unsigned	mx = ((unsigned)payload[0] << 8) | payload[1];
			unsigned	my = ((unsigned)payload[2] << 8) | payload[3];

			handle_mouse(p, p->session, mx, my, payload[4],
			    payload[5]);
		}
		return (1);
	case DV_MSG_KEY:
		handle_key_msg(p, payload, len);
		return (1);
	case DV_MSG_RESIZE:
		handle_resize(p, payload, len);
		return (1);
	case DV_MSG_EXIT:
		return (0);
	default:
		return (1);
	}
}

static void
accept_peer(void)
{
	struct dv_peer	*p;
	int		 fd;

	fd = accept(dv_srv->listen_fd, NULL, NULL);
	if (fd < 0)
		return;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	{
		int	sz = 256 * 1024;	/* a full frame in one write */

		setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
	}
	p = dv_new<dv_peer>();
	p->fd = fd;
	dv_conn_init(&p->conn, fd);
	TAILQ_INSERT_TAIL(&dv_srv->peers, p, entry);
}

static void
read_pty(struct dv_window *w)
{
	unsigned char	 buf[8192];
	ssize_t		 n;

	n = read(w->pty_fd, buf, sizeof buf);
	if (n > 0) {
		if (w->pipe_fd >= 0 && write(w->pipe_fd, buf, (size_t)n) < 0) {
			close(w->pipe_fd);	/* the log went away: stop */
			w->pipe_fd = -1;
		}
		broadcast_pty(w, buf, (size_t)n);
		return;
	}
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	dv_log("window %u pty closed", w->id);
	dv_window_close(w);
}

static void
reap_children(void)
{
	int	status;

	while (waitpid(-1, &status, WNOHANG) > 0)
		continue;
}

static void
collect_dead_sessions(void)
{
	struct dv_session	*s, *s1;
	struct dv_peer		*p;

	TAILQ_FOREACH_SAFE(s, &dv_srv->sessions, entry, s1) {
		if (!TAILQ_EMPTY(&s->desktop.windows))
			continue;
		TAILQ_FOREACH(p, &dv_srv->peers, entry) {
			if (p->session == s) {
				dv_conn_send(&p->conn, DV_MSG_EXIT, NULL, 0);
				p->session = NULL;
				p->display = 0;
			}
		}
		dv_session_destroy(s);
	}
}

/*
 * A file-transfer OSC from a window goes to a terminal that speaks the
 * protocol, if one is attached to the session: kitty for OSC 5113,
 * goblin-mosh for OSC 777, known by their XTVERSION names. One terminal
 * only, or two would both save the file and both answer.
 */
static int
srv_forward_osc(struct dv_window *w, const char *proto, unsigned osc,
    const char *body, size_t n)
{
	struct dv_peer	*p;

	TAILQ_FOREACH(p, &dv_srv->peers, entry) {
		struct dv_buf	b;
		char		head[32];
		int		hn;

		if (!p->display || p->session != w->session ||
		    !dv_term_is(p->host, proto))
			continue;
		memset(&b, 0, sizeof b);
		hn = snprintf(head, sizeof head, "\033]%u;", osc);
		dv_buf_append(&b, head, (size_t)hn);
		dv_buf_append(&b, body, n);
		dv_buf_append(&b, "\033\\", 2);
		dv_conn_send(&p->conn, DV_MSG_PASSTHROUGH, b.data, b.len);
		dv_conn_flush(&p->conn);
		dv_buf_free(&b);
		return (1);
	}
	return (0);
}

struct dv_config	dv_cfg;

static int
server_loop(void)
{
	struct pollfd	 pfds[256];
	struct dv_peer	*p, *p1;
	struct dv_session *s;
	struct dv_window *w;
	int		 nfd, i, nfds;

	while (!dv_quit) {
		nfds = 0;
		pfds[nfds].fd = dv_srv->listen_fd;
		pfds[nfds].events = POLLIN;
		nfds++;
		TAILQ_FOREACH(p, &dv_srv->peers, entry) {
			if (nfds >= 255)
				break;
			pfds[nfds].fd = p->fd;
			pfds[nfds].events = POLLIN;
			if (p->conn.out.len)
				pfds[nfds].events |= POLLOUT;
			nfds++;
		}
		TAILQ_FOREACH(s, &dv_srv->sessions, entry) {
			TAILQ_FOREACH(w, &s->desktop.windows, entry) {
				if (w->pty_fd < 0 || nfds >= 255)
					continue;
				pfds[nfds].fd = w->pty_fd;
				pfds[nfds].events = POLLIN;
				if (w->pty_out.len > 0)
					pfds[nfds].events |= POLLOUT;
				nfds++;
			}
		}
		nfd = poll(pfds, (nfds_t)nfds, 500);
		if (nfd < 0) {
			if (errno == EINTR) {
				reap_children();
				continue;
			}
			break;
		}
		reap_children();
		if (pfds[0].revents & POLLIN)
			accept_peer();

		TAILQ_FOREACH_SAFE(p, &dv_srv->peers, entry, p1) {
			int	want_in = 0, want_out = 0, r;

			for (i = 1; i < nfds; i++) {
				if (pfds[i].fd != p->fd)
					continue;
				if (pfds[i].revents & (POLLIN | POLLHUP | POLLERR))
					want_in = 1;
				if (pfds[i].revents & POLLOUT)
					want_out = 1;
			}
			if (want_out) {
				if (dv_conn_flush(&p->conn) < 0) {
					peer_close(p);
					continue;
				}
			}
			if (want_in) {
				r = dv_conn_read(&p->conn);
				if (r <= 0) {
					peer_close(p);
					continue;
				}
				for (;;) {
					int		 type;
					unsigned char	*pl = NULL;
					size_t		 plen = 0;
					int		 st;

					st = dv_msg_take(&p->conn.in, &type,
					    &pl, &plen);
					if (st == 0)
						break;
					if (st < 0) {
						peer_close(p);
						p = NULL;
						break;
					}
					r = handle_msg(p, type, pl, plen);
					free(pl);
					if (!r) {
						peer_close(p);
						p = NULL;
						break;
					}
				}
			}
		}

		TAILQ_FOREACH(s, &dv_srv->sessions, entry) {
			struct dv_window	*w1;

			TAILQ_FOREACH_SAFE(w, &s->desktop.windows, entry, w1) {
				int	want = 0, wout = 0;

				if (w->pty_fd < 0)
					continue;
				for (i = 1; i < nfds; i++) {
					if (pfds[i].fd != w->pty_fd)
						continue;
					if (pfds[i].revents &
					    (POLLIN | POLLHUP | POLLERR))
						want = 1;
					if (pfds[i].revents & POLLOUT)
						wout = 1;
				}
				if (wout) {
					/* the program took some: send more */
					dv_window_pty_flush(w);
					dv_xfer_pump(w);
				}
				if (want)
					read_pty(w);
			}
		}
		collect_dead_sessions();

		TAILQ_FOREACH(s, &dv_srv->sessions, entry) {
			if (s->dirty) {
				s->dirty = 0;
				compose_send(s);
			}
		}
	}

	while ((p = TAILQ_FIRST(&dv_srv->peers)) != NULL)
		peer_close(p);
	while ((s = TAILQ_FIRST(&dv_srv->sessions)) != NULL)
		dv_session_destroy(s);
	close(dv_srv->listen_fd);
	unlink(dv_srv->sock_path);
	free(dv_srv->sock_path);
	free(dv_srv);
	dv_srv = NULL;
	return (0);
}

static void
server_signals(void)
{
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, on_sigchld);
	signal(SIGTERM, on_sigterm);
	signal(SIGINT, on_sigterm);
}

/*
 * The server's settings from the config file. An environment variable
 * that names the same thing wins; the file fills in what is unset.
 */
static void
server_load_config(void)
{
	dv_config_defaults(&dv_cfg);
	dv_config_load(&dv_cfg, dv_config_path(), NULL, NULL);
	dv_scrollback_lines = dv_cfg.scrollback;
	dv_backdrop_glyph = dv_cfg.backdrop;
	if (dv_cfg.kitty_medium[0] != '\0' && getenv("GOBLIN_VIEW_KITTY_MEDIUM") == NULL)
		setenv("GOBLIN_VIEW_KITTY_MEDIUM", dv_cfg.kitty_medium, 1);
	if (dv_cfg.download_dir[0] != '\0' && getenv("GOBLIN_VIEW_DOWNLOAD_DIR") == NULL)
		setenv("GOBLIN_VIEW_DOWNLOAD_DIR", dv_cfg.download_dir, 1);
	if (dv_cfg.term[0] != '\0' && getenv("GOBLIN_VIEW_TERM") == NULL)
		setenv("GOBLIN_VIEW_TERM", dv_cfg.term, 1);
	dv_limits_apply(&dv_cfg);
}

/*
 * This process was a goblin-view server that exec'd us: take over its
 * sessions, windows and clients from the state file on the descriptors
 * it left open, tell the client that asked, and carry on.
 */
int
dv_server_takeover(const char *statefile, const char *sock_path, int debug)
{
	struct dv_peer		*p;
	struct dv_session	*s;
	int			 requester = -1;
	char			 msg[128];

	server_load_config();
	server_signals();
	dv_srv = dv_new<dv_server>();
	dv_srv->debug = debug;
	dv_xfer_forward_hook = srv_forward_osc;
	dv_dnd_app_hook = srv_dnd_from_app;
	dv_srv->sock_path = xstrdup(sock_path);
	TAILQ_INIT(&dv_srv->sessions);
	TAILQ_INIT(&dv_srv->peers);
	if (!dv_state_read(statefile, &requester))
		dv_fatal("takeover: cannot read %s", statefile);
	unlink(statefile);
	dv_log("takeover: %s, listening on fd %d", statefile, dv_srv->listen_fd);
	snprintf(msg, sizeof msg, "upgraded: goblin-view %s (protocol %d)\n", DV_VERSION,
	    DV_PROTOCOL);
	TAILQ_FOREACH(s, &dv_srv->sessions, entry)
		refresh_gfx(s);
	TAILQ_FOREACH(p, &dv_srv->peers, entry)
		if (p->display && p->session != nullptr)
			dv_silkie_attach(p);
	TAILQ_FOREACH(p, &dv_srv->peers, entry) {
		if (p->fd == requester)
			send_reply(p, 0, msg);
		else if (p->display && p->identified)
			send_hello(p);		/* a client of another build says so */
		p->view.full = 1;
	}
	TAILQ_FOREACH(s, &dv_srv->sessions, entry) {
		struct dv_window	*w;

		s->dirty = 1;
		TAILQ_FOREACH(w, &s->desktop.windows, entry)
			dv_window_pty_flush(w);
	}
	return (server_loop());
}

int
dv_server_main(const char *sock_path, int debug)
{
	server_load_config();
	server_signals();

	dv_srv = dv_new<dv_server>();
	dv_srv->debug = debug;
	dv_xfer_forward_hook = srv_forward_osc;
	dv_dnd_app_hook = srv_dnd_from_app;
	dv_srv->sock_path = xstrdup(sock_path);
	TAILQ_INIT(&dv_srv->sessions);
	TAILQ_INIT(&dv_srv->peers);

	dv_srv->listen_fd = dv_socket_listen(sock_path);
	if (dv_srv->listen_fd < 0)
		dv_fatal("listen %s: %s", sock_path, strerror(errno));
	dv_log("server listen %s", sock_path);

	return (server_loop());
}
