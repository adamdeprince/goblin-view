/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Multiplexer side of the Silkie OSC 777 extensions: caps, tablet
 * pointer, scroll, gesture, haptic, ink, frame, latency, display, font
 * and depth.
 * Written from the published specifications. The inner program never
 * talks to the host; this process answers queries with what it can
 * carry and what an attached display listed, and it translates
 * coordinates into the pane.
 */

#include "silkie.hpp"
#include "pstatus.hpp"
#include "pointer_coordinates.hpp"

#include <algorithm>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
	Q_TABLET,
	Q_FONT,
	Q_LAST = Q_FONT,
	/* Queries that follow a brush, numbered by it. */
	Q_INK_SYNC = 0x60000000,
	Q_INK_SYNC_MASK = 0x1fffffff
};

static const char *const k_selectors[] = {
	"caps", "display", "frame", "gesture", "haptic", "ink",
	"latency", "scroll", "tablet-pointer", "font", "depth", nullptr
};

/* The numbers the font protocol accepts, until a terminal says what it does. */
enum { FONT_WMIN = 100, FONT_WMAX = 900, FONT_SMIN = -1500, FONT_SMAX = 1500 };

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
	    "\033]777;tablet-pointer;v=1:op=query:id=17349531\033\\"
	    "\033]777;font;v=1:op=query:id=17349532\033\\"
	    "\033]7501;?\033\\");
}

__attribute__((weak)) void
dv_host_passthrough(struct dv_session *, const char *, size_t)
{
}

__attribute__((weak)) struct dv_peer *
dv_host_first_peer(void)
{
	return (nullptr);
}

__attribute__((weak)) void
dv_host_send_peer(struct dv_peer *, const char *, size_t)
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
	std::string		 buf;
	int			 n = 0;
	std::vector<char *>	 key;
	std::vector<char *>	 val;
};

static std::string
strf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static std::string
strf(const char *fmt, ...)
{
	va_list		ap, again;
	std::string	out;
	int		n;

	va_start(ap, fmt);
	va_copy(again, ap);
	n = vsnprintf(nullptr, 0, fmt, ap);
	va_end(ap);
	if (n > 0) {
		out.resize((size_t)n + 1);
		vsnprintf(out.data(), out.size(), fmt, again);
		out.resize((size_t)n);
	}
	va_end(again);
	return (out);
}

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
parse_fields(struct silk_f *f, const char *s, size_t n)
{
	char				*p, *end, *eq;
	std::vector<std::string_view>	 seen;

	if (n == 0 || memchr(s, '\0', n) != nullptr)
		return (0);
	f->buf.assign(s, n);
	f->n = 0;
	f->key.clear();
	f->val.clear();
	p = f->buf.data();
	for (;;) {
		end = strchr(p, ':');
		if (end != nullptr)
			*end = '\0';
		eq = strchr(p, '=');
		if (eq == nullptr || eq == p)
			return (0);
		*eq = '\0';
		f->key.push_back(p);
		f->val.push_back(eq + 1);
		seen.emplace_back(p);
		f->n++;
		if (end == nullptr)
			break;
		p = end + 1;
	}
	/* A frame's length is the program's to choose, so this is not a scan
	 * of the keys for every key. */
	std::sort(seen.begin(), seen.end());
	return (std::adjacent_find(seen.begin(), seen.end()) == seen.end());
}

/* The fields back as key=value:key=value. */
static std::string
fields_join(const struct silk_f *f)
{
	std::string	out;

	for (int i = 0; i < f->n; i++) {
		if (i != 0)
			out += ':';
		out += f->key[i];
		out += '=';
		out += f->val[i];
	}
	return (out);
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
 * -1 a known selector whose frame is rejected. sel holds the longest name
 * in k_selectors; a longer one is not ours. */
static int
open_osc(const char *s, size_t n, char *sel, size_t selcap, struct silk_f *f)
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
	if (!parse_fields(f, b + i + 1, bn - i - 1))
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
pty_fields(struct dv_window *w, const char *sel, const std::string &fields)
{
	std::string	buf;

	if (w == nullptr || w->pty_fd < 0)
		return;
	buf = strf("\033]777;%s;", sel) + fields + "\033\\";
	dv_window_pty_send(w, buf.data(), buf.size());
}

static void
host_fields(struct dv_session *s, const char *sel, const std::string &fields)
{
	std::string	buf;

	if (s == nullptr)
		return;
	buf = strf("\033]777;%s;", sel) + fields + "\033\\";
	dv_host_passthrough(s, buf.data(), buf.size());
}

/* ---- font styles ---------------------------------------------------- */

/*
 * Every distinct role, weight and slant any program has selected, so a cell
 * holds a number. Id 0 is the configured face. The table outlives the cells,
 * and grows only with what programs select, not with how much they write.
 */
struct font_table {
	std::vector<struct dv_font_style>		 styles;
	std::unordered_map<std::string, uint32_t>	 index;
};

static struct font_table &
fonts(void)
{
	static struct font_table	t = [] {
		struct font_table	n;

		n.styles.push_back({ "default", 0, DV_FONT_SLANT_AUTO });
		return (n);
	}();

	return (t);
}

static std::string
font_key(const char *role, int weight, int slant)
{
	return (strf("%d %d %s", weight, slant, role));
}

uint32_t
dv_font_intern(const char *role, int weight, int slant)
{
	struct font_table	&t = fonts();
	std::string		 key;

	if (weight == 0 && slant == DV_FONT_SLANT_AUTO &&
	    strcmp(role, "default") == 0)
		return (0);
	key = font_key(role, weight, slant);
	auto it = t.index.find(key);
	if (it != t.index.end())
		return (it->second);
	if (t.styles.size() > UINT32_MAX)
		dv_fatal("font style space exhausted");
	t.styles.push_back({ role, weight, slant });
	t.index.emplace(key, (uint32_t)(t.styles.size() - 1));
	return ((uint32_t)(t.styles.size() - 1));
}

const struct dv_font_style *
dv_font_get(uint32_t id)
{
	struct font_table	&t = fonts();

	return (&t.styles[id < t.styles.size() ? id : 0]);
}

/* What takes the terminal to this style. SGR is the compositor's. */
void
dv_font_select(uint32_t font, struct dv_buf *out)
{
	const struct dv_font_style	*st = dv_font_get(font);
	std::string			 b;

	if (font == 0)
		b = "\033]777;font;v=1:op=reset\033\\";
	else {
		b = strf("\033]777;font;v=1:op=select:role=%s:weight=%d",
		    st->role.c_str(), st->weight);
		if (st->slant == DV_FONT_SLANT_AUTO)
			b += ":slant=auto";
		else
			b += strf(":slant=%d", st->slant);
		b += "\033\\";
	}
	dv_buf_append(out, b.data(), b.size());
}

/* The table for a state handed to another server: cells keep their ids. */
void
dv_font_save(struct dv_buf *out)
{
	struct font_table	&t = fonts();

	for (size_t i = 1; i < t.styles.size(); i++) {
		std::string	l = strf("fstyle %zu %d %d %s\n", i, t.styles[i].weight,
		    t.styles[i].slant, t.styles[i].role.c_str());

		dv_buf_append(out, l.data(), l.size());
	}
}

void
dv_font_restore(uint32_t id, const char *role, int weight, int slant)
{
	struct font_table	&t = fonts();

	if (id == 0)
		return;
	if (id >= t.styles.size())
		t.styles.resize((size_t)id + 1, { "default", 0, DV_FONT_SLANT_AUTO });
	t.styles[id] = { role, weight, slant };
	t.index[font_key(role, weight, slant)] = id;
}

/* SGR 10 is the default role and 11-19 the first nine configured ones; an
 * unassigned slot is the default. Weight and slant stay. */
uint32_t
dv_font_slot(const struct dv_session *s, uint32_t font, int sgr)
{
	const struct dv_font_style	*cur = dv_font_get(font);
	std::string			 role = "default", tok;
	int				 weight = cur->weight, slant = cur->slant;

	if (sgr > 10 && s != nullptr && s->silk_font_roles != nullptr) {
		const char	*p = s->silk_font_roles;
		int		 want = sgr - 10;

		for (;;) {
			const char	*end = strchr(p, ',');

			tok.assign(p, end != nullptr ? (size_t)(end - p) : strlen(p));
			if (tok != "default" && !tok.empty() && --want == 0) {
				role = tok;
				break;
			}
			if (end == nullptr)
				break;
			p = end + 1;
		}
	}
	return (dv_font_intern(role.c_str(), weight, slant));
}

/* ---- stereo depth --------------------------------------------------- */

/*
 * Every distinct depth and vertical phase a program has asked for, so a
 * cell holds a number, as for font styles. Id 0 is the terminal plane.
 * Floats are compared and saved by their bits, which is exact.
 */
struct depth_table {
	std::vector<struct dv_depth_style>		 styles;
	std::unordered_map<std::string, uint32_t>	 index;
};

static struct depth_table &
depths(void)
{
	static struct depth_table	t = [] {
		struct depth_table	n;

		n.styles.push_back({ DV_DEPTH_FLAT, 0, 0.0f, 0.0f });
		return (n);
	}();

	return (t);
}

static uint32_t
f32_bits(float f)
{
	uint32_t	u;

	memcpy(&u, &f, sizeof u);
	return (u);
}

static float
f32_from(uint32_t u)
{
	float	f;

	memcpy(&f, &u, sizeof f);
	return (f);
}

/* One spelling per meaning: no depth has no amount, and -0 is 0. */
static struct dv_depth_style
depth_norm(struct dv_depth_style st)
{
	if (st.kind == DV_DEPTH_DEG && st.d == 0)
		st.kind = DV_DEPTH_FLAT;
	if (st.kind == DV_DEPTH_PX && st.p == 0.0f)
		st.kind = DV_DEPTH_FLAT;
	if (st.kind != DV_DEPTH_DEG)
		st.d = 0;
	if (st.kind != DV_DEPTH_PX)
		st.p = 0.0f;
	if (st.y == 0.0f)
		st.y = 0.0f;
	return (st);
}

static std::string
depth_key(const struct dv_depth_style *st)
{
	return (strf("%d %d %08x %08x", st->kind, st->d, f32_bits(st->p),
	    f32_bits(st->y)));
}

uint32_t
dv_depth_intern(const struct dv_depth_style *in)
{
	struct depth_table	&t = depths();
	struct dv_depth_style	 st = depth_norm(*in);
	std::string		 key;

	if (st.kind == DV_DEPTH_FLAT && st.y == 0.0f)
		return (0);
	key = depth_key(&st);
	auto it = t.index.find(key);
	if (it != t.index.end())
		return (it->second);
	if (t.styles.size() > UINT32_MAX)
		dv_fatal("depth style space exhausted");
	t.styles.push_back(st);
	t.index.emplace(key, (uint32_t)(t.styles.size() - 1));
	return ((uint32_t)(t.styles.size() - 1));
}

const struct dv_depth_style *
dv_depth_get(uint32_t id)
{
	struct depth_table	&t = depths();

	return (&t.styles[id < t.styles.size() ? id : 0]);
}

/*
 * What takes the terminal to this depth. Both attributes are always named,
 * so whatever the terminal had before does not matter. %.9g gives back the
 * same float when the terminal parses it.
 */
void
dv_depth_select(uint32_t depth, struct dv_buf *out)
{
	const struct dv_depth_style	*st = dv_depth_get(depth);
	std::string			 b;

	if (depth == 0)
		b = "\033]777;depth;v=1:op=reset\033\\";
	else {
		b = "\033]777;depth;v=1:op=set";
		if (st->kind == DV_DEPTH_PX)
			b += strf(":p=%.9g", (double)st->p);
		else
			b += strf(":d=%d", st->kind == DV_DEPTH_DEG ? st->d : 0);
		b += strf(":y=%.9g", (double)st->y);
		b += "\033\\";
	}
	dv_buf_append(out, b.data(), b.size());
}

/* The table for a state handed to another server: cells keep their ids. */
void
dv_depth_save(struct dv_buf *out)
{
	struct depth_table	&t = depths();

	for (size_t i = 1; i < t.styles.size(); i++) {
		const struct dv_depth_style	*st = &t.styles[i];
		std::string			 l = strf("dstyle %zu %d %d %08x %08x\n",
		    i, st->kind, st->d, f32_bits(st->p), f32_bits(st->y));

		dv_buf_append(out, l.data(), l.size());
	}
}

/* One "dstyle" line's value. 0 when it is not one dv_depth_save wrote. */
int
dv_depth_restore(const char *line)
{
	struct depth_table	&t = depths();
	struct dv_depth_style	 st;
	unsigned long		 id;
	int			 kind, d;
	unsigned		 pb, yb;

	if (sscanf(line, "%lu %d %d %x %x", &id, &kind, &d, &pb, &yb) != 5 ||
	    id == 0 || id > UINT32_MAX || kind < DV_DEPTH_FLAT ||
	    kind > DV_DEPTH_PX)
		return (0);
	st.kind = kind;
	st.d = d;
	st.p = f32_from(pb);
	st.y = f32_from(yb);
	if (!isfinite(st.p) || !isfinite(st.y))
		return (0);
	st = depth_norm(st);
	if (id >= t.styles.size())
		t.styles.resize((size_t)id + 1, { DV_DEPTH_FLAT, 0, 0.0f, 0.0f });
	t.styles[id] = st;
	t.index[depth_key(&st)] = (uint32_t)id;
	return (1);
}

/*
 * A finite decimal that a 32-bit float can hold: an optional sign, digits
 * with an optional fraction, an optional exponent. Rejected when it
 * overflows a float or a nonzero value would round to zero.
 */
static int
parse_f32(const char *val, float *ans)
{
	const char	*p = val;
	char		*end;
	int		 digits = 0, nonzero = 0;
	double		 v;

	if (val == nullptr)
		return (0);
	if (*p == '-' || *p == '+')
		p++;
	for (; *p >= '0' && *p <= '9'; p++, digits++)
		nonzero |= *p != '0';
	if (*p == '.')
		for (p++; *p >= '0' && *p <= '9'; p++, digits++)
			nonzero |= *p != '0';
	if (digits == 0)
		return (0);
	if (*p == 'e' || *p == 'E') {
		p++;
		if (*p == '-' || *p == '+')
			p++;
		if (*p < '0' || *p > '9')
			return (0);
		while (*p >= '0' && *p <= '9')
			p++;
	}
	if (*p != '\0')
		return (0);
	v = strtod(val, &end);
	if (*end != '\0' || !isfinite(v) || fabs(v) > (double)FLT_MAX)
		return (0);
	if (nonzero && (float)v == 0.0f)
		return (0);
	*ans = (float)v;
	return (1);
}

int
dv_parse_px(const char *val, float *ans)
{
	return (parse_f32(val, ans));
}

/*
 * Silkie measures angular depth against the eye's horizontal field of view
 * and reference pixels against a 1920-pixel eye. The user can change the
 * field of view and nothing reports it, so moving an angular depth by
 * pixels assumes Silkie's default, 52 degrees: 52000/1920 millidegrees a
 * reference pixel. Pixel and flat depths move exactly.
 */
#define DEPTH_MDEG_PER_PX	(52000.0 / 1920.0)

/* This depth moved px reference pixels toward the viewer: negative
 * disparity, in front. Vertical phase stays. */
uint32_t
dv_depth_nearer(uint32_t depth, float px)
{
	struct dv_depth_style	st = *dv_depth_get(depth);

	if (px == 0.0f || !isfinite(px))
		return (depth);
	if (st.kind == DV_DEPTH_DEG) {
		double	d = (double)st.d - (double)px * DEPTH_MDEG_PER_PX;

		d = nearbyint(d);
		st.d = d < (double)INT32_MIN ? INT32_MIN :
		    d > (double)INT32_MAX ? INT32_MAX : (int32_t)d;
	} else {
		double	p = (st.kind == DV_DEPTH_PX ? (double)st.p : 0.0) -
		    (double)px;

		st.kind = DV_DEPTH_PX;
		st.p = p < -(double)FLT_MAX ? -FLT_MAX :
		    p > (double)FLT_MAX ? FLT_MAX : (float)p;
	}
	return (dv_depth_intern(&st));
}

/*
 * op=set, validated as a whole: d and p together, or any malformed value,
 * change nothing. Each given attribute replaces the pen's; the rest stay.
 */
static void
depth_set(struct dv_window *w, const struct silk_f *f)
{
	struct dv_depth_style	 st = *dv_depth_get(w->screen.depth);
	const char		*vd = ffield(f, "d");
	const char		*vp = ffield(f, "p");
	const char		*vy = ffield(f, "y");
	long long		 d;
	float			 v;

	if (vd != nullptr && vp != nullptr)
		return;
	if (vd != nullptr) {
		if (!parse_i64(vd, INT32_MIN, INT32_MAX, &d))
			return;
		st.kind = DV_DEPTH_DEG;
		st.d = (int32_t)d;
		st.p = 0.0f;
	}
	if (vp != nullptr) {
		if (!parse_f32(vp, &v))
			return;
		st.kind = DV_DEPTH_PX;
		st.p = v;
		st.d = 0;
	}
	if (vy != nullptr) {
		if (!parse_f32(vy, &v))
			return;
		st.y = v;
	}
	w->screen.depth = dv_depth_intern(&st);
}

/* The headroom the display reported, for XTGETTCAP. 0 when nothing did. */
int
dv_silkie_headroom(const struct dv_session *s, char *out, size_t cap)
{
	const char	*p;
	size_t		 n;

	if (!offered(s, "display") || s->silk_display == nullptr)
		return (0);
	for (p = s->silk_display; p != nullptr; p = strchr(p, ':')) {
		if (*p == ':')
			p++;
		if (strncmp(p, "headroom=", 9) == 0)
			break;
	}
	if (p == nullptr)
		return (0);
	p += 9;
	n = strcspn(p, ":");
	/* The field is 0 where the display did not say: there is none then. */
	if (n == 0 || n >= cap || (n == 1 && p[0] == '0'))
		dv_strlcpy(out, "1", cap);
	else {
		memcpy(out, p, n);
		out[n] = '\0';
	}
	return (1);
}

void
dv_silkie_free_session(struct dv_session *s)
{
	free(s->silk_ext);
	free(s->silk_media);
	free(s->silk_format);
	free(s->silk_display);
	free(s->silk_font_roles);
	delete s->silk_pad_btn;
}

void
dv_silkie_free_peer(struct dv_peer *p)
{
	free(p->silkie_blob);
	delete p->silk_hit;
	delete p->silk_desc;
	delete p->silk_ink;
}

void
dv_silkie_free_window(struct dv_window *w)
{
	delete w->silk_live;
	w->silk_live = nullptr;
	delete w->silk_strokes;
	w->silk_strokes = nullptr;
}

/* ---- capability blob ---------------------------------------------- */

static int
blob_get(const char *blob, const char *key, std::string *out)
{
	size_t		kl = strlen(key);
	const char	*p = blob ? blob : "";

	out->clear();
	while (*p != '\0') {
		const char	*nl = strchr(p, '\n');
		size_t		 n = nl != nullptr ? (size_t)(nl - p) : strlen(p);

		if (n > kl + 1 && strncmp(p, key, kl) == 0 && p[kl] == '=') {
			out->assign(p + kl + 1, n - kl - 1);
			return (1);
		}
		if (nl == nullptr)
			break;
		p = nl + 1;
	}
	return (0);
}

int
dv_silkie_peer_has(const struct dv_peer *p, const char *key)
{
	std::string	v;

	return (p != nullptr && blob_get(p->silkie_blob, key, &v) && v != "0");
}

static void
blob_set(char **blob, const char *key, const char *val)
{
	std::string	 next;
	size_t		 kl = strlen(key);
	const char	*p = *blob != nullptr ? *blob : "";

	while (*p != '\0') {
		const char	*nl = strchr(p, '\n');
		size_t		 n = nl != nullptr ? (size_t)(nl - p) : strlen(p);
		int		 drop = n > kl && strncmp(p, key, kl) == 0 &&
		    p[kl] == '=';

		if (!drop) {
			next.append(p, n);
			next += '\n';
		}
		if (nl == nullptr)
			break;
		p = nl + 1;
	}
	if (val != nullptr && val[0] != '\0') {
		next += key;
		next += '=';
		next += val;
		next += '\n';
	}
	dv_strset(blob, next.c_str());
}

static void
ext_add(std::string *ext, const char *sel)
{
	if (ext_has(ext->c_str(), sel))
		return;
	if (!ext->empty())
		*ext += ',';
	*ext += sel;
	*ext += ".1";
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
csv_intersect(std::string *acc, const char *list)
{
	std::string	 out, tok;
	const char	*p = acc->c_str();

	for (;;) {
		const char	*end = strchr(p, ',');

		tok.assign(p, end != nullptr ? (size_t)(end - p) : strlen(p));
		if (!tok.empty() && csv_has(list, tok.c_str())) {
			if (!out.empty())
				out += ',';
			out += tok;
		}
		if (end == nullptr)
			break;
		p = end + 1;
	}
	*acc = out;
}

/* Append what list has that acc does not, keeping acc's order first. */
static void
csv_union(std::string *acc, const char *list)
{
	std::string	 tok;
	const char	*p = list;

	for (;;) {
		const char	*end = strchr(p, ',');

		tok.assign(p, end != nullptr ? (size_t)(end - p) : strlen(p));
		if (!tok.empty() && !csv_has(acc->c_str(), tok.c_str())) {
			if (!acc->empty())
				*acc += ',';
			*acc += tok;
		}
		if (end == nullptr)
			break;
		p = end + 1;
	}
}

static int
peer_gfx(const struct dv_peer *p, const char *blob)
{
	std::string	tmp;

	if (p->features & DV_IDENT_KITTY_GFX)
		return (1);
	if (blob_get(blob, "format", &tmp))
		return (1);
	if (blob_get(blob, "media", &tmp))
		return (1);
	return (0);
}

/*
 * A display reply's color model and channel widths. An older terminal's
 * bits applies to each of its channels; widths it did not give are 0.
 */
static void
display_profile(std::string line, int *color, int bits[4])
{
	struct silk_f	 f;
	const char	*c;
	long long	 v, all = 0;
	static const char *const keys[] = {
		"red_bits", "green_bits", "blue_bits", "gray_bits"
	};

	*color = 0;
	bits[0] = bits[1] = bits[2] = bits[3] = 0;
	if (!parse_fields(&f, line.data(), line.size()))
		return;
	c = ffield(&f, "color");
	if (c != nullptr && strcmp(c, "gray") == 0)
		*color = 1;
	else if (c != nullptr && strcmp(c, "mono") == 0)
		*color = 2;
	if (!ffield_int(&f, "bits", 0, 64, &all))
		all = 0;
	for (int i = 0; i < 4; i++) {
		v = 0;
		if (!ffield_int(&f, keys[i], 0, 64, &v))
			v = 0;
		if (v == 0 && (i == 3) == (*color != 0))
			v = all;
		bits[i] = (int)v;
	}
}

/*
 * One display reply for a session shown on several: what the best of them
 * can show, attribute by attribute, since each terminal is sent what its own
 * display takes (the bits and the gamut of a picture are cut down for it in
 * kitty.cc, a colour's gamut in compose.cc). The colour model, the gamut,
 * the headroom and the channel widths are each the most any of them has.
 * What describes one physical display rather than a capability -- its kind,
 * scale, refresh period, density -- stays that of the display leading in
 * those, in that order.
 */
typedef std::vector<std::pair<std::string, std::string>> disp_fields;

static disp_fields
disp_split(const std::string &line)
{
	disp_fields	out;
	size_t		at = 0;

	while (at <= line.size()) {
		size_t		end = line.find(':', at);
		std::string	tok = line.substr(at, end == std::string::npos ?
				    std::string::npos : end - at);
		size_t		eq = tok.find('=');

		if (!tok.empty())
			out.push_back({ tok.substr(0, eq), eq == std::string::npos ?
			    "" : tok.substr(eq + 1) });
		if (end == std::string::npos)
			break;
		at = end + 1;
	}
	return (out);
}

static std::string
disp_get(const disp_fields &f, const char *key)
{
	for (const auto &[k, v] : f)
		if (k == key)
			return (v);
	return ("");
}

static void
disp_put(disp_fields *f, const char *key, const std::string &val)
{
	for (auto &[k, v] : *f)
		if (k == key) {
			v = val;
			return;
		}
	f->push_back({ key, val });
}

static int
disp_color_rank(const disp_fields &f)
{
	std::string	c = disp_get(f, "color");

	return (c == "rgb" ? 3 : c == "gray" ? 2 : c == "mono" ? 1 : 0);
}

static int
disp_gamut_rank(const disp_fields &f)
{
	std::string	g = disp_get(f, "gamut");

	return (g == "rec2020" ? 3 : g == "p3" ? 2 : g == "srgb" ? 1 : 0);
}

static double
disp_num(const disp_fields &f, const char *key)
{
	std::string	v = disp_get(f, key);
	char		*end;
	double		 d = strtod(v.c_str(), &end);

	return (v.empty() || *end != '\0' || !(d >= 0) ? 0 : d);
}

static std::string
display_merge(const std::string &one, const std::string &two)
{
	static const char *const widths[] = {
		"bits", "red_bits", "green_bits", "blue_bits", "gray_bits",
		"output_bits"
	};
	disp_fields	a = disp_split(one), b = disp_split(two), *lead = &a, *rest = &b;
	int		ca = disp_color_rank(a), cb = disp_color_rank(b);
	int		ga = disp_gamut_rank(a), gb = disp_gamut_rank(b);
	double		ha = disp_num(a, "headroom"), hb = disp_num(b, "headroom");
	std::string	out;

	if (cb > ca || (cb == ca && (gb > ga || (gb == ga && hb > ha)))) {
		lead = &b;
		rest = &a;
	}
	if (disp_gamut_rank(*rest) > disp_gamut_rank(*lead))
		disp_put(lead, "gamut", disp_get(*rest, "gamut"));
	if (disp_num(*rest, "headroom") > disp_num(*lead, "headroom"))
		disp_put(lead, "headroom", disp_get(*rest, "headroom"));
	/* Widths compare only between displays of one colour model. */
	if (ca == cb)
		for (const char *k : widths)
			if (disp_num(*rest, k) > disp_num(*lead, k))
				disp_put(lead, k, disp_get(*rest, k));
	for (const auto &[k, v] : *lead) {
		if (!out.empty())
			out += ':';
		out += k + "=" + v;
	}
	return (out);
}

/* Ask a client's terminal about its display again: a resize is how the
 * client says something about it may have changed. */
void
dv_silkie_requery_display(struct dv_peer *p)
{
	std::string	line, q;

	if (p == nullptr || !p->display ||
	    !blob_get(p->silkie_blob, "ext", &line) ||
	    !ext_has(line.c_str(), "display"))
		return;
	q = strf("\033]777;display;v=1:op=query:id=%d\033\\", Q_DISPLAY);
	dv_host_send_peer(p, q.data(), q.size());
}

void
dv_silkie_recompute(struct dv_session *s, struct dv_peer *first)
{
	struct dv_peer	*p;
	std::string	 ext, format, media, display, roles, line;
	std::string	 prev_disp;
	int		 have_fmt = 0, saw_gfx = 0, wg = 0, any_vector = 0;
	int		 frame_caps = 0, frame_period = 0, gesture = 0;
	int		 haptic = 0, ink = 0, lat_caps = 0, lat_in = 0, lat_re = 0;
	int		 scroll = 0, tablet = 0;
	int		 font_caps = 0, font_synth = 1, saw_font = 0;
	int		 wmin = FONT_WMIN, wmax = FONT_WMAX;
	int		 smin = FONT_SMIN, smax = FONT_SMAX;

	if (s == nullptr)
		return;
	if (s->silk_display != nullptr)
		prev_disp = s->silk_display;
	for (p = first; p != nullptr; p = TAILQ_NEXT(p, entry)) {
		int	peer_font = 0, peer_vector = 0, peer_depth = 0;
		int	peer_packed = 0, peer_wg = 0, peer_hdr = 0;

		if (!p->display || p->session != s)
			continue;
		if (blob_get(p->silkie_blob, "ext", &line)) {
			char	*tok, *save;

			for (tok = strtok_r(line.data(), ",", &save); tok != nullptr;
			    tok = strtok_r(nullptr, ",", &save)) {
				char		*dot = strchr(tok, '.');
				std::string	 name;

				if (dot == nullptr || strcmp(dot + 1, "1") != 0)
					continue;
				name.assign(tok, (size_t)(dot - tok));
				if (known_sel(name.c_str()))
					ext_add(&ext, name.c_str());
				if (name == "font")
					peer_font = 1;
				if (name == "depth")
					peer_depth = 1;
			}
		}
		if (blob_get(p->silkie_blob, "display", &line))
			display = display.empty() ? line : display_merge(display, line);
		if (blob_get(p->silkie_blob, "frame", &line)) {
			int	c = 0, per = 0;

			sscanf(line.c_str(), "%d %d", &c, &per);
			frame_caps |= c;
			if (per != 0)
				frame_period = per;
		}
		if (blob_get(p->silkie_blob, "gesture", &line))
			gesture |= atoi(line.c_str());
		if (blob_get(p->silkie_blob, "haptic", &line))
			haptic |= atoi(line.c_str());
		if (blob_get(p->silkie_blob, "ink", &line))
			ink |= atoi(line.c_str());
		if (blob_get(p->silkie_blob, "latency", &line)) {
			int	c = 0, in = 0, re = 0;

			sscanf(line.c_str(), "%d %d %d", &c, &in, &re);
			lat_caps |= c;
			if (in != 0)
				lat_in = in;
			if (re != 0)
				lat_re = re;
		}
		if (blob_get(p->silkie_blob, "scroll", &line))
			scroll |= atoi(line.c_str());
		if (blob_get(p->silkie_blob, "tablet", &line))
			tablet |= atoi(line.c_str());
		if (peer_font) {
			struct silk_f	 ff;
			long long	 v;
			const char	*r = "default";

			saw_font = 1;
			font_caps |= 1;
			if (blob_get(p->silkie_blob, "font", &line) &&
			    parse_fields(&ff, line.data(), line.size())) {
				v = 1;
				(void)ffield_int(&ff, "caps", 0, 2147483647ll, &v);
				font_caps = (font_caps & ~1) | (int)v;
				v = wmin;
				if (ffield_int(&ff, "weight_min", 0, 100000, &v) && v > wmin)
					wmin = (int)v;
				v = wmax;
				if (ffield_int(&ff, "weight_max", 0, 100000, &v) && v < wmax)
					wmax = (int)v;
				v = smin;
				if (ffield_int(&ff, "slant_min", -100000, 100000, &v) && v > smin)
					smin = (int)v;
				v = smax;
				if (ffield_int(&ff, "slant_max", -100000, 100000, &v) && v < smax)
					smax = (int)v;
				v = 0;
				(void)ffield_int(&ff, "synthetic_slant", 0, 1, &v);
				if (v == 0)
					font_synth = 0;
				if (ffield(&ff, "roles") != nullptr)
					r = ffield(&ff, "roles");
			} else
				font_synth = 0;
			csv_union(&roles, r);
		}
		if (peer_gfx(p, p->silkie_blob)) {
			std::string	f, m;
			int		has_f, has_m;

			saw_gfx = 1;
			has_f = blob_get(p->silkie_blob, "format", &f);
			has_m = blob_get(p->silkie_blob, "media", &m);
			/*
			 * A kitty peer with no Silkie list does not understand
			 * G=. One that does is enough for programs to use it:
			 * the others are sent those pixels as sRGB.
			 */
			peer_wg = has_f || has_m;
			wg |= peer_wg;
			if (!has_f)
				f = "24,32,100";
			if (!has_m)
				m = "d,f,t,s";
			if (!have_fmt) {
				format = f;
				media = m;
				have_fmt = 1;
			} else {
				csv_intersect(&format, f.c_str());
				csv_intersect(&media, m.c_str());
			}
			peer_vector = csv_has(f.c_str(), "200");
			peer_hdr = csv_has(f.c_str(), "64");
			any_vector |= peer_vector;
			peer_packed = dv_kitty_fmt_list(f.c_str());
		}
		if (peer_font != p->view.font) {
			p->view.font = peer_font;
			p->view.full = 1;	/* the styles it has not been shown */
			s->dirty = 1;
		}
		{
			/*
			 * The display it reported, read again on every resize:
			 * images are packed for it, so another one sends them
			 * again.
			 */
			int	color = 0, bits[4] = { 0, 0, 0, 0 };

			if (blob_get(p->silkie_blob, "display", &line))
				display_profile(line, &color, bits);
			if (color != p->view.disp_color ||
			    memcmp(bits, p->view.disp_bits, sizeof bits) != 0) {
				p->view.disp_color = color;
				memcpy(p->view.disp_bits, bits, sizeof bits);
				if (p->view.kimg_gen != nullptr)
					p->view.kimg_gen->clear();
				if (p->view.kres_gen != nullptr)
					p->view.kres_gen->clear();
				p->view.full = 1;
				s->dirty = 1;
			}
		}
		if (peer_wg != p->view.wg) {
			/* Its pictures and colours were made for the other. */
			p->view.wg = peer_wg;
			if (p->view.kimg_gen != nullptr)
				p->view.kimg_gen->clear();
			if (p->view.kres_gen != nullptr)
				p->view.kres_gen->clear();
			p->view.full = 1;
			s->dirty = 1;
		}
		{
			/* Sixteen bit colours (SGR 38:0), for a terminal that said so. */
			int	peer_deep = blob_get(p->silkie_blob, "sgr", &line) &&
			    csv_has(line.c_str(), "deep");

			if (peer_deep != p->view.deep) {
				p->view.deep = peer_deep;
				p->view.full = 1;
				s->dirty = 1;
			}
		}
		if (peer_depth != p->view.depth) {
			p->view.depth = peer_depth;
			p->view.full = 1;	/* the depths it has not been shown */
			s->dirty = 1;
		}
		if (peer_vector != p->view.vector || peer_hdr != p->view.hdr) {
			/* What it holds was made for a terminal that drew
			 * scenes itself, or did not: made again for this one. */
			p->view.vector = peer_vector;
			p->view.hdr = peer_hdr;
			if (p->view.kimg_gen != nullptr)
				p->view.kimg_gen->clear();
			if (p->view.kres_gen != nullptr)
				p->view.kres_gen->clear();
			p->view.full = 1;
			s->dirty = 1;
		}
		if (peer_packed != p->view.packed) {
			/* What it holds was packed for the formats it took. */
			p->view.packed = peer_packed;
			if (p->view.kimg_gen != nullptr)
				p->view.kimg_gen->clear();
			if (p->view.kres_gen != nullptr)
				p->view.kres_gen->clear();
			p->view.full = 1;
			s->dirty = 1;
		}
	}
	if (!ext.empty())
		ext_add(&ext, "caps");
	/*
	 * Vector scenes and HDR pixels are not left to the intersection: a
	 * terminal that takes neither is sent the one drawn and the other
	 * brought down to eight bits (kitty.cc), so programs may always use
	 * them where pictures show at all.
	 */
	if (have_fmt)
		for (const char *always : { "64", "200" })
			if (!csv_has(format.c_str(), always))
				format += std::string(format.empty() ? "" : ",") + always;
	dv_strset(&s->silk_ext, ext.c_str());
	dv_strset(&s->silk_format, format.c_str());
	dv_strset(&s->silk_media, media.c_str());
	dv_strset(&s->silk_display, display.c_str());
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
	s->silk_any = !ext.empty();
	s->silk_wg = saw_gfx && wg;
	s->silk_vector = any_vector;
	if (saw_font) {
		/* A detached session keeps the last list: its programs go on
		 * using SGR 11-19 with no terminal to ask. */
		dv_strset(&s->silk_font_roles, roles.c_str());
		s->silk_font_caps = font_caps;
		s->silk_font_synth = font_synth;
		s->silk_font_wmin = wmin;
		s->silk_font_wmax = wmax;
		s->silk_font_smin = smin;
		s->silk_font_smax = smax;
	}
	if (prev_disp != display) {
		struct dv_window	*w;

		TAILQ_FOREACH(w, &s->desktop.windows, entry) {
			if (!w->screen.silk_display)
				continue;
			dv_strset(&w->screen.silk_shown, nullptr);
		}
	}
}

static void
note_line(struct dv_host *h, const char *key, const char *val)
{
	blob_set(&h->silkie_blob, key, val);
}

/* What a cap reply of the font selector offers, for the blob. */
static std::string
font_blob(const struct silk_f *f)
{
	static const char *keys[] = {
		"caps", "roles", "weight_min", "weight_max", "slant_min",
		"slant_max", "synthetic_slant", nullptr
	};
	std::string	out;

	for (int i = 0; keys[i] != nullptr; i++) {
		const char	*v = ffield(f, keys[i]);

		if (v == nullptr)
			continue;
		if (!out.empty())
			out += ':';
		out += keys[i];
		out += '=';
		out += v;
	}
	return (out);
}

static std::string
display_body(const struct silk_f *f)
{
	static const char *keys[] = {
		"kind", "color", "bits", "gamut", "headroom", "scale",
		"period", "ppi", "red_bits", "green_bits", "blue_bits",
		"gray_bits", "precision", "output_bits", "output_type",
		"output_dither", nullptr
	};
	std::string	out;

	for (int i = 0; keys[i] != nullptr; i++) {
		const char	*v = ffield(f, keys[i]);

		if (v == nullptr)
			continue;
		if (!out.empty())
			out += ':';
		out += keys[i];
		out += '=';
		out += v;
	}
	return (out);
}

void
dv_silkie_note(struct dv_host *h, const unsigned char *osc, size_t len)
{
	char		 sel[32];
	struct silk_f	 f;
	long long	 id = -1;
	const char	*op;
	std::string	 built;

	if (h == nullptr || osc == nullptr)
		return;
	/* The program status feature query: OSC 7501 ; ? [: ...] */
	{
		const unsigned char	*b = osc;
		size_t			 bl = len;

		if (bl >= 2 && b[0] == 0x1b && b[1] == ']') {
			b += 2;
			bl -= 2;
		}
		if (bl >= 6 && memcmp(b, "7501;?", 6) == 0) {
			note_line(h, "pstatus", "1");
			return;
		}
	}
	if (open_osc((const char *)osc, len, sel, sizeof sel, &f) != 1)
		return;
	op = ffield(&f, "op");
	if (op == nullptr || strcmp(op, "cap") != 0)
		return;
	(void)ffield_int(&f, "id", 0, 2147483647ll, &id);
	if (id < Q_CAPS || id > Q_LAST)
		return;
	if (strcmp(sel, "caps") == 0) {
		const char	*e = ffield(&f, "ext");
		const char	*m = ffield(&f, "media");
		const char	*fmt = ffield(&f, "format");

		if (e != nullptr)
			note_line(h, "ext", e);
		if (ffield(&f, "sgr") != nullptr)
			note_line(h, "sgr", ffield(&f, "sgr"));
		if (m != nullptr)
			note_line(h, "media", m);
		if (fmt != nullptr)
			note_line(h, "format", fmt);
		return;
	}
	if (strcmp(sel, "display") == 0) {
		built = display_body(&f);
		if (!built.empty())
			note_line(h, "display", built.c_str());
		return;
	}
	if (strcmp(sel, "font") == 0) {
		note_line(h, "font", font_blob(&f).c_str());
		return;
	}
	if (strcmp(sel, "frame") == 0) {
		long long	caps = 0, per = 0;

		(void)ffield_int(&f, "caps", 0, 2147483647ll, &caps);
		(void)ffield_int(&f, "period", 0, 2147483647ll, &per);
		note_line(h, "frame", strf("%lld %lld", caps, per).c_str());
		return;
	}
	if (strcmp(sel, "latency") == 0) {
		long long	caps = 0, in = 0, re = 0;

		(void)ffield_int(&f, "caps", 0, 2147483647ll, &caps);
		(void)ffield_int(&f, "input", 0, 2147483647ll, &in);
		(void)ffield_int(&f, "repaint", 0, 2147483647ll, &re);
		note_line(h, "latency", strf("%lld %lld %lld", caps, in, re).c_str());
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
		if (key != nullptr)
			note_line(h, key, strf("%lld", caps).c_str());
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

	if (w->silk_live == nullptr)
		w->silk_live = new std::vector<struct dv_silk_live>;
	for (auto &L : *w->silk_live) {
		if (L.used && L.sel == sel && L.device == dev &&
		    L.contact == contact && L.kind == kind) {
			slot = &L;
			break;
		}
		if (!L.used && slot == nullptr)
			slot = &L;
	}
	if (slot == nullptr)
		slot = &w->silk_live->emplace_back();
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
	if (w == nullptr || w->silk_live == nullptr)
		return;
	for (auto &L : *w->silk_live)
		if (L.used && L.sel == sel && L.device == dev &&
		    L.contact == contact)
			L.used = 0;
}

static int	brush_num(const char *, double *);
static struct dv_window *win_id(struct dv_session *, unsigned);
static void	ink_push_peer(struct dv_peer *);
static void	brush_consume(struct dv_window *, uint32_t);
static unsigned	cell_px(const struct dv_session *, int);

/*
 * Ink: the terminal applied the records the reports describe to its copy of
 * the scene, so the same ones go into the table for every other copy, however
 * many there are and whenever they attach.
 */
static struct dv_silk_stroke *
ink_find(struct dv_window *w, const struct dv_peer *p, int dev, int contact)
{
	if (w == nullptr || w->silk_strokes == nullptr)
		return (nullptr);
	for (auto &st : *w->silk_strokes)
		if (st.peer == p && st.device == dev && st.contact == contact)
			return (&st);
	return (nullptr);
}

static void
ink_end(struct dv_window *w, const struct dv_silk_stroke *st)
{
	size_t	i = (size_t)(st - w->silk_strokes->data());

	w->silk_strokes->erase(w->silk_strokes->begin() + (std::ptrdiff_t)i);
	dv_silkie_push(w->session);	/* ink may have been kept on for it */
}

/* It ended as it should: the terminal's element becomes the program's. */
static void
ink_finish(struct dv_window *w, const struct dv_silk_stroke *st)
{
	if (st->own)
		dv_kitty_ink_settle(w, st);
	ink_end(w, st);
}

/* The stroke goes from every copy, the one its terminal drew too. */
static void
ink_withdraw(struct dv_window *w, struct dv_silk_stroke *st)
{
	if (st->own)
		dv_kitty_ink_drop(w, st->peer, st->dv_id, st->token);
	if (st->began)
		dv_kitty_ink_record(w, st, strf("x id=%u", st->id));
}

/* The point of one report, as a record: the first makes the element. */
static void
ink_point(struct dv_window *w, struct dv_silk_stroke *st, const struct silk_f *f,
    int tx, int ty)
{
	long long	 valid = 0, pressure = 65535;
	double		 x, y, width;
	std::string	 pts;

	if (st->over)
		return;
	/* The image is replaced, or the program turned ink off: a down that is
	 * reported had been drawn before anything was told to stop it. */
	if (!dv_kitty_ink_live(w, st) || (!st->down && !w->screen.silk_ink)) {
		st->over = 1;
		return;
	}
	st->down = 0;
	if (!dv_kitty_ink_point(w, st, tx, ty, &x, &y))
		return;
	(void)ffield_int(f, "valid", 0, 4294967295ll, &valid);
	if ((valid & 1) == 0 ||
	    !ffield_int(f, "p", 0, 65535, &pressure))
		pressure = 65535;
	width = st->w0 + (st->w1 - st->w0) * pow(pressure / 65535.0, st->e);
	pts = strf("pts=%.10g,%.10g,%.10g", x, y, width);
	if (st->began) {
		dv_kitty_ink_record(w, st, strf("a id=%u ", st->id) + pts);
		return;
	}
	st->began = 1;
	dv_kitty_ink_record(w, st, strf("k id=%u", st->id) +
	    (st->group != 0 ? strf(" g=%u ", st->group) : std::string(" ")) +
	    pts + " fill=" + st->fill);
}

/* A stroke on the placement a lease is for, with the look it had. */
static int
ink_begin(struct dv_peer *p, struct dv_window *w, const struct silk_f *f, int dev,
    int contact, const struct dv_silk_lease *lease, uint32_t token, uint32_t id,
    int tx, int ty)
{
	struct silk_f		 b;
	struct dv_silk_stroke	 st;
	const char		*fill;
	long long		 image = 0, group = 0;

	if (!parse_fields(&b, lease->look.data(), lease->look.size()) ||
	    !ffield_int(&b, "i", 1, 4294967295ll, &image) ||
	    !ffield_int(&b, "g", 0, 4294967295ll, &group) ||
	    (fill = ffield(&b, "fill")) == nullptr ||
	    !brush_num(ffield(&b, "w0"), &st.w0) ||
	    !brush_num(ffield(&b, "w1"), &st.w1) ||
	    !brush_num(ffield(&b, "e"), &st.e))
		return (0);
	st.peer = p;
	st.device = dev;
	st.contact = contact;
	st.id = id;
	st.token = token;
	st.group = (uint32_t)group;
	st.image = (unsigned)image;
	st.dv_id = lease->dv_id;
	st.fill = fill;
	if (!dv_kitty_ink_bind(w, &st, tx, ty))
		return (0);
	if (w->silk_strokes == nullptr)
		w->silk_strokes = new std::vector<struct dv_silk_stroke>;
	if (auto *stale = ink_find(w, p, dev, contact))
		ink_finish(w, stale);
	w->silk_strokes->push_back(std::move(st));
	ink_point(w, &w->silk_strokes->back(), f, tx, ty);
	return (1);
}

/* The next id a program set aside for the terminal: 0 when it has none. */
static uint32_t
ink_next_id(const struct dv_window *w)
{
	struct silk_f	 b;
	const char	*raw = w->screen.silk_brush;
	long long	 k = 0, n = 0;

	if (raw == nullptr || !parse_fields(&b, raw, strlen(raw)) ||
	    !ffield_int(&b, "k", 1, 4294967295ll, &k) ||
	    !ffield_int(&b, "n", 0, 4294967295ll, &n) || n <= 0)
		return (0);
	return ((uint32_t)k);
}

static void
ink_field_drop(struct silk_f *f)
{
	for (int i = 0; i < f->n; i++)
		if (strcmp(f->key[i], "ink") == 0) {
			f->key.erase(f->key.begin() + i);
			f->val.erase(f->val.begin() + i);
			f->n--;
			return;
		}
}

/*
 * A down that says the terminal is drawing. The lease says whose image it drew
 * on. It is that window's stroke if the report is for that window and its
 * program has an id left, and the report is given the id; if not, as when the
 * report raced with a move or is for a program that will not hear of the
 * contact, the ink goes and the program's report has no ink field. w is the
 * window the program of which would be told.
 */
static void
ink_claim(struct dv_peer *p, struct dv_window *w, struct silk_f *f, int dev,
    int contact, int tx, int ty, std::string *hold)
{
	struct dv_silk_ink	*ink = p->silk_ink;
	struct dv_silk_lease	 lease;
	long long		 token = 0;
	struct dv_window	*owner;
	uint32_t		 id;
	int			 found = 0;

	if (ffield(f, "ink") == nullptr)
		return;
	(void)ffield_int(f, "ink", 0, 4294967295ll, &token);
	if (ink != nullptr)
		for (auto it = ink->leases.rbegin(); it != ink->leases.rend(); ++it)
			if (token > 0 && token >= it->first &&
			    token - it->first < it->n) {
				it->used++;
				lease = *it;
				found = 1;
				break;
			}
	if (!found) {
		ink_field_drop(f);
		return;
	}
	owner = win_id(p->session, lease.window);
	id = owner != nullptr ? ink_next_id(owner) : 0;
	if (owner == w && id != 0 && ink_begin(p, w, f, dev, contact, &lease,
	    (uint32_t)token, id, tx, ty)) {
		*hold = strf("%u", id);
		for (int i = 0; i < f->n; i++)
			if (strcmp(f->key[i], "ink") == 0)
				f->val[i] = hold->data();
		brush_consume(w, id);
	} else {
		if (owner != nullptr)
			dv_kitty_ink_drop(owner, p, lease.dv_id, (uint32_t)token);
		ink_field_drop(f);
	}
	ink_push_peer(p);
}

/* The pen moves over a window: a stroke there would need its brush. */
static void
ink_hover(struct dv_peer *p, struct dv_window *w, int tx, int ty)
{
	struct dv_silk_ink	*ink = p->silk_ink;
	unsigned		 cw = cell_px(p->session, 0), ch = cell_px(p->session, 1);
	unsigned		 id = w != nullptr ? w->id : 0;
	int			 col = gp::cell_at(tx, cw), row = gp::cell_at(ty, ch);

	if (ink == nullptr)
		ink = p->silk_ink = new struct dv_silk_ink;
	if (ink->hover_win == id && ink->hover_col == col && ink->hover_row == row)
		return;
	ink->hover_win = id;
	ink->hover_x = tx;
	ink->hover_y = ty;
	ink->hover_col = col;
	ink->hover_row = row;
	ink_push_peer(p);
}

/* The stroke the terminal was drawing goes from every copy, its own too. */
static void
ink_cancel_window(struct dv_window *w)
{
	if (w->silk_strokes == nullptr || w->silk_strokes->empty())
		return;
	for (auto &st : *w->silk_strokes)
		ink_withdraw(w, &st);
	w->silk_strokes->clear();
	dv_silkie_push(w->session);
}

/* What it drew stays. */
void
dv_silkie_ink_peer_gone(struct dv_peer *p)
{
	struct dv_window	*w;

	if (p->session == nullptr)
		return;
	TAILQ_FOREACH(w, &p->session->desktop.windows, entry)
		if (w->silk_strokes != nullptr)
			std::erase_if(*w->silk_strokes,
			    [p](const struct dv_silk_stroke &st) {
				return (st.peer == p);
			});
	delete p->silk_ink;
	p->silk_ink = nullptr;
}

/* A terminal sent the image again ends its stroke there: it is told the rest. */
void
dv_silkie_ink_resent(struct dv_window *w, const struct dv_view *v, unsigned dv_id)
{
	if (w->silk_strokes == nullptr)
		return;
	for (auto &st : *w->silk_strokes)
		if (st.dv_id == dv_id && &st.peer->view == v)
			st.own = 0;
}

/* Report every key of the pad that is down as released, to the window that
 * had them. */
static void
pad_release(struct dv_session *s, struct dv_window *w)
{
	if (s->silk_pad_btn != nullptr) {
		for (int button : *s->silk_pad_btn)
			pty_fields(w, "tablet-pointer", strf(
			    "v=1:op=event:kind=3:device=%d:button=%d:state=0",
			    s->silk_pad_dev, button));
		s->silk_pad_btn->clear();
	}
	s->silk_pad_win = 0;
}

void
dv_silkie_cancel_window(struct dv_window *w)
{
	if (w == nullptr)
		return;
	ink_cancel_window(w);
	if (w->silk_live != nullptr)
		for (auto &L : *w->silk_live) {
			if (!L.used)
				continue;
			if (L.sel == 0)
				pty_fields(w, "tablet-pointer", strf(
				    "v=1:op=event:kind=%d:phase=4:device=%d:contact=%d:x=%d:y=%d:buttons=0:p=0",
				    L.kind, L.device, L.contact, L.x, L.y));
			else
				pty_fields(w, "gesture", strf(
				    "v=1:op=event:kind=%d:phase=4:x=%d:y=%d",
				    L.kind, L.x, L.y));
			L.used = 0;
		}
	if (w->session != nullptr && w->session->silk_pad_win == w->id) {
		pad_release(w->session, w);
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
	dv_strset(&sc->silk_brush, nullptr);
	dv_strset(&sc->silk_shown, nullptr);
	if (w->session != nullptr)
		dv_silkie_push(w->session);
}

static void
send_display_event(struct dv_window *w)
{
	const char	*body;
	const char	*unk =
	    "kind=unknown:color=unknown:bits=0:gamut=unknown:headroom=0:scale=0:period=0:ppi=0";

	if (w->session == nullptr)
		return;
	body = w->session->silk_display != nullptr &&
	    w->session->silk_display[0] != '\0' ? w->session->silk_display : unk;
	pty_fields(w, "display", std::string("v=1:op=event:t=0:") + body);
	dv_strset(&w->screen.silk_shown, body);
}

static void
ink_send(struct dv_peer *p, const std::string &fields)
{
	std::string	frame = "\033]777;ink;" + fields + "\033\\";

	dv_host_send_peer(p, frame.data(), frame.size());
}

/*
 * The program turned ink off: only mode 0 makes a terminal stop a stroke it is
 * drawing, whatever window its next one would be for. The push that follows
 * turns ink back on there if another window wants it.
 */
static void
ink_stop_window(struct dv_window *w)
{
	struct dv_peer	*p;

	if (w->silk_strokes == nullptr)
		return;
	for (auto &st : *w->silk_strokes) {
		if (!st.over && st.own)
			for (p = dv_host_first_peer(); p != nullptr;
			    p = TAILQ_NEXT(p, entry))
				if (p == st.peer && p->silk_ink != nullptr) {
					ink_send(p, "v=1:op=mode:mode=0");
					p->silk_ink->sent = 0;
				}
		st.over = 1;
	}
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
	if (strcmp(sel, "ink") == 0 && ((sc->silk_tablet & 1) == 0 ||
	    sc->silk_brush == nullptr))
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
	if (strcmp(sel, "ink") == 0 && mode == 0)
		ink_stop_window(w);
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
			dv_strset(&sc->silk_shown, nullptr);
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
	std::string		 fields;
	unsigned		 cw, ch;

	if (s == nullptr)
		return;
	cw = cell_px(s, 0);
	ch = cell_px(s, 1);
	if (strcmp(sel, "caps") == 0) {
		fields = strf("v=1:op=cap:id=%lld:caps=3:ext=%s", id,
		    s->silk_ext != nullptr ? s->silk_ext : "");
		fields += s->silk_ext && *s->silk_ext ? ",ssh-agent.1" : "ssh-agent.1";
		/*
		 * The SGR extensions are kept in every cell and brought down for
		 * a terminal that lacks them (compose.cc), so programs may
		 * always use them.
		 */
		fields += ":sgr=alpha,deep,gamut,hdr";
		if (s->silk_media != nullptr && s->silk_media[0] != '\0')
			fields += strf(":media=%s", s->silk_media);
		if (s->silk_format != nullptr && s->silk_format[0] != '\0') {
			std::string	fmt = s->silk_format;

			/* Decoded here for a terminal that does not take them:
			 * indexed colour, packed, palette and high precision. */
			csv_union(&fmt, "8,16,301,302,304,308,516,401,402,404,408,"
			    "316,632,664");
			fields += strf(":format=%s", fmt.c_str());
		}
	} else if (strcmp(sel, "tablet-pointer") == 0) {
		fields = strf(
		    "v=1:op=cap:id=%lld:caps=%d:mode=%d:w=%u:h=%u:cw=%u:ch=%u",
		    id, s->silk_tablet_caps, sc->silk_tablet,
		    sc->cols * cw, sc->rows * ch, cw, ch);
	} else if (strcmp(sel, "display") == 0) {
		const char	*body = s->silk_display != nullptr &&
		    s->silk_display[0] != '\0' ? s->silk_display :
		    "kind=unknown:color=unknown:bits=0:gamut=unknown:headroom=0:scale=0:period=0:ppi=0";

		fields = strf("v=1:op=cap:id=%lld:mode=%d:%s", id,
		    sc->silk_display, body);
	} else if (strcmp(sel, "font") == 0) {
		fields = strf(
		    "v=1:op=cap:id=%lld:caps=%d:roles=%s:weight_min=%d:weight_max=%d:slant_min=%d:slant_max=%d:synthetic_slant=%d",
		    id, s->silk_font_caps,
		    s->silk_font_roles != nullptr ? s->silk_font_roles : "default",
		    s->silk_font_wmin, s->silk_font_wmax, s->silk_font_smin,
		    s->silk_font_smax, s->silk_font_synth);
		/* The roles can change when the user reloads the terminal's
		 * configuration, and nothing says when. */
		host_fields(s, "font", strf("v=1:op=query:id=%d", Q_FONT));
	} else if (strcmp(sel, "frame") == 0) {
		fields = strf("v=1:op=cap:id=%lld:caps=%d:mode=%d:period=%d",
		    id, s->silk_frame_caps, sc->silk_frame, s->silk_frame_period);
	} else if (strcmp(sel, "gesture") == 0) {
		fields = strf("v=1:op=cap:id=%lld:caps=%d:mode=%d",
		    id, s->silk_gesture_caps, sc->silk_gesture);
	} else if (strcmp(sel, "haptic") == 0) {
		fields = strf("v=1:op=cap:id=%lld:caps=%d",
		    id, s->silk_haptic_caps);
	} else if (strcmp(sel, "ink") == 0) {
		fields = strf("v=1:op=cap:id=%lld:caps=%d:mode=%d",
		    id, s->silk_ink_caps, sc->silk_ink);
		if (sc->silk_brush != nullptr) {
			fields += ':';
			fields += sc->silk_brush;
		}
	} else if (strcmp(sel, "latency") == 0) {
		fields = strf(
		    "v=1:op=cap:id=%lld:caps=%d:mode=%d:input=%d:repaint=%d",
		    id, s->silk_latency_caps, sc->silk_latency,
		    s->silk_latency_input, s->silk_latency_repaint);
	} else if (strcmp(sel, "scroll") == 0) {
		fields = strf("v=1:op=cap:id=%lld:caps=%d:mode=%d",
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

/* A number as the records of a vector image write them, and finite. */
static int
brush_num(const char *v, double *ans)
{
	char	*end;

	if (v == nullptr || v[0] == '\0' || strpbrk(v, "xXnNiI") != nullptr)
		return (0);
	*ans = strtod(v, &end);
	return (*end == '\0' && isfinite(*ans));
}

/*
 * The frame as the protocol's table says, every field checked, and written
 * back with every default filled in: that is what a query reports and what
 * goes to the terminal. A key we do not know is ignored.
 */
static int
brush_normal(const struct silk_f *f, std::string *out)
{
	long long	 i = -1, g = 0, k = -1, n = 1, tools = 58;
	double		 w1, w0, e = 1;
	const char	*fill = ffield(f, "fill");
	const char	*vw0 = ffield(f, "w0"), *ve = ffield(f, "e");

	if (ffield(f, "i") == nullptr || ffield(f, "k") == nullptr ||
	    !ffield_int(f, "i", 1, 4294967295ll, &i) ||
	    !ffield_int(f, "k", 1, 4294967295ll, &k) ||
	    !ffield_int(f, "g", 0, 4294967295ll, &g) ||
	    !ffield_int(f, "n", 1, 4294967295ll, &n) ||
	    !ffield_int(f, "tools", 1, 511, &tools))
		return (0);
	if (k + n - 1 > 4294967295ll)
		return (0);
	if (fill != nullptr && (fill[0] != '#' && fill[0] != '@'))
		return (0);		/* none is refused; a color has no colon */
	if (!brush_num(ffield(f, "w1"), &w1) || w1 < 0)
		return (0);
	if (vw0 == nullptr)
		vw0 = ffield(f, "w1");
	else if (!brush_num(vw0, &w0) || w0 < 0)
		return (0);
	if (ve == nullptr)
		ve = "1";
	else if (!brush_num(ve, &e) || e <= 0)
		return (0);
	*out = strf("i=%lld:g=%lld:k=%lld:n=%lld:fill=%s:w0=%s:w1=%s:e=%s:tools=%lld",
	    i, g, k, n, fill != nullptr ? fill : "#000000", vw0, ffield(f, "w1"),
	    ve, tools);
	return (1);
}

/* The program's id is given: k and n are what is left. */
static void
brush_consume(struct dv_window *w, uint32_t id)
{
	struct silk_f	 f;
	long long	 k = 0, n = 0;
	const char	*raw = w->screen.silk_brush;
	std::string	 out;

	if (raw == nullptr || !parse_fields(&f, raw, strlen(raw)) ||
	    !ffield_int(&f, "k", 1, 4294967295ll, &k) ||
	    !ffield_int(&f, "n", 0, 4294967295ll, &n) || id < k || id >= k + n)
		return;
	for (int i = 0; i < f.n; i++) {
		if (i != 0)
			out += ':';
		out += f.key[i];
		out += '=';
		if (strcmp(f.key[i], "k") == 0)
			out += strf("%u", id + 1);
		else if (strcmp(f.key[i], "n") == 0)
			out += strf("%lld", k + n - ((long long)id + 1));
		else
			out += f.val[i];
	}
	dv_strset(&w->screen.silk_brush, out.c_str());
}

/* A role name: an ASCII letter, then letters, digits, _ . and -. */
static int
font_role_ok(const char *r)
{
	if (!((r[0] >= 'a' && r[0] <= 'z') || (r[0] >= 'A' && r[0] <= 'Z')))
		return (0);
	for (; *r != '\0'; r++)
		if (!((*r >= 'a' && *r <= 'z') || (*r >= 'A' && *r <= 'Z') ||
		    (*r >= '0' && *r <= '9') || *r == '_' || *r == '.' || *r == '-'))
			return (0);
	return (1);
}

/* Validated as a whole: a bad field changes nothing. */
static void
font_select(struct dv_window *w, const struct silk_f *f)
{
	const struct dv_session		*s = w->session;
	const struct dv_font_style	*cur = dv_font_get(w->screen.font);
	const char			*vrole = ffield(f, "role");
	const char			*vslant = ffield(f, "slant");
	std::string			 role = cur->role;
	long long			 weight = cur->weight, slant = cur->slant;
	long long			 italic = -1;
	int				 wmin = FONT_WMIN, wmax = FONT_WMAX;
	int				 smin = FONT_SMIN, smax = FONT_SMAX;

	if (s != nullptr && s->silk_font_caps != 0) {
		wmin = s->silk_font_wmin;
		wmax = s->silk_font_wmax;
		smin = s->silk_font_smin;
		smax = s->silk_font_smax;
	}
	if (vrole != nullptr) {
		if (!font_role_ok(vrole))
			return;
		role = vrole;
	}
	if (!ffield_int(f, "weight", 0, wmax, &weight) ||
	    (weight != 0 && weight < wmin))
		return;
	if (vslant != nullptr && strcmp(vslant, "auto") == 0)
		slant = DV_FONT_SLANT_AUTO;
	else if (vslant != nullptr && !ffield_int(f, "slant", smin, smax, &slant))
		return;
	if (!ffield_int(f, "italic", 0, 1, &italic))
		return;
	w->screen.font = dv_font_intern(role.c_str(), (int)weight, (int)slant);
	if (italic == 1)
		w->screen.attr |= DV_ATTR_ITALIC;
	else if (italic == 0)
		w->screen.attr &= (uint8_t)~DV_ATTR_ITALIC;
}

static void
font_state(struct dv_window *w, long long id)
{
	const struct dv_font_style	*st = dv_font_get(w->screen.font);
	std::string			 fields = "v=1:op=state";

	if (id >= 0)
		fields += strf(":id=%lld", id);
	fields += strf(":role=%s:weight=%d", st->role.c_str(), st->weight);
	if (st->slant == DV_FONT_SLANT_AUTO)
		fields += ":slant=auto";
	else
		fields += strf(":slant=%d", st->slant);
	fields += strf(":italic=%d", (w->screen.attr & DV_ATTR_ITALIC) != 0);
	pty_fields(w, "font", fields);
}

/* A pad's control and what it does now, for the system to show. */
static int
label_ok(const struct silk_f *f)
{
	static const char *ctl[] = { "button", "ring", "strip", "dial", nullptr };
	const char	*text = ffield(f, "text");
	long long	 v = 0;
	int		 nctl = 0;

	if (ffield(f, "device") == nullptr ||
	    !ffield_int(f, "device", 0, 2147483647ll, &v))
		return (0);
	for (int i = 0; ctl[i] != nullptr; i++) {
		if (ffield(f, ctl[i]) == nullptr)
			continue;
		if (!ffield_int(f, ctl[i], 0, 255, &v))
			return (0);
		nctl++;
	}
	if (nctl != 1)
		return (0);
	if (text != nullptr) {
		size_t	n = strlen(text), pad = 0;

		if (n % 4 != 0)
			return (0);
		for (size_t i = 0; i < n; i++) {
			char	c = text[i];

			if (c == '=' && i + 2 >= n) {
				pad++;
				continue;
			}
			if (pad != 0 || !((c >= 'A' && c <= 'Z') ||
			    (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			    c == '+' || c == '/'))
				return (0);
		}
	}
	return (1);
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
	st = open_osc(body, n, sel, sizeof sel, &f);
	if (st == 0)
		return (0);
	if (st < 0)
		return (1);
	op = ffield(&f, "op");
	(void)ffield_int(&f, "id", 0, 2147483647ll, &id);
	/*
	 * Font selection is a text attribute, not a mode. It goes with the
	 * cells whether or not a terminal is attached right now, and is
	 * replayed to whichever attaches. Only discovery says who is there.
	 */
	/*
	 * Depth is a text attribute too: kept with the cells and placements,
	 * and drawn by whichever terminal listed depth.1, flat elsewhere.
	 */
	if (strcmp(sel, "depth") == 0) {
		if (strcmp(op, "set") == 0)
			depth_set(w, &f);
		else if (strcmp(op, "reset") == 0)
			w->screen.depth = 0;
		return (1);
	}
	if (strcmp(sel, "font") == 0) {
		if (strcmp(op, "select") == 0)
			font_select(w, &f);
		else if (strcmp(op, "reset") == 0)
			w->screen.font = 0;
		else if (strcmp(op, "state") == 0)
			font_state(w, id);
		else if (strcmp(op, "query") == 0 && id >= 0 &&
		    offered(w->session, sel))
			reply_query(w, sel, id);
		return (1);
	}
	if (!offered(w->session, sel))
		return (1);
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
		std::string	brush;

		if (!brush_normal(&f, &brush))
			return (1);
		dv_strset(&w->screen.silk_brush, brush.c_str());
		set_mode(w, sel, 1);	/* a brush turns ink on */
		if (w->session != nullptr && w->session->desktop.focused == w)
			dv_silkie_push(w->session);
		return (1);
	}
	if (strcmp(sel, "tablet-pointer") == 0 && strcmp(op, "label") == 0) {
		/* Only from the program the pad is reporting to. */
		if (w->session != nullptr && w->session->desktop.focused == w &&
		    (w->screen.silk_tablet & 4) && label_ok(&f))
			host_fields(w->session, sel, fields_join(&f));
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
		if (w->session != nullptr && w->session->desktop.focused == w)
			host_fields(w->session, "haptic", fields_join(&f));
		return (1);
	}
	return (1);
}

/* ---- host push ------------------------------------------------------ */

/*
 * Ids to lease to a terminal at a time, while the program has more. This limits
 * nothing: another lease goes with the brush well before these are used up.
 */
#define INK_LEASE	256u
/* Our ids are as far from the ids programs tend to pick as can be. */
#define INK_TOKENS	0x80000000u

/* The window whose brush a stroke here would need, and on which placement. */
static int
ink_brush_ready(struct dv_window *w, long long *image)
{
	struct silk_f	 b;
	const char	*raw;

	if (w == nullptr || !w->screen.silk_ink || (w->screen.silk_tablet & 1) == 0 ||
	    (raw = w->screen.silk_brush) == nullptr ||
	    !parse_fields(&b, raw, strlen(raw)) ||
	    !ffield_int(&b, "i", 1, 4294967295ll, image))
		return (0);
	return (ink_next_id(w) != 0);
}

/*
 * A terminal holds one brush, so it holds the one its pen is about to need: the
 * window under the pen if it can ink the placement there, and if not, or with
 * no pen in sight, the focused window's.
 */
static struct dv_window *
ink_wanted(struct dv_peer *p, unsigned *dv_id)
{
	struct dv_session	*s = p->session;
	const struct dv_silk_ink *ink = p->silk_ink;
	struct dv_window	*w;
	long long		 image = 0;

	if (ink != nullptr && ink->hover_win != 0) {
		w = win_id(s, ink->hover_win);
		if (ink_brush_ready(w, &image) &&
		    (*dv_id = dv_kitty_ink_under(w, (unsigned)image, ink->hover_x,
		    ink->hover_y)) != 0)
			return (w);
	}
	w = s->desktop.focused;
	if (ink_brush_ready(w, &image) &&
	    (*dv_id = dv_kitty_ink_target(w, (unsigned)image)) != 0)
		return (w);
	return (nullptr);
}

/* Is the terminal drawing a stroke mode 0 would stop? */
static int
ink_peer_drawing(const struct dv_peer *p)
{
	const struct dv_window	*w;

	TAILQ_FOREACH(w, &p->session->desktop.windows, entry)
		if (w->silk_strokes != nullptr)
			for (const auto &st : *w->silk_strokes)
				if (st.peer == p && st.own && !st.over)
					return (1);
	return (0);
}

/* The brush's fields but for k and n, and those a stroke's look is made of. */
static std::string
ink_look(const struct dv_window *w)
{
	struct silk_f	 f;
	const char	*raw = w->screen.silk_brush;
	std::string	 out;

	if (raw == nullptr || !parse_fields(&f, raw, strlen(raw)))
		return (out);
	for (int i = 0; i < f.n; i++) {
		if (strcmp(f.key[i], "k") == 0 || strcmp(f.key[i], "n") == 0)
			continue;
		if (!out.empty())
			out += ':';
		out += f.key[i];
		out += '=';
		out += f.val[i];
	}
	return (out);
}

/* The look's fields under the terminal's id of the placement, with our ids. */
static std::string
ink_host_brush(const struct dv_silk_lease *lease)
{
	struct silk_f	 f;
	std::string	 out = strf("v=1:op=brush:i=%u:k=%u:n=%u", lease->host,
			    lease->first, lease->n);

	if (!parse_fields(&f, lease->look.data(), lease->look.size()))
		return (out);
	for (int i = 0; i < f.n; i++)
		if (strcmp(f.key[i], "i") != 0)
			out += strf(":%s=%s", f.key[i], f.val[i]);
	return (out);
}

static void
ink_push_peer(struct dv_peer *p)
{
	struct dv_session	*s = p->session;
	struct dv_silk_ink	*ink;
	struct dv_window	*w;
	unsigned		 dv_id = 0;

	if (s == nullptr || !p->display || !ext_has(s->silk_ext, "ink"))
		return;
	if ((ink = p->silk_ink) == nullptr)
		ink = p->silk_ink = new struct dv_silk_ink;
	w = ink_wanted(p, &dv_id);
	if (w == nullptr) {
		/* Off once nothing it is drawing would be cut short. */
		if (ink->sent != 0 && !ink_peer_drawing(p)) {
			ink_send(p, "v=1:op=mode:mode=0");
			ink->sent = 0;
		}
		return;
	}

	struct silk_f	 b;
	long long	 left = 0;
	std::string	 look = ink_look(w);
	const char	*raw = w->screen.silk_brush;

	if (parse_fields(&b, raw, strlen(raw)))
		(void)ffield_int(&b, "n", 0, 4294967295ll, &left);
	if (!ink->leases.empty()) {
		const struct dv_silk_lease	*cur = &ink->leases.back();
		long long			 unused = cur->n - cur->used;

		if (cur->window == w->id && cur->dv_id == dv_id &&
		    cur->host == dv_kitty_host_image(w, &p->view, dv_id) &&
		    cur->look == look &&
		    unused > 0 && (unused * 2 > cur->n || left <= unused)) {
			if (ink->sent != 1) {
				ink_send(p, "v=1:op=mode:mode=1");
				ink->sent = 1;
			}
			return;
		}
	}

	struct dv_silk_lease	 lease;

	lease.n = left < INK_LEASE ? (uint32_t)left : INK_LEASE;
	if (ink->next < INK_TOKENS || 0xffffffffu - ink->next < lease.n)
		ink->next = INK_TOKENS;		/* long since out of use */
	lease.first = ink->next;
	ink->next += lease.n;
	lease.sync = ink->sync = (ink->sync + 1) & Q_INK_SYNC_MASK;
	lease.window = w->id;
	lease.dv_id = dv_id;
	lease.host = dv_kitty_host_image(w, &p->view, dv_id);
	lease.look = std::move(look);
	ink_send(p, ink_host_brush(&lease));
	ink_send(p, "v=1:op=mode:mode=1");
	ink_send(p, strf("v=1:op=query:id=%u", Q_INK_SYNC + lease.sync));
	ink->sent = 1;
	ink->leases.push_back(std::move(lease));
}

/*
 * An answer to the query that followed a brush: reports are in the order the
 * terminal made them, so none will come from the leases before that one.
 */
static void
ink_synced(struct dv_peer *p, long long id)
{
	struct dv_silk_ink	*ink = p->silk_ink;

	if (ink == nullptr)
		return;
	for (size_t i = 0; i < ink->leases.size(); i++)
		if (Q_INK_SYNC + (long long)ink->leases[i].sync == id) {
			ink->leases.erase(ink->leases.begin(),
			    ink->leases.begin() + (std::ptrdiff_t)i);
			return;
		}
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
	for (struct dv_peer *q = dv_host_first_peer(); q != nullptr;
	    q = TAILQ_NEXT(q, entry))
		if (q->session == s)
			ink_push_peer(q);
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
	delete p->silk_ink;
	p->silk_ink = nullptr;
	dv_silkie_peers_changed(p->session);
	dv_pstatus_attach(p);
}

void
dv_silkie_focus(struct dv_session *s, struct dv_window *prev)
{
	if (s == nullptr)
		return;
	dv_window_report_focus(prev);
	dv_window_report_focus(s->desktop.focused);
	if (prev != nullptr && s->silk_pad_win == prev->id)
		pad_release(s, prev);
	dv_silkie_push(s);
}

void
dv_silkie_output(struct dv_window *w)
{
	if (w != nullptr && w->screen.silk_frame)
		w->screen.silk_frame_dirty = 1;
	/* Its output can place, scroll away or delete what a brush draws on. */
	if (w != nullptr && w->screen.silk_brush != nullptr && w->session != nullptr)
		dv_silkie_push(w->session);
}

int
dv_silkie_gfx_ok(const struct dv_session *s, long f, int G, int medium)
{
	int	stdf = (f == 24 || f == 32 || f == 100);
	int	stdt = (medium == 'd' || medium == 'f' || medium == 't' ||
	    medium == 's');
	char	num[16];

	if (!dv_kitty_fmt_known(f))
		return (0);
	/* G means something only where it tags colours: a PNG says what its
	 * colours are, f=64 is extended sRGB, each paint of f=200 names its
	 * own, gray has none and indexed colour is the terminal's. */
	if (!dv_kitty_fmt_gamut(f))
		G = 0;
	/*
	 * Kitty's indexed colour and Silkie's packed, palette and high
	 * precision pixels describe only what the program sends: they are
	 * decoded here for a terminal that did not list them, so any attached
	 * terminal can show them. Their media are the ordinary ones. A gamut
	 * needs a terminal that takes one.
	 */
	if (dv_kitty_fmt_bit(f) != 0) {
		if (!stdt)
			return (0);
		if (G == 0)
			return (1);
		return (s != nullptr && s->silk_any && s->silk_wg);
	}
	if (stdf && stdt && G == 0)
		return (1);
	/* Drawn or converted here for a terminal that takes neither. */
	if ((f == 64 || f == 200) && stdt)
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
	if (G != 0 && !s->silk_wg)
		return (0);
	return (1);
}

/* ---- host events ---------------------------------------------------- */

static struct dv_window *
silk_at(struct dv_session *s, int x, int y)
{
	struct dv_window	*w, *best = nullptr;

	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		int	framed = dv_window_framed(w);
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
	int		framed = dv_window_framed(w);
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
	if (p->silk_hit == nullptr)
		return (nullptr);
	for (auto &h : *p->silk_hit)
		if (h.used && h.sel == sel && h.device == dev &&
		    h.contact == contact && (kind < 0 || h.kind == kind))
			return (&h);
	return (nullptr);
}

static struct dv_silk_hit *
hit_new(struct dv_peer *p)
{
	if (p->silk_hit == nullptr)
		p->silk_hit = new std::vector<struct dv_silk_hit>;
	for (auto &h : *p->silk_hit)
		if (!h.used)
			return (&h);
	return (&p->silk_hit->emplace_back());
}

static void
emit_fields(struct dv_window *w, const char *sel, const struct silk_f *f,
    int tx, int ty, int do_xy, int strip_hw)
{
	std::string	fields;

	for (int i = 0; i < f->n; i++) {
		const char	*k = f->key[i];
		const char	*v = f->val[i];

		if (strip_hw && (strcmp(k, "serial") == 0 || strcmp(k, "hwid") == 0 ||
		    strcmp(k, "vid") == 0 || strcmp(k, "pid") == 0))
			continue;
		if (!fields.empty())
			fields += ':';
		if (do_xy && (strcmp(k, "x") == 0 || strcmp(k, "y") == 0)) {
			fields += strf("%s=%d", k, strcmp(k, "x") == 0 ? tx : ty);
			continue;
		}
		fields += k;
		fields += '=';
		fields += v;
	}
	pty_fields(w, sel, fields);
}

static void
send_desc(struct dv_peer *p, struct dv_window *w, int dev, int allow_hw)
{
	if (p->silk_desc == nullptr)
		return;
	for (const auto &d : *p->silk_desc) {
		if (d.device != dev)
			continue;
		if (allow_hw)
			pty_fields(w, "tablet-pointer", d.fields);
		else {
			struct silk_f	f;

			if (parse_fields(&f, d.fields.data(), d.fields.size()))
				emit_fields(w, "tablet-pointer", &f, 0, 0, 0, 1);
		}
		return;
	}
}

static void
store_desc(struct dv_peer *p, const struct silk_f *f, int dev, int kind)
{
	struct dv_silk_desc	*d = nullptr;

	if (p->silk_desc == nullptr)
		p->silk_desc = new std::vector<struct dv_silk_desc>;
	for (auto &e : *p->silk_desc)
		if (e.device == dev) {
			d = &e;
			break;
		}
	if (d == nullptr)
		d = &p->silk_desc->emplace_back();
	d->device = dev;
	d->kind = kind;
	d->fields = fields_join(f);
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

/* Where the program is told a report is: on a stroke, by the terminal's ink. */
static void
ink_report(struct dv_peer *p, struct dv_window *w, int dev, int contact, int tx,
    int ty, int *rx, int *ry)
{
	auto	*st = ink_find(w, p, dev, contact);

	*rx = tx;
	*ry = ty;
	if (st != nullptr && !st->over)
		dv_kitty_ink_report(w, st, rx, ry);
}

static void
tablet_event(struct dv_peer *p, struct silk_f *f)
{
	struct dv_session	*s = p->session;
	long long		 kind = 0, phase = 0, dev = 0, contact = 0;
	long long		 x = 0, y = 0, src = 0, conf = 1, buttons = 0;
	long long		 tool = 0;
	int			 dx, dy, tx, ty, rx, ry, have_xy;
	std::string		 ink_id;
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
		if (s->silk_pad_btn == nullptr)
			s->silk_pad_btn = new std::vector<int>;
		if (button >= 0 && state == 1) {
			s->silk_pad_dev = (int)dev;
			s->silk_pad_win = fw->id;
			s->silk_pad_btn->push_back((int)button);
		} else if (button >= 0 && state == 0)
			std::erase(*s->silk_pad_btn, (int)button);
		return;
	}

	if (phase == 0) {
		w = silk_at(s, dx, dy);
		if (kind == 1 && w != nullptr && (w->screen.silk_tablet & 1)) {
			local_xy(p, w, (int)x, (int)y, &tx, &ty);
			emit_fields(w, "tablet-pointer", f, tx, ty, src != 1, 0);
			if (src != 1)
				ink_hover(p, w, tx, ty);
		} else {
			if (have_xy)
				inject_at(p, (int)x, (int)y, mouse_code(0, 1), 1);
			if (kind == 1)
				ink_hover(p, nullptr, 0, 0);
		}
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
		if (auto *st = ink_find(w, p, (int)dev, (int)contact))
			ink_finish(w, st);
		if (kind == 1)
			ink_hover(p, nullptr, 0, 0);
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
			if (w == nullptr && p->silk_hit != nullptr)
				for (const auto &h : *p->silk_hit)
					if (h.used && h.sel == 0 && h.tool == (int)tool) {
						w = win_id(s, h.window);
						if (w != nullptr)
							break;
					}
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
	if (phase == 1) {
		int	legacy = hit != nullptr ? hit->legacy :
			    w != nullptr && (w->screen.silk_tablet & bit) == 0;
		int	told = w != nullptr && !legacy && src != 1 && (conf != 0 ||
			    (w->screen.silk_tablet & 64) != 0);

		tx = ty = 0;
		if (told)
			local_xy(p, w, (int)x, (int)y, &tx, &ty);
		ink_claim(p, told ? w : nullptr, f, (int)dev, (int)contact, tx, ty,
		    &ink_id);
	}

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
		ink_report(p, w, (int)dev, (int)contact, tx, ty, &rx, &ry);
		emit_fields(w, "tablet-pointer", f, rx, ry, src != 1, 0);
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
			if (auto *st = ink_find(w, p, (int)dev, (int)contact)) {
				ink_withdraw(w, st);
				ink_end(w, st);
			}
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
	ink_report(p, w, (int)dev, (int)contact, tx, ty, &rx, &ry);
	if (src == 1)
		emit_fields(w, "tablet-pointer", f, 0, 0, 0, 0);
	else
		emit_fields(w, "tablet-pointer", f, rx, ry, 1, 0);
	if (hit->reported)
		live_add(w, 0, (int)kind, (int)dev, (int)contact, tx, ty,
		    (int)buttons, (int)tool);
	if (auto *st = ink_find(w, p, (int)dev, (int)contact)) {
		if (phase == 4) {
			if (st->began)
				dv_kitty_ink_record(w, st, strf("x id=%u", st->id));
			ink_end(w, st);
		} else {
			if ((phase == 2 || phase == 3) && have_xy)
				ink_point(w, st, f, tx, ty);
			if (phase == 3)
				ink_finish(w, st);
		}
	}
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
		std::string	fields;

		if (!w->screen.silk_frame || !w->screen.silk_frame_dirty)
			continue;
		fields = strf("v=1:op=event:seq=%u:t=%s", w->screen.silk_frame_seq,
		    t != nullptr ? t : "0");
		if (tu != nullptr)
			fields += strf(":tu=%s", tu);
		fields += strf(":period=%d", s->silk_frame_period);
		pty_fields(w, "frame", fields);
		w->screen.silk_frame_seq++;
		w->screen.silk_frame_dirty = 0;
	}
}

static void
display_event(struct dv_peer *p, const struct silk_f *f)
{
	struct dv_session	*s = p->session;
	struct dv_window	*w;
	std::string		 body = display_body(f), fields;
	const char		*t = ffield(f, "t");

	if (body.empty())
		return;
	/* This client's own display changed: what its images are packed for. */
	blob_set(&p->silkie_blob, "display", body.c_str());
	dv_silkie_recompute(s, dv_host_first_peer());
	/* Programs hear of the session's display: the best of every
	 * terminal's, which this one's change may have left as it was. */
	if (s->silk_display != nullptr && s->silk_display[0] != '\0')
		body = s->silk_display;
	fields = strf("v=1:op=event:t=%s:", t != nullptr ? t : "0") + body;
	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		if (!w->screen.silk_display)
			continue;
		if (w->screen.silk_shown != nullptr &&
		    body == w->screen.silk_shown)
			continue;
		pty_fields(w, "display", fields);
		dv_strset(&w->screen.silk_shown, body.c_str());
	}
}

static void
absorb_reply(struct dv_peer *p, const char *sel, const struct silk_f *f)
{
	std::string	built;
	long long	caps = 0, per = 0, in = 0, re = 0;

	if (strcmp(sel, "caps") == 0) {
		if (ffield(f, "ext") != nullptr)
			blob_set(&p->silkie_blob, "ext", ffield(f, "ext"));
		/* A terminal that understands none of them omits the field. */
		blob_set(&p->silkie_blob, "sgr", ffield(f, "sgr"));
		if (ffield(f, "media") != nullptr)
			blob_set(&p->silkie_blob, "media", ffield(f, "media"));
		if (ffield(f, "format") != nullptr)
			blob_set(&p->silkie_blob, "format", ffield(f, "format"));
		return;
	}
	if (strcmp(sel, "display") == 0) {
		built = display_body(f);
		if (!built.empty())
			blob_set(&p->silkie_blob, "display", built.c_str());
		return;
	}
	if (strcmp(sel, "font") == 0) {
		blob_set(&p->silkie_blob, "font", font_blob(f).c_str());
		return;
	}
	if (strcmp(sel, "frame") == 0) {
		(void)ffield_int(f, "caps", 0, 2147483647ll, &caps);
		(void)ffield_int(f, "period", 0, 2147483647ll, &per);
		blob_set(&p->silkie_blob, "frame",
		    strf("%lld %lld", caps, per).c_str());
		return;
	}
	if (strcmp(sel, "latency") == 0) {
		(void)ffield_int(f, "caps", 0, 2147483647ll, &caps);
		(void)ffield_int(f, "input", 0, 2147483647ll, &in);
		(void)ffield_int(f, "repaint", 0, 2147483647ll, &re);
		blob_set(&p->silkie_blob, "latency",
		    strf("%lld %lld %lld", caps, in, re).c_str());
		return;
	}
	(void)ffield_int(f, "caps", 0, 2147483647ll, &caps);
	built = strf("%lld", caps);
	if (strcmp(sel, "gesture") == 0)
		blob_set(&p->silkie_blob, "gesture", built.c_str());
	else if (strcmp(sel, "haptic") == 0)
		blob_set(&p->silkie_blob, "haptic", built.c_str());
	else if (strcmp(sel, "ink") == 0)
		blob_set(&p->silkie_blob, "ink", built.c_str());
	else if (strcmp(sel, "scroll") == 0)
		blob_set(&p->silkie_blob, "scroll", built.c_str());
	else if (strcmp(sel, "tablet-pointer") == 0)
		blob_set(&p->silkie_blob, "tablet", built.c_str());
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
	st = open_osc(body, n, sel, sizeof sel, &f);
	if (st == 0)
		return (0);
	if (st < 0)
		return (1);
	op = ffield(&f, "op");
	(void)ffield_int(&f, "id", 0, 2147483647ll, &id);
	if (op != nullptr && strcmp(op, "cap") == 0 && strcmp(sel, "ink") == 0 &&
	    id >= Q_INK_SYNC) {
		ink_synced(p, id);
		return (1);
	}
	if (op != nullptr && strcmp(op, "cap") == 0 && id >= Q_CAPS && id <= Q_LAST) {
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
