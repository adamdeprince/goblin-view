#include "dv.hpp"
#include "pstatus.hpp"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int failed;

static void
fail(const char *n)
{
	fprintf(stderr, "FAIL %s\n", n);
	failed = 1;
}

static void
put(struct dv_window *w, const char *s)
{
	dv_vt_write(w, s, strlen(s));
}

static uint32_t
at(struct dv_window *w, unsigned x, unsigned y)
{
	struct dv_cell	*c = dv_screen_at(&w->screen, x, y);

	return (c == nullptr ? 0xffffffffu : c->ch);
}

/* Does the window's host-bound output contain this string? */
static int
host_has(struct dv_window *w, const char *str)
{
	return (w->host_out.len > 0 &&
	    memmem(w->host_out.data, w->host_out.len, str, strlen(str)) != NULL);
}

static void
fresh(struct dv_window *w, unsigned cols, unsigned rows)
{
	memset(w, 0, sizeof *w);
	w->pty_fd = -1;
	dv_screen_init(&w->screen, cols, rows);
}

/* Decode the base64 medium payload of a t=<m> transmit (the bytes after
 * the control block's ';', up to ST) into `out`. Returns the byte count,
 * 0 if there is no such transmit. */
static size_t
medium_name(const struct dv_buf *b, char m, char *out, size_t cap)
{
	char		key[6];
	unsigned char	*at, *semi, *end;
	size_t		i, o = 0, left;
	int		acc = 0, bits = 0;

	at = (unsigned char *)memmem(b->data, b->len, "\033_Ga=t", 6);
	if (at == NULL)
		return (0);
	left = b->len - (size_t)(at - (unsigned char *)b->data);
	semi = (unsigned char *)memchr(at, ';', left);
	if (semi == NULL)
		return (0);
	snprintf(key, sizeof key, "t=%c", m);		/* medium must be present */
	if (memmem(at, (size_t)(semi - at), key, 3) == NULL)
		return (0);
	end = semi + 1;
	while (end < (unsigned char *)b->data + b->len && *end != 0x1b)
		end++;
	for (i = 0; semi + 1 + i < end && o < cap; i++) {
		int	ch = semi[1 + i], v;

		if (ch >= 'A' && ch <= 'Z')
			v = ch - 'A';
		else if (ch >= 'a' && ch <= 'z')
			v = ch - 'a' + 26;
		else if (ch >= '0' && ch <= '9')
			v = ch - '0' + 52;
		else if (ch == '+')
			v = 62;
		else if (ch == '/')
			v = 63;
		else
			continue;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[o++] = (char)((acc >> bits) & 0xff);
		}
	}
	return (o);
}

static struct dv_kplace *
first_place(struct dv_window *w)
{
	return (w->screen.kpl == nullptr || w->screen.kpl->empty() ?
	    nullptr : &w->screen.kpl->begin()->second);
}

static int
count_places(struct dv_window *w)
{
	return (w->screen.kpl == nullptr ? 0 : (int)w->screen.kpl->size());
}


/* ---- file transfer: the endpoint of last resort ---- */

static void
tb64(const char *in, char *out)
{
	static const char A[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t	n = strlen(in), i, o = 0;

	for (i = 0; i < n; i += 3) {
		unsigned	v = (unsigned)(unsigned char)in[i] << 16;

		if (i + 1 < n)
			v |= (unsigned)(unsigned char)in[i + 1] << 8;
		if (i + 2 < n)
			v |= (unsigned char)in[i + 2];
		out[o++] = A[(v >> 18) & 63];
		out[o++] = A[(v >> 12) & 63];
		out[o++] = i + 1 < n ? A[(v >> 6) & 63] : '=';
		out[o++] = i + 2 < n ? A[v & 63] : '=';
	}
	out[o] = '\0';
}

/* Everything the program's pty has been sent since the last call. */
static char *
drain(int fd)
{
	static char	buf[65536];
	ssize_t		n, t = 0;

	for (;;) {
		n = read(fd, buf + t, sizeof buf - 1 - (size_t)t);
		if (n <= 0)
			break;
		t += n;
	}
	buf[t] = '\0';
	return (buf);
}

static int
file_is(const char *path, const char *want)
{
	char	got[4096];
	FILE	*f = fopen(path, "r");
	size_t	n;

	if (f == nullptr)
		return (0);
	n = fread(got, 1, sizeof got - 1, f);
	fclose(f);
	got[n] = '\0';
	return (strcmp(got, want) == 0);
}

static const char	*hook_proto;
static unsigned		 hook_osc;
static int
test_hook(struct dv_window *, const char *proto, unsigned osc, const char *, size_t)
{
	hook_proto = proto;
	hook_osc = osc;
	return (1);
}

static void
test_xfer(void)
{
	struct dv_session	 ses;
	struct dv_window	 w;
	char			 dir[] = "/tmp/gvxferXXXXXX", path[1200], enc[1600];
	char			*rep;
	int			 pfd[2];

	if (mkdtemp(dir) == nullptr) {
		fail("mkdtemp");
		return;
	}
	setenv("ODIN_SEAT_DOWNLOAD_DIR", dir, 1);
	memset(&ses, 0, sizeof ses);
	TAILQ_INIT(&ses.desktop.windows);
	fresh(&w, 40, 10);
	w.id = 3;
	dv_strlcpy(w.cmd, "bash", sizeof w.cmd);
	w.session = &ses;
	TAILQ_INSERT_TAIL(&ses.desktop.windows, &w, entry);
	ses.desktop.focused = &w;
	if (pipe(pfd) != 0)
		fail("pipe");
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	w.pty_fd = pfd[1];

	/* kitty send: ask first, then OK, STARTED, PROGRESS, OK, saved. */
	put(&w, "\033]5113;ac=send;id=t1\033\\");
	if (!ses.xfer_prompt || strstr(ses.xfer_text[0], "Window 3 (bash) wants to send you files") == NULL)
		fail("a kitty send asks the user first");
	if (strstr(drain(pfd[0]), "5113") != NULL)
		fail("nothing is answered before the user decides");
	dv_xfer_answer(&ses, 1);
	rep = drain(pfd[0]);
	if (strstr(rep, "\033]5113;ac=status;id=t1;st=T0s=\033\\") == NULL)
		fail("yes answers the send with st=OK");
	if (ses.xfer_prompt)
		fail("the box closes once answered");
	put(&w, "\033]5113;ac=file;id=t1;fid=f1;n=aGVsbG8udHh0;sz=5;ft=regular;prm=420\033\\");
	if (strstr(drain(pfd[0]), ";fid=f1;st=U1RBUlRFRA==\033\\") == NULL)
		fail("a file's metadata is answered with STARTED");
	put(&w, "\033]5113;ac=data;id=t1;fid=f1;d=aGVs\033\\");
	if (strstr(drain(pfd[0]), ";fid=f1;st=UFJPR1JFU1M=;sz=3\033\\") == NULL)
		fail("a data chunk is answered with PROGRESS and the bytes so far");
	put(&w, "\033]5113;ac=end_data;id=t1;fid=f1;d=bG8=\033\\");
	if (strstr(drain(pfd[0]), ";fid=f1;st=T0s=;sz=5\033\\") == NULL)
		fail("end_data is answered with OK and the size");
	snprintf(path, sizeof path, "%s/hello.txt", dir);
	if (!file_is(path, "hello"))
		fail("the file lands in the download directory with its content");
	put(&w, "\033]5113;ac=finish;id=t1\033\\");
	if (strstr(drain(pfd[0]), ";id=t1;st=T0s=") == NULL || dv_xfer_active(&w))
		fail("finish is acknowledged and ends the transfer");

	/* The same name again is published beside it, never over it. */
	put(&w, "\033]5113;ac=send;id=t2\033\\");
	dv_xfer_answer(&ses, 1);
	put(&w, "\033]5113;ac=file;id=t2;fid=f1;n=aGVsbG8udHh0;sz=2\033\\");
	put(&w, "\033]5113;ac=end_data;id=t2;fid=f1;d=aGk=\033\\");
	put(&w, "\033]5113;ac=finish;id=t2\033\\");
	drain(pfd[0]);
	snprintf(path, sizeof path, "%s/hello.txt.1", dir);
	if (!file_is(path, "hi"))
		fail("a name collision publishes as name.1");
	snprintf(path, sizeof path, "%s/hello.txt", dir);
	if (!file_is(path, "hello"))
		fail("the earlier file is untouched");

	/* No means EPERM, and nothing is kept. */
	put(&w, "\033]5113;ac=send;id=t3\033\\");
	dv_xfer_answer(&ses, 0);
	if (strstr(drain(pfd[0]), ";id=t3;st=RVBFUk06") == NULL || dv_xfer_active(&w))
		fail("no answers with EPERM and drops the transfer");

	/* A short file is not published. */
	put(&w, "\033]5113;ac=send;id=t4\033\\");
	dv_xfer_answer(&ses, 1);
	put(&w, "\033]5113;ac=file;id=t4;fid=f1;n=aGVsbG8udHh0;sz=9\033\\");
	put(&w, "\033]5113;ac=end_data;id=t4;fid=f1;d=aGVs\033\\");
	if (strstr(drain(pfd[0]), ";fid=f1;st=RUlPOnNpemUgbWlzbWF0Y2g=") == NULL)
		fail("a size mismatch is EIO");
	snprintf(path, sizeof path, "%s/hello.txt.2", dir);
	if (access(path, F_OK) == 0)
		fail("a mismatched file is not published");
	put(&w, "\033]5113;ac=cancel;id=t4\033\\");
	if (strstr(drain(pfd[0]), "st=Q0FOQ0VMRUQ=") == NULL || dv_xfer_active(&w))
		fail("cancel is acknowledged with CANCELED");

	/* zlib-compressed data is inflated; quiet=1 suppresses the good news. */
	put(&w, "\033]5113;ac=send;id=t5;q=1\033\\");
	dv_xfer_answer(&ses, 1);
	if (drain(pfd[0])[0] != '\0')
		fail("quiet=1: OK is not sent");
	put(&w, "\033]5113;ac=file;id=t5;fid=z;n=ei5iaW4=;sz=64;zip=zlib\033\\");
	put(&w, "\033]5113;ac=data;id=t5;fid=z;d=eJzLSM3JyVeoyslMUijPL8pJySCRDwAidRi1\033\\");
	put(&w, "\033]5113;ac=end_data;id=t5;fid=z\033\\");
	put(&w, "\033]5113;ac=finish;id=t5\033\\");
	if (drain(pfd[0])[0] != '\0')
		fail("quiet=1: progress and OK are not sent");
	snprintf(path, sizeof path, "%s/z.bin", dir);
	if (!file_is(path, "hello zlib worldhello zlib worldhello zlib worldhello zlib world"))
		fail("zlib data is inflated into the file");

	/* goblin-download: query, begin (confirm), approved, data, end. */
	put(&w, "\033]777;goblin-download;v=1:op=query:id=9\033\\");
	if (strcmp(drain(pfd[0]), "\033]777;goblin-download;v=1:op=status:id=9:status=supported:"
	    "max_chunk=4096:max_size=67108864:approval=1\033\\") != 0)
		fail("a goblin query is answered with our limits");
	put(&w, "\033]777;goblin-download;v=1:op=begin:id=9:name=ci5wZGY=:size=3:reply=1:confirm=1\033\\");
	if (!ses.xfer_prompt || strstr(ses.xfer_text[1], "r.pdf  (3 B)") == NULL)
		fail("a goblin begin asks, naming the file and its size");
	if (drain(pfd[0])[0] != '\0')
		fail("nothing is said until the user answers");
	dv_xfer_answer(&ses, 1);
	if (strcmp(drain(pfd[0]), "\033]777;goblin-download;v=1:op=status:id=9:status=approved\033\\") != 0)
		fail("yes sends approved when confirm=1");
	put(&w, "\033]777;goblin-download;v=1:op=data:id=9;YWJj\033\\");
	put(&w, "\033]777;goblin-download;v=1:op=end:id=9\033\\");
	rep = drain(pfd[0]);
	if (strstr(rep, ":id=9:status=queued\033\\") == NULL ||
	    strstr(rep, ":id=9:status=saved:name=ci5wZGY=\033\\") == NULL)
		fail("end reports queued, then saved with the final name");
	snprintf(path, sizeof path, "%s/r.pdf", dir);
	if (!file_is(path, "abc") || dv_xfer_active(&w))
		fail("the goblin file is saved");

	/* goblin refused: an error with a message; data before the answer waits. */
	put(&w, "\033]777;goblin-download;v=1:op=begin:id=10:name=eC50eHQ=:size=1:reply=1\033\\");
	dv_xfer_answer(&ses, 0);
	if (strstr(drain(pfd[0]), ":id=10:status=error:message=") == NULL || dv_xfer_active(&w))
		fail("no is an error status with a message");
	put(&w, "\033]777;goblin-download;v=1:op=begin:id=11:name=eS50eHQ=:size=3:reply=1\033\\");
	put(&w, "\033]777;goblin-download;v=1:op=data:id=11;YWJj\033\\");
	put(&w, "\033]777;goblin-download;v=1:op=end:id=11\033\\");
	if (drain(pfd[0])[0] != '\0')
		fail("a complete stream still waits for the answer");
	dv_xfer_answer(&ses, 1);
	rep = drain(pfd[0]);
	if (strstr(rep, "status=approved") != NULL)
		fail("approved is only sent when begin asked for confirm");
	if (strstr(rep, ":id=11:status=queued") == NULL || strstr(rep, ":id=11:status=saved:name=eS50eHQ=") == NULL)
		fail("the waiting stream is published once approved");
	snprintf(path, sizeof path, "%s/y.txt", dir);
	if (!file_is(path, "abc"))
		fail("the waiting file has its content");

	/* kitty receive: the program reads a local file, with permission. */
	snprintf(path, sizeof path, "%s/in.txt", dir);
	{
		FILE	*f = fopen(path, "w");

		if (f != nullptr) {
			fputs("data!", f);
			fclose(f);
		}
	}
	tb64(path, enc);
	put(&w, "\033]5113;ac=receive;id=r1;sz=1\033\\");
	if (ses.xfer_prompt)
		fail("a receive waits for the paths before asking");
	{
		char	req[2048];

		snprintf(req, sizeof req, "\033]5113;ac=file;id=r1;fid=q1;n=%s\033\\", enc);
		put(&w, req);
	}
	if (!ses.xfer_prompt || strstr(ses.xfer_text[0], "wants to read files") == NULL ||
	    strstr(ses.xfer_text[1], "in.txt") == NULL)
		fail("a receive asks, listing the paths");
	dv_xfer_answer(&ses, 1);
	rep = drain(pfd[0]);
	if (strstr(rep, "\033]5113;ac=status;id=r1;st=T0s=\033\\") == NULL)
		fail("yes answers the receive with OK");
	if (strstr(rep, "\033]5113;ac=file;id=r1;fid=q1;st=MQ==;mod=") == NULL ||
	    strstr(rep, ";sz=5;ft=regular\033\\") == NULL)
		fail("each path is listed with an id of ours, size and type");
	if (strstr(rep, ";st=T0s=;n=") == NULL)
		fail("the listing ends with OK and the home directory");
	put(&w, "\033]5113;ac=file;id=r1;fid=1;tt=rsync\033\\");
	if (strstr(drain(pfd[0]), ";fid=1;st=RUlOVkFMOnJzeW5jIGlzIG5vdCBzdXBwb3J0ZWQgaGVyZQ==") == NULL)
		fail("rsync deltas are declined with EINVAL");
	put(&w, "\033]5113;ac=file;id=r1;fid=1\033\\");
	rep = drain(pfd[0]);
	if (strstr(rep, "\033]5113;ac=end_data;id=r1;fid=1;d=ZGF0YSE=\033\\") == NULL)
		fail("the file's bytes stream to the program, ending with end_data");
	put(&w, "\033]5113;ac=finish;id=r1\033\\");
	drain(pfd[0]);
	if (dv_xfer_active(&w))
		fail("finish ends the receive");

	/* A terminal that speaks the protocol gets the request instead. */
	dv_xfer_forward_hook = test_hook;
	put(&w, "\033]5113;ac=send;id=fw1\033\\");
	if (hook_proto == NULL || strcmp(hook_proto, "kitty") != 0 || hook_osc != 5113)
		fail("OSC 5113 is offered to a kitty host");
	if (ses.xfer_prompt || dv_xfer_active(&w))
		fail("a forwarded transfer asks nothing here");
	{
		unsigned	k;
		int		routed = 0;

		for (k = 0; k < DV_OSC_ROUTES; k++)
			if (ses.routes[k].osc == 5113 && strcmp(ses.routes[k].id, "fw1") == 0 &&
			    ses.routes[k].win == 3)
				routed = 1;
		if (!routed)
			fail("the host's replies are routed back by id");
	}
	put(&w, "\033]777;goblin-download;v=1:op=query:id=5\033\\");
	if (hook_proto == NULL || strcmp(hook_proto, "goblin") != 0 || hook_osc != 777)
		fail("OSC 777 is offered to a goblin host");
	dv_xfer_forward_hook = nullptr;

	close(pfd[0]);
	close(pfd[1]);
	w.pty_fd = -1;
	dv_xfer_free(&w);
	dv_buf_free(&w.pty_out);
	dv_screen_free(&w.screen);
	dv_buf_free(&w.host_out);
	dv_buf_free(&w.cap);
	{
		char	cmd[1300];

		snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
		(void)!system(cmd);
	}
}


/* ---- the kitty keyboard protocol per window ---- */

static struct dv_key
key(unsigned code, int final, unsigned mods, int type, const char *legacy)
{
	struct dv_key	k;

	memset(&k, 0, sizeof k);
	k.code = code;
	k.final = final;
	k.mods = mods;
	k.type = type;
	k.nlegacy = strlen(legacy);
	memcpy(k.legacy, legacy, k.nlegacy);
	return (k);
}

static void
enc_is(const struct dv_key *k, int flags, const char *want, const char *name)
{
	unsigned char	out[160];
	size_t		n = dv_kbd_encode(k, flags, out, sizeof out);

	if (n != strlen(want) || memcmp(out, want, n) != 0) {
		fprintf(stderr, "FAIL %s: flags %d got %zu bytes:", name, flags, n);
		for (size_t i = 0; i < n; i++)
			fprintf(stderr, " %02x", out[i]);
		fprintf(stderr, "\n");
		failed = 1;
	}
}

static void
test_kbd(void)
{
	struct dv_key	k;

	/* No flags: the legacy bytes, whatever they are. */
	k = key('a', 'u', 0, 1, "a");
	enc_is(&k, 0, "a", "legacy text");
	k = key('c', 'u', 4, 1, "\003");
	enc_is(&k, 0, "\003", "legacy ctrl-c");

	/* Disambiguate (1): text stays text; modified keys and Esc are CSI u;
	 * Enter, Tab and Backspace stay legacy unless modified. */
	k = key('a', 'u', 0, 1, "a");
	enc_is(&k, 1, "a", "flag 1 plain text");
	k = key('a', 'u', 1, 1, "A");
	enc_is(&k, 1, "A", "flag 1 shifted text");
	k = key('a', 'u', 4, 1, "\001");
	enc_is(&k, 1, "\033[97;5u", "flag 1 ctrl-a");
	k = key('a', 'u', 2, 1, "\033a");
	enc_is(&k, 1, "\033[97;3u", "flag 1 alt-a");
	k = key(27, 'u', 0, 1, "\033");
	enc_is(&k, 1, "\033[27u", "flag 1 escape");
	k = key(13, 'u', 0, 1, "\r");
	enc_is(&k, 1, "\r", "flag 1 enter");
	k = key(13, 'u', 4, 1, "\r");
	enc_is(&k, 1, "\033[13;5u", "flag 1 ctrl-enter");
	k = key(9, 'u', 0, 1, "\t");
	enc_is(&k, 1, "\t", "flag 1 tab");
	k = key(1, 'A', 0, 1, "\033[A");
	enc_is(&k, 1, "\033[A", "flag 1 up");
	k = key(1, 'A', 1, 1, "\033[1;2A");
	enc_is(&k, 1, "\033[1;2A", "flag 1 shift-up");
	k = key(13, '~', 0, 1, "\033[13~");
	enc_is(&k, 1, "\033[13~", "flag 1 F3");
	k = key(3, '~', 4, 1, "\033[3;5~");
	enc_is(&k, 1, "\033[3;5~", "flag 1 ctrl-delete");
	k = key(57399, 'u', 0, 1, "0");
	enc_is(&k, 1, "\033[57399u", "flag 1 keypad 0 is its own key");
	k = key('a', 'u', 0, 3, "");
	enc_is(&k, 1, "", "flag 1 drops releases");
	k = key(57441, 'u', 1, 1, "");
	enc_is(&k, 1, "", "flag 1 drops modifier keys");
	k = key('a', 'u', 64, 1, "a");
	enc_is(&k, 1, "a", "lock modifiers are not reported for text");

	/* Report events (2): releases and repeats carry their type. */
	k = key('a', 'u', 0, 3, "");
	enc_is(&k, 3, "\033[97;1:3u", "flag 2 text release");
	k = key('a', 'u', 0, 2, "a");
	enc_is(&k, 3, "a", "flag 2 text repeat is text");
	k = key(1, 'A', 0, 3, "");
	enc_is(&k, 3, "\033[1;1:3A", "flag 2 up release");
	k = key(13, 'u', 0, 3, "");
	enc_is(&k, 3, "", "flag 2 enter has no release without all-escapes");
	k = key('a', 'u', 4, 2, "\001");
	enc_is(&k, 3, "\033[97;5:2u", "flag 2 ctrl-a repeat");

	/* Alternate keys (4). */
	k = key('a', 'u', 5, 1, "");
	k.shifted = 'A';
	enc_is(&k, 5, "\033[97:65;6u", "flag 4 shifted alternate");
	k.shifted = 0;
	k.base = 'q';
	enc_is(&k, 5, "\033[97::113;6u", "flag 4 base alternate alone");
	k.shifted = 'A';
	enc_is(&k, 5, "\033[97:65:113;6u", "flag 4 both alternates");
	enc_is(&k, 1, "\033[97;6u", "alternates only with flag 4");

	/* All keys as escapes (8): everything, modifiers included. */
	k = key('a', 'u', 0, 1, "a");
	enc_is(&k, 8, "\033[97u", "flag 8 plain a");
	k = key('a', 'u', 1, 1, "A");
	enc_is(&k, 8, "\033[97;2u", "flag 8 shift-a");
	k = key(13, 'u', 0, 1, "\r");
	enc_is(&k, 8, "\033[13u", "flag 8 enter");
	k = key(57441, 'u', 1, 1, "");
	enc_is(&k, 8, "\033[57441;2u", "flag 8 left shift press");
	k = key(57441, 'u', 0, 3, "");
	enc_is(&k, 10, "\033[57441;1:3u", "flags 8|2 left shift release");
	k = key('a', 'u', 64, 1, "a");
	enc_is(&k, 8, "\033[97;65u", "flag 8 reports caps lock");
	k = key(1, 'A', 0, 1, "\033[A");
	enc_is(&k, 8, "\033[A", "flag 8 keeps the arrow form");

	/* Associated text (16, with 8). */
	k = key('a', 'u', 0, 1, "a");
	k.text[0] = 'a';
	k.ntext = 1;
	enc_is(&k, 24, "\033[97;1;97u", "flag 16 text");
	k = key('a', 'u', 1, 1, "A");
	k.text[0] = 'A';
	k.ntext = 1;
	enc_is(&k, 24, "\033[97;2;65u", "flag 16 shifted text");
	k = key('a', 'u', 4, 1, "\001");
	k.text[0] = 'a';
	k.ntext = 1;
	enc_is(&k, 24, "\033[97;5u", "flag 16 no text with ctrl");

	/* Packing survives the wire. */
	{
		unsigned char	buf[200];
		struct dv_key	k2;
		size_t		n;

		k = key('a', 'u', 5, 2, "\001");
		k.shifted = 'A';
		k.base = 'q';
		k.text[0] = 'x';
		k.text[1] = 'y';
		k.ntext = 2;
		n = dv_key_pack(&k, buf, sizeof buf);
		if (n == 0 || !dv_key_unpack(buf, n, &k2) || memcmp(&k, &k2, sizeof k) != 0)
			fail("a key packs and unpacks unchanged");
	}
}

/* The flag stacks: push, pop, set, query, one per screen. */
static void
test_kbd_stack(void)
{
	struct dv_window	w;
	int			pfd[2];
	char			rb[64];
	ssize_t			rn;

	fresh(&w, 20, 4);
	if (pipe(pfd) != 0)
		fail("pipe");
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	w.pty_fd = pfd[1];
	if (dv_screen_kbd_flags(&w.screen) != 0)
		fail("no flags at first");
	put(&w, "\033[>1u");
	if (dv_screen_kbd_flags(&w.screen) != 1)
		fail("push sets the flags");
	put(&w, "\033[?u");
	rn = read(pfd[0], rb, sizeof rb - 1);
	rb[rn > 0 ? rn : 0] = '\0';
	if (strcmp(rb, "\033[?1u") != 0)
		fail("query answers the flags");
	put(&w, "\033[=8;2u");
	if (dv_screen_kbd_flags(&w.screen) != 9)
		fail("set mode 2 ors bits in");
	put(&w, "\033[=1;3u");
	if (dv_screen_kbd_flags(&w.screen) != 8)
		fail("set mode 3 clears bits");
	put(&w, "\033[=3u");
	if (dv_screen_kbd_flags(&w.screen) != 3)
		fail("set mode 1 replaces");
	put(&w, "\033[>16u");
	if (dv_screen_kbd_flags(&w.screen) != 16)
		fail("a second push");
	put(&w, "\033[<u");
	if (dv_screen_kbd_flags(&w.screen) != 3)
		fail("pop returns to the previous flags");
	put(&w, "\033[?1049h");
	if (dv_screen_kbd_flags(&w.screen) != 0)
		fail("the alternate screen has its own, empty stack");
	put(&w, "\033[>2u");
	put(&w, "\033[?1049l");
	if (dv_screen_kbd_flags(&w.screen) != 3)
		fail("back on the main screen its flags are intact");
	put(&w, "\033[?1049h");
	if (dv_screen_kbd_flags(&w.screen) != 2)
		fail("the alternate stack is kept too");
	put(&w, "\033[<5u\033[?1049l");
	put(&w, "\033[<9u");
	if (dv_screen_kbd_flags(&w.screen) != 0)
		fail("popping past the bottom resets everything");
	for (int i = 0; i < 10; i++)
		put(&w, "\033[>1u");
	put(&w, "\033[<7u");
	if (dv_screen_kbd_flags(&w.screen) != 1)
		fail("a full stack evicts its oldest entries");
	put(&w, "\033[<u");
	if (dv_screen_kbd_flags(&w.screen) != 0)
		fail("...and the evicted base is gone");
	close(pfd[0]);
	close(pfd[1]);
	w.pty_fd = -1;
	dv_screen_free(&w.screen);
	dv_buf_free(&w.pty_out);
	dv_buf_free(&w.cap);
}


/* ---- extra cursors (CSI > ... SP q) and drop acceptance (OSC 72) ---- */

static const char	*dnd_hook_body;
static void
dnd_test_hook(struct dv_window *, const char *body, size_t)
{
	dnd_hook_body = body;
}

static void
test_xcursors(void)
{
	struct dv_window	w;
	int			pfd[2];
	char			rb[512];
	ssize_t			rn;

	fresh(&w, 20, 8);
	if (pipe(pfd) != 0)
		fail("pipe");
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	w.pty_fd = pfd[1];
	put(&w, "\033[> q");
	rn = read(pfd[0], rb, sizeof rb - 1);
	rb[rn > 0 ? rn : 0] = '\0';
	if (strcmp(rb, "\033[>1;2;3;29;30;40;100;101 q") != 0)
		fail("the support query lists the shapes we take");
	put(&w, "\033[>2;2:4:5 q");
	if (dv_xcur_at(&w.screen, 4, 3) != 2 || w.screen.nxcur != 1)
		fail("a beam cursor at row 4, column 5");
	put(&w, "\033[>1;4:1:1:2:3 q");
	if (dv_xcur_at(&w.screen, 0, 0) != 1 || dv_xcur_at(&w.screen, 2, 1) != 1 ||
	    dv_xcur_at(&w.screen, 3, 1) != 0 || w.screen.nxcur != 7)
		fail("a rectangle of block cursors");
	put(&w, "\033[>100 q");
	rn = read(pfd[0], rb, sizeof rb - 1);
	rb[rn > 0 ? rn : 0] = '\0';
	if (strncmp(rb, "\033[>100;", 7) != 0 || strstr(rb, "2:2:4:5") == NULL ||
	    strstr(rb, "1:2:1:1") == NULL || rb[strlen(rb) - 2] != ' ' ||
	    rb[strlen(rb) - 1] != 'q')
		fail("the cursor query lists them as points");
	put(&w, "\033[>0;2:4:5 q");
	if (dv_xcur_at(&w.screen, 4, 3) != 0)
		fail("shape 0 clears a cell");
	put(&w, "\033[>40;2:255:0:0 q\033[>30;5:7 q\033[>101 q");
	rn = read(pfd[0], rb, sizeof rb - 1);
	rb[rn > 0 ? rn : 0] = '\0';
	if (strcmp(rb, "\033[>101;30:5:7;40:2:255:0:0 q") != 0)
		fail("the colour query answers both colours");
	put(&w, "\033[3;3H\033[>3 q");
	if (dv_xcur_at(&w.screen, 2, 2) != 3)
		fail("no coordinates: the main cursor's cell");
	put(&w, "\033[>3;0 q");
	if (dv_xcur_at(&w.screen, 2, 2) != 3)
		fail("type 0: the main cursor's cell");
	put(&w, "\033[2J");
	if (w.screen.nxcur != 0)
		fail("ED 2 clears the extra cursors");
	put(&w, "\033[>1;2:1:1 q\033[?1049h");
	if (w.screen.nxcur != 0)
		fail("switching to the alternate screen clears them");
	put(&w, "\033[>1;2:1:1:2:2 q\033[>0;4 q");
	if (w.screen.nxcur != 0)
		fail("shape 0 over the whole screen clears them all");
	put(&w, "\033[>1;2:1:1 q\033[?1049l");
	if (w.screen.nxcur != 0)
		fail("switching back clears them too");
	put(&w, "\033[>29;2:2:2 q\033[22J");
	if (w.screen.nxcur != 0)
		fail("ED 22 clears them");
	put(&w, "\033[2 q");
	if (w.screen.nxcur != 0)
		fail("DECSCUSR is not a cursor placement");

	/* OSC 72: a program that takes drops says so, and the server hears. */
	dv_dnd_app_hook = dnd_test_hook;
	put(&w, "\033]72;t=a;text/plain image/png\033\\");
	if (!w.dnd_accept || strcmp(w.dnd_types, "text/plain image/png") != 0)
		fail("OSC 72 t=a records the accepted types");
	if (dnd_hook_body == NULL || strncmp(dnd_hook_body, "t=a;", 4) != 0)
		fail("the server is told about acceptance");
	put(&w, "\033]72;t=A\033\\");
	if (w.dnd_accept || w.dnd_types[0] != '\0')
		fail("OSC 72 t=A withdraws it");
	dv_dnd_app_hook = nullptr;

	close(pfd[0]);
	close(pfd[1]);
	w.pty_fd = -1;
	dv_screen_free(&w.screen);
	dv_buf_free(&w.pty_out);
	dv_buf_free(&w.cap);
}


/* Cells as text for a handed-over state: a round trip, runs folded. */
static void
test_cells_codec(void)
{
	struct dv_cell	in[6], out[6];
	struct dv_buf	b;
	unsigned	n;

	memset(in, 0, sizeof in);
	for (n = 0; n < 6; n++) {
		in[n].ch = U' ';
		in[n].fg = in[n].bg = in[n].ucolor = DV_COLOR_DEFAULT;
	}
	in[0].ch = U'é'; in[0].fg = DV_COLOR_RGB | 0x123456; in[0].attr = DV_ATTR_BOLD;
	in[0].ustyle = 3; in[0].ucolor = 9;
	in[4].ch = DV_KITTY_PLACEHOLDER; in[4].fg = 0x10000001; in[4].irow = 3; in[4].icol = 7;
	memset(&b, 0, sizeof b);
	dv_cells_encode(&b, in, 6);
	dv_buf_append(&b, "", 1);
	if (strstr((char *)b.data, "*3") == NULL)
		fail("three equal blanks fold into one run");
	memset(out, 0xff, sizeof out);
	if (dv_cells_decode((char *)b.data, out, 6) != 6)
		fail("six cells decode");
	for (n = 0; n < 6; n++)
		if (out[n].ch != in[n].ch || out[n].fg != in[n].fg || out[n].bg != in[n].bg ||
		    out[n].attr != in[n].attr || out[n].ustyle != in[n].ustyle ||
		    out[n].ucolor != in[n].ucolor || out[n].irow != in[n].irow ||
		    out[n].icol != in[n].icol)
			fail("cells decode to exactly what was encoded");
	dv_buf_free(&b);
}

/* XTGETTCAP: the capabilities a program can count on, one reply each. */
static void
test_xtgettcap(void)
{
	struct dv_window	w;
	int			pfd[2];
	char			rb[2048];
	ssize_t			rn;

	fresh(&w, 20, 4);
	if (pipe(pfd) != 0)
		fail("pipe");
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	w.pty_fd = pfd[1];
	unsetenv("ODIN_SEAT_TERM");
	put(&w, "\033[>q");
	rn = read(pfd[0], rb, sizeof rb - 1);
	rb[rn > 0 ? rn : 0] = '\0';
	if (strcmp(rb, "\033P>|OdinSeat " DV_VERSION "\033\\") != 0)
		fail("XTVERSION reports the portable Odin Seat identity");
	put(&w, "\033P+q5463;524742;5375\033\\");		/* Tc RGB Su */
	rn = read(pfd[0], rb, sizeof rb - 1);
	rb[rn > 0 ? rn : 0] = '\0';
	if (strcmp(rb, "\033P1+r5463\033\\\033P1+r524742\033\\\033P1+r5375\033\\") != 0)
		fail("Tc, RGB and Su are true, answered by name alone");
	put(&w, "\033P+q544e\033\\");				/* TN */
	rn = read(pfd[0], rb, sizeof rb - 1);
	rb[rn > 0 ? rn : 0] = '\0';
	if (strcmp(rb, "\033P1+r544e=787465726d2d323536636f6c6f72\033\\") != 0)
		fail("TN is the window's TERM (xterm-256color)");
	put(&w, "\033P+q636f6c6f7273;7a7a\033\\");		/* colors, zz */
	rn = read(pfd[0], rb, sizeof rb - 1);
	rb[rn > 0 ? rn : 0] = '\0';
	if (strcmp(rb, "\033P1+r636f6c6f7273=323536\033\\\033P0+r7a7a\033\\") != 0)
		fail("colors is 256; an unknown name is unknown");
	put(&w, "\033P+q536d756c78\033\\");			/* Smulx */
	rn = read(pfd[0], rb, sizeof rb - 1);
	rb[rn > 0 ? rn : 0] = '\0';
	if (strncmp(rb, "\033P1+r536d756c78=1b5b343a",
	    strlen("\033P1+r536d756c78=1b5b343a")) != 0)
		fail("Smulx is the styled-underline sequence");
	close(pfd[0]);
	close(pfd[1]);
	w.pty_fd = -1;
	dv_screen_free(&w.screen);
	dv_buf_free(&w.pty_out);
	dv_buf_free(&w.cap);
}

/* The old 4096-entry image and placement tables evicted live images. */
static void
test_many_images(void)
{
	struct dv_window w;
	char cmd[192];
	constexpr unsigned count = 5000, first = 0x80000000u;

	fresh(&w, 20, 6);
	for (unsigned i = 0; i < count; i++) {
		snprintf(cmd, sizeof cmd,
		    "\033_Ga=T,f=32,s=1,v=1,i=%u,U=1,c=1,r=1,q=2;QUJDRA==\033\\",
		    first + i);
		put(&w, cmd);
	}
	if (w.kimg->size() != count || count_places(&w) != count)
		fail("more than 4096 named images and placements coexist");
	unsigned host_id = dv_kitty_virtual_id(&w, first);
	if (host_id == 0 || dv_kitty_virtual_id(&w, first + count - 1) == 0)
		fail("the first and last virtual images survive table growth");
	auto *im = &w.kimg->at(first);
	auto *placement = &w.screen.kpl->at(host_id);
	unsigned gen = placement->gen;

	/* Replacing a named image retains its placement, even when chunked. */
	put(&w, "\033_Ga=t,f=32,s=1,v=1,i=2147483648,m=1,q=2;RUZH\033\\");
	if (im->done || dv_kitty_virtual_id(&w, first) != 0)
		fail("an incomplete replacement is not displayed");
	put(&w, "\033_Gm=0,q=2;SA==\033\\");
	if (!im->done || im->data.len != 8 ||
	    memcmp(im->data.data, "RUZHSA==", 8) != 0 ||
	    placement->gen == gen || dv_kitty_virtual_id(&w, first) != host_id)
		fail("a chunked replacement refreshes the original placement");
	put(&w, "\033_Ga=p,U=1,i=2147483648,c=2,r=1,q=2\033\\");
	if (count_places(&w) != count || placement->cols != 2)
		fail("re-registering a virtual placement does not add an entry");
	/* Pixel buffers grow on demand and retain capacity for replacements. */
	put(&w, "\033_Ga=t,f=32,s=1,v=1,i=2147483648,m=1,q=2;QUJD\033\\");
	char chunk[608];
	memset(chunk, 'A', 600);
	chunk[600] = '\0';
	put(&w, "\033_Gm=0,q=2;");
	put(&w, chunk);
	put(&w, "\033\\");
	if (im->data.space != 1024)
		fail("a pixel buffer grows to accommodate additional chunks");
	put(&w, "\033_Ga=t,f=32,s=1,v=1,i=2147483648,q=2;QUJDRA==\033\\");
	if (im->data.space != 1024)
		fail("a smaller replacement reuses the existing pixel buffer");

	/* Anonymous images and the largest protocol ID must remain distinct. */
	put(&w, "\033_Ga=T,f=32,s=1,v=1,i=4294967295,C=1,q=2;QUJDRA==\033\\");
	for (unsigned i = 0; i < count; i++)
		put(&w, "\033_Ga=T,f=32,s=1,v=1,C=1,q=2;QUJDRA==\033\\");
	if (w.kimg->size() != 2 * count + 1 ||
	    count_places(&w) != 2 * count + 1 ||
	    !w.kimg->contains(UINT32_MAX) || im != &w.kimg->at(first) ||
	    placement != &w.screen.kpl->at(host_id))
		fail("anonymous images grow without colliding or invalidating references");
	put(&w, "\033_Ga=d,d=I,i=2147483648,q=2\033\\");
	if (w.kimg->contains(first) || w.screen.kpl->contains(host_id))
		fail("deleting an image removes its pixels and placements");
	put(&w, "\033_Ga=T,f=32,s=1,v=1,i=2147483648,U=1,c=1,r=1,q=2;QUJDRA==\033\\");
	if (dv_kitty_virtual_id(&w, first) == host_id)
		fail("recreating a deleted image does not reuse its stale host ID");

	/* A deleted partial upload must not leave a dangling reassembly key. */
	put(&w, "\033_Ga=t,f=32,s=1,v=1,i=2147483648,m=1,q=2;QUJD\033\\");
	put(&w, "\033_Ga=d,d=I,i=2147483648,q=2\033\\");
	if (w.kreassem != 0)
		fail("deleting a partial upload clears its reassembly key");
	put(&w, "\033_Gm=0,q=2;RA==\033\\");
	put(&w, "\033_Ga=d,d=A,q=2\033\\");
	if (w.kimg != nullptr || count_places(&w) != 0)
		fail("delete-all releases dynamic image storage and placements");

	/* A deferred wrap on a one-row screen can scroll away placements. */
	dv_screen_resize(&w.screen, 1, 1);
	put(&w, "X\033_Ga=T,f=32,s=1,v=1,q=2;QUJDRA==\033\\");
	if (count_places(&w) != 1)
		fail("a new image survives deferred wrapping on a one-row screen");
	dv_kitty_free(&w);
	dv_screen_free(&w.screen);
	dv_buf_free(&w.cap);
}

/* Image memory grows beyond the former aggregate and external-medium caps. */
static void
test_dynamic_image_memory(void)
{
	struct dv_window w;
	constexpr size_t mib = 1024 * 1024, payload = 4096;
	char chunk[sizeof "Gm=1,q=2;" - 1 + payload];
	constexpr size_t header = sizeof "Gm=1,q=2;" - 1;
	memcpy(chunk, "Gm=1,q=2;", header);
	memset(chunk + header, 'A', payload);

	fresh(&w, 20, 6);
	auto apc = [&w](const char *control) {
		dv_kitty_apc(&w, (const unsigned char *)control, strlen(control));
	};
	auto append_chunks = [&w, &chunk](size_t bytes) {
		for (size_t off = 0; off < bytes; off += payload)
			dv_kitty_apc(&w, (const unsigned char *)chunk, sizeof chunk);
	};

	/* Together, the two images exceed the old 64 MiB window budget. */
	for (unsigned id = 1; id <= 2; id++) {
		char control[96];
		snprintf(control, sizeof control,
		    "Ga=t,f=32,s=1,v=1,i=%u,m=1,q=2;", id);
		apc(control);
		append_chunks(32 * mib + payload);
		apc("Gm=0,q=2;");
	}
	if (w.kimg->size() != 2 || !w.kimg->at(1).done || !w.kimg->at(2).done ||
	    w.kimg->at(1).data.len != 32 * mib + payload ||
	    w.kimg->at(2).data.len != 32 * mib + payload)
		fail("stored images can exceed 64 MiB in aggregate without eviction");

	/* A single chunked image can also exceed the old window budget. */
	apc("Ga=T,f=32,s=1,v=1,i=1,U=1,c=1,r=1,m=1,q=2;");
	append_chunks(64 * mib + payload);
	apc("Gm=0,q=2;");
	if (!w.kimg->at(1).done || w.kimg->at(1).data.len != 64 * mib + payload ||
	    dv_kitty_virtual_id(&w, 1) == 0 || !w.kimg->at(2).done)
		fail("a chunked image grows beyond 64 MiB and remains displayable");
	apc("Ga=d,d=I,i=1,q=2;");
	if (w.kimg->contains(1) || count_places(&w) != 0 || !w.kimg->at(2).done)
		fail("deleting a large image preserves other stored images");

	/* Sparse file pixels exceed the old 32 MiB external-medium limit. */
	char path[] = "/tmp/desqview-kitty-memory-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0)
		fail("create the large image medium");
	else {
		off_t size = 32 * mib + 1;
		if (ftruncate(fd, size) != 0 || pwrite(fd, "XYZ", 3, size - 3) != 3)
			fail("write the large image medium");
		else {
			char encoded[256], control[384];
			tb64(path, encoded);
			snprintf(control, sizeof control,
			    "Ga=t,t=f,f=32,s=1,v=1,i=3,q=2;%s", encoded);
			apc(control);
			auto it = w.kimg->find(3);
			size_t expected = ((size_t)size + 2) / 3 * 4;
			if (it == w.kimg->end() || !it->second.done ||
			    it->second.data.len != expected ||
			    memcmp(it->second.data.data + expected - 4, "WFla", 4) != 0)
				fail("external images larger than 32 MiB are read in full");
		}
		close(fd);
		unlink(path);
	}

	apc("Ga=d,d=A,q=2;");
	if (w.kimg != nullptr || count_places(&w) != 0 || w.kreassem != 0)
		fail("delete-all releases large dynamically allocated images");
	dv_kitty_free(&w);
	dv_screen_free(&w.screen);
}

static void
test_tiny_screen_resize(void)
{
	struct dv_window w;
	fresh(&w, 80, 24);
	for (unsigned cols : {1u, 2u, 1u, 80u}) {
		for (unsigned rows : {1u, 2u, 1u, 24u}) {
			/* Resize both screens with saved cursors beyond the new edge. */
			put(&w, "\033[999;999H\0337\033[?1049h\033[999;999H\0337");
			dv_screen_resize(&w.screen, cols, rows);
			put(&w, "\0338");
			if (w.screen.cc >= cols || w.screen.cr >= rows ||
			    w.screen.mt != 0 || w.screen.mb != rows - 1)
				fail("tiny alternate screen clamps cursor and scroll region");
			put(&w, "\033[?1049l\0338");
			if (w.screen.cc >= cols || w.screen.cr >= rows)
				fail("tiny primary screen clamps the saved cursor");
			/* Wrap, scroll both ways, edit lines/cells, and print wide text. */
			put(&w, "ab\r\ncd\033[S\033[T\033[L\033[M\033[@\033[P"
			    "\xe4\xb8\xad\033[2J\033[HX\033[H");
			if (at(&w, 0, 0) != 'X' || w.screen.cc != 0 || w.screen.cr != 0)
				fail("tiny screen stays writable after scrolling and editing");
		}
	}
	dv_screen_resize(&w.screen, 0, 0);
	if (w.screen.cols != 1 || w.screen.rows != 1)
		fail("zero screen dimensions clamp to 1x1");
	dv_screen_free(&w.screen);
}


/* ---- program status (OSC 7501) and ConEmu's OSC 9 ---- */

static void
test_pstatus(void)
{
	struct dv_session		 ses;
	struct dv_window		 w;
	const struct dv_pstatus_rec	*r;
	struct dv_pstatus_view		 v;
	int				 pfd[2];
	char				 big[9000];
	std::string			 longid, report;

	memset(&ses, 0, sizeof ses);
	TAILQ_INIT(&ses.desktop.windows);
	fresh(&w, 40, 10);
	w.id = 7;
	dv_strlcpy(w.cmd, "bash", sizeof w.cmd);
	dv_strlcpy(w.title, "bash", sizeof w.title);
	w.session = &ses;
	TAILQ_INSERT_TAIL(&ses.desktop.windows, &w, entry);
	if (pipe(pfd) != 0)
		fail("pipe");
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	w.pty_fd = pfd[1];

	/* The feature query is answered with the states and kinds. */
	put(&w, "\033]7501;?\033\\");
	if (strstr(drain(pfd[0]), "\033]7501;?:states=idle,working,done,"
	    "blocked,error:kinds=permission,question,auth\033\\") == nullptr)
		fail("OSC 7501 ? is answered");

	/* A report makes the root record; base64 text is decoded. */
	put(&w, "\033]7501;state=working:app=rsync:progress=40:msg=U3luY2luZw\033\\");
	r = dv_pstatus_find(&w, "");
	if (r == nullptr || r->state != DV_PS_WORKING || r->progress != 40 ||
	    r->app != "rsync" || r->msg != "Syncing")
		fail("a working report is kept, unpadded base64 decoded");
	if (!dv_pstatus_view_of(&w, &v) || v.state != DV_PS_WORKING ||
	    v.progress != 40 || v.text != "rsync: Syncing")
		fail("the frame shows the working record");

	/* A report replaces its record whole: no carried-over keys. */
	put(&w, "\033]7501;state=working\007");
	r = dv_pstatus_find(&w, "");
	if (r == nullptr || !r->app.empty() || r->progress != -1 || !r->msg.empty())
		fail("a report replaces its record completely");

	/* Malformed pairs are skipped, the rest applies; whitespace trimmed. */
	put(&w, "\033]7501; state = blocked :kind=question:bogus:K=x:app=a b:progress=101\033\\");
	r = dv_pstatus_find(&w, "");
	if (r == nullptr || r->state != DV_PS_BLOCKED ||
	    r->kind != DV_PS_KIND_QUESTION || !r->app.empty() || r->progress != -1)
		fail("malformed pairs skipped, out of range progress absent");

	/* Unknown state, a bad id, bad base64, a control: all ignored whole. */
	put(&w, "\033]7501;state=sleeping:id=x\033\\");
	put(&w, "\033]7501;state=done:id=a//b\033\\");
	put(&w, "\033]7501;state=done:id=c:msg=QUJ=D\033\\");	/* not base64 */
	put(&w, "\033]7501;state=done:id=d:msg=YQpi\033\\");	/* "a\nb" */
	put(&w, "\033]7501;state=done:id=e:msg=wIA=\033\\");	/* not UTF-8 */
	put(&w, "\033]7501;app=x\033\\");			/* no state */
	if (dv_pstatus_count(&w) != 1)
		fail("refused reports leave nothing behind");

	/* Children; app is inherited for display; the hierarchy clears. */
	put(&w, "\033]7501;state=working:app=deploy\033\\");
	put(&w, "\033]7501;state=working:id=us-east:progress=10\033\\");
	put(&w, "\033]7501;state=blocked:id=us-east/db:kind=permission\033\\");
	put(&w, "\033]7501;state=done:id=eu-west\033\\");
	if (dv_pstatus_count(&w) != 4)
		fail("records per id");
	if (!dv_pstatus_view_of(&w, &v) || v.state != DV_PS_BLOCKED ||
	    v.kind != DV_PS_KIND_PERMISSION || v.text != "deploy" || v.count != 1)
		fail("blocked is the most urgent, its app inherited");
	put(&w, "\033]7501;state=clear:id=us-east\033\\");
	if (dv_pstatus_count(&w) != 2 || dv_pstatus_find(&w, "us-east/db") != nullptr)
		fail("clear removes the record and those beneath it");

	/* A prompt ends working, blocked and idle; done stays. */
	put(&w, "\033]7501;state=idle:id=i\033\\");
	put(&w, "\033]133;A\033\\");
	if (dv_pstatus_count(&w) != 1 || dv_pstatus_find(&w, "eu-west") == nullptr)
		fail("a new prompt drops working, blocked and idle");
	/* Done stays until the user comes back to the window. */
	dv_pstatus_focus(&ses);
	if (dv_pstatus_count(&w) != 1)
		fail("an unfocused window keeps its results");
	ses.desktop.focused = &w;
	dv_pstatus_focus(&ses);
	if (dv_pstatus_count(&w) != 0)
		fail("focusing the window lets its results go");
	put(&w, "\033]7501;state=error:msg=ZmFpbGVk\033\\");
	dv_pstatus_focus(&ses);
	if (dv_pstatus_count(&w) != 1)
		fail("a result arriving in the focused window stays");
	dv_pstatus_key(&w);
	if (dv_pstatus_count(&w) != 0)
		fail("a key in it lets it go");

	/* None of the spec's size limits: long ids, many records, long msg. */
	for (int i = 0; i < 12; i++)
		longid += (i ? "/" : "") + std::string(40, 'a' + i);
	put(&w, ("\033]7501;state=working:id=" + longid + "\033\\").c_str());
	for (int i = 0; i < 300; i++)
		put(&w, ("\033]7501;state=working:id=r" + std::to_string(i) +
		    "\033\\").c_str());
	if (dv_pstatus_find(&w, longid.c_str()) == nullptr ||
	    dv_pstatus_count(&w) != 301)
		fail("no cap on id length, depth or record count");
	for (size_t i = 0; i + 4 < sizeof big; i += 4)	/* "abc" again and again */
		memcpy(big + i, "YWJj", 4);
	big[8996] = '\0';
	report = std::string("\033]7501;state=done:id=big:msg=") + big + "\033\\";
	put(&w, report.c_str());
	r = dv_pstatus_find(&w, "big");
	if (r == nullptr || r->msg.size() != 8996 / 4 * 3)
		fail("a msg past 2048 bytes is kept");
	/* RIS removes every record. */
	put(&w, "\033c");
	if (dv_pstatus_count(&w) != 0)
		fail("RIS removes every record");

	/* OSC 9;4 is progress, mapped to the root until a real report. */
	w.host_out.len = 0;
	put(&w, "\033]9;4;1;55\033\\");
	r = dv_pstatus_find(&w, "");
	if (r == nullptr || r->state != DV_PS_WORKING || r->progress != 55 || !r->osc9)
		fail("OSC 9;4;1 maps to a working root record");
	if (host_has(&w, "99;"))
		fail("OSC 9;4 is not a notification");
	put(&w, "\033]9;4;2\033\\");
	r = dv_pstatus_find(&w, "");
	if (r == nullptr || r->state != DV_PS_ERROR)
		fail("OSC 9;4;2 is an error");
	put(&w, "\033]9;4\033\\");
	if (dv_pstatus_count(&w) != 0)
		fail("the bare OSC 9;4 clears progress");
	put(&w, "\033]7501;state=idle:id=x\033\\");
	put(&w, "\033]9;4;3\033\\");
	if (dv_pstatus_find(&w, "") != nullptr)
		fail("after a real report OSC 9;4 is not mapped");
	put(&w, "\033c");

	/* ConEmu's other sub-commands. */
	put(&w, "\033]9;3;\"Build log\"\033\\");
	if (strcmp(w.title, "Build log") != 0)
		fail("OSC 9;3 sets the title, quotes removed");
	put(&w, "\033]9;3;\033\\");
	if (strcmp(w.title, "bash") != 0)
		fail("an empty OSC 9;3 restores the default title");
	put(&w, "\033]9;9;\"/tmp\"\033\\");
	if (strcmp(w.cwd, "/tmp") != 0)
		fail("OSC 9;9 records the working directory");
	put(&w, "\033]7;file://host/usr/local%20dir\033\\");
	if (strcmp(w.cwd, "/usr/local dir") != 0)
		fail("OSC 7 records the working directory, percent-decoded");
	put(&w, "\033]7501;state=working\033\\");
	put(&w, "\033]9;12\033\\");
	if (dv_pstatus_count(&w) != 0)
		fail("OSC 9;12 marks a prompt like OSC 133;A");
	put(&w, "\033]9;1;500\033\\\033]9;11;hello\033\\\033]9;6;x\033\\");
	if (strcmp(w.title, "bash") != 0 || dv_pstatus_count(&w) != 0)
		fail("unimplemented sub-commands and comments do nothing");
	put(&w, "\033]9;3;Title that is longer than the frame could ever hold "
	    "and then some \xe4\xb8\xad\xe4\xb8\xad\033\\");
	if (strlen(w.title) >= sizeof w.title ||
	    ((unsigned char)w.title[strlen(w.title) - 1] & 0xc0) == 0xc0)
		fail("a long title is cut on a character boundary");

	close(pfd[0]);
	close(pfd[1]);
	w.pty_fd = -1;
	dv_pstatus_close(&w);
	dv_screen_free(&w.screen);
	dv_buf_free(&w.host_out);
	dv_buf_free(&w.cap);
}

int
main(void)
{
	test_pstatus();
	test_tiny_screen_resize();
	test_many_images();
	test_dynamic_image_memory();
	struct dv_window	w;
	struct dv_cell		*c;
	struct dv_kplace	*p;

	fresh(&w, 10, 4);
	put(&w, "hi\r\n");
	if (at(&w, 0, 0) != 'h' || at(&w, 1, 0) != 'i')
		fail("hi");
	if (w.screen.cr != 1 || w.screen.cc != 0)
		fail("cursor after newline");

	put(&w, "\033[2J\033[H");
	if (at(&w, 0, 0) != ' ')
		fail("ED");
	if (w.screen.cr != 0 || w.screen.cc != 0)
		fail("CUP home");

	put(&w, "\033[1;31mX\033[0m");
	c = dv_screen_at(&w.screen, 0, 0);
	if (c == nullptr || c->ch != 'X' || c->fg != 1 ||
	    !(c->attr & DV_ATTR_BOLD))
		fail("SGR red bold");
	if (w.screen.fg != DV_COLOR_DEFAULT || w.screen.attr != 0)
		fail("SGR 0 is the default colour, not white on black");

	put(&w, "\033]0;title\007");
	if (strcmp(w.title, "title") != 0)
		fail("OSC title");

	/* A sequence split across two pty reads is still one sequence. */
	put(&w, "\033[H\033[2J\033[1;");
	put(&w, "3");
	put(&w, "4mZ");
	c = dv_screen_at(&w.screen, 0, 0);
	if (c == nullptr || c->ch != 'Z' || c->fg != 4)
		fail("CSI split across writes");
	if (at(&w, 1, 0) == '3' || at(&w, 1, 0) == '4')
		fail("split CSI must not print its digits");

	/* Truecolor is kept, colon subparameters included. */
	put(&w, "\033[H\033[38;2;10;20;30mA\033[48:2::1:2:3mB\033[0m");
	c = dv_screen_at(&w.screen, 0, 0);
	if (c == nullptr || c->fg != DV_RGB(10, 20, 30))
		fail("SGR 38;2;r;g;b");
	c = dv_screen_at(&w.screen, 1, 0);
	if (c == nullptr || c->bg != DV_RGB(1, 2, 3))
		fail("SGR 48:2::r:g:b");

	/* Silkie colour: a colon gamut id, brightness in steps of 25, even alpha. */
	put(&w, "\033[38:2:1:9:8:7m");
	if (w.screen.fg != DV_RGB(9, 8, 7) || w.screen.fg_gamut != 1)
		fail("SGR 38:2:1 is Display P3");
	put(&w, "\033[56:250;57:150;70:31m");
	if (w.screen.fg_br != 250 || w.screen.bg_br != 150 || w.screen.alpha != 30 ||
	    w.screen.fg_gamut != 1)
		fail("SGR brightness and alpha keep the gamut");
	put(&w, "\0337\033[56:200;70:0m\0338");
	if (w.screen.fg_br != 250 || w.screen.alpha != 30)
		fail("restoring a cursor brings back the pen that was saved");
	put(&w, "\033[0m");
	if (w.screen.alpha != 100 || w.screen.fg_br != 100 || w.screen.bg_br != 100 ||
	    w.screen.fg_gamut != 0 || w.screen.bg_gamut != 0)
		fail("SGR 0 resets alpha, brightness and gamut");
	/* Deep colours: 38:0:id:r:g:b, sixteen bits a channel, kept exactly. */
	put(&w, "\033[0m\033[38:0:1:65535:32768:1;48:0:0:257:0:65535;4;58:0:2:128:129:40000mD");
	c = dv_screen_at(&w.screen, w.screen.cc - 1, w.screen.cr);
	if (c == nullptr || c->fg != DV_RGB(255, 128, 0) || c->fg_gamut != 1 ||
	    dv_deep_chan(c->fg, c->fg_x, 0) != 65535 ||
	    dv_deep_chan(c->fg, c->fg_x, 1) != 32768 ||
	    dv_deep_chan(c->fg, c->fg_x, 2) != 1)
		fail("SGR 38:0 keeps all sixteen bits and the gamut");
	if (c == nullptr || c->bg != DV_RGB(1, 0, 255) || c->bg_gamut != 0 ||
	    dv_deep_chan(c->bg, c->bg_x, 0) != 257 ||
	    dv_deep_chan(c->bg, c->bg_x, 2) != 65535)
		fail("SGR 48:0: 257 is an eight-bit 1 exactly");
	if (c == nullptr || dv_deep_gamut(c->u_x) != 2 ||
	    dv_deep_chan(c->ucolor, c->u_x, 0) != 128 ||
	    dv_deep_chan(c->ucolor, c->u_x, 1) != 129 ||
	    dv_deep_chan(c->ucolor, c->u_x, 2) != 40000)
		fail("SGR 58:0 keeps its gamut, and values either side of a half step");
	{
		/* Every sixteen-bit value comes back from its cell. */
		int	bad = 0;

		for (unsigned v = 0; v <= 65535; v++) {
			uint32_t	rgb, x = dv_deep_make(v, 65535 - v, v / 2, 0, &rgb);

			if (dv_deep_chan(rgb, x, 0) != v ||
			    dv_deep_chan(rgb, x, 1) != 65535 - v ||
			    dv_deep_chan(rgb, x, 2) != v / 2)
				bad = 1;
		}
		if (bad)
			fail("dv_deep_make round-trips every value");
	}
	{
		/* The cell codec (the state handover) carries the deep words. */
		struct dv_buf	enc;
		struct dv_cell	back;

		memset(&enc, 0, sizeof enc);
		memset(&back, 0, sizeof back);
		dv_cells_encode(&enc, c, 1);
		dv_buf_append(&enc, "", 1);
		if (dv_cells_decode((const char *)enc.data, &back, 1) != 1 ||
		    back.fg_x != c->fg_x || back.bg_x != c->bg_x || back.u_x != c->u_x)
			fail("deep colours survive the cell codec");
		dv_buf_free(&enc);
	}
	/* Malformed groups change nothing; what follows them still applies. */
	put(&w, "\033[38:0:0:1:2m");
	put(&w, "\033[38:0:0:70000:0:0m");
	put(&w, "\033[38:0::65535:0:0;1m");
	if (w.screen.fg != DV_RGB(255, 128, 0) || w.screen.fg_x == 0 ||
	    !(w.screen.attr & DV_ATTR_BOLD))
		fail("a malformed 38:0 group is ignored, the rest applies");
	/* Any ordinary colour of the same kind replaces a deep one. */
	put(&w, "\033[38;2;1;2;3;49;59m");
	if (w.screen.fg_x != 0 || w.screen.bg_x != 0 || w.screen.u_x != 0 ||
	    w.screen.fg_gamut != 0)
		fail("38;2, 49 and 59 replace deep colours");
	put(&w, "\033[38:0:1:1:2:3m\0337\033[0m\0338");
	if (w.screen.fg_x == 0 || w.screen.fg_gamut != 1)
		fail("DECSC/DECRC keep the deep pen");
	put(&w, "\033[0m");
	if (w.screen.fg_x != 0)
		fail("SGR 0 clears a deep colour");
	put(&w, "\033[2J");

	put(&w, "\033[56:250m\033[56;250m");
	if (w.screen.fg_br != 100)
		fail("SGR 56 without a colon subparameter resets brightness");
	put(&w, "\033[70:101m");
	if (w.screen.alpha != 100)
		fail("SGR 70 at or above 100 is opaque");
	put(&w, "\033[56:110m");
	if (w.screen.fg_br != 100)
		fail("SGR 56:110 rounds down to SDR white");
	put(&w, "\033[56:113m");
	if (w.screen.fg_br != 125)
		fail("SGR 56:113 rounds to 125");
	put(&w, "\033[2J");
	c = dv_screen_at(&w.screen, 0, 0);
	if (c == nullptr || c->alpha != 100 || c->fg_br != 100 || c->bg_br != 100)
		fail("an erased cell is opaque SDR white");

	/* A wide glyph takes two cells; the second is a continuation. */
	put(&w, "\033[H\033[2J\xe4\xb8\xad" "x");
	c = dv_screen_at(&w.screen, 0, 0);
	if (c == nullptr || c->ch != 0x4e2d || !(c->attr & DV_ATTR_WIDE))
		fail("wide glyph");
	if (at(&w, 1, 0) != 0 || at(&w, 2, 0) != 'x')
		fail("wide continuation then x");
	if (dv_wcwidth(0x1F600) != 2 || dv_wcwidth('a') != 1 ||
	    dv_wcwidth(0x0301) != 0)
		fail("wcwidth");

	/* A UTF-8 character split across writes. */
	put(&w, "\033[H\033[2J\xc3");
	put(&w, "\xa9");
	if (at(&w, 0, 0) != 0xe9)
		fail("UTF-8 split across writes");

	/* IL inserts at the cursor, not at the top of the region. */
	put(&w, "\033[H\033[2Ja\r\nb\r\nc\r\nd\033[2;1H\033[L");
	if (at(&w, 0, 0) != 'a' || at(&w, 0, 1) != ' ' || at(&w, 0, 2) != 'b')
		fail("IL at cursor");
	put(&w, "\033[2;1H\033[M");
	if (at(&w, 0, 1) != 'b' || at(&w, 0, 2) != 'c')
		fail("DL at cursor");

	/* Save/restore and the alternate screen. */
	put(&w, "\033[H\033[2Jshell\033[2;3H\0337\033[?1049hvim\033[?1049l");
	if (at(&w, 0, 0) != 's')
		fail("primary screen survives 1049");
	if (w.screen.cr != 1 || w.screen.cc != 2)
		fail("1049l restores the cursor");
	put(&w, "\033[H\0337\033[3;4H\0338");
	if (w.screen.cr != 0 || w.screen.cc != 0)
		fail("DECSC/DECRC");

	/* DEC line drawing. */
	put(&w, "\033[H\033(0lqk\033(B");
	if (at(&w, 0, 0) != 0x250c || at(&w, 1, 0) != 0x2500 ||
	    at(&w, 2, 0) != 0x2510)
		fail("DEC special graphics");
	put(&w, "\033[Hlqk");
	if (at(&w, 0, 0) != 'l')
		fail("ESC ( B goes back to ASCII");

	/* Deferred wrap: the tenth column does not wrap until the next char. */
	put(&w, "\033[H\033[2J0123456789");
	if (w.screen.cr != 0 || at(&w, 9, 0) != '9')
		fail("deferred wrap");
	put(&w, "a");
	if (w.screen.cr != 1 || at(&w, 0, 1) != 'a')
		fail("wrap on the next char");
	put(&w, "\033[?7l\033[H0123456789abc");
	if (w.screen.cr != 0 || at(&w, 9, 0) != 'c')
		fail("DECAWM off");
	put(&w, "\033[?7h");

	/* Erase uses the current background and drops attributes. */
	put(&w, "\033[H\033[2J\033[1;4;44m\033[K");
	c = dv_screen_at(&w.screen, 5, 0);
	if (c == nullptr || c->bg != 4 || c->attr != 0)
		fail("bce erase");
	put(&w, "\033[0m");

	dv_screen_resize(&w.screen, 20, 8);
	if (w.screen.cols != 20 || w.screen.rows != 8)
		fail("resize");
	dv_screen_free(&w.screen);

	/* OSC-52 clipboard is captured for the host, not drawn. */
	fresh(&w, 20, 4);
	put(&w, "\033]52;c;SGVsbG8=\007");
	if (w.host_out.len == 0 ||
	    memmem(w.host_out.data, w.host_out.len, "52;c;SGVsbG8=", 13) == NULL)
		fail("OSC-52 forwarded to host");
	if (memmem(w.host_out.data, w.host_out.len, "\033]", 2) == NULL)
		fail("OSC-52 keeps its introducer");
	if (w.clip_query)
		fail("a clipboard SET is not a read query");
	dv_screen_free(&w.screen);
	dv_buf_free(&w.host_out);
	dv_buf_free(&w.cap);

	/* "52;c;?" is a read: the host answers, and the answer must come back. */
	fresh(&w, 20, 4);
	put(&w, "\033]52;c;?\033\\");
	if (!w.clip_query)
		fail("OSC-52 ? is flagged as a read query");
	if (w.host_out.len == 0)
		fail("the read query is still forwarded to the host");
	dv_screen_free(&w.screen);
	dv_buf_free(&w.host_out);
	dv_buf_free(&w.cap);

	/*
	 * Colours are per window (kitty OSC 21, xterm OSC 4/10/11/12): a set
	 * is recorded in the window's own palette for the compositor to
	 * apply, a query is answered from it, and a reset drops it. Replies
	 * go to the program's pty.
	 */
	{
		struct dv_session	 ses;
		int			 pfd[2];
		char			 rb[512];
		ssize_t			 rn;

		memset(&ses, 0, sizeof ses);
		fresh(&w, 20, 4);
		w.id = 7;
		w.session = &ses;
		ses.desktop.focused = &w;
		if (pipe(pfd) != 0)
			fail("pipe");
		fcntl(pfd[0], F_SETFL, O_NONBLOCK);
		w.pty_fd = pfd[1];

		put(&w, "\033]4;1;rgb:ff/00/00\033\\");
		if (!w.screen.pal.set[1] || w.screen.pal.idx[1] != (DV_COLOR_RGB | 0xff0000) ||
		    !w.screen.pal.any)
			fail("OSC 4 sets a palette entry in the window's palette");
		put(&w, "\033]4;1;?\033\\");
		rn = read(pfd[0], rb, sizeof rb - 1);
		rb[rn > 0 ? rn : 0] = '\0';
		if (rn <= 0 || strstr(rb, "\033]4;1;rgb:ffff/0000/0000\033\\") == NULL)
			fail("OSC 4 ; N ; ? answers the window's own colour");

		put(&w, "\033]21;1=?;foreground=?;cursor=?\033\\");
		rn = read(pfd[0], rb, sizeof rb - 1);
		rb[rn > 0 ? rn : 0] = '\0';
		if (rn <= 0 || strstr(rb, "\033]21;1=rgb:ffff/0000/0000;"
		    "foreground=rgb:e5e5/e5e5/e5e5;cursor=rgb:e5e5/e5e5/e5e5\033\\") == NULL)
			fail("OSC 21 queries answer palette and dynamic colours");

		put(&w, "\033]21;background=#0000ff;2=red\033\\");
		if (!w.screen.pal.bgset || w.screen.pal.bg != (DV_COLOR_RGB | 0x0000ff))
			fail("OSC 21 sets the background from #rrggbb");
		if (!w.screen.pal.set[2] || w.screen.pal.idx[2] != (DV_COLOR_RGB | 0xff0000))
			fail("OSC 21 accepts a colour name");
		put(&w, "\033]11;?\033\\");
		rn = read(pfd[0], rb, sizeof rb - 1);
		rb[rn > 0 ? rn : 0] = '\0';
		if (rn <= 0 || strstr(rb, "\033]11;rgb:0000/0000/ffff\033\\") == NULL)
			fail("OSC 11 ; ? sees the OSC 21 background");

		/* The cursor colour is the host's, for the focused window. */
		dv_buf_free(&w.host_out);
		put(&w, "\033]12;rgb:00/ff/00\033\\");
		if (!host_has(&w, "\033]12;rgb:0000/ffff/0000\033\\"))
			fail("a focused window's cursor colour goes to the host");
		dv_buf_free(&w.host_out);
		put(&w, "\033]112\033\\");
		if (!host_has(&w, "\033]112\033\\"))
			fail("resetting the cursor colour resets the host's");
		if (w.screen.pal.curset)
			fail("OSC 112 clears the cursor override");

		/* Push and pop the whole colour state (OSC 30001 / 30101). */
		put(&w, "\033]30001\033\\");
		put(&w, "\033]4;1;rgb:00/00/ff\033\\\033]104;2\033\\\033]111\033\\");
		if (w.screen.pal.idx[1] != (DV_COLOR_RGB | 0x0000ff) || w.screen.pal.set[2] ||
		    w.screen.pal.bgset)
			fail("changes after a push take effect");
		put(&w, "\033]30101\033\\");
		if (w.screen.pal.idx[1] != (DV_COLOR_RGB | 0xff0000) || !w.screen.pal.set[2] ||
		    !w.screen.pal.bgset)
			fail("OSC 30101 restores the pushed colours");
		/* The stack has no maximum depth: twenty pushes, twenty pops. */
		for (int k = 0; k < 20; k++) {
			char	set[64];

			snprintf(set, sizeof set, "\033]30001\033\\\033]4;3;rgb:%02x/00/00\033\\", k);
			put(&w, set);
		}
		for (int k = 19; k >= 0; k--) {
			if (w.screen.pal.idx[3] != (DV_COLOR_RGB | ((uint32_t)k << 16)))
				fail("each pushed level comes back in turn");
			put(&w, "\033]30101\033\\");
		}
		if (w.screen.pal.idx[1] != (DV_COLOR_RGB | 0xff0000))
			fail("the bottom of a deep stack is the first push");
		put(&w, "\033]104\033\\\033]110\033\\\033]111\033\\");
		if (w.screen.pal.any)
			fail("resetting everything leaves no override");

		/* Pointer shapes: a per-window stack, applied when focused. */
		dv_buf_free(&w.host_out);
		put(&w, "\033]22;>wait,grab\033\\");
		if (w.npshape != 2 || strcmp(w.pshape[1], "grab") != 0)
			fail("OSC 22 > pushes shapes");
		if (!host_has(&w, "\033]22;grab\033\\"))
			fail("the focused window's pointer shape reaches the host");
		dv_buf_free(&w.host_out);
		put(&w, "\033]22;<\033\\");
		if (w.npshape != 1 ||
		    !host_has(&w, "\033]22;wait\033\\"))
			fail("OSC 22 < pops back to the previous shape");
		dv_buf_free(&w.host_out);
		put(&w, "\033]22;\033\\");
		if (w.npshape != 0 ||
		    !host_has(&w, "\033]22;\033\\"))
			fail("an empty OSC 22 resets to the default pointer");
		dv_buf_free(&w.host_out);
		put(&w, "\033]22;?\033\\");
		if (!host_has(&w, "\033]22;?\033\\"))
			fail("a pointer query is forwarded to the host");
		if (ses.routes[0].osc != 22 || ses.routes[0].win != 7)
			fail("the pointer query's reply is routed back to the window");

		/* Forwarded OSCs remember who asked, by OSC number and id. */
		dv_buf_free(&w.host_out);
		put(&w, "\033]5522;type=read:id=q1;dGV4dC9wbGFpbg==\033\\");
		if (!host_has(&w, "\033]5522;type=read:id=q1;"))
			fail("OSC 5522 is forwarded to the host");
		if (ses.routes[1].osc != 5522 || strcmp(ses.routes[1].id, "q1") != 0 ||
		    ses.routes[1].win != 7)
			fail("OSC 5522 routes by id");
		put(&w, "\033]99;i=n9:d=0:a=focus;hello\033\\");
        if (!host_has(&w, ".n.n9:") || !host_has(&w, ":r=odin-seat/"))
            fail("OSC 99 carries a scoped id and return route");
		put(&w, "\033]5522;type=read:id=q1;dGV4dA==\033\\");
		if (ses.routes[3].osc != 0 || ses.routes[1].seq != ses.route_seq)
			fail("a repeated id refreshes its route rather than adding one");
		{
			char	v[32];

			if (dv_meta_val("a=1:b=xyz:c=", 12, "b", v, sizeof v) != 3 ||
			    strcmp(v, "xyz") != 0)
				fail("dv_meta_val finds a colon-separated value");
			if (dv_meta_val("id=abc;rest", 11, "id", v, sizeof v) != 3 ||
			    strcmp(v, "abc") != 0)
				fail("dv_meta_val stops at ; too");
			if (dv_meta_val("a=1:b=xyz", 9, "zz", v, sizeof v) != 0 || v[0] != 0)
				fail("dv_meta_val reports an absent key as empty");
		}
		close(pfd[0]);
		close(pfd[1]);
		w.pty_fd = -1;
		dv_screen_free(&w.screen);
		dv_buf_free(&w.host_out);
		dv_buf_free(&w.cap);
	}

	/*
	 * A kitty image is a placement floating over the grid, as in kitty:
	 * sized from its pixels at the host's cell size, anchored where the
	 * cursor was, and not a text cell -- redrawing the text under it
	 * leaves it be. The cursor ends after it, as after a run of text.
	 */
	fresh(&w, 20, 6);
	w.id = 1;
	put(&w, "\033[3;5H");		/* cursor to row 2, col 4 (0-based) */
	put(&w, "\033_Ga=T,f=32,s=32,v=64;QUJD\033\\");
	p = first_place(&w);
	if (p == NULL)
		fail("a=T displays: a placement exists");
	else {
		if (!w.kimg->at(p->img).done)
			fail("image stored");
		if (p->dv_id < DV_KITTY_FIRST_ID)
			fail("placement id is in our host-global space");
		if (p->cols != 2 || p->rows != 2)
			fail("image measured to cells (32x64 px / 16x32)");
		if (p->row != 2 || p->col != 4)
			fail("placed at the cursor");
		if (w.screen.cr != 3 || w.screen.cc != 6)
			fail("cursor ends on the image's last row, to its right");
		put(&w, "\033[3;5H  \033[4;5H  ");	/* redraw the text beneath */
		if (!p->used)
			fail("text written under the image leaves it in place");
		put(&w, "\033[2J");
		if (count_places(&w) != 0)
			fail("clearing the screen removes the image");
	}
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);

	/*
	 * A previewer stores with a=t and places with a=p, giving c/r: it
	 * renders extra pixels for sharpness and relies on c/r for the cell
	 * size. Re-placing with the same placement id replaces; d=I deletes
	 * the image and its placements.
	 */
	fresh(&w, 20, 6);
	w.id = 1;
	put(&w, "\033_Ga=t,f=32,s=64,v=64,i=7,q=2;QUJD\033\\");
	if (first_place(&w) != NULL)
		fail("a=t only stores");
	put(&w, "\033[2;3H\033_Ga=p,i=7,p=3,c=2,r=1,q=2\033\\");
	p = first_place(&w);
	if (p == NULL || p->cols != 2 || p->rows != 1 || p->pid != 3)
		fail("a=p places at c/r, not the pixel size");
	if (p != NULL && (p->row != 1 || p->col != 2))
		fail("a=p places at the cursor");
	put(&w, "\033[1;1H\033_Ga=p,i=7,p=3,c=3,r=2,q=2\033\\");
	if (count_places(&w) != 1 || first_place(&w)->cols != 3)
		fail("the same placement id replaces the old placement");
	put(&w, "\033_Ga=p,i=7,p=4,c=1,r=1,q=2\033\\");
	if (count_places(&w) != 2)
		fail("a new placement id adds a placement");
	put(&w, "\033_Ga=d,d=I,i=7,q=2\033\\");
	if (count_places(&w) != 0)
		fail("d=I deletes the image's placements");
	if (w.kimg->contains(7))
		fail("d=I frees the image");
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);

	/* Scrolling carries an image with the text; off the top it is gone. */
	fresh(&w, 20, 4);
	w.id = 1;
	put(&w, "\033[3;1H\033_Ga=T,f=32,s=16,v=32,C=1;QUJD\033\\");
	put(&w, "\033[4;1H\n\n");		/* two scrolls */
	p = first_place(&w);
	if (p == NULL || p->row != 0)
		fail("scrolling moves the placement up with the text");
	put(&w, "\n");
	if (count_places(&w) != 0)
		fail("an image scrolled off the top is deleted");
	/* A tall image scrolls the screen so its bottom (and the prompt) show. */
	put(&w, "\033[H\033_Ga=T,f=32,s=16,v=320;QUJD\033\\");	/* 1x10 cells */
	p = first_place(&w);
	if (p == NULL || p->row != -6 || w.screen.cr != 3)
		fail("a tall image scrolls like text; the cursor is at the bottom");
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);

	/* Several images with their own ids coexist. */
	fresh(&w, 20, 6);
	w.id = 1;
	put(&w, "\033[1;1H\033_Ga=T,f=32,s=16,v=32,i=1,C=1;QUJD\033\\");
	put(&w, "\033[3;1H\033_Ga=T,f=32,s=16,v=32,i=2,C=1;RUZH\033\\");
	if (count_places(&w) != 2)
		fail("two images make two placements");
	if (w.screen.kpl->begin()->second.dv_id ==
	    std::next(w.screen.kpl->begin())->second.dv_id)
		fail("each placement has its own host id");
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);

	/*
	 * Placeholder mode (what a program uses inside tmux): a=p,U=1
	 * registers a virtual placement and the program writes U+10EEEE
	 * cells with row/column diacritics and the image id in the
	 * foreground. Those cells map to our host id for that image.
	 */
	fresh(&w, 20, 6);
	w.id = 1;
	put(&w, "\033_Ga=t,f=32,s=32,v=64,i=5,q=2;QUJD\033\\");
	put(&w, "\033_Ga=p,U=1,i=5,c=2,r=2,q=2\033\\");
	put(&w, "\033[H\033[38;5;5m\U0010EEEE̅̅\U0010EEEE̅̍"
	    "\U0010EEEE");
	{
		struct dv_cell	*c0 = dv_screen_at(&w.screen, 0, 0);
		struct dv_cell	*c1 = dv_screen_at(&w.screen, 1, 0);
		struct dv_cell	*c2 = dv_screen_at(&w.screen, 2, 0);

		if (c0->ch != DV_KITTY_PLACEHOLDER || c0->fg != 5 ||
		    c0->irow != 0 || c0->icol != 0)
			fail("placeholder cell carries the image id and row/col");
		if (c1->icol != 1 || c1->irow != 0)
			fail("column diacritic decoded");
		if (c2->ch != DV_KITTY_PLACEHOLDER || c2->icol != 2)
			fail("a placeholder without diacritics continues the row");
		if (dv_kitty_virtual_id(&w, 5) == 0)
			fail("the virtual placement is known under our host id");
		if (dv_kitty_virtual_id(&w, 6) != 0)
			fail("unknown image ids map to nothing");
	}
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);

	/* A chunked transmission is complete only after the final m=0, and
	 * the compression key travels with the pixels. */
	fresh(&w, 20, 6);
	w.id = 2;
	put(&w, "\033_Ga=T,f=32,s=16,v=16,o=z,m=1;QUJD\033\\");
	if (first_place(&w) != NULL)
		fail("m=1 chunk is not complete yet");
	put(&w, "\033_Gm=0;RUZH\033\\");
	if (first_place(&w) == NULL)
		fail("m=0 completes the transmission and displays");
	if (w.kimg->at(first_place(&w)->img).data.len != 8 ||
	    memcmp(w.kimg->at(first_place(&w)->img).data.data, "QUJDRUZH", 8) != 0)
		fail("chunk payloads joined, framing stripped");
	if (strstr(w.kimg->at(first_place(&w)->img).fmt, "o=z") == NULL)
		fail("o=z is kept for the host");
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);

	/*
	 * A sixel (DCS q ...) is decoded to pixels and shown as an image of
	 * the window's own, so a host that speaks kitty graphics sees one
	 * protocol. Here: define colour 0 red, select it, one all-on data
	 * column -- a 1x6 pixel picture.
	 */
	fresh(&w, 20, 6);
	w.id = 1;
	put(&w, "\033Pq#0;2;100;0;0#0~\033\\");
	p = first_place(&w);
	if (p == NULL)
		fail("a sixel becomes a placement");
	else {
		struct dv_kimage	*im = &w.kimg->at(p->img);

		if (!im->done)
			fail("the decoded sixel is stored as an image");
		if (im->wpx != 1 || im->hpx != 6)
			fail("the sixel is measured to its pixel size (1x6)");
		if (strstr(im->fmt, "o=z") == NULL || strstr(im->fmt, "f=32") == NULL)
			fail("a decoded sixel is deflated RGBA for the host");
	}
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);

	/*
	 * Scrollback: lines that scroll off the top of the primary screen
	 * are kept, the view can scroll back into them, and new output or
	 * (in the server) a keypress snaps back to the live bottom. The
	 * alternate screen keeps no history.
	 */
	fresh(&w, 8, 3);
	w.id = 1;
	{
		const char	*ln[] = { "A\r\n", "B\r\n", "C\r\n", "D\r\n",
		    "E\r\n", "F\r\n", "G\r\n", "H\r\n" };
		unsigned	 k;

		for (k = 0; k < sizeof ln / sizeof ln[0]; k++)
			put(&w, ln[k]);
	}
	if (w.screen.sb_len == 0)
		fail("lines scrolling off the top enter scrollback");
	if (w.screen.sb_off != 0)
		fail("the view starts at the live bottom");
	if (!dv_screen_scroll(&w.screen, 100))
		fail("scrolling back changes the view");
	if (w.screen.sb_off == 0)
		fail("the view is scrolled back into history");
	{
		struct dv_cell	*c = dv_screen_disp_at(&w.screen, 0, 0);

		if (c == NULL || c->ch != U'A')
			fail("the oldest scrollback line shows at the top");
	}
	put(&w, "X");			/* new output snaps to the bottom */
	if (w.screen.sb_off != 0)
		fail("new output returns to the live bottom");
	if (dv_screen_disp_at(&w.screen, 0, 0)->ch != U'G')
		fail("at the bottom the live screen shows");
	put(&w, "\033[?1049h");		/* the alternate screen has no history */
	if (dv_screen_scroll(&w.screen, 5))
		fail("no scrollback on the alternate screen");
	dv_screen_free(&w.screen);
	dv_buf_free(&w.cap);

	/*
	 * OSC 66 text sizing: a run occupies (base width * scale) columns by
	 * (scale) rows, the cursor advances by the scaled width on the same
	 * row (which is how a program detects support), and RIS clears it.
	 */
	fresh(&w, 20, 6);
	w.id = 1;
	put(&w, "\033[H");			/* cursor to 0,0 */
	put(&w, "\033]66;s=2;Hi\a");		/* "Hi" (width 2) at scale 2 */
	if (w.screen.cc != 4 || w.screen.cr != 0)
		fail("cursor advances by the scaled width, staying on the row");
	if (dv_screen_at(&w.screen, 0, 0)->ch != DV_STEXT_MARK)
		fail("the run's origin is a scaled-text cell");
	{
		struct dv_cell	*cc = dv_screen_at(&w.screen, 3, 1);

		if (cc->ch != DV_STEXT_MARK || cc->irow != 1 || cc->icol != 3)
			fail("the run fills a 4x2 rectangle of cells");
	}
	{
		const struct dv_stext	*run = w.screen.stexts == nullptr ? nullptr :
		    &w.screen.stexts->at(dv_screen_at(&w.screen, 0, 0)->fg);

		if (run == nullptr || run->s != 2 || run->cols != 4 ||
		    run->rows != 2 || run->text != "Hi")
			fail("the run records its scale, footprint and text");
	}
	/* An explicit width: w=3 at scale 2 is six columns. */
	put(&w, "\033[3;1H\033]66;w=3:s=2;X\a");
	if (w.screen.cc != 6)
		fail("w= sets the base width (6 columns at w=3,s=2)");
	/* Support detection: a scale run moves the cursor (unsupported would
	 * leave it put). */
	put(&w, "\033[5;1H\033]66;s=3;Z\a");
	if (w.screen.cc != 3)
		fail("a scale-3 run advances the cursor three columns");
	put(&w, "\033c");			/* RIS clears the runs */
	if (w.screen.stexts != nullptr)
		fail("RIS clears scaled-text runs");
	dv_screen_free(&w.screen);
	dv_buf_free(&w.cap);

	/*
	 * Outgoing transmission medium is tunable (ODIN_SEAT_KITTY_MEDIUM)
	 * and defaults to shared memory; direct, temp file and file also
	 * work. The image here is a 1x1 RGB pixel "ABC".
	 */
	fresh(&w, 20, 6);
	w.id = 7;
	put(&w, "\033[H\033_Ga=T,f=24,s=1,v=1;QUJD\a");
	p = first_place(&w);
	if (p == NULL)
		fail("the image made a placement to transmit");
	else {
		struct dv_buf	tb;
		char		nm[512], raw[64];
		size_t		nn;

		/* Default: shared memory. Verify the shm holds "ABC". */
		unsetenv("ODIN_SEAT_KITTY_MEDIUM");
		memset(&tb, 0, sizeof tb);
		dv_kitty_transmit(&w, p, &tb);
		if (memmem(tb.data, tb.len, "t=s,", 4) == NULL)
			fail("the default emit medium is shared memory (t=s)");
		nn = medium_name(&tb, 's', nm, sizeof nm - 1);
		if (nn == 0)
			fail("the shm transmit names a shared-memory object");
		else {
			int	fd;

			nm[nn] = '\0';
			fd = shm_open(nm, O_RDONLY, 0);
			if (fd < 0)
				fail("the named shared-memory object exists");
			else {
				void	*mp = mmap(NULL, 3, PROT_READ, MAP_SHARED,
				    fd, 0);

				if (mp == MAP_FAILED || memcmp(mp, "ABC", 3) != 0)
					fail("the shm holds the decoded pixels");
				else
					munmap(mp, 3);
				close(fd);
				shm_unlink(nm);
			}
			(void)raw;
		}
		dv_buf_free(&tb);

		/* Direct: inline base64, no medium key. */
		setenv("ODIN_SEAT_KITTY_MEDIUM", "direct", 1);
		memset(&tb, 0, sizeof tb);
		dv_kitty_transmit(&w, p, &tb);
		if (memmem(tb.data, tb.len, "t=s", 3) != NULL ||
		    memmem(tb.data, tb.len, "t=f", 3) != NULL ||
		    memmem(tb.data, tb.len, "t=t", 3) != NULL)
			fail("direct emits no out-of-band medium key");
		if (memmem(tb.data, tb.len, "QUJD", 4) == NULL)
			fail("direct emits the pixels inline as base64");
		dv_buf_free(&tb);

		/* Temp file: t=t, and the file exists (host would delete it). */
		setenv("ODIN_SEAT_KITTY_MEDIUM", "tempfile", 1);
		memset(&tb, 0, sizeof tb);
		dv_kitty_transmit(&w, p, &tb);
		if (memmem(tb.data, tb.len, "t=t,", 4) == NULL)
			fail("tempfile emits t=t");
		nn = medium_name(&tb, 't', nm, sizeof nm - 1);
		if (nn > 0) {
			struct stat	stt;

			nm[nn] = '\0';
			if (stat(nm, &stt) != 0 || stt.st_size != 3)
				fail("the temp file holds the pixels");
			unlink(nm);
		} else
			fail("the temp-file transmit names a path");
		dv_buf_free(&tb);

		/* File: t=f. */
		setenv("ODIN_SEAT_KITTY_MEDIUM", "file", 1);
		memset(&tb, 0, sizeof tb);
		dv_kitty_transmit(&w, p, &tb);
		if (memmem(tb.data, tb.len, "t=f,", 4) == NULL)
			fail("file emits t=f");
		nn = medium_name(&tb, 'f', nm, sizeof nm - 1);
		if (nn > 0) {
			nm[nn] = '\0';
			unlink(nm);		/* we would otherwise defer this */
		}
		dv_buf_free(&tb);
		unsetenv("ODIN_SEAT_KITTY_MEDIUM");
	}
	dv_screen_free(&w.screen);
	dv_kitty_free(&w);
	dv_buf_free(&w.cap);

	/* CSI 8 ; rows ; cols t records a resize request for the server. */
	fresh(&w, 20, 6);
	w.id = 9;
	put(&w, "\033[8;12;40t");
	if (!w.resize_req || w.req_rows != 12 || w.req_cols != 40)
		fail("CSI 8 t records the requested rows and columns");
	dv_screen_free(&w.screen);
	dv_buf_free(&w.cap);

	/*
	 * Kitty extensions in the VT. Styled/coloured underlines: 4:n picks
	 * a style, 58/59 the colour, 21 is double, 24 clears; 221/222 reset
	 * bold and faint independently.
	 */
	fresh(&w, 10, 4);
	put(&w, "\033[4:3m\033[58;2;255;0;0mx");
	if (w.screen.ustyle != 3 || !(w.screen.attr & DV_ATTR_UNDER))
		fail("SGR 4:3 sets a curly underline");
	if (w.screen.ucolor != DV_RGB(255, 0, 0))
		fail("SGR 58;2;r;g;b sets the underline colour");
	{
		struct dv_cell	*c = dv_screen_at(&w.screen, 0, 0);

		if (c->ch != U'x' || c->ustyle != 3 || c->ucolor != DV_RGB(255, 0, 0))
			fail("a cell records its underline style and colour");
	}
	put(&w, "\033[59m");
	if (w.screen.ucolor != DV_COLOR_DEFAULT)
		fail("SGR 59 resets the underline colour");
	put(&w, "\033[21m");
	if (w.screen.ustyle != 2)
		fail("SGR 21 is a double underline");
	put(&w, "\033[24m");
	if (w.screen.ustyle != 0 || (w.screen.attr & DV_ATTR_UNDER))
		fail("SGR 24 clears the underline");
	put(&w, "\033[1;2m\033[221m");
	if ((w.screen.attr & DV_ATTR_BOLD) || !(w.screen.attr & DV_ATTR_DIM))
		fail("SGR 221 resets bold only");
	put(&w, "\033[1m\033[222m");
	if (!(w.screen.attr & DV_ATTR_BOLD) || (w.screen.attr & DV_ATTR_DIM))
		fail("SGR 222 resets faint only");
	dv_screen_free(&w.screen);
	dv_buf_free(&w.cap);

	/* DECCARA applies SGR to a region: a rectangle under DECSACE 2, a
	 * stream otherwise. */
	fresh(&w, 8, 3);
	put(&w, "\033[0mabcdefgh\r\nijklmnop");
	put(&w, "\033[2*x\033[1;1;2;2;7$r");	/* rectangle rows 1-2, cols 1-2 */
	if (!(dv_screen_at(&w.screen, 0, 0)->attr & DV_ATTR_REV) ||
	    !(dv_screen_at(&w.screen, 1, 1)->attr & DV_ATTR_REV))
		fail("DECCARA rectangle styles the corners");
	if (dv_screen_at(&w.screen, 2, 0)->attr & DV_ATTR_REV)
		fail("DECCARA rectangle leaves cells outside alone");
	put(&w, "\033[0*x\033[1;3;2;2;1$r");	/* stream (1,3) .. (2,2) */
	if (!(dv_screen_at(&w.screen, 2, 0)->attr & DV_ATTR_BOLD) ||
	    !(dv_screen_at(&w.screen, 7, 0)->attr & DV_ATTR_BOLD) ||
	    !(dv_screen_at(&w.screen, 0, 1)->attr & DV_ATTR_BOLD) ||
	    !(dv_screen_at(&w.screen, 1, 1)->attr & DV_ATTR_BOLD))
		fail("DECCARA stream covers the run from start to end cell");
	if ((dv_screen_at(&w.screen, 1, 0)->attr & DV_ATTR_BOLD) ||
	    (dv_screen_at(&w.screen, 2, 1)->attr & DV_ATTR_BOLD))
		fail("DECCARA stream stops at its bounds");
	dv_screen_free(&w.screen);
	dv_buf_free(&w.cap);

	/* Unscroll (CSI n + T) pulls lines back out of scrollback; CSI 22 J
	 * pushes the whole screen into it. */
	fresh(&w, 8, 3);
	put(&w, "A\r\nB\r\nC\r\nD\r\nE\r\n");	/* screen D,E,_ ; scrollback A,B,C */
	if (w.screen.sb_len != 3)
		fail("three lines scrolled into scrollback");
	put(&w, "\033[2+T");
	if (dv_screen_at(&w.screen, 0, 0)->ch != U'B' ||
	    dv_screen_at(&w.screen, 0, 1)->ch != U'C' ||
	    dv_screen_at(&w.screen, 0, 2)->ch != U'D')
		fail("unscroll refills the top from the newest scrollback lines");
	if (w.screen.sb_len != 1)
		fail("unscrolled lines leave the scrollback");
	put(&w, "\033[22J");
	if (w.screen.sb_len != 4 || dv_screen_at(&w.screen, 0, 0)->ch != U' ')
		fail("CSI 22 J moves the screen into scrollback and clears it");
	dv_screen_free(&w.screen);
	dv_buf_free(&w.cap);

	/* XTSAVE / XTRESTORE: named modes, and all side-effect-free modes
	 * when no parameter is given. Pixel mouse mode 1016 is tracked. */
	fresh(&w, 8, 3);
	put(&w, "\033[?1h\033[?1s\033[?1l\033[?1r");
	if (!w.screen.app_cursor)
		fail("XTSAVE/XTRESTORE of a named mode");
	put(&w, "\033[?1h\033[?7l\033[?2004h\033[?1016h\033[?s");
	put(&w, "\033[?1l\033[?7h\033[?2004l\033[?1016l");
	if (w.screen.app_cursor || !w.screen.wrap || w.screen.bracketed ||
	    w.screen.mouse_sgr)
		fail("modes changed after the save");
	put(&w, "\033[?r");
	if (!w.screen.app_cursor || w.screen.wrap || !w.screen.bracketed ||
	    w.screen.mouse_sgr != 2)
		fail("XTRESTORE with no parameter restores every saved mode");
	dv_screen_free(&w.screen);
	dv_buf_free(&w.cap);

	/*
	 * DECRQM says which private modes are kept here, and how each stands,
	 * so a program learns it may use synchronized output and pixel mouse
	 * reports. A synchronized update (mode 2026) is one frame: the parser
	 * stops where it ends, so what follows is not shown with it.
	 */
	fresh(&w, 8, 3);
	{
		int	pfd[2];
		char	rb[128];
		ssize_t	rn;

		if (pipe(pfd) != 0)
			fail("pipe");
		fcntl(pfd[0], F_SETFL, O_NONBLOCK);
		w.pty_fd = pfd[1];
		put(&w, "\033[?2026$p\033[?1016$p\033[?9999$p");
		rn = read(pfd[0], rb, sizeof rb - 1);
		rb[rn > 0 ? rn : 0] = '\0';
		if (strcmp(rb, "\033[?2026;2$y\033[?1016;2$y\033[?9999;0$y") != 0)
			fail("DECRQM reports a kept mode reset and an unknown one unrecognised");
		put(&w, "\033[?2026h\033[?1016h\033[?2026$p\033[?1016$p");
		rn = read(pfd[0], rb, sizeof rb - 1);
		rb[rn > 0 ? rn : 0] = '\0';
		if (!w.screen.sync || strcmp(rb, "\033[?2026;1$y\033[?1016;1$y") != 0)
			fail("DECRQM reports a mode that is set");
		close(pfd[0]);
		close(pfd[1]);
		w.pty_fd = -1;
	}
	{
		const char	*upd = "ab\033[?2026lcd";
		int		 ended = 0;
		size_t		 used = dv_vt_write_frame(&w, upd, strlen(upd), &ended);

		if (used != 10 || !ended || w.screen.sync)
			fail("the parser stops where a synchronized update ends");
		if (dv_screen_at(&w.screen, 1, 0)->ch != 'b' ||
		    dv_screen_at(&w.screen, 2, 0)->ch == 'c')
			fail("what follows an update is not drawn with it");
		used = dv_vt_write_frame(&w, upd + used, strlen(upd) - used, &ended);
		if (used != 2 || ended || dv_screen_at(&w.screen, 2, 0)->ch != 'c')
			fail("the rest is drawn when it is written on");
		put(&w, "\033[?2026h");
		used = dv_vt_write_frame(&w, "x\033[?2026l", 9, &ended);
		if (used != 9 || !ended)
			fail("an update ending with the bytes written has ended");
		used = dv_vt_write_frame(&w, "\033[?2026lz", 9, &ended);
		if (used != 9 || ended)
			fail("a reset with no update under way ends none");
		/* All of it through the plain entry: nothing is left unread. */
		put(&w, "\033[H\033[?2026hQ\033[?2026lR\033[?2026hS");
		if (dv_screen_at(&w.screen, 2, 0)->ch != 'S' || !w.screen.sync)
			fail("dv_vt_write takes every update it is given");
		put(&w, "\033c");
		if (w.screen.sync)
			fail("RIS ends a synchronized update");
	}
	dv_screen_free(&w.screen);
	dv_buf_free(&w.cap);
	dv_buf_free(&w.pty_out);

	/*
	 * Sized text: as many runs as the screen's cells name, with the
	 * fractional scale and alignment a program gave; those drawn over
	 * are forgotten rather than piling up.
	 */
	fresh(&w, 40, 10);
	for (unsigned i = 0; i < 100; i++) {
		char	run[64];

		snprintf(run, sizeof run, "\033[%u;%uH\033]66;n=4:d=5:w=2:v=2:h=1;%02u\033\\",
		    i / 20 + 1, (i % 20) * 2 + 1, i);
		put(&w, run);
	}
	{
		unsigned	held = 0;

		for (unsigned i = 0; i < 100; i++) {
			const struct dv_cell	*cell = dv_screen_at(&w.screen,
			    (i % 20) * 2, i / 20);
			char			 want[8];

			snprintf(want, sizeof want, "%02u", i);
			if (cell->ch != DV_STEXT_MARK || w.screen.stexts == nullptr)
				continue;
			auto it = w.screen.stexts->find(cell->fg);

			if (it != w.screen.stexts->end() && it->second.text == want &&
			    it->second.n == 4 && it->second.d == 5 && it->second.v == 2 &&
			    it->second.h == 1 && it->second.wparam == 2 &&
			    it->second.cols == 2 && it->second.rows == 1)
				held++;
		}
		if (held != 100)
			fail("every sized-text run on the screen is kept, with its fraction");
	}
	for (unsigned i = 0; i < 5000; i++)
		put(&w, "\033[H\033]66;n=1:d=2:w=1;x\033\\");
	if (w.screen.stexts == nullptr || w.screen.stexts->size() > 400)
		fail("sized-text runs drawn over are forgotten");
	{
		const struct dv_cell	*cell = dv_screen_at(&w.screen, 2, 0);

		if (cell->ch != DV_STEXT_MARK || !w.screen.stexts->contains(cell->fg))
			fail("a sweep keeps the runs the cells still name");
	}
	put(&w, "\033[H\033]66;n=9:d=5:v=7;bad\033\\");
	{
		const struct dv_cell	*cell = dv_screen_at(&w.screen, 0, 0);
		auto it = w.screen.stexts->find(cell->fg);

		if (it == w.screen.stexts->end() || it->second.n != 0 ||
		    it->second.d != 0 || it->second.v != 0 || it->second.cols != 3)
			fail("a fraction that is not one, and an alignment that is none, are dropped");
	}
	dv_screen_free(&w.screen);
	dv_buf_free(&w.cap);

	test_xfer();
	test_kbd();
	test_kbd_stack();
	test_xcursors();
	test_xtgettcap();
	test_cells_codec();
	if (failed) {
		fprintf(stderr, "test-term: FAILED\n");
		return (1);
	}
	printf("test-term: ok\n");
	return (0);
}
