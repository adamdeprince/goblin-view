/*
 * The kitty keyboard protocol, per window.
 *
 * The terminal we are attached to reports keys as structured events (the
 * client runs it with every enhancement on); a program in a window gets
 * each key encoded for the flags it pushed with CSI > flags u, or the
 * plain bytes a legacy terminal would send when it pushed none. So a
 * program that wants Ctrl+I told apart from Tab, or key releases, gets
 * them in its own window while the shell next door keeps typing text.
 */

#include <stdio.h>
#include <string.h>

#include "dv.hpp"

#define KF_DISAMBIGUATE	1
#define KF_EVENTS	2
#define KF_ALTERNATES	4
#define KF_ALL_ESCAPES	8
#define KF_TEXT		16

#define KM_SHIFT	1u
#define KM_NONTEXT	(2u | 4u | 8u | 16u | 32u)	/* alt ctrl super hyper meta */
#define KM_LOCKS	(64u | 128u)

#define K_FUNCTIONAL	57344u
#define K_MOD_FIRST	57441u
#define K_MOD_LAST	57454u

static size_t
emit(unsigned char *out, size_t cap, const char *s)
{
	size_t	n = strlen(s);

	if (n > cap)
		return (0);
	memcpy(out, s, n);
	return (n);
}

/* CSI code[:shifted[:base]] [; mods[:type]] [; text] u */
static size_t
csi_u(const struct dv_key *k, int flags, unsigned code, unsigned mods, int type,
    unsigned char *out, size_t cap)
{
	char	buf[160];
	int	n, with_type = (flags & KF_EVENTS) && type != 1;
	int	text = (flags & KF_TEXT) && k->ntext > 0 && !(mods & KM_NONTEXT);

	n = snprintf(buf, sizeof buf, "\033[%u", code);
	if ((flags & KF_ALTERNATES) && (k->shifted || k->base)) {
		if (k->shifted)
			n += snprintf(buf + n, sizeof buf - (size_t)n, ":%u", k->shifted);
		if (k->base)
			n += snprintf(buf + n, sizeof buf - (size_t)n, "%s:%u",
			    k->shifted ? "" : ":", k->base);
	}
	if (mods || with_type || text) {
		n += snprintf(buf + n, sizeof buf - (size_t)n, ";%u", mods + 1);
		if (with_type)
			n += snprintf(buf + n, sizeof buf - (size_t)n, ":%d", type);
	}
	if (text) {
		int	i;

		for (i = 0; i < k->ntext && i < 4; i++)
			n += snprintf(buf + n, sizeof buf - (size_t)n, "%c%u",
			    i ? ':' : ';', k->text[i]);
	}
	snprintf(buf + n, sizeof buf - (size_t)n, "u");
	return (emit(out, cap, buf));
}

/* CSI [1 ; mods[:type]] X for arrows, Home/End, F1 F2 F4, keypad Begin */
static size_t
csi_letter(int flags, char letter, unsigned mods, int type, unsigned char *out,
    size_t cap)
{
	char	buf[64];
	int	with_type = (flags & KF_EVENTS) && type != 1;

	if (mods || with_type) {
		if (with_type)
			snprintf(buf, sizeof buf, "\033[1;%u:%d%c", mods + 1, type, letter);
		else
			snprintf(buf, sizeof buf, "\033[1;%u%c", mods + 1, letter);
	} else
		snprintf(buf, sizeof buf, "\033[%c", letter);
	return (emit(out, cap, buf));
}

/* CSI number [; mods[:type]] ~ for Insert, Delete, Page keys, F3, F5-F12 */
static size_t
csi_tilde(int flags, unsigned num, unsigned mods, int type, unsigned char *out,
    size_t cap)
{
	char	buf[64];
	int	with_type = (flags & KF_EVENTS) && type != 1;

	if (mods || with_type) {
		if (with_type)
			snprintf(buf, sizeof buf, "\033[%u;%u:%d~", num, mods + 1, type);
		else
			snprintf(buf, sizeof buf, "\033[%u;%u~", num, mods + 1);
	} else
		snprintf(buf, sizeof buf, "\033[%u~", num);
	return (emit(out, cap, buf));
}

static size_t
legacy(const struct dv_key *k, unsigned char *out, size_t cap)
{
	if (k->nlegacy > cap)
		return (0);
	memcpy(out, k->legacy, k->nlegacy);
	return (k->nlegacy);
}

size_t
dv_kbd_encode(const struct dv_key *k, int flags, unsigned char *out, size_t cap)
{
	unsigned	mods = k->mods, code = k->code;
	int		type = k->type ? k->type : 1;
	int		disamb = (flags & (KF_DISAMBIGUATE | KF_ALL_ESCAPES)) != 0;
	int		all = (flags & KF_ALL_ESCAPES) != 0;

	if (flags == 0)
		return (legacy(k, out, cap));
	if (type == 3 && !(flags & KF_EVENTS))
		return (0);			/* releases only when asked for */
	if (type == 2 && !(flags & KF_EVENTS))
		type = 1;			/* a repeat is a press */

	if (code >= K_MOD_FIRST && code <= K_MOD_LAST) {	/* a modifier key */
		if (!all)
			return (0);
		return (csi_u(k, flags, code, mods, type, out, cap));
	}
	if (k->final != 'u' && k->final != '~' && k->final != 0)
		return (csi_letter(flags, (char)k->final, mods, type, out, cap));
	if (k->final == '~')
		return (csi_tilde(flags, code, mods, type, out, cap));

	if (code >= K_FUNCTIONAL || all)	/* no legacy form, or none wanted */
		return (csi_u(k, flags, code, mods, type, out, cap));

	mods &= ~KM_LOCKS;			/* not reported for text keys */
	if (code == 13 || code == 9 || code == 127) {	/* Enter, Tab, Backspace */
		if (mods == 0 || !disamb) {
			if (type == 3)
				return (0);	/* no releases without all-escapes */
			return (legacy(k, out, cap));
		}
		return (csi_u(k, flags, code, mods, type, out, cap));
	}
	if (code == 27) {				/* Escape */
		if (!disamb) {
			if (type == 3)
				return (0);
			return (legacy(k, out, cap));
		}
		return (csi_u(k, flags, code, mods, type, out, cap));
	}
	/* A text key: escape it when a modifier makes it not text, or on
	 * release; otherwise it is the text a legacy terminal would send. */
	if ((mods & KM_NONTEXT) && disamb)
		return (csi_u(k, flags, code, mods, type, out, cap));
	if (type == 3)
		return (csi_u(k, flags, code, mods, type, out, cap));
	if (k->nlegacy == 0 && disamb)		/* e.g. super+key: no text */
		return (csi_u(k, flags, code, mods, type, out, cap));
	return (legacy(k, out, cap));
}

/* ---- on the wire between client and server ---- */

static void
put32(unsigned char *p, unsigned v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static unsigned
get32(const unsigned char *p)
{
	return ((unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]);
}

#define KEY_HEAD	34

size_t
dv_key_pack(const struct dv_key *k, unsigned char *out, size_t cap)
{
	int	i;

	if (cap < KEY_HEAD + k->nlegacy)
		return (0);
	put32(out, k->code);
	put32(out + 4, k->shifted);
	put32(out + 8, k->base);
	out[12] = (unsigned char)(k->mods >> 8);
	out[13] = (unsigned char)k->mods;
	out[14] = (unsigned char)k->type;
	out[15] = (unsigned char)k->final;
	out[16] = (unsigned char)(k->ntext < 0 ? 0 : k->ntext > 4 ? 4 : k->ntext);
	for (i = 0; i < 4; i++)
		put32(out + 17 + 4 * i, i < k->ntext ? k->text[i] : 0);
	out[33] = (unsigned char)k->nlegacy;
	memcpy(out + KEY_HEAD, k->legacy, k->nlegacy);
	return (KEY_HEAD + k->nlegacy);
}

int
dv_key_unpack(const unsigned char *in, size_t n, struct dv_key *k)
{
	int	i;

	if (n < KEY_HEAD)
		return (0);
	memset(k, 0, sizeof *k);
	k->code = get32(in);
	k->shifted = get32(in + 4);
	k->base = get32(in + 8);
	k->mods = (unsigned)in[12] << 8 | in[13];
	k->type = in[14];
	k->final = in[15];
	k->ntext = in[16] > 4 ? 4 : in[16];
	for (i = 0; i < k->ntext; i++)
		k->text[i] = get32(in + 17 + 4 * i);
	k->nlegacy = in[33];
	if (k->nlegacy > sizeof k->legacy || KEY_HEAD + k->nlegacy > n)
		return (0);
	memcpy(k->legacy, in + KEY_HEAD, k->nlegacy);
	return (1);
}
