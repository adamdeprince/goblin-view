/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * The Program Status Protocol (OSC 7501, revision 0.4), from its published
 * specification at superlogical.com. Nothing here is taken from any
 * terminal's implementation.
 *
 * A window is the protocol's terminal: it keeps one record per id, each
 * the keys of the last report for that id, and applies the lifetime rules
 * -- working, blocked and idle records end at the next shell prompt;
 * done and error records stay until the user comes back to the window
 * (focuses it or types in it); RIS removes them all. As Silkie does, none
 * of the Limits section's caps are applied: a report, its fields and the
 * number of records are bounded only by memory (and escape-limit, when
 * the user sets one). The syntax is checked in full, and a report whose
 * text holds a control character is refused, as the Security section
 * requires.
 *
 * The frame shows the most urgent record (dv_pstatus_view_of). And since
 * a terminal holds any number of records in a hierarchy, every attached
 * terminal that answers the feature query is sent each window's records
 * beneath an id of the window's own, w<id>, with its root record standing
 * for the most urgent of them all.
 */

#include "pstatus.hpp"
#include "silkie.hpp"

#include <string.h>

#include <algorithm>
#include <string>
#include <string_view>

static unsigned long long	ps_seq;	/* orders reports across windows */

static const char *const	state_names[] = {
	"idle", "done", "working", "error", "blocked"
};
static const char *const	kind_names[] = {
	"", "permission", "question", "auth"
};

static std::string_view
trim(std::string_view v)
{
	while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
		v.remove_prefix(1);
	while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
		v.remove_suffix(1);
	return (v);
}

static bool
is_alnum(unsigned char c)
{
	return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
	    (c >= '0' && c <= '9'));
}

/* value := [A-Za-z0-9_.,+/=-]* */
static bool
value_ok(std::string_view v)
{
	for (unsigned char c : v)
		if (!is_alnum(c) && strchr("_.,+/=-", c) == nullptr)
			return (false);
	return (true);
}

/* [A-Za-z0-9_.+-]+: an app, or a segment of an id. No length cap. */
static bool
name_ok(std::string_view v)
{
	if (v.empty())
		return (false);
	for (unsigned char c : v)
		if (!is_alnum(c) && strchr("_.+-", c) == nullptr)
			return (false);
	return (true);
}

/* id := segment ("/" segment)* */
static bool
id_ok(std::string_view v)
{
	for (;;) {
		size_t	slash = v.find('/');

		if (!name_ok(v.substr(0, slash)))
			return (false);
		if (slash == v.npos)
			return (true);
		v.remove_prefix(slash + 1);
	}
}

/* Standard base64, padding optional. False when it is not base64. */
static bool
b64_text(std::string_view in, std::string *out)
{
	unsigned	acc = 0;
	int		bits = 0;
	size_t		n = in.size(), pad = 0;

	while (pad < 2 && n > 0 && in[n - 1] == '=') {
		n--;
		pad++;
	}
	if (n % 4 == 1 || (pad > 0 && (n + pad) % 4 != 0))
		return (false);
	out->clear();
	for (size_t i = 0; i < n; i++) {
		unsigned char	c = (unsigned char)in[i];
		unsigned	v;

		if (c >= 'A' && c <= 'Z')
			v = c - 'A';
		else if (c >= 'a' && c <= 'z')
			v = c - 'a' + 26;
		else if (c >= '0' && c <= '9')
			v = c - '0' + 52;
		else if (c == '+')
			v = 62;
		else if (c == '/')
			v = 63;
		else
			return (false);
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out->push_back((char)((acc >> bits) & 0xff));
		}
	}
	return (true);
}

/* Next code point of valid UTF-8 (shortest form, no surrogates). */
static bool
utf8_next(std::string_view s, size_t *i, uint32_t *cp)
{
	unsigned char	c = (unsigned char)s[*i];
	unsigned	need, min;
	uint32_t	v;

	if (c < 0x80) {
		*cp = c;
		(*i)++;
		return (true);
	}
	if ((c & 0xe0) == 0xc0) {
		need = 1;
		min = 0x80;
		v = c & 0x1f;
	} else if ((c & 0xf0) == 0xe0) {
		need = 2;
		min = 0x800;
		v = c & 0x0f;
	} else if ((c & 0xf8) == 0xf0) {
		need = 3;
		min = 0x10000;
		v = c & 0x07;
	} else
		return (false);
	if (s.size() - *i <= need)
		return (false);
	for (unsigned k = 1; k <= need; k++) {
		unsigned char	cc = (unsigned char)s[*i + k];

		if ((cc & 0xc0) != 0x80)
			return (false);
		v = (v << 6) | (cc & 0x3f);
	}
	if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
		return (false);
	*cp = v;
	*i += need + 1;
	return (true);
}

/* Decoded free text: valid UTF-8 with no C0, DEL or C1 control. */
static bool
text_ok(std::string_view s)
{
	size_t		i = 0;
	uint32_t	cp;

	while (i < s.size()) {
		if (!utf8_next(s, &i, &cp))
			return (false);
		if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f))
			return (false);
	}
	return (true);
}

static int
lookup(std::string_view v, const char *const *names, int count)
{
	for (int i = 0; i < count; i++)
		if (names[i][0] != '\0' && v == names[i])
			return (i);
	return (-1);
}

static bool
is_focused(const struct dv_window *w)
{
	const struct dv_session	*s = w->session;

	return (s != nullptr && s->desktop.focused == w && !s->host_unfocused &&
	    !w->hidden && !w->aside);
}

static struct dv_pstatus *
ps_of(struct dv_window *w)
{
	if (w->pstatus == nullptr) {
		w->pstatus = new dv_pstatus;
		/* Focus it has now is not the user coming back to it. */
		w->pstatus->focused = is_focused(w);
	}
	return (w->pstatus);
}

/* id is beneath (or is) under. "" is above everything. */
static bool
beneath(const std::string &id, const std::string &under)
{
	if (under.empty() || id == under)
		return (true);
	return (id.size() > under.size() && id.compare(0, under.size(), under) == 0 &&
	    id[under.size()] == '/');
}

/* ---- export to the attached terminals ---- */

static bool
peer_takes(const struct dv_peer *p, const struct dv_session *s)
{
	return (p->display && p->session == s && dv_silkie_peer_has(p, "pstatus"));
}

static void
send_to(struct dv_peer *p, const std::string &body)
{
	std::string	seq = "\033]7501;" + body + "\033\\";

	dv_host_send_peer(p, seq.data(), seq.size());
}

static void
send_all(struct dv_session *s, const std::string &body)
{
	for (struct dv_peer *p = dv_host_first_peer(); p != nullptr;
	    p = TAILQ_NEXT(p, entry))
		if (peer_takes(p, s))
			send_to(p, body);
}

static std::string
exported_id(const struct dv_window *w, const std::string &id)
{
	std::string	out = "w" + std::to_string(w->id);

	if (!id.empty())
		out += "/" + id;
	return (out);
}

std::string
dv_pstatus_report(const struct dv_pstatus_rec *r, const char *id)
{
	std::string	out = "state=";

	out += state_names[r->state];
	if (id != nullptr && id[0] != '\0') {
		out += ":id=";
		out += id;
	}
	if (r->state == DV_PS_BLOCKED && r->kind != DV_PS_KIND_NONE) {
		out += ":kind=";
		out += kind_names[r->kind];
	}
	if ((r->state == DV_PS_WORKING || r->state == DV_PS_BLOCKED) &&
	    r->progress >= 0)
		out += ":progress=" + std::to_string(r->progress);
	if (!r->app.empty())
		out += ":app=" + r->app;
	if (!r->title_b64.empty())
		out += ":title=" + r->title_b64;
	if (!r->msg_b64.empty())
		out += ":msg=" + r->msg_b64;
	return (out);
}

static std::string
b64_of(const std::string &in)
{
	static const char	t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	    "abcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string		out;

	for (size_t i = 0; i < in.size(); i += 3) {
		unsigned	v = (unsigned)(unsigned char)in[i] << 16;

		if (i + 1 < in.size())
			v |= (unsigned)(unsigned char)in[i + 1] << 8;
		if (i + 2 < in.size())
			v |= (unsigned char)in[i + 2];
		out += t[(v >> 18) & 63];
		out += t[(v >> 12) & 63];
		out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
		out += i + 2 < in.size() ? t[v & 63] : '=';
	}
	return (out);
}

/* The app a record shows: its own, else its nearest ancestor's. */
static std::string
app_of(const struct dv_pstatus *ps, const struct dv_pstatus_rec *r)
{
	std::string	id = r->id;

	if (!r->app.empty())
		return (r->app);
	while (!id.empty()) {
		size_t	slash = id.rfind('/');

		id = slash == id.npos ? std::string() : id.substr(0, slash);
		for (const auto &a : ps->recs)
			if (a.id == id && !a.app.empty())
				return (a.app);
	}
	return ("");
}

static bool
more_urgent(const struct dv_pstatus_rec *a, const struct dv_pstatus_rec *b)
{
	if (b == nullptr)
		return (true);
	if (a->state != b->state)
		return (a->state > b->state);
	return (a->seq > b->seq);
}

/*
 * The terminal's root record: the most urgent record of the session, with
 * the app it shows and its window's title, or nothing at all.
 */
static std::string
root_body(struct dv_session *s, const struct dv_window *skip)
{
	const struct dv_pstatus_rec	*best = nullptr;
	const struct dv_window		*bw = nullptr;
	struct dv_window		*w;
	struct dv_pstatus_rec		 r;

	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		if (w->pstatus == nullptr || w == skip)
			continue;
		for (const auto &rec : w->pstatus->recs)
			if (more_urgent(&rec, best)) {
				best = &rec;
				bw = w;
			}
	}
	if (best == nullptr)
		return ("state=clear");
	r = *best;
	r.app = app_of(bw->pstatus, best);
	{
		/* A terminal refuses text with a control in it: none go. */
		std::string	title = std::to_string(bw->id) + ":", clean;

		title += bw->title[0] ? bw->title : bw->cmd;
		for (unsigned char c : title)
			if (c >= 0x20 && c != 0x7f)
				clean += (char)c;
		if (text_ok(clean))
			r.title_b64 = b64_of(clean);
	}
	return (dv_pstatus_report(&r, nullptr));
}

static void
export_root(struct dv_session *s, const struct dv_window *skip = nullptr)
{
	std::string	body = root_body(s, skip);

	if (s->pstatus_root != nullptr && body == s->pstatus_root)
		return;
	dv_strset(&s->pstatus_root, body.c_str());
	send_all(s, body);
}

/* A record of this window, as it now stands, to every terminal. */
static void
export_rec(struct dv_window *w, const struct dv_pstatus_rec *r)
{
	if (w->session == nullptr)
		return;
	send_all(w->session, dv_pstatus_report(r, exported_id(w, r->id).c_str()));
}

static void
export_clear(struct dv_window *w, const std::string &id)
{
	if (w->session == nullptr)
		return;
	send_all(w->session, "state=clear:id=" + exported_id(w, id));
}

void
dv_pstatus_attach(struct dv_peer *p)
{
	struct dv_session	*s;
	struct dv_window	*w;

	if (p == nullptr || (s = p->session) == nullptr || !peer_takes(p, s))
		return;
	/* What it held from before is no longer so. */
	send_to(p, "state=clear");
	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		if (w->pstatus == nullptr)
			continue;
		for (const auto &r : w->pstatus->recs)
			send_to(p, dv_pstatus_report(&r, exported_id(w, r.id).c_str()));
	}
	{
		std::string	body = root_body(s, nullptr);

		dv_strset(&s->pstatus_root, body.c_str());
		if (body != "state=clear")
			send_to(p, body);
	}
}

void
dv_pstatus_session_gone(struct dv_session *s)
{
	if (s != nullptr)
		dv_strset(&s->pstatus_root, nullptr);
}

/* ---- records ---- */

static void
changed(struct dv_window *w)
{
	if (w->session == nullptr)
		return;
	w->session->dirty = 1;
	export_root(w->session);
}

/* Remove id and everything beneath it ("" is everything). */
static void
clear_tree(struct dv_window *w, const std::string &id)
{
	struct dv_pstatus	*ps = w->pstatus;
	size_t			 before;

	if (ps == nullptr)
		return;
	before = ps->recs.size();
	std::erase_if(ps->recs, [&](const dv_pstatus_rec &r) {
		return (beneath(r.id, id));
	});
	if (ps->recs.size() != before) {
		export_clear(w, id);
		changed(w);
	}
}

static void
store(struct dv_window *w, struct dv_pstatus_rec &&r)
{
	struct dv_pstatus	*ps = ps_of(w);

	r.seq = ++ps_seq;
	for (auto &old : ps->recs)
		if (old.id == r.id) {
			old = std::move(r);
			export_rec(w, &old);
			changed(w);
			return;
		}
	ps->recs.push_back(std::move(r));
	export_rec(w, &ps->recs.back());
	changed(w);
}

/* Drop the records whose state is in the mask. */
static void
drop_states(struct dv_window *w, unsigned mask)
{
	struct dv_pstatus	*ps = w->pstatus;
	std::vector<std::string> gone;

	if (ps == nullptr)
		return;
	for (const auto &r : ps->recs)
		if (mask & (1u << r.state))
			gone.push_back(r.id);
	if (gone.empty())
		return;
	std::erase_if(ps->recs, [&](const dv_pstatus_rec &r) {
		return ((mask & (1u << r.state)) != 0);
	});
	for (const auto &id : gone)
		export_clear(w, id);
	/*
	 * A clear takes what is beneath the id with it, on the terminal; put
	 * back any record of ours that was beneath one that went.
	 */
	for (const auto &r : ps->recs)
		for (const auto &id : gone)
			if (beneath(r.id, id)) {
				export_rec(w, &r);
				break;
			}
	changed(w);
}

void
dv_pstatus_osc(struct dv_window *w, const char *body, size_t n)
{
	std::string_view	 in(body, n), state, id, kind, progress, app;
	std::string_view	 title, msg;
	bool			 has_id = false, has_title = false, has_msg = false;
	struct dv_pstatus_rec	 r;
	int			 st;

	if (trim(in) == "?") {
		static const char	reply[] = "\033]7501;?:states=idle,working,"
		    "done,blocked,error:kinds=permission,question,auth\033\\";

		dv_window_pty_send(w, reply, sizeof reply - 1);
		return;
	}
	for (;;) {
		size_t			 colon = in.find(':');
		std::string_view	 pair = in.substr(0, colon), key, val;
		size_t			 eq = pair.find('=');
		bool			 key_ok = true;

		if (eq != pair.npos) {
			key = trim(pair.substr(0, eq));
			val = trim(pair.substr(eq + 1));
			if (key.empty())
				key_ok = false;
			for (unsigned char c : key)
				if (c < 'a' || c > 'z')
					key_ok = false;
			/* A malformed pair is skipped; the rest still applies. */
			if (key_ok && value_ok(val)) {
				if (key == "state")
					state = val;
				else if (key == "id") {
					id = val;
					has_id = true;
				} else if (key == "kind")
					kind = val;
				else if (key == "progress")
					progress = val;
				else if (key == "app")
					app = val;
				else if (key == "title") {
					title = val;
					has_title = true;
				} else if (key == "msg") {
					msg = val;
					has_msg = true;
				}
				/* Unknown keys are the extension mechanism. */
			}
		}
		if (colon == in.npos)
			break;
		in.remove_prefix(colon + 1);
	}

	/* A missing or unknown state ignores the report. */
	if (state == "clear")
		st = -1;
	else if ((st = lookup(state, state_names, 5)) < 0)
		return;
	/* An id that breaks the grammar does not fall back to the root. */
	if (has_id && !id_ok(id))
		return;
	r.id.assign(id);
	if (st < 0) {
		ps_of(w)->real = true;
		clear_tree(w, r.id);
		return;
	}
	/* Text that is not base64, or decodes to a control, refuses it all. */
	if (has_title && !title.empty()) {
		if (!b64_text(title, &r.title) || !text_ok(r.title))
			return;
		r.title_b64.assign(title);
	}
	if (has_msg && !msg.empty()) {
		if (!b64_text(msg, &r.msg) || !text_ok(r.msg))
			return;
		r.msg_b64.assign(msg);
	}
	r.state = st;
	if (st == DV_PS_BLOCKED) {
		int	k = lookup(kind, kind_names, 4);

		r.kind = k > 0 ? k : DV_PS_KIND_NONE;
	}
	if ((st == DV_PS_WORKING || st == DV_PS_BLOCKED) && !progress.empty() &&
	    progress.size() <= 3 &&
	    std::all_of(progress.begin(), progress.end(),
	    [](char c) { return (c >= '0' && c <= '9'); })) {
		int	v = 0;

		for (char c : progress)
			v = v * 10 + (c - '0');
		if (v <= 100)
			r.progress = v;
	}
	if (name_ok(app))
		r.app.assign(app);
	ps_of(w)->real = true;
	store(w, std::move(r));
}

/*
 * OSC 9;4;st;pr, ConEmu's progress. Mapped to the root record, as the
 * protocol allows, until the program sends a report of its own: a report
 * replaces its record whole, so a mapped 9;4 would wipe its kind and msg.
 */
void
dv_pstatus_progress(struct dv_window *w, const char *args, size_t n)
{
	std::string_view	 in(args, n), sf, pf;
	struct dv_pstatus	*ps = ps_of(w);
	struct dv_pstatus_rec	 r;
	long			 st = 0, pr = -1;
	const struct dv_pstatus_rec *root;

	if (ps->real)
		return;
	if (!in.empty() && in.front() == ';')
		in.remove_prefix(1);
	sf = in.substr(0, in.find(';'));
	if (in.find(';') != in.npos)
		pf = in.substr(in.find(';') + 1);
	pf = pf.substr(0, pf.find(';'));
	for (char c : sf) {
		if (c < '0' || c > '9')
			return;
		st = st * 10 + (c - '0');
		if (st > 9)
			return;
	}
	if (!pf.empty()) {
		pr = 0;
		for (char c : pf) {
			if (c < '0' || c > '9') {
				pr = -1;
				break;
			}
			if (pr <= 100)
				pr = pr * 10 + (c - '0');
		}
		if (pr > 100)
			pr = 100;
	}
	root = dv_pstatus_find(w, "");
	switch (st) {
	case 0:				/* remove, and the bare "9;4" */
		if (root != nullptr && root->osc9)
			clear_tree(w, "");
		return;
	case 1:				/* a value */
	case 4:				/* paused at a value */
		r.state = DV_PS_WORKING;
		r.progress = (int)pr;
		break;
	case 2:				/* error */
		r.state = DV_PS_ERROR;
		break;
	case 3:				/* busy, no value */
		r.state = DV_PS_WORKING;
		break;
	default:
		return;
	}
	if (st == 4) {
		r.msg = "paused";
		r.msg_b64 = b64_of(r.msg);
	}
	r.osc9 = true;
	store(w, std::move(r));
}

/* A new prompt: what was running is over. Idle goes too, as it may. */
void
dv_pstatus_prompt(struct dv_window *w)
{
	if (w != nullptr)
		drop_states(w, (1u << DV_PS_WORKING) | (1u << DV_PS_BLOCKED) |
		    (1u << DV_PS_IDLE));
}

void
dv_pstatus_reset(struct dv_window *w)
{
	if (w == nullptr || w->pstatus == nullptr)
		return;
	clear_tree(w, "");
	w->pstatus->real = false;
}

/* The user is back: the results they have now seen are let go. */
void
dv_pstatus_key(struct dv_window *w)
{
	if (w != nullptr)
		drop_states(w, (1u << DV_PS_DONE) | (1u << DV_PS_ERROR));
}

void
dv_pstatus_focus(struct dv_session *s)
{
	struct dv_window	*w;

	if (s == nullptr)
		return;
	TAILQ_FOREACH(w, &s->desktop.windows, entry) {
		bool	now = is_focused(w);

		if (w->pstatus == nullptr)
			continue;
		if (now && !w->pstatus->focused)
			dv_pstatus_key(w);
		w->pstatus->focused = now;
	}
}

void
dv_pstatus_close(struct dv_window *w)
{
	struct dv_session	*s = w->session;

	if (w->pstatus == nullptr)
		return;
	if (!w->pstatus->recs.empty() && s != nullptr) {
		export_clear(w, "");
		w->pstatus->recs.clear();
		export_root(s, w);		/* worked out without it */
	}
	delete w->pstatus;
	w->pstatus = nullptr;
}

size_t
dv_pstatus_count(const struct dv_window *w)
{
	return (w->pstatus != nullptr ? w->pstatus->recs.size() : 0);
}

const struct dv_pstatus_rec *
dv_pstatus_find(const struct dv_window *w, const char *id)
{
	if (w->pstatus == nullptr)
		return (nullptr);
	for (const auto &r : w->pstatus->recs)
		if (r.id == id)
			return (&r);
	return (nullptr);
}

int
dv_pstatus_view_of(const struct dv_window *w, struct dv_pstatus_view *v)
{
	const struct dv_pstatus_rec	*best = nullptr;
	std::string			 app, label;

	*v = dv_pstatus_view();
	if (w->pstatus == nullptr)
		return (0);
	for (const auto &r : w->pstatus->recs)
		if (more_urgent(&r, best))
			best = &r;
	if (best == nullptr || best->state == DV_PS_IDLE)
		return (0);
	for (const auto &r : w->pstatus->recs)
		if (r.state == best->state)
			v->count++;
	v->state = best->state;
	v->kind = best->kind;
	v->progress = best->progress;
	app = app_of(w->pstatus, best);
	label = !best->title.empty() ? best->title : app;
	if (!label.empty() && !best->msg.empty())
		v->text = label + ": " + best->msg;
	else if (!best->msg.empty())
		v->text = best->msg;
	else if (!label.empty())
		v->text = label;
	else if (best->state == DV_PS_BLOCKED && best->kind != DV_PS_KIND_NONE)
		v->text = std::string("waiting for ") + kind_names[best->kind];
	else
		v->text = state_names[best->state];
	return (1);
}

/*
 * "<real> <osc9> <report>": whether reports have been seen, whether this
 * record came from OSC 9;4, and the report that makes it again. The
 * records are in the order they were last reported, which is what the
 * frame's choice between equally urgent ones goes by.
 */
std::vector<std::string>
dv_pstatus_save(const struct dv_window *w)
{
	std::vector<const dv_pstatus_rec *>	order;
	std::vector<std::string>		out;

	if (w->pstatus == nullptr)
		return (out);
	for (const auto &r : w->pstatus->recs)
		order.push_back(&r);
	std::sort(order.begin(), order.end(),
	    [](const dv_pstatus_rec *a, const dv_pstatus_rec *b) {
		return (a->seq < b->seq);
	});
	for (const auto *r : order)
		out.push_back(std::string(w->pstatus->real ? "1 " : "0 ") +
		    (r->osc9 ? "1 " : "0 ") + dv_pstatus_report(r, r->id.c_str()));
	if (order.empty() && w->pstatus->real)
		out.push_back("1 0 -");
	return (out);
}

void
dv_pstatus_restore(struct dv_window *w, const char *line)
{
	std::string_view	v(line);
	bool			real, osc9;

	if (v.size() < 5 || v[1] != ' ' || v[3] != ' ')
		return;
	real = v[0] == '1';
	osc9 = v[2] == '1';
	v.remove_prefix(4);
	if (v != "-") {
		std::string	report(v);

		ps_of(w)->real = false;
		dv_pstatus_osc(w, report.data(), report.size());
		if (!w->pstatus->recs.empty()) {
			/* The record just made is the newest. */
			auto	newest = std::max_element(w->pstatus->recs.begin(),
			    w->pstatus->recs.end(),
			    [](const dv_pstatus_rec &a, const dv_pstatus_rec &b) {
				return (a.seq < b.seq);
			});

			newest->osc9 = osc9;
		}
	}
	ps_of(w)->real = real;
}
