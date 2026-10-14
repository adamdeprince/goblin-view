/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Ghostty is the host terminal. We turn on the kitty keyboard protocol
 * (disambiguate + events + report-all + text, flags 27) so an isolated
 * Alt tap is the menu key, XTWINOPS / mode 2048 for cell pixels, and a
 * kitty graphics query. Inner PTY keys are re-encoded to legacy bytes
 * until libghostty-vt owns per-window keyboard mode.
 */

#include "dv.hpp"
#include "silkie.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define KITTY_LEFT_ALT		57443u
#define KITTY_RIGHT_ALT		57449u
#define KITTY_FIRST_MOD		57441u
#define KITTY_LAST_MOD		57452u
#define KITTY_FUNCTIONAL	57344u

#define MOD_SHIFT	1u
#define MOD_ALT		2u
#define MOD_CTRL	4u
#define MOD_SUPER	8u

/* How long we wait for Ghostty's size replies before attaching. */
#define BOOTSTRAP_MS	250

void
dv_host_init(struct dv_host *h)
{
	memset(h, 0, sizeof *h);
}

static void
host_write(int fd, const char *s)
{
	size_t	 n = strlen(s);
	ssize_t	 w;

	while (n > 0) {
		w = write(fd, s, n);
		if (w < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return;
		}
		s += w;
		n -= (size_t)w;
	}
}

static long long
now_ms(void)
{
	struct timespec	ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void
apply_size(struct dv_size *sz, const struct dv_size *ev)
{
	if (ev->cols)
		sz->cols = ev->cols;
	if (ev->rows)
		sz->rows = ev->rows;
	if (ev->xpixel)
		sz->xpixel = ev->xpixel;
	if (ev->ypixel)
		sz->ypixel = ev->ypixel;
}

/*
 * Push the modes we need and wait briefly for the replies, so the
 * session is created at the measured cell size. Anything typed while
 * we wait is queued, not lost. The tty stays blocking: it is the same
 * open file as stdout, and a non-blocking stdout drops frames.
 */
void
dv_host_bootstrap(struct dv_host *h, int in_fd, int out_fd, struct dv_size *sz)
{
	long long	deadline = now_ms() + BOOTSTRAP_MS;

	/*
	 * Keyboard flags, in-band resize, cell and text-area sizes, then a
	 * kitty graphics query with DA1 behind it: a host that takes kitty
	 * graphics answers the query before DA1, one that does not answers
	 * only DA1, and either way DA1 ends the wait.
	 */
	host_write(out_fd, "\033[>27u\033[?2048h\033[16t\033[18t"
	    "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\\033[>q"
	    "\033]777;goblin-pointer;v=1:op=query:id=17349522\033\\");
	host_write(out_fd, dv_silkie_probe());
	host_write(out_fd, "\033]72;t=q:i=gv\033\\\033[> q\033[c");
	for (;;) {
		struct pollfd	pf;
		unsigned char	buf[512];
		ssize_t		n;
		long long	left = deadline - now_ms();

		if (left <= 0)
			break;
		if (h->have_cell && h->cols && h->rows && h->da)
			break;
		pf.fd = in_fd;
		pf.events = POLLIN;
		n = poll(&pf, 1, (int)left);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		n = read(in_fd, buf, sizeof buf);
		if (n <= 0)
			break;
		dv_host_feed(h, buf, (size_t)n);
		for (;;) {
			struct dv_host_ev	ev;

			if (!dv_host_next(h, &ev))
				break;
			if (ev.type == DV_HEV_SIZE) {
				if (sz != nullptr)
					apply_size(sz, &ev.size);
			} else if (ev.type == DV_HEV_OSC) {
				/* OSC 72 t=q answered: the host does drag and drop */
				if (h->osc.len >= 8 && memmem(h->osc.data,
				    h->osc.len < 12 ? h->osc.len : 12, "72;t=q", 6) != NULL)
					h->dnd = 1;
                goblin::pointer::Message pointer;
                if (goblin::pointer::parse(std::string_view(reinterpret_cast<const char *>(h->osc.data),h->osc.len),pointer)
                    && pointer.op == goblin::pointer::Op::Cap && pointer.id == 17349522)
                    h->pointer_caps = pointer.caps;
				dv_silkie_note(h, h->osc.data, h->osc.len);
				h->osc.len = 0;
			}
			else if (h->npend < sizeof h->pend / sizeof h->pend[0])
				h->pend[h->npend++] = ev;
		}
	}
}

/*
 * The keyboard mode stack is per screen, so pop it before leaving the
 * alternate screen the client entered at attach.
 */
void
dv_host_disable(int fd)
{
	/*
	 * Leave the terminal as we found it. The screen is cleared before
	 * the alternate screen is left: a terminal that has one discards
	 * the cleared page and shows the saved screen again, and one that
	 * has none (mosh) is left with a clean page and the cursor at the
	 * top instead of the desktop we painted over its shell.
	 */
	static const char seq[] =
	    "\033[0m"
	    "\033[?25h"
	    "\033[?1000l\033[?1002l\033[?1003l\033[?1006l"
	    "\033]777;goblin-pointer;v=1:op=mode:mode=0\033\\"
	    "\033]777;caps;v=1:op=reset\033\\"
	    "\033[?2004l"
	    "\033[<u"
	    "\033]72;t=A\033\\"		/* no more drops for us */
	    "\033[>0;4 q"			/* no extra cursors either */
	    "\033]22;\033\\"			/* the pointer shape we may have set */
	    "\033]112\033\\"			/* and the cursor colour */
	    "\033[?2048l"
	    "\033[2J\033[H"
	    "\033[?1049l";

	(void)!write(fd, seq, sizeof seq - 1);
}

static void
host_consume(struct dv_host *h, size_t n)
{
	if (n >= h->len)
		h->len = 0;
	else {
		memmove(h->buf, h->buf + n, h->len - n);
		h->len -= n;
	}
}

/*
 * A host OSC reply (the clipboard for an OSC-52 read, say) can run to
 * kilobytes and arrive in pieces, so it is collected in h->osc rather
 * than the fixed key buffer, terminator included, until BEL or ESC \.
 */
static int
osc_collect(struct dv_host *h, const unsigned char *b, size_t n, size_t *used)
{
	size_t	i;

	for (i = 0; i < n; i++) {
		unsigned char	c = b[i];

		dv_buf_append(&h->osc, &c, 1);
		if (c == 0x07 || (h->osc_esc && c == '\\')) {
			h->osc_esc = 0;
			h->in_osc = 0;
			h->osc_ready = 1;
			*used = i + 1;
			return (1);
		}
		h->osc_esc = (c == 0x1b);
		if (dv_escape_limit && h->osc.len > dv_escape_limit) {	/* configured bound */
			h->osc.len = 0;
			h->osc_esc = 0;
			h->in_osc = 0;
			*used = i + 1;
			return (1);
		}
	}
	*used = n;
	return (0);
}

static int hold_ender(struct dv_host *h, unsigned char c);

static void host_hold_more(struct dv_host *h, const unsigned char *b, size_t n);

/* A CSI, DCS or APC that no longer fits in buf. A limit of 0 keeps it; a set
 * limit discards the sequence through its final byte. */
static void
host_begin_hold(struct dv_host *h, const unsigned char *b, size_t n)
{
	unsigned char	kind = h->buf[1];
	size_t		limit = kind == '[' ? dv_csi_limit : dv_escape_limit;

	h->hold_csi = kind == '[';
	h->hold_esc = 0;
	h->holding = 1;
	h->hold_drop = 0;
	if (limit && h->len >= limit) {
		h->hold_drop = 1;
		h->len = 0;
		host_hold_more(h, b, n);
		return;
	}
	dv_buf_append(&h->hold, h->buf, h->len);
	h->len = 0;
	host_hold_more(h, b, n);
}

static int
hold_ender(struct dv_host *h, unsigned char c)
{
	int	end;

	if (h->hold_csi)
		return (c >= 0x40 && c <= 0x7e);
	end = c == 0x07 || (h->hold_esc && c == '\\');
	h->hold_esc = c == 0x1b;
	return (end);
}

static void
host_hold_more(struct dv_host *h, const unsigned char *b, size_t n)
{
	size_t	limit = h->hold_csi ? dv_csi_limit : dv_escape_limit;
	size_t	i;

	if (!h->hold_drop && limit && h->hold.len + n > limit) {
		dv_buf_free(&h->hold);
		h->hold_drop = 1;
		h->hold_esc = 0;
	}
	if (h->hold_drop) {
		for (i = 0; i < n; i++) {
			if (!hold_ender(h, b[i]))
				continue;
			h->holding = 0;
			h->hold_drop = 0;
			h->hold_esc = 0;
			if (i + 1 < n)
				dv_host_feed(h, b + i + 1, n - (i + 1));
			return;
		}
		return;
	}
	dv_buf_append(&h->hold, b, n);
}

void
dv_host_feed(struct dv_host *h, const void *p, size_t n)
{
	const unsigned char	*b = static_cast<const unsigned char *>(p);

	if (n == 0)
		return;
	if (h->holding) {
		host_hold_more(h, b, n);
		return;
	}
	if (h->in_osc) {
		size_t	used = 0;

		if (!osc_collect(h, b, n, &used))
			return;
		b += used;
		n -= used;
		if (n == 0)
			return;
	}
	if (h->len >= 2 && h->buf[0] == 0x1b &&
	    (h->buf[1] == '[' || h->buf[1] == 'P' || h->buf[1] == '_') &&
	    h->len + n > sizeof h->buf) {
		host_begin_hold(h, b, n);
		return;
	}
	if (h->len + n > sizeof h->buf) {
		if (n >= sizeof h->buf) {
			memcpy(h->buf, b + (n - sizeof h->buf), sizeof h->buf);
			h->len = sizeof h->buf;
			return;
		}
		host_consume(h, h->len + n - sizeof h->buf);
	}
	memcpy(h->buf + h->len, b, n);
	h->len += n;
}

/* Which keys open the menu: the Alt tap, Ctrl-\, both (default) or none. */
static int	menu_keys = DV_MENU_KEY_ALT | DV_MENU_KEY_CTRL_BACKSLASH;

void
dv_host_set_menu_keys(int mask)
{
	menu_keys = mask;
}

static int
is_mod_key(unsigned code)
{
	return (code >= KITTY_FIRST_MOD && code <= KITTY_LAST_MOD);
}

static int
is_alt_key(unsigned code)
{
	return (code == KITTY_LEFT_ALT || code == KITTY_RIGHT_ALT);
}

static size_t
utf8_encode(unsigned cp, unsigned char *out)
{
	if (cp < 0x80) {
		out[0] = (unsigned char)cp;
		return (1);
	}
	if (cp < 0x800) {
		out[0] = (unsigned char)(0xc0 | (cp >> 6));
		out[1] = (unsigned char)(0x80 | (cp & 0x3f));
		return (2);
	}
	if (cp < 0x10000) {
		out[0] = (unsigned char)(0xe0 | (cp >> 12));
		out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (unsigned char)(0x80 | (cp & 0x3f));
		return (3);
	}
	out[0] = (unsigned char)(0xf0 | (cp >> 18));
	out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (unsigned char)(0x80 | (cp & 0x3f));
	return (4);
}

/* Legacy control byte for Ctrl+key, or -1 if there is none. */
static int
ctrl_byte(unsigned code)
{
	if (code >= 'a' && code <= 'z')
		return ((int)(code - 'a' + 1));
	if (code >= 'A' && code <= 'Z')
		return ((int)(code - 'A' + 1));
	switch (code) {
	case ' ':
	case '@':
	case '2':
		return (0);
	case '[':
	case '3':
		return (0x1b);
	case '\\':
	case '4':
		return (0x1c);
	case ']':
	case '5':
		return (0x1d);
	case '^':
	case '6':
		return (0x1e);
	case '_':
	case '-':
	case '/':
	case '7':
		return (0x1f);
	case '?':
	case '8':
		return (0x7f);
	default:
		return (-1);
	}
}

/*
 * Kitty's private-use codes for the keypad, mapped to the ordinary key
 * they stand for. Everything else above 57344 (media keys, lock keys)
 * has no legacy byte and is dropped.
 */
static int
keypad_map(unsigned code, unsigned *out_code, int *out_final)
{
	if (code >= 57399 && code <= 57408) {
		*out_code = '0' + (code - 57399);
		*out_final = 'u';
		return (1);
	}
	switch (code) {
	case 57409: *out_code = '.'; *out_final = 'u'; return (1);
	case 57410: *out_code = '/'; *out_final = 'u'; return (1);
	case 57411: *out_code = '*'; *out_final = 'u'; return (1);
	case 57412: *out_code = '-'; *out_final = 'u'; return (1);
	case 57413: *out_code = '+'; *out_final = 'u'; return (1);
	case 57414: *out_code = 13;  *out_final = 'u'; return (1);
	case 57415: *out_code = '='; *out_final = 'u'; return (1);
	case 57417: *out_code = 1; *out_final = 'D'; return (1);
	case 57418: *out_code = 1; *out_final = 'C'; return (1);
	case 57419: *out_code = 1; *out_final = 'A'; return (1);
	case 57420: *out_code = 1; *out_final = 'B'; return (1);
	case 57421: *out_code = 5; *out_final = '~'; return (1);
	case 57422: *out_code = 6; *out_final = '~'; return (1);
	case 57423: *out_code = 1; *out_final = 'H'; return (1);
	case 57424: *out_code = 1; *out_final = 'F'; return (1);
	case 57425: *out_code = 2; *out_final = '~'; return (1);
	case 57426: *out_code = 3; *out_final = '~'; return (1);
	case 57427: *out_code = 1; *out_final = 'E'; return (1);
	default:
		return (0);
	}
}

static size_t
key_to_pty(unsigned code, unsigned shifted, unsigned mods, unsigned text,
    int final, unsigned char *out)
{
	unsigned	m = (mods & 7u) + 1;	/* xterm modifier parameter */
	size_t		n = 0;
	int		c, rc;

	if (final == 'u' && code >= KITTY_FUNCTIONAL) {
		unsigned	mc;
		int		mf;

		if (is_mod_key(code) || !keypad_map(code, &mc, &mf))
			return (0);
		code = mc;
		final = mf;
	}

	switch (final) {
	case 'A': case 'B': case 'C': case 'D':
	case 'H': case 'F': case 'E':
		if (m > 1)
			rc = snprintf((char *)out, 16, "\033[1;%u%c", m, final);
		else
			rc = snprintf((char *)out, 16, "\033[%c", final);
		return (rc > 0 ? (size_t)rc : 0);
	case 'P': case 'Q': case 'S':
		if (m > 1)
			rc = snprintf((char *)out, 16, "\033[1;%u%c", m, final);
		else
			rc = snprintf((char *)out, 16, "\033O%c", final);
		return (rc > 0 ? (size_t)rc : 0);
	case 'Z':
		memcpy(out, "\033[Z", 3);
		return (3);
	case '~':
		if (m > 1)
			rc = snprintf((char *)out, 16, "\033[%u;%u~", code, m);
		else
			rc = snprintf((char *)out, 16, "\033[%u~", code);
		return (rc > 0 ? (size_t)rc : 0);
	default:
		break;
	}

	if (mods & MOD_ALT)
		out[n++] = 0x1b;

	switch (code) {
	case 13:
		out[n++] = '\r';
		return (n);
	case 9:
		if (mods & MOD_SHIFT) {
			memcpy(out, "\033[Z", 3);
			return (3);
		}
		out[n++] = '\t';
		return (n);
	case 127:
		out[n++] = (mods & MOD_CTRL) ? 0x08 : 0x7f;
		return (n);
	case 27:
		out[n++] = 0x1b;
		return (n);
	default:
		break;
	}

	if (mods & MOD_CTRL) {
		c = ctrl_byte(code);
		if (c >= 0) {
			out[n++] = (unsigned char)c;
			return (n);
		}
	}
	if (text != 0)
		return (n + utf8_encode(text, out + n));
	if (mods & MOD_SUPER)
		return (0);	/* Cmd+key with no text is the host's business */
	if (mods & MOD_SHIFT) {
		if (shifted != 0)
			code = shifted;
		else if (code >= 'a' && code <= 'z')
			code -= 32;
	}
	if (code > 0 && code < 0x110000)
		return (n + utf8_encode(code, out + n));
	return (0);
}

/*
 * CSI parameters: fields split by ';', each with sub-fields split by
 * ':'. The kitty key report is `code:shifted:base ; mods:event ; text`.
 */
struct fields {
	unsigned	v[8][4];
	int		nsub[8];
	int		nf;
};

static int
parse_fields(const unsigned char *s, const unsigned char *end,
    struct fields *f)
{
	int		 fi = 0, si = 0, have = 0, any = 0;
	unsigned	 v = 0;

	memset(f, 0, sizeof *f);
	while (s < end) {
		unsigned char	c = *s++;

		if (c >= '0' && c <= '9') {
			v = v * 10 + (c - '0');
			if (v > 0x10ffff)
				v = 0x10ffff;
			have = 1;
			any = 1;
			continue;
		}
		if (c == ';' || c == ':') {
			any = 1;
			if (fi < 8 && si < 4) {
				f->v[fi][si] = have ? v : 0;
				f->nsub[fi] = si + 1;
			}
			v = 0;
			have = 0;
			if (c == ':')
				si++;
			else {
				fi++;
				si = 0;
			}
			continue;
		}
		return (0);
	}
	if (!any)
		return (1);
	if (fi < 8 && si < 4) {
		f->v[fi][si] = have ? v : 0;
		f->nsub[fi] = si + 1;
	}
	f->nf = fi < 8 ? fi + 1 : 8;
	return (1);
}

/*
 * A key from the host. Besides the legacy bytes, the event carries the
 * key itself (code, alternates, modifiers, press/repeat/release, text):
 * a window that pushed kitty keyboard flags is encoded from that by the
 * server. Releases and modifier keys travel too, with no bytes, since a
 * program may have asked for them.
 */
static int
handle_key(struct dv_host *h, unsigned code, unsigned shifted, unsigned base,
    unsigned mods, int action, const unsigned *text, int ntext, int final,
    struct dv_host_ev *ev)
{
	struct dv_key	*k = &ev->key;
	int		 i;

	h->kbd = 1;
	memset(k, 0, sizeof *k);
	k->code = code;
	k->shifted = shifted;
	k->base = base;
	k->mods = mods;
	k->type = action;
	k->final = final;
	for (i = 0; i < ntext && i < 4; i++)
		k->text[k->ntext++] = text[i];
	ev->has_key = 1;
	ev->type = DV_HEV_PTY;
	ev->n = 0;

	if (action == 3) {	/* release */
		if (is_alt_key(code)) {
			int	tap = h->alt_down && !h->alt_chord;

			h->alt_down = 0;
			h->alt_chord = 0;
			if (tap && (menu_keys & DV_MENU_KEY_ALT)) {
				ev->has_key = 0;
				ev->type = DV_HEV_MENU;
				return (1);
			}
		}
		return (1);	/* bytes-free: only a kitty-keyboard window sees it */
	}
	if (is_alt_key(code)) {
		h->alt_down = 1;
		h->alt_chord = 0;
		return (1);
	}
	if (is_mod_key(code))
		return (1);
	if (h->alt_down)
		h->alt_chord = 1;

	/* Ctrl-\ is the fallback menu key, including under CSI u. */
	if (code == '\\' && (mods & MOD_CTRL) &&
	    (menu_keys & DV_MENU_KEY_CTRL_BACKSLASH)) {
		ev->has_key = 0;
		ev->type = DV_HEV_MENU;
		return (1);
	}

	ev->n = key_to_pty(code, shifted, mods, ntext > 0 ? text[0] : 0, final,
	    ev->data);
	memcpy(k->legacy, ev->data, ev->n);
	k->nlegacy = ev->n;
	return (1);
}

static unsigned char *
host_view(struct dv_host *h, size_t *len)
{
	if (h->holding && !h->hold_drop && h->hold.data != nullptr) {
		*len = h->hold.len;
		return (h->hold.data);
	}
	*len = h->len;
	return (h->buf);
}

static void
host_take(struct dv_host *h, size_t n)
{
	if (h->holding) {
		dv_buf_consume(&h->hold, n);
		if (h->hold.len == 0)
			h->holding = 0;
		return;
	}
	host_consume(h, n);
}

/*
 * Parse one CSI / APC / run of bytes from h->buf. Returns 1 and fills
 * ev, 0 if more input is needed, -1 if bytes were swallowed with no
 * event.
 */
static int
parse_one(struct dv_host *h, struct dv_host_ev *ev)
{
	size_t		 len;
	unsigned char	*b;

	if (h->holding && h->hold_drop)
		return (0);
	b = host_view(h, &len);
	ev->has_key = 0;
	size_t		 i, end;
	struct fields	 f;
	unsigned	 p[8];
	int		 np, final, k;

	memset(ev, 0, sizeof *ev);
	if (h->osc_ready) {		/* a complete host OSC reply */
		h->osc_ready = 0;
		ev->type = DV_HEV_OSC;
		ev->n = h->osc.len;
		return (1);
	}
	if (len == 0)
		return (0);

	if (b[0] == 0x1c && (menu_keys & DV_MENU_KEY_CTRL_BACKSLASH)) {
		host_take(h, 1);	/* C-\ fallback menu key */
		ev->type = DV_HEV_MENU;
		return (1);
	}

	if (b[0] != 0x1b) {
		/* A run of plain bytes (typing, or a paste) in one event. */
		for (i = 0; i < len && i < sizeof ev->data; i++) {
			if (b[i] == 0x1b ||
			    (b[i] == 0x1c && (menu_keys & DV_MENU_KEY_CTRL_BACKSLASH)))
				break;
		}
		ev->type = DV_HEV_PTY;
		memcpy(ev->data, b, i);
		ev->n = i;
		host_take(h, i);
		return (1);
	}
	if (len < 2)
		return (0);

	/*
	 * DCS: ESC P ... ESC \. XTVERSION answers ESC P > | name ESC \,
	 * which names the terminal we are attached to; other DCS replies are
	 * consumed. A bare ESC P from a legacy host is Alt+P: what follows
	 * tells them apart, since a reply starts with > or a digit.
	 */
	if (b[1] == 'P') {
		if (len < 3)
			return (0);
		if (b[2] == '>' || b[2] == '$' || b[2] == '+' || b[2] == '!' ||
		    (b[2] >= '0' && b[2] <= '9')) {
			for (i = 3; i < len; i++) {
				if (!(b[i - 1] == 0x1b && b[i] == '\\'))
					continue;
				if (b[2] == '>' && i >= 5 && b[3] == '|') {
					size_t	nl = i - 1 - 4;

					if (nl >= sizeof h->name)
						nl = sizeof h->name - 1;
					memcpy(h->name, b + 4, nl);
					h->name[nl] = '\0';
				}
				host_take(h, i + 1);
				return (-1);
			}
			if (dv_escape_limit ? len > dv_escape_limit : (!h->holding && len + 1 >= sizeof h->buf)) {
				host_take(h, 1);
				return (-1);
			}
			return (0);
		}
	}

	/* APC: ESC _ G ... ESC \ */
	if (b[1] == '_') {
		if (len < 3)
			return (0);
		if (b[2] != 'G') {
			host_take(h, 1);
			return (-1);
		}
		for (i = 3; i < len; i++) {
			int	term = 0;

			if (b[i] == 0x07)
				term = 1;
			else if (b[i - 1] == 0x1b && b[i] == '\\')
				term = 1;
			if (!term)
				continue;
			if (memmem(b, i + 1, ";OK", 3) != NULL)
				h->gfx = 1;
			host_take(h, i + 1);
			return (-1);
		}
		if (dv_escape_limit ? len > dv_escape_limit : (!h->holding && len + 1 >= sizeof h->buf)) {
			host_take(h, 1);
			return (-1);
		}
		return (0);
	}

	/*
	 * OSC reply: ESC ] <digit> ... (BEL | ESC \). Only a digit makes it
	 * an OSC; a bare ESC ] is Alt+] from a legacy host.
	 */
	if (b[1] == ']') {
		if (len < 3)
			return (0);
		if (b[2] >= '0' && b[2] <= '9') {
			size_t	used = 0;
			int	done;

			h->osc.len = 0;
			h->osc_esc = 0;
			h->in_osc = 1;
			done = osc_collect(h, b, len, &used);
			host_take(h, used);
			if (done && h->osc_ready) {
				h->osc_ready = 0;
				ev->type = DV_HEV_OSC;
				ev->n = h->osc.len;
				return (1);
			}
			return (len > 0 ? -1 : 0);
		}
	}

	if (b[1] != '[') {
		/* ESC + something else (Alt+key from a legacy host). */
		ev->type = DV_HEV_PTY;
		ev->data[0] = 0x1b;
		ev->data[1] = b[1];
		ev->n = 2;
		host_take(h, 2);
		return (1);
	}

	/* CSI: ESC [ ... final */
	end = 0;
	for (i = 2; i < len; i++) {
		if (b[i] >= 0x40 && b[i] <= 0x7e) {
			end = i;
			break;
		}
	}
	if (end == 0) {
		if (dv_csi_limit ? len > dv_csi_limit : (!h->holding && len + 1 >= sizeof h->buf)) {
			host_take(h, 1);
			return (-1);
		}
		return (0);
	}
	final = b[end];

	/* SGR mouse: ESC [ < code ; x ; y (M|m). */
	if (b[2] == '<' && (final == 'M' || final == 'm')) {
		unsigned	m[3] = {0, 0, 0};
		int		mi = 0;
		size_t		j;

		for (j = 3; j < end && mi < 3; j++) {
			if (b[j] >= '0' && b[j] <= '9')
				m[mi] = m[mi] * 10 + (b[j] - '0');
			else if (b[j] == ';')
				mi++;
			else
				break;
		}
		ev->type = DV_HEV_MOUSE;
		ev->mcode = m[0];
		ev->mx = m[1];
		ev->my = m[2];
		ev->mpress = (final == 'M') ? DV_MOUSE_PRESS : DV_MOUSE_RELEASE;
		host_take(h, end + 1);
		return (1);
	}

	/* Replies with a private prefix (CSI ? flags u, DECRPM): not keys. */
	if (b[2] == '?' || b[2] == '>' || b[2] == '<' || b[2] == '=') {
		if (final == 'u' && b[2] == '?')
			h->kbd = 1;
		if (final == 'q' && b[2] == '>')	/* CSI > 1;2;... SP q */
			h->mcursors = 1;
		if (final == 'c' && b[2] == '?') {
			size_t	j;
			int	v = -1;

			/* DA1 (CSI ? ... c): the bootstrap replies are in. A
			 * parameter of 4 means the host renders sixel. */
			h->da = 1;
			for (j = 3; j <= end; j++) {
				if (b[j] >= '0' && b[j] <= '9')
					v = (v < 0 ? 0 : v * 10) + (b[j] - '0');
				else {
					if (v == 4)
						h->sixel = 1;
					v = -1;
				}
			}
		}
		host_take(h, end + 1);
		return (-1);
	}

	if (!parse_fields(b + 2, b + end, &f)) {
		host_take(h, end + 1);
		return (-1);
	}
	np = f.nf;
	for (k = 0; k < 8; k++)
		p[k] = k < np ? f.v[k][0] : 0;

	/*
	 * Bracketed-paste markers (CSI 200 ~ / CSI 201 ~) are not keys: pass
	 * them through verbatim as terminal bytes. The client enables DECSET
	 * 2004 on the host, so a drag-and-drop or Ctrl-V arrives wrapped in
	 * these; the server routes the paste between them to the focused
	 * window in that window's own paste mode. The pasted content itself
	 * is ordinary bytes handled by the plain-run path.
	 */
	if (final == '~' && (p[0] == 200 || p[0] == 201)) {
		size_t	m = end + 1;

		if (m > sizeof ev->data)
			m = sizeof ev->data;
		memcpy(ev->data, b, m);
		ev->n = m;
		ev->type = DV_HEV_PTY;
		host_take(h, end + 1);
		return (1);
	}

	/* Kitty CSI u / functional CSI ... [A-Z~] keys */
	if (final == 'u' || final == '~' ||
	    (final >= 'A' && final <= 'Z')) {
		unsigned	code, shifted = 0, base = 0, mods = 0, text[4];
		int		action = 1, ntext = 0, ti;

		if (final == 'u' || final == '~') {
			code = np > 0 ? f.v[0][0] : 0;
			if (f.nsub[0] > 1)
				shifted = f.v[0][1];
			if (f.nsub[0] > 2)
				base = f.v[0][2];
		} else
			code = 1;
		if (np > 1) {
			if (f.v[1][0] > 0)
				mods = f.v[1][0] - 1;
			if (f.nsub[1] > 1 && f.v[1][1] > 0)
				action = (int)f.v[1][1];
		}
		if (np > 2)
			for (ti = 0; ti < f.nsub[2] && ti < 4; ti++)
				if (f.v[2][ti] != 0)
					text[ntext++] = f.v[2][ti];
		host_take(h, end + 1);
		if (final != 'u' && final != '~' && final != 'Z' &&
		    strchr("ABCDEFHPQS", final) == NULL)
			return (-1);	/* focus in/out, CPR, ... */
		if (handle_key(h, code, shifted, base, mods, action, text, ntext,
		    final, ev))
			return (1);
		return (-1);
	}

	/* XTWINOPS / mode 2048 size reports, final 't'. */
	if (final == 't' && np >= 1) {
		struct dv_size	sz;

		memset(&sz, 0, sizeof sz);
		if (p[0] == 6 && np >= 3) {		/* cell pixels */
			h->ypixel = p[1];
			h->xpixel = p[2];
			h->have_cell = 1;
			sz.xpixel = h->xpixel;
			sz.ypixel = h->ypixel;
			sz.cols = h->cols;
			sz.rows = h->rows;
			ev->type = DV_HEV_SIZE;
			ev->size = sz;
			host_take(h, end + 1);
			return (1);
		}
		if (p[0] == 8 && np >= 3) {		/* cells */
			h->rows = p[1];
			h->cols = p[2];
			sz.cols = h->cols;
			sz.rows = h->rows;
			sz.xpixel = h->xpixel;
			sz.ypixel = h->ypixel;
			ev->type = DV_HEV_SIZE;
			ev->size = sz;
			host_take(h, end + 1);
			return (1);
		}
		if (p[0] == 4 && np >= 3 && h->cols && h->rows &&
		    !h->have_cell) {
			/* window pixels → estimate until CSI 16 lands */
			if (p[2] && h->cols)
				h->xpixel = p[2] / h->cols;
			if (p[1] && h->rows)
				h->ypixel = p[1] / h->rows;
			sz.cols = h->cols;
			sz.rows = h->rows;
			sz.xpixel = h->xpixel;
			sz.ypixel = h->ypixel;
			ev->type = DV_HEV_SIZE;
			ev->size = sz;
			host_take(h, end + 1);
			return (1);
		}
		if (p[0] == 48 && np >= 5) {		/* mode 2048 */
			h->rows = p[1];
			h->cols = p[2];
			if (h->cols && p[4])
				h->xpixel = p[4] / h->cols;
			if (h->rows && p[3])
				h->ypixel = p[3] / h->rows;
			h->have_cell = 1;
			sz.cols = h->cols;
			sz.rows = h->rows;
			sz.xpixel = h->xpixel;
			sz.ypixel = h->ypixel;
			ev->type = DV_HEV_SIZE;
			ev->size = sz;
			host_take(h, end + 1);
			return (1);
		}
		host_take(h, end + 1);
		return (-1);
	}

	host_take(h, end + 1);
	return (-1);
}

/*
 * A lone ESC is indistinguishable from the start of a CSI until the
 * next byte arrives, so parse_one holds it. Without a timer, Esc never
 * reaches the inner window and never cancels a latched menu. The client
 * polls with DV_ESC_TIMEOUT when this says bytes are waiting.
 *
 * A kitty graphics reply (ESC _ G ...) legitimately arrives in pieces
 * and is not a keystroke: leave it to accumulate.
 */
int
dv_host_pending(const struct dv_host *h)
{
	if (h->npend > 0 || h->in_osc || h->osc_ready)
		return (0);
	if (h->len == 0)
		return (0);
	if (h->len >= 2 && h->buf[0] == 0x1b && h->buf[1] == '_')
		return (0);
	return (1);
}

int
dv_host_expire(struct dv_host *h, struct dv_host_ev *ev)
{
	size_t	n;

	memset(ev, 0, sizeof *ev);
	if (!dv_host_pending(h))
		return (0);
	n = h->len;
	if (n > sizeof ev->data)
		n = sizeof ev->data;
	memcpy(ev->data, h->buf, n);
	ev->n = n;
	ev->type = DV_HEV_PTY;
	host_consume(h, n);
	return (1);
}

int
dv_host_next(struct dv_host *h, struct dv_host_ev *ev)
{
	int	r, n;

	if (h->npend > 0) {
		*ev = h->pend[0];
		h->npend--;
		memmove(&h->pend[0], &h->pend[1], h->npend * sizeof h->pend[0]);
		return (1);
	}
	for (n = 0; n < 4096; n++) {
		r = parse_one(h, ev);
		if (r == 0)
			return (0);
		if (r > 0)
			return (1);
	}
	return (0);
}

/* Does a terminal's XTVERSION name say it is `what`? ("kitty(0.35.2)",
 * "ghostty 1.1.3", "goblin-mosh 0.4": a case-insensitive substring.) */
int
dv_term_is(const char *name, const char *what)
{
	size_t	n = strlen(name), m = strlen(what), i;

	if (m == 0)
		return (0);
	for (i = 0; i + m <= n; i++)
		if (strncasecmp(name + i, what, m) == 0)
			return (1);
	return (0);
}

int
dv_host_features(const struct dv_host *h)
{
	int	f = DV_IDENT_TTY | DV_IDENT_GHOSTTY;

	if (h->gfx)
		f |= DV_IDENT_KITTY_GFX;
	if (h->sixel)
		f |= DV_IDENT_SIXEL;
	if (h->have_cell)
		f |= DV_IDENT_CELLSIZE;
	if (h->dnd)
		f |= DV_IDENT_DND;
	if (h->mcursors)
		f |= DV_IDENT_MCURSORS;
	return (f);
}
