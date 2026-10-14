/*
 * File transfer: the endpoint of last resort.
 *
 * A program in a window may offer files -- kitty's file transfer
 * protocol (OSC 5113, ac=send) or goblin-mosh's background download
 * (OSC 777 goblin-download) -- or ask to read files from here (OSC
 * 5113, ac=receive). When the terminal we are attached to speaks the
 * protocol (its XTVERSION name says kitty, or goblin) the request is
 * handed to it verbatim and its replies are routed back to the window.
 * Otherwise the buck stops here: the user is asked, once per transfer,
 * whether to save into the downloads directory (or to let the program
 * read the named files). Incoming files are staged and published
 * without overwriting anything (report.pdf, then report.pdf.1), sizes
 * are verified, nothing is opened or executed, and the program is
 * answered on its pty in the protocol it spoke. A pasted "y" cannot
 * approve: the answer comes from a key the user typed.
 */

#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "dv.hpp"

#define XF_FILES	256		/* entries per transfer (a listed tree) */
#define XF_CHUNK	2048		/* raw bytes per data escape we send */
#define XF_MAX_GOBLIN	(64LL * 1024 * 1024)
#define XF_LOWATER	16384		/* queue more when the pty queue is below */

enum xf_dir { XD_DOWN = 0, XD_UP = 1 };	/* to us, or from us */
enum xf_state { XS_ASK = 1, XS_GO, XS_END };

struct xf_file {
	char		 fid[64];	/* the program's id (down) or ours (up) */
	char		 name[256];	/* basename to publish as */
	char		 tmp[DV_PATH_MAX];	/* staging path while receiving */
	char		 path[DV_PATH_MAX];	/* local path (up) */
	int		 fd;
	long long	 sz;		/* announced or stat size, -1 unknown */
	long long	 got;		/* bytes written (down) or read (up) */
	long long	 mtime_ns;
	unsigned	 perm;
	int		 zip;		/* zlib on the wire */
	z_stream	 z;
	int		 zinit;
	int		 kind;		/* 0 regular, 1 directory, 2 other */
	int		 want;		/* up: the program asked for its data */
	int		 done;
};

struct dv_xfer {
	unsigned	 proto;		/* 5113 or 777 */
	int		 dir;
	int		 state;
	char		 id[64];
	int		 quiet;		/* kitty q= */
	int		 reply;		/* goblin: it wants status replies */
	int		 confirm;	/* goblin: it waits for "approved" */
	int		 ended;		/* goblin: op=end arrived */
	int		 asked;		/* the prompt has been shown */
	char		 text[4][100];	/* the prompt */
	int		 nlines;
	int		 want;		/* kitty receive: paths announced (sz=) */
	int		 sending;	/* up: index streaming now, -1 none */
	char		 dldir[DV_PATH_MAX];
	struct xf_file	 files[XF_FILES];
	int		 nfiles;
};

int (*dv_xfer_forward_hook)(struct dv_window *, const char *, unsigned,
    const char *, size_t) = nullptr;

/* ---- small helpers ---- */

struct kv {
	const char	*k, *v;
	size_t		 kl, vl;
};

/* Split "k=v<sep>k=v..." (values may hold '=': base64 padding). */
static size_t
split_kv(const char *b, size_t n, char sep, struct kv *out, size_t cap)
{
	size_t	i = 0, c = 0;

	while (i < n && c < cap) {
		size_t	st = i, eq = 0;

		while (i < n && b[i] != sep) {
			if (eq == 0 && b[i] == '=')
				eq = i;
			i++;
		}
		if (i > st) {
			out[c].k = b + st;
			if (eq) {
				out[c].kl = eq - st;
				out[c].v = b + eq + 1;
				out[c].vl = i - eq - 1;
			} else {
				out[c].kl = i - st;
				out[c].v = b + i;
				out[c].vl = 0;
			}
			c++;
		}
		if (i < n)
			i++;
	}
	return (c);
}

static const struct kv *
kget(const struct kv *kv, size_t n, const char *key)
{
	size_t	kl = strlen(key), i;

	for (i = 0; i < n; i++)
		if (kv[i].kl == kl && memcmp(kv[i].k, key, kl) == 0)
			return (&kv[i]);
	return (nullptr);
}

static void
kstr(const struct kv *kv, size_t n, const char *key, char *out, size_t cap)
{
	const struct kv	*e = kget(kv, n, key);
	size_t		 l = e ? e->vl : 0;

	if (l >= cap)
		l = cap - 1;
	if (e)
		memcpy(out, e->v, l);
	out[l] = '\0';
}

static long long
knum(const struct kv *kv, size_t n, const char *key, long long def)
{
	const struct kv	*e = kget(kv, n, key);
	long long	 v = 0;
	size_t		 i;

	if (e == nullptr || e->vl == 0)
		return (def);
	for (i = 0; i < e->vl; i++) {
		if (e->v[i] < '0' || e->v[i] > '9')
			return (def);
		v = v * 10 + (e->v[i] - '0');
	}
	return (v);
}

static int
kis(const struct kv *kv, size_t n, const char *key, const char *val)
{
	const struct kv	*e = kget(kv, n, key);
	size_t		 vl = strlen(val);

	return (e != nullptr && e->vl == vl && memcmp(e->v, val, vl) == 0);
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t
b64enc(const unsigned char *in, size_t n, char *out, size_t cap)
{
	size_t	o = 0, i;

	for (i = 0; i < n; i += 3) {
		unsigned	v = (unsigned)in[i] << 16;

		if (o + 5 > cap)
			break;
		if (i + 1 < n)
			v |= (unsigned)in[i + 1] << 8;
		if (i + 2 < n)
			v |= in[i + 2];
		out[o++] = B64[(v >> 18) & 63];
		out[o++] = B64[(v >> 12) & 63];
		out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < n ? B64[v & 63] : '=';
	}
	out[o] = '\0';
	return (o);
}

/* Decode, tolerating missing padding; -1 on a bad character. */
long
dv_b64_decode(const char *in, size_t n, unsigned char *out, size_t cap)
{
	unsigned	acc = 0;
	int		bits = 0;
	size_t		i, o = 0;

	for (i = 0; i < n; i++) {
		int	c = (unsigned char)in[i], v;

		if (c == '=' || c == '\n' || c == '\r' || c == ' ')
			continue;
		if (c >= 'A' && c <= 'Z')
			v = c - 'A';
		else if (c >= 'a' && c <= 'z')
			v = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			v = c - '0' + 52;
		else if (c == '+' || c == '-')
			v = 62;
		else if (c == '/' || c == '_')
			v = 63;
		else
			return (-1);
		acc = (acc << 6) | (unsigned)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o < cap)
				out[o] = (unsigned char)(acc >> bits);
			o++;
		}
	}
	return (o > cap ? (long)cap : (long)o);
}

static void
bput(struct dv_buf *b, const char *s)
{
	dv_buf_append(b, s, strlen(s));
}

static void
bfmt(struct dv_buf *b, const char *fmt, ...)
{
	char	tmp[2048];
	va_list	ap;
	int	n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n > 0)
		dv_buf_append(b, tmp, n >= (int)sizeof tmp ? sizeof tmp - 1 : (size_t)n);
}

/* ";key=<base64 of s>" */
static void
bput_b64(struct dv_buf *b, const char *key, const char *s)
{
	char	enc[1600];

	b64enc((const unsigned char *)s, strlen(s), enc, sizeof enc);
	bfmt(b, ";%s=%s", key, enc);
}

static void
fmt_size(long long n, char *out, size_t cap)
{
	if (n < 0)
		snprintf(out, cap, "unknown size");
	else if (n < 1024)
		snprintf(out, cap, "%lld B", n);
	else if (n < 1024 * 1024)
		snprintf(out, cap, "%.1f KB", (double)n / 1024);
	else if (n < 1024LL * 1024 * 1024)
		snprintf(out, cap, "%.1f MB", (double)n / (1024 * 1024));
	else
		snprintf(out, cap, "%.2f GB", (double)n / (1024.0 * 1024 * 1024));
}

/* $HOME shown as ~ */
static void
pretty_path(const char *p, char *out, size_t cap)
{
	const char	*home = getenv("HOME");
	size_t		 hl = home ? strlen(home) : 0;

	if (hl > 1 && strncmp(p, home, hl) == 0 && (p[hl] == '/' || p[hl] == 0))
		snprintf(out, cap, "~%s", p + hl);
	else
		snprintf(out, cap, "%s", p);
}

/*
 * Where files land: $GOBLIN_VIEW_DOWNLOAD_DIR, else $XDG_DOWNLOAD_DIR,
 * else the XDG user-dirs file's entry, else ~/Downloads; whichever of
 * those exists, falling back to $HOME.
 */
static int
is_dir(const char *p)
{
	struct stat	st;

	return (p[0] != '\0' && stat(p, &st) == 0 && S_ISDIR(st.st_mode));
}

static void
download_dir(char *out, size_t cap)
{
	const char	*e, *home = getenv("HOME");
	char		 p[DV_PATH_MAX];
	FILE		*f;

	if (home == nullptr || *home == '\0')
		home = "/tmp";
	e = getenv("GOBLIN_VIEW_DOWNLOAD_DIR");
	if (e != nullptr && is_dir(e)) {
		snprintf(out, cap, "%s", e);
		return;
	}
	e = getenv("XDG_DOWNLOAD_DIR");
	if (e != nullptr && is_dir(e)) {
		snprintf(out, cap, "%s", e);
		return;
	}
	snprintf(p, sizeof p, "%s/.config/user-dirs.dirs", home);
	f = fopen(p, "r");
	if (f != nullptr) {
		char	line[DV_PATH_MAX + 64];

		while (fgets(line, sizeof line, f) != nullptr) {
			char	*v, *q, val[DV_PATH_MAX];

			if (strncmp(line, "XDG_DOWNLOAD_DIR=", 17) != 0)
				continue;
			v = line + 17;
			if (*v == '"')
				v++;
			q = strchr(v, '"');
			if (q != nullptr)
				*q = '\0';
			q = strchr(v, '\n');
			if (q != nullptr)
				*q = '\0';
			if (strncmp(v, "$HOME", 5) == 0)
				snprintf(val, sizeof val, "%s%s", home, v + 5);
			else
				snprintf(val, sizeof val, "%s", v);
			if (is_dir(val)) {
				fclose(f);
				snprintf(out, cap, "%s", val);
				return;
			}
		}
		fclose(f);
	}
	snprintf(p, sizeof p, "%s/Downloads", home);
	if (is_dir(p)) {
		snprintf(out, cap, "%s", p);
		return;
	}
	snprintf(out, cap, "%s", home);
}

/* A publishable basename: no directories, no control characters. */
static void
safe_name(const char *in, size_t n, char *out, size_t cap)
{
	const char	*base = in;
	size_t		 i, o = 0;

	for (i = 0; i < n; i++)
		if (in[i] == '/')
			base = in + i + 1;
	n -= (size_t)(base - in);
	for (i = 0; i < n && o + 1 < cap; i++) {
		unsigned char	c = (unsigned char)base[i];

		out[o++] = (c < 0x20 || c == 0x7f || c == '/') ? '_' : (char)c;
	}
	out[o] = '\0';
	if (o == 0 || strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
		snprintf(out, cap, "download");
}

/* Expand ~ and make relative paths relative to $HOME. */
static void
expand_path(const char *in, size_t n, char *out, size_t cap)
{
	const char	*home = getenv("HOME");
	char		 tmp[DV_PATH_MAX];

	if (n >= sizeof tmp)
		n = sizeof tmp - 1;
	memcpy(tmp, in, n);
	tmp[n] = '\0';
	if (home == nullptr)
		home = "/";
	if (tmp[0] == '~' && (tmp[1] == '/' || tmp[1] == '\0'))
		snprintf(out, cap, "%s%s", home, tmp + 1);
	else if (tmp[0] != '/')
		snprintf(out, cap, "%s/%s", home, tmp);
	else
		snprintf(out, cap, "%s", tmp);
}

/* Publish a staged file under a name nothing else has: name, name.1 ... */
static int
publish(const char *tmp, const char *dir, const char *name, char *final,
    size_t cap)
{
	int	k, fd;

	for (k = 0; k < 10000; k++) {
		if (k == 0)
			snprintf(final, cap, "%s/%s", dir, name);
		else
			snprintf(final, cap, "%s/%s.%d", dir, name, k);
		fd = open(final, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0) {
			if (errno == EEXIST)
				continue;
			return (-1);
		}
		close(fd);
		if (rename(tmp, final) < 0) {
			unlink(final);
			return (-1);
		}
		return (0);
	}
	return (-1);
}

/* ---- transfer objects ---- */

static struct dv_xfer *
xfer_new(struct dv_window *w, unsigned proto, int dir, const char *id)
{
	struct dv_xfer	*x = dv_new<dv_xfer>();

	x->proto = proto;
	x->dir = dir;
	x->state = XS_ASK;
	x->sending = -1;
	dv_strlcpy(x->id, id, sizeof x->id);
	download_dir(x->dldir, sizeof x->dldir);
	w->xfer = x;
	return (x);
}

static void
file_close(struct xf_file *f, int keep_tmp)
{
	if (f->fd >= 0) {
		close(f->fd);
		f->fd = -1;
	}
	if (f->zinit) {
		if (f->want)		/* deflating (up) */
			deflateEnd(&f->z);
		else
			inflateEnd(&f->z);
		f->zinit = 0;
	}
	if (!keep_tmp && f->tmp[0] != '\0') {
		unlink(f->tmp);
		f->tmp[0] = '\0';
	}
}

static void
xfer_drop(struct dv_window *w)
{
	struct dv_xfer	*x = w->xfer;
	int		 i;

	if (x == nullptr)
		return;
	for (i = 0; i < x->nfiles; i++)
		file_close(&x->files[i], 0);
	if (w->session != nullptr && w->session->xfer_prompt &&
	    w->session->xfer_prompt_win == w->id) {
		w->session->xfer_prompt = 0;
		w->session->dirty = 1;
	}
	free(x);
	w->xfer = nullptr;
}

static struct xf_file *
file_add(struct dv_xfer *x, const char *fid)
{
	struct xf_file	*f;

	if (x->nfiles >= XF_FILES)
		return (nullptr);
	f = &x->files[x->nfiles++];
	memset(f, 0, sizeof *f);
	f->fd = -1;
	f->sz = -1;
	dv_strlcpy(f->fid, fid, sizeof f->fid);
	return (f);
}

static struct xf_file *
file_find(struct dv_xfer *x, const char *fid)
{
	int	i;

	for (i = 0; i < x->nfiles; i++)
		if (strcmp(x->files[i].fid, fid) == 0)
			return (&x->files[i]);
	return (nullptr);
}

/* Open a staging file in the download directory. */
static int
stage_open(struct dv_window *w, struct xf_file *f, const char *dir)
{
	static unsigned	seq;

	snprintf(f->tmp, sizeof f->tmp, "%s/.goblin-view.%ld.%u.%u.part", dir,
	    (long)getpid(), w->id, ++seq);
	f->fd = open(f->tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (f->fd < 0) {
		f->tmp[0] = '\0';
		return (-1);
	}
	return (0);
}

/* Append received bytes (inflating when compressed); -1 on error. */
static int
stage_write(struct xf_file *f, const unsigned char *d, size_t n, int finish)
{
	if (f->zip) {
		unsigned char	out[16384];

		if (!f->zinit) {
			memset(&f->z, 0, sizeof f->z);
			if (inflateInit(&f->z) != Z_OK)
				return (-1);
			f->zinit = 1;
		}
		f->z.next_in = const_cast<unsigned char *>(d);
		f->z.avail_in = (uInt)n;
		do {
			int	rc;
			size_t	have;

			f->z.next_out = out;
			f->z.avail_out = sizeof out;
			rc = inflate(&f->z, finish ? Z_FINISH : Z_NO_FLUSH);
			if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR)
				return (-1);
			have = sizeof out - f->z.avail_out;
			if (have > 0 && write(f->fd, out, have) != (ssize_t)have)
				return (-1);
			f->got += (long long)have;
			if (rc == Z_STREAM_END)
				break;
		} while (f->z.avail_out == 0 || f->z.avail_in > 0);
		return (0);
	}
	while (n > 0) {
		ssize_t	r = write(f->fd, d, n);

		if (r <= 0)
			return (-1);
		d += r;
		n -= (size_t)r;
		f->got += r;
	}
	return (0);
}

/* Verify and publish; the final path goes in `final`. */
static int
stage_finish(struct xf_file *f, const char *dir, char *final, size_t cap,
    const char **err)
{
	if (f->sz >= 0 && f->got != f->sz) {
		*err = "EIO:size mismatch";
		file_close(f, 0);
		return (-1);
	}
	if (f->fd >= 0) {
		if (f->perm)
			fchmod(f->fd, (f->perm & 0666) | 0600);
		if (f->mtime_ns > 0) {
			struct timespec	ts[2];

			ts[0].tv_sec = ts[1].tv_sec = (time_t)(f->mtime_ns / 1000000000LL);
			ts[0].tv_nsec = ts[1].tv_nsec = (long)(f->mtime_ns % 1000000000LL);
			futimens(f->fd, ts);
		}
	}
	file_close(f, 1);
	if (publish(f->tmp, dir, f->name, final, cap) < 0) {
		*err = "EIO:cannot publish";
		unlink(f->tmp);
		f->tmp[0] = '\0';
		return (-1);
	}
	f->tmp[0] = '\0';
	f->done = 1;
	return (0);
}

/* ---- the prompt ---- */

static void
prompt_show(struct dv_session *s, struct dv_window *w, struct dv_xfer *x)
{
	int	i;

	for (i = 0; i < x->nlines; i++)
		dv_strlcpy(s->xfer_text[i], x->text[i], sizeof s->xfer_text[i]);
	s->xfer_nlines = x->nlines;
	s->xfer_prompt = 1;
	s->xfer_prompt_win = w->id;
	x->asked = 1;
	s->dirty = 1;
}

/* Show the next transfer that is waiting for an answer, if any. */
static void
prompt_next(struct dv_session *s)
{
	struct dv_window	*w;

	if (s->xfer_prompt)
		return;
	TAILQ_FOREACH(w, &s->desktop.windows, entry)
		if (w->xfer != nullptr && w->xfer->state == XS_ASK && !w->xfer->asked) {
			prompt_show(s, w, w->xfer);
			return;
		}
}

static void
ask(struct dv_window *w, struct dv_xfer *x)
{
	if (w->session == nullptr)
		return;
	x->asked = 0;
	prompt_next(w->session);
}

/* ---- kitty (OSC 5113) ---- */

static void
kstatus(struct dv_window *w, struct dv_xfer *x, const char *fid,
    const char *st, long long sz, const char *name)
{
	struct dv_buf	b;
	int		err = strchr(st, ':') != nullptr;

	if (x->quiet >= 2 || (x->quiet >= 1 && !err))
		return;
	memset(&b, 0, sizeof b);
	bfmt(&b, "\033]5113;ac=status;id=%s", x->id);
	if (fid != nullptr && fid[0] != '\0')
		bfmt(&b, ";fid=%s", fid);
	bput_b64(&b, "st", st);
	if (sz >= 0)
		bfmt(&b, ";sz=%lld", sz);
	if (name != nullptr)
		bput_b64(&b, "n", name);
	bput(&b, "\033\\");
	dv_window_pty_send(w, b.data, b.len);
	dv_buf_free(&b);
}

/* Announce one local path (and, for a directory, what is under it). */
static void
list_path(struct dv_window *w, struct dv_xfer *x, const char *qfid,
    const char *path, const char *pr, int depth)
{
	struct stat	 st;
	struct xf_file	*f;
	struct dv_buf	 b;
	const char	*ft;
	char		 own[64];

	if (lstat(path, &st) < 0) {
		char	msg[160];

		snprintf(msg, sizeof msg, "ENOENT:%s", strerror(errno));
		kstatus(w, x, qfid, msg, -1, nullptr);
		return;
	}
	snprintf(own, sizeof own, "%d", x->nfiles + 1);
	f = file_add(x, own);
	if (f == nullptr)
		return;
	dv_strlcpy(f->path, path, sizeof f->path);
	f->sz = (long long)st.st_size;
	f->kind = S_ISREG(st.st_mode) ? 0 : S_ISDIR(st.st_mode) ? 1 : 2;
	ft = f->kind == 0 ? "regular" : f->kind == 1 ? "directory" : "symlink";

	memset(&b, 0, sizeof b);
	bfmt(&b, "\033]5113;ac=file;id=%s;fid=%s", x->id, qfid);
	bput_b64(&b, "st", own);
	bfmt(&b, ";mod=%lld;prm=%u", (long long)st.st_mtime * 1000000000LL,
	    (unsigned)(st.st_mode & 0777));
	bput_b64(&b, "n", path);
	bfmt(&b, ";sz=%lld;ft=%s", f->sz, ft);
	if (pr != nullptr)
		bfmt(&b, ";pr=%s", pr);
	bput(&b, "\033\\");
	dv_window_pty_send(w, b.data, b.len);
	dv_buf_free(&b);

	if (f->kind == 1 && depth < 6) {
		DIR		*d = opendir(path);
		struct dirent	*e;

		if (d == nullptr)
			return;
		while ((e = readdir(d)) != nullptr) {
			char	child[DV_PATH_MAX];

			if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
				continue;
			if (x->nfiles >= XF_FILES)
				break;
			snprintf(child, sizeof child, "%s/%s", path, e->d_name);
			list_path(w, x, qfid, child, own, depth + 1);
		}
		closedir(d);
	}
}

/* Queue data escapes for the file the program asked for, while the
 * pty queue has room; called again as it drains. */
static void
kpump(struct dv_window *w, struct dv_xfer *x)
{
	while (w->pty_out.len < XF_LOWATER) {
		struct xf_file	*f;
		unsigned char	 raw[XF_CHUNK], comp[XF_CHUNK + 64];
		const unsigned char *payload;
		char		 enc[(XF_CHUNK + 64) / 3 * 4 + 8];
		ssize_t		 r;
		size_t		 pn;
		int		 last;
		struct dv_buf	 b;

		if (x->sending < 0) {
			int	i;

			for (i = 0; i < x->nfiles; i++)
				if (x->files[i].want && !x->files[i].done)
					break;
			if (i >= x->nfiles)
				return;
			f = &x->files[i];
			f->fd = open(f->path, O_RDONLY);
			if (f->fd < 0) {
				char	msg[160];

				snprintf(msg, sizeof msg, "EIO:%s", strerror(errno));
				kstatus(w, x, f->fid, msg, -1, nullptr);
				f->done = 1;
				continue;
			}
			f->got = 0;
			x->sending = i;
		}
		f = &x->files[x->sending];
		r = read(f->fd, raw, sizeof raw);
		if (r < 0) {
			kstatus(w, x, f->fid, "EIO:read failed", -1, nullptr);
			file_close(f, 1);
			f->done = 1;
			x->sending = -1;
			continue;
		}
		f->got += r;
		last = (r == 0 || f->got >= f->sz);
		payload = raw;
		pn = (size_t)r;
		if (f->zip) {
			if (!f->zinit) {
				memset(&f->z, 0, sizeof f->z);
				deflateInit(&f->z, Z_DEFAULT_COMPRESSION);
				f->zinit = 1;
			}
			f->z.next_in = raw;
			f->z.avail_in = (uInt)r;
			f->z.next_out = comp;
			f->z.avail_out = sizeof comp;
			deflate(&f->z, last ? Z_FINISH : Z_SYNC_FLUSH);
			payload = comp;
			pn = sizeof comp - f->z.avail_out;
		}
		if (pn == 0 && !last)
			continue;
		b64enc(payload, pn, enc, sizeof enc);
		memset(&b, 0, sizeof b);
		bfmt(&b, "\033]5113;ac=%s;id=%s;fid=%s;d=", last ? "end_data" : "data",
		    x->id, f->fid);
		bput(&b, enc);
		bput(&b, "\033\\");
		dv_window_pty_send(w, b.data, b.len);
		dv_buf_free(&b);
		if (last) {
			file_close(f, 1);
			f->done = 1;
			x->sending = -1;
		}
	}
}

static void
kitty_approved(struct dv_window *w, struct dv_xfer *x)
{
	const char	*home = getenv("HOME");
	int		 i, n = x->nfiles;

	x->state = XS_GO;
	kstatus(w, x, nullptr, "OK", -1, nullptr);
	if (x->dir == XD_UP) {
		/* The requests were parked in the table. Take them out, then
		 * answer each with the real entries, numbered from 1. */
		struct xf_file	*req = dv_allocn<xf_file>((size_t)n);

		memcpy(req, x->files, sizeof *req * (size_t)n);
		x->nfiles = 0;
		for (i = 0; i < n; i++)
			list_path(w, x, req[i].fid, req[i].path, nullptr, 0);
		free(req);
		kstatus(w, x, nullptr, "OK", -1, home ? home : "/");
	}
}

static void
kitty_refused(struct dv_window *w, struct dv_xfer *x)
{
	kstatus(w, x, nullptr, "EPERM:User refused the transfer", -1, nullptr);
	xfer_drop(w);
}

static void
kitty_osc(struct dv_window *w, const char *body, size_t n)
{
	struct kv	 kv[24];
	size_t		 nk = split_kv(body, n, ';', kv, 24);
	struct dv_xfer	*x = w->xfer;
	char		 id[64], fid[64];

	kstr(kv, nk, "id", id, sizeof id);
	kstr(kv, nk, "fid", fid, sizeof fid);

	if (kis(kv, nk, "ac", "send") || kis(kv, nk, "ac", "receive")) {
		int	up = kis(kv, nk, "ac", "receive");

		if (x != nullptr)
			xfer_drop(w);		/* one at a time per window */
		x = xfer_new(w, 5113, up ? XD_UP : XD_DOWN, id);
		x->quiet = (int)knum(kv, nk, "q", 0);
		if (up) {
			x->want = (int)knum(kv, nk, "sz", 0);
			if (x->want <= 0) {
				kstatus(w, x, nullptr, "EINVAL:no files requested", -1, nullptr);
				xfer_drop(w);
				return;
			}
			return;			/* prompt once the paths are in */
		}
		snprintf(x->text[0], sizeof x->text[0],
		    "Window %u (%s) wants to send you files.", w->id, w->cmd);
		{
			char	d[DV_PATH_MAX];

			pretty_path(x->dldir, d, sizeof d);
			snprintf(x->text[1], sizeof x->text[1],
			    "Save them to %s?   y / n", d);
		}
		x->nlines = 2;
		ask(w, x);
		return;
	}
	if (x == nullptr || x->proto != 5113 || strcmp(x->id, id) != 0) {
		struct dv_xfer	tmp;

		memset(&tmp, 0, sizeof tmp);
		dv_strlcpy(tmp.id, id, sizeof tmp.id);
		if (!kis(kv, nk, "ac", "finish") && !kis(kv, nk, "ac", "cancel"))
			kstatus(w, &tmp, fid, "ENOENT:no such transfer", -1, nullptr);
		return;
	}

	if (kis(kv, nk, "ac", "cancel")) {
		kstatus(w, x, nullptr, "CANCELED", -1, nullptr);
		xfer_drop(w);
		return;
	}
	if (kis(kv, nk, "ac", "finish")) {
		kstatus(w, x, nullptr, "OK", -1, nullptr);
		xfer_drop(w);
		return;
	}

	if (kis(kv, nk, "ac", "file")) {
		const struct kv	*nm = kget(kv, nk, "n");
		unsigned char	 raw[DV_PATH_MAX];
		long		 rn = nm ? dv_b64_decode(nm->v, nm->vl, raw, sizeof raw - 1) : 0;
		struct xf_file	*f;

		if (rn < 0)
			rn = 0;
		if (x->dir == XD_UP && x->state == XS_ASK) {
			/* A path the program wants to read: collect them all,
			 * then ask once. */
			f = file_add(x, fid);
			if (f == nullptr)
				return;
			expand_path((const char *)raw, (size_t)rn, f->path, sizeof f->path);
			if (x->nfiles >= x->want) {
				int	i, shown = 0;

				snprintf(x->text[0], sizeof x->text[0],
				    "Window %u (%s) wants to read files on this computer:",
				    w->id, w->cmd);
				x->nlines = 1;
				for (i = 0; i < x->nfiles && x->nlines < 3; i++) {
					pretty_path(x->files[i].path, x->text[x->nlines],
					    sizeof x->text[0]);
					x->nlines++;
					shown++;
				}
				if (x->nfiles > shown)
					snprintf(x->text[x->nlines - 1], sizeof x->text[0],
					    "... and %d more", x->nfiles - shown + 1);
				snprintf(x->text[x->nlines], sizeof x->text[0],
				    "Allow it to read them?   y / n");
				x->nlines++;
				ask(w, x);
			}
			return;
		}
		if (x->state != XS_GO) {
			kstatus(w, x, fid, "EPERM:transfer not approved", -1, nullptr);
			return;
		}
		if (x->dir == XD_UP) {
			/* The program is ready for a listed file's data. */
			f = file_find(x, fid);
			if (f == nullptr || f->kind != 0) {
				kstatus(w, x, fid, "ENOENT:not a listed file", -1, nullptr);
				return;
			}
			if (kis(kv, nk, "tt", "rsync")) {
				kstatus(w, x, fid, "EINVAL:rsync is not supported here", -1, nullptr);
				return;
			}
			f->zip = kis(kv, nk, "zip", "zlib");
			f->want = 1;
			kpump(w, x);
			return;
		}
		/* An incoming file's metadata. */
		if (kis(kv, nk, "ft", "directory")) {
			kstatus(w, x, fid, "OK", -1, nullptr);
			return;
		}
		if (!kis(kv, nk, "ft", "regular") && kget(kv, nk, "ft") != nullptr) {
			kstatus(w, x, fid, "OK", -1, nullptr);	/* links: nothing to save */
			return;
		}
		f = file_find(x, fid);
		if (f == nullptr)
			f = file_add(x, fid);
		if (f == nullptr) {
			kstatus(w, x, fid, "EIO:too many files", -1, nullptr);
			return;
		}
		safe_name((const char *)raw, (size_t)rn, f->name, sizeof f->name);
		f->sz = knum(kv, nk, "sz", -1);
		f->mtime_ns = knum(kv, nk, "mod", 0);
		f->perm = (unsigned)knum(kv, nk, "prm", 0);
		f->zip = kis(kv, nk, "zip", "zlib");
		if (stage_open(w, f, x->dldir) < 0) {
			kstatus(w, x, fid, "EIO:cannot create file", -1, nullptr);
			return;
		}
		kstatus(w, x, fid, "STARTED", -1, nullptr);
		return;
	}

	if (kis(kv, nk, "ac", "data") || kis(kv, nk, "ac", "end_data")) {
		const struct kv	*d = kget(kv, nk, "d");
		struct xf_file	*f = x->dir == XD_DOWN ? file_find(x, fid) : nullptr;
		unsigned char	*raw;
		long		 rn = 0;
		int		 end = kis(kv, nk, "ac", "end_data");

		if (f == nullptr || f->fd < 0) {
			kstatus(w, x, fid, "ENOENT:no such file in transfer", -1, nullptr);
			return;
		}
		raw = dv_allocn<unsigned char>(d ? d->vl + 3 : 3);
		if (d != nullptr)
			rn = dv_b64_decode(d->v, d->vl, raw, d->vl + 3);
		if (rn < 0 || stage_write(f, raw, (size_t)rn, end) < 0) {
			free(raw);
			kstatus(w, x, fid, "EIO:write failed", -1, nullptr);
			file_close(f, 0);
			return;
		}
		free(raw);
		if (!end) {
			kstatus(w, x, fid, "PROGRESS", f->got, nullptr);
			return;
		}
		{
			char		final[DV_PATH_MAX];
			const char	*err = "EIO:failed";

			if (stage_finish(f, x->dldir, final, sizeof final, &err) < 0)
				kstatus(w, x, fid, err, -1, nullptr);
			else
				kstatus(w, x, fid, "OK", f->got, nullptr);
		}
		return;
	}
	/* status from a program, or an unknown action: nothing to do */
}

/* ---- goblin-download (OSC 777) ---- */

static void
greply(struct dv_window *w, const char *id, const char *fmt, ...)
{
	struct dv_buf	b;
	char		tmp[1024];
	va_list		ap;
	int		n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	memset(&b, 0, sizeof b);
	bfmt(&b, "\033]777;goblin-download;v=1:op=status:id=%s:%s\033\\", id, tmp);
	dv_window_pty_send(w, b.data, b.len);
	dv_buf_free(&b);
}

static void
gerror(struct dv_window *w, struct dv_xfer *x, const char *msg)
{
	char	enc[512];

	if (x == nullptr || !x->reply)
		return;
	b64enc((const unsigned char *)msg, strlen(msg), enc, sizeof enc);
	greply(w, x->id, "status=error:message=%s", enc);
}

/* op=end has arrived and the user said yes: publish and report. */
static void
goblin_finish(struct dv_window *w, struct dv_xfer *x)
{
	struct xf_file	*f = &x->files[0];
	char		 final[DV_PATH_MAX], enc[1024];
	const char	*err = "EIO:failed", *base;

	if (x->reply)
		greply(w, x->id, "status=queued");
	if (stage_finish(f, x->dldir, final, sizeof final, &err) < 0) {
		gerror(w, x, strchr(err, ':') ? strchr(err, ':') + 1 : err);
		xfer_drop(w);
		return;
	}
	base = strrchr(final, '/');
	base = base ? base + 1 : final;
	b64enc((const unsigned char *)base, strlen(base), enc, sizeof enc);
	if (x->reply)
		greply(w, x->id, "status=saved:name=%s", enc);
	xfer_drop(w);
}

static void
goblin_osc(struct dv_window *w, const char *body, size_t n)
{
	const char	*meta, *payload = nullptr;
	size_t		 ml, pl = 0, i;
	struct kv	 kv[16];
	size_t		 nk;
	char		 id[64];
	struct dv_xfer	*x = w->xfer;

	/* "goblin-download;k=v:k=v[;payload]" */
	if (n < 16 || memcmp(body, "goblin-download;", 16) != 0)
		return;
	meta = body + 16;
	ml = n - 16;
	for (i = 0; i < ml; i++)
		if (meta[i] == ';') {
			payload = meta + i + 1;
			pl = ml - i - 1;
			ml = i;
			break;
		}
	nk = split_kv(meta, ml, ':', kv, 16);
	kstr(kv, nk, "id", id, sizeof id);

	if (kis(kv, nk, "op", "query")) {
		greply(w, id, "status=supported:max_chunk=4096:max_size=%lld:approval=1",
		    XF_MAX_GOBLIN);
		return;
	}
	if (kis(kv, nk, "op", "begin")) {
		const struct kv	*nm = kget(kv, nk, "name");
		unsigned char	 raw[512];
		long		 rn = nm ? dv_b64_decode(nm->v, nm->vl, raw, sizeof raw - 1) : 0;
		struct xf_file	*f;
		char		 sz[32], d[DV_PATH_MAX];

		if (x != nullptr)
			xfer_drop(w);
		x = xfer_new(w, 777, XD_DOWN, id);
		x->reply = knum(kv, nk, "reply", 0) != 0;
		x->confirm = knum(kv, nk, "confirm", 0) != 0;
		f = file_add(x, id);
		safe_name((const char *)raw, rn < 0 ? 0 : (size_t)rn, f->name, sizeof f->name);
		f->sz = knum(kv, nk, "size", -1);
		if (f->sz > XF_MAX_GOBLIN) {
			gerror(w, x, "file too large");
			xfer_drop(w);
			return;
		}
		if (stage_open(w, f, x->dldir) < 0) {
			gerror(w, x, "cannot create file");
			xfer_drop(w);
			return;
		}
		fmt_size(f->sz, sz, sizeof sz);
		pretty_path(x->dldir, d, sizeof d);
		snprintf(x->text[0], sizeof x->text[0],
		    "Window %u (%s) wants to send you a file:", w->id, w->cmd);
		snprintf(x->text[1], sizeof x->text[1], "%s  (%s)", f->name, sz);
		snprintf(x->text[2], sizeof x->text[2], "Save it to %s?   y / n", d);
		x->nlines = 3;
		ask(w, x);
		return;
	}
	if (x == nullptr || x->proto != 777 || strcmp(x->id, id) != 0)
		return;
	if (kis(kv, nk, "op", "data")) {
		unsigned char	*raw = dv_allocn<unsigned char>(pl + 3);
		long		 rn = payload ? dv_b64_decode(payload, pl, raw, pl + 3) : 0;
		struct xf_file	*f = &x->files[0];

		if (rn < 0 || (f->fd >= 0 && stage_write(f, raw, (size_t)rn, 0) < 0)) {
			free(raw);
			gerror(w, x, "write failed");
			xfer_drop(w);
			return;
		}
		free(raw);
		if (f->got > XF_MAX_GOBLIN) {
			gerror(w, x, "file too large");
			xfer_drop(w);
		}
		return;
	}
	if (kis(kv, nk, "op", "end")) {
		x->ended = 1;
		if (x->state == XS_GO)
			goblin_finish(w, x);
		/* still being asked: finish (or drop) with the answer */
		return;
	}
	if (kis(kv, nk, "op", "cancel")) {
		xfer_drop(w);
		return;
	}
}

/* ---- entry points ---- */

void
dv_xfer_osc(struct dv_window *w, unsigned osc, const char *body, size_t n)
{
	const char	*proto = osc == 5113 ? "kitty" : "goblin";
	char		 id[64];
	size_t		 idl;

	if (dv_xfer_forward_hook != nullptr &&
	    dv_xfer_forward_hook(w, proto, osc, body, n)) {
		/* The terminal speaks it: its answers come back here. */
		idl = dv_meta_val(body, n, "id", id, sizeof id);
		dv_route_expect(w, osc, id, idl);
		return;
	}
	if (osc == 5113)
		kitty_osc(w, body, n);
	else
		goblin_osc(w, body, n);
}

void
dv_xfer_answer(struct dv_session *s, int yes)
{
	struct dv_window	*w = nullptr, *it;
	struct dv_xfer		*x;

	if (!s->xfer_prompt)
		return;
	TAILQ_FOREACH(it, &s->desktop.windows, entry)
		if (it->id == s->xfer_prompt_win)
			w = it;
	s->xfer_prompt = 0;
	s->dirty = 1;
	if (w == nullptr || (x = w->xfer) == nullptr || x->state != XS_ASK) {
		prompt_next(s);
		return;
	}
	if (x->proto == 5113) {
		if (yes)
			kitty_approved(w, x);
		else
			kitty_refused(w, x);
	} else {
		if (!yes) {
			gerror(w, x, "User refused the transfer");
			xfer_drop(w);
		} else {
			x->state = XS_GO;
			if (x->reply && x->confirm)
				greply(w, x->id, "status=approved");
			if (x->ended)
				goblin_finish(w, x);
		}
	}
	prompt_next(s);
}

void
dv_xfer_pump(struct dv_window *w)
{
	struct dv_xfer	*x = w->xfer;

	if (x != nullptr && x->proto == 5113 && x->dir == XD_UP && x->state == XS_GO)
		kpump(w, x);
}

void
dv_xfer_free(struct dv_window *w)
{
	xfer_drop(w);
	if (w->session != nullptr)
		prompt_next(w->session);
}

int
dv_xfer_active(const struct dv_window *w)
{
	return (w->xfer != nullptr);
}
