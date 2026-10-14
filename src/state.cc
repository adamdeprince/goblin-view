/*
 * The running server's state, written for the binary that replaces it
 * and read back by that binary. `goblin-view upgrade` makes the server
 * write it, mark its file descriptors inheritable and exec the new
 * program in place: the same process, so the programs in the windows
 * (its children) and the attached clients (its sockets) carry on. The
 * new server reads the state, rebuilds every session, window, screen
 * and peer on the same descriptors, and repaints.
 *
 * The format is text, one keyed line per thing, so a newer reader can
 * take an older writer's file: unknown keys are skipped and missing
 * ones keep their defaults. Images, text-sizing runs, file transfers,
 * drags and menus in progress are not carried; programs redraw images.
 */

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dv.hpp"

static void
put_str(struct dv_buf *b, const char *key, const char *v)
{
	dv_buf_append(b, key, strlen(key));
	dv_buf_append(b, " ", 1);
	for (; *v != '\0'; v++) {
		if (*v == '\\')
			dv_buf_append(b, "\\\\", 2);
		else if (*v == '\n')
			dv_buf_append(b, "\\n", 2);
		else
			dv_buf_append(b, v, 1);
	}
	dv_buf_append(b, "\n", 1);
}

static void
unescape(char *s)
{
	char	*o = s;

	for (; *s != '\0'; s++) {
		if (*s == '\\' && s[1] == 'n') {
			*o++ = '\n';
			s++;
		} else if (*s == '\\' && s[1] == '\\') {
			*o++ = '\\';
			s++;
		} else
			*o++ = *s;
	}
	*o = '\0';
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void
put_b64(struct dv_buf *b, const char *key, const unsigned char *d, size_t n)
{
	size_t	i;

	dv_buf_append(b, key, strlen(key));
	dv_buf_append(b, " ", 1);
	for (i = 0; i < n; i += 3) {
		unsigned	v = (unsigned)d[i] << 16;
		char		o[4];

		if (i + 1 < n)
			v |= (unsigned)d[i + 1] << 8;
		if (i + 2 < n)
			v |= d[i + 2];
		o[0] = B64[(v >> 18) & 63];
		o[1] = B64[(v >> 12) & 63];
		o[2] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
		o[3] = i + 2 < n ? B64[v & 63] : '=';
		dv_buf_append(b, o, 4);
	}
	dv_buf_append(b, "\n", 1);
}

static void
line(struct dv_buf *b, const char *fmt, ...)
{
	char	t[1024];
	va_list	ap;
	int	n;

	va_start(ap, fmt);
	n = vsnprintf(t, sizeof t, fmt, ap);
	va_end(ap);
	if (n > 0)
		dv_buf_append(b, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
	dv_buf_append(b, "\n", 1);
}

static void
write_screen(struct dv_buf *b, struct dv_screen *sc)
{
	unsigned	y, i;
	int		a;

	line(b, "screen %u %u %u %u %u %u %d %d %d %d %d %d %d %d %d %d %d %u %u %u %u %u %d %d %d %d %u %u %u %u %u %u %u",
	    sc->cols, sc->rows, sc->cr, sc->cc, sc->mt, sc->mb, sc->wrap, sc->visible,
	    sc->alt_on, sc->app_cursor, sc->insert, sc->mouse, sc->alt_scroll, sc->bracketed,
	    sc->mouse_sgr, sc->sixel_dm, sc->sixel_right, sc->fg, sc->bg, sc->attr, sc->ustyle,
	    sc->ucolor, sc->sace, sc->charset[0], sc->charset[1], sc->shift, sc->sb_off,
	    sc->last_ch, sc->alpha, sc->fg_br, sc->bg_br, sc->fg_gamut, sc->bg_gamut);
	line(b, "silk %d %d %d %d %d %d %d %u %d", sc->silk_tablet, sc->silk_scroll,
	    sc->silk_gesture, sc->silk_frame, sc->silk_latency, sc->silk_display,
	    sc->silk_ink, sc->silk_frame_seq, sc->silk_frame_dirty);
	put_str(b, "sbrush", sc->silk_brush);
	put_str(b, "sshown", sc->silk_shown);
	line(b, "pointer %d", sc->pointer_mode);
	for (a = 0; a < 2; a++) {
		struct dv_buf	k;

		memset(&k, 0, sizeof k);
		line(&k, "kbd %d %d", a, sc->nkbd[a]);
		k.len--;		/* the flags follow on the same line */
		for (i = 0; i < (unsigned)sc->nkbd[a]; i++) {
			char	t[16];

			snprintf(t, sizeof t, " %d", sc->kbd[a][i]);
			dv_buf_append(&k, t, strlen(t));
		}
		dv_buf_append(&k, "\n", 1);
		dv_buf_append(b, k.data, k.len);
		dv_buf_free(&k);
	}
	line(b, "xcurcol %u %u %u %u", sc->xcur_tcs, sc->xcur_tcol, sc->xcur_ccs, sc->xcur_ccol);
	for (i = 0; i < (unsigned)sc->nxcur; i++)
		line(b, "xcur %u %u %u", sc->xcur[i].col, sc->xcur[i].row, sc->xcur[i].shape);
	line(b, "pal %u %u %u %u %u %u", sc->pal.fgset, sc->pal.fg, sc->pal.bgset, sc->pal.bg,
	    sc->pal.curset, sc->pal.cursor);
	for (i = 0; i < 256; i++)
		if (sc->pal.set[i])
			line(b, "palidx %u %u", i, sc->pal.idx[i]);
	for (i = 0; i < (unsigned)sc->nsmode; i++)
		line(b, "smode %d %d", sc->smode_num[i], sc->smode_val[i]);
	for (a = 0; a < 2; a++) {
		const struct dv_cursor_save	*c = a ? &sc->saved_alt : &sc->saved;

		line(b, "csave %d %u %u %u %u %u %d %u %u %u %u %u %u %u", a, c->cr, c->cc,
		    c->fg, c->bg, c->attr, c->valid, c->ustyle, c->ucolor, c->alpha,
		    c->fg_br, c->bg_br, c->fg_gamut, c->bg_gamut);
	}
	{
		/* main = the primary screen, alt = the alternate buffer,
		 * whichever of the two arrays holds them right now */
		struct dv_cell	*main_cells = sc->alt_on ? sc->alt : sc->cells;
		struct dv_cell	*alt_cells = sc->alt_on ? sc->cells : sc->alt;

		for (y = 0; y < sc->rows && main_cells != nullptr; y++) {
			line(b, "main %u", y);
			b->len--;
			dv_buf_append(b, " ", 1);
			dv_cells_encode(b, main_cells + (size_t)y * sc->cols, sc->cols);
			dv_buf_append(b, "\n", 1);
		}
		for (y = 0; y < sc->rows && alt_cells != nullptr; y++) {
			line(b, "alt %u", y);
			b->len--;
			dv_buf_append(b, " ", 1);
			dv_cells_encode(b, alt_cells + (size_t)y * sc->cols, sc->cols);
			dv_buf_append(b, "\n", 1);
		}
	}
	for (i = 0; i < sc->sb_len; i++) {
		unsigned		 width;
		const struct dv_cell	*row = dv_screen_history_row(sc, i, &width);

		if (row == nullptr)
			continue;
		line(b, "sb %u", width);
		b->len--;
		dv_buf_append(b, " ", 1);
		dv_cells_encode(b, row, width);
		dv_buf_append(b, "\n", 1);
	}
}

int
dv_state_write(const char *path, const struct dv_peer *requester)
{
	struct dv_buf		 b;
	struct dv_session	*s;
	struct dv_window	*w;
	struct dv_peer		*p;
	int			 fd;

	memset(&b, 0, sizeof b);
	line(&b, "goblin-view-state 1");
	line(&b, "version %s", DV_VERSION);
	line(&b, "protocol %d", DV_PROTOCOL);
	line(&b, "listen %d", dv_srv->listen_fd);
	TAILQ_FOREACH(s, &dv_srv->sessions, entry) {
		line(&b, "session %u %u %u %u %u %u %u %d", s->desktop.cols, s->desktop.rows,
		    s->desktop.xpixel, s->desktop.ypixel, s->desktop.next_snap,
		    s->desktop.cascade, s->desktop.focused ? s->desktop.focused->id : 0,
		    s->broadcast);
		put_str(&b, "name", s->name);
		if (s->mark.len > 0)
			put_b64(&b, "mark", s->mark.data, s->mark.len);
		line(&b, "im %d", s->im.method);
		TAILQ_FOREACH(w, &s->desktop.windows, entry) {
			unsigned	i;
			int		k;

			line(&b, "window %u %d %ld %d %d %u %u %d %d %d %d %d %d %d %u %u %u %u %d %d %u %u %d %d",
			    w->id, w->pty_fd, (long)w->pid, w->geom.x, w->geom.y, w->geom.cols,
			    w->geom.rows, w->z, w->framed, w->zoomed, w->hidden, w->aside, w->frozen,
			    w->locked, w->lcols, w->lrows, w->ox, w->oy, w->title_locked,
			    w->resize_req, w->req_cols, w->req_rows, w->dnd_accept, w->pipe_fd);
			line(&b, "wsaved %d %d %u %u", w->saved.x, w->saved.y, w->saved.cols,
			    w->saved.rows);
			put_str(&b, "title", w->title);
			put_str(&b, "cmd", w->cmd);
			put_str(&b, "cwd", w->cwd);
			put_str(&b, "dnd_types", w->dnd_types);
			for (i = 0; i < w->cmdv_len; i += (unsigned)strlen(w->cmdv + i) + 1)
				put_str(&b, "arg", w->cmdv + i);
			for (k = 0; k < w->npshape; k++)
				put_str(&b, "pshape", w->pshape[k]);
			if (w->pty_out.len > 0)
				put_b64(&b, "pty_out", w->pty_out.data, w->pty_out.len);
			write_screen(&b, &w->screen);
			line(&b, "end");
		}
	}
	TAILQ_FOREACH(p, &dv_srv->peers, entry) {
		line(&b, "peer %d %d %d %u %u %u %u %d %d %d %d", p->fd, p->display,
		    p->identified, p->tty.cols, p->tty.rows, p->tty.xpixel, p->tty.ypixel,
		    p->features, p == requester, p->view.vx, p->view.vy);
		put_str(&b, "pterm", p->term);
		put_str(&b, "phost", p->host);
		line(&b, "ppointer %d", p->pointer_caps);
		if (p->silkie_blob[0] != '\0')
			put_str(&b, "psilkie", p->silkie_blob);
		put_str(&b, "pcwd", p->cwd);
		put_str(&b, "psession", p->session != NULL ? p->session->name : "");
		if (p->conn.out.len > 0)
			put_b64(&b, "pout", p->conn.out.data, p->conn.out.len);
		if (p->conn.in.len > 0)
			put_b64(&b, "pin", p->conn.in.data, p->conn.in.len);
		line(&b, "pend");
	}
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		dv_buf_free(&b);
		return (0);
	}
	{
		size_t	off = 0;

		while (off < b.len) {
			ssize_t	r = write(fd, (const char *)b.data + off, b.len - off);

			if (r <= 0) {
				close(fd);
				dv_buf_free(&b);
				return (0);
			}
			off += (size_t)r;
		}
	}
	close(fd);
	dv_buf_free(&b);
	return (1);
}

/* Every descriptor the new server needs must survive exec; afterwards
 * (or on a failed exec) they close on exec again. */
void
dv_state_inherit_fds(int on)
{
	struct dv_session	*s;
	struct dv_window	*w;
	struct dv_peer		*p;
	int			 flag = on ? 0 : FD_CLOEXEC;

	fcntl(dv_srv->listen_fd, F_SETFD, flag);
	TAILQ_FOREACH(p, &dv_srv->peers, entry)
		fcntl(p->fd, F_SETFD, flag);
	TAILQ_FOREACH(s, &dv_srv->sessions, entry)
		TAILQ_FOREACH(w, &s->desktop.windows, entry) {
			if (w->pty_fd >= 0)
				fcntl(w->pty_fd, F_SETFD, flag);
			if (w->pipe_fd >= 0)
				fcntl(w->pipe_fd, F_SETFD, flag);
		}
}

static long
b64_decode_into(const char *t, struct dv_buf *out)
{
	unsigned char	*raw = dv_allocn<unsigned char>(strlen(t) + 3);
	long		 n = dv_b64_decode(t, strlen(t), raw, strlen(t) + 3);

	if (n > 0)
		dv_buf_append(out, raw, (size_t)n);
	free(raw);
	return (n);
}

int
dv_state_read(const char *path, int *requester_fd)
{
	FILE			*f = fopen(path, "r");
	char			*ln = nullptr;
	size_t			 cap = 0;
	ssize_t			 n;
	struct dv_session	*s = nullptr;
	struct dv_window	*w = nullptr;
	struct dv_peer		*p = nullptr;
	struct dv_size		 sz;
	unsigned		 focused_id = 0, next_snap = 0, cascade = 0;
	int			 broadcast = 0, alt_on_pending = 0;
	int			 ok = 0;

	*requester_fd = -1;
	memset(&sz, 0, sizeof sz);
	if (f == nullptr)
		return (0);
	if ((n = getline(&ln, &cap, f)) < 0 || strncmp(ln, "goblin-view-state ", 18) != 0) {
		free(ln);
		fclose(f);
		return (0);
	}
	while ((n = getline(&ln, &cap, f)) >= 0) {
		char	*key = ln, *val;

		if (n > 0 && ln[n - 1] == '\n')
			ln[n - 1] = '\0';
		val = strchr(ln, ' ');
		if (val != nullptr)
			*val++ = '\0';
		else
			val = ln + strlen(ln);
		if (strcmp(key, "listen") == 0) {
			dv_srv->listen_fd = atoi(val);
			ok = 1;
		} else if (strcmp(key, "session") == 0) {
			s = nullptr;
			w = nullptr;
			sscanf(val, "%u %u %u %u %u %u %u %d", &sz.cols, &sz.rows, &sz.xpixel,
			    &sz.ypixel, &next_snap, &cascade, &focused_id, &broadcast);
		} else if (strcmp(key, "name") == 0) {
			unescape(val);
			s = dv_session_create(val, sz);
			s->desktop.next_snap = next_snap;
			s->desktop.cascade = cascade;
			s->broadcast = broadcast;
			s->dirty = 1;
		} else if (s != nullptr && strcmp(key, "mark") == 0) {
			b64_decode_into(val, &s->mark);
		} else if (s != nullptr && strcmp(key, "im") == 0) {
			s->im.method = atoi(val);
		} else if (s != nullptr && strcmp(key, "window") == 0) {
			unsigned	id, cols, rows, lcols, lrows, ox, oy, rcols, rrows;
			int		fd, x, y, z, framed, zoomed, hidden, aside, frozen, locked,
					tlocked, rreq, dnd, pipe;
			long		pid;

			if (sscanf(val, "%u %d %ld %d %d %u %u %d %d %d %d %d %d %d %u %u %u %u %d %d %u %u %d %d",
			    &id, &fd, &pid, &x, &y, &cols, &rows, &z, &framed, &zoomed, &hidden,
			    &aside, &frozen, &locked, &lcols, &lrows, &ox, &oy, &tlocked, &rreq,
			    &rcols, &rrows, &dnd, &pipe) < 24) {
				w = nullptr;
				continue;
			}
			w = dv_window_adopt(s, id, fd, (pid_t)pid, lcols > 0 ? lcols : 1,
			    lrows > 0 ? lrows : 1);
			w->geom.x = x;
			w->geom.y = y;
			w->geom.cols = cols;
			w->geom.rows = rows;
			w->z = z;
			w->framed = framed;
			w->zoomed = zoomed;
			w->hidden = hidden;
			w->aside = aside;
			w->frozen = frozen;
			w->locked = locked;
			w->ox = ox;
			w->oy = oy;
			w->title_locked = tlocked;
			w->resize_req = rreq;
			w->req_cols = rcols;
			w->req_rows = rrows;
			w->dnd_accept = dnd;
			w->pipe_fd = pipe;
			alt_on_pending = 0;
			if (id == focused_id)
				s->desktop.focused = w;
		} else if (w != nullptr && strcmp(key, "wsaved") == 0) {
			sscanf(val, "%d %d %u %u", &w->saved.x, &w->saved.y, &w->saved.cols,
			    &w->saved.rows);
		} else if (w != nullptr && strcmp(key, "title") == 0) {
			unescape(val);
			dv_strlcpy(w->title, val, sizeof w->title);
		} else if (w != nullptr && strcmp(key, "cmd") == 0) {
			unescape(val);
			dv_strlcpy(w->cmd, val, sizeof w->cmd);
		} else if (w != nullptr && strcmp(key, "cwd") == 0) {
			unescape(val);
			dv_strlcpy(w->cwd, val, sizeof w->cwd);
		} else if (w != nullptr && strcmp(key, "dnd_types") == 0) {
			unescape(val);
			dv_strlcpy(w->dnd_types, val, sizeof w->dnd_types);
		} else if (w != nullptr && strcmp(key, "arg") == 0) {
			size_t	l;

			unescape(val);
			l = strlen(val) + 1;
			if (w->cmdv_len + l <= sizeof w->cmdv) {
				memcpy(w->cmdv + w->cmdv_len, val, l);
				w->cmdv_len += (unsigned)l;
			}
		} else if (w != nullptr && strcmp(key, "pshape") == 0) {
			unescape(val);
			if (w->npshape < 16) {
				dv_strlcpy(w->pshape[w->npshape], val, sizeof w->pshape[0]);
				w->npshape++;
			}
		} else if (w != nullptr && strcmp(key, "pty_out") == 0) {
			b64_decode_into(val, &w->pty_out);
		} else if (w != nullptr && strcmp(key, "screen") == 0) {
			struct dv_screen	*sc = &w->screen;
			unsigned		 cols, rows, attr, ustyle;
			unsigned		 alpha = 100, fgbr = 100, bgbr = 100, fgg = 0, bgg = 0;

			sscanf(val, "%u %u %u %u %u %u %d %d %d %d %d %d %d %d %d %d %d %u %u %u %u %u %d %d %d %d %u %u %u %u %u %u %u",
			    &cols, &rows, &sc->cr, &sc->cc, &sc->mt, &sc->mb, &sc->wrap, &sc->visible,
			    &alt_on_pending, &sc->app_cursor, &sc->insert, &sc->mouse, &sc->alt_scroll,
			    &sc->bracketed, &sc->mouse_sgr, &sc->sixel_dm, &sc->sixel_right, &sc->fg,
			    &sc->bg, &attr, &ustyle, &sc->ucolor, &sc->sace, &sc->charset[0],
			    &sc->charset[1], &sc->shift, &sc->sb_off, &sc->last_ch, &alpha, &fgbr,
			    &bgbr, &fgg, &bgg);
			sc->attr = (uint8_t)attr;
			sc->ustyle = (uint8_t)ustyle;
			sc->alpha = (uint8_t)alpha;
			sc->fg_br = (uint16_t)fgbr;
			sc->bg_br = (uint16_t)bgbr;
			sc->fg_gamut = (uint8_t)fgg;
			sc->bg_gamut = (uint8_t)bgg;
			if (cols != sc->cols || rows != sc->rows)
				dv_screen_resize(sc, cols, rows);
		} else if (w != nullptr && strcmp(key, "silk") == 0) {
			unsigned	seq = 0;
			int		tab, scr, ges, fr, lat, disp, ink, dirty;

			if (sscanf(val, "%d %d %d %d %d %d %d %u %d", &tab, &scr, &ges, &fr,
			    &lat, &disp, &ink, &seq, &dirty) == 9) {
				w->screen.silk_tablet = tab;
				w->screen.silk_scroll = scr;
				w->screen.silk_gesture = ges;
				w->screen.silk_frame = fr;
				w->screen.silk_latency = lat;
				w->screen.silk_display = disp;
				w->screen.silk_ink = ink;
				w->screen.silk_frame_seq = seq;
				w->screen.silk_frame_dirty = dirty;
			}
		} else if (w != nullptr && strcmp(key, "sbrush") == 0) {
			unescape(val);
			dv_strlcpy(w->screen.silk_brush, val, sizeof w->screen.silk_brush);
		} else if (w != nullptr && strcmp(key, "sshown") == 0) {
			unescape(val);
			dv_strlcpy(w->screen.silk_shown, val, sizeof w->screen.silk_shown);
		} else if (w != nullptr && strcmp(key, "pointer") == 0) {
			w->screen.pointer_mode = atoi(val) == 1;
		} else if (w != nullptr && strcmp(key, "kbd") == 0) {
			int	a, cnt, i, v;
			char	*q = val, *end;

			a = (int)strtol(q, &end, 10);
			q = end;
			cnt = (int)strtol(q, &end, 10);
			q = end;
			if (a == 0 || a == 1) {
				w->screen.nkbd[a] = 0;
				for (i = 0; i < cnt && i < 8; i++) {
					v = (int)strtol(q, &end, 10);
					if (end == q)
						break;
					q = end;
					w->screen.kbd[a][w->screen.nkbd[a]++] = v;
				}
			}
		} else if (w != nullptr && strcmp(key, "xcurcol") == 0) {
			unsigned	a, c;

			sscanf(val, "%u %u %u %u", &a, &w->screen.xcur_tcol, &c, &w->screen.xcur_ccol);
			w->screen.xcur_tcs = (uint8_t)a;
			w->screen.xcur_ccs = (uint8_t)c;
		} else if (w != nullptr && strcmp(key, "xcur") == 0) {
			unsigned	col, row, shape;

			if (sscanf(val, "%u %u %u", &col, &row, &shape) == 3 &&
			    w->screen.nxcur < DV_XCURSORS) {
				w->screen.xcur[w->screen.nxcur].col = (uint16_t)col;
				w->screen.xcur[w->screen.nxcur].row = (uint16_t)row;
				w->screen.xcur[w->screen.nxcur].shape = (uint8_t)shape;
				w->screen.nxcur++;
			}
		} else if (w != nullptr && strcmp(key, "pal") == 0) {
			unsigned	a, c, d;

			sscanf(val, "%u %u %u %u %u %u", &a, &w->screen.pal.fg, &c, &w->screen.pal.bg,
			    &d, &w->screen.pal.cursor);
			w->screen.pal.fgset = (uint8_t)a;
			w->screen.pal.bgset = (uint8_t)c;
			w->screen.pal.curset = (uint8_t)d;
			w->screen.pal.any = a || c || d;
		} else if (w != nullptr && strcmp(key, "palidx") == 0) {
			unsigned	i, rgb;

			if (sscanf(val, "%u %u", &i, &rgb) == 2 && i < 256) {
				w->screen.pal.idx[i] = rgb;
				w->screen.pal.set[i] = 1;
				w->screen.pal.any = 1;
			}
		} else if (w != nullptr && strcmp(key, "smode") == 0) {
			int	num, v;

			if (sscanf(val, "%d %d", &num, &v) == 2 && w->screen.nsmode < 16) {
				w->screen.smode_num[w->screen.nsmode] = num;
				w->screen.smode_val[w->screen.nsmode] = v;
				w->screen.nsmode++;
			}
		} else if (w != nullptr && strcmp(key, "csave") == 0) {
			int			 a;
			unsigned		 attr, ustyle;
			struct dv_cursor_save	 c;

			unsigned		 alpha = 100, fgbr = 100, bgbr = 100, fgg = 0, bgg = 0;

			memset(&c, 0, sizeof c);
			c.alpha = 100;
			c.fg_br = c.bg_br = 100;
			if (sscanf(val, "%d %u %u %u %u %u %d %u %u %u %u %u %u %u", &a, &c.cr,
			    &c.cc, &c.fg, &c.bg, &attr, &c.valid, &ustyle, &c.ucolor, &alpha,
			    &fgbr, &bgbr, &fgg, &bgg) >= 9) {
				c.attr = (uint8_t)attr;
				c.ustyle = (uint8_t)ustyle;
				c.alpha = (uint8_t)alpha;
				c.fg_br = (uint16_t)fgbr;
				c.bg_br = (uint16_t)bgbr;
				c.fg_gamut = (uint8_t)fgg;
				c.bg_gamut = (uint8_t)bgg;
				if (a == 0)
					w->screen.saved = c;
				else
					w->screen.saved_alt = c;
			}
		} else if (w != nullptr && (strcmp(key, "main") == 0 || strcmp(key, "alt") == 0)) {
			struct dv_screen	*sc = &w->screen;
			char			*end;
			unsigned		 y = (unsigned)strtoul(val, &end, 10);

			if (y < sc->rows) {
				struct dv_cell	*dst;

				if (key[0] == 'a') {
					if (sc->alt == nullptr) {
						unsigned	i;

						sc->alt = dv_allocn<dv_cell>((size_t)sc->cols * sc->rows);
						for (i = 0; i < sc->cols * sc->rows; i++) {
							sc->alt[i].ch = U' ';
							sc->alt[i].fg = sc->alt[i].bg = DV_COLOR_DEFAULT;
							sc->alt[i].ucolor = DV_COLOR_DEFAULT;
							sc->alt[i].alpha = 100;
							sc->alt[i].fg_br = sc->alt[i].bg_br = 100;
						}
					}
					dst = sc->alt + (size_t)y * sc->cols;
				} else
					dst = sc->cells + (size_t)y * sc->cols;
				dv_cells_decode(end, dst, sc->cols);
			}
		} else if (w != nullptr && strcmp(key, "sb") == 0) {
			char		*end;
			unsigned	 width = (unsigned)strtoul(val, &end, 10);
			struct dv_cell	*row;

			if (width == 0 || width > 4096)
				continue;
			row = dv_allocn<dv_cell>(width);
			dv_cells_decode(end, row, width);
			dv_screen_sb_push(&w->screen, row, width);
			free(row);
		} else if (w != nullptr && strcmp(key, "end") == 0) {
			struct dv_screen	*sc = &w->screen;

			if (alt_on_pending) {	/* the alternate screen was showing */
				struct dv_cell	*t = sc->cells;

				if (sc->alt == nullptr) {
					sc->alt = dv_allocn<dv_cell>((size_t)sc->cols * sc->rows);
					memcpy(sc->alt, sc->cells, (size_t)sc->cols * sc->rows * sizeof *t);
				}
				sc->cells = sc->alt;
				sc->alt = t;
				sc->alt_on = 1;
			}
			if (sc->sb_off > sc->sb_len)
				sc->sb_off = sc->sb_len;
			w = nullptr;
		} else if (p != nullptr && strcmp(key, "ppointer") == 0) {
            p->pointer_caps = atoi(val) & 7;
		} else if (p != nullptr && strcmp(key, "psilkie") == 0) {
			unescape(val);
			dv_strlcpy(p->silkie_blob, val, sizeof p->silkie_blob);
        } else if (strcmp(key, "peer") == 0) {
			int	fd, display, identified, features, req, vx, vy;
			struct dv_size	t;

			memset(&t, 0, sizeof t);
			if (sscanf(val, "%d %d %d %u %u %u %u %d %d %d %d", &fd, &display, &identified,
			    &t.cols, &t.rows, &t.xpixel, &t.ypixel, &features, &req, &vx, &vy) < 11) {
				p = nullptr;
				continue;
			}
			p = dv_new<dv_peer>();
			p->fd = fd;
			dv_conn_init(&p->conn, fd);
			p->display = display;
			p->identified = identified;
			p->tty = t;
			p->features = features;
			p->view.vx = vx;
			p->view.vy = vy;
			p->view.full = 1;
			TAILQ_INSERT_TAIL(&dv_srv->peers, p, entry);
			if (req)
				*requester_fd = fd;
		} else if (p != nullptr && strcmp(key, "pterm") == 0) {
			unescape(val);
			dv_strlcpy(p->term, val, sizeof p->term);
		} else if (p != nullptr && strcmp(key, "phost") == 0) {
			unescape(val);
			dv_strlcpy(p->host, val, sizeof p->host);
		} else if (p != nullptr && strcmp(key, "pcwd") == 0) {
			unescape(val);
			dv_strlcpy(p->cwd, val, sizeof p->cwd);
		} else if (p != nullptr && strcmp(key, "psession") == 0) {
			unescape(val);
			p->session = val[0] ? dv_session_find(val) : nullptr;
            if (p->session && p->display) p->session->pointer_caps |= p->pointer_caps;
		} else if (p != nullptr && strcmp(key, "pout") == 0) {
			b64_decode_into(val, &p->conn.out);
		} else if (p != nullptr && strcmp(key, "pin") == 0) {
			b64_decode_into(val, &p->conn.in);
		} else if (strcmp(key, "pend") == 0) {
			p = nullptr;
		}
	}
	free(ln);
	fclose(f);
	return (ok);
}
