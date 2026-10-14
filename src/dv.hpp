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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

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
 */
#define DV_PROTOCOL 5 /* pointer capability/capture state and tiled graphics IDs */

#define DV_MIN_INNER_COLS 2
#define DV_MIN_INNER_ROWS 1
#define DV_DEFAULT_XPIXEL 16
#define DV_DEFAULT_YPIXEL 32
#define DV_MAX_MSG (1024 * 1024)
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
	DV_MSG_KEY,		/* client -> server: a structured key (dv_key) */
	DV_MSG_HELLO		/* server -> client: protocol + version, after IDENTIFY */
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
	DV_HEV_OSC		/* a host OSC reply, bytes in dv_host.osc */
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
};

struct dv_host {
	unsigned char		 buf[8192];
	size_t			 len;
	struct dv_host_ev	 pend[32];	/* keys typed during bootstrap */
	unsigned		 npend;
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
	char			 silkie_blob[2048];
	int			 dnd;		/* answered the OSC 72 query */
	int			 mcursors;	/* answered the multiple-cursors query */
	int			 have_cell;
	unsigned		 xpixel;
	unsigned		 ypixel;
	unsigned		 cols;
	unsigned		 rows;
};


/*
 * A window is its own terminal, hence its own kitty endpoint: it stores
 * the image the inner program transmits and answers the program's
 * queries. goblin-view then acts as a kitty CLIENT of the host, re-transmitting
 * the stored pixels under a remapped, terminal-global id and placing
 * them where the window sits. Child bytes never reach the host.
 */
#define DV_KITTY_FIRST_ID	0x10000000u	/* our id space (see tmux) */

/* The display keys of an a=T / a=p: columns, rows, source rectangle,
 * z-index, cursor stays (C), placement id, virtual (U). */
struct dv_kreq {
	long	c, r, x, y, w, h, z, C, p, U;
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
	char		 fmt[96];	/* replayable keys: f=,s=,v=,o=z,G= */
	int		 gpu;		/* t=g: payload is a surface token, not pixels */
	int		 reassembling;	/* mid chunked transmit */
	int		 done;		/* complete */
	unsigned	 wpx;		/* pixel size (0 = unknown) */
	unsigned	 hpx;
	unsigned	 virtual_id;	/* host ID of the U=1 placement, if any */
	int		 want_place;	/* a=T: display once the last chunk lands */
	struct dv_kreq	 req;		/* with these keys */
};

/*
 * A placement: kitty's unit of display. A direct one has an origin in
 * screen cells (the row goes negative as it scrolls off the top) and
 * floats above the text like it does in kitty -- redraws beneath leave
 * it, scrolling carries it, ED/RIS/leaving the alternate screen clear it.
 * A virtual one (U=1) is positioned by placeholder cells the program
 * writes itself. Each placement is its own image on the host, so a crop
 * or a scale of the same pixels is simply another transmit.
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
	int		 z;
	int		 virt;		/* U=1 */
	int		 alt;		/* made on the alternate screen */
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
#define DV_STEXTS		32		/* scaled-text runs stored per screen */
#define DV_OSC_ROUTES		32		/* pending host-reply routes per session */

struct dv_stext {
	int		 used;
	unsigned	 gen;		/* arrival order, for eviction */
	uint8_t		 s;		/* scale, 1-7 */
	uint8_t		 wparam;	/* the w= metadata (0 = auto width) */
	uint16_t	 bw;		/* base width in cells */
	uint16_t	 cols;		/* footprint width  = bw * s */
	uint16_t	 rows;		/* footprint height = s */
	uint32_t	 fg;		/* the run's colours, at OSC-66 time */
	uint32_t	 bg;
	uint8_t		 attr;
	uint16_t	 len;		/* text length in bytes */
	char		*text;		/* UTF-8 (malloc) */
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
	int		 used;
	int		 device;
	int		 kind;
	char		 fields[700];
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
	int		 bracketed;	/* DECSET 2004: app wraps pastes in ESC[200~/201~ */
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
	/* Per-pane Silkie modes. The host is the union, pushed when it changes. */
	int		 silk_tablet, silk_scroll, silk_gesture;
	int		 silk_frame, silk_latency, silk_display, silk_ink;
	unsigned	 silk_frame_seq;
	int		 silk_frame_dirty;
	char		 silk_brush[160];
	char		 silk_shown[400];
	int		 sace;		/* DECSACE: 0/1 stream, 2 rectangle (for DECCARA) */
	/*
	 * Colour overrides this window's program set (OSC 4/10/11/12/21):
	 * they apply to this window only -- the compositor remaps its cells
	 * -- so one program cannot recolour the others. 30001/30101 push
	 * and pop the whole set.
	 */
	struct dv_palette pal;
	struct dv_palette palstack[4];
	int		 npal;
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

	struct dv_stext	  stext[DV_STEXTS];	/* OSC 66 scaled-text runs */
	unsigned	  stext_gen;

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
	int		 gfx;		/* this client's host takes kitty graphics */
	int		 sixel;		/* this client's host renders sixel */
	int		 mcursors;	/* this client's host draws extra cursors */
	int		 mono;		/* strict black and white: ink, paper, reverse */
	char		*xcur_last;	/* the extra-cursor set last sent to it */
	/* Images (by id+gen) transmitted to this client as virtual placements. */
	std::unordered_map<unsigned, unsigned> *kimg_gen;
};

#define DV_IN_NORMAL		0
#define DV_IN_REARRANGE		1
#define DV_IN_MOVE		2
#define DV_IN_RESIZE		3
#define DV_IN_POSITION		4

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

struct dv_window {
	unsigned		 id;
	char			 title[64];
	char			 cmd[128];
	char			 cwd[DV_PATH_MAX];
	int			 z;
	struct dv_geom		 geom;
	struct dv_geom		 saved;
	int			 framed;
	int			 zoomed;
	int			 title_locked;	/* user named it; ignore app OSC title */
	int			 hidden;
	int			 aside;
	int			 frozen;
	int			 locked;
	unsigned		 lcols;
	unsigned		 lrows;
	unsigned		 ox;
	unsigned		 oy;
	int			 pty_fd;
	pid_t			 pid;
	unsigned pointer_epoch;
	int npointer;
	goblin::pointer::Message pointer_active[32];
	struct dv_silk_live	 silk_live[16];
	unsigned		 silk_epoch;
	struct dv_buf		 cap;		/* the OSC/APC string being read */
	std::unordered_map<uint64_t, dv_kimage> *kimg;
	uint64_t		 kreassem;	/* image key mid-transmit, 0 none */
	uint64_t		 kanon;		/* anonymous keys above UINT32_MAX */
	struct dv_buf		 host_out;	/* OSC-52 etc. bound for the host */
	int			 clip_query;	/* host_out holds an OSC-52 read */
	/* OSC 22 mouse pointer shapes: this window's own stack; the top is
	 * applied to the host while the window is focused. */
	char			 pshape[16][32];
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
	char			 name[64];
	struct dv_desktop	 desktop;
	int			 input_mode;
	int			 menu_open;
	int			 quit_confirm;
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
	int			 switch_sel;	/* index into the filtered list */
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
	char			 silk_ext[768];
	char			 silk_media[80];
	char			 silk_format[80];
	char			 silk_display[384];
	int			 silk_frame_caps, silk_frame_period;
	int			 silk_gesture_caps, silk_haptic_caps, silk_ink_caps;
	int			 silk_latency_caps, silk_latency_input, silk_latency_repaint;
	int			 silk_scroll_caps, silk_tablet_caps;
	int			 silk_any;
	int			 silk_wg;	/* every graphics peer listed Silkie formats */
	int			 silk_sent_tablet, silk_sent_scroll, silk_sent_gesture;
	int			 silk_sent_frame, silk_sent_latency, silk_sent_display;
	int			 silk_sent_ink;
	char			 silk_sent_brush[200];
	int			 silk_pad_dev, silk_pad_n;
	int			 silk_pad_btn[16];
	unsigned		 silk_pad_win;
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
	int			 fd;
	struct dv_conn		 conn;
	int			 identified;
	int			 pointer_caps;
	dv_pointer_capture pointer_contacts[32];
    int pointer_device_count, pointer_host_device[64], pointer_app_device[64];
	int			 display;
	struct dv_size		 tty;
	char			 term[64];
	char			 cwd[DV_PATH_MAX];
	int			 features;
	char			 host[64];	/* the terminal's XTVERSION name, "" unknown */
	struct dv_session	*session;
	struct dv_view		 view;
	char			 silkie_blob[2048];
	struct dv_silk_hit	 silk_hit[32];
	struct dv_silk_desc	 silk_desc[8];
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
extern int		 dv_mono;	/* client: -m / $GOBLIN_VIEW_MONO, black and white */
uint32_t	 dv_index_rgb(unsigned);	/* the default RGB of a palette index */

void		*xmalloc(size_t);
void		*xcalloc(size_t, size_t);
void		*xrealloc(void *, size_t);
char		*xstrdup(const char *);
char		*xasprintf(const char *, ...)
		    __attribute__((format(printf, 1, 2)));
size_t		 dv_strlcpy(char *, const char *, size_t);

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
void		 dv_conn_init(struct dv_conn *, int);
void		 dv_conn_free(struct dv_conn *);

char		*dv_socket_path(const char *label, const char *path_override);
int		 dv_socket_listen(const char *);
int		 dv_socket_connect(const char *);
int		 dv_socket_start_server(const char *, int debug, char **argv0);

unsigned	 dv_frame_cols(const struct dv_geom *, int framed);
unsigned	 dv_frame_rows(const struct dv_geom *, int framed);
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
void		 dv_kitty_transmit(struct dv_window *, const struct dv_kplace *,
		    struct dv_buf *out);
void		 dv_kitty_delete(unsigned dv_id, struct dv_buf *out);
struct dv_kplace *dv_kitty_lookup(struct dv_session *, unsigned dv_id,
		    struct dv_window **);
unsigned	 dv_kitty_virtual_id(struct dv_window *, unsigned child_id);
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

void		 dv_screen_init(struct dv_screen *, unsigned cols,
		    unsigned rows);
void		 dv_screen_free(struct dv_screen *);
void		 dv_screen_resize(struct dv_screen *, unsigned cols,
		    unsigned rows);
struct dv_cell	*dv_screen_at(struct dv_screen *, unsigned x, unsigned y);
void		 dv_vt_write(struct dv_window *, const void *, size_t);
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
void		 dv_window_close(struct dv_window *);
void		 dv_window_raise(struct dv_window *);
void		 dv_window_focus(struct dv_window *);
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

struct dv_session *dv_session_create(const char *name, struct dv_size);
struct dv_session *dv_session_find(const char *name);
struct dv_session *dv_session_default(void);
void		 dv_session_destroy(struct dv_session *);

int		 dv_server_main(const char *sock_path, int debug);

int		 dv_tty_size(struct dv_size *);
int		 dv_client_run(const char *sock_path, int display,
		    struct dv_size, const char *term,
		    char **cmdv);

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

/* src/config.cc: ~/.config/goblin-view/config. */
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
unsigned	 dv_switch_list(const struct dv_session *, unsigned *ids, unsigned max);
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
int		 dv_exe_path(char *, size_t);

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
