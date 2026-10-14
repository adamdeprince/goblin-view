/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Multiplexer side of the Silkie OSC 777 extensions: caps, tablet
 * pointer, scroll, gesture, haptic, ink, frame, latency and display.
 * Written from the published specifications. The inner program never
 * talks to the host; this process answers queries with what it can
 * carry and what an attached display listed, and it translates
 * coordinates into the pane.
 */

#include "silkie.hpp"
#include "pointer_coordinates.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace gp = goblin::pointer;

/* Query ids, after goblin-pointer's 17349522. One per selector. */
enum {
	Q_BASE = 17349523,
	Q_CAPS = Q_BASE,
	Q_DISPLAY,
	Q_FRAME,
	Q_GESTURE,
	Q_HAPTIC,
	Q_INK,
	Q_LATENCY,
	Q_SCROLL,
	Q_TABLET
};

static const char *const k_selectors[] = {
	"caps", "display", "frame", "gesture", "haptic", "ink",
	"latency", "scroll", "tablet-pointer", nullptr
};

const char *
dv_silkie_probe(void)
{
	return (
	    "\033]777;caps;v=1:op=query:id=17349523\033\\"
	    "\033]777;display;v=1:op=query:id=17349524\033\\"
	    "\033]777;frame;v=1:op=query:id=17349525\033\\"
	    "\033]777;gesture;v=1:op=query:id=17349526\033\\"
	    "\033]777;haptic;v=1:op=query:id=17349527\033\\"
	    "\033]777;ink;v=1:op=query:id=17349528\033\\"
	    "\033]777;latency;v=1:op=query:id=17349529\033\\"
	    "\033]777;scroll;v=1:op=query:id=17349530\033\\"
	    "\033]777;tablet-pointer;v=1:op=query:id=17349531\033\\");
}

__attribute__((weak)) void
dv_host_passthrough(struct dv_session *, const char *, size_t)
{
}

__attribute__((weak)) void
dv_mouse_inject(struct dv_peer *, unsigned, unsigned, unsigned, int)
{
}

__attribute__((weak)) void
dv_silkie_peers_changed(struct dv_session *)
{
}

/* ---- field frames -------------------------------------------------- */

struct silk_f {
	char	 buf[2048];
	int	 n;
	char	*key[48];
	char	*val[48];
};

static int
parse_i64(const char *val, long long min, long long max, long long *ans)
{
	int		 neg;
	const char	*p;
	long long	 v;

	if (val == nullptr || val[0] == '\0')
		return (0);
	neg = val[0] == '-';
	p = val + neg;
	if (*p < '0' || *p > '9')
		return (0);
	v = 0;
	for (; *p >= '0' && *p <= '9'; p++) {
		if (v > 1000000000000000ll)
			return (0);
		v = v * 10 + (*p - '0');
	}
	if (*p != '\0')
		return (0);
	if (neg)
		v = -v;
	if (v < min || v > max)
		return (0);
	*ans = v;
	return (1);
}

static const char *
ffield(const struct silk_f *f, const char *key)
{
	int	i;

	for (i = 0; i < f->n; i++)
		if (strcmp(f->key[i], key) == 0)
			return (f->val[i]);
	return (nullptr);
}

/* Absent leaves *ans alone and succeeds. A bad number rejects the frame. */
static int
ffield_int(const struct silk_f *f, const char *key, long long min,
    long long max, long long *ans)
{
	const char	*v = ffield(f, key);

	if (v == nullptr)
		return (1);
	return (parse_i64(v, min, max, ans));
}

static int
parse_fields(struct silk_f *f, const char *s, size_t n, int from_program)
{
	char	*p, *end, *eq;
	size_t	 lim = from_program ? 128 : sizeof f->buf;

	if (n >= lim || n >= sizeof f->buf)
		return (0);
	memcpy(f->buf, s, n);
	f->buf[n] = '\0';
	f->n = 0;
	p = f->buf;
	if (p[0] == '\0')
		return (0);
	for (;;) {
		end = strchr(p, ':');
		if (end != nullptr)
			*end = '\0';
		eq = strchr(p, '=');
		if (eq == nullptr || eq == p)
			return (0);
		*eq = '\0';
		for (int i = 0; i < f->n; i++)
			if (strcmp(f->key[i], p) == 0)
				return (0);
		if (f->n >= 48)
			return (0);
		f->key[f->n] = p;
		f->val[f->n] = eq + 1;
		f->n++;
		if (end == nullptr)
			break;
		p = end + 1;
	}
	return (1);
}

static int
known_sel(const char *sel)
{
	for (int i = 0; k_selectors[i] != nullptr; i++)
		if (strcmp(k_selectors[i], sel) == 0)
			return (1);
	return (0);
}

/* "name.1" as a comma-separated token. */
static int
ext_has(const char *ext, const char *sel)
{
	size_t		nl;
	const char	*p;

	if (ext == nullptr || sel == nullptr)
		return (0);
	nl = strlen(sel);
	for (p = ext; (p = strstr(p, sel)) != nullptr; p += nl) {
		if (p != ext && p[-1] != ',')
			continue;
		if (p[nl] == '.' || p[nl] == '\0' || p[nl] == ',')
			return (1);
	}
	return (0);
}

static int
offered(const struct dv_session *s, const char *sel)
{
	return (s != nullptr && s->silk_any && ext_has(s->silk_ext, sel));
}

/* OSC text in, selector and fields out. 1 known selector, 0 not ours,
 * -1 a known selector whose frame is rejected. */
static int
open_osc(const char *s, size_t n, int from_program, char *sel, size_t selcap,
    struct silk_f *f)
{
	const char	*b;
	size_t		 bn, i;

	if (n >= 2 && (unsigned char)s[0] == 0x1b && s[1] == ']') {
		s += 2;
		n -= 2;
	}
	while (n > 0 && s[n - 1] == '\0')
		n--;
	if (n >= 2 && s[n - 1] == '\\' && (unsigned char)s[n - 2] == 0x1b)
		n -= 2;
	else if (n >= 1 && s[n - 1] == 0x07)
		n--;
	if (n < 4 || memcmp(s, "777;", 4) != 0)
		return (0);
	b = s + 4;
	bn = n - 4;
	for (i = 0; i < bn && b[i] != ';'; i++)
		;
	if (i == 0 || i + 1 >= bn || i >= selcap)
		return (0);
	memcpy(sel, b, i);
	sel[i] = '\0';
	if (!known_sel(sel))
		return (0);
	if (!parse_fields(f, b + i + 1, bn - i - 1, from_program))
		return (-1);
	{
		long long	ver = 0;
		const char	*op;

		if (!parse_i64(ffield(f, "v"), 0, 2147483647ll, &ver) || ver != 1)
			return (-1);
		op = ffield(f, "op");
		if (op == nullptr || op[0] == '\0')
			return (-1);
		if (!ffield_int(f, "id", 0, 2147483647ll, &ver))
			return (-1);
	}
	return (1);
}

static void
pty_fields(struct dv_window *w, const char *sel, const char *fields)
{
	char	buf[2048];
	int	n;

	if (w == nullptr || w->pty_fd < 0)
		return;
	n = snprintf(buf, sizeof buf, "\033]777;%s;%s\033\\", sel, fields);
	if (n > 0 && (size_t)n < sizeof buf)
		dv_window_pty_send(w, buf, (size_t)n);
}

static void
host_fields(struct dv_session *s, const char *sel, const char *fields)
{
	char	buf[2048];
	int	n;

	if (s == nullptr)
		return;
	n = snprintf(buf, sizeof buf, "\033]777;%s;%s\033\\", sel, fields);
	if (n > 0 && (size_t)n < sizeof buf)
		dv_host_passthrough(s, buf, (size_t)n);
}

/* ---- capability blob ---------------------------------------------- */

static int
blob_get(const char *blob, const char *key, char *out, size_t cap)
{
	size_t		kl = strlen(key);
	const char	*p = blob ? blob : "";

	out[0] = '\0';
	while (*p != '\0') {
		const char	*nl = strchr(p, '\n');
		size_t		 n = nl != nullptr ? (size_t)(nl - p) : strlen(p);

		if (n > kl + 1 && strncmp(p, key, kl) == 0 && p[kl] == '=') {
			size_t	vl = n - kl - 1;

			if (vl >= cap)
				vl = cap - 1;
			memcpy(out, p + kl + 1, vl);
			out[vl] = '\0';
			return (1);
		}
		if (nl == nullptr)
			break;
		p = nl + 1;
	}
	return (0);
}

static void
blob_set(char *blob, size_t cap, const char *key, const char *val)
{
	char		 next[2048];
	size_t		 kl = strlen(key), used = 0;
	const char	*p = blob;

	next[0] = '\0';
	while (*p != '\0' && used + 1 < sizeof next) {
		const char	*nl = strchr(p, '\n');
		size_t		 n = nl != nullptr ? (size_t)(nl - p) : strlen(p);
		int		 drop = n > kl && strncmp(p, key, kl) == 0 &&
		    p[kl] == '=';

		if (!drop && used + n + 1 < sizeof next) {
			memcpy(next + used, p, n);
			used += n;
			next[used++] = '\n';
			next[used] = '\0';
		}
		if (nl == nullptr)
			break;
		p = nl + 1;
	}
	if (val != nullptr && val[0] != '\0' &&
	    used + kl + 1 + strlen(val) + 1 < sizeof next) {
		used += (size_t)snprintf(next + used, sizeof next - used,
		    "%s=%s\n", key, val);
	}
	if (used >= cap)
		used = cap - 1;
	memcpy(blob, next, used);
	blob[used] = '\0';
}

static void
ext_add(char *ext, size_t cap, const char *sel)
{
	char	tok[64];

	if (ext_has(ext, sel))
		return;
	snprintf(tok, sizeof tok, "%s.1", sel);
	if (ext[0] == '\0')
		snprintf(ext, cap, "%s", tok);
	else {
		size_t	n = strlen(ext);

		if (n + 1 + strlen(tok) + 1 < cap) {
			ext[n] = ',';
			memcpy(ext + n + 1, tok, strlen(tok) + 1);
		}
	}
}

static int
csv_has(const char *list, const char *tok)
{
	size_t		nl = strlen(tok);
	const char	*p;

	if (list == nullptr)
		return (0);
	for (p = list; (p = strstr(p, tok)) != nullptr; p += nl) {
		if ((p == list || p[-1] == ',') &&
		    (p[nl] == '\0' || p[nl] == ','))
			return (1);
	}
	return (0);
}

static void
csv_intersect(char *acc, size_t cap, const char *list)
{
	char	out[80], tok[16];
	size_t	i = 0, o = 0;

	out[0] = '\0';
	while (acc[i] != '\0') {
		size_t	k = 0;

		while (acc[i] == ',')
			i++;
		while (acc[i] != '\0' && acc[i] != ',' && k + 1 < sizeof tok)
			tok[k++] = acc[i++];
		tok[k] = '\0';
		if (k == 0)
			break;
		if (csv_has(list, tok)) {
			if (o != 0 && o + 1 < sizeof out)
				out[o++] = ',';
			if (o + k < sizeof out) {
				memcpy(out + o, tok, k);
				o += k;
			}
			out[o] = '\0';
		}
	}
	snprintf(acc, cap, "%s", out);
}

static int
peer_gfx(const struct dv_peer *p, const char *blob)
{
	char	tmp[80];

	if (p->features & DV_IDENT_KITTY_GFX)
		return (1);
	if (blob_get(blob, "format", tmp, sizeof tmp))
		return (1);
	if (blob_get(blob, "media", tmp, sizeof tmp))
		return (1);
	return (0);
}

void
dv_silkie_recompute(struct dv_session *s, struct dv_peer *first)
{
	struct dv_peer	*p;
	char		 ext[768];
	char		 format[80], media[80], display[384];
	int		 have_fmt = 0, saw_gfx = 0, wg = 1;
	int		 frame_caps = 0, frame_period = 0, gesture = 0;
	int		 haptic = 0, ink = 0, lat_caps = 0, lat_in = 0, lat_re = 0;
	int		 scroll = 0, tablet = 0;
	char		 prev_disp[384];

	if (s == nullptr)
		return;
	snprintf(prev_disp, sizeof prev_disp, "%s", s->silk_display);
	ext[0] = format[0] = media[0] = display[0] = '\0';
	for (p = first; p != nullptr; p = TAILQ_NEXT(p, entry)) {
		char	line[768], *tok, *save;

		if (!p->display || p->session != s)
			continue;
		if (blob_get(p->silkie_blob, "ext", line, sizeof line)) {
			for (tok = strtok_r(line, ",", &save); tok != nullptr;
			    tok = strtok_r(nullptr, ",", &save)) {
				char	*dot = strchr(tok, '.');
				char	name[64];

				if (dot == nullptr || strcmp(dot + 1, "1") != 0)
					continue;
				if ((size_t)(dot - tok) >= sizeof name)
					continue;
				memcpy(name, tok, (size_t)(dot - tok));
				name[dot - tok] = '\0';
				if (known_sel(name))
					ext_add(ext, sizeof ext, name);
			}
		}
		if (blob_get(p->silkie_blob, "display", line, sizeof line))
			snprintf(display, sizeof display, "%s", line);
		if (blob_get(p->silkie_blob, "frame", line, sizeof line)) {
			int	c = 0, per = 0;

			sscanf(line, "%d %d", &c, &per);
			frame_caps |= c;
			if (per != 0)
				frame_period = per;
		}
		if (blob_get(p->silkie_blob, "gesture", line, sizeof line))
			gesture |= atoi(line);
		if (blob_get(p->silkie_blob, "haptic", line, sizeof line))
			haptic |= atoi(line);
		if (blob_get(p->silkie_blob, "ink", line, sizeof line))
			ink |= atoi(line);
		if (blob_get(p->silkie_blob, "latency", line, sizeof line)) {
			int	c = 0, in = 0, re = 0;

			sscanf(line, "%d %d %d", &c, &in, &re);
			lat_caps |= c;
			if (in != 0)
				lat_in = in;
			if (re != 0)
				lat_re = re;
		}
		if (blob_get(p->silkie_blob, "scroll", line, sizeof line))
			scroll |= atoi(line);
		if (blob_get(p->silkie_blob, "tablet", line, sizeof line))
			tablet |= atoi(line);
		if (peer_gfx(p, p->silkie_blob)) {
			char	f[80], m[80];
			int	has_f, has_m;

			saw_gfx = 1;
			has_f = blob_get(p->silkie_blob, "format", f, sizeof f);
			has_m = blob_get(p->silkie_blob, "media", m, sizeof m);
			/* A kitty peer with no Silkie list does not understand G=. */
			if (!has_f && !has_m)
				wg = 0;
			if (!has_f)
				snprintf(f, sizeof f, "24,32,100");
			if (!has_m)
				snprintf(m, sizeof m, "d,f,t,s");
			if (!have_fmt) {
				snprintf(format, sizeof format, "%s", f);
				snprintf(media, sizeof media, "%s", m);
				have_fmt = 1;
			} else {
				csv_intersect(format, sizeof format, f);
				csv_intersect(media, sizeof media, m);
			}
		}
	}
	if (ext[0] != '\0')
		ext_add(ext, sizeof ext, "caps");
	snprintf(s->silk_ext, sizeof s->silk_ext, "%s", ext);
	snprintf(s->silk_format, sizeof s->silk_format, "%s", format);
	snprintf(s->silk_media, sizeof s->silk_media, "%s", media);
	snprintf(s->silk_display, sizeof s->silk_display, "%s", display);
	s->silk_frame_caps = frame_caps;
	s->silk_frame_period = frame_period;
	s->silk_gesture_caps = gesture;
	s->silk_haptic_caps = haptic;
	s->silk_ink_caps = ink;
	s->silk_latency_caps = lat_caps;
	s->silk_latency_input = lat_in;
	s->silk_latency_repaint = lat_re;
	s->silk_scroll_caps = scroll;
	s->silk_tablet_caps = tablet;
	s->silk_any = ext[0] != '\0';
	s->silk_wg = saw_gfx && wg;
	if (strcmp(prev_disp, display) != 0) {
		struct dv_window	*w;

		TAILQ_FOREACH(w, &s->desktop.windows, entry) {
			if (!w->screen.silk_display)
				continue;
			w->screen.silk_shown[0] = '\0';
		}
	}
}

static void
note_line(struct dv_host *h, const char *key, const char *val)
{
	blob_set(h->silkie_blob, sizeof h->silkie_blob, key, val);
}

void
dv_silkie_note(struct dv_host *h, const unsigned char *osc, size_t len)
{
	char		 sel[32];
	struct silk_f	 f;
	long long	 id = -1;
	const char	*op;
	char		 built[768];

	if (h == nullptr || osc == nullptr)
		return;
	if (open_osc((const char *)osc, len, 0, sel, sizeof sel, &f) != 1)
		return;
	op = ffield(&f, "op");
	if (op == nullptr || strcmp(op, "cap") != 0)
		return;
	(void)ffield_int(&f, "id", 0, 2147483647ll, &id);
	if (id < Q_CAPS || id > Q_TABLET)
		return;
	if (strcmp(sel, "caps") == 0) {
		const char	*e = ffield(&f, "ext");
		const char	*m = ffield(&f, "media");
		const char	*fmt = ffield(&f, "format");

		if (e != nullptr)
			note_line(h, "ext", e);
		if (m != nullptr)
			note_line(h, "media", m);
		if (fmt != nullptr)
			note_line(h, "format", fmt);
		return;
	}
	if (strcmp(sel, "display") == 0) {
		static const char *keys[] = {
			"kind", "color", "bits", "gamut", "headroom",
			"scale", "period", "ppi", nullptr
		};
		size_t	used = 0;

		built[0] = '\0';
		for (int i = 0; keys[i] != nullptr; i++) {
			const char	*v = ffield(&f, keys[i]);

			if (v == nullptr)
				continue;
			if (used != 0 && used + 1 < sizeof built)
				built[used++] = ':';
			used += (size_t)snprintf(built + used, sizeof built - used,
			    "%s=%s", keys[i], v);
		}
		if (built[0] != '\0')
			note_line(h, "display", built);
		return;
	}
	if (strcmp(sel, "frame") == 0) {
		long long	caps = 0, per = 0;

		(void)ffield_int(&f, "caps", 0, 2147483647ll, &caps);
		(void)ffield_int(&f, "period", 0, 2147483647ll, &per);
		snprintf(built, sizeof built, "%lld %lld", caps, per);
		note_line(h, "frame", built);
		return;
	}
	if (strcmp(sel, "latency") == 0) {
		long long	caps = 0, in = 0, re = 0;

		(void)ffield_int(&f, "caps", 0, 2147483647ll, &caps);
		(void)ffield_int(&f, "input", 0, 2147483647ll, &in);
		(void)ffield_int(&f, "repaint", 0, 2147483647ll, &re);
		snprintf(built, sizeof built, "%lld %lld %lld", caps, in, re);
		note_line(h, "latency", built);
		return;
	}
	{
		long long	caps = 0;
		const char	*key = nullptr;

		(void)ffield_int(&f, "caps", 0, 2147483647ll, &caps);
		if (strcmp(sel, "gesture") == 0)
			key = "gesture";
		else if (strcmp(sel, "haptic") == 0)
			key = "haptic";
		else if (strcmp(sel, "ink") == 0)
			key = "ink";
		else if (strcmp(sel, "scroll") == 0)
			key = "scroll";
		else if (strcmp(sel, "tablet-pointer") == 0)
			key = "tablet";
		if (key != nullptr) {
			snprintf(built, sizeof built, "%lld", caps);
			note_line(h, key, built);
		}
	}
}

/* ---- modes ---------------------------------------------------------- */

static int
tablet_grant(int caps)
{
	int	g = 0;

	if (caps & 1)
		g |= 1;
	if (caps & (2 | 4))
		g |= 2;
	if (caps & 16)
		g |= 4;
	if (caps & 64)
		g |= 8;
	if (caps & 32)
		g |= 16;
	if (caps & 128)
		g |= 32;
	if (caps & 256)
		g |= 64;
	return (g);
}

static int
gesture_grant(int caps)
{
	int	g = 0;

	if (caps & (1 | 2 | 4 | 8))
		g |= 1;
	if (caps & 16)
		g |= 2;
	return (g);
}

static int
grant_of(const struct dv_session *s, const char *sel)
{
	if (s == nullptr)
		return (0);
	if (strcmp(sel, "tablet-pointer") == 0)
		return (tablet_grant(s->silk_tablet_caps));
	if (strcmp(sel, "gesture") == 0)
		return (gesture_grant(s->silk_gesture_caps));
	if (strcmp(sel, "scroll") == 0)
		return (s->silk_scroll_caps & 1) ? 1 : 0;
	if (strcmp(sel, "frame") == 0)
		return (s->silk_frame_caps & 1) ? 1 : 0;
	if (strcmp(sel, "latency") == 0)
		return (s->silk_latency_caps & 1) ? 1 : 0;
	if (strcmp(sel, "display") == 0)
		return (1);
	if (strcmp(sel, "ink") == 0)
		return (1);
	return (0);
}

static int *
mode_ptr(struct dv_screen *sc, const char *sel)
{
	if (strcmp(sel, "tablet-pointer") == 0)
		return (&sc->silk_tablet);
	if (strcmp(sel, "scroll") == 0)
		return (&sc->silk_scroll);
	if (strcmp(sel, "gesture") == 0)
		return (&sc->silk_gesture);
	if (strcmp(sel, "frame") == 0)
		return (&sc->silk_frame);
	if (strcmp(sel, "latency") == 0)
		return (&sc->silk_latency);
	if (strcmp(sel, "display") == 0)
		return (&sc->silk_display);
	if (strcmp(sel, "ink") == 0)
		return (&sc->silk_ink);
	return (nullptr);
}

static int
mode_of(const struct dv_screen *sc, const char *sel)
{
	if (strcmp(sel, "tablet-pointer") == 0)
		return (sc->silk_tablet);
	if (strcmp(sel, "scroll") == 0)
		return (sc->silk_scroll);
	if (strcmp(sel, "gesture") == 0)
		return (sc->silk_gesture);
	if (strcmp(sel, "frame") == 0)
		return (sc->silk_frame);
	if (strcmp(sel, "latency") == 0)
		return (sc->silk_latency);
	if (strcmp(sel, "display") == 0)
		return (sc->silk_display);
	if (strcmp(sel, "ink") == 0)
		return (sc->silk_ink);
	return (0);
}

static void
live_add(struct dv_window *w, int sel, int kind, int dev, int contact,
    int x, int y, int buttons, int tool)
{
	struct dv_silk_live	*slot = nullptr;

	for (int i = 0; i < 16; i++) {
		struct dv_silk_live	*L = &w->silk_live[i];

		if (L->used && L->sel == sel && L->device == dev &&
		    L->contact == contact && L->kind == kind) {
			slot = L;
			break;
		}
		if (!L->used && slot == nullptr)
			slot = L;
	}
	if (slot == nullptr)
		return;
	slot->used = 1;
	slot->sel = sel;
	slot->kind = kind;
	slot->device = dev;
	slot->contact = contact;
	slot->x = x;
	slot->y = y;
	slot->buttons = buttons;
	slot->tool = tool;
}

static void
live_drop(struct dv_window *w, int sel, int dev, int contact)
{
	for (int i = 0; i < 16; i++) {
		struct dv_silk_live	*L = &w->silk_live[i];

		if (L->used && L->sel == sel && L->device == dev &&
		    L->contact == contact)
			L->used = 0;
	}
}

void
dv_silkie_cancel_window(struct dv_window *w)
{
	char	fields[160];

	if (w == nullptr)
		return;
	for (int i = 0; i < 16; i++) {
		struct dv_silk_live	*L = &w->silk_live[i];

		if (!L->used)
			continue;
		if (L->sel == 0)
			snprintf(fields, sizeof fields,
			    "v=1:op=event:kind=%d:phase=4:device=%d:contact=%d:x=%d:y=%d:buttons=0:p=0",
			    L->kind, L->device, L->contact, L->x, L->y);
		else
			snprintf(fields, sizeof fields,
			    "v=1:op=event:kind=%d:phase=4:x=%d:y=%d",
			    L->kind, L->x, L->y);
		pty_fields(w, L->sel == 0 ? "tablet-pointer" : "gesture", fields);
		L->used = 0;
	}
	if (w->session != nullptr && w->session->silk_pad_win == w->id) {
		for (int i = 0; i < w->session->silk_pad_n; i++) {
			snprintf(fields, sizeof fields,
			    "v=1:op=event:kind=3:device=%d:button=%d:state=0",
			    w->session->silk_pad_dev, w->session->silk_pad_btn[i]);
			pty_fields(w, "tablet-pointer", fields);
		}
		w->session->silk_pad_n = 0;
		w->session->silk_pad_win = 0;
	}
	w->silk_epoch++;
}

void
dv_silkie_reset_pane(struct dv_window *w)
{
	struct dv_screen	*sc;

	if (w == nullptr)
		return;
	dv_silkie_cancel_window(w);
	sc = &w->screen;
	sc->silk_tablet = sc->silk_scroll = sc->silk_gesture = 0;
	sc->silk_frame = sc->silk_latency = sc->silk_display = sc->silk_ink = 0;
	sc->silk_frame_seq = 0;
	sc->silk_frame_dirty = 0;
	sc->silk_brush[0] = '\0';
	sc->silk_shown[0] = '\0';
	if (w->session != nullptr)
		dv_silkie_push(w->session);
}

static void
send_display_event(struct dv_window *w)
{
	const char	*body;
	char		 fields[512];
	const char	*unk =
	    "kind=unknown:color=unknown:bits=0:gamut=unknown:headroom=0:scale=0:period=0:ppi=0";

	if (w->session == nullptr)
		return;
	body = w->session->silk_display[0] != '\0' ? w->session->silk_display : unk;
	snprintf(fields, sizeof fields, "v=1:op=event:t=0:%s", body);
	pty_fields(w, "display", fields);
	snprintf(w->screen.silk_shown, sizeof w->screen.silk_shown, "%s", body);
}

static int
set_mode(struct dv_window *w, const char *sel, int mode)
{
	struct dv_screen	*sc = &w->screen;
	int			*slot = mode_ptr(sc, sel);
	int			 grant, prev;

	if (slot == nullptr || w->session == nullptr)
		return (0);
	grant = grant_of(w->session, sel);
	mode &= grant;
	if (strcmp(sel, "ink") == 0 && (sc->silk_tablet & 1) == 0)
		mode = 0;
	if (strcmp(sel, "scroll") == 0 || strcmp(sel, "frame") == 0 ||
	    strcmp(sel, "latency") == 0 || strcmp(sel, "display") == 0 ||
	    strcmp(sel, "ink") == 0)
		mode = mode ? 1 : 0;
	prev = *slot;
	if (prev == mode)
		return (mode);
	if (strcmp(sel, "tablet-pointer") == 0 || strcmp(sel, "gesture") == 0)
		dv_silkie_cancel_window(w);
	*slot = mode;
	if (strcmp(sel, "tablet-pointer") == 0 && (mode & 1) == 0 &&
	    sc->silk_ink) {
		sc->silk_ink = 0;
	}
	if (strcmp(sel, "frame") == 0) {
		if (mode) {
			sc->silk_frame_seq = 0;
			sc->silk_frame_dirty = 1;
		} else
			sc->silk_frame_dirty = 0;
	}
	if (strcmp(sel, "display") == 0) {
		if (mode)
			send_display_event(w);
		else
			sc->silk_shown[0] = '\0';
	}
	dv_silkie_push(w->session);
	return (mode);
}

static void
reply_mode(struct dv_window *w, const char *sel, long long id)
{
	char	fields[96];
	int	mode = mode_of(&w->screen, sel);

	if (id >= 0)
		snprintf(fields, sizeof fields, "v=1:op=mode:id=%lld:mode=%d",
		    id, mode);
	else
		snprintf(fields, sizeof fields, "v=1:op=mode:mode=%d", mode);
	pty_fields(w, sel, fields);
}

static unsigned
cell_px(const struct dv_session *s, int y)
{
	unsigned	v = 0;

	if (s != nullptr)
		v = y ? s->desktop.ypixel : s->desktop.xpixel;
	if (v == 0)
		v = y ? DV_DEFAULT_YPIXEL : DV_DEFAULT_XPIXEL;
	return (v);
}

static void
reply_query(struct dv_window *w, const char *sel, long long id)
{
	struct dv_session	*s = w->session;
	struct dv_screen	*sc = &w->screen;
	char			 fields[640];
	unsigned		 cw, ch;

	if (s == nullptr)
		return;
	cw = cell_px(s, 0);
	ch = cell_px(s, 1);
	if (strcmp(sel, "caps") == 0) {
		snprintf(fields, sizeof fields,
		    "v=1:op=cap:id=%lld:caps=3:ext=%s", id, s->silk_ext);
		if (s->silk_media[0] != '\0')
			snprintf(fields + strlen(fields),
			    sizeof fields - strlen(fields), ":media=%s",
			    s->silk_media);
		if (s->silk_format[0] != '\0')
			snprintf(fields + strlen(fields),
			    sizeof fields - strlen(fields), ":format=%s",
			    s->silk_format);
	} else if (strcmp(sel, "tablet-pointer") == 0) {
		snprintf(fields, sizeof fields,
		    "v=1:op=cap:id=%lld:caps=%d:mode=%d:w=%u:h=%u:cw=%u:ch=%u",
		    id, s->silk_tablet_caps, sc->silk_tablet,
		    sc->cols * cw, sc->rows * ch, cw, ch);
	} else if (strcmp(sel, "display") == 0) {
		const char	*body = s->silk_display[0] != '\0' ?
		    s->silk_display :
		    "kind=unknown:color=unknown:bits=0:gamut=unknown:headroom=0:scale=0:period=0:ppi=0";

		snprintf(fields, sizeof fields,
		    "v=1:op=cap:id=%lld:mode=%d:%s", id, sc->silk_display, body);
	} else if (strcmp(sel, "frame") == 0) {
		snprintf(fields, sizeof fields,
		    "v=1:op=cap:id=%lld:caps=%d:mode=%d:period=%d",
		    id, s->silk_frame_caps, sc->silk_frame, s->silk_frame_period);
	} else if (strcmp(sel, "gesture") == 0) {
		snprintf(fields, sizeof fields,
		    "v=1:op=cap:id=%lld:caps=%d:mode=%d",
		    id, s->silk_gesture_caps, sc->silk_gesture);
	} else if (strcmp(sel, "haptic") == 0) {
		snprintf(fields, sizeof fields, "v=1:op=cap:id=%lld:caps=%d",
		    id, s->silk_haptic_caps);
	} else if (strcmp(sel, "ink") == 0) {
		snprintf(fields, sizeof fields,
		    "v=1:op=cap:id=%lld:caps=%d:mode=%d",
		    id, s->silk_ink_caps, sc->silk_ink);
	} else if (strcmp(sel, "latency") == 0) {
		snprintf(fields, sizeof fields,
		    "v=1:op=cap:id=%lld:caps=%d:mode=%d:input=%d:repaint=%d",
		    id, s->silk_latency_caps, sc->silk_latency,
		    s->silk_latency_input, s->silk_latency_repaint);
	} else if (strcmp(sel, "scroll") == 0) {
		snprintf(fields, sizeof fields,
		    "v=1:op=cap:id=%lld:caps=%d:mode=%d",
		    id, s->silk_scroll_caps, sc->silk_scroll);
	} else
		return;
	pty_fields(w, sel, fields);
}

static int
one_of(const char *v, const char *const *ok)
{
	if (v == nullptr)
		return (0);
	for (int i = 0; ok[i] != nullptr; i++)
		if (strcmp(v, ok[i]) == 0)
			return (1);
	return (0);
}

static int
brush_ok(const struct silk_f *f)
{
	if (ffield(f, "i") == nullptr || ffield(f, "k") == nullptr ||
	    ffield(f, "w1") == nullptr)
		return (0);
	return (1);
}

static void
store_brush(struct dv_window *w, const struct silk_f *f)
{
	char	buf[160];
	size_t	used = 0;

	buf[0] = '\0';
	for (int i = 0; i < f->n && used + 1 < sizeof buf; i++) {
		if (strcmp(f->key[i], "v") == 0 || strcmp(f->key[i], "op") == 0 ||
		    strcmp(f->key[i], "id") == 0)
			continue;
		if (used != 0)
			buf[used++] = ':';
		used += (size_t)snprintf(buf + used, sizeof buf - used, "%s=%s",
		    f->key[i], f->val[i]);
	}
	snprintf(w->screen.silk_brush, sizeof w->screen.silk_brush, "%s", buf);
}

int
dv_silkie_program(struct dv_window *w, const char *body, size_t n)
{
	char		 sel[32];
	struct silk_f	 f;
	int		 st;
	const char	*op;
	long long	 id = -1, mode = -1;
	int		 have_mode;

	if (w == nullptr || body == nullptr)
		return (0);
	st = open_osc(body, n, 1, sel, sizeof sel, &f);
	if (st == 0)
		return (0);
	if (st < 0)
		return (1);
	if (!offered(w->session, sel))
		return (1);
	op = ffield(&f, "op");
	(void)ffield_int(&f, "id", 0, 2147483647ll, &id);
	have_mode = ffield(&f, "mode") != nullptr;
	if (have_mode && !ffield_int(&f, "mode", 0, 2147483647ll, &mode))
		return (1);
	if (strcmp(sel, "caps") == 0) {
		if (strcmp(op, "reset") == 0)
			dv_silkie_reset_pane(w);
		else if (strcmp(op, "query") == 0 && id >= 0)
			reply_query(w, sel, id);
		return (1);
	}
	if (strcmp(op, "query") == 0) {
		if (id >= 0)
			reply_query(w, sel, id);
		return (1);
	}
	if (strcmp(op, "mode") == 0) {
		if (!have_mode)
			reply_mode(w, sel, id);
		else {
			set_mode(w, sel, (int)mode);
			reply_mode(w, sel, id);
		}
		return (1);
	}
	if (strcmp(sel, "ink") == 0 && strcmp(op, "brush") == 0) {
		if (!brush_ok(&f))
			return (1);
		store_brush(w, &f);
		if (w->session != nullptr && w->session->desktop.focused == w)
			dv_silkie_push(w->session);
		return (1);
	}
	if (strcmp(sel, "haptic") == 0 &&
	    (strcmp(op, "play") == 0 || strcmp(op, "start") == 0 ||
	    strcmp(op, "stop") == 0)) {
		static const char *tap[] = {
			"generic", "alignment", "level", "click", "complete", nullptr
		};
		static const char *cont[] = {
			"ink", "pencil", "marker", "brush", "eraser", nullptr
		};
		static const char *tgt[] = {
			"trackpad", "pen", "dial", nullptr
		};
		const char	*pattern = ffield(&f, "pattern");
		const char	*target = ffield(&f, "target");

		if (target != nullptr && !one_of(target, tgt))
			return (1);
		if (strcmp(op, "play") == 0 && !one_of(pattern, tap))
			return (1);
		if (strcmp(op, "start") == 0 && !one_of(pattern, cont))
			return (1);
		if (w->session != nullptr && w->session->desktop.focused == w) {
			char	fields[512];
			size_t	used = 0;

			fields[0] = '\0';
			for (int i = 0; i < f.n && used + 1 < sizeof fields; i++) {
				if (used != 0)
					fields[used++] = ':';
				used += (size_t)snprintf(fields + used,
				    sizeof fields - used, "%s=%s", f.key[i], f.val[i]);
			}
			host_fields(w->session, "haptic", fields);
		}
		return (1);
	}
	return (1);
}

/* ---- host push ------------------------------------------------------ */

static int
remap_brush(struct dv_window *w, char *out, size_t cap)
{
	struct silk_f	 f;
	size_t		 used = 0;
	const char	*raw = w->screen.silk_brush;

	out[0] = '\0';
	if (raw[0] == '\0')
		return (0);
	if (!parse_fields(&f, raw, strlen(raw), 1))
		return (0);
	for (int i = 0; i < f.n && used + 1 < cap; i++) {
		char		piece[64];
		const char	*val = f.val[i];

		if (strcmp(f.key[i], "i") == 0) {
			long long	id = 0;
			unsigned	host;

			if (!parse_i64(val, 0, 2147483647ll, &id))
				return (0);
			host = dv_kitty_virtual_id(w, (unsigned)id);
			if (host == 0)
				return (0);
			snprintf(piece, sizeof piece, "i=%u", host);
			val = nullptr;
			if (used != 0)
				out[used++] = ':';
			used += (size_t)snprintf(out + used, cap - used, "%s", piece);
			continue;
		}
		if (used != 0)
			out[used++] = ':';
		used += (size_t)snprintf(out + used, cap - used, "%s=%s",
		    f.key[i], val);
	}
	return (1);
}

void
dv_silkie_push(struct dv_session *s)
{
	struct dv_window	*w;
	int			 tablet = 0, scroll = 0, gesture = 0;
	int			 frame = 0, latency = 0, display = 0;
	char			 num[32];

	if (s == nullptr || !s->silk_any)
		return;
	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		tablet |= w->screen.silk_tablet;
		scroll |= w->screen.silk_scroll ? 1 : 0;
		gesture |= w->screen.silk_gesture;
		frame |= w->screen.silk_frame ? 1 : 0;
		latency |= w->screen.silk_latency ? 1 : 0;
		display |= w->screen.silk_display ? 1 : 0;
	}
	if (ext_has(s->silk_ext, "tablet-pointer") &&
	    tablet != s->silk_sent_tablet) {
		snprintf(num, sizeof num, "v=1:op=mode:mode=%d", tablet);
		host_fields(s, "tablet-pointer", num);
		s->silk_sent_tablet = tablet;
	}
	if (ext_has(s->silk_ext, "scroll") && scroll != s->silk_sent_scroll) {
		snprintf(num, sizeof num, "v=1:op=mode:mode=%d", scroll);
		host_fields(s, "scroll", num);
		s->silk_sent_scroll = scroll;
	}
	if (ext_has(s->silk_ext, "gesture") && gesture != s->silk_sent_gesture) {
		snprintf(num, sizeof num, "v=1:op=mode:mode=%d", gesture);
		host_fields(s, "gesture", num);
		s->silk_sent_gesture = gesture;
	}
	if (ext_has(s->silk_ext, "frame") && frame != s->silk_sent_frame) {
		snprintf(num, sizeof num, "v=1:op=mode:mode=%d", frame);
		host_fields(s, "frame", num);
		s->silk_sent_frame = frame;
	}
	if (ext_has(s->silk_ext, "latency") && latency != s->silk_sent_latency) {
		snprintf(num, sizeof num, "v=1:op=mode:mode=%d", latency);
		host_fields(s, "latency", num);
		s->silk_sent_latency = latency;
	}
	if (ext_has(s->silk_ext, "display") && display != s->silk_sent_display) {
		snprintf(num, sizeof num, "v=1:op=mode:mode=%d", display);
		host_fields(s, "display", num);
		s->silk_sent_display = display;
	}
	if (ext_has(s->silk_ext, "ink")) {
		struct dv_window	*fw = s->desktop.focused;
		char			 brush[200];
		int			 on = fw != nullptr && fw->screen.silk_ink &&
		    (fw->screen.silk_tablet & 1) && remap_brush(fw, brush, sizeof brush);

		if (!on) {
			if (s->silk_sent_ink != 0 || s->silk_sent_brush[0] != '\0') {
				host_fields(s, "ink", "v=1:op=mode:mode=0");
				s->silk_sent_ink = 0;
				s->silk_sent_brush[0] = '\0';
			}
		} else if (s->silk_sent_ink != 1 ||
		    strcmp(s->silk_sent_brush, brush) != 0) {
			char	fields[240];

			snprintf(fields, sizeof fields, "v=1:op=brush:%s", brush);
			host_fields(s, "ink", fields);
			host_fields(s, "ink", "v=1:op=mode:mode=1");
			s->silk_sent_ink = 1;
			snprintf(s->silk_sent_brush, sizeof s->silk_sent_brush,
			    "%s", brush);
		}
	}
}

void
dv_silkie_attach(struct dv_peer *p)
{
	if (p == nullptr || p->session == nullptr)
		return;
	p->session->silk_sent_tablet = -1;
	p->session->silk_sent_scroll = -1;
	p->session->silk_sent_gesture = -1;
	p->session->silk_sent_frame = -1;
	p->session->silk_sent_latency = -1;
	p->session->silk_sent_display = -1;
	p->session->silk_sent_ink = -1;
	p->session->silk_sent_brush[0] = '\0';
	dv_silkie_peers_changed(p->session);
}

void
dv_silkie_focus(struct dv_session *s, struct dv_window *prev)
{
	char	fields[128];

	if (s == nullptr)
		return;
	if (prev != nullptr && s->silk_pad_win == prev->id) {
		for (int i = 0; i < s->silk_pad_n; i++) {
			snprintf(fields, sizeof fields,
			    "v=1:op=event:kind=3:device=%d:button=%d:state=0",
			    s->silk_pad_dev, s->silk_pad_btn[i]);
			pty_fields(prev, "tablet-pointer", fields);
		}
		s->silk_pad_n = 0;
		s->silk_pad_win = 0;
	}
	dv_silkie_push(s);
}

void
dv_silkie_output(struct dv_window *w)
{
	if (w != nullptr && w->screen.silk_frame)
		w->screen.silk_frame_dirty = 1;
}

void
dv_silkie_ink_placed(struct dv_window *w)
{
	if (w != nullptr && w->session != nullptr &&
	    w->session->desktop.focused == w)
		dv_silkie_push(w->session);
}

int
dv_silkie_gfx_ok(const struct dv_session *s, long f, int G, int medium)
{
	int	stdf = (f == 24 || f == 32 || f == 100);
	int	stdt = (medium == 'd' || medium == 'f' || medium == 't' ||
	    medium == 's');
	char	num[16];

	if (stdf && stdt && G == 0)
		return (1);
	if (s == nullptr || !s->silk_any)
		return (0);
	if (f == 64 || f == 200) {
		snprintf(num, sizeof num, "%ld", f);
		if (!csv_has(s->silk_format, num))
			return (0);
	} else if (!stdf)
		return (0);
	if (medium == 'g') {
		if (!csv_has(s->silk_media, "g"))
			return (0);
	} else if (!stdt)
		return (0);
	if (G != 0 && ((f != 24 && f != 32) || !s->silk_wg))
		return (0);
	return (1);
}

/* ---- host events ---------------------------------------------------- */

static struct dv_window *
silk_at(struct dv_session *s, int x, int y)
{
	struct dv_window	*w, *best = nullptr;

	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		int	framed = w->framed && !w->zoomed;
		int	fw = (int)w->geom.cols + (framed ? 2 : 0);
		int	fh = (int)w->geom.rows + (framed ? 2 : 0);

		if (w->hidden || w->aside)
			continue;
		if (x < w->geom.x || x >= w->geom.x + fw || y < w->geom.y ||
		    y >= w->geom.y + fh)
			continue;
		if (best == nullptr || w->z > best->z)
			best = w;
	}
	return (best);
}

static struct dv_window *
win_id(struct dv_session *s, unsigned id)
{
	struct dv_window	*w;

	if (s == nullptr || id == 0)
		return (nullptr);
	TAILQ_FOREACH(w, &s->desktop.windows, entry)
		if (w->id == id)
			return (w);
	return (nullptr);
}

static void
host_cell(const struct dv_peer *p, unsigned *cw, unsigned *ch)
{
	*cw = p->tty.xpixel ? p->tty.xpixel : DV_DEFAULT_XPIXEL;
	*ch = p->tty.ypixel ? p->tty.ypixel : DV_DEFAULT_YPIXEL;
}

static void
local_xy(struct dv_peer *p, struct dv_window *w, int x, int y, int *ox, int *oy)
{
	int		framed = w->framed && !w->zoomed;
	unsigned	hcw, hch, acw, ach;

	host_cell(p, &hcw, &hch);
	acw = cell_px(w->session, 0);
	ach = cell_px(w->session, 1);
	*ox = gp::window_coordinate(x, p->view.vx,
	    w->geom.x + (framed ? 1 : 0), (int)w->ox, hcw, acw);
	*oy = gp::window_coordinate(y, p->view.vy,
	    w->geom.y + (framed ? 1 : 0), (int)w->oy, hch, ach);
}

static void
desk_of(struct dv_peer *p, int x, int y, int *cx, int *cy)
{
	unsigned	hcw, hch;

	host_cell(p, &hcw, &hch);
	*cx = gp::cell_at(x, hcw) + p->view.vx;
	*cy = gp::cell_at(y, hch) + p->view.vy;
}

static struct dv_silk_hit *
hit_of(struct dv_peer *p, int sel, int dev, int contact, int kind)
{
	for (int i = 0; i < 32; i++) {
		struct dv_silk_hit	*h = &p->silk_hit[i];

		if (h->used && h->sel == sel && h->device == dev &&
		    h->contact == contact && (kind < 0 || h->kind == kind))
			return (h);
	}
	return (nullptr);
}

static struct dv_silk_hit *
hit_new(struct dv_peer *p)
{
	for (int i = 0; i < 32; i++)
		if (!p->silk_hit[i].used)
			return (&p->silk_hit[i]);
	return (nullptr);
}

static void
emit_fields(struct dv_window *w, const char *sel, const struct silk_f *f,
    int tx, int ty, int do_xy, int strip_hw)
{
	char	fields[2048];
	size_t	used = 0;

	fields[0] = '\0';
	for (int i = 0; i < f->n && used + 1 < sizeof fields; i++) {
		char	piece[64];
		const char *k = f->key[i];
		const char *v = f->val[i];

		if (strip_hw && (strcmp(k, "serial") == 0 || strcmp(k, "hwid") == 0 ||
		    strcmp(k, "vid") == 0 || strcmp(k, "pid") == 0))
			continue;
		if (do_xy && (strcmp(k, "x") == 0 || strcmp(k, "y") == 0)) {
			snprintf(piece, sizeof piece, "%s=%d", k,
			    strcmp(k, "x") == 0 ? tx : ty);
			v = nullptr;
			if (used != 0)
				fields[used++] = ':';
			used += (size_t)snprintf(fields + used, sizeof fields - used,
			    "%s", piece);
			continue;
		}
		if (used != 0)
			fields[used++] = ':';
		used += (size_t)snprintf(fields + used, sizeof fields - used,
		    "%s=%s", k, v);
	}
	pty_fields(w, sel, fields);
}

static void
send_desc(struct dv_peer *p, struct dv_window *w, int dev, int allow_hw)
{
	for (int i = 0; i < 8; i++) {
		struct dv_silk_desc	*d = &p->silk_desc[i];

		if (!d->used || d->device != dev)
			continue;
		if (allow_hw)
			pty_fields(w, "tablet-pointer", d->fields);
		else {
			struct silk_f	f;

			if (parse_fields(&f, d->fields, strlen(d->fields), 0))
				emit_fields(w, "tablet-pointer", &f, 0, 0, 0, 1);
		}
		return;
	}
}

static void
store_desc(struct dv_peer *p, const struct silk_f *f, int dev, int kind)
{
	struct dv_silk_desc	*d = nullptr;
	char			 fields[700];
	size_t			 used = 0;

	for (int i = 0; i < 8; i++) {
		if (p->silk_desc[i].used && p->silk_desc[i].device == dev) {
			d = &p->silk_desc[i];
			break;
		}
		if (!p->silk_desc[i].used && d == nullptr)
			d = &p->silk_desc[i];
	}
	if (d == nullptr)
		return;
	fields[0] = '\0';
	for (int i = 0; i < f->n && used + 1 < sizeof fields; i++) {
		if (used != 0)
			fields[used++] = ':';
		used += (size_t)snprintf(fields + used, sizeof fields - used,
		    "%s=%s", f->key[i], f->val[i]);
	}
	d->used = 1;
	d->device = dev;
	d->kind = kind;
	snprintf(d->fields, sizeof d->fields, "%s", fields);
}

static unsigned
mouse_code(int buttons, int motion)
{
	int	b;

	if (buttons & 4)
		b = 1;
	else if (buttons & 2)
		b = 2;
	else if (buttons & 1)
		b = 0;
	else
		b = 3;
	return (unsigned)(motion ? 32 + b : b);
}

static void
inject_at(struct dv_peer *p, int x, int y, unsigned code, int press)
{
	unsigned	hcw, hch;
	int		vx, vy;

	host_cell(p, &hcw, &hch);
	vx = gp::cell_at(x, hcw) + 1;
	vy = gp::cell_at(y, hch) + 1;
	if (vx < 1)
		vx = 1;
	if (vy < 1)
		vy = 1;
	dv_mouse_inject(p, (unsigned)vx, (unsigned)vy, code, press);
}

static int
kind_bit(int kind)
{
	if (kind == 1)
		return (1);
	if (kind == 2)
		return (2);
	if (kind == 3)
		return (4);
	if (kind == 4)
		return (16);
	return (0);
}

static void
tablet_event(struct dv_peer *p, struct silk_f *f)
{
	struct dv_session	*s = p->session;
	long long		 kind = 0, phase = 0, dev = 0, contact = 0;
	long long		 x = 0, y = 0, src = 0, conf = 1, buttons = 0;
	long long		 tool = 0;
	int			 dx, dy, tx, ty, have_xy;
	struct dv_window	*w = nullptr;
	struct dv_silk_hit	*hit;
	int			 bit;

	if (!ffield_int(f, "kind", 0, 64, &kind))
		return;
	(void)ffield_int(f, "phase", 0, 64, &phase);
	(void)ffield_int(f, "device", 0, 2147483647ll, &dev);
	(void)ffield_int(f, "contact", 0, 2147483647ll, &contact);
	have_xy = ffield(f, "x") != nullptr;
	(void)ffield_int(f, "x", -1073741823ll, 1073741823ll, &x);
	(void)ffield_int(f, "y", -1073741823ll, 1073741823ll, &y);
	(void)ffield_int(f, "src", 0, 64, &src);
	if (ffield(f, "conf") == nullptr)
		conf = 1;
	else if (!ffield_int(f, "conf", 0, 1, &conf))
		return;
	(void)ffield_int(f, "buttons", 0, 2147483647ll, &buttons);
	(void)ffield_int(f, "tool", 0, 2147483647ll, &tool);
	desk_of(p, (int)x, (int)y, &dx, &dy);
	bit = kind_bit((int)kind);
	hit = hit_of(p, 0, (int)dev, (int)contact, (int)kind);

	if (kind == 3) {
		struct dv_window	*fw = s->desktop.focused;
		long long		 button = -1, state = -1;

		if (fw == nullptr || (fw->screen.silk_tablet & 4) == 0)
			return;
		(void)ffield_int(f, "button", 0, 2147483647ll, &button);
		(void)ffield_int(f, "state", 0, 64, &state);
		send_desc(p, fw, (int)dev, (fw->screen.silk_tablet & 8) != 0);
		emit_fields(fw, "tablet-pointer", f, 0, 0, 0, 0);
		if (button >= 0 && state == 1 && s->silk_pad_n < 16) {
			s->silk_pad_dev = (int)dev;
			s->silk_pad_win = fw->id;
			s->silk_pad_btn[s->silk_pad_n++] = (int)button;
		} else if (button >= 0 && state == 0) {
			int	j = 0;

			for (int i = 0; i < s->silk_pad_n; i++)
				if (s->silk_pad_btn[i] != (int)button)
					s->silk_pad_btn[j++] = s->silk_pad_btn[i];
			s->silk_pad_n = j;
		}
		return;
	}

	if (phase == 0) {
		w = silk_at(s, dx, dy);
		if (kind == 1 && w != nullptr && (w->screen.silk_tablet & 1)) {
			local_xy(p, w, (int)x, (int)y, &tx, &ty);
			emit_fields(w, "tablet-pointer", f, tx, ty, src != 1, 0);
		} else if (have_xy)
			inject_at(p, (int)x, (int)y, mouse_code(0, 1), 1);
		return;
	}

	if (hit != nullptr) {
		w = win_id(s, hit->window);
		if (w == nullptr || hit->epoch != w->silk_epoch) {
			hit->used = 0;
			hit = nullptr;
			w = nullptr;
		}
	}

	if (phase == 5) {
		if (w != nullptr) {
			local_xy(p, w, (int)x, (int)y, &tx, &ty);
			emit_fields(w, "tablet-pointer", f, tx, ty, src != 1, 0);
		}
		if (hit != nullptr)
			hit->used = 0;
		live_drop(w, 0, (int)dev, (int)contact);
		return;
	}

	if (phase == 7 && hit != nullptr && w != nullptr && !hit->legacy) {
		local_xy(p, w, (int)x, (int)y, &tx, &ty);
		emit_fields(w, "tablet-pointer", f, tx, ty, src != 1, 0);
		return;
	}

	if (hit == nullptr && (phase == 1 || kind == 4)) {
		if (src == 1) {
			w = win_id(s, s->hover_win);
			if (w == nullptr)
				w = s->desktop.focused;
		} else
			w = silk_at(s, dx, dy);
		if (kind == 4) {
			for (int i = 0; i < 32 && w == nullptr; i++)
				if (p->silk_hit[i].used && p->silk_hit[i].sel == 0 &&
				    p->silk_hit[i].tool == (int)tool)
					w = win_id(s, p->silk_hit[i].window);
			if (w == nullptr)
				w = silk_at(s, dx, dy);
			if (w != nullptr && (w->screen.silk_tablet & 16)) {
				local_xy(p, w, (int)x, (int)y, &tx, &ty);
				emit_fields(w, "tablet-pointer", f, tx, ty, 1, 0);
			}
			return;
		}
	}

	if (w == nullptr && hit == nullptr)
		w = silk_at(s, dx, dy);

	if (phase == 1 && hit == nullptr && w != nullptr) {
		int	legacy = (w->screen.silk_tablet & bit) == 0;
		int	hide = !legacy && conf == 0 && (w->screen.silk_tablet & 64) == 0;

		hit = hit_new(p);
		if (hit == nullptr)
			return;
		memset(hit, 0, sizeof *hit);
		hit->used = 1;
		hit->sel = 0;
		hit->kind = (int)kind;
		hit->device = (int)dev;
		hit->contact = (int)contact;
		hit->src = (int)src;
		hit->window = w->id;
		hit->epoch = w->silk_epoch;
		hit->legacy = legacy;
		hit->conf = (int)conf;
		hit->buttons = (int)buttons;
		hit->tool = (int)tool;
		hit->reported = 0;
		if (legacy) {
			inject_at(p, (int)x, (int)y, mouse_code((int)buttons, 0), 1);
			return;
		}
		if (hide)
			return;
		if (!hit->described) {
			send_desc(p, w, (int)dev, (w->screen.silk_tablet & 8) != 0);
			hit->described = 1;
		}
		local_xy(p, w, (int)x, (int)y, &tx, &ty);
		emit_fields(w, "tablet-pointer", f, tx, ty, src != 1, 0);
		hit->reported = 1;
		live_add(w, 0, (int)kind, (int)dev, (int)contact, tx, ty,
		    (int)buttons, (int)tool);
		return;
	}

	if (hit == nullptr || w == nullptr)
		return;
	if (hit->legacy) {
		int	motion = phase == 2 || phase == 6;
		int	press = phase == 1 || phase == 2 || phase == 6;

		inject_at(p, (int)x, (int)y, mouse_code((int)buttons, motion), press);
		if (phase == 3 || phase == 4)
			hit->used = 0;
		return;
	}
	if (conf == 0 && (w->screen.silk_tablet & 64) == 0) {
		if (hit->reported) {
			char	fields[160];

			snprintf(fields, sizeof fields,
			    "v=1:op=event:kind=%lld:phase=4:device=%lld:contact=%lld:buttons=0:p=0",
			    kind, dev, contact);
			pty_fields(w, "tablet-pointer", fields);
			live_drop(w, 0, (int)dev, (int)contact);
			hit->reported = 0;
		}
		if (phase == 3 || phase == 4)
			hit->used = 0;
		return;
	}
	if (!hit->reported && conf != 0 && (phase == 2 || phase == 6)) {
		/* A contact that stops being accidental begins with down. */
		local_xy(p, w, (int)x, (int)y, &tx, &ty);
		for (int i = 0; i < f->n; i++)
			if (strcmp(f->key[i], "phase") == 0)
				f->val[i] = const_cast<char *>("1");
		if (!hit->described) {
			send_desc(p, w, (int)dev, (w->screen.silk_tablet & 8) != 0);
			hit->described = 1;
		}
		emit_fields(w, "tablet-pointer", f, tx, ty, src != 1, 0);
		hit->reported = 1;
		live_add(w, 0, (int)kind, (int)dev, (int)contact, tx, ty,
		    (int)buttons, (int)tool);
		return;
	}
	if (phase == 6 && !hit->reported)
		return;
	local_xy(p, w, (int)x, (int)y, &tx, &ty);
	if (src == 1)
		emit_fields(w, "tablet-pointer", f, 0, 0, 0, 0);
	else
		emit_fields(w, "tablet-pointer", f, tx, ty, 1, 0);
	if (hit->reported)
		live_add(w, 0, (int)kind, (int)dev, (int)contact, tx, ty,
		    (int)buttons, (int)tool);
	if (phase == 3 || phase == 4) {
		live_drop(w, 0, (int)dev, (int)contact);
		hit->used = 0;
	}
}

static void
gesture_event(struct dv_peer *p, struct silk_f *f)
{
	struct dv_session	*s = p->session;
	long long		 kind = 0, phase = 0, x = 0, y = 0;
	int			 dx, dy, tx, ty, bit;
	struct dv_window	*w;
	struct dv_silk_hit	*hit;

	if (!ffield_int(f, "kind", 1, 5, &kind) ||
	    !ffield_int(f, "phase", 1, 4, &phase))
		return;
	(void)ffield_int(f, "x", -1073741823ll, 1073741823ll, &x);
	(void)ffield_int(f, "y", -1073741823ll, 1073741823ll, &y);
	bit = kind == 5 ? 2 : 1;
	desk_of(p, (int)x, (int)y, &dx, &dy);
	hit = hit_of(p, 1, 0, 0, (int)kind);
	if (phase == 1 || hit == nullptr) {
		w = silk_at(s, dx, dy);
		if (w == nullptr || (w->screen.silk_gesture & bit) == 0)
			return;
		if (phase != 1)
			return;
		hit = hit_of(p, 1, 0, 0, (int)kind);
		if (hit == nullptr)
			hit = hit_new(p);
		if (hit == nullptr)
			return;
		memset(hit, 0, sizeof *hit);
		hit->used = 1;
		hit->sel = 1;
		hit->kind = (int)kind;
		hit->window = w->id;
		hit->epoch = w->silk_epoch;
	} else {
		w = win_id(s, hit->window);
		if (w == nullptr || hit->epoch != w->silk_epoch) {
			hit->used = 0;
			return;
		}
	}
	local_xy(p, w, (int)x, (int)y, &tx, &ty);
	emit_fields(w, "gesture", f, tx, ty, 1, 0);
	if (phase == 1)
		live_add(w, 1, (int)kind, 0, 0, tx, ty, 0, 0);
	if (phase == 3 || phase == 4) {
		live_drop(w, 1, 0, 0);
		hit->used = 0;
	}
}

static void
scroll_event(struct dv_peer *p, struct silk_f *f)
{
	struct dv_session	*s = p->session;
	long long		 x = 0, y = 0, dy = 0;
	int			 dx, dy_cell, tx, ty;
	struct dv_window	*w;
	unsigned		 hch;

	(void)ffield_int(f, "x", -1073741823ll, 1073741823ll, &x);
	(void)ffield_int(f, "y", -1073741823ll, 1073741823ll, &y);
	if (!ffield_int(f, "dy", -2000000000ll, 2000000000ll, &dy))
		return;
	desk_of(p, (int)x, (int)y, &dx, &dy_cell);
	w = silk_at(s, dx, dy_cell);
	if (w != nullptr && w->screen.silk_scroll) {
		local_xy(p, w, (int)x, (int)y, &tx, &ty);
		emit_fields(w, "scroll", f, tx, ty, 1, 0);
		return;
	}
	host_cell(p, &hch, &hch);
	{
		int	step = (int)hch * 100;

		if (step < 1)
			step = 1;
		p->silk_scroll_resid += (int)dy;
		while (p->silk_scroll_resid >= step) {
			inject_at(p, (int)x, (int)y, 65, 1);
			p->silk_scroll_resid -= step;
		}
		while (p->silk_scroll_resid <= -step) {
			inject_at(p, (int)x, (int)y, 64, 1);
			p->silk_scroll_resid += step;
		}
	}
}

static void
frame_event(struct dv_peer *p, const struct silk_f *f)
{
	struct dv_session	*s = p->session;
	struct dv_window	*w;
	long long		 per = 0;
	const char		*t = ffield(f, "t");
	const char		*tu = ffield(f, "tu");

	(void)ffield_int(f, "period", 0, 2147483647ll, &per);
	if (per != 0)
		s->silk_frame_period = (int)per;
	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		char	fields[128];

		if (!w->screen.silk_frame || !w->screen.silk_frame_dirty)
			continue;
		if (tu != nullptr)
			snprintf(fields, sizeof fields,
			    "v=1:op=event:seq=%u:t=%s:tu=%s:period=%d",
			    w->screen.silk_frame_seq, t != nullptr ? t : "0", tu,
			    s->silk_frame_period);
		else
			snprintf(fields, sizeof fields,
			    "v=1:op=event:seq=%u:t=%s:period=%d",
			    w->screen.silk_frame_seq, t != nullptr ? t : "0",
			    s->silk_frame_period);
		pty_fields(w, "frame", fields);
		w->screen.silk_frame_seq++;
		w->screen.silk_frame_dirty = 0;
	}
}

static void
display_body(const struct silk_f *f, char *out, size_t cap)
{
	static const char *keys[] = {
		"kind", "color", "bits", "gamut", "headroom", "scale",
		"period", "ppi", nullptr
	};
	size_t	used = 0;

	out[0] = '\0';
	for (int i = 0; keys[i] != nullptr; i++) {
		const char	*v = ffield(f, keys[i]);

		if (v == nullptr)
			continue;
		if (used != 0 && used + 1 < cap)
			out[used++] = ':';
		used += (size_t)snprintf(out + used, cap - used, "%s=%s",
		    keys[i], v);
	}
}

static void
display_event(struct dv_peer *p, const struct silk_f *f)
{
	struct dv_session	*s = p->session;
	struct dv_window	*w;
	char			 body[384], fields[512];
	const char		*t = ffield(f, "t");

	display_body(f, body, sizeof body);
	if (body[0] == '\0')
		return;
	snprintf(s->silk_display, sizeof s->silk_display, "%s", body);
	snprintf(fields, sizeof fields, "v=1:op=event:t=%s:%s",
	    t != nullptr ? t : "0", body);
	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		if (!w->screen.silk_display)
			continue;
		if (strcmp(w->screen.silk_shown, body) == 0)
			continue;
		pty_fields(w, "display", fields);
		snprintf(w->screen.silk_shown, sizeof w->screen.silk_shown, "%s",
		    body);
	}
}

static void
absorb_reply(struct dv_peer *p, const char *sel, const struct silk_f *f)
{
	char		built[768];
	long long	caps = 0, per = 0, in = 0, re = 0;

	if (strcmp(sel, "caps") == 0) {
		if (ffield(f, "ext") != nullptr)
			blob_set(p->silkie_blob, sizeof p->silkie_blob, "ext",
			    ffield(f, "ext"));
		if (ffield(f, "media") != nullptr)
			blob_set(p->silkie_blob, sizeof p->silkie_blob, "media",
			    ffield(f, "media"));
		if (ffield(f, "format") != nullptr)
			blob_set(p->silkie_blob, sizeof p->silkie_blob, "format",
			    ffield(f, "format"));
		return;
	}
	if (strcmp(sel, "display") == 0) {
		display_body(f, built, sizeof built);
		if (built[0] != '\0')
			blob_set(p->silkie_blob, sizeof p->silkie_blob, "display",
			    built);
		return;
	}
	if (strcmp(sel, "frame") == 0) {
		(void)ffield_int(f, "caps", 0, 2147483647ll, &caps);
		(void)ffield_int(f, "period", 0, 2147483647ll, &per);
		snprintf(built, sizeof built, "%lld %lld", caps, per);
		blob_set(p->silkie_blob, sizeof p->silkie_blob, "frame", built);
		return;
	}
	if (strcmp(sel, "latency") == 0) {
		(void)ffield_int(f, "caps", 0, 2147483647ll, &caps);
		(void)ffield_int(f, "input", 0, 2147483647ll, &in);
		(void)ffield_int(f, "repaint", 0, 2147483647ll, &re);
		snprintf(built, sizeof built, "%lld %lld %lld", caps, in, re);
		blob_set(p->silkie_blob, sizeof p->silkie_blob, "latency", built);
		return;
	}
	(void)ffield_int(f, "caps", 0, 2147483647ll, &caps);
	snprintf(built, sizeof built, "%lld", caps);
	if (strcmp(sel, "gesture") == 0)
		blob_set(p->silkie_blob, sizeof p->silkie_blob, "gesture", built);
	else if (strcmp(sel, "haptic") == 0)
		blob_set(p->silkie_blob, sizeof p->silkie_blob, "haptic", built);
	else if (strcmp(sel, "ink") == 0)
		blob_set(p->silkie_blob, sizeof p->silkie_blob, "ink", built);
	else if (strcmp(sel, "scroll") == 0)
		blob_set(p->silkie_blob, sizeof p->silkie_blob, "scroll", built);
	else if (strcmp(sel, "tablet-pointer") == 0)
		blob_set(p->silkie_blob, sizeof p->silkie_blob, "tablet", built);
}

int
dv_silkie_from_host(struct dv_peer *p, const char *body, size_t n)
{
	char		 sel[32];
	struct silk_f	 f;
	int		 st;
	const char	*op;
	long long	 id = -1;

	if (p == nullptr || body == nullptr)
		return (0);
	st = open_osc(body, n, 0, sel, sizeof sel, &f);
	if (st == 0)
		return (0);
	if (st < 0)
		return (1);
	op = ffield(&f, "op");
	(void)ffield_int(&f, "id", 0, 2147483647ll, &id);
	if (op != nullptr && strcmp(op, "cap") == 0 && id >= Q_CAPS && id <= Q_TABLET) {
		absorb_reply(p, sel, &f);
		if (p->session != nullptr)
			dv_silkie_peers_changed(p->session);
		return (1);
	}
	if (p->session == nullptr)
		return (1);
	if (strcmp(sel, "tablet-pointer") == 0) {
		if (strcmp(op, "device") == 0) {
			long long	dev = 0, kind = 0;

			(void)ffield_int(&f, "device", 0, 2147483647ll, &dev);
			(void)ffield_int(&f, "kind", 0, 64, &kind);
			store_desc(p, &f, (int)dev, (int)kind);
			return (1);
		}
		if (strcmp(op, "label") == 0) {
			struct dv_window	*fw = p->session->desktop.focused;

			if (fw != nullptr && (fw->screen.silk_tablet & 4))
				emit_fields(fw, "tablet-pointer", &f, 0, 0, 0, 0);
			return (1);
		}
		if (strcmp(op, "event") == 0)
			tablet_event(p, &f);
		return (1);
	}
	if (strcmp(op, "event") != 0)
		return (1);
	if (strcmp(sel, "scroll") == 0)
		scroll_event(p, &f);
	else if (strcmp(sel, "gesture") == 0)
		gesture_event(p, &f);
	else if (strcmp(sel, "frame") == 0)
		frame_event(p, &f);
	else if (strcmp(sel, "display") == 0)
		display_event(p, &f);
	return (1);
}
