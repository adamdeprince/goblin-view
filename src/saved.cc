/*
 * Sessions saved to a file and recreated from it: every window's place,
 * size, title, flags, the working directory its program is in now, and
 * the command it was started with. A restore starts the programs afresh
 * in those directories; it cannot bring back what was running inside.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

#include "dv.hpp"

const char *
dv_saved_path(void)
{
	static char	path[DV_PATH_MAX];
	const char	*xdg = getenv("XDG_STATE_HOME"), *home = getenv("HOME");

	if (xdg != NULL && xdg[0] == '/')
		snprintf(path, sizeof path, "%s/odin-seat/sessions", xdg);
	else
		snprintf(path, sizeof path, "%s/.local/state/odin-seat/sessions",
		    home != NULL ? home : "/tmp");
	return (path);
}

/* The directory a running program is in; its window's start directory
 * when that cannot be known. */
static void
proc_cwd(const struct dv_window *w, char *out, size_t cap)
{
#ifdef __APPLE__
	struct proc_vnodepathinfo	vpi;

	if (w->pid > 0 && proc_pidinfo(w->pid, PROC_PIDVNODEPATHINFO, 0, &vpi,
	    sizeof vpi) == (int)sizeof vpi && vpi.pvi_cdir.vip_path[0] == '/') {
		dv_strlcpy(out, vpi.pvi_cdir.vip_path, cap);
		return;
	}
#else
	char	link[64];
	ssize_t	n;

	snprintf(link, sizeof link, "/proc/%ld/cwd", (long)w->pid);
	n = w->pid > 0 ? readlink(link, out, cap - 1) : -1;
	if (n > 0) {
		out[n] = '\0';
		return;
	}
#endif
	dv_strlcpy(out, w->cwd, cap);
}

/* Strings on their own line: backslash and newline escaped. */
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

static void
mkdirs(const char *path)
{
	char	dir[DV_PATH_MAX], *p;

	dv_strlcpy(dir, path, sizeof dir);
	p = strrchr(dir, '/');
	if (p == NULL || p == dir)
		return;
	*p = '\0';
	for (p = dir + 1; *p != '\0'; p++)
		if (*p == '/') {
			*p = '\0';
			(void)mkdir(dir, 0700);
			*p = '/';
		}
	(void)mkdir(dir, 0700);
}

int
dv_sessions_save(const char *path, struct dv_buf *report)
{
	struct dv_session	*s;
	struct dv_window	*w;
	struct dv_buf		 b;
	char			 tmp[DV_PATH_MAX + 16], line[256];
	FILE			*f;
	int			 nsess = 0, nwin = 0;

	memset(&b, 0, sizeof b);
	dv_buf_append(&b, "odin-seat-sessions 1\n", sizeof "odin-seat-sessions 1\n" - 1);
	TAILQ_FOREACH(s, &dv_srv->sessions, entry) {
		nsess++;
		snprintf(line, sizeof line, "session %u %u\n", s->desktop.cols, s->desktop.rows);
		dv_buf_append(&b, line, strlen(line));
		put_str(&b, "name", s->name);
		TAILQ_FOREACH(w, &s->desktop.windows, entry) {
			const struct dv_geom	*g = w->zoomed ? &w->saved : &w->geom;
			char			 cwd[DV_PATH_MAX];
			unsigned		 i;

			nwin++;
			snprintf(line, sizeof line,
			    "window %u %d %d %u %u %d %d %d %d %d %d\n", w->id, g->x, g->y,
			    g->cols, g->rows, w->z, w->framed, w->zoomed, w->hidden,
			    s->desktop.focused == w, w->title_locked);
			dv_buf_append(&b, line, strlen(line));
			put_str(&b, "title", w->title);
			proc_cwd(w, cwd, sizeof cwd);
			put_str(&b, "cwd", cwd);
			for (i = 0; i < w->cmdv_len; i += (unsigned)strlen(w->cmdv + i) + 1)
				put_str(&b, "arg", w->cmdv + i);
			dv_buf_append(&b, "end\n", 4);
		}
	}
	mkdirs(path);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	f = fopen(tmp, "w");
	if (f == NULL || fwrite(b.data, 1, b.len, f) != b.len || fclose(f) != 0 ||
	    rename(tmp, path) != 0) {
		char	msg[DV_PATH_MAX + 64];

		snprintf(msg, sizeof msg, "cannot write %s: %s\n", path, strerror(errno));
		dv_buf_append(report, msg, strlen(msg));
		if (f != NULL)
			unlink(tmp);
		dv_buf_free(&b);
		return (0);
	}
	dv_buf_free(&b);
	snprintf(line, sizeof line, "saved %d session%s, %d window%s to %s\n", nsess,
	    nsess == 1 ? "" : "s", nwin, nwin == 1 ? "" : "s", path);
	dv_buf_append(report, line, strlen(line));
	return (1);
}

int
dv_sessions_restore(const char *path, struct dv_buf *report)
{
	FILE			*f = fopen(path, "r");
	char			 line[2048], msg[DV_PATH_MAX + 64];
	struct dv_session	*s = NULL;
	struct dv_window	*w = NULL, *focus = NULL;
	int			 skipping = 0, nsess = 0, nwin = 0;
	/* a window being read */
	unsigned		 wid = 0, wcols = 0, wrows = 0;
	int			 wx = 0, wy = 0, wz = 0, wframed = 1, wzoomed = 0, whidden = 0,
				 wfocused = 0, wlocked = 0;
	char			 wtitle[64] = "", wcwd[DV_PATH_MAX] = "";
	char			*wargv[64];
	int			 wargc = 0;

	if (f == NULL) {
		snprintf(msg, sizeof msg, "cannot read %s: %s\n", path, strerror(errno));
		dv_buf_append(report, msg, strlen(msg));
		return (0);
	}
	if (fgets(line, sizeof line, f) == NULL ||
	    strncmp(line, "odin-seat-sessions ", sizeof "odin-seat-sessions " - 1) != 0) {
		fclose(f);
		snprintf(msg, sizeof msg, "%s is not a odin-seat sessions file\n", path);
		dv_buf_append(report, msg, strlen(msg));
		return (0);
	}
	while (fgets(line, sizeof line, f) != NULL) {
		size_t	n = strlen(line);

		if (n > 0 && line[n - 1] == '\n')
			line[--n] = '\0';
		if (strncmp(line, "session ", 8) == 0) {
			struct dv_size	sz;

			memset(&sz, 0, sizeof sz);
			sscanf(line + 8, "%u %u", &sz.cols, &sz.rows);
			s = NULL;
			skipping = 0;
			/* the name follows; create once it is known */
			if (fgets(line, sizeof line, f) == NULL)
				break;
			n = strlen(line);
			if (n > 0 && line[n - 1] == '\n')
				line[--n] = '\0';
			if (strncmp(line, "name ", 5) != 0)
				continue;
			unescape(line + 5);
			if (dv_session_find(line + 5) != NULL) {
				snprintf(msg, sizeof msg, "session %s exists, left alone\n", line + 5);
				dv_buf_append(report, msg, strlen(msg));
				skipping = 1;
				continue;
			}
			if (sz.cols == 0 || sz.rows == 0) {
				sz.cols = 80;
				sz.rows = 24;
			}
			sz.xpixel = DV_DEFAULT_XPIXEL;
			sz.ypixel = DV_DEFAULT_YPIXEL;
			s = dv_session_create(line + 5, sz);
			nsess++;
			focus = NULL;
		} else if (strncmp(line, "window ", 7) == 0) {
			wargc = 0;
			wtitle[0] = wcwd[0] = '\0';
			sscanf(line + 7, "%u %d %d %u %u %d %d %d %d %d %d", &wid, &wx, &wy, &wcols,
			    &wrows, &wz, &wframed, &wzoomed, &whidden, &wfocused, &wlocked);
		} else if (strncmp(line, "title ", 6) == 0) {
			unescape(line + 6);
			dv_strlcpy(wtitle, line + 6, sizeof wtitle);
		} else if (strncmp(line, "cwd ", 4) == 0) {
			unescape(line + 4);
			dv_strlcpy(wcwd, line + 4, sizeof wcwd);
		} else if (strncmp(line, "arg ", 4) == 0) {
			unescape(line + 4);
			if (wargc < 63)
				wargv[wargc++] = xstrdup(line + 4);
		} else if (strcmp(line, "end") == 0) {
			int	i;

			wargv[wargc] = NULL;
			if (s != NULL && !skipping) {
				w = dv_window_open(s, NULL, wargc > 0 ? wargv : NULL, 0,
				    wcwd[0] ? wcwd : NULL);
				if (w != NULL) {
					nwin++;
					if (dv_window_find(s, wid) == NULL && wid > 0)
						w->id = wid;
					w->geom.x = wx;
					w->geom.y = wy;
					if (wcols >= DV_MIN_INNER_COLS && wrows >= DV_MIN_INNER_ROWS) {
						w->geom.cols = wcols;
						w->geom.rows = wrows;
					}
					w->framed = wframed;
					dv_clamp_geom(s->desktop.cols, s->desktop.rows, w->framed, &w->geom);
					w->saved = w->geom;
					w->z = wz;
					w->hidden = whidden;
					w->title_locked = wlocked;
					if (wtitle[0] != '\0')
						dv_strlcpy(w->title, wtitle, sizeof w->title);
					if (wzoomed)
						dv_desktop_apply_zoom(w);
					dv_window_set_pty_size(w);
					if (wfocused)
						focus = w;
				} else {
					snprintf(msg, sizeof msg, "window %u: failed to start\n", wid);
					dv_buf_append(report, msg, strlen(msg));
				}
				if (s->desktop.focused == nullptr || focus != NULL)
					s->desktop.focused = focus != NULL ? focus : s->desktop.focused;
			}
			for (i = 0; i < wargc; i++)
				free(wargv[i]);
			wargc = 0;
		}
	}
	fclose(f);
	snprintf(msg, sizeof msg, "restored %d session%s, %d window%s from %s\n", nsess,
	    nsess == 1 ? "" : "s", nwin, nwin == 1 ? "" : "s", path);
	dv_buf_append(report, msg, strlen(msg));
	return (1);
}
