# Silkie feature review

Reviewed the local `~/dev/silkie` integration branch `silkie` at
`7158e42b3`, including the working tree's published extension specifications
and `docs/silkie/window-actions.rst`. No Silkie or kitty implementation code
was imported; see [the clean-room rules](CLEANROOM.md).

The extension review found existing Odin Seat implementations of the OSC 777
selectors, HDR brightness and image format, wide-gamut colors, cell background
alpha, GPU-surface transport, font roles, vector services and terminal-side ink.
Their existing capability negotiation and host restrictions still apply.

The newer window-action documentation describes splitting the active pane
and killing an unresponsive window's foreground job and shell. Odin Seat now
provides `split-window` (right or below) and `kill-window -f`, with menu actions
`E` then `R`/`B`, and `K` with confirmation. Splitting divides only the chosen
window; its neighbors are not relaid out. Both halves remain overlapping
windows rather than members of a persistent tiling tree.

The related window-id, compositor, switch-list and poll-descriptor storage
now grows with the number of windows and clients. It does not stop at the
former 255/256-entry boundaries. Integer representation and actual OS
allocation, process, PTY and descriptor constraints still apply.

Validation lives in `src/test-desktop.cc`, `src/test-menu.cc` and
`regress/windows.py`: split geometry and tiny-screen refusal, repeated and
zoomed splits, confirmation ownership, real foreground job-control groups,
400 synthetic windows, and more than 256 connected clients.

## Stereo depth and packed pixels (Silkie 0.49.2-silkie20261116.2)

Re-reviewed `~/dev/silkie` at `a53bfeef8` against the published
`docs/spatial-protocol.rst`, `docs/silkie/packed-image-protocol.rst`,
`docs/silkie/pointer-image-protocol.rst` and `docs/capabilities-protocol.rst`.
Again, no Silkie code was read or imported.

Two gaps were found and closed:

- **`depth`** (spatial display). Odin Seat now speaks `depth.1`: `op=set`
  with `d=` (signed 32-bit millidegrees) or `p=` (finite 32-bit float
  reference pixels) and `y=` (vertical phase), validated as a whole, and
  `op=reset`. Depth and phase are interned per distinct value
  (`dv_depth_intern`, like font styles), so each cell holds one id. They are
  kept through scrollback, reflow, DECSC/DECRC, DECCARA and a live upgrade
  (`sdepth`, `dstyle` and a fifteenth cell field), and reset by SGR 0,
  `op=reset`, RIS and OSC 133 `A`; a caps reset, which is for modes, leaves
  them. The compositor sends depth only to terminals that listed `depth.1`,
  re-sends it after every SGR 0, and leaves the terminal on its plane at the
  end of each frame. Kitty placements record the pen's depth when made and
  are transmitted with an explicit depth before `a=p`, and a reset after it.
  Vector `depth=`, `depth_px=` and `phase=` keys already passed through in
  the stored records. Spatial presentation itself (side-by-side,
  red/blue, XREAL tracking) is the terminal's job and needs nothing here.
- **Packed image formats** `f=1`, `2`, `4`, `8` (gray) and `f=16` (RGB565)
  were refused with `EINVAL`, although a Silkie terminal's `format` list
  advertised them through `caps`. They are now always accepted (RGB565
  with `G=` only when the terminals take wide gamut) and always offered,
  passed packed to terminals whose own list has them, expanded to `f=32`
  for other kitty terminals, and decoded for sixel.

Image pointers (OSC 22 `image:`) were already supported.

Validation: `test_depth` and `test_packed` in `src/test-silkie.cc`,
`test_depth_paint` in `src/test-menu.cc`, and the depth and packed-image
checks in `regress/silkie.py`.

## Focus depth and per-display packing

`--3d-focus=X` (a client option, carried in the identify message) shows the
focused window, its frame and its images X reference pixels nearer on that
client when it listed `depth.1`. The compositor marks each desk cell
painted from the focused window (`dv_cell.focus`), compares that mark
only for such clients, shifts depths per client (`dv_depth_nearer`; a
millidegree depth moves at Silkie's default 52-degree field of view, since
nothing reports the real one), forces a full repaint when focus moves, and
places images again at their new depth without their pixels
(`dv_kitty_replace`).

Images are re-packed per client (`image_emit_for`, cached per image
transmission and display profile): reduced to the display's gray levels or
channel widths (`display` reply `color`, `bits` and the newer
`red_bits`/`green_bits`/`blue_bits`/`gray_bits`, or `-m`), then the
narrowest exact format the client listed, deflated when smaller, or the
program's own form when unreduced and no bigger. The display is queried at
attach and on every client resize; a display event updates that client's
profile, and a changed profile resends its images. `display_body` now
relays the channel widths and `precision` to programs.

Validation: `test_repack` and the `--3d-focus` cases in `src/test-silkie.cc`,
`test_focus_depth` in `src/test-menu.cc`, and `regress/silkie.py` (a 4-bit
gray terminal, and a second terminal attached with `--3d-focus=4`).

## Silkie catch-up (3557a0425)

Re-reviewed `~/dev/silkie` at `3557a0425` against what its published
documents gained since `a53bfeef8`: `docs/misc-protocol.rst` (the Program
Status Protocol), `docs/desktop-notifications.rst` and
`docs/silkie/notification-routing.rst` (ConEmu's OSC 9), `docs/deep-color-protocol.rst`,
`docs/capabilities-protocol.rst`, `docs/display-protocol.rst`,
`docs/color-stack.rst`, the graphics, packed, palette and precision image
documents and `docs/silkie/image-tiling.rst`. The Program Status Protocol
itself was implemented from its own specification, revision 0.4, at
superlogical.com. No Silkie or kitty code was read for any of it.

### Program status (OSC 7501)

`src/pstatus.cc`. Each window keeps one record per id, replaced whole by
its next report; malformed pairs are skipped and the rest applies; an
unknown state, an id outside the grammar, text that is not base64 or that
decodes to invalid UTF-8 or a control character refuses the report whole.
As Silkie does, none of the Limits section's caps apply: reports, fields,
id depth and the number of records are bounded only by memory. Lifetime:
working, blocked and idle end at OSC 133;A (and ConEmu's 9;12); done and
error stay until the window gains focus or is typed in (a record that
arrives in the focused window stays until a key); RIS removes all and
re-enables the OSC 9;4 mapping. `OSC 7501 ; ?` is answered with the states
and kinds.

Which record to show when a window has several: the most urgent, blocked
before error before working before done (idle is not shown), the newest
among equals, with the count of records in that state. The frame shows it
after the title and the record's text (title or inherited app, then msg,
with bidi and invisible formatting characters left out) along the bottom
border.

Because the protocol holds any number of records per terminal in a
hierarchy, they are exported as well: the client probes `OSC 7501 ; ?`
at bootstrap, and a terminal that answers is sent each window's records
under `w<window id>/<id>`, with its root record standing for the most
urgent record of the session (titled `id:window title`). Attaching clears
the terminal's records first and sends them all; a cleared or dropped
record is cleared there (records beneath one are sent again where the
clear took them); closing a window clears `w<id>`; detaching clears
everything. Records survive a live upgrade (`pstatus` state lines).

### OSC 9 (ConEmu) and OSC 7

ConEmu's sub-commands are recognised exactly as Silkie documents (a number
1-12 followed by `;` or the end; anything else, `13;text` included, is an
iTerm2 notification). 2 is a notification of its unquoted text; 3 sets the
title, empty restoring the command name; 4 is progress, mapped to the
window's root status record until a real 7501 report (never a
notification, which it used to become); 9 records the working directory;
12 marks a prompt; 1, 5-8, 10 and 11 are ignored. OSC 7 (`file://host/path`,
percent-decoded) now also records the directory, which split windows start
in. OSC 0/2 and 9;3 titles are cut on a character boundary.

### Deep colours, `sgr`, display output, colour stack

SGR `38:0:id:r:g:b`, `48:0` and `58:0` (a gamut id, sixteen-bit sRGB-encoded
components): a cell keeps the nearest eight-bit colour in its colour word
and, in a new word per colour, each channel's difference from it (-128..128
in nine bits, plus the underline's gamut), so the sixteen bits are exact
without a table to collect. Malformed groups (too few or empty fields, a
component above 65535) change nothing, and the parameters after them apply.
Any ordinary colour of the same kind, SGR 0 and RIS clear them; DECSC/DECRC,
DECCARA, scrollback, reflow and the upgrade state keep them (three optional
cell-codec fields, `pendeep`, `csdeep`). The compositor sends a terminal
that lists `deep` in its `sgr` field the eight-bit colour, then the deep
form in a sequence of its own; any other gets the eight-bit colour alone,
and a colour brought down to sRGB loses its deep word.

The caps reply lists `sgr=alpha,deep,gamut,hdr`: odin-seat keeps all four
per cell and brings them down for terminals without them. The terminals'
own `sgr` lists are kept in their capability blobs. The display reply
relays `output_bits`, `output_type` and `output_dither`, `output_bits`
taking the widest of the attached terminals like the channel widths.

The colour stack (OSC 30001/30101) grows with the pushes instead of
silently ignoring a fifth.

### Images

Read: `docs/graphics-protocol.rst`, `docs/silkie/packed-image-protocol.rst`,
`docs/silkie/palette-image-protocol.rst`,
`docs/silkie/precision-image-protocol.rst`, `docs/silkie/image-tiling.rst`,
and the format list of `docs/capabilities-protocol.rst`.

- **Format numbers.** Silkie moved its packed formats out of kitty's way:
  gray is now `f=301`, `302`, `304`, `308` and RGB565 `f=516`. odin-seat
  takes, offers, sends and reads them by the new numbers, and refuses the
  old `f=1`, `2`, `4`. A terminal's `format=` list is read with both older
  numberings in mind (`dv_kitty_fmt_list`): a list with 1, 2 or 4 is from
  before 2026-11-16, and its 8 and 16 are gray and RGB565; a list with 316
  but no 516, 632 or 664 is from the weeks when 316 was RGB565. Neither
  number is taken for what it is now.
- **Kitty's indexed colour** (`f=8`, `f=16`): indices into the window's
  colours. They are decoded with the window's palette (its OSC 4/10/11/12
  overrides over the default theme odin-seat answers colour queries
  with; 250-254, a selection, cursor and URL hover colour odin-seat does not
  draw in a program's colours, are the foreground or background as the
  protocol says for a colour taken from the cell). A terminal that lists
  the format is sent the indices, so the picture follows its theme, unless
  the window has colours of its own; then it is sent the colours. A change
  of the window's colours is noticed before each frame (`pal_stamp`) and
  sends every placement of such an image again.
- **Palette images** (`f=401`-`408`) and their `a=e` palette updates
  (`x`, `w`, `G` that must match, any medium, chunked, `o=z`). `a=e` goes to
  the palette for a palette image, to the records for a vector image, and
  is `EINVAL` otherwise. The sizes are exact (`ENODATA`, `EFBIG` for direct
  data, `EINVAL` for compressed data of another size, `EOVERFLOW`). A
  palette image is kept decoded so that an update patches it in place. A
  terminal that lists the format is sent the image as it is; the others
  its colours. An update is sent as the image again, its indices and
  palette (or colours), rather than relayed as an `a=e`: the clients that
  hold it at the old generation are found the same way as for any
  retransmission. Pictures of few colours go as palette images to a
  terminal that lists them when that is the smallest exact form.
- **High precision** (`f=316`, `632`, `664`) and the **transfer function**
  `E` (sRGB, linear, PQ, HLG; refused for the formats the specification
  names, and other than `E=8` for `f=64`). A terminal that lists a high
  precision format is taken to read `E` (both are one specification). It
  is sent the image as it is when its display shows all of it. A terminal
  that takes `f=64` is sent half floats: the curve undone (PQ with 203 nits
  at SDR white, HLG through the BT.2100 OOTF for a 1000 nit display), the
  gamut moved to linear extended sRGB without clipping. Any other gets
  eight bits of sRGB, brighter than SDR white clipped. `dv_kimage_rgba`
  decodes every format, so sixel and cell pictures show them too.
- **Animation frames and composition** of indexed, palette, high precision
  or non-sRGB images are refused with `EINVAL`, as the specifications ask.
  odin-seat still composes no frames of other images.
- **Feature queries** (`a=q` with `e=`): answered with `e` and every key of
  the query, with its value, that odin-seat takes for that action (`t`,
  `T`, `q`, `p`, `d`, `e`, and `v` when a terminal answers the vector
  service). An action it does not take (`f`, `a`, `c`, `r`) is answered
  without `e`. What depends on the terminals (a gamut, `t=g`) is asked of
  `dv_silkie_gfx_ok` as a transmission would be. Nothing is done: a chunked
  transmission under way is left as it was.
- **`S` and `O` are 64-bit**, and any other key larger than 32 bits makes
  the command malformed, unanswered (`cmd_malformed`). An end past a file
  offset is `EOVERFLOW` before anything is opened; only the pages that
  hold the data are mapped. A shared memory name is passed to `shm_open()`
  as it came, with or without a leading slash.
- **No size limits.** The PNG decoder's 8192 pixel cap and the 100000
  cell / 4 million pixel cap on a placement drawn as sixel are gone; sizes
  are bounded by overflow checks. `inflate_grow` feeds zlib in 32-bit
  pieces, so a stream past 4 GiB is no longer cut short. `dv_kimage.fmt`
  stays a fixed buffer: its keys are bounded by the protocol (57 bytes at
  most) and a longer one is a fatal error, not a truncation.

Validation: `test_packed`, `test_repack`, `test_indexed`, `test_palette`,
`test_precision`, `test_feature_query` and `test_medium64` in
`src/test-silkie.cc`, and the palette image, indexed colour, palette update
and feature query checks in `regress/silkie.py`.

### Not adopted

The DnD change is a clarification only. The custom shader, launch wrapper,
layout, session and theme changes are Silkie's own window management and
UI, not protocols a program speaks. The vector rasterizer copy is already
at `50487665`, the newest commit to `kitty/vector.c`.

Validation: `test_pstatus` and the deep colour and colour stack cases in
`src/test-term.cc`; `test_deep_paint` in `src/test-menu.cc`; the `sgr`,
display output and probe cases in `src/test-silkie.cc` and
`src/test-host.cc`; and `regress/pstatus.py` (export, hierarchy, root
record, prompt, frame badges, ConEmu OSC 9 and upgrade, through a real
client and server).
