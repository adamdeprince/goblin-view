/*
 * Copyright (c) 2026 Adam DePrince <adam.deprince@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this notice appear in all copies.
 */

#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include <sys/types.h>
#include <sys/queue.h>

/*
 * TAILQ_FOREACH_SAFE lets a loop free or remove the current element. BSD
 * and macOS <sys/queue.h> define it; glibc's does not, so supply it when
 * missing (glibc still provides TAILQ_FIRST / TAILQ_NEXT).
 */
#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = TAILQ_FIRST((head));				\
	    (var) != NULL && ((tvar) = TAILQ_NEXT((var), field), 1);	\
	    (var) = (tvar))
#endif

#include "goblin_pointer.hpp"
#include <cstddef>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define DV_VERSION "0.1.0"
/*
 * The client/server wire protocol. Bumped whenever anything a client sends
 * or expects changes -- a message's shape, and the command words too, since
 * the client's own Alt tap is a command -- so a new client meeting a server
 * still running an old build says so instead of typing into the void.
 *   2: keys became DV_MSG_KEY; DV_MSG_HELLO added.
 *   3: the menu command is "menu" (was "desq"); list-clients.
 *   4: the upgrade command and takeover; a second HELLO after an upgrade;
 *      send-keys, capture-window, pipe-window, save, restore.
 *   6: cell font styles, unbounded Silkie capabilities, host graphics replies.
 *   7: split-window, kill-window and their menu actions.
 */
#define DV_PROTOCOL 8 /* terminal agent messages and fragment reassembly */

#define DV_MIN_INNER_COLS 1
#define DV_MIN_INNER_ROWS 1
#define DV_DEFAULT_XPIXEL 16
#define DV_DEFAULT_YPIXEL 32
/* Guards against a corrupt length word, not a size anything should reach: a
 * sender with more than this splits a byte stream (dv_conn_send_stream). */
#define DV_MAX_MSG (1024u * 1024u * 1024u)
#define DV_SNAP_COUNT 9
#define DV_PATH_MAX 1024
#define DV_ESC_TIMEOUT 50	/* ms to wait before a lone ESC is Esc */

enum dv_msg_type {
	DV_MSG_IDENTIFY = 1,
	DV_MSG_STDIN,
	DV_MSG_STDOUT,
	DV_MSG_RESIZE,
	DV_MSG_COMMAND,
	DV_MSG_REPLY,
	DV_MSG_EXIT,
	DV_MSG_MOUSE,
	DV_MSG_DETACH,
	DV_MSG_PASSTHROUGH,
	DV_MSG_OSC,		/* client -> server: a host OSC reply */
	DV_MSG_APC,		/* client -> server: a host graphics reply */
	DV_MSG_KEY,		/* client -> server: a structured key (dv_key) */
	DV_MSG_HELLO,		/* server -> client: protocol + version, after IDENTIFY */
    DV_MSG_AGENT_PART,	/* fragment of a terminal agent frame */
    DV_MSG_AGENT_END	/* final fragment; both directions */
};

#define DV_IDENT_TTY		0x01
#define DV_IDENT_GHOSTTY	0x02
#define DV_IDENT_KITTY_GFX	0x04
#define DV_IDENT_CELLSIZE	0x08
#define DV_IDENT_SIXEL		0x10
#define DV_IDENT_DND		0x20		/* host speaks OSC 72 drag and drop */
#define DV_IDENT_MCURSORS	0x40		/* host draws extra cursors (CSI > q) */
#define DV_IDENT_MONO		0x80		/* paint this client in black and white only */

enum dv_host_ev_type {
	DV_HEV_MENU = 1,
	DV_HEV_PTY,
	DV_HEV_SIZE,
	DV_HEV_MOUSE,
	DV_HEV_OSC,		/* a host OSC reply, bytes in dv_host.osc */
	DV_HEV_APC		/* a host graphics reply, bytes in dv_host.osc */
};

/* SGR mouse: button code (with motion/mod bits) and where. */
#define DV_MOUSE_PRESS		1
#define DV_MOUSE_RELEASE	0

struct dv_geom {
	int		 x;
	int		 y;
	unsigned	 cols;
	unsigned	 rows;
};

struct dv_size {
	unsigned	 cols;
	unsigned	 rows;
	unsigned	 xpixel;
	unsigned	 ypixel;
};

struct dv_buf {
	unsigned char	*data;
	size_t		 len;
	size_t		 space;
};

/* A key as a kitty-keyboard host reports it, with the bytes a legacy
 * terminal would have sent; the server encodes it per window. */
struct dv_key {
	unsigned	 code;		/* unicode key code or functional number */
	unsigned	 shifted;	/* alternate: the shifted key, 0 none */
	unsigned	 base;		/* alternate: base-layout key, 0 none */
	unsigned	 mods;		/* kitty modifier bits: shift 1 ... numlock 128 */
	int		 type;		/* 1 press, 2 repeat, 3 release */
	int		 final;		/* the host's final byte: 'u', '~', or a letter */
	unsigned	 text[4];	/* associated text, codepoints */
	int		 ntext;
	unsigned char	 legacy[64];	/* what a plain terminal would get */
	size_t		 nlegacy;
};

struct dv_host_ev {
	int			 type;
	unsigned char		 data[64];
	size_t			 n;
	int			 has_key;	/* a structured key: `key` is set */
	struct dv_key		 key;
	struct dv_size		 size;
	unsigned		 mx;	/* 1-based cell, mouse events */
	unsigned		 my;
	unsigned		 mcode;	/* raw SGR button code */
	int			 mpress;
	int			 mpix;	/* mpx/mpy are a real pixel, 0-based in the host */
	int			 mpx;
	int			 mpy;
};

struct dv_host {
	unsigned char		 buf[8192];
	size_t			 len;
	struct dv_buf pending;	/* input received during bootstrap, drained once */
	size_t pending_pos;
	struct dv_buf		 osc;		/* a host OSC reply being read */
	struct dv_buf		 hold;		/* a CSI/DCS/APC that outgrew buf */
	int			 holding;
	int			 hold_drop;	/* past the configured limit: wait for the end, store nothing */
	int			 hold_csi;	/* holding a CSI; otherwise a DCS or APC */
	int			 hold_esc;	/* ESC seen while dropping a DCS or APC */
	int			 in_osc;	/* still waiting for its ST */
	int			 osc_esc;	/* saw ESC inside it (ESC \ ends) */
	int			 osc_ready;	/* complete: hand it to the client */
	int			 alt_down;
	int			 alt_chord;
	int			 kbd;
	int			 gfx;		/* the host takes kitty graphics */
	int			 sixel;		/* the host renders sixel (DA1 param 4) */
	int			 da;		/* its DA1 reply arrived (bootstrap done) */
	char			 name[64];	/* XTVERSION reply: the terminal's name */
	int			 pointer_caps;
	char			*silkie_blob;	/* key=value lines, grown as replies arrive */
	int			 dnd;		/* answered the OSC 72 query */
	int			 mcursors;	/* answered the multiple-cursors query */
	int			 have_cell;
	int			 pixel_mouse;	/* host confirmed SGR pixel mode (1016) */
	unsigned		 xpixel;
	unsigned		 ypixel;
	unsigned		 cols;
	unsigned		 rows;
};


/*
 * A window is its own terminal, hence its own kitty endpoint: it stores
 * the image the inner program transmits and answers the program's
 * queries. odin-seat then acts as a kitty CLIENT of the host, re-transmitting
 * the stored pixels under a remapped, terminal-global id and placing
 * them where the window sits. Child bytes never reach the host.
 */
#define DV_KITTY_FIRST_ID	0x10000000u	/* our id space (see tmux) */

/* The display keys of an a=T / a=p: columns, rows, source rectangle,
 * z-index, cursor stays (C), placement id, virtual (U). */
struct dv_kreq {
	long	c, r, x, y, w, h, z, C, p, U, X, Y;
};

/*
 * An image a window stores: the window is the kitty endpoint the program
 * talks to. Pixels stay base64-encoded as they arrive (a file or shared
 * memory medium is read here and re-encoded) so they replay to the host
 * with the same transmit keys.
 */
struct dv_kimage {
	int		 used;
	unsigned	 child_id;	/* id the program used (0 = anonymous) */
	struct dv_buf	 data;
	/*
	 * Replayable keys: f=,s=,v=,T=,G=,E=,o=z. Bounded by the protocol:
	 * f is one of the formats odin-seat knows (three digits), s, v and T
	 * are 32-bit (a larger value makes the command malformed, and it is
	 * dropped before this is filled), G is 1 or 2 and E two digits --
	 * at most 57 bytes. capture_fmt checks it all the same.
	 */
	char		 fmt[96];
	int		 direct;	/* sent by the direct medium (t=d) */
	/*
	 * Kitty's indexed colour (f=8, f=16): the palette its indices name
	 * is the window's (dv_screen.pal, over the default theme), and the
	 * picture is made again when that changes (pal_stamp).
	 */
	const struct dv_palette *pal;
	uint64_t	 pal_stamp;
	struct dv_vscene *vscene;	/* f=200: the records, keyed by element id */
	struct dv_kpacks *packs;	/* re-packed per client display, by profile */
	unsigned	 gen;		/* bumped by each completed transmission */
	int		 gpu;		/* t=g: payload is a surface token, not pixels */
	int		 reassembling;	/* mid chunked transmit */
	int		 done;		/* complete */
	unsigned	 wpx;		/* pixel size (0 = unknown) */
	unsigned	 hpx;
	unsigned	 virtual_id;	/* host ID of the U=1 placement, if any */
	unsigned	 hid;		/* host ID shared by its layered placements, 0 none yet */
	int		 want_place;	/* a=T: display once the last chunk lands */
	struct dv_kreq	 req;		/* with these keys */
};

/*
 * Ids of the a=v vector service on the host, which replies to a window's
 * request under one of these. They name requests, not images. The rest of
 * the host's image ids are DV_KITTY_FIRST_ID and DV_STEXT_FIRST up.
 */
#define DV_VREQ_FIRST		0x70000000u
#define DV_VREQ_LAST		0x7fffffffu

/*
 * A placement: kitty's unit of display. A direct one has an origin in
 * screen cells (the row goes negative as it scrolls off the top) and
 * floats above the text like it does in kitty -- redraws beneath leave
 * it, scrolling carries it, ED/RIS/leaving the alternate screen clear it.
 * A virtual one (U=1) is positioned by placeholder cells the program
 * writes itself. Each placement is its own image on the host, so a crop
 * or a scale of the same pixels is simply another transmit.
 *
 * That holds while a cell shows one picture above its text, which is all a
 * placeholder cell can say. A window whose placements go under the text
 * (z below 0), start part way into a cell (X, Y) or overlap one another is
 * layered (dv_window.klayered): a kitty graphics terminal then holds each
 * image once, under dv_kimage.hid, and is sent real placements of it at
 * their z, cut to the cells where the window shows (dv_layers in
 * compose.cc). One whose cell is not the session's holds it scaled to its
 * own cell, and its placements are in its own pixels (dv_view.cell_w).
 */
struct dv_kplace {
	int		 used;
	uint64_t	 img;		/* key in the window's image map */
	unsigned	 child_id;	/* that image's id, for placeholder lookups */
	unsigned	 pid;		/* the program's placement id (0 = none) */
	unsigned	 dv_id;		/* host-global placement id */
	int		 row;		/* origin in screen cells */
	int		 col;
	unsigned	 cols;		/* size in cells */
	unsigned	 rows;
	unsigned	 sx, sy, sw, sh; /* source rectangle in pixels */
	/* As asked for, 0 = not given: what the program takes the size to be. */
	unsigned	 want_c, want_r;
	unsigned	 off_x, off_y;	/* its X and Y within the first cell */
	int		 z;
	int		 virt;		/* U=1 */
	int		 alt;		/* made on the alternate screen */
	uint32_t	 depth;		/* the pen's stereo depth when placed */
	unsigned	 gen;		/* bumped when pixels or geometry change */
};

/*
 * A kitty image is drawn as ordinary grid cells: each cell holds the
 * placeholder codepoint, the image id in its foreground, and its (row,
 * col) within the image. The compositor then clips and occludes it like
 * any text, and the host reassembles it from the Unicode placeholders.
 */
#define DV_KITTY_PLACEHOLDER	0x10EEEEu

/*
 * OSC 66 text sizing. A run of text drawn at an integer scale occupies a
 * rectangle of cells; each such cell carries this marker with the run's
 * host-global id in the foreground and its (row,col) offset in irow/icol,
 * so the run scrolls, clears and is occluded like ordinary text. The
 * compositor re-emits OSC 66 for the run's origin to a host that speaks
 * it, and draws the text unscaled otherwise.
 */
#define DV_STEXT_MARK		0x10EEEFu
#define DV_STEXT_FIRST		0x20000000u	/* run id space, distinct from kitty */
#define DV_STEXT_END		0x30000000u	/* one past its last id */
/*
 * Cells of a layered window flattened to pixels for a sixel terminal carry
 * this with the window's id where a placeholder cell carries its picture's:
 * they are the window's pictures, all of them, as they show in that cell.
 * No picture is ever given an id from here up (host_id_alloc).
 */
#define DV_FLAT_FIRST		0xF0000000u
/* The protocol's limit on the text of one run, in bytes. */
#define DV_STEXT_TEXT_MAX	4096
#define DV_OSC_ROUTES		32		/* pending host-reply routes per session */

struct dv_stext {
	uint8_t		 s;		/* scale, 1-7 */
	uint8_t		 wparam;	/* the w= metadata (0 = auto width) */
	uint8_t		 n, d;		/* fractional scale n/d, 0-15 (0 = none) */
	uint8_t		 v, h;		/* its alignment: 0 top/left, 1 bottom/right, 2 centred */
	uint16_t	 bw;		/* base width in cells */
	uint16_t	 cols;		/* footprint width  = bw * s */
	uint16_t	 rows;		/* footprint height = s */
	uint32_t	 fg;		/* the run's colours, at OSC-66 time */
	uint32_t	 bg;
	uint8_t		 attr;
	uint32_t	 font;		/* Silkie font style, dv_font_style */
	uint32_t	 depth;		/* Silkie stereo depth, dv_depth_style */
	std::string	 text;		/* UTF-8, at most DV_STEXT_TEXT_MAX bytes */
};

/*
 * Input methods. A method turns keystrokes into a preedit that commits
 * Unicode into the focused window, so CJK and accented entry is the same
 * on every terminal. Phonetic tables are hand-authored (no third-party
 * data), so provenance is clean; coverage is deliberately common-subset.
 */
enum {
	DV_IM_OFF = 0,
	DV_IM_ROMAJI,		/* romaji -> hiragana (algorithmic) */
	DV_IM_USINTL,		/* US-International dead keys */
	DV_IM_PINYIN,		/* pinyin -> hanzi candidates */
	DV_IM_ZHUYIN,		/* bopomofo keys -> pinyin -> hanzi */
	DV_IM_WUBI,		/* wubi86 shape codes -> hanzi/words */
	DV_IM_COUNT
};

struct dv_im {
	int		method;
	char		pre[48];	/* the ascii being composed */
	int		prelen;
	char		cand[10][28];	/* candidate strings (UTF-8) */
	int		ncand;
	int		page;		/* candidate page, for long lists */
	int		dead;		/* US-International dead key pending */
};

#define DV_ATTR_BOLD		0x01
#define DV_ATTR_UNDER		0x02
#define DV_ATTR_REV		0x04
#define DV_ATTR_DIM		0x08
#define DV_ATTR_ITALIC		0x10
#define DV_ATTR_STRIKE		0x20
#define DV_ATTR_BLINK		0x40
#define DV_ATTR_WIDE		0x80	/* first cell of a two-column glyph */

/*
 * A colour is a palette index 0-255, an RGB triple tagged DV_COLOR_RGB,
 * or DV_COLOR_DEFAULT, which the host renders with its own theme.
 */
#define DV_COLOR_RGB		0x01000000u
#define DV_COLOR_DEFAULT	0x02000000u
#define DV_RGB(r, g, b)		(DV_COLOR_RGB | ((uint32_t)(r) << 16) | \
				    ((uint32_t)(g) << 8) | (uint32_t)(b))

/*
 * A deep colour (Silkie's SGR 38:0:id:r:g:b, sixteen bits a channel). The
 * cell's colour word keeps the nearest eight-bit colour, so everything
 * that knows only eight bits goes on working, and the cell's _x word how
 * far each channel is from it: a sixteen-bit value c is n * 257 + d with
 * n = (c * 255 + 32767) / 65535 and d in -128..128, kept as d + 128 in
 * nine bits a channel (bits 0-8 red, 9-17 green, 18-26 blue). Bits 27-28
 * hold the gamut, which an underline colour has nowhere else to keep, and
 * bit 31 says the word is set. So a deep colour is exact, needs no table,
 * and is copied, scrolled and saved with its cell. Whatever sets the
 * colour word some other way clears the _x word with it.
 */
#define DV_DEEP_ON		0x80000000u

static inline uint32_t
dv_deep_make(unsigned r, unsigned g, unsigned b, unsigned gamut, uint32_t *rgb8)
{
	const unsigned	c[3] = { r, g, b };
	unsigned	n[3];
	uint32_t	x = DV_DEEP_ON | ((uint32_t)(gamut & 3) << 27);

	for (int k = 0; k < 3; k++) {
		int	d;

		n[k] = (c[k] * 255u + 32767u) / 65535u;
		d = (int)c[k] - (int)(n[k] * 257u);
		x |= (uint32_t)(d + 128) << (9 * k);
	}
	*rgb8 = DV_RGB(n[0], n[1], n[2]);
	return (x);
}

/* Channel k (0 red, 1 green, 2 blue) of a deep colour, 0-65535. */
static inline unsigned
dv_deep_chan(uint32_t rgb8, uint32_t x, int k)
{
	int	n = (int)((rgb8 >> (16 - 8 * k)) & 0xff);
	int	d = (int)((x >> (9 * k)) & 0x1ff) - 128;
	int	v = n * 257 + d;

	return ((unsigned)(v < 0 ? 0 : v > 65535 ? 65535 : v));
}

static inline unsigned
dv_deep_gamut(uint32_t x)
{
	return ((x >> 27) & 3);
}

/*
 * Colour overrides a window's program set (OSC 4/10/11/12/21): they apply
 * to that window only -- the compositor remaps its cells -- so one
 * program cannot recolour the others. OSC 30001/30101 push and pop.
 */
struct dv_xfer;				/* a file transfer in progress (src/xfer.cc) */

#define DV_XCURSORS	256		/* extra cursors a program may set */
struct dv_xcur {
	uint16_t	 row, col;	/* 0-based */
	uint8_t		 shape;		/* 1 block 2 beam 3 underline 29 as main */
};

/* A host OSC reply's way back to the window that asked (see dv_session). */
struct dv_osc_route {
	unsigned	 osc;		/* 0 = free slot */
	char		 id[64];
	unsigned	 win;
	unsigned	 seq;
};

struct dv_palette {
	uint32_t	 idx[256];	/* RGB for palette index N... */
	uint8_t		 set[256];	/* ...when set[N] */
	uint32_t	 fg, bg, cursor;
	uint8_t		 fgset, bgset, curset;
	uint8_t		 any;		/* any override at all */
};

/* ch == 0 marks the second cell of a wide glyph. */
struct dv_cell {
	uint32_t	 ch;
	uint32_t	 fg;
	uint32_t	 bg;
	uint8_t		 attr;
	uint8_t		 ustyle;	/* underline style 0-5 (SGR 4:n); valid when UNDER */
	uint32_t	 ucolor;	/* underline colour (SGR 58), DV_COLOR_DEFAULT = follow fg */
	uint16_t	 irow;		/* if ch==placeholder: cell row in image */
	uint16_t	 icol;		/* and column */
	uint8_t		 alpha;		/* SGR 70, percent, 100 = opaque */
	uint16_t	 fg_br;		/* SGR 56, percent of standard white */
	uint16_t	 bg_br;		/* SGR 57 */
	uint8_t		 fg_gamut;	/* 0 sRGB, 1 Display P3, 2 Rec.2020 */
	uint8_t		 bg_gamut;
	/* Sixteen bits a channel (SGR 38:0/48:0/58:0), 0 = none: dv_deep_make. */
	uint32_t	 fg_x, bg_x, u_x;
	uint32_t	 font;		/* Silkie font style, 0 = the configured face */
	uint32_t	 depth;		/* Silkie stereo depth, 0 = the terminal plane */
	uint8_t		 focus;		/* compositor: painted from the focused window */
};

/*
 * What OSC 777 font selects beyond SGR, interned so a cell holds a number
 * (dv_font_intern). Slant is hundredths of a degree or DV_FONT_SLANT_AUTO,
 * weight 100-900 or 0 for the configured face and SGR bold.
 */
#define DV_FONT_SLANT_AUTO	INT32_MIN
struct dv_font_style {
	std::string	 role;
	int		 weight;
	int		 slant;
};

/*
 * What OSC 777 depth requests for the cells and placements that follow,
 * interned like font styles (dv_depth_intern). Depth is binocular disparity
 * relative to the terminal plane: millidegrees (d=) or reference pixels
 * (p=), never both. y is vertical phase in physical pixels. Id 0 is all
 * zero, the plane itself.
 */
#define DV_DEPTH_FLAT	0	/* no depth requested */
#define DV_DEPTH_DEG	1	/* d: signed millidegrees */
#define DV_DEPTH_PX	2	/* p: reference pixels */
struct dv_depth_style {
	int		 kind;
	int32_t		 d;
	float		 p;
	float		 y;
};

struct dv_cursor_save {
	unsigned	 cr;
	unsigned	 cc;
	uint32_t	 fg;
	uint32_t	 bg;
	uint8_t		 attr;
	int		 valid;
	uint8_t		 ustyle;
	uint32_t	 ucolor;
	uint8_t		 alpha;
	uint16_t	 fg_br, bg_br;
	uint8_t		 fg_gamut, bg_gamut;
	uint32_t	 fg_x, bg_x, u_x;
	uint32_t	 font;
	uint32_t	 depth;
};

/* One Silkie contact this window has been told about. */
struct dv_silk_live {
	int		 used;
	int		 sel;		/* 0 tablet, 1 gesture */
	int		 kind;
	int		 device;
	int		 contact;
	int		 x, y;
	int		 buttons;
	int		 tool;
};

/*
 * A stroke of ink a terminal is drawing for itself. The reports say what it
 * applied to its copy of the scene, and the same records go into the table
 * and on to the other copies.
 */
struct dv_silk_stroke {
	const struct dv_peer *peer;	/* whose pen */
	int		 device = 0;
	int		 contact = 0;
	int		 own = 1;	/* that terminal is drawing it: not told */
	int		 began = 0;	/* its k record is in the table */
	int		 over = 0;	/* no more points */
	int		 down = 1;	/* its first report is still to be taken */
	uint32_t	 id = 0;	/* what the program knows it by */
	uint32_t	 token = 0;	/* what that terminal knows it by, until it ends */
	uint32_t	 group = 0;
	unsigned	 image = 0;	/* the program's id */
	unsigned	 dv_id = 0;	/* the placement it began in */
	unsigned	 gen = 0;
	uint64_t	 scene = 0;	/* the transmission it began on */
	int		 cx = -1, cy = -1;	/* last seen in this cell */
	double		 w0 = 0, w1 = 0, e = 1;
	std::string	 fill;
};

/*
 * Ids a terminal was told to give its strokes, standing for one window's brush
 * on one placement. The id in a report thus says exactly whose image the
 * terminal drew on and with what look, however many windows and terminals are
 * inking and whichever ids their programs chose. Such an id is no id of the
 * program's: that is given when the report arrives.
 */
struct dv_silk_lease {
	uint32_t	 first = 0;
	uint32_t	 n = 0;
	uint32_t	 used = 0;
	uint32_t	 sync = 0;	/* a query behind it answered: it has been seen */
	unsigned	 window = 0;
	unsigned	 dv_id = 0;
	unsigned	 host = 0;	/* the terminal's id of that placement's image */
	std::string	 look;		/* the brush's fields but for k and n */
};

/* What one terminal holds for ink. */
struct dv_silk_ink {
	int		 sent = -1;	/* the mode it was told, -1 = nothing yet */
	uint32_t	 next = 0;	/* the next id to lease */
	uint32_t	 sync = 0;
	std::vector<struct dv_silk_lease> leases;	/* newest last */
	unsigned	 hover_win = 0;	/* the window the pen is over, 0 = none */
	int		 hover_x = 0, hover_y = 0;	/* in it, in 1/256 pixel */
	int		 hover_col = 0, hover_row = 0;
};

/* A stroke or gesture captured on a display peer. */
struct dv_silk_hit {
	int		 used;
	int		 sel;
	int		 kind;
	int		 device;
	int		 contact;
	int		 src;
	int		 conf;
	int		 described;
	int		 reported;
	int		 legacy;	/* delivered as mouse, not OSC */
	int		 buttons;
	int		 tool;
	unsigned	 window;
	unsigned	 epoch;
};

struct dv_silk_desc {
	int		 device;
	int		 kind;
	std::string	 fields;
};

#define DV_VT_STR		256

struct dv_screen {
	unsigned	 cols;
	unsigned	 rows;
	struct dv_cell	*cells;
	struct dv_cell	*alt;
	unsigned	 cr;
	unsigned	 cc;
	unsigned	 mt;		/* scroll top, 0-based */
	unsigned	 mb;		/* scroll bottom, inclusive */
	int		 wrap;		/* DECAWM */
	int		 visible;	/* DECTCEM */
	int		 alt_on;
	int		 app_cursor;	/* DECCKM */
	int		 insert;	/* IRM */
	int		 mouse;		/* 0 off, 1000 click, 1002 drag, 1003 any */
	int		 alt_scroll;	/* DECSET 1007: wheel -> arrows on the alt screen */
	int		 focus_report;	/* DECSET 1004 */
	int		 bracketed;	/* DECSET 2004: app wraps pastes in ESC[200~/201~ */
	int		 sync;		/* DECSET 2026: an update is under way, not to be shown yet */
	int		 sync_done;	/* one just ended: dv_vt_write_frame stops there */
	int		 pointer_mode;
	int		 mouse_sgr;	/* 0 X10, 1 SGR (1006), 2 SGR pixels (1016) */
	int		 kbd[2][8];	/* kitty keyboard flag stacks: main, alternate */
	int		 nkbd[2];
	/* Extra cursors (kitty multiple cursors, CSI > ... SP q). */
	struct dv_xcur	 xcur[DV_XCURSORS];
	int		 nxcur;
	uint8_t		 xcur_tcs, xcur_ccs;	/* colour spaces: 0 unset 1 special 2 rgb 5 index */
	uint32_t	 xcur_tcol, xcur_ccol;
	int		 sixel_dm;	/* DECSDM (mode 80): sixel at top left */
	int		 sixel_right;	/* mode 8452: cursor right of a sixel */
	uint32_t	 fg;
	uint32_t	 bg;
	uint8_t		 attr;
	uint8_t		 ustyle;	/* current underline style */
	uint32_t	 ucolor;	/* current underline colour */
	uint8_t		 alpha;		/* pen: SGR 70, 100 = opaque */
	uint16_t	 fg_br, bg_br;
	uint8_t		 fg_gamut, bg_gamut;
	uint32_t	 fg_x, bg_x, u_x;	/* pen: deep colours */
	uint32_t	 font;		/* pen: Silkie font style */
	uint32_t	 depth;		/* pen: Silkie stereo depth */
	/* Per-pane Silkie modes. The host is the union, pushed when it changes. */
	int		 silk_tablet, silk_scroll, silk_gesture;
	int		 silk_frame, silk_latency, silk_display, silk_ink;
	unsigned	 silk_frame_seq;
	int		 silk_frame_dirty;
	char		*silk_brush;	/* the ink brush's fields, NULL = none */
	char		*silk_shown;	/* the display body last reported */
	int		 sace;		/* DECSACE: 0/1 stream, 2 rectangle (for DECCARA) */
	/*
	 * Colour overrides this window's program set (OSC 4/10/11/12/21):
	 * they apply to this window only -- the compositor remaps its cells
	 * -- so one program cannot recolour the others. 30001/30101 push
	 * and pop the whole set.
	 */
	struct dv_palette pal;
	/* Pushed sets, as deep as the program pushes: none are dropped. */
	std::vector<struct dv_palette> *palstack;
	/* XTSAVE / XTRESTORE (CSI ? Pm s / r): saved private modes. */
	int		 smode_num[16];
	int		 smode_val[16];
	int		 nsmode;
	struct dv_cursor_save saved;
	struct dv_cursor_save saved_alt;
	int		 charset[2];	/* G0, G1: 0 ASCII, 1 DEC graphics */
	int		 shift;		/* 0 = G0, 1 = G1 */
	uint32_t	 last_ch;	/* for REP */

	/* Scrollback for the primary screen: lines that scrolled off the
	 * top, a ring, plus how far the viewer has scrolled back into it. */
	struct dv_cell	**sb;		/* ring of row pointers (sb_cap slots) */
	unsigned	 *sb_w;		/* the width each stored row was captured at */
	unsigned	  sb_cap;	/* ring capacity */
	unsigned	  sb_len;	/* lines held */
	unsigned	  sb_head;	/* index of the oldest line */
	unsigned	  sb_off;	/* lines scrolled back from the live bottom (0 = live) */

	/* OSC 66 scaled-text runs, by the id their cells carry: as many as
	 * the cells of the screens and the scrollback still name. Those no
	 * cell names are swept out when stext_sweep runs are held. */
	std::unordered_map<unsigned, dv_stext> *stexts;
	size_t		  stext_sweep;
	int		  sb_stext;	/* a scrollback row may hold part of a run */

	/* Parser state persists across writes: a pty splits sequences. */
	int		 st;
	int		 prefix;	/* private marker: ? > < = (or a lone intermediate) */
	int		 inter;		/* intermediate byte: $ * + SP ! ... */
	int		*params;	/* grown as a CSI arrives; NULL until the first parameter */
	uint8_t		*sub;
	unsigned	 param_cap;
	int		 np;
	int		 val;
	int		 next_sub;
	size_t		 csi_bytes;	/* bytes of the CSI in progress, after ESC [ */
	int		 csi_drop;	/* csi-limit exceeded: ignore the sequence */
	int		 str_drop;	/* escape-limit exceeded: ignore the OSC/APC/DCS */
	char		 str[DV_VT_STR];
	unsigned	 slen;
	int		 str_esc;
	int		 str_kind;	/* STR_OSC / STR_APC / ... */
	uint32_t	 ucp;
	int		 uneed;

	/* Kitty placements live with the grid: they scroll with it and go
	 * with ED, RIS and leaving the alternate screen. */
	/* Allocated lazily: screens are zero-initialized with calloc/memset. */
	std::unordered_map<unsigned, dv_kplace> *kpl;
	unsigned	 kgen;
	/* The placeholder cell just written, for its trailing diacritics
	 * (row, column, id high byte); ph_n < 0 when there is none. */
	unsigned	 ph_x;
	unsigned	 ph_y;
	int		 ph_n;
};

/*
 * The pixel formats beyond f=24, 32 and 100 a terminal listed (caps format=),
 * as bits of dv_view.packed: Silkie's packed gray and RGB565, kitty's
 * indexed colour, Silkie's palette images and its high precision formats.
 * DV_FMT_E is the transfer function key E, which a terminal is taken to
 * read when it lists a high precision format (one specification has both).
 */
#define DV_FMT_G1	(1 << 0)	/* f=301 */
#define DV_FMT_G2	(1 << 1)	/* f=302 */
#define DV_FMT_G4	(1 << 2)	/* f=304 */
#define DV_FMT_G8	(1 << 3)	/* f=308 */
#define DV_FMT_565	(1 << 4)	/* f=516 */
#define DV_FMT_I8	(1 << 5)	/* f=8, kitty indexed colour */
#define DV_FMT_I16	(1 << 6)	/* f=16, indexed colour and alpha */
#define DV_FMT_P1	(1 << 7)	/* f=401, palette images */
#define DV_FMT_P2	(1 << 8)	/* f=402 */
#define DV_FMT_P4	(1 << 9)	/* f=404 */
#define DV_FMT_P8	(1 << 10)	/* f=408 */
#define DV_FMT_G16	(1 << 11)	/* f=316, 16-bit gray */
#define DV_FMT_10A2	(1 << 12)	/* f=632, RGB10A2 */
#define DV_FMT_16A	(1 << 13)	/* f=664, 16-bit RGBA */
#define DV_FMT_E	(1 << 14)	/* the E key */

/* What one display client last saw: the compositor diffs against it. */
struct dv_view {
	struct dv_cell	*last;
	unsigned	 cols;		/* viewport size = min(tty, desktop) */
	unsigned	 rows;
	int		 vx;		/* viewport origin in desktop cells */
	int		 vy;
	int		 cx;
	int		 cy;
	int		 vis;
	int		 full;
	int		 font;		/* this client's host listed font.1 */
	int		 depth;		/* this client's host listed depth.1 */
	int		 deep;		/* and deep in its sgr list: SGR 38:0 colours */
	int		 packed;	/* DV_FMT_*: the formats beyond f=24/32/100 it listed */
	/* Its display, from the terminal's display reply. */
	int		 disp_color;	/* 0 rgb or unknown, 1 gray, 2 mono */
	int		 disp_bits[4];	/* red, green, blue, gray; 0 unknown */
	/* --3d-focus: the focused window this many reference pixels nearer. */
	float		 focus_px;
	unsigned	 focus_win;	/* the focused window it was last painted with */
	std::unordered_map<uint32_t, uint32_t> *focus_depth;	/* depth -> nearer */
	/* The depth each placement was last made at for it, when it shows depth. */
	std::unordered_map<unsigned, uint32_t> *kimg_depth;
	int		 vector;	/* this client's host takes f=200 */
	int		 hdr;		/* and f=64, HDR pixels */
	int		 gfx;		/* this client's host takes kitty graphics */
	int		 wg;		/* it knows colours and pixels in a wider gamut */
	int		 sixel;		/* this client's host renders sixel */
	int		 mcursors;	/* this client's host draws extra cursors */
	int		 mono;		/* strict black and white: ink, paper, reverse */
	/* Its host shows no pictures of any kind: they are drawn for it as
	 * cells, two coloured halves each. Set by the server for each frame. */
	int		 mosaic;
	char		*xcur_last;	/* the extra-cursor set last sent to it */
	/* Images (by id+gen) transmitted to this client as virtual placements. */
	std::unordered_map<unsigned, unsigned> *kimg_gen;
	/* Images sent without a placement, for a pointer or a vector scene. */
	std::unordered_map<unsigned, unsigned> *kres_gen;
	/*
	 * The pixels of its cell, and of the cell the session's programs were
	 * told (the largest of the attached terminals', see recalculate_size).
	 * Where they differ it is sent the pictures of layered windows scaled
	 * to its own cell, and their placements in its own pixels. Set by the
	 * server for each frame; 0 before the first.
	 */
	unsigned	 cell_w, cell_h;
	unsigned	 scell_w, scell_h;
	/* Images of layered windows it holds (dv_kimage.hid -> gen). */
	std::unordered_map<unsigned, unsigned> *limg_gen;
	struct dv_layers *layers;	/* the real placements it shows */
	/* Session fonts uploaded to this client, by host handle serial. */
	std::unordered_set<unsigned> *vfont_sent;
};

#define DV_IN_NORMAL		0
#define DV_IN_REARRANGE		1
#define DV_IN_MOVE		2
#define DV_IN_RESIZE		3
#define DV_IN_POSITION		4
#define DV_IN_SPLIT		5
#define DV_IN_TILE		7	/* menu L: a letter picks the layout */

/* session.drag modes (not input_mode): forward to app, or text-select. */
#define DV_DRAG_FORWARD		5
#define DV_DRAG_SELECT		6

struct dv_conn {
	int		 fd;
	struct dv_buf	 in;
	struct dv_buf	 out;
};

struct dv_window;
struct dv_session;
struct dv_peer;
struct dv_server;

struct dv_pstatus;			/* OSC 7501 records (src/pstatus.cc) */

struct dv_window {
	char *notification_pending;
	struct dv_pstatus	*pstatus;	/* its program's status records, or null */
	int focus_state;
	unsigned		 id;
	char			 title[64];
	char			 cmd[128];
	char			 cwd[DV_PATH_MAX];
	int			 z;
	struct dv_geom		 geom;
	struct dv_geom		 saved;
	/*
	 * The place it had before a smaller desktop squeezed it, and the one
	 * it was squeezed to. While it is still there, untouched, a desktop
	 * that grows again gives it its own place back (dv_desktop_resize).
	 */
	struct dv_geom		 want, fitted;
	int			 squeezed;
	int			 framed;
	int			 zoomed;
	int			 title_locked;	/* user named it; ignore app OSC title */
	int			 hidden;
	int			 selected;	/* one of the windows menu B types to and L lays out */
	int			 aside;
	int			 frozen;
	int			 locked;
	unsigned		 lcols;
	unsigned		 lrows;
	unsigned		 ox;
	unsigned		 oy;
	int			 pty_fd;
	int			 pfd_slot;	/* its pty's entry in this pass's poll list */
	long long		 sync_since;	/* when its update under way was first seen, ms; 0 none */
	pid_t			 pid;
	unsigned pointer_epoch;
	int npointer;
	goblin::pointer::Message pointer_active[32];
	std::vector<struct dv_silk_live> *silk_live;	/* grown with the contacts */
	std::vector<struct dv_silk_stroke> *silk_strokes;
	unsigned		 silk_epoch;
	struct dv_buf		 cap;		/* the OSC/APC string being read */
	std::unordered_map<uint64_t, dv_kimage> *kimg;
	uint64_t		 kreassem;	/* image key mid-transmit, 0 none */
	uint64_t		 kanon;		/* anonymous keys above UINT32_MAX */
	int			 klayered;	/* direct placements are layered (dv_kplace) */
	/* Host ids of images used without a placement, by the program's id. */
	std::unordered_map<unsigned, unsigned> *kres;
	struct dv_vwin		*vwin;		/* vector service and session fonts */
	struct dv_buf		 host_out;	/* OSC-52 etc. bound for the host */
	int			 clip_query;	/* host_out holds an OSC-52 read */
	/*
	 * OSC 22 mouse pointer shapes: this window's own stack; the top is
	 * applied to the host while the window is focused. A name is a CSS
	 * pointer name or image:<id>:<x>:<y> with three 32 bit numbers, which
	 * fits in 48 bytes.
	 */
	char			 pshape[16][48];
	int			 npshape;
	struct dv_buf		 pty_out;	/* for the program: drained as its pty takes it */
	struct dv_xfer		*xfer;		/* file transfer in progress, or null */
	int			 dnd_accept;	/* OSC 72 t=a: the program takes drops */
	char			 dnd_types[256];	/* ...of these MIME types */
	int			 pipe_fd;	/* pipe-window: raw output copied here, or -1 */
	char			 cmdv[1024];	/* the program's argv, NUL-separated; empty = the shell */
	unsigned		 cmdv_len;
	int			 resize_req;	/* program asked to resize (CSI 8 t) */
	unsigned		 req_cols;	/* to this content size (0 = max) */
	unsigned		 req_rows;
	struct dv_screen	 screen;
	struct dv_session	*session;
	TAILQ_ENTRY(dv_window)	 entry;
};

struct dv_desktop {
	unsigned		 cols;
	unsigned		 rows;
	unsigned		 xpixel;
	unsigned		 ypixel;
	unsigned		 next_snap;
	unsigned		 cascade;	/* nth framed window, for offset */
	TAILQ_HEAD(dv_windows, dv_window) windows;
	struct dv_window	*focused;
	int			 gfx;		/* a client showing this can display images (kitty or sixel) */
	int			 mcursors_host;	/* a client's host draws extra cursors */
};

struct dv_session {
	char *notification_scope;
	char			*pstatus_root;	/* the root record the terminals were last sent */
	int host_unfocused;
	char			 name[64];
	struct dv_desktop	 desktop;
	int			 input_mode;
	int			 menu_open;
	int			 quit_confirm;
	unsigned		 kill_confirm;	/* target window id, 0 when no question */
	int			 help_open;	/* the ? help box */
	/* A file transfer asking the user's yes or no (src/xfer.cc). */
	int			 xfer_prompt;
	unsigned		 xfer_prompt_win;
	unsigned		 hover_win;	/* window under the pointer, 0 none */
	/* A drag in progress (OSC 72 from the host), routed by position. */
	unsigned		 dnd_win;	/* window under the drag, 0 none */
	char			 dnd_mimes[512];	/* what is being dragged */
	int			 dnd_self;	/* we accepted it: the data is ours to paste */
	unsigned		 dnd_target;	/* ...into this window */
	int			 dnd_idx;	/* the MIME index we asked for */
	int			 dnd_peer_fd;	/* the host dragging */
	struct dv_buf		 dnd_data;
	uint8_t			*xcur_map;	/* extra cursors per desk cell, when composed */
	/* The window whose own cells show at each desk cell (0: a frame, the
	 * backdrop or a menu), when composed with a layered window on show. */
	std::vector<unsigned>	*owner;
	/* Decoded pictures kept between frames for a terminal shown them as
	 * cells (compose.cc). */
	struct dv_flat_cache	*flat;
	size_t			 xcur_map_n;
	int			 xcur_any;
	char			 xfer_text[4][100];
	int			 xfer_nlines;
	int			 in_paste;	/* mid host bracketed paste (a drop or Ctrl-V) */
	struct dv_buf		 paste;		/* its content, re-wrapped for the focused window */
	int			 pointer_caps;
	unsigned		 clip_query_win;/* window awaiting an OSC-52 reply */
	/*
	 * Host OSC replies routed back to the window that asked: kitty
	 * clipboard (5522), notifications (99), file transfer (5113), pointer
	 * shape queries (22). Keyed by OSC number and the request's id.
	 */
	struct dv_osc_route	 routes[DV_OSC_ROUTES];
	unsigned		 route_seq;
	/* Mouse text selection on a window, in that window's screen cells. */
	int			 sel_active;	/* dragging now */
	int			 sel_done;	/* a selection is shown */
	unsigned		 sel_win;	/* window id being selected in */
	int			 sel_ax;	/* anchor col,row */
	int			 sel_ay;
	int			 sel_cx;	/* current col,row */
	int			 sel_cy;
	int			 uni_open;
	int			 uni_cat;
	struct dv_im		 im;
	int			 im_choose;	/* the method chooser overlay */
	/* The switch-window list (menu S): a filter typed over the windows. */
	int			 switch_open;
	size_t			 switch_sel;	/* index into the filtered list */
	char			 switch_filter[32];
	int			 broadcast;	/* typing goes to every window */
	/* Copy mode (menu M): a cursor over a window's scrollback and screen
	 * (virtual rows: scrollback lines, then screen rows), a selection, a
	 * search. y keeps the text as the mark, which Transfer (T) pastes. */
	int			 copy_open;
	unsigned		 copy_win;
	int			 copy_x;
	long			 copy_y;
	int			 copy_sel;
	int			 copy_ax;
	long			 copy_ay;
	int			 copy_searching;	/* typing a pattern: 1 forward, -1 back */
	int			 copy_dir;		/* the last search's direction */
	char			 copy_pattern[64];
	struct dv_buf		 mark;
	/* Inline title editing (double-click a window's title bar). */
	int			 title_edit;
	unsigned		 title_win;
	char			 title_ed[64];
	int			 title_edlen;
	long long		 last_click_ms;	/* for double-click detection */
	unsigned		 last_click_win;
	int			 last_click_x;
	int			 last_click_y;
	int			 uni_sel;
	int			 dirty;
	/* Silkie capabilities of the attached displays, and the last modes
	 * pushed to them. ext is the union of selectors; format and media
	 * are the intersection across graphics peers. */
	char			*silk_ext;
	char			*silk_media;
	char			*silk_format;
	char			*silk_display;
	char			*silk_font_roles;	/* configuration order, default first */
	int			 silk_font_caps, silk_font_synth;
	int			 silk_font_wmin, silk_font_wmax;
	int			 silk_font_smin, silk_font_smax;
	int			 silk_frame_caps, silk_frame_period;
	int			 silk_gesture_caps, silk_haptic_caps, silk_ink_caps;
	int			 silk_latency_caps, silk_latency_input, silk_latency_repaint;
	int			 silk_scroll_caps, silk_tablet_caps;
	int			 silk_any;
	int			 silk_wg;	/* a graphics peer takes a gamut (G=); the rest get sRGB */
	/* A terminal attached draws vector scenes itself, and so answers the
	 * vector service: with none, it is answered here (vtext.cc). */
	int			 silk_vector;
	int			 silk_sent_tablet, silk_sent_scroll, silk_sent_gesture;
	int			 silk_sent_frame, silk_sent_latency, silk_sent_display;
	int			 silk_pad_dev;
	std::vector<int>	*silk_pad_btn;	/* the pad's keys that are down */
	unsigned		 silk_pad_win;
	std::vector<struct dv_vreq> *vreqs;	/* vector requests the host owes */
	int			 drag;		/* 0 none, DV_IN_MOVE, DV_IN_RESIZE, 3 forward */
	unsigned		 drag_win;	/* window id being dragged */
	int			 grab_dx;	/* press offset into the window */
	int			 grab_dy;
	struct dv_geom		 mode_saved;
	TAILQ_ENTRY(dv_session)	 entry;
};

struct dv_pointer_capture {
    unsigned window;
    int used, legacy;
    unsigned epoch;
    goblin::pointer::Message event;
};
struct dv_peer {
    struct dv_buf agent_input;
	int			 fd;
	int			 pfd_slot;	/* its entry in this pass's poll list */
	int			 behind;	/* not sent the latest frame: was still writing */
	struct dv_conn		 conn;
	int			 identified;
	int			 pointer_caps;
	dv_pointer_capture pointer_contacts[32];
    int pointer_device_count, pointer_host_device[64], pointer_app_device[64];
	int			 display;
	int host_unfocused;
	struct dv_size		 tty;
	char			 term[64];
	char			 cwd[DV_PATH_MAX];
	int			 features;
	float			 focus_px;	/* its --3d-focus */
	char			 host[64];	/* the terminal's XTVERSION name, "" unknown */
	struct dv_session	*session;
	struct dv_view		 view;
	char			*silkie_blob;
	std::vector<struct dv_silk_hit> *silk_hit;
	std::vector<struct dv_silk_desc> *silk_desc;
	struct dv_silk_ink	*silk_ink;
	int			 silk_scroll_resid;
	TAILQ_ENTRY(dv_peer)	 entry;
};

struct dv_server {
	int			 listen_fd;
	char			*sock_path;
	char			*sock_dir;
	int			 debug;
	TAILQ_HEAD(, dv_session) sessions;
	TAILQ_HEAD(, dv_peer)	 peers;
};

extern struct dv_server	*dv_srv;
extern int		 dv_verbose;
extern int		 dv_debug;
extern int		 dv_mono;	/* client: -m / $ODIN_SEAT_MONO, black and white */
extern float		 dv_focus_px;	/* client: --3d-focus, reference pixels nearer */
uint32_t	 dv_index_rgb(unsigned);	/* the default RGB of a palette index */

void		*xmalloc(size_t);
void		*xcalloc(size_t, size_t);
void		*xrealloc(void *, size_t);
char		*xstrdup(const char *);
char		*xasprintf(const char *, ...)
		    __attribute__((format(printf, 1, 2)));
size_t		 dv_strlcpy(char *, const char *, size_t);
void		 dv_strset(char **, const char *);

template<class T>
T *
dv_new()
{
	return static_cast<T *>(xcalloc(1, sizeof(T)));
}

template<class T>
T *
dv_allocn(size_t n)
{
	return static_cast<T *>(xcalloc(n, sizeof(T)));
}

template<class T>
T *
dv_resize(T *p, size_t bytes)
{
	return static_cast<T *>(xrealloc(p, bytes));
}

void		 dv_log(const char *, ...)
		    __attribute__((format(printf, 1, 2)));
[[noreturn]] void dv_fatal(const char *, ...)
		    __attribute__((format(printf, 1, 2)));

void		 dv_buf_append(struct dv_buf *, const void *, size_t);
void		 dv_buf_consume(struct dv_buf *, size_t);
void		 dv_buf_free(struct dv_buf *);

int		 dv_msg_pack(struct dv_buf *, int, const void *, size_t);
int		 dv_msg_take(struct dv_buf *, int *, unsigned char **, size_t *);
int		 dv_conn_read(struct dv_conn *);
int		 dv_conn_flush(struct dv_conn *);
int		 dv_conn_send(struct dv_conn *, int, const void *, size_t);
void		 dv_conn_send_stream(struct dv_conn *, int, const void *, size_t);
void dv_conn_send_agent(struct dv_conn *, const void *, size_t);
void		 dv_conn_init(struct dv_conn *, int);
void		 dv_conn_free(struct dv_conn *);

char		*dv_socket_path(const char *label, const char *path_override);
int		 dv_socket_listen(const char *);
int		 dv_socket_connect(const char *);
int		 dv_socket_start_server(const char *, int debug, char **argv0);

/* Two border cells must leave room for at least one content cell. */
inline bool
dv_frame_fits(unsigned cols, unsigned rows)
{
	return (cols >= DV_MIN_INNER_COLS + 2 && rows >= DV_MIN_INNER_ROWS + 2);
}

/* Keep the frame preference while a tiny desktop temporarily hides it. */
inline bool
dv_window_framed(const struct dv_window *w)
{
	return (w->framed && !w->zoomed && (w->session == nullptr ||
	    dv_frame_fits(w->session->desktop.cols, w->session->desktop.rows)));
}

unsigned	 dv_frame_cols(const struct dv_geom *, int framed);
unsigned	 dv_frame_rows(const struct dv_geom *, int framed);
unsigned	 dv_desktop_next_window_id(const struct dv_desktop *);
int		 dv_split_geom(const struct dv_geom *, int framed, int below,
		    struct dv_geom *first, struct dv_geom *second);
/*
 * Ways to lay several windows out at once (menu L). Each gives every window
 * a rectangle that is a function of the desktop and of how many there are.
 */
#define DV_TILE_STACK	1	/* each the whole desktop, one on another */
#define DV_TILE_GRID	2	/* rows and columns, as near square as the screen allows */
#define DV_TILE_ROWS	3	/* horizontal bars: each the full width */
#define DV_TILE_COLS	4	/* vertical bars: each the full height */
#define DV_TILE_2COL	5	/* two columns, the windows shared between them */

std::vector<struct dv_geom> dv_tile_frames(unsigned desk_cols, unsigned desk_rows,
		    unsigned xpixel, unsigned ypixel, int layout, size_t n);

/*
 * The controls on a framed window's top edge, by the column of the frame
 * they are at: four that send it to a half of the desktop and one that
 * selects it. A frame narrower than DV_FRAME_BUTTONS_MIN has none.
 */
#define DV_FB_NONE	0
#define DV_FB_LEFT	1
#define DV_FB_UP	2
#define DV_FB_DOWN	3
#define DV_FB_RIGHT	4
#define DV_FB_SELECT	5
#define DV_FRAME_BUTTONS_MIN	16
#define DV_FRAME_BUTTONS_W	8	/* columns they take, right of the title */

int		 dv_frame_button(unsigned frame_cols, unsigned x);
int		 dv_half_snap(int dx, int dy);

void		 dv_snap_geom(unsigned desk_cols, unsigned desk_rows,
		    int snap, int framed, struct dv_geom *);
void		 dv_clamp_geom(unsigned desk_cols, unsigned desk_rows,
		    int framed, struct dv_geom *);
void		 dv_zoom_geom(unsigned desk_cols, unsigned desk_rows,
		    struct dv_geom *);
void		 dv_desktop_init(struct dv_desktop *, struct dv_size);
void		 dv_desktop_resize(struct dv_desktop *, struct dv_size);
void		 dv_desktop_apply_zoom(struct dv_window *);
void		 dv_desktop_apply_unzoom(struct dv_window *);
void		 dv_geom_nudge(struct dv_geom *, int dx, int dy,
		    unsigned desk_cols, unsigned desk_rows, int framed);
void		 dv_geom_grow(struct dv_geom *, int dcols, int drows,
		    unsigned desk_cols, unsigned desk_rows, int framed);

/* Multiplexer-side kitty (src/tmuxport/kitty.cc). */
void		 dv_kitty_apc(struct dv_window *, const unsigned char *cap,
		    size_t len);
void		 dv_kitty_free(struct dv_window *);
void		 dv_kitty_close(struct dv_window *);
void		 dv_kitty_free_session(struct dv_session *);
size_t		 dv_kitty_scene_slots(const struct dv_kimage *);
void		 dv_kitty_host_reply(struct dv_peer *, const unsigned char *cap,
		    size_t len);
void		 dv_kitty_peer_gone(struct dv_peer *);
void		 dv_kitty_sync(struct dv_session *, struct dv_view *,
		    struct dv_buf *out);
int		 dv_kitty_pointer_image(struct dv_window *, const char *name,
		    char *out, size_t cap);
size_t		 dv_kitty_pointer_reply(struct dv_window *, const char *in,
		    size_t n, struct dv_buf *out);
/* Strong definitions live in the server. Weak no-ops let unit tests link. */
int		 dv_kitty_send_primary(struct dv_session *, const char *, size_t);
void		 dv_kitty_send_patch(struct dv_session *, unsigned dv_id,
		    unsigned gen, const char *, size_t,
		    const struct dv_peer *except);
void		 dv_kitty_sync_views(struct dv_session *);
struct dv_peer	*dv_host_first_peer(void);
void		 dv_host_send_peer(struct dv_peer *, const char *bytes, size_t n);
void		 dv_kitty_send_patch_to(const struct dv_peer *, unsigned dv_id, unsigned gen,
		    const char *bytes, size_t n);
void		 dv_kitty_peer_stale(const struct dv_peer *, unsigned dv_id);
/* An edit of a scene, to the clients that hold the image once for a layered
 * window (dv_kimage.hid) as it was at `was`: they hold it at `now`. */
void		 dv_kitty_send_layer_patch(struct dv_session *, unsigned hid,
		    unsigned was, unsigned now, const char *bytes, size_t n,
		    const struct dv_peer *except, int not_scaled);
void		 dv_kitty_send_layer_patch_to(const struct dv_peer *, unsigned hid,
		    unsigned gen, const char *bytes, size_t n);
int		 dv_kitty_layered(const struct dv_window *, const struct dv_view *);
unsigned	 dv_kitty_host_image(struct dv_window *, const struct dv_view *,
		    unsigned dv_id);
unsigned	 dv_kitty_layer_id(struct dv_window *, uint64_t img);
int		 dv_view_scaled(const struct dv_view *);
unsigned	 dv_scale_len(uint64_t n, unsigned to, unsigned from);
unsigned	 dv_scale_size(uint64_t n, unsigned to, unsigned from);
int		 dv_kitty_scalable(const struct dv_kimage *);
/* Pixel formats (f=): its DV_FMT_* bit (0 for 24, 32, 64, 100 and 200 and
 * for numbers that are no format), whether odin-seat knows it, and whether
 * the G key means something for it. */
int		 dv_kitty_fmt_bit(long f);
int		 dv_kitty_fmt_known(long f);
int		 dv_kitty_fmt_gamut(long f);
/* The DV_FMT_* bits of a terminal's caps format= list. */
int		 dv_kitty_fmt_list(const char *csv);
void		 dv_gamut_to_srgb(int gamut, unsigned char rgb[3]);
void		 dv_kitty_packs_forget(struct dv_session *);
int		 dv_kitty_layer_send(struct dv_window *, uint64_t img,
		    struct dv_buf *out, struct dv_view *);
/* With a view: depth for a host that listed depth.1, and packed pixels
 * expanded for a host that did not list them. */
void		 dv_kitty_transmit(struct dv_window *, const struct dv_kplace *,
		    struct dv_buf *out, const struct dv_view *v = nullptr,
		    uint32_t depth = UINT32_MAX);
/* The placement alone, made again at another depth: no pixels. */
void		 dv_kitty_replace(struct dv_window *, const struct dv_kplace *,
		    struct dv_buf *out, const struct dv_view *v, uint32_t depth);
void		 dv_kitty_delete(unsigned dv_id, struct dv_buf *out);
struct dv_kplace *dv_kitty_lookup(struct dv_session *, unsigned dv_id,
		    struct dv_window **);
unsigned	 dv_kitty_virtual_id(struct dv_window *, unsigned child_id);
unsigned	 dv_kitty_ink_target(struct dv_window *, unsigned child_id);
unsigned	 dv_kitty_ink_under(struct dv_window *, unsigned child_id, int x, int y);
void		 dv_kitty_ink_drop(struct dv_window *, const struct dv_peer *,
		    unsigned dv_id, uint32_t token);
void		 dv_kitty_ink_settle(struct dv_window *, const struct dv_silk_stroke *);
int		 dv_kitty_ink_bind(struct dv_window *, struct dv_silk_stroke *,
		    int x, int y);
int		 dv_kitty_ink_live(struct dv_window *,
		    const struct dv_silk_stroke *);
int		 dv_kitty_ink_point(struct dv_window *, struct dv_silk_stroke *,
		    int x, int y, double *ox, double *oy);
void		 dv_kitty_ink_report(struct dv_window *, struct dv_silk_stroke *,
		    int *x, int *y);
void		 dv_kitty_ink_record(struct dv_window *,
		    const struct dv_silk_stroke *, const std::string &line);
void		 dv_silkie_ink_resent(struct dv_window *, const struct dv_view *,
		    unsigned dv_id);
int		 dv_kitty_host_gfx(const struct dv_window *);
void		 dv_kitty_place_rgba(struct dv_window *, const unsigned char *rgba,
		    unsigned wpx, unsigned hpx, int at_home, int cursor_right);
int		 dv_sixel_decode(const unsigned char *d, size_t n,
		    unsigned char **rgba, unsigned *wpx, unsigned *hpx);
void		 dv_sixel_encode(const unsigned char *rgba, unsigned iw,
		    unsigned ih, unsigned x, unsigned y, unsigned w, unsigned h,
		    struct dv_buf *out);
int		 dv_png_decode(const unsigned char *data, size_t n,
		    unsigned char **rgba, unsigned *w, unsigned *h);
int		 dv_kimage_rgba(const struct dv_kimage *, unsigned char **rgba,
		    unsigned *w, unsigned *h);

/* Silkie font styles (src/silkie.cc). */
uint32_t	 dv_font_intern(const char *role, int weight, int slant);
const struct dv_font_style *dv_font_get(uint32_t);
void		 dv_font_select(uint32_t font, struct dv_buf *out);
void		 dv_font_save(struct dv_buf *out);
void		 dv_font_restore(uint32_t id, const char *role, int weight,
		    int slant);
uint32_t	 dv_font_slot(const struct dv_session *, uint32_t font, int sgr);
uint32_t	 dv_depth_intern(const struct dv_depth_style *);
uint32_t	 dv_depth_nearer(uint32_t depth, float px);
int		 dv_parse_px(const char *, float *);
const struct dv_depth_style *dv_depth_get(uint32_t);
void		 dv_depth_select(uint32_t depth, struct dv_buf *out);
void		 dv_depth_save(struct dv_buf *out);
int		 dv_depth_restore(const char *line);
void		 dv_silkie_free_session(struct dv_session *);
void		 dv_silkie_free_peer(struct dv_peer *);
void		 dv_silkie_free_window(struct dv_window *);
int		 dv_silkie_headroom(const struct dv_session *, char *out, size_t cap);

void		 dv_screen_init(struct dv_screen *, unsigned cols,
		    unsigned rows);
void		 dv_screen_free(struct dv_screen *);
void		 dv_screen_resize(struct dv_screen *, unsigned cols,
		    unsigned rows);
struct dv_cell	*dv_screen_at(struct dv_screen *, unsigned x, unsigned y);
void		 dv_vt_write(struct dv_window *, const void *, size_t);
/* The same, stopping where a synchronized update (mode 2026) ends, with
 * *ended set: the screen is then as its program means it to be seen. */
size_t		 dv_vt_write_frame(struct dv_window *, const void *, size_t,
		    int *ended);
void		 dv_screen_linefeed(struct dv_screen *);
uint32_t	 dv_placeholder_diacritic(unsigned n);
int		 dv_screen_scroll(struct dv_screen *, int delta);
struct dv_cell	*dv_screen_disp_at(struct dv_screen *, unsigned x, unsigned y);
struct dv_stext	*dv_stext_lookup(struct dv_session *, unsigned id,
		    struct dv_window **);
size_t		 dv_meta_val(const char *m, size_t n, const char *key, char *out,
		    size_t cap);
void		 dv_window_apply_pointer(struct dv_window *);
void		 dv_window_apply_cursor_color(struct dv_window *);
int		 dv_placeholder_index(uint32_t cp);
int		 dv_wcwidth(uint32_t);

void		 dv_compose_cells(struct dv_session *, struct dv_cell *desk);
void		 dv_compose_cursor(struct dv_session *, int *cx, int *cy,
		    int *vis);
size_t		 dv_compose_frame(struct dv_session *,
		    const struct dv_cell *desk, struct dv_view *,
		    unsigned char **out);
void		 dv_view_free(struct dv_view *);
void		 dv_flat_free(struct dv_session *);

struct dv_menu_item {
	const char	*key;
	const char	*label;
};
#define DV_MENU_W	44
const struct dv_menu_item *dv_menu_items(unsigned *n);
unsigned	 dv_menu_height(void);
int		 dv_menu_box_fits(unsigned cols, unsigned rows);
void		 dv_menu_paint(struct dv_cell *desk, unsigned cols,
		    unsigned rows);
void		 dv_help_paint(struct dv_cell *desk, unsigned cols, unsigned rows);
void		 dv_confirm_paint(struct dv_cell *desk, unsigned cols,
		    unsigned rows, const char *msg);

/* Unicode picker overlay: Alt then U. */
void		 dv_uni_paint(struct dv_cell *desk, unsigned cols,
		    unsigned rows, int cat, int sel);
int		 dv_uni_count(int cat);
int		 dv_uni_cats(void);
uint32_t	 dv_uni_char(int cat, int sel);
void		 dv_uni_box(unsigned cols, unsigned rows, int *bx, int *by,
		    int *gx, int *gy);
void		 dv_uni_grid(unsigned cols, unsigned rows, int *gcols,
		    int *grows);
int		 dv_uni_click(unsigned cols, unsigned rows, int cat,
		    int cursel, int mx, int my, int *newcat, int *newsel);

int		 dv_sel_contains(const struct dv_session *, int sx, int sy);

/* Input methods (src/im.cc). */
int		 dv_im_key(struct dv_im *, int ch, struct dv_buf *commit);
void		 dv_im_reset(struct dv_im *);
const char	*dv_im_name(int method);

struct dv_window *dv_window_open(struct dv_session *, const char *cmd,
		    char *const *cmd_argv, int snap, const char *cwd);
struct dv_window *dv_window_split(struct dv_window *, int below,
		    char *const *cmd_argv, const char *cwd);
void		 dv_window_close(struct dv_window *);
void		 dv_window_raise(struct dv_window *);
extern int (*dv_agent_app_hook)(struct dv_window *, const char *, size_t);
void		 dv_window_focus(struct dv_window *);
void		 dv_window_report_focus(struct dv_window *, bool force = false);
struct dv_window *dv_window_find(struct dv_session *, unsigned id);
void		 dv_window_default_geom(struct dv_desktop *, struct dv_geom *);
void		 dv_window_banner(struct dv_window *);
void		 dv_window_set_pty_size(struct dv_window *);
void		 dv_window_resize_to(struct dv_window *, unsigned cols,
		    unsigned rows);

int		 dv_pty_spawn(struct dv_window *, const char *cmd,
		    char *const *cmd_argv);
void		 dv_pty_resize(struct dv_window *);
void		 dv_pty_close(struct dv_window *);
void		 dv_pty_kill(struct dv_window *);

struct dv_session *dv_session_create(const char *name, struct dv_size);
struct dv_session *dv_session_find(const char *name);
struct dv_session *dv_session_default(void);
void		 dv_session_destroy(struct dv_session *);

int		 dv_server_main(const char *sock_path, int debug);

int		 dv_tty_size(struct dv_size *);
int		 dv_client_run(const char *sock_path, int display,
		    struct dv_size, const char *term,
		    char **cmdv, bool local_agent = false);

void		 dv_host_init(struct dv_host *);
void		 dv_host_bootstrap(struct dv_host *, int in_fd, int out_fd,
		    struct dv_size *);
void		 dv_host_disable(int fd);
void		 dv_host_feed(struct dv_host *, const void *, size_t);
int		 dv_host_next(struct dv_host *, struct dv_host_ev *);
int		 dv_host_pending(const struct dv_host *);
int		 dv_host_expire(struct dv_host *, struct dv_host_ev *);
int		 dv_host_features(const struct dv_host *);
int		 dv_term_is(const char *name, const char *what);

/* Drag and drop (OSC 72) from a program: the server relays and routes. */
extern void	(*dv_dnd_app_hook)(struct dv_window *, const char *, size_t);
int		 dv_xcur_at(const struct dv_screen *, unsigned col, unsigned row);

/* src/config.cc: ~/.config/odin-seat/config. */
#define DV_MENU_KEY_ALT			0x1
#define DV_MENU_KEY_CTRL_BACKSLASH	0x2
struct dv_config {
	char		shell[DV_PATH_MAX];	/* program for a new window ("" = $SHELL) */
	int		scrollback;		/* lines kept per window */
	char		download_dir[DV_PATH_MAX];
	char		kitty_medium[16];
	char		term[64];
	uint32_t	backdrop;		/* the desktop's stipple glyph */
	int		click_raises;		/* a click raises the window it focuses */
	int		cell_pictures;		/* draw pictures as coloured cells where a terminal shows none */
	int		mono;			/* strict black and white (client) */
	int		menu_key;		/* DV_MENU_KEY_* mask (client) */
	size_t		csi_limit;		/* bytes of one CSI; 0 means no limit */
	size_t		escape_limit;		/* bytes of one OSC/APC/DCS; 0 means no limit */
};
/* The limits in force. 0 means no limit. Set from the config, then the environment. */
inline size_t		 dv_csi_limit = 0;
inline size_t		 dv_escape_limit = 0;
void		 dv_limits_apply(const struct dv_config *);
void		 dv_config_defaults(struct dv_config *);
const char	*dv_config_path(void);
int		 dv_config_load(struct dv_config *, const char *, const char *,
		    void (*)(const char *, int, const char *));
extern struct dv_config	 dv_cfg;		/* the server's settings */
extern int		 dv_scrollback_lines;	/* term.cc: new screens' scrollback */
extern uint32_t		 dv_backdrop_glyph;	/* compose.cc */
void		 dv_host_set_menu_keys(int);
/* The windows the switch list shows for the session's filter, in order;
 * returns how many ids were written (at most max). */
std::vector<unsigned> dv_switch_list(const struct dv_session *);
void		 dv_switch_paint(struct dv_cell *, unsigned, unsigned, const struct dv_session *);
/* A scrollback line, 0 the oldest; null past the end. *width is its length. */
const struct dv_cell *dv_screen_history_row(struct dv_screen *, unsigned, unsigned *);
void		 dv_buf_put_utf8(struct dv_buf *, uint32_t);
/* One row of cells as text, trailing blanks trimmed; ids and fillers as spaces. */
void		 dv_row_text(struct dv_buf *, const struct dv_cell *, unsigned);
void		 dv_window_focus_noraise(struct dv_window *);

/* src/state.cc: the running server's state handed to a new binary. */
int		 dv_state_write(const char *, const struct dv_peer *requester);
int		 dv_state_read(const char *, int *requester_fd);
void		 dv_state_inherit_fds(int on);
int		 dv_server_takeover(const char *, const char *, int);
struct dv_window *dv_window_adopt(struct dv_session *, unsigned, int, pid_t,
		    unsigned, unsigned);
void		 dv_screen_sb_push(struct dv_screen *, const struct dv_cell *, unsigned);
void		 dv_cells_encode(struct dv_buf *, const struct dv_cell *, unsigned);
unsigned	 dv_cells_decode(const char *, struct dv_cell *, unsigned);

/* src/saved.cc: sessions written to a file and recreated from it. */
const char	*dv_saved_path(void);
int		 dv_sessions_save(const char *, struct dv_buf *report);
int		 dv_sessions_restore(const char *, struct dv_buf *report);

/* src/im.cc: where the input-method dictionaries live, and our own path. */
const char	*dv_im_datadir(void);
char		*dv_exe_path(void); /* allocated canonical path; caller frees */

/* src/kbd.cc: the kitty keyboard protocol per window. */
size_t		 dv_kbd_encode(const struct dv_key *, int flags, unsigned char *,
		    size_t);
size_t		 dv_key_pack(const struct dv_key *, unsigned char *, size_t);
int		 dv_key_unpack(const unsigned char *, size_t, struct dv_key *);
int		 dv_screen_kbd_flags(const struct dv_screen *);

/* Bytes for the program in a window, queued while its pty is full. */
void dv_pointer_send(struct dv_window *, const goblin::pointer::Message &);
void dv_pointer_cancel(struct dv_window *);
void		 dv_window_pty_send(struct dv_window *, const void *, size_t);
void		 dv_window_pty_flush(struct dv_window *);
void		 dv_route_expect(struct dv_window *, unsigned, const char *, size_t);
void		 dv_prompt_paint(struct dv_cell *, unsigned, unsigned,
		    const char *const *, unsigned);

/* src/xfer.cc: file transfer (kitty OSC 5113, goblin-download OSC 777),
 * forwarded to a terminal that speaks it or handled here. */
void		 dv_xfer_osc(struct dv_window *, unsigned, const char *, size_t);
void		 dv_xfer_answer(struct dv_session *, int);
void		 dv_xfer_pump(struct dv_window *);
void		 dv_xfer_free(struct dv_window *);
int		 dv_xfer_active(const struct dv_window *);
long		 dv_b64_decode(const char *, size_t, unsigned char *, size_t);
extern int	(*dv_xfer_forward_hook)(struct dv_window *, const char *, unsigned,
		    const char *, size_t);
