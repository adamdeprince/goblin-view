/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Input methods. A method composes keystrokes into a preedit and commits
 * Unicode into the focused window, so entry is identical on every host
 * terminal (the point of doing it in the multiplexer).
 *
 *   Romaji          algorithmic romaji -> hiragana (no data table)
 *   US-International dead key (' ` ^ " ~) + letter -> accented latin
 *   Pinyin / Zhuyin a syllable -> hanzi candidates (share/pinyin.dict)
 *   Wubi            wubi86 shape code -> hanzi/words (share/wubi.dict)
 *
 * The pinyin and wubi tables are built by tools/gen-imdata.py from
 * permissively-licensed sources (mozillazg/pinyin-data, MIT, over Unicode
 * Unihan; KyleBing/rime-wubi86-jidian, Apache-2.0) -- see share/README.md.
 * They load at runtime, so the binary stays small and the data is
 * replaceable.
 */

#include "dv.hpp"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <unistd.h>

#define CANDS_PER_PAGE	9	/* selectable with keys 1-9 */

static void
commit_cp(struct dv_buf *out, uint32_t cp)
{
	unsigned char	u[4];
	size_t		n;

	if (cp < 0x80) {
		u[0] = (unsigned char)cp;
		n = 1;
	} else if (cp < 0x800) {
		u[0] = 0xc0 | (cp >> 6);
		u[1] = 0x80 | (cp & 0x3f);
		n = 2;
	} else if (cp < 0x10000) {
		u[0] = 0xe0 | (cp >> 12);
		u[1] = 0x80 | ((cp >> 6) & 0x3f);
		u[2] = 0x80 | (cp & 0x3f);
		n = 3;
	} else {
		u[0] = 0xf0 | (cp >> 18);
		u[1] = 0x80 | ((cp >> 12) & 0x3f);
		u[2] = 0x80 | ((cp >> 6) & 0x3f);
		u[3] = 0x80 | (cp & 0x3f);
		n = 4;
	}
	dv_buf_append(out, u, n);
}

static void
commit_str(struct dv_buf *out, const char *s)
{
	dv_buf_append(out, s, strlen(s));
}

void
dv_im_reset(struct dv_im *im)
{
	im->prelen = 0;
	im->pre[0] = '\0';
	im->ncand = 0;
	im->page = 0;
	im->dead = 0;
}

const char *
dv_im_name(int method)
{
	switch (method) {
	case DV_IM_ROMAJI:
		return ("Romaji");
	case DV_IM_USINTL:
		return ("US-Intl");
	case DV_IM_PINYIN:
		return ("Pinyin");
	case DV_IM_ZHUYIN:
		return ("Zhuyin");
	case DV_IM_WUBI:
		return ("Wubi");
	default:
		return ("Off");
	}
}

/* ---- Runtime dictionary: key -> space-separated candidate blob --------- */

struct imnode {
	char	*key;
	char	*val;
};

struct immap {
	struct imnode	*tab;
	size_t		 cap;
	int		 loaded;	/* 0 untried, 1 ok, -1 failed */
};

static struct immap	py_map, wb_map;

static size_t
im_hash(const char *s)
{
	size_t	h = 1469598103934665603ULL;

	for (; *s != '\0'; s++) {
		h ^= (unsigned char)*s;
		h *= 1099511628211ULL;
	}
	return (h);
}

static void
im_put(struct immap *m, const char *key, const char *val)
{
	size_t	i = im_hash(key) & (m->cap - 1);

	while (m->tab[i].key != nullptr) {
		if (strcmp(m->tab[i].key, key) == 0)
			return;
		i = (i + 1) & (m->cap - 1);
	}
	m->tab[i].key = strdup(key);
	m->tab[i].val = strdup(val);
}

static const char *
im_get(struct immap *m, const char *key)
{
	size_t	i;

	if (m->loaded != 1)
		return (nullptr);
	i = im_hash(key) & (m->cap - 1);
	while (m->tab[i].key != nullptr) {
		if (strcmp(m->tab[i].key, key) == 0)
			return (m->tab[i].val);
		i = (i + 1) & (m->cap - 1);
	}
	return (nullptr);
}

/* Resolve without imposing a path limit below the operating system's own. */
char *
dv_exe_path(void)
{
	char	*path, *resolved;

#ifdef __APPLE__
	/* _NSGetExecutablePath reports its required allocation as uint32_t. */
	uint32_t	sz = 0;

	(void)_NSGetExecutablePath(nullptr, &sz);
	if (sz == 0)
		return (nullptr);
	path = static_cast<char *>(xmalloc(sz));
	while (_NSGetExecutablePath(path, &sz) != 0)
		path = static_cast<char *>(xrealloc(path, sz));
#else
	size_t	cap = 128;

	path = static_cast<char *>(xmalloc(cap));
	for (;;) {
		ssize_t	n = readlink("/proc/self/exe", path, cap - 1);

		if (n <= 0) {
			free(path);
			return (nullptr);
		}
		if (static_cast<size_t>(n) < cap - 1) {
			path[n] = '\0';
			break;
		}
		/* readlink returns a signed byte count. */
		if (cap > static_cast<size_t>(SSIZE_MAX) / 2)
			dv_fatal("executable path allocation overflow");
		cap *= 2;
		path = static_cast<char *>(xrealloc(path, cap));
	}
#endif
	resolved = realpath(path, nullptr);
	if (resolved == nullptr && errno == ENOMEM)
		dv_fatal("realpath: %s", strerror(errno));
	free(path);
	return (resolved);
}

/*
 * Where the dictionaries are: $ODIN_SEAT_IMDATA if set; else the
 * share/odin-seat beside this executable's bin directory, so a relocated
 * installation (a Homebrew bottle poured into another prefix, a tree
 * copied elsewhere) still finds them; else the directory compiled in.
 */
const char *
dv_im_datadir(void)
{
	static char	*resolved;
	const char	*e = getenv("ODIN_SEAT_IMDATA");
	char		*real;
#ifdef ODIN_SEAT_DATADIR
	const char	*compiled = ODIN_SEAT_DATADIR;
#else
	const char	*compiled = "share";
#endif

	if (e != nullptr && e[0] != '\0')
		return (e);
	if (resolved != nullptr)
		return (resolved);
	if ((real = dv_exe_path()) != nullptr) {
		char	*slash = strrchr(real, '/');

		if (slash != nullptr) {
			char	*probe;

			*slash = '\0';		/* .../bin */
			resolved = xasprintf("%s/../share/odin-seat", real);
			probe = xasprintf("%s/pinyin.dict", resolved);
			int found = access(probe, R_OK) == 0;
			free(probe);
			if (found) {
				free(real);
				return (resolved);
			}
			free(resolved);
			resolved = nullptr;
			if (compiled[0] != '/') {	/* a relative fallback: from bin/ */
				resolved = xasprintf("%s/%s", real, compiled);
				free(real);
				return (resolved);
			}
		}
		free(real);
	}
	resolved = xstrdup(compiled);
	return (resolved);
}

static const char *
im_datadir(void)
{
	return (dv_im_datadir());
}

static void
im_load(struct immap *m, const char *name, size_t cap)
{
	char	*path;
	FILE	*f;
	char	*line = nullptr;
	size_t	 linecap = 0;
	ssize_t	 n;

	if (m->loaded != 0)
		return;
	m->loaded = -1;
	path = xasprintf("%s/%s", im_datadir(), name);
	f = fopen(path, "r");
	free(path);
	if (f == nullptr)
		return;
	m->cap = cap;
	m->tab = static_cast<struct imnode *>(calloc(cap, sizeof *m->tab));
	if (m->tab == nullptr) {
		fclose(f);
		return;
	}
	while ((n = getline(&line, &linecap, f)) > 0) {
		char	*tab, *nl;

		if (line[0] == '#')
			continue;
		nl = strchr(line, '\n');
		if (nl != nullptr)
			*nl = '\0';
		tab = strchr(line, '\t');
		if (tab == nullptr)
			continue;
		*tab = '\0';
		im_put(m, line, tab + 1);
	}
	free(line);
	fclose(f);
	m->loaded = 1;
}

/* Fill cand[] with page `im->page` of the space-separated blob. */
static void
fill_page(struct dv_im *im, const char *blob)
{
	int		skip = im->page * CANDS_PER_PAGE;
	const char	*p = blob;

	im->ncand = 0;
	if (blob == nullptr)
		return;
	while (*p != '\0' && im->ncand < CANDS_PER_PAGE) {
		const char	*sp = strchr(p, ' ');
		size_t		 len = sp ? (size_t)(sp - p) : strlen(p);

		if (skip > 0)
			skip--;
		else {
			if (len >= sizeof im->cand[0])
				len = sizeof im->cand[0] - 1;
			memcpy(im->cand[im->ncand], p, len);
			im->cand[im->ncand][len] = '\0';
			im->ncand++;
		}
		p = sp ? sp + 1 : p + len;
	}
}

static int
has_more_pages(struct dv_im *im, const char *blob)
{
	int		limit = (im->page + 1) * CANDS_PER_PAGE, n = 0;
	const char	*p = blob;

	if (blob == nullptr)
		return (0);
	while (*p != '\0') {
		const char	*sp = strchr(p, ' ');

		if (++n > limit)
			return (1);
		if (sp == nullptr)
			break;
		p = sp + 1;
	}
	return (0);
}

static void
lookup(struct dv_im *im, struct immap *m)
{
	fill_page(im, im_get(m, im->pre));
}

/* ---- Romaji -> Hiragana (algorithmic) ---------------------------------- */

struct romaji {
	const char	*r;
	const char	*kana;
};

static const struct romaji romaji_tab[] = {
	{ "kya", "きゃ" }, { "kyu", "きゅ" }, { "kyo", "きょ" },
	{ "sha", "しゃ" }, { "shu", "しゅ" }, { "sho", "しょ" }, { "shi", "し" },
	{ "cha", "ちゃ" }, { "chu", "ちゅ" }, { "cho", "ちょ" }, { "chi", "ち" },
	{ "tsu", "つ" },
	{ "nya", "にゃ" }, { "nyu", "にゅ" }, { "nyo", "にょ" },
	{ "hya", "ひゃ" }, { "hyu", "ひゅ" }, { "hyo", "ひょ" },
	{ "mya", "みゃ" }, { "myu", "みゅ" }, { "myo", "みょ" },
	{ "rya", "りゃ" }, { "ryu", "りゅ" }, { "ryo", "りょ" },
	{ "gya", "ぎゃ" }, { "gyu", "ぎゅ" }, { "gyo", "ぎょ" },
	{ "ja",  "じゃ" }, { "ju",  "じゅ" }, { "jo",  "じょ" }, { "ji", "じ" },
	{ "bya", "びゃ" }, { "byu", "びゅ" }, { "byo", "びょ" },
	{ "pya", "ぴゃ" }, { "pyu", "ぴゅ" }, { "pyo", "ぴょ" },
	{ "ka", "か" }, { "ki", "き" }, { "ku", "く" }, { "ke", "け" }, { "ko", "こ" },
	{ "sa", "さ" }, { "su", "す" }, { "se", "せ" }, { "so", "そ" },
	{ "ta", "た" }, { "te", "て" }, { "to", "と" },
	{ "na", "な" }, { "ni", "に" }, { "nu", "ぬ" }, { "ne", "ね" }, { "no", "の" },
	{ "ha", "は" }, { "hi", "ひ" }, { "fu", "ふ" }, { "he", "へ" }, { "ho", "ほ" },
	{ "ma", "ま" }, { "mi", "み" }, { "mu", "む" }, { "me", "め" }, { "mo", "も" },
	{ "ya", "や" }, { "yu", "ゆ" }, { "yo", "よ" },
	{ "ra", "ら" }, { "ri", "り" }, { "ru", "る" }, { "re", "れ" }, { "ro", "ろ" },
	{ "wa", "わ" }, { "wo", "を" },
	{ "ga", "が" }, { "gi", "ぎ" }, { "gu", "ぐ" }, { "ge", "げ" }, { "go", "ご" },
	{ "za", "ざ" }, { "zu", "ず" }, { "ze", "ぜ" }, { "zo", "ぞ" },
	{ "da", "だ" }, { "de", "で" }, { "do", "ど" },
	{ "ba", "ば" }, { "bi", "び" }, { "bu", "ぶ" }, { "be", "べ" }, { "bo", "ぼ" },
	{ "pa", "ぱ" }, { "pi", "ぴ" }, { "pu", "ぷ" }, { "pe", "ぺ" }, { "po", "ぽ" },
	{ "a", "あ" }, { "i", "い" }, { "u", "う" }, { "e", "え" }, { "o", "お" },
	{ "n", "ん" },
	{ nullptr, nullptr },
};

static const char *
romaji_lookup(const char *r, int len)
{
	int	i;

	for (i = 0; romaji_tab[i].r != nullptr; i++)
		if ((int)strlen(romaji_tab[i].r) == len &&
		    strncmp(romaji_tab[i].r, r, (size_t)len) == 0)
			return (romaji_tab[i].kana);
	return (nullptr);
}

static void
romaji_convert(struct dv_im *im, struct dv_buf *out, int flush)
{
	while (im->prelen > 0) {
		const char	*k;
		int		n;

		if (!flush && im->prelen == 1 && im->pre[0] == 'n')
			break;
		if (im->prelen >= 2 && im->pre[0] == im->pre[1] &&
		    im->pre[0] != 'a' && im->pre[0] != 'e' && im->pre[0] != 'i' &&
		    im->pre[0] != 'o' && im->pre[0] != 'u' && im->pre[0] != 'n') {
			commit_str(out, "っ");
			memmove(im->pre, im->pre + 1, (size_t)(--im->prelen));
			continue;
		}
		if (im->prelen >= 2 && im->pre[0] == 'n' &&
		    strchr("aiueoy", im->pre[1]) == nullptr) {
			commit_str(out, "ん");
			memmove(im->pre, im->pre + 1, (size_t)(--im->prelen));
			continue;
		}
		n = im->prelen < 3 ? im->prelen : 3;
		for (; n >= 1; n--) {
			k = romaji_lookup(im->pre, n);
			if (k != nullptr) {
				commit_str(out, k);
				memmove(im->pre, im->pre + n,
				    (size_t)(im->prelen - n));
				im->prelen -= n;
				break;
			}
		}
		if (n == 0)
			break;
	}
	im->pre[im->prelen] = '\0';
}

/* ---- US-International dead keys ---------------------------------------- */

struct deadmap {
	char		dead;
	char		base;
	uint32_t	cp;
};

static const struct deadmap deadmap[] = {
	{ '\'', 'a', 0xE1 }, { '\'', 'e', 0xE9 }, { '\'', 'i', 0xED },
	{ '\'', 'o', 0xF3 }, { '\'', 'u', 0xFA }, { '\'', 'y', 0xFD },
	{ '\'', 'c', 0xE7 }, { '\'', 'A', 0xC1 }, { '\'', 'E', 0xC9 },
	{ '\'', 'I', 0xCD }, { '\'', 'O', 0xD3 }, { '\'', 'U', 0xDA },
	{ '`', 'a', 0xE0 }, { '`', 'e', 0xE8 }, { '`', 'i', 0xEC },
	{ '`', 'o', 0xF2 }, { '`', 'u', 0xF9 },
	{ '^', 'a', 0xE2 }, { '^', 'e', 0xEA }, { '^', 'i', 0xEE },
	{ '^', 'o', 0xF4 }, { '^', 'u', 0xFB },
	{ '"', 'a', 0xE4 }, { '"', 'e', 0xEB }, { '"', 'i', 0xEF },
	{ '"', 'o', 0xF6 }, { '"', 'u', 0xFC }, { '"', 'y', 0xFF },
	{ '~', 'n', 0xF1 }, { '~', 'o', 0xF5 }, { '~', 'a', 0xE3 },
	{ '~', 'N', 0xD1 },
	{ 0, 0, 0 },
};

static int
is_dead(int ch)
{
	return (ch == '\'' || ch == '`' || ch == '^' || ch == '"' || ch == '~');
}

/* Bopomofo key -> pinyin letter (standard keyboard layout, subset). */
static char
zhuyin_key(int ch)
{
	switch (ch) {
	case '1': return ('b');
	case 'q': return ('p');
	case 'a': return ('m');
	case 'z': return ('f');
	case '2': return ('d');
	case 'w': return ('t');
	case 's': return ('n');
	case 'x': return ('l');
	case 'e': return ('g');
	case 'd': return ('k');
	case 'c': return ('h');
	case 'r': return ('j');
	case 'f': return ('q');
	case 'v': return ('x');
	case 'u': return ('i');
	case 'j': return ('u');
	case '8': return ('a');
	case 'i': return ('o');
	case 'o': return ('e');
	default:  return ((char)ch);
	}
}

static int
commit_candidate(struct dv_im *im, int pick, struct dv_buf *out)
{
	if (pick < 0 || pick >= im->ncand)
		return (0);
	commit_str(out, im->cand[pick]);
	dv_im_reset(im);
	return (1);
}

static void
commit_raw(struct dv_im *im, struct dv_buf *out)
{
	int	i;

	for (i = 0; i < im->prelen; i++)
		commit_cp(out, (unsigned char)im->pre[i]);
	dv_im_reset(im);
}

/* Table-backed methods: pinyin, zhuyin, wubi. */
static int
table_key(struct dv_im *im, struct immap *m, int maxlen, int ch,
    struct dv_buf *out)
{
	int	c = (im->method == DV_IM_ZHUYIN) ? zhuyin_key(ch) : ch;

	if (c >= 'a' && c <= 'z') {
		if (im->prelen < maxlen && im->prelen < (int)sizeof im->pre - 1)
			im->pre[im->prelen++] = (char)c;
		im->pre[im->prelen] = '\0';
		im->page = 0;
		lookup(im, m);
		return (1);
	}
	if (im->prelen == 0)
		return (0);
	if (ch == ' ') {
		if (!commit_candidate(im, 0, out))
			commit_raw(im, out);
		return (1);
	}
	if (ch >= '1' && ch <= '9') {
		commit_candidate(im, ch - '1', out);
		return (1);
	}
	if (ch == '.' || ch == '=' || ch == ']') {	/* next page */
		if (has_more_pages(im, im_get(m, im->pre))) {
			im->page++;
			lookup(im, m);
		}
		return (1);
	}
	if (ch == ',' || ch == '-' || ch == '[') {	/* previous page */
		if (im->page > 0) {
			im->page--;
			lookup(im, m);
		}
		return (1);
	}
	if (ch == '\r' || ch == '\n') {
		/* Enter confirms the top candidate (what most people expect),
		 * falling back to the raw letters only when nothing matched. */
		if (im->ncand > 0)
			commit_candidate(im, 0, out);
		else
			commit_raw(im, out);
		return (1);
	}
	return (0);
}

int
dv_im_key(struct dv_im *im, int ch, struct dv_buf *commit)
{
	if (im->method == DV_IM_OFF)
		return (0);
	if (im->method == DV_IM_PINYIN || im->method == DV_IM_ZHUYIN)
		im_load(&py_map, "pinyin.dict", 1024);
	else if (im->method == DV_IM_WUBI)
		im_load(&wb_map, "wubi.dict", 262144);

	if (ch == 0x7f || ch == 0x08) {		/* backspace edits the preedit */
		if (im->prelen > 0) {
			im->pre[--im->prelen] = '\0';
			im->page = 0;
			if (im->method == DV_IM_PINYIN || im->method == DV_IM_ZHUYIN)
				lookup(im, &py_map);
			else if (im->method == DV_IM_WUBI)
				lookup(im, &wb_map);
			return (1);
		}
		return (0);
	}
	if (ch == 0x1b) {			/* Esc drops the preedit */
		if (im->prelen > 0 || im->dead) {
			dv_im_reset(im);
			return (1);
		}
		return (0);
	}

	switch (im->method) {
	case DV_IM_ROMAJI:
		if (ch >= 'a' && ch <= 'z') {
			if (im->prelen < (int)sizeof im->pre - 1)
				im->pre[im->prelen++] = (char)ch;
			im->pre[im->prelen] = '\0';
			romaji_convert(im, commit, 0);
			return (1);
		}
		if (im->prelen > 0) {
			romaji_convert(im, commit, 1);
			commit_raw(im, commit);
		}
		return (0);		/* also let the key through */

	case DV_IM_USINTL:
		if (im->dead) {
			int	d = im->dead, i;

			im->dead = 0;
			for (i = 0; deadmap[i].dead != 0; i++)
				if (deadmap[i].dead == d && deadmap[i].base == ch) {
					commit_cp(commit, deadmap[i].cp);
					return (1);
				}
			commit_cp(commit, (unsigned char)d);
			if (is_dead(ch)) {
				im->dead = ch;
				return (1);
			}
			return (0);
		}
		if (is_dead(ch)) {
			im->dead = ch;
			return (1);
		}
		return (0);

	case DV_IM_PINYIN:
	case DV_IM_ZHUYIN:
		return (table_key(im, &py_map, 6, ch, commit));
	case DV_IM_WUBI:
		return (table_key(im, &wb_map, 4, ch, commit));
	default:
		return (0);
	}
}
