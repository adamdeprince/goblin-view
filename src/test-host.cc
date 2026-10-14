/*
 * Ghostty host-input parser: Alt-tap menu key, Ctrl-\ fallback, CSI u keys,
 * XTWINOPS / mode 2048 sizes, kitty graphics OK reply.
 */

#include "dv.hpp"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failed;

static void
fail(const char *n)
{
	fprintf(stderr, "FAIL %s\n", n);
	failed = 1;
}

static void
feed(struct dv_host *h, const char *s)
{
	dv_host_feed(h, s, strlen(s));
}

static int
expect_type(struct dv_host *h, int type, const char *n)
{
	struct dv_host_ev	ev;

	if (!dv_host_next(h, &ev)) {
		fail(n);
		return (0);
	}
	if (ev.type != type) {
		fprintf(stderr, "FAIL %s: type %d want %d\n", n, ev.type, type);
		failed = 1;
		return (0);
	}
	return (1);
}

static void
test_alt_tap(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;

	dv_host_init(&h);
	feed(&h, "\033[57443;3:1u");	/* left alt press */
	if (dv_host_next(&h, &ev) && (ev.n != 0 || !ev.has_key))
		fail("alt press carries no bytes, only the key");
	feed(&h, "\033[57443;1:3u");	/* left alt release */
	if (!expect_type(&h, DV_HEV_MENU, "alt tap"))
		return;
}

static void
test_alt_chord(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;

	dv_host_init(&h);
	feed(&h, "\033[57443;3:1u");		/* alt press */
	while (dv_host_next(&h, &ev))
		if (ev.n != 0)
			fail("alt press has no bytes");
	feed(&h, "\033[97;3:1u");		/* a with alt */
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_PTY || ev.n != 2 ||
	    ev.data[0] != 0x1b || ev.data[1] != 'a')
		fail("alt+a should be a PTY key");
	if (!ev.has_key || ev.key.code != 'a' || ev.key.mods != 2)
		fail("alt+a carries the key with its modifiers");
	feed(&h, "\033[57443;1:3u");		/* alt release */
	if (dv_host_next(&h, &ev) && ev.type == DV_HEV_MENU)
		fail("alt+a must not also open the menu");
}

static void
test_ctrl_backslash(void)
{
	struct dv_host	h;

	dv_host_init(&h);
	feed(&h, "\x1c");
	expect_type(&h, DV_HEV_MENU, "C-\\ byte");

	dv_host_init(&h);
	feed(&h, "\033[92;5u");			/* ctrl+\ as CSI u */
	expect_type(&h, DV_HEV_MENU, "C-\\ CSI u");
}

static void
test_plain_a(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;

	dv_host_init(&h);
	feed(&h, "\033[97u");
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_PTY ||
	    ev.n != 1 || ev.data[0] != 'a')
		fail("plain a");
}

static void
expect_bytes(const char *in, const char *want, const char *name)
{
	struct dv_host		h;
	struct dv_host_ev	ev;
	size_t			wl = strlen(want);

	dv_host_init(&h);
	feed(&h, in);
	/* A byte-free key event (a release, a modifier) is "no bytes" too. */
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_PTY || ev.n == 0) {
		if (wl == 0)
			return;
		fail(name);
		return;
	}
	if (wl == 0) {
		fprintf(stderr, "FAIL %s: expected no key\n", name);
		failed = 1;
		return;
	}
	if (ev.n != wl || memcmp(ev.data, want, wl) != 0) {
		fprintf(stderr, "FAIL %s: got %zu bytes", name, ev.n);
		for (size_t i = 0; i < ev.n; i++)
			fprintf(stderr, " %02x", ev.data[i]);
		fprintf(stderr, "\n");
		failed = 1;
	}
}

/* Kitty CSI u re-encoded to the legacy bytes the pty expects. */
static void
test_key_encoding(void)
{
	expect_bytes("\033[97;2u", "A", "shift+a without text");
	expect_bytes("\033[97;2;65u", "A", "shift+a with text");
	expect_bytes("\033[97;;97u", "a", "a with text field");
	expect_bytes("\033[97;3u", "\033a", "alt+a is ESC a");
	expect_bytes("\033[99;5u", "\003", "ctrl+c");
	expect_bytes("\033[91;5u", "\033", "ctrl+[");
	expect_bytes("\033[13u", "\r", "enter");
	expect_bytes("\033[9;2u", "\033[Z", "shift+tab");
	expect_bytes("\033[127u", "\177", "backspace");
	expect_bytes("\033[27u", "\033", "esc");
	expect_bytes("\033[3~", "\033[3~", "delete is not ctrl+c");
	expect_bytes("\033[15~", "\033[15~", "F5");
	expect_bytes("\033[15;2~", "\033[15;2~", "shift+F5");
	expect_bytes("\033[A", "\033[A", "up");
	expect_bytes("\033[1;2A", "\033[1;2A", "shift+up");
	expect_bytes("\033[1;5C", "\033[1;5C", "ctrl+right");
	expect_bytes("\033[P", "\033OP", "F1");
	expect_bytes("\033[57414u", "\r", "keypad enter");
	expect_bytes("\033[57358u", "", "caps lock has no bytes");
	expect_bytes("\033[97;1:3u", "", "a release is silent");
	expect_bytes("\033[97;1:2u", "a", "a repeat is a press");
	expect_bytes("\033[I", "", "focus in is not a key");
	expect_bytes("hello", "hello", "a plain run stays one event");
}

static void
test_ctrl_space_length(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;

	dv_host_init(&h);
	feed(&h, "\033[32;5u");
	if (!dv_host_next(&h, &ev) || ev.n != 1 || ev.data[0] != 0)
		fail("ctrl+space is one NUL byte");
}

/* A host OSC reply is one event carrying the whole sequence, not keys. */
static void
test_osc_reply(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;
	const char		*rep = "\033]52;c;aGVsbG8=\033\\";

	dv_host_init(&h);
	feed(&h, rep);
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_OSC)
		fail("OSC reply is an OSC event");
	else if (h.osc.len != strlen(rep) || memcmp(h.osc.data, rep, h.osc.len))
		fail("OSC reply carried intact");
	h.osc.len = 0;
	if (dv_host_next(&h, &ev))
		fail("nothing left after the OSC reply");

	/* Split across reads, BEL-terminated, followed by a key. */
	dv_host_init(&h);
	feed(&h, "\033]52;c;aG");
	if (dv_host_next(&h, &ev))
		fail("a partial OSC waits");
	if (dv_host_pending(&h))
		fail("a partial OSC must not be chopped by the ESC timer");
	feed(&h, "VsbG8=\007a");
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_OSC)
		fail("split OSC completes");
	else if (memmem(h.osc.data, h.osc.len, "aGVsbG8=", 8) == NULL)
		fail("split OSC joined");
	h.osc.len = 0;
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_PTY || ev.data[0] != 'a')
		fail("the key after the OSC is a key");

	/* ESC ] without a digit is Alt+] from a legacy host, not an OSC. */
	dv_host_init(&h);
	feed(&h, "\033]x");
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_PTY || ev.n != 2)
		fail("ESC ] non-digit stays a key");
	dv_buf_free(&h.osc);
}

static void
test_cell_size(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;

	dv_host_init(&h);
	feed(&h, "\033[6;17;10t");		/* CSI 16 t reply */
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_SIZE)
		fail("CSI 16 t");
	if (ev.size.xpixel != 10 || ev.size.ypixel != 17)
		fail("cell pixels 10x17");
}

static void
test_mode_2048(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;

	dv_host_init(&h);
	feed(&h, "\033[48;43;132;860;1320t");
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_SIZE)
		fail("mode 2048");
	if (ev.size.cols != 132 || ev.size.rows != 43)
		fail("2048 cells");
	if (ev.size.xpixel != 10 || ev.size.ypixel != 20)
		fail("2048 pixels");
}

static void
test_gfx_bel(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;

	dv_host_init(&h);
	feed(&h, "\033_Gi=1;OK\x07");
	while (dv_host_next(&h, &ev))
		continue;
	if (!h.gfx)
		fail("kitty graphics OK (BEL)");
	feed(&h, "\033[97u");
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_PTY ||
	    ev.n != 1 || ev.data[0] != 'a')
		fail("key after BEL APC");
}

/* XTVERSION's reply names the terminal; it is not a key, and a bare
 * ESC P from a legacy host still is. */
static void
test_xtversion(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;

	dv_host_init(&h);
	feed(&h, "\033P>|kitty(0.35.2)\033\\");
	if (dv_host_next(&h, &ev))
		fail("an XTVERSION reply produces no event");
	if (strcmp(h.name, "kitty(0.35.2)") != 0)
		fail("the terminal's name is captured");
	if (!dv_term_is(h.name, "kitty") || dv_term_is(h.name, "goblin"))
		fail("dv_term_is matches the name");
	feed(&h, "\033P>|");
	if (dv_host_next(&h, &ev))
		fail("a partial reply waits");
	feed(&h, "ghostty 1.1.3\033\\\033[97u");
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_PTY || ev.data[0] != 'a')
		fail("the key after the reply arrives");
	if (strcmp(h.name, "ghostty 1.1.3") != 0)
		fail("a later reply replaces the name");
	feed(&h, "\033P1+r544e=787465726d\033\\");
	if (dv_host_next(&h, &ev))
		fail("another DCS reply is consumed silently");
	if (!dv_term_is("Goblin-Mosh 0.4 (kitty compatible)", "goblin") ||
	    dv_term_is("", "kitty"))
		fail("dv_term_is is case-insensitive and safe on empty");
}

/* Every key travels with its structure: code, alternates, modifiers,
 * event type and text, so the server can encode it per window. */
static void
test_structured_keys(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;

	dv_host_init(&h);
	feed(&h, "\033[97;5u");			/* ctrl+a */
	if (!dv_host_next(&h, &ev) || !ev.has_key || ev.key.code != 'a' ||
	    ev.key.mods != 4 || ev.key.type != 1 || ev.key.final != 'u' ||
	    ev.n != 1 || ev.data[0] != 1 || ev.key.nlegacy != 1)
		fail("ctrl+a: key and legacy byte");
	feed(&h, "\033[97;1:3u");		/* release */
	if (!dv_host_next(&h, &ev) || !ev.has_key || ev.n != 0 || ev.key.type != 3)
		fail("a release is a byte-free key event");
	feed(&h, "\033[97:65:113;6:2u");	/* ctrl+shift+a repeat, alternates */
	if (!dv_host_next(&h, &ev) || !ev.has_key || ev.key.shifted != 'A' ||
	    ev.key.base != 'q' || ev.key.mods != 5 || ev.key.type != 2)
		fail("alternates and repeat are carried");
	feed(&h, "\033[97;2;65u");		/* shift+a with text */
	if (!dv_host_next(&h, &ev) || !ev.has_key || ev.key.ntext != 1 ||
	    ev.key.text[0] != 'A' || ev.n != 1 || ev.data[0] != 'A')
		fail("associated text is carried and is the legacy text");
	feed(&h, "\033[1;2A");			/* shift+up */
	if (!dv_host_next(&h, &ev) || !ev.has_key || ev.key.final != 'A' ||
	    ev.key.mods != 1 || ev.n != 6)
		fail("a legacy-form key keeps its final byte");
	feed(&h, "\033[57441;2u");		/* left shift press */
	if (!dv_host_next(&h, &ev) || !ev.has_key || ev.n != 0 ||
	    ev.key.code != 57441)
		fail("a modifier press is a byte-free key event");
	feed(&h, "x");				/* a plain byte from a legacy host */
	if (!dv_host_next(&h, &ev) || ev.has_key || ev.n != 1)
		fail("plain bytes carry no key");
}

/* Leaving: the screen is cleared before the alternate screen is left,
 * so a terminal without one is not left showing the desktop, and what
 * we changed on the host is put back. */
static void
test_disable_sequence(void)
{
	int	pfd[2];
	char	buf[1024];
	ssize_t	n;
	char	*clear, *leave;

	if (pipe(pfd) != 0) {
		fail("pipe");
		return;
	}
	dv_host_disable(pfd[1]);
	close(pfd[1]);
	n = read(pfd[0], buf, sizeof buf - 1);
	close(pfd[0]);
	buf[n > 0 ? n : 0] = '\0';
	clear = strstr(buf, "\033[2J\033[H");
	leave = strstr(buf, "\033[?1049l");
	if (clear == NULL || leave == NULL || clear > leave)
		fail("the screen is cleared before the alternate screen is left");
	if (leave == NULL || strcmp(leave, "\033[?1049l") != 0)
		fail("leaving the alternate screen is the last thing written");
	if (strstr(buf, "\033]22;\033\\") == NULL || strstr(buf, "\033]112\033\\") == NULL)
		fail("the pointer shape and cursor colour are reset");
	if (strstr(buf, "\033[<u") == NULL || strstr(buf, "\033[?1003l") == NULL ||
	    strstr(buf, "\033[?2004l") == NULL || strstr(buf, "\033[?25h") == NULL)
		fail("keyboard flags, mouse tracking and paste mode are undone; the cursor shows");
}

static void
test_gfx_ok(void)
{
	struct dv_host	h;

	dv_host_init(&h);
	feed(&h, "\033_Gi=1;OK\033\\");
	{
		struct dv_host_ev ev{};

		while (dv_host_next(&h, &ev))
			continue;
	}
	if (!h.gfx)
		fail("kitty graphics OK");
}

/*
 * A lone ESC has to expire on a timer or Esc never cancels a latched
 * menu; a partial graphics reply must keep accumulating.
 */
static void
test_lone_esc_expires(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;

	dv_host_init(&h);
	feed(&h, "\033");
	if (dv_host_next(&h, &ev))
		fail("a lone ESC waits for the next byte");
	if (!dv_host_pending(&h))
		fail("a held ESC is pending");
	if (!dv_host_expire(&h, &ev))
		fail("a held ESC expires");
	if (ev.type != DV_HEV_PTY || ev.n != 1 || ev.data[0] != 0x1b)
		fail("expiry hands the ESC to the window");
	if (dv_host_pending(&h))
		fail("nothing is pending after expiry");

	dv_host_init(&h);
	feed(&h, "\033[");
	if (!dv_host_pending(&h))
		fail("a half-typed CSI is pending");

	dv_host_init(&h);
	feed(&h, "\033_Gi=1");
	if (dv_host_pending(&h))
		fail("a partial graphics reply must keep accumulating");
	if (dv_host_expire(&h, &ev))
		fail("expiry must not chop up a graphics reply");
}

static void
test_paste_passthrough(void)
{
	struct dv_host		h;
	struct dv_host_ev	ev;

	dv_host_init(&h);
	feed(&h, "\033[200~");
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_PTY || ev.n != 6 ||
	    memcmp(ev.data, "\033[200~", 6) != 0)
		fail("bracketed-paste start passes through as bytes");
	feed(&h, "\033[201~");
	if (!dv_host_next(&h, &ev) || ev.type != DV_HEV_PTY || ev.n != 6 ||
	    memcmp(ev.data, "\033[201~", 6) != 0)
		fail("bracketed-paste end passes through as bytes");
}

int
main(void)
{
	test_alt_tap();
	test_alt_chord();
	test_ctrl_backslash();
	test_lone_esc_expires();
	test_plain_a();
	test_key_encoding();
	test_ctrl_space_length();
	test_osc_reply();
	test_cell_size();
	test_mode_2048();
	test_gfx_bel();
	test_paste_passthrough();
	test_gfx_ok();
	test_disable_sequence();
	test_xtversion();
	test_structured_keys();
	if (failed) {
		fprintf(stderr, "test-host: FAILED\n");
		return (1);
	}
	printf("test-host: ok\n");
	return (0);
}
