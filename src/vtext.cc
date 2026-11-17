/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Text for vector scenes drawn here, for a terminal that cannot draw them.
 *
 * A scene's text record carries a layout object: runs of text, each with a
 * font, a size and paints, set on a baseline (Silkie's vector protocol,
 * "Text layout and measurement"). A terminal with vector images shapes it
 * and keeps the outlines. For one without, the scene is drawn by the
 * rasterizer in src/vector, which draws paths, so the text is made paths
 * here: shaped by HarfBuzz, its glyph outlines taken from the font, its
 * decorations added as rectangles. The same layout answers the measuring
 * service, so a program is told the size of what will be drawn.
 *
 * The layout and the shaping are Silkie's, by the same author, brought over
 * as the rasterizer was (docs/CLEANROOM.md): layout() and segments() from
 * its kitty/fonts/vector.py, shape() from its kitty/vector-text.h, both
 * written against HarfBuzz alone. Finding fonts is not: Silkie asks kitty's
 * font code, which is not here. The fonts of this machine are found by
 * reading the font files in its font directories, by HarfBuzz as well, and
 * one is chosen by its names, weight and slant as written below.
 */

#include "vtext.hpp"

#include <hb.h>
#include <hb-ot.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

extern "C" bool vector_color_paints(const char *s, size_t len);

namespace {

struct bad : std::runtime_error {
	using std::runtime_error::runtime_error;
};

/* ---- JSON, as much of it as a layout object is ---- */

struct J {
	enum kind { Null, Bool, Num, Str, Arr, Obj };
	kind					 t = Null;
	bool					 b = false;
	double					 n = 0;
	std::string				 s;
	std::vector<J>				 a;
	std::vector<std::pair<std::string, J>>	 o;

	const J *get(const char *key) const {
		if (t == Obj)
			for (const auto &[k, v] : o)
				if (k == key)
					return (&v);
		return (nullptr);
	}
};

/*
 * A layout object is an object of scalars, with one array of such objects
 * and a few arrays of strings: three levels. The parser recurses, on the
 * stack, into text a program sent, so it stops well before the stack does;
 * nothing the protocol defines comes near this.
 */
constexpr int JSON_DEPTH = 64;

struct parser {
	const std::string	&in;
	size_t			 at = 0;

	void ws() {
		while (at < in.size() && (in[at] == ' ' || in[at] == '\t' ||
		    in[at] == '\n' || in[at] == '\r'))
			at++;
	}
	void utf8(std::string *out, uint32_t cp) {
		if (cp < 0x80)
			*out += (char)cp;
		else if (cp < 0x800) {
			*out += (char)(0xc0 | (cp >> 6));
			*out += (char)(0x80 | (cp & 0x3f));
		} else if (cp < 0x10000) {
			*out += (char)(0xe0 | (cp >> 12));
			*out += (char)(0x80 | ((cp >> 6) & 0x3f));
			*out += (char)(0x80 | (cp & 0x3f));
		} else {
			*out += (char)(0xf0 | (cp >> 18));
			*out += (char)(0x80 | ((cp >> 12) & 0x3f));
			*out += (char)(0x80 | ((cp >> 6) & 0x3f));
			*out += (char)(0x80 | (cp & 0x3f));
		}
	}
	uint32_t hex4() {
		uint32_t	v = 0;

		if (at + 4 > in.size())
			throw bad("Bad JSON escape");
		for (int i = 0; i < 4; i++) {
			char	c = in[at++];

			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= (uint32_t)(c - '0');
			else if (c >= 'a' && c <= 'f')
				v |= (uint32_t)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				v |= (uint32_t)(c - 'A' + 10);
			else
				throw bad("Bad JSON escape");
		}
		return (v);
	}
	std::string str() {
		std::string	out;

		at++;				/* the opening quote */
		for (;;) {
			if (at >= in.size())
				throw bad("Unterminated JSON string");
			char	c = in[at++];

			if (c == '"')
				return (out);
			if (c != '\\') {
				out += c;
				continue;
			}
			if (at >= in.size())
				throw bad("Bad JSON escape");
			c = in[at++];
			switch (c) {
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'u': {
				uint32_t	cp = hex4();

				if (cp >= 0xd800 && cp < 0xdc00 && at + 1 < in.size() &&
				    in[at] == '\\' && in[at + 1] == 'u') {
					at += 2;
					uint32_t	lo = hex4();

					if (lo >= 0xdc00 && lo < 0xe000)
						cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
				}
				utf8(&out, cp);
				break;
			}
			default: out += c; break;	/* quote, slash, backslash */
			}
		}
	}
	J value(int depth) {
		J	v;

		if (depth > JSON_DEPTH)
			throw bad("JSON nested too deeply for a layout");
		ws();
		if (at >= in.size())
			throw bad("Truncated JSON");
		char	c = in[at];

		if (c == '{') {
			v.t = J::Obj;
			at++;
			ws();
			if (at < in.size() && in[at] == '}') {
				at++;
				return (v);
			}
			for (;;) {
				ws();
				if (at >= in.size() || in[at] != '"')
					throw bad("Expected a JSON key");
				std::string	k = str();

				ws();
				if (at >= in.size() || in[at++] != ':')
					throw bad("Expected a colon in JSON");
				v.o.push_back({ std::move(k), value(depth + 1) });
				ws();
				if (at < in.size() && in[at] == ',') {
					at++;
					continue;
				}
				if (at < in.size() && in[at] == '}') {
					at++;
					return (v);
				}
				throw bad("Bad JSON object");
			}
		}
		if (c == '[') {
			v.t = J::Arr;
			at++;
			ws();
			if (at < in.size() && in[at] == ']') {
				at++;
				return (v);
			}
			for (;;) {
				v.a.push_back(value(depth + 1));
				ws();
				if (at < in.size() && in[at] == ',') {
					at++;
					continue;
				}
				if (at < in.size() && in[at] == ']') {
					at++;
					return (v);
				}
				throw bad("Bad JSON array");
			}
		}
		if (c == '"') {
			v.t = J::Str;
			v.s = str();
			return (v);
		}
		if (in.compare(at, 4, "true") == 0) {
			v.t = J::Bool;
			v.b = true;
			at += 4;
			return (v);
		}
		if (in.compare(at, 5, "false") == 0) {
			v.t = J::Bool;
			at += 5;
			return (v);
		}
		if (in.compare(at, 4, "null") == 0) {
			at += 4;
			return (v);
		}
		char	*end = nullptr;

		v.n = strtod(in.c_str() + at, &end);
		if (end == in.c_str() + at)
			throw bad("Bad JSON value");
		v.t = J::Num;
		at = (size_t)(end - in.c_str());
		return (v);
	}
};

J
parse_object(const std::string &text)
{
	parser	p{ text };
	J	v = p.value(0);

	p.ws();
	if (p.at != text.size() || v.t != J::Obj)
		throw bad("Expected a JSON object");
	return (v);
}

std::string
quoted(const std::string &s)
{
	std::string	out = "\"";

	for (unsigned char c : s) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += (char)c;
		} else if (c < 0x20) {
			char	u[8];

			snprintf(u, sizeof u, "\\u%04x", c);
			out += u;
		} else
			out += (char)c;
	}
	return (out + "\"");
}

/* A number as JSON has it: the shortest that reads back the same. */
std::string
num(double v)
{
	char	t[40];
	auto	r = std::to_chars(t, t + sizeof t, v);

	return (std::isfinite(v) ? std::string(t, r.ptr) : "0");
}

std::string
unbase64(const char *in, size_t len)
{
	std::string	out;
	uint32_t	acc = 0;
	int		bits = 0;

	for (size_t i = 0; i < len; i++) {
		char	c = in[i];
		int	v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 :
			    c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 :
			    c == '=' ? -2 : -1;

		if (v == -2)
			break;
		if (v < 0)
			throw bad("Bad base64");
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += (char)((acc >> bits) & 0xff);
		}
	}
	return (out);
}

std::vector<uint32_t>
codepoints(const std::string &s)
{
	std::vector<uint32_t>	out;

	for (size_t i = 0; i < s.size(); ) {
		unsigned char	c = (unsigned char)s[i];
		uint32_t	cp = c;
		int		need = 0;

		if (c >= 0xf0) {
			cp = c & 0x07;
			need = 3;
		} else if (c >= 0xe0) {
			cp = c & 0x0f;
			need = 2;
		} else if (c >= 0xc0) {
			cp = c & 0x1f;
			need = 1;
		}
		i++;
		while (need-- > 0 && i < s.size() && ((unsigned char)s[i] & 0xc0) == 0x80)
			cp = (cp << 6) | ((unsigned char)s[i++] & 0x3f);
		out.push_back(cp);
	}
	return (out);
}

/* ---- fonts ---- */

struct axis {
	char	tag[5];
	double	min, def, max;
};

struct info {
	unsigned		upem = 0, glyphs = 0, weight = 400;
	double			slant = 0;
	std::vector<axis>	axes;
};

info
font_info(hb_font_t *font)
{
	hb_face_t	*face = hb_font_get_face(font);
	info		 i;
	unsigned	 n = hb_ot_var_get_axis_count(face), len = 0;

	i.upem = hb_face_get_upem(face);
	i.glyphs = hb_face_get_glyph_count(face);
	if (n != 0) {
		std::vector<hb_ot_var_axis_info_t>	a(n);

		hb_ot_var_get_axis_infos(face, 0, &n, a.data());
		for (unsigned k = 0; k < n; k++) {
			axis	x = { {}, a[k].min_value, a[k].default_value, a[k].max_value };

			hb_tag_to_string(a[k].tag, x.tag);
			x.tag[4] = '\0';
			i.axes.push_back(x);
		}
	}
	hb_blob_t		*table = hb_face_reference_table(face, HB_TAG('O', 'S', '/', '2'));
	const unsigned char	*b = (const unsigned char *)hb_blob_get_data(table, &len);

	if (len >= 6)
		i.weight = (unsigned)b[4] * 256 + b[5];
	hb_blob_destroy(table);
	table = hb_face_reference_table(face, HB_TAG('p', 'o', 's', 't'));
	b = (const unsigned char *)hb_blob_get_data(table, &len);
	if (len >= 8)
		i.slant = (int32_t)((uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 |
		    (uint32_t)b[6] << 8 | b[7]) / 65536.0;
	hb_blob_destroy(table);
	return (i);
}

std::string
info_members(const info &i)
{
	std::string	out = "\"upem\":" + std::to_string(i.upem) + ",\"glyphs\":" +
			    std::to_string(i.glyphs) + ",\"axes\":[";

	for (size_t k = 0; k < i.axes.size(); k++)
		out += std::string(k ? "," : "") + "{\"tag\":" + quoted(i.axes[k].tag) +
		    ",\"minimum\":" + num(i.axes[k].min) + ",\"default\":" +
		    num(i.axes[k].def) + ",\"maximum\":" + num(i.axes[k].max) + "}";
	return (out + "],\"weight\":" + std::to_string(i.weight) + ",\"slant\":" +
	    num(i.slant));
}

/* A face of a font file on this machine: what choosing among them needs,
 * read once; the font itself opened when it is first chosen. */
struct face {
	std::string	 path;
	unsigned	 index = 0;
	std::string	 family, style, postscript;
	unsigned	 weight = 400;
	double		 slant = 0;
	bool		 monospace = false, italic = false;
	hb_font_t	*font = nullptr;
	info		 inf;
};

std::vector<face>				 system_faces;
bool						 scanned;
/* Uploaded by programs, by handle. A handle is never reused. */
std::unordered_map<std::string, hb_font_t *>	 session_fonts;

std::string
fold(const std::string &s)
{
	std::string	out = s;

	for (char &c : out)
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	return (out);
}

std::string
face_name(hb_face_t *f, hb_ot_name_id_t id)
{
	unsigned	len = hb_ot_name_get_utf8(f, id, HB_LANGUAGE_INVALID, nullptr, nullptr);

	if (len == 0)
		len = hb_ot_name_get_utf8(f, id, hb_language_from_string("en", -1),
		    nullptr, nullptr);
	if (len == 0)
		return ("");
	std::string	out(len + 1, '\0');
	unsigned	cap = len + 1;

	if (hb_ot_name_get_utf8(f, id, HB_LANGUAGE_INVALID, &cap, out.data()) == 0) {
		cap = len + 1;
		hb_ot_name_get_utf8(f, id, hb_language_from_string("en", -1), &cap, out.data());
	}
	out.resize(cap);
	return (out);
}

/*
 * A font file's bytes, or nullptr when it cannot be read. HarfBuzz before
 * 2.8.2 has no _or_fail form: its plain form gives the empty blob instead.
 */
hb_blob_t *
blob_from_file(const char *path)
{
#if HB_VERSION_ATLEAST(2, 8, 2)
	return (hb_blob_create_from_file_or_fail(path));
#else
	hb_blob_t	*blob = hb_blob_create_from_file(path);

	if (hb_blob_get_length(blob) == 0) {
		hb_blob_destroy(blob);
		return (nullptr);
	}
	return (blob);
#endif
}

void
scan_file(const std::string &path)
{
	hb_blob_t	*blob = blob_from_file(path.c_str());

	if (blob == nullptr)
		return;
	unsigned	n = hb_face_count(blob);

	for (unsigned i = 0; i < n; i++) {
		hb_face_t	*f = hb_face_create(blob, i);
		face		 x;
		unsigned	 len = 0;

		if (hb_face_get_glyph_count(f) == 0) {
			hb_face_destroy(f);
			continue;
		}
		x.path = path;
		x.index = i;
		x.family = face_name(f, HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY);
		if (x.family.empty())
			x.family = face_name(f, HB_OT_NAME_ID_FONT_FAMILY);
		x.style = face_name(f, HB_OT_NAME_ID_TYPOGRAPHIC_SUBFAMILY);
		if (x.style.empty())
			x.style = face_name(f, HB_OT_NAME_ID_FONT_SUBFAMILY);
		x.postscript = face_name(f, HB_OT_NAME_ID_POSTSCRIPT_NAME);
		hb_blob_t		*t = hb_face_reference_table(f, HB_TAG('O', 'S', '/', '2'));
		const unsigned char	*b = (const unsigned char *)hb_blob_get_data(t, &len);

		if (len >= 6)
			x.weight = (unsigned)b[4] * 256 + b[5];
		if (len >= 64)			/* fsSelection: italic, oblique */
			x.italic = (b[63] & 1) != 0 || (b[62] & 2) != 0;
		hb_blob_destroy(t);
		t = hb_face_reference_table(f, HB_TAG('p', 'o', 's', 't'));
		b = (const unsigned char *)hb_blob_get_data(t, &len);
		if (len >= 16) {
			x.slant = (int32_t)((uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 |
			    (uint32_t)b[6] << 8 | b[7]) / 65536.0;
			x.monospace = (b[12] | b[13] | b[14] | b[15]) != 0;
		}
		hb_blob_destroy(t);
		if (!x.italic) {
			std::string	st = fold(x.style);

			x.italic = st.find("italic") != std::string::npos ||
			    st.find("oblique") != std::string::npos;
		}
		hb_face_destroy(f);
		if (!x.family.empty())
			system_faces.push_back(std::move(x));
	}
	hb_blob_destroy(blob);
}

/*
 * The font directories of this machine. ODIN_SEAT_FONT_PATH, directories
 * separated by colons, replaces them: for a machine that keeps its fonts
 * somewhere else, and for a test that wants to know what it will find.
 */
void
scan(void)
{
	std::vector<std::string>	dirs;
	const char			*env = getenv("ODIN_SEAT_FONT_PATH"), *home = getenv("HOME");

	if (scanned)
		return;
	scanned = true;
	if (env != nullptr && *env != '\0') {
		std::string	all = env;
		size_t		at = 0;

		while (at <= all.size()) {
			size_t	end = all.find(':', at);

			if (end == std::string::npos)
				end = all.size();
			if (end > at)
				dirs.push_back(all.substr(at, end - at));
			at = end + 1;
		}
	} else {
		const char	*xdg = getenv("XDG_DATA_HOME"), *xdgs = getenv("XDG_DATA_DIRS");

		dirs = { "/System/Library/Fonts", "/Library/Fonts", "/usr/share/fonts",
		    "/usr/local/share/fonts", "/usr/X11R6/lib/X11/fonts",
		    "/opt/homebrew/share/fonts" };
		if (home != nullptr) {
			dirs.push_back(std::string(home) + "/Library/Fonts");
			dirs.push_back(std::string(home) + "/.fonts");
			dirs.push_back(std::string(home) + "/.local/share/fonts");
		}
		if (xdg != nullptr && *xdg != '\0')
			dirs.push_back(std::string(xdg) + "/fonts");
		if (xdgs != nullptr) {
			std::string	all = xdgs;
			size_t		at = 0;

			while (at <= all.size()) {
				size_t	end = all.find(':', at);

				if (end == std::string::npos)
					end = all.size();
				if (end > at)
					dirs.push_back(all.substr(at, end - at) + "/fonts");
				at = end + 1;
			}
		}
	}
	std::vector<std::string>	files;

	for (const auto &d : dirs) {
		std::error_code	ec;
		auto		it = std::filesystem::recursive_directory_iterator(d,
				    std::filesystem::directory_options::skip_permission_denied |
				    std::filesystem::directory_options::follow_directory_symlink, ec);

		for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
			std::string	ext = fold(it->path().extension().string());

			if (ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc")
				files.push_back(it->path().string());
		}
	}
	std::sort(files.begin(), files.end());
	files.erase(std::unique(files.begin(), files.end()), files.end());
	for (const auto &f : files)
		scan_file(f);
	std::sort(system_faces.begin(), system_faces.end(), [](const face &a, const face &b) {
		return (std::tie(a.family, a.style, a.postscript) <
		    std::tie(b.family, b.style, b.postscript));
	});
}

hb_font_t *
face_font(face *f)
{
	if (f->font == nullptr) {
		hb_blob_t	*blob = blob_from_file(f->path.c_str());

		if (blob == nullptr)
			throw bad("Font file is gone");
		hb_face_t	*hf = hb_face_create(blob, f->index);

		hb_blob_destroy(blob);
		f->font = hb_font_create(hf);
		hb_face_destroy(hf);
		hb_ot_font_set_funcs(f->font);
		f->inf = font_info(f->font);
	}
	return (f->font);
}

/* What a role means where nothing is configured: the first of these that is
 * installed. The default is a monospaced face, as a terminal's own font is. */
const std::vector<const char *> &
role_families(const std::string &role)
{
	static const std::vector<const char *>	mono = { "Iosevka", "Menlo", "DejaVu Sans Mono",
	    "Liberation Mono", "Noto Sans Mono", "SF Mono", "Monaco", "Courier New", "FreeMono" };
	static const std::vector<const char *>	sans = { "Helvetica Neue", "Helvetica", "Arial",
	    "DejaVu Sans", "Liberation Sans", "Noto Sans", "FreeSans" };
	static const std::vector<const char *>	serif = { "Times New Roman", "Times", "DejaVu Serif",
	    "Liberation Serif", "Noto Serif", "FreeSerif" };

	if (role == "sans" || role == "sans-serif")
		return (sans);
	if (role == "serif")
		return (serif);
	return (mono);
}

/* The face of a family nearest a weight and slope: Silkie's order of
 * preference, the slope first, then the weight, then by name. */
face *
family_face(const std::string &name, double weight, bool italic)
{
	std::string	want = fold(name);
	face		*best = nullptr;

	scan();
	for (auto &f : system_faces)
		if (fold(f.postscript) == want)
			return (&f);
	for (auto &f : system_faces) {
		if (fold(f.family) != want)
			continue;
		if (best == nullptr ||
		    std::make_tuple(f.italic != italic, std::fabs((double)f.weight - weight), f.postscript) <
		    std::make_tuple(best->italic != italic, std::fabs((double)best->weight - weight),
		    best->postscript))
			best = &f;
	}
	return (best);
}

face *
role_face(const std::string &role, double weight, bool italic)
{
	for (const char *fam : role_families(role)) {
		face	*f = family_face(fam, weight, italic);

		if (f != nullptr)
			return (f);
	}
	scan();
	/* None of them: any monospaced face, then any face at all. */
	for (int any = 0; any < 2; any++)
		for (auto &f : system_faces)
			if ((any || f.monospace) && !f.italic)
				return (family_face(f.family, weight, italic));
	return (nullptr);
}

/* A run's style: its own fields, then the layout's. */
struct style {
	const J	*run, *base;

	const J *get(const char *k) const {
		const J	*v = run != nullptr ? run->get(k) : nullptr;

		return (v != nullptr ? v : base != nullptr ? base->get(k) : nullptr);
	}
	double number(const char *k, double def) const {
		const J	*v = get(k);

		if (v == nullptr)
			return (def);
		if (v->t != J::Num || !std::isfinite(v->n))
			throw bad("Expected a finite number");
		return (v->n);
	}
	std::string string(const char *k, const char *def) const {
		const J	*v = get(k);

		if (v == nullptr)
			return (def);
		if (v->t != J::Str)
			throw bad(std::string(k) + " must be a string");
		return (v->s);
	}
	bool boolean(const char *k) const {
		const J	*v = get(k);

		if (v == nullptr)
			return (false);
		if (v->t != J::Bool)
			throw bad(std::string(k) + " must be a boolean");
		return (v->b);
	}
};

struct resolved {
	hb_font_t			*font = nullptr;
	std::map<std::string, double>	 axes;
	double				 shear = 0;
};

/*
 * The font a style asks for. A session handle is that font. A family or a
 * PostScript name is the face of it nearest the weight and slope; a name
 * this machine does not have falls back to the role's font rather than
 * failing, since the program was written for another machine's fonts and
 * its text is better set in some font than not shown.
 */
resolved
resolve(const style &st, const std::string &font_override = "")
{
	resolved	r;
	std::string	name = font_override.empty() ? st.string("font", "") : font_override;
	double		weight = st.number("weight", 400), native = 0;
	bool		italic = st.boolean("italic");
	const J		*axes = font_override.empty() ? st.get("axes") : nullptr;
	info		 inf;

	if (weight < 100 || weight > 900)
		throw bad("weight must be between 100 and 900");
	if (axes != nullptr) {
		if (axes->t != J::Obj)
			throw bad("axes must map four-character OpenType tags to numbers");
		for (const auto &[k, v] : axes->o) {
			if (k.size() != 4 || v.t != J::Num || !std::isfinite(v.n))
				throw bad("axes must map four-character OpenType tags to numbers");
			r.axes[k] = v.n;
		}
	}
	if (!name.empty() && name[0] == '@') {
		auto	it = session_fonts.find(name.substr(1));

		if (it == session_fonts.end())
			throw bad("No such session font " + name);
		r.font = it->second;
		inf = font_info(r.font);
		native = inf.slant;
	} else {
		face	*f = name.empty() ? nullptr : family_face(name, weight, italic);

		if (f == nullptr)
			f = role_face(st.string("role", "default"), weight, italic);
		if (f == nullptr)
			throw bad("No fonts are installed");
		r.font = face_font(f);
		inf = f->inf;
		native = f->slant;
	}
	for (const auto &a : inf.axes) {
		double	want = strcmp(a.tag, "wght") == 0 ? weight :
			    strcmp(a.tag, "ital") == 0 ? (italic ? 1.0 : 0.0) : NAN;

		if (!std::isnan(want) && !r.axes.contains(a.tag))
			r.axes[a.tag] = std::max(a.min, std::min(a.max, want));
	}
	for (const auto &[tag, value] : r.axes) {
		bool	ok = false;

		for (const auto &a : inf.axes)
			ok |= tag == a.tag && value >= a.min && value <= a.max;
		if (!ok)
			throw bad("Unsupported value for font axis " + tag);
	}
	if (r.axes.contains("slnt"))
		native = r.axes["slnt"];
	if (st.get("slant") != nullptr) {
		double	slant = st.number("slant", 0);

		if (slant < -15 || slant > 15)
			throw bad("slant must be between -15 and 15 degrees");
		r.shear = std::tan(native * M_PI / 180) - std::tan(slant * M_PI / 180);
	}
	return (r);
}

bool
covers(hb_font_t *font, const uint32_t *cp, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		hb_codepoint_t	g;

		/* Default ignorables are handled by shaping, not font fallback. */
		if (cp[i] == 0x200c || cp[i] == 0x200d || (cp[i] >= 0xfe00 && cp[i] <= 0xfe0f) ||
		    (cp[i] >= 0xe0100 && cp[i] <= 0xe01ef))
			continue;
		if (!hb_font_get_nominal_glyph(font, cp[i], &g))
			return (false);
	}
	return (true);
}

/* ---- shaping ---- */

typedef OdinTextCmd cmd;

struct shaped {
	std::vector<cmd>	path;
	double			adv_x = 0, adv_y = 0;
	double			ascent = 0, descent = 0, gap = 0, underline_y = 0, thickness = 0,
				strike_y = 0;
	std::vector<unsigned>	missing;
	/* A glyph has ink and no outline to draw it with: only drawing fails. */
	bool			no_outline = false;
};

struct pen {
	std::vector<cmd>	*out;
	double			 scale, x, y, shear;

	void add(int op, const float *pts, int n) {
		cmd	c = { op, { 0, 0, 0, 0, 0, 0 } };

		for (int i = 0; i < n; i++) {
			c.p[2 * i] = x + (pts[2 * i] + shear * pts[2 * i + 1]) * scale;
			c.p[2 * i + 1] = y - pts[2 * i + 1] * scale;
		}
		out->push_back(c);
	}
};

/*
 * Glyph outlines come from HarfBuzz's draw API, 4.0 and later. A build
 * against an older HarfBuzz (Kylin V10 has 2.6.4) still shapes, lays out
 * and measures text, but has no outlines to draw: a glyph with ink is then
 * the error a bitmap glyph is (shape), never blank text.
 */
#if HB_VERSION_ATLEAST(4, 0, 0)
#define DV_HB_OUTLINES	1

void
draw_move(hb_draw_funcs_t *, void *d, hb_draw_state_t *, float x, float y, void *)
{
	float	p[] = { x, y };

	((pen *)d)->add(0, p, 1);
}

void
draw_line(hb_draw_funcs_t *, void *d, hb_draw_state_t *, float x, float y, void *)
{
	float	p[] = { x, y };

	((pen *)d)->add(1, p, 1);
}

void
draw_quad(hb_draw_funcs_t *, void *d, hb_draw_state_t *, float cx, float cy, float x,
    float y, void *)
{
	float	p[] = { cx, cy, x, y };

	((pen *)d)->add(2, p, 2);
}

void
draw_cubic(hb_draw_funcs_t *, void *d, hb_draw_state_t *, float c1x, float c1y, float c2x,
    float c2y, float x, float y, void *)
{
	float	p[] = { c1x, c1y, c2x, c2y, x, y };

	((pen *)d)->add(3, p, 3);
}

void
draw_close(hb_draw_funcs_t *, void *d, hb_draw_state_t *, void *)
{
	((pen *)d)->add(4, nullptr, 0);
}

hb_draw_funcs_t *
draw_funcs(void)
{
	static hb_draw_funcs_t	*f;

	if (f == nullptr) {
		f = hb_draw_funcs_create();
		hb_draw_funcs_set_move_to_func(f, draw_move, nullptr, nullptr);
		hb_draw_funcs_set_line_to_func(f, draw_line, nullptr, nullptr);
		hb_draw_funcs_set_quadratic_to_func(f, draw_quad, nullptr, nullptr);
		hb_draw_funcs_set_cubic_to_func(f, draw_cubic, nullptr, nullptr);
		hb_draw_funcs_set_close_path_func(f, draw_close, nullptr, nullptr);
		hb_draw_funcs_make_immutable(f);
	}
	return (f);
}
#endif

shaped
shape(const resolved &r, const uint32_t *text, size_t n, double size,
    const std::vector<std::string> &features, const std::string &direction,
    const std::string &language, const std::string &script, bool boxes = false)
{
	shaped		 out;
	hb_face_t	*face = hb_font_get_face(r.font);
	hb_font_t	*font = hb_font_create(face);
	hb_buffer_t	*buf = hb_buffer_create();
	unsigned	 upem = hb_face_get_upem(face), count = 0;

	hb_ot_font_set_funcs(font);
	hb_font_set_scale(font, (int)upem, (int)upem);
	if (!r.axes.empty()) {
		std::vector<hb_variation_t>	v;

		for (const auto &[tag, value] : r.axes)
			v.push_back({ hb_tag_from_string(tag.c_str(), -1), (float)value });
		hb_font_set_variations(font, v.data(), (unsigned)v.size());
	}
	std::vector<hb_feature_t>	ff(features.size());
	bool				ok = true;

	for (size_t i = 0; i < features.size() && ok; i++)
		ok = hb_feature_from_string(features[i].c_str(), -1, &ff[i]);
	/* HarfBuzz counts a buffer in an int. */
	if (ok && n <= (size_t)INT32_MAX) {
		for (size_t i = 0; i < n; i++)
			hb_buffer_add(buf, text[i], (unsigned)i);
		hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
		if (!direction.empty())
			hb_buffer_set_direction(buf, hb_direction_from_string(direction.c_str(), -1));
		if (!language.empty())
			hb_buffer_set_language(buf, hb_language_from_string(language.c_str(), -1));
		if (!script.empty())
			hb_buffer_set_script(buf, hb_script_from_string(script.c_str(), -1));
		hb_buffer_guess_segment_properties(buf);
		hb_shape(font, buf, ff.data(), (unsigned)ff.size());
		ok = hb_buffer_allocation_successful(buf);
	} else
		ok = false;
	if (!ok) {
		hb_buffer_destroy(buf);
		hb_font_destroy(font);
		throw bad(n > (size_t)INT32_MAX ? "Text exceeds HarfBuzz buffer address space" :
		    "Invalid OpenType feature");
	}
	hb_glyph_info_t		*gi = hb_buffer_get_glyph_infos(buf, &count);
	hb_glyph_position_t	*gp = hb_buffer_get_glyph_positions(buf, &count);
	const double		 scale = size / upem;
	pen			 p = { &out.path, scale, 0, 0, r.shear };
	double			 x = 0, y = 0;
	bool			 no_outline = false;

	for (unsigned i = 0; i < count; i++) {
		size_t	before = out.path.size();

		p.x = x + gp[i].x_offset * scale;
		p.y = y - gp[i].y_offset * scale;
		if (gi[i].codepoint == 0)
			out.missing.push_back(gi[i].cluster);
#if HB_VERSION_ATLEAST(7, 0, 0)
		hb_font_draw_glyph(font, gi[i].codepoint, draw_funcs(), &p);
#elif defined(DV_HB_OUTLINES)
		hb_font_get_glyph_shape(font, gi[i].codepoint, draw_funcs(), &p);
#else
		(void)p;
#endif
		if (out.path.size() == before) {
			hb_glyph_extents_t	e = {};

			/* Ink, and no outline to draw it with: a bitmap or a
			 * colour glyph, which outline text has none of. */
			if (hb_font_get_glyph_extents(font, gi[i].codepoint, &e) &&
			    (e.width != 0 || e.height != 0)) {
				no_outline = true;
				/*
				 * Measuring (boxes): its ink box stands in for the
				 * outline it would have filled, which is all a
				 * measurement's ink is.
				 */
				if (boxes) {
					float	l = (float)e.x_bearing, t = (float)e.y_bearing;
					float	rr = l + (float)e.width, b = t + (float)e.height;
					float	q[][2] = { { l, t }, { rr, t }, { rr, b }, { l, b } };

					p.add(0, q[0], 1);
					for (int k = 1; k < 4; k++)
						p.add(1, q[k], 1);
					p.add(4, nullptr, 0);
				}
			}
		}
		x += gp[i].x_advance * scale;
		y -= gp[i].y_advance * scale;
	}
	hb_font_extents_t	ext = {};
	hb_position_t		under = 0, thick = 0, strike = 0;

	hb_font_get_h_extents(font, &ext);
	hb_ot_metrics_get_position(font, HB_OT_METRICS_TAG_UNDERLINE_OFFSET, &under);
	hb_ot_metrics_get_position(font, HB_OT_METRICS_TAG_UNDERLINE_SIZE, &thick);
	hb_ot_metrics_get_position(font, HB_OT_METRICS_TAG_STRIKEOUT_OFFSET, &strike);
	out.adv_x = x;
	out.adv_y = y;
	out.ascent = ext.ascender * scale;
	out.descent = -ext.descender * scale;
	out.gap = ext.line_gap * scale;
	out.underline_y = -under * scale;
	out.thickness = thick * scale;
	out.strike_y = -strike * scale;
	hb_buffer_destroy(buf);
	hb_font_destroy(font);
	out.no_outline = no_outline;
	return (out);
}

/* ---- layout ---- */

struct run_out {
	std::vector<cmd>	path;
	std::string		fill, stroke;
	double			width = 0;
};

struct metrics {
	double			width = 0, height = 0, adv_x = 0, adv_y = 0, ascent = 0, descent = 0;
	bool			has_ink = false;
	double			ink[4] = { 0, 0, 0, 0 };
	std::vector<size_t>	missing;
};

void
rect(std::vector<cmd> *out, double x, double y, double w, double h)
{
	out->push_back({ 0, { x, y } });
	out->push_back({ 1, { x + w, y } });
	out->push_back({ 1, { x + w, y + h } });
	out->push_back({ 1, { x, y + h } });
	out->push_back({ 4, {} });
}

/* The exact extrema of a path's lines and curves, not its control boxes. */
bool
bounds(const std::vector<cmd> &path, double b[4])
{
	bool	any = false;
	double	x = 0, y = 0, sx = 0, sy = 0;
	auto	take = [&](double px, double py) {
		if (!any) {
			b[0] = b[2] = px;
			b[1] = b[3] = py;
			any = true;
		}
		b[0] = std::min(b[0], px);
		b[1] = std::min(b[1], py);
		b[2] = std::max(b[2], px);
		b[3] = std::max(b[3], py);
	};

	for (const auto &c : path) {
		if (c.op == 0) {
			x = sx = c.p[0];
			y = sy = c.p[1];
			continue;
		}
		if (c.op == 4 || c.op == 1) {
			double	tx = c.op == 4 ? sx : c.p[0], ty = c.op == 4 ? sy : c.p[1];

			take(x, y);
			take(tx, ty);
			x = tx;
			y = ty;
			continue;
		}
		int	deg = c.op == 2 ? 2 : 3;
		double	ex = c.p[2 * deg - 2], ey = c.p[2 * deg - 1];

		take(x, y);
		take(ex, ey);
		for (int ax = 0; ax < 2; ax++) {
			double	v[4] = { ax ? y : x, c.p[ax], c.p[2 + ax], deg == 3 ? c.p[4 + ax] : 0 };
			double	ts[2];
			int	nt = 0;

			if (deg == 2) {
				double	den = v[0] - 2 * v[1] + v[2];

				if (den != 0)
					ts[nt++] = (v[0] - v[1]) / den;
			} else {
				double	a = -v[0] + 3 * v[1] - 3 * v[2] + v[3];
				double	bb = 2 * (v[0] - 2 * v[1] + v[2]), cc = v[1] - v[0];

				if (std::fabs(a) < 1e-15) {
					if (bb != 0)
						ts[nt++] = -cc / bb;
				} else {
					double	disc = bb * bb - 4 * a * cc;

					if (disc >= 0) {
						ts[nt++] = (-bb + std::sqrt(disc)) / (2 * a);
						ts[nt++] = (-bb - std::sqrt(disc)) / (2 * a);
					}
				}
			}
			for (int k = 0; k < nt; k++) {
				double	t = ts[k];

				if (!(t > 0 && t < 1))
					continue;
				double	q[4] = { v[0], v[1], v[2], v[3] };

				for (int m = deg; m > 0; m--)
					for (int j = 0; j < m; j++)
						q[j] = q[j] * (1 - t) + q[j + 1] * t;
				if (ax)
					take(x, q[0]);
				else
					take(q[0], y);
			}
		}
		x = ex;
		y = ey;
	}
	return (any);
}

bool
combining(uint32_t cp)
{
	return (hb_unicode_combining_class(hb_unicode_funcs_get_default(), cp) !=
	    HB_UNICODE_COMBINING_CLASS_NOT_REORDERED);
}

/* A part of a line set in one font: the run's, or a fallback's for the
 * clusters the run's font has no glyph for. */
struct segment {
	std::vector<uint32_t>	text;
	resolved		font;
};

/* Fonts tried for what the run's font and its named fallbacks lack. */
const std::vector<const char *> &
last_resort(uint32_t cp)
{
	static const std::vector<const char *>	cjk = { "Noto Sans CJK SC", "Noto Sans CJK JP",
	    "Noto Sans CJK KR", "PingFang SC", "Hiragino Sans", "Hiragino Sans GB",
	    "Apple SD Gothic Neo", "Source Han Sans SC", "WenQuanYi Micro Hei", "Arial Unicode MS" };
	static const std::vector<const char *>	other = { "Noto Sans", "DejaVu Sans",
	    "Arial Unicode MS", "Apple Symbols", "Noto Sans Symbols", "Noto Sans Symbols2",
	    "Noto Sans Math", "STIX Two Math", "Symbola" };

	return ((cp >= 0x1100 && cp <= 0x11ff) || (cp >= 0x2e80 && cp <= 0xd7ff) ||
	    (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0x20000 && cp <= 0x3ffff) ? cjk : other);
}

std::vector<segment>
segments(const style &st, const std::vector<uint32_t> &text)
{
	std::vector<segment>		out;
	resolved			primary = resolve(st);
	std::vector<std::string>	named;
	const J				*fb = st.get("fallback");

	if (text.empty()) {
		out.push_back({ {}, primary });
		return (out);
	}
	if (fb != nullptr) {
		if (fb->t != J::Arr)
			throw bad("fallback must be an array of font names");
		for (const auto &f : fb->a) {
			if (f.t != J::Str)
				throw bad("fallback must be an array of font names");
			named.push_back(f.s);
		}
	}
	/* Combining marks, variation selectors and joined sequences stay with
	 * their base when a font is chosen for them. */
	size_t	start = 0;

	for (size_t i = 1; i <= text.size(); i++) {
		if (i < text.size()) {
			uint32_t	cp = text[i];

			if (combining(cp) || text[i - 1] == 0x200d || cp == 0x200c || cp == 0x200d ||
			    (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xe0100 && cp <= 0xe01ef))
				continue;
		}
		resolved	chosen = primary;

		if (!covers(primary.font, &text[start], i - start)) {
			bool	found = false;

			for (const auto &name : named) {
				if (name.empty() || (name[0] != '@' &&
				    family_face(name, 400, false) == nullptr))
					continue;
				resolved	f = resolve(st, name);

				if (covers(f.font, &text[start], i - start)) {
					chosen = f;
					found = true;
					break;
				}
			}
			for (const char *name : last_resort(text[start])) {
				if (found)
					break;
				if (family_face(name, 400, false) == nullptr)
					continue;
				resolved	f = resolve(st, name);

				if (covers(f.font, &text[start], i - start)) {
					chosen = f;
					found = true;
				}
			}
		}
		if (out.empty() || out.back().font.font != chosen.font)
			out.push_back({ {}, chosen });
		out.back().text.insert(out.back().text.end(), text.begin() + (long)start,
		    text.begin() + (long)i);
		start = i;
	}
	return (out);
}

/* draw: the runs will be drawn, so every glyph needs an outline. Measuring
 * needs only the metrics, which every HarfBuzz gives. */
void
layout(const J &obj, std::vector<run_out> *output, metrics *m, bool draw = true)
{
	const J			*runs = obj.get("runs");
	J			 single;
	std::vector<const J *>	 list;
	std::vector<std::array<double, 4>> logical;
	double			 x = 0, y = 0, width = 0, line_height = 0;
	size_t			 index = 0;

	if (runs != nullptr) {
		if (runs->t != J::Arr)
			throw bad("runs must contain objects with a text string");
		for (const auto &r : runs->a) {
			const J	*t = r.get("text");

			if (r.t != J::Obj || t == nullptr || t->t != J::Str)
				throw bad("runs must contain objects with a text string");
			list.push_back(&r);
		}
	} else {
		const J	*t = obj.get("text");

		if (t != nullptr && t->t != J::Str)
			throw bad("text must be a string");
		J	empty;

		empty.t = J::Str;
		single.t = J::Obj;
		single.o.push_back({ "text", t != nullptr ? *t : empty });
		list.push_back(&single);
	}
	for (const J *run : list) {
		style				st = { run, &obj };
		double				size = st.number("size", 16);
		std::vector<std::string>	features;
		const J				*fj = st.get("features");
		std::string			 direction = st.string("direction", "");
		std::string			 color = st.string("fill", "#000000");
		std::string			 under = st.string("underline", "none");
		bool				 strike = st.boolean("strike"), over = st.boolean("overline");
		std::vector<uint32_t>		 text = codepoints(run->get("text")->s);

		if (size <= 0)
			throw bad("size must be positive");
		if (fj != nullptr) {
			if (fj->t != J::Arr)
				throw bad("features must be an array of OpenType feature strings");
			for (const auto &f : fj->a) {
				if (f.t != J::Str)
					throw bad("features must be an array of OpenType feature strings");
				features.push_back(f.s);
			}
		}
		if (direction != "" && direction != "ltr" && direction != "rtl" &&
		    direction != "ttb" && direction != "btt")
			throw bad("Invalid text direction");
		if (under != "none" && under != "single" && under != "double" &&
		    under != "curly" && under != "dotted" && under != "dashed")
			throw bad("Invalid underline style");
		std::string	deco_color = st.string("underline_color", color.c_str());
		std::string	stroke = st.string("stroke", "none");
		std::string	background = st.string("background", "");
		double		stroke_width = st.number("stroke_width", 1);

		/* The lines of the run: a newline starts another. */
		for (size_t from = 0, part = 0; from <= text.size(); part++) {
			size_t	to = from;

			while (to < text.size() && text[to] != '\n')
				to++;
			if (part != 0) {
				width = std::max(width, x);
				x = 0;
				y += line_height != 0 ? line_height : size * 1.2;
				line_height = 0;
				index++;
			}
			std::vector<uint32_t>	line(text.begin() + (long)from, text.begin() + (long)to);

			for (const auto &seg : segments(st, line)) {
				shaped	sh = shape(seg.font, seg.text.data(), seg.text.size(), size,
					    features, direction, st.string("language", ""),
					    st.string("script", ""), !draw);
				double	a = sh.ascent, d = sh.descent, advance = sh.adv_x, dy = sh.adv_y;
				run_out	glyphs;

#ifdef DV_HB_OUTLINES
				if (draw && sh.no_outline)
					throw bad("Glyph has no supported vector outline");
#else
				if (draw && sh.no_outline)
					throw bad("Glyph outlines need HarfBuzz 4.0 or later");
#endif

				for (auto c : sh.path) {
					int	points = c.op <= 1 ? 1 : c.op == 2 ? 2 : c.op == 3 ? 3 : 0;

					for (int k = 0; k < points; k++) {
						c.p[2 * k] += x;
						c.p[2 * k + 1] += y;
					}
					glyphs.path.push_back(c);
				}
				m->ascent = std::max(m->ascent, a);
				m->descent = std::max(m->descent, d);
				line_height = std::max(line_height, a + d + sh.gap);
				logical.push_back({ std::min(x, x + advance), std::min(y, y + dy) - a,
				    std::max(x, x + advance), std::max(y, y + dy) + d });
				if (!background.empty()) {
					run_out	bg;

					rect(&bg.path, x, y - a, advance, a + d);
					bg.fill = background;
					bg.stroke = "none";
					output->push_back(std::move(bg));
				}
				glyphs.fill = color;
				glyphs.stroke = stroke;
				glyphs.width = stroke_width;
				output->push_back(std::move(glyphs));

				double			thickness = std::max(sh.thickness, size / 32);
				std::vector<cmd>	deco;

				if (under != "none") {
					double	uy = y + std::max(sh.underline_y, thickness);

					if (under == "single" || under == "double") {
						rect(&deco, x, uy, advance, thickness);
						if (under == "double")
							rect(&deco, x, uy + 2 * thickness, advance, thickness);
					} else {
						double	step = thickness * (under == "curly" || under == "dashed" ? 4 : 2);

						for (double at = 0; at < advance; at += under != "curly" ? step : step / 2) {
							double	len = std::min(step / 2, advance - at);

							if (under == "curly") {
								deco.push_back({ 0, { x + at, uy } });
								deco.push_back({ 3, { x + at + step / 4, uy - 2 * thickness,
								    x + at + step / 4, uy + 2 * thickness, x + at + len, uy } });
							} else
								rect(&deco, x + at, uy, len, thickness);
						}
					}
				}
				if (under == "curly" && !deco.empty()) {
					run_out	c;

					c.path = std::move(deco);
					c.fill = "none";
					c.stroke = deco_color;
					c.width = thickness;
					output->push_back(std::move(c));
					deco.clear();
				}
				if (strike)
					rect(&deco, x, y + sh.strike_y, advance, thickness);
				if (over)
					rect(&deco, x, y - a, advance, thickness);
				if (!deco.empty()) {
					run_out	c;

					c.path = std::move(deco);
					c.fill = deco_color;
					c.stroke = "none";
					output->push_back(std::move(c));
				}
				for (unsigned c : sh.missing)
					m->missing.push_back(index + c);
				index += seg.text.size();
				x += advance;
				y += dy;
			}
			if (to >= text.size())
				break;
			from = to + 1;
		}
	}
	width = std::max(width, x);

	style	top = { nullptr, &obj };
	double	ox = top.number("x", 0), oy = top.number("y", 0);
	double	angle = top.number("angle", 0) * M_PI / 180, c = std::cos(angle), s = std::sin(angle);

	for (auto &r : *output) {
		if (r.width < 0)
			throw bad("Invalid text paint or stroke width");
		for (const std::string *paint : { &r.fill, &r.stroke })
			if (*paint != "none" && !vector_color_paints(paint->data(), paint->size()))
				throw bad("Invalid vector color");
		for (auto &k : r.path) {
			int	points = k.op <= 1 ? 1 : k.op == 2 ? 2 : k.op == 3 ? 3 : 0;

			for (int i = 0; i < points; i++) {
				double	px = k.p[2 * i], py = k.p[2 * i + 1];

				k.p[2 * i] = ox + c * px - s * py;
				k.p[2 * i + 1] = oy + s * px + c * py;
			}
		}
		double	b[4];

		if ((r.fill != "none" || r.stroke != "none") && bounds(r.path, b)) {
			double	grow = r.stroke != "none" ? r.width / 2 : 0;

			if (!m->has_ink) {
				m->ink[0] = b[0] - grow;
				m->ink[1] = b[1] - grow;
				m->ink[2] = b[2] + grow;
				m->ink[3] = b[3] + grow;
				m->has_ink = true;
			}
			m->ink[0] = std::min(m->ink[0], b[0] - grow);
			m->ink[1] = std::min(m->ink[1], b[1] - grow);
			m->ink[2] = std::max(m->ink[2], b[2] + grow);
			m->ink[3] = std::max(m->ink[3], b[3] + grow);
		}
	}
	if (!logical.empty()) {
		double	l = logical[0][0], t = logical[0][1], r = logical[0][2], bt = logical[0][3];

		for (const auto &q : logical) {
			l = std::min(l, q[0]);
			t = std::min(t, q[1]);
			r = std::max(r, q[2]);
			bt = std::max(bt, q[3]);
		}
		width = r - l;
		m->height = bt - t;
	}
	m->width = width;
	m->adv_x = x;
	m->adv_y = y;
}

std::string
metrics_json(const metrics &m)
{
	std::string	out = "{\"width\":" + num(m.width) + ",\"height\":" + num(m.height) +
			    ",\"advance\":[" + num(m.adv_x) + "," + num(m.adv_y) + "],\"ascent\":" +
			    num(m.ascent) + ",\"descent\":" + num(m.descent) + ",\"ink\":";

	if (m.has_ink)
		out += "[" + num(m.ink[0]) + "," + num(m.ink[1]) + "," + num(m.ink[2]) + "," +
		    num(m.ink[3]) + "]";
	else
		out += "null";
	out += ",\"missing\":[";
	for (size_t i = 0; i < m.missing.size(); i++)
		out += (i ? "," : "") + std::to_string(m.missing[i]);
	return (out + "]}");
}

} /* namespace */

int
dv_vtext_upload(const std::string &handle, const std::string &request,
    std::string *info, std::string *err)
{
	try {
		J		 obj = parse_object(request);
		const J		*data = obj.get("data"), *idx = obj.get("index");
		unsigned	 index = 0;

		if (data == nullptr || data->t != J::Str)
			throw bad("An upload needs base64 font data");
		if (idx != nullptr) {
			if (idx->t != J::Num || idx->n < 0 || idx->n != std::floor(idx->n) ||
			    idx->n > (double)UINT32_MAX)
				throw bad("Invalid font or collection index");
			index = (unsigned)idx->n;
		}
		std::string	raw = unbase64(data->s.data(), data->s.size());

		/* An OpenType file addresses itself in 32 bits. */
		if (raw.empty() || raw.size() > (size_t)UINT32_MAX)
			throw bad("Font exceeds OpenType address space");
		hb_blob_t	*blob = hb_blob_create(raw.data(), (unsigned)raw.size(),
				    HB_MEMORY_MODE_DUPLICATE, nullptr, nullptr);

		if (index >= hb_face_count(blob)) {
			hb_blob_destroy(blob);
			throw bad("Invalid font or collection index");
		}
		hb_face_t	*face = hb_face_create(blob, index);

		hb_blob_destroy(blob);
		hb_font_t	*font = hb_font_create(face);

		hb_face_destroy(face);
		hb_ot_font_set_funcs(font);
		if (hb_face_get_glyph_count(hb_font_get_face(font)) == 0) {
			hb_font_destroy(font);
			throw bad("Font has no glyphs");
		}
		auto	it = session_fonts.find(handle);

		if (it != session_fonts.end())
			hb_font_destroy(it->second);
		session_fonts[handle] = font;
		if (info != nullptr)
			*info = info_members(font_info(font));
		return (1);
	} catch (const std::exception &e) {
		if (err != nullptr)
			*err = e.what();
		return (0);
	}
}

void
dv_vtext_release(const std::string &handle)
{
	auto	it = session_fonts.find(handle);

	if (it == session_fonts.end())
		return;
	hb_font_destroy(it->second);
	session_fonts.erase(it);
}

/* Whether text can be drawn: glyph outlines need HarfBuzz 4.0 or later. */
int
dv_vtext_outlines(void)
{
#ifdef DV_HB_OUTLINES
	return (1);
#else
	return (0);
#endif
}

int
dv_vtext_service(const std::string &request,
    const std::vector<std::pair<std::string, std::string>> &session,
    std::string *result, std::string *err)
{
	try {
		J		 obj = parse_object(request);
		const J		*opj = obj.get("op");
		std::string	 op = opj != nullptr && opj->t == J::Str ? opj->s : "";

		if (op == "fonts") {
			/* Without glyph outlines text is measured, not drawn. */
			std::string	out = std::string("{\"version\":1,\"features\":[") +
					    (dv_vtext_outlines() ? "\"text\"," : "") + "\"session-fonts\","
					    "\"measure\",\"clip-box\",\"hdr\",\"p3\",\"rec2020\"],\"fonts\":[";

			scan();
			for (size_t i = 0; i < system_faces.size(); i++) {
				const face	&f = system_faces[i];

				out += std::string(i ? "," : "") + "{\"family\":" + quoted(f.family) +
				    ",\"style\":" + quoted(f.style) + ",\"postscript_name\":" +
				    quoted(f.postscript) + ",\"weight\":" + std::to_string(f.weight) +
				    ",\"slant\":" + num(f.slant) + ",\"monospace\":" +
				    (f.monospace ? "true" : "false") + "}";
			}
			out += "],\"roles\":[\"default\",\"sans\",\"serif\"],\"session\":[";
			bool	first = true;

			for (const auto &[name, handle] : session) {
				auto	it = session_fonts.find(handle);

				if (it == session_fonts.end())
					continue;
				out += std::string(first ? "" : ",") + "{\"id\":" + quoted(name) + "," +
				    info_members(font_info(it->second)) + "}";
				first = false;
			}
			*result = out + "]}";
		} else if (op == "inspect") {
			style		st = { nullptr, &obj };
			resolved	r = resolve(st);

			*result = "{" + info_members(font_info(r.font)) + "}";
		} else if (op == "measure") {
			std::vector<run_out>	runs;
			metrics			m;

			layout(obj, &runs, &m, false);
			*result = metrics_json(m);
		} else
			throw bad("Unknown vector service operation");
		return (1);
	} catch (const std::exception &e) {
		if (err != nullptr)
			*err = e.what();
		return (0);
	}
}

int
odin_vector_text(const char *b64, size_t len, OdinTextRun **runs, size_t *nruns,
    const char **why)
{
	static std::string	message;

	*runs = nullptr;
	*nruns = 0;
	try {
		J			 obj = parse_object(unbase64(b64, len));
		std::vector<run_out>	 out;
		metrics			 m;

		layout(obj, &out, &m);
		if (out.empty())
			return (1);
		OdinTextRun	*r = (OdinTextRun *)calloc(out.size(), sizeof *r);

		if (r == nullptr)
			throw bad("Out of memory laying out vector text");
		*runs = r;
		*nruns = out.size();
		for (size_t i = 0; i < out.size(); i++) {
			r[i].ncmds = out[i].path.size();
			r[i].cmds = (OdinTextCmd *)calloc(r[i].ncmds ? r[i].ncmds : 1, sizeof(OdinTextCmd));
			r[i].fill = strdup(out[i].fill.c_str());
			r[i].stroke = strdup(out[i].stroke.c_str());
			r[i].width = out[i].width;
			if (r[i].cmds == nullptr || r[i].fill == nullptr || r[i].stroke == nullptr) {
				odin_vector_text_free(r, out.size());
				*runs = nullptr;
				*nruns = 0;
				throw bad("Out of memory laying out vector text");
			}
			std::copy(out[i].path.begin(), out[i].path.end(), r[i].cmds);
		}
		return (1);
	} catch (const std::exception &e) {
		message = e.what();
		if (why != nullptr)
			*why = message.c_str();
		return (0);
	}
}

void
odin_vector_text_free(OdinTextRun *runs, size_t nruns)
{
	if (runs == nullptr)
		return;
	for (size_t i = 0; i < nruns; i++) {
		free(runs[i].cmds);
		free(runs[i].fill);
		free(runs[i].stroke);
	}
	free(runs);
}
