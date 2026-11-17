/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 */

#include "dv.hpp"
#include "pstatus.hpp"
#include "agent.hpp"
#include "silkie.hpp"

#include <algorithm>
#include <limits.h>
#include <string.h>
#include <unistd.h>

static int
max_z(struct dv_desktop *d)
{
	struct dv_window	*w;
	int			 z = 0;

	TAILQ_FOREACH(w, &d->windows, entry) {
		if (w->z > z)
			z = w->z;
	}
	return (z);
}

/*
 * A new framed window: about three-quarters of the desktop, cascaded
 * down-right from the last one so a fresh window never lands exactly on
 * top of the previous. Everything is a function of the live desktop.
 */
void
dv_window_default_geom(struct dv_desktop *d, struct dv_geom *g)
{
	unsigned	W = d->cols, H = d->rows;
	unsigned	iw, ih;
	int		step = (int)(d->cascade % 6);

	iw = W > 8 ? (W * 3) / 4 : W;
	ih = H > 6 ? (H * 3) / 4 : H;
	if (iw > 2)
		iw -= 2;		/* leave room for the frame */
	if (ih > 2)
		ih -= 2;
	g->cols = iw < DV_MIN_INNER_COLS ? DV_MIN_INNER_COLS : iw;
	g->rows = ih < DV_MIN_INNER_ROWS ? DV_MIN_INNER_ROWS : ih;
	g->x = 1 + step * 3;
	g->y = 1 + step * 2;
	dv_clamp_geom(W, H, 1, g);
	d->cascade++;
}

/*
 * Six lines of instruction painted into a fresh window's own screen.
 * They are ordinary cells: the shell's first prompt lands just below,
 * and anything that clears the screen scrolls them away. People arrive
 * without the 1993 manual, so the menu key is spelled out.
 */
void
dv_window_banner(struct dv_window *w)
{
	static const char	*lines[] = {
	    "\033[1;36m Odin Seat " DV_VERSION "\033[0m  Overlapping terminal windows.\r\n",
	    " The \033[1;33mmenu key\033[0m is a tap of \033[1;33mAlt\033[0m"
	    " (Option on a Mac): press, let go. Or press \033[1;33mCtrl-\\\033[0m.\r\n",
	    " Menu: \033[1;33mO\033[0mpen \033[1;33mC\033[0mlose"
	    " \033[1;33mR\033[0mearrange \033[1;33mZ\033[0moom"
	    " \033[1;33m1-9\033[0m switch \033[1;33mD\033[0metach"
	    " \033[1;33mQ\033[0muit  \033[1;33m?\033[0m help.\r\n",
	    " Move/resize: \033[1;33mR\033[0m then \033[1;33mM\033[0m or"
	    " \033[1;33mR\033[0m, arrows (Shift=5), Enter. Or drag the frame"
	    " with the mouse.\r\n",
	    " Ghostty: set \033[1;33mmacos-option-as-alt = true\033[0m in"
	    " ~/.config/ghostty/config so the Option tap reaches odin-seat.\r\n",
	    " Windows overlap and stay live when you detach.  Reattach with"
	    " \033[1;33modin-seat\033[0m.\r\n\r\n",
	};
	unsigned	i;

	for (i = 0; i < sizeof lines / sizeof lines[0]; i++)
		dv_vt_write(w, lines[i], strlen(lines[i]));
}

void
dv_window_set_pty_size(struct dv_window *w)
{
	if (!w->locked) {
		w->lcols = w->geom.cols;
		w->lrows = w->geom.rows;
	}
	if (w->screen.cols != w->lcols || w->screen.rows != w->lrows) {
		dv_pointer_cancel(w);
		dv_silkie_cancel_window(w);
	}
	dv_screen_resize(&w->screen, w->lcols, w->lrows);
	dv_pty_resize(w);
}

/*
 * Honour a program's CSI 8 ; rows ; cols t: set the window's content size,
 * then let dv_clamp_geom cap it to the desktop (largest that fits in each
 * dimension) and shift the top-left in when it would run past the right or
 * bottom edge. A zero dimension means "as large as fits". Left alone while
 * zoomed or hidden. The pty and screen follow, so the program gets the
 * size it asked for (or the capped one).
 */
void
dv_window_resize_to(struct dv_window *w, unsigned cols, unsigned rows)
{
	struct dv_desktop	*d = &w->session->desktop;
	int			 framed = dv_window_framed(w);
	unsigned		 cc = framed ? 2u : 0u, cr = framed ? 2u : 0u;

	if (w->zoomed || w->hidden || w->aside)
		return;
	if (cols == 0)
		cols = d->cols > cc ? d->cols - cc : 1;
	if (rows == 0)
		rows = d->rows > cr ? d->rows - cr : 1;
	w->geom.cols = cols;
	w->geom.rows = rows;
	dv_clamp_geom(d->cols, d->rows, framed, &w->geom);
	dv_window_set_pty_size(w);
	w->session->dirty = 1;
}

void
dv_window_raise(struct dv_window *w)
{
	struct dv_desktop *d = &w->session->desktop;
	int top = max_z(d);

	if (top == INT_MAX) {
		std::vector<dv_window *> windows;
		struct dv_window *it;

		TAILQ_FOREACH(it, &d->windows, entry)
			windows.push_back(it);
		if (windows.size() >= INT_MAX)
			dv_fatal("window stacking order exhausted");
		std::sort(windows.begin(), windows.end(), [](const auto *a, const auto *b) {
			return (a->z < b->z);
		});
		int z = 0;
		for (auto *window : windows)
			window->z = z++;
		top = z - 1;
	}
	w->z = top + 1;
}

void
dv_window_focus(struct dv_window *w)
{
	struct dv_window	*prev = w->session->desktop.focused;

	w->session->desktop.focused = w;
	dv_window_raise(w);
	if (prev != w)
		dv_silkie_focus(w->session, prev);
}

/* Focus without raising: the window keeps its place in the stack. */
void
dv_window_focus_noraise(struct dv_window *w)
{
	struct dv_window	*prev = w->session->desktop.focused;

	w->session->desktop.focused = w;
	if (prev != w)
		dv_silkie_focus(w->session, prev);
}

struct dv_window *
dv_window_find(struct dv_session *s, unsigned id)
{
	struct dv_window	*w;

	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		if (w->id == id)
			return (w);
	}
	return (NULL);
}

struct dv_window *
dv_window_open(struct dv_session *s, const char *cmd, char *const *cmd_argv,
    int snap, const char *cwd)
{
	struct dv_desktop	*d = &s->desktop;
	struct dv_window	*w;
	unsigned		 id;
	int			 first;

	id = dv_desktop_next_window_id(d);
	if (id == 0)
		return (NULL);

	w = dv_new<dv_window>();
	w->id = id;
	w->session = s;
	w->pty_fd = -1;
	w->pipe_fd = -1;
	first = TAILQ_EMPTY(&d->windows);

	if (cmd != NULL && cmd[0] != '\0')
		dv_strlcpy(w->cmd, cmd, sizeof w->cmd);
	else if (cmd_argv != NULL && cmd_argv[0] != NULL)
		dv_strlcpy(w->cmd, cmd_argv[0], sizeof w->cmd);
	else
		dv_strlcpy(w->cmd, "shell", sizeof w->cmd);
	dv_strlcpy(w->title, w->cmd, sizeof w->title);
	if (cwd != NULL)
		dv_strlcpy(w->cwd, cwd, sizeof w->cwd);
	if (cmd_argv != NULL) {		/* remembered for save/restore */
		int	i;

		for (i = 0; cmd_argv[i] != NULL; i++) {
			size_t	l = strlen(cmd_argv[i]) + 1;

			if (w->cmdv_len + l > sizeof w->cmdv)
				break;
			memcpy(w->cmdv + w->cmdv_len, cmd_argv[i], l);
			w->cmdv_len += (unsigned)l;
		}
	}

	/*
	 * Every window is a framed float, cascaded onto the backdrop --
	 * overlapping desk, not a tiled spine. A snap (1-9) places instead.
	 */
	w->framed = 1;
	w->zoomed = 0;
	if (snap >= 1 && snap <= DV_SNAP_COUNT)
		dv_snap_geom(d->cols, d->rows, snap, 1, &w->geom);
	else
		dv_window_default_geom(d, &w->geom);
	w->saved = w->geom;
	w->lcols = w->geom.cols;
	w->lrows = w->geom.rows;
	dv_screen_init(&w->screen, w->lcols, w->lrows);

	if (dv_pty_spawn(w, cmd, cmd_argv) != 0) {
		dv_screen_free(&w->screen);
		free(w->notification_pending);
	free(w);
		return (NULL);
	}

	TAILQ_INSERT_TAIL(&d->windows, w, entry);
	dv_window_focus(w);
	if (first)
		dv_window_banner(w);
	return (w);
}

struct dv_window *
dv_window_split(struct dv_window *w, int below, char *const *cmd_argv,
    const char *cwd)
{
	struct dv_session *s = w->session;
	struct dv_geom original = w->geom, first, second;
	struct dv_window *created;
	int framed = dv_frame_fits(s->desktop.cols, s->desktop.rows);

	/* A zoomed window divides the full desktop into two framed windows. */
	if (w->zoomed) {
		unsigned chrome = framed ? 2u : 0u;

		original.cols -= chrome;
		original.rows -= chrome;
	}
	if (!dv_split_geom(&original, framed, below, &first, &second))
		return (nullptr);
	/* Spawn first: an exhausted PTY or failed fork leaves the original
	 * window's geometry and zoom state unchanged. */
	created = dv_window_open(s, nullptr, cmd_argv, 0,
	    cwd != nullptr && cwd[0] != '\0' ? cwd : w->cwd);
	if (created == nullptr)
		return (nullptr);
	dv_pointer_cancel(w);
	dv_silkie_cancel_window(w);
	w->zoomed = 0;
	w->framed = created->framed = 1;
	w->geom = w->saved = first;
	created->geom = created->saved = second;
	dv_window_set_pty_size(w);
	dv_window_set_pty_size(created);
	s->dirty = 1;
	return (created);
}

/* A window taken over from a previous server: its pty and program are
 * already there; the screen is filled in by the state reader. */
struct dv_window *
dv_window_adopt(struct dv_session *s, unsigned id, int pty_fd, pid_t pid,
    unsigned cols, unsigned rows)
{
	struct dv_window	*w = dv_new<dv_window>();

	w->id = id;
	w->session = s;
	w->pty_fd = pty_fd;
	w->pid = pid;
	w->pipe_fd = -1;
	w->framed = 1;
	w->lcols = cols;
	w->lrows = rows;
	dv_screen_init(&w->screen, cols, rows);
	TAILQ_INSERT_TAIL(&s->desktop.windows, w, entry);
	return (w);
}

void
dv_window_close(struct dv_window *w)
{
	dv_agent_window_closed(w);
	struct dv_desktop	*d = &w->session->desktop;
	struct dv_window	*next;

	/*
	 * Repaint the desktop after any close, including a window whose
	 * shell just exited on Ctrl-D. Without this the dead window's frame
	 * lingers on screen until the next unrelated event, which reads as
	 * "the close needs another keypress" -- and invites a second Ctrl-D
	 * that lands on the window now focused, closing it too.
	 */
	w->session->dirty = 1;
	if (w->session->kill_confirm == w->id)
		w->session->kill_confirm = 0;
	dv_silkie_reset_pane(w);	/* pty is still open; cancel reaches the program */
	dv_pstatus_close(w);		/* and its status leaves the terminals */
	{
		unsigned	k;

		for (k = 0; k < DV_OSC_ROUTES; k++)	/* no reply can find it now */
			if (w->session->routes[k].win == w->id)
				w->session->routes[k].osc = 0;
	}

	dv_xfer_free(w);
	if (w->pipe_fd >= 0)
		close(w->pipe_fd);
	dv_buf_free(&w->pty_out);
	dv_pty_close(w);
	dv_screen_free(&w->screen);
	dv_buf_free(&w->cap);
	dv_kitty_free(w);
	dv_kitty_close(w);
	dv_silkie_free_window(w);
	dv_buf_free(&w->host_out);
	TAILQ_REMOVE(&d->windows, w, entry);
	if (d->focused == w) {
		struct dv_window	*it;

		next = nullptr;
		TAILQ_FOREACH(it, &d->windows, entry)
			next = it;
		d->focused = next;
		if (next != nullptr)
			dv_window_raise(next);
		dv_window_report_focus(next);
		dv_silkie_push(w->session);
	}
	free(w->notification_pending);
	free(w);
}
