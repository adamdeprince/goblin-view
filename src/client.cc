/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * The attach client. It owns the host tty (raw mode, alternate screen,
 * kitty keyboard) and relays: host keys go to the server as stdin, the
 * menu tap goes as a command, and frames from the server go to the tty
 * with blocking writes so a frame is never cut in half.
 */

#include "dv.hpp"

#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct termios	 orig_termios;
static int		 raw_set;
static int		 orig_flags = -1;
static int		 on_alt_screen;
static int		 winch_r = -1, winch_w = -1;

static void
restore_tty(void)
{
	if (on_alt_screen) {
		dv_host_disable(STDOUT_FILENO);
		on_alt_screen = 0;
	}
	if (raw_set)
		tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
	raw_set = 0;
	if (orig_flags >= 0)
		fcntl(STDIN_FILENO, F_SETFL, orig_flags);
	orig_flags = -1;
}

static void
on_winch(int sig)
{
	char	c = 1;

	(void)sig;
	if (winch_w >= 0)
		(void)!write(winch_w, &c, 1);
}

static void
on_exit_signal(int sig)
{
	restore_tty();
	_exit(128 + sig);
}

static int
set_raw(void)
{
	struct termios	t;

	if (tcgetattr(STDIN_FILENO, &orig_termios) < 0)
		return (-1);
	t = orig_termios;
	cfmakeraw(&t);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &t) < 0)
		return (-1);
	raw_set = 1;
	/*
	 * stdin and stdout are one open file description on a tty. Make
	 * sure it is blocking: a non-blocking stdout silently truncates
	 * any frame larger than the pty's output queue.
	 */
	orig_flags = fcntl(STDIN_FILENO, F_GETFL);
	if (orig_flags >= 0 && (orig_flags & O_NONBLOCK))
		fcntl(STDIN_FILENO, F_SETFL, orig_flags & ~O_NONBLOCK);
	atexit(restore_tty);
	return (0);
}

/*
 * GOBLIN_VIEW_TRACE=path: append everything painted to the host there, so a frame
 * the terminal did not show can be told from one that was never sent.
 */
static void
trace_bytes(const void *p, size_t n)
{
	static int	trace_fd = -2;

	if (trace_fd == -2) {
		const char	*t = getenv("GOBLIN_VIEW_TRACE");

		trace_fd = (t != NULL && *t != '\0') ?
		    open(t, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
	}
	if (trace_fd >= 0)
		(void)!write(trace_fd, p, n);
}

static void
tty_write(int fd, const void *p, size_t n)
{
	const char	*s = static_cast<const char *>(p);
	ssize_t		 w;

	if (fd == STDOUT_FILENO)
		trace_bytes(p, n);
	while (n > 0) {
		w = write(fd, s, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				struct pollfd	pf;

				pf.fd = fd;
				pf.events = POLLOUT;
				poll(&pf, 1, -1);
				continue;
			}
			return;
		}
		s += w;
		n -= (size_t)w;
	}
}

/*
 * Bytes a window asked us to send to the host (OSC-52 clipboard). If we
 * are ourselves nested inside tmux/goblin-tmux, wrap them so they reach
 * the real terminal instead of being eaten by the outer layer.
 */
static void
tty_passthrough(int fd, const unsigned char *p, size_t n)
{
	static int	nested = -1;

	/* A black-and-white host is told no cursor colours (OSC 12, 112). */
	if (dv_mono) {
		static struct dv_buf	kept;
		size_t			 i = 0;

		kept.len = 0;
		while (i < n) {
			if (p[i] == 0x1b && i + 4 < n && p[i + 1] == ']' &&
			    ((p[i + 2] == '1' && p[i + 3] == '2' && p[i + 4] == ';') ||
			    (p[i + 2] == '1' && p[i + 3] == '1' && p[i + 4] == '2'))) {
				size_t	j = i + 2;

				while (j < n && p[j] != 0x07 &&
				    !(p[j] == 0x1b && j + 1 < n && p[j + 1] == '\\'))
					j++;
				i = j < n ? (p[j] == 0x07 ? j + 1 : j + 2) : n;
				continue;
			}
			dv_buf_append(&kept, &p[i], 1);
			i++;
		}
		p = kept.data;
		n = kept.len;
		if (n == 0)
			return;
	}
	if (nested < 0)
		nested = (getenv("TMUX") != NULL ||
		    getenv("GOBLIN_TMUX") != NULL) ? 1 : 0;
	if (!nested) {
		tty_write(fd, p, n);
		return;
	}
	tty_write(fd, "\033Ptmux;", 8);
	while (n > 0) {
		size_t	i = 0;

		while (i < n && p[i] != 0x1b)
			i++;
		if (i > 0)
			tty_write(fd, p, i);
		if (i < n) {
			tty_write(fd, "\033\033", 2);	/* double the ESC */
			i++;
		}
		p += i;
		n -= i;
	}
	tty_write(fd, "\033\\", 2);
}

static void
put_u16(unsigned char **p, unsigned v)
{
	*(*p)++ = (unsigned char)((v >> 8) & 0xff);
	*(*p)++ = (unsigned char)(v & 0xff);
}

int
dv_tty_size(struct dv_size *sz)
{
	struct winsize	ws;

	memset(sz, 0, sizeof *sz);
	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0)
		return (-1);
	sz->cols = ws.ws_col;
	sz->rows = ws.ws_row;
	if (ws.ws_col > 0 && ws.ws_xpixel > 0)
		sz->xpixel = ws.ws_xpixel / ws.ws_col;
	else
		sz->xpixel = DV_DEFAULT_XPIXEL;
	if (ws.ws_row > 0 && ws.ws_ypixel > 0)
		sz->ypixel = ws.ws_ypixel / ws.ws_row;
	else
		sz->ypixel = DV_DEFAULT_YPIXEL;
	if (sz->cols < 1)
		sz->cols = 1;
	if (sz->rows < 1)
		sz->rows = 1;
	return (0);
}

static int
send_identify(struct dv_conn *c, int display, struct dv_size sz,
    const char *term, int features, const char *hostname, int pointer_caps,
    const char *silkie)
{
	unsigned char	 head[128];
	unsigned char	*p = head;
	char		 cwd[DV_PATH_MAX];
	struct dv_buf	 b;
	size_t		 tlen, clen;
	int		 rc;

	if (term == NULL)
		term = "";
	tlen = strlen(term);
	if (tlen > 63)
		tlen = 63;
	put_u16(&p, sz.cols);
	put_u16(&p, sz.rows);
	put_u16(&p, sz.xpixel);
	put_u16(&p, sz.ypixel);
	*p++ = (unsigned char)(display ? features : 0);
	*p++ = (unsigned char)tlen;
	memcpy(p, term, tlen);
	p += tlen;

	/* The server lives in "/": windows start where the client is. */
	if (getcwd(cwd, sizeof cwd) == NULL)
		cwd[0] = '\0';
	clen = strlen(cwd);
	put_u16(&p, (unsigned)clen);

	memset(&b, 0, sizeof b);
	dv_buf_append(&b, head, (size_t)(p - head));
	dv_buf_append(&b, cwd, clen);

	/* The terminal's XTVERSION name, so the server knows what protocols
	 * (kitty file transfer, goblin-mosh downloads) it can hand on. */
	{
		unsigned char	 hl[2];
		size_t		 hn = hostname ? strlen(hostname) : 0;

		if (hn > 63)
			hn = 63;
		hl[0] = (unsigned char)(hn >> 8);
		hl[1] = (unsigned char)hn;
		dv_buf_append(&b, hl, 2);
		dv_buf_append(&b, hostname, hn);
	}
	const unsigned char pc = (unsigned char)pointer_caps;
	dv_buf_append(&b, &pc, 1);
	{
		size_t		bn = silkie != nullptr ? strlen(silkie) : 0;
		unsigned char	hl[2];

		if (bn > 2047)
			bn = 2047;
		hl[0] = (unsigned char)(bn >> 8);
		hl[1] = (unsigned char)bn;
		dv_buf_append(&b, hl, 2);
		if (bn != 0)
			dv_buf_append(&b, silkie, bn);
	}
	rc = dv_conn_send(c, DV_MSG_IDENTIFY, b.data, b.len);
	dv_buf_free(&b);
	return (rc);
}

static int
send_command(struct dv_conn *c, char **cmdv)
{
	struct dv_buf	 b;
	int		 i;

	memset(&b, 0, sizeof b);
	if (cmdv == NULL) {
		dv_buf_append(&b, "attach", 7);
	} else {
		for (i = 0; cmdv[i] != NULL; i++)
			dv_buf_append(&b, cmdv[i], strlen(cmdv[i]) + 1);
	}
	i = dv_conn_send(c, DV_MSG_COMMAND, b.data, b.len);
	dv_buf_free(&b);
	return (i);
}

static int
send_resize(struct dv_conn *c, struct dv_size sz)
{
	unsigned char	 buf[8];
	unsigned char	*p = buf;

	put_u16(&p, sz.cols);
	put_u16(&p, sz.rows);
	put_u16(&p, sz.xpixel);
	put_u16(&p, sz.ypixel);
	return (dv_conn_send(c, DV_MSG_RESIZE, buf, 8));
}

static int
send_menu_word(struct dv_conn *c, const char *word)
{
	char	*av[3];

	av[0] = const_cast<char *>("menu");
	av[1] = const_cast<char *>(word);
	av[2] = nullptr;
	return (send_command(c, av));
}

static void
handle_host_ev(struct dv_conn *c, struct dv_host *h, struct dv_host_ev *ev,
    struct dv_size *sz)
{
	switch (ev->type) {
	case DV_HEV_OSC:	/* the host answered a window's OSC query */
		if (h->osc.len > 0)
			dv_conn_send(c, DV_MSG_OSC, h->osc.data, h->osc.len);
		h->osc.len = 0;
		break;
	case DV_HEV_SIZE:
		if (ev->size.cols)
			sz->cols = ev->size.cols;
		if (ev->size.rows)
			sz->rows = ev->size.rows;
		if (ev->size.xpixel)
			sz->xpixel = ev->size.xpixel;
		if (ev->size.ypixel)
			sz->ypixel = ev->size.ypixel;
		send_resize(c, *sz);
		break;
	case DV_HEV_MENU:
		send_menu_word(c, "menu");
		break;
	case DV_HEV_MOUSE: {
		unsigned char	m[7];

		m[0] = (unsigned char)(ev->mx >> 8);
		m[1] = (unsigned char)(ev->mx & 0xff);
		m[2] = (unsigned char)(ev->my >> 8);
		m[3] = (unsigned char)(ev->my & 0xff);
		m[4] = (unsigned char)(ev->mcode >> 8);	/* kitty's leave bit is 256 */
		m[5] = (unsigned char)(ev->mcode & 0xff);
		m[6] = (unsigned char)ev->mpress;
		dv_conn_send(c, DV_MSG_MOUSE, m, 7);
		break;
	}
	case DV_HEV_PTY:
		if (ev->has_key) {		/* the key itself: encoded per window */
			unsigned char	kb[128];
			size_t		kn = dv_key_pack(&ev->key, kb, sizeof kb);

			if (kn > 0)
				dv_conn_send(c, DV_MSG_KEY, kb, kn);
		} else if (ev->n > 0)
			dv_conn_send(c, DV_MSG_STDIN, ev->data, ev->n);
		break;
	}
}

static int
connect_or_start(const char *sock_path)
{
	int	fd;

	fd = dv_socket_connect(sock_path);
	if (fd >= 0)
		return (fd);
	return (dv_socket_start_server(sock_path, dv_debug, NULL));
}

int
dv_client_run(const char *sock_path, int display, struct dv_size sz,
    const char *term, char **cmdv)
{
	struct dv_conn	 conn;
	struct dv_host	 host;
	int		 fd, rc = 0, pipes[2];
	struct pollfd	 pf[3];
	int		 got_reply = 0, got_hello = 0, upgrading = 0;
	char		 fatal[512] = "";

	dv_host_init(&host);

	fd = connect_or_start(sock_path);
	if (fd < 0) {
		fprintf(stderr, "goblin-view: connect %s: %s\n", sock_path,
		    strerror(errno));
		return (1);
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	dv_conn_init(&conn, fd);
	signal(SIGPIPE, SIG_IGN);

	if (display) {
		if (set_raw() < 0) {
			fprintf(stderr, "goblin-view: raw tty: %s\n", strerror(errno));
			close(fd);
			return (1);
		}
		signal(SIGWINCH, on_winch);
		signal(SIGINT, on_exit_signal);
		signal(SIGTERM, on_exit_signal);
		signal(SIGHUP, on_exit_signal);
		if (pipe(pipes) == 0) {
			winch_r = pipes[0];
			winch_w = pipes[1];
			fcntl(winch_r, F_SETFL, O_NONBLOCK);
			fcntl(winch_w, F_SETFL, O_NONBLOCK);
		}
		/*
		 * The desktop lives on the alternate screen so the shell
		 * we were started from is untouched underneath. The kitty
		 * keyboard stack is per screen: enter first, then push.
		 */
		{
			static const char init[] =
			    "\033[?1049h\033[H\033[2J"
			    "\033[?1003h\033[?1006h"	/* any-motion + SGR mouse (hover) */
			    "\033]777;caps;v=1:op=reset\033\\"
			    "\033]777;goblin-pointer;v=1:op=mode:mode=1\033\\"
			    "\033[?2004h"		/* bracketed paste (drops arrive wrapped) */
			    "\033]72;t=a;text/uri-list text/plain\033\\";	/* drops, routed */

			tty_write(STDOUT_FILENO, init, sizeof init - 1);
		}
		on_alt_screen = 1;
		dv_host_bootstrap(&host, STDIN_FILENO, STDOUT_FILENO, &sz);
		if (sz.cols < 2 || sz.rows < 2)
			(void)dv_tty_size(&sz);
		if (sz.cols < 2)
			sz.cols = 80;
		if (sz.rows < 2)
			sz.rows = 24;
	}

	if (display) {
		/* This terminal's settings: a [host "..."] section may match
		 * its XTVERSION name. A flag or variable already set wins. */
		struct dv_config	cfg;

		dv_config_defaults(&cfg);
		dv_config_load(&cfg, dv_config_path(), host.name, NULL);
		if (cfg.mono)
			dv_mono = 1;
		dv_host_set_menu_keys(cfg.menu_key);
		dv_limits_apply(&cfg);
	}
	send_identify(&conn, display, sz, term,
	    display ? (dv_host_features(&host) | (dv_mono ? DV_IDENT_MONO : 0)) : 0,
	    display ? host.name : "", display ? host.pointer_caps : 0,
	    display ? host.silkie_blob : "");
	send_command(&conn, cmdv);
	dv_conn_flush(&conn);

	for (;;) {
		int	n, i, nfd_stdin = -1, nfd_winch = -1;

		i = 0;
		pf[i].fd = fd;
		pf[i].events = POLLIN;
		if (conn.out.len)
			pf[i].events |= POLLOUT;
		i++;
		if (display) {
			nfd_stdin = i;
			pf[i].fd = STDIN_FILENO;
			pf[i].events = POLLIN;
			i++;
			if (winch_r >= 0) {
				nfd_winch = i;
				pf[i].fd = winch_r;
				pf[i].events = POLLIN;
				i++;
			}
		}
		/* A held ESC has to expire, or Esc never reaches anyone. */
		n = poll(pf, (nfds_t)i,
		    display && dv_host_pending(&host) ? DV_ESC_TIMEOUT : -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = 1;
			break;
		}
		if (n == 0) {
			struct dv_host_ev	ev;

			while (dv_host_expire(&host, &ev))
				handle_host_ev(&conn, &host, &ev, &sz);
			dv_conn_flush(&conn);
			continue;
		}
		if (nfd_winch >= 0 && (pf[nfd_winch].revents & POLLIN)) {
			char	drain[8];

			while (read(winch_r, drain, sizeof drain) > 0)
				continue;
			if (dv_tty_size(&sz) == 0)
				send_resize(&conn, sz);
		}
		if (nfd_stdin >= 0 && (pf[nfd_stdin].revents & (POLLIN | POLLHUP))) {
			unsigned char	buf[4096];
			ssize_t		nr;

			nr = read(STDIN_FILENO, buf, sizeof buf);
			if (nr == 0 || (nr < 0 && errno != EINTR &&
			    errno != EAGAIN)) {
				dv_conn_send(&conn, DV_MSG_EXIT, NULL, 0);
				dv_conn_flush(&conn);
				break;
			}
			if (nr > 0) {
				struct dv_host_ev	ev;

				dv_host_feed(&host, buf, (size_t)nr);
				while (dv_host_next(&host, &ev))
					handle_host_ev(&conn, &host, &ev, &sz);
			}
		}
		if (pf[0].revents & POLLOUT) {
			if (dv_conn_flush(&conn) < 0) {
				rc = 1;
				break;
			}
		}
		if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
			int	r = dv_conn_read(&conn);

			if (r <= 0)
				break;
			for (;;) {
				int		 type;
				unsigned char	*pl = NULL;
				size_t		 plen = 0;
				int		 st;

				st = dv_msg_take(&conn.in, &type, &pl, &plen);
				if (st == 0)
					break;
				if (st < 0) {
					rc = 1;
					free(pl);
					goto out;
				}
				if (type == DV_MSG_HELLO) {
					unsigned	proto = plen >= 2 ?
					    ((unsigned)pl[0] << 8 | pl[1]) : 0;
					char		ver[32];
					size_t		vl = plen > 2 ? plen - 2 : 0;

					if (vl >= sizeof ver)
						vl = sizeof ver - 1;
					memcpy(ver, pl + 2, vl);
					ver[vl] = '\0';
					got_hello = 1;
					if (proto == DV_PROTOCOL) {
						if (upgrading)
							dv_log("server upgraded to %s", ver);
						upgrading = 0;
					} else if (display && !upgrading && proto >= 4 &&
					    proto < DV_PROTOCOL) {
						/* An older server that can be replaced in place:
						 * ask it to become this program, then carry on. */
						static char	exe[DV_PATH_MAX], real[DV_PATH_MAX];
						char		*up[3];

						if (dv_exe_path(exe, sizeof exe) &&
						    realpath(exe, real) != NULL) {
							up[0] = const_cast<char *>("upgrade");
							up[1] = real;
							up[2] = NULL;
							send_command(&conn, up);
							upgrading = 1;
							dv_log("server is protocol %u; upgrading it", proto);
						}
					}
					if (proto != DV_PROTOCOL && !upgrading) {
						snprintf(fatal, sizeof fatal,
						    "goblin-view: the server at %s is goblin-view %s "
						    "(protocol %u); this is goblin-view %s (protocol %u).\n%s",
						    sock_path, ver, proto, DV_VERSION, (unsigned)DV_PROTOCOL,
						    proto > DV_PROTOCOL ?
						    "The server is newer: run the newer goblin-view instead." :
						    "Its windows keep running. To use this version, "
						    "close them, run `goblin-view kill-server`, and "
						    "start again.");
						if (display) {
							rc = 1;
							free(pl);
							goto out;
						}
						fprintf(stderr, "%s\n", fatal);
						fatal[0] = '\0';
					}
				} else if (type == DV_MSG_STDOUT && plen > 0) {
					if (display && !got_hello) {
						/* A server too old to introduce itself would
						 * ignore every key we type. Say so, don't. */
						snprintf(fatal, sizeof fatal,
						    "goblin-view: the server at %s is an older "
						    "goblin-view that does not speak protocol %u; typed "
						    "keys would be lost.\nIts windows keep running. To "
						    "use this version, close them, run `goblin-view "
						    "kill-server`, and start again.", sock_path,
						    (unsigned)DV_PROTOCOL);
						rc = 1;
						free(pl);
						goto out;
					}
					tty_write(STDOUT_FILENO, pl, plen);
				} else if (type == DV_MSG_PASSTHROUGH && plen > 0) {
					tty_passthrough(STDOUT_FILENO, pl, plen);
				} else if (type == DV_MSG_REPLY) {
					int	status = 0;

					if (display) {	/* an upgrade's answer: not for the screen */
						free(pl);
						continue;
					}
					if (plen >= 4) {
						status = (pl[0] << 24) |
						    (pl[1] << 16) |
						    (pl[2] << 8) | pl[3];
						if (plen > 4)
							fwrite(pl + 4, 1,
							    plen - 4, stdout);
						fflush(stdout);
					}
					rc = status;
					got_reply = 1;
					free(pl);
					if (!display)
						goto out;
					continue;
				} else if (type == DV_MSG_EXIT) {
					free(pl);
					goto out;
				}
				free(pl);
			}
			if (got_reply && !display)
				break;
		}
		/* Keys typed generate commands; push them out now. */
		if (conn.out.len && dv_conn_flush(&conn) < 0) {
			rc = 1;
			break;
		}
	}
out:
	restore_tty();
	if (fatal[0] != '\0')
		fprintf(stderr, "%s\n", fatal);
	dv_buf_free(&host.osc);
	dv_conn_free(&conn);
	close(fd);
	if (winch_r >= 0)
		close(winch_r);
	if (winch_w >= 0)
		close(winch_w);
	return (rc);
}
