# Ghostty is the host

GoblinView (the `goblin-view` command) runs best **inside Ghostty**: that
is the environment the input and graphics paths are written for. It runs
on any terminal, but the graphics features need a host that speaks kitty
graphics (see **Fallback**).

Ghostty speaks the protocols we need without a lowest-common-denominator
layer:

| need | Ghostty feature |
| --- | --- |
| menu key = isolated Alt tap | kitty keyboard protocol, flags 11 (disambiguate + events + report-all). Ghostty only emits modifier press/release when report-all is on. |
| live cell pixels | XTWINOPS `CSI 16 t` (reply `CSI 6 ; ph ; pw t`) and in-band resize mode 2048 (`CSI 48 ; rows ; cols ; height ; width t`) |
| program resize | a window's program may resize it with XTWINOPS `CSI 8 ; rows ; cols t`; goblin-view caps it to the desktop and shifts it on-screen |
| kitty graphics | APC `ESC _ G … ST`, including the query tmux uses (`a=q,t=d,f=24`). A program's sixel is decoded and re-emitted over this too. |
| text sizing | OSC 66 (`ESC ] 66 ; s=..:w=.. ; text ST`). A window lays the scaled run into its grid and re-emits OSC 66 to the host. |
| truecolor / cursor / sync | `xterm-ghostty`, `COLORTERM=truecolor` |

On attach the client pushes those modes onto the host tty and pops them
on detach (`CSI < u`, `CSI ? 2048 l`).

Child PTYs get `TERM=xterm-ghostty` (override with `GOBLIN_VIEW_TERM`). Inner
keyboard mode will later follow each window via libghostty-vt; today
CSI u is decoded and re-encoded to legacy bytes for the PTY.

## libghostty-vt

The per-window emulator (grid, scrollback, kitty image *storage*) will
be libghostty-vt, same library goblin-term pins. That is a separate
build from the host-side protocol in `src/host.c`. Multiplexer concerns
that libghostty-vt does not own — global image-id remap, z-order clip
onto the host Ghostty — still come from `~/dev/tmux` via `src/tmuxport/`
(see `docs/tmux-tracking.md`).

## Asks (things Ghostty could add or guarantee)

We can ship without these, but they would save real work or make Alt-tap
reliable on macOS.

1. **Bare Option tap under kitty keyboard flags 11.** The menu key is
   press-Alt/release-Alt with no other key. Ghostty only emits modifier
   events when `report_all` is on (we push flags 11). Please guarantee
   that an isolated Option tap still produces `LEFT_ALT`/`RIGHT_ALT`
   press then release **even when `macos-option-as-alt` is false**
   (Option still composing for Option+letter). If a tap is currently
   swallowed unless option-as-alt is on, that is the number-one host
   change we need. A dedicated `macos-option-as-alt = "tap"` that
   reports the modifier without stealing Option+key unicode would be
   ideal.

2. **libghostty-vt: re-encode kitty placements as APC.** Inner windows
   will use libghostty-vt (pixels + placement metadata). The host
   Ghostty still needs `ESC _ G` with remapped ids and cropped
   `x,y,w,h`. Today that means we re-compress pixels. A C API to emit
   transmit + place (caller supplies image id, placement id, source
   rect, C=1) would be the highest-leverage library addition.

3. **Unicode placeholders (`U=1`) on the libghostty-vt grid.** If
   virtual placements stay as `U+10EEEE` cells we can compositor-clip
   by only painting uncovered placeholders. If they are stripped to
   empty cells, we have to rebuild them.

4. **Default PNG decoder in libghostty-vt.** The C API requires
   `GHOSTTY_SYS_OPT_DECODE_PNG` before images store. Shipping a
   built-in decoder (or a "use stb/libpng" compile option) means we
   do not have to wire one.

Not asking: in-band resize 2048, XTWINOPS 16/18/14, keyboard push/pop,
graphics query replies, synchronized output 2026. Ghostty already has
those; we turn them on from the client.

## Fallback

Ctrl-\ still opens the menu so `make test` and non-Ghostty attaches can drive
the menu path. They will not get Alt-tap or cell-pixel reports.

**Graphics adapt to the host; text-only is the floor.** At attach the
client asks the terminal a kitty graphics query with DA1 behind it: a
kitty reply means kitty graphics, a `4` in the DA1 parameters means
sixel, and DA1 ends the wait either way. Both bits ride in the identify
message. The server keeps a per-session flag: graphics are live when any
attached client can show an image by *either* protocol, and then windows
advertise both kitty graphics (answer `a=q`) and sixel (the `4` in DA1,
plus `XTSMGRAPHICS`) to their programs, because we convert to whatever
each client's host speaks:

| child draws | host speaks | goblin-view emits |
| --- | --- | --- |
| kitty | kitty | kitty (Unicode-placeholder placements) |
| kitty | sixel | sixel, rasterised at the cell grid |
| sixel | kitty | kitty (sixel decoded to RGBA) |
| sixel | sixel | sixel (decoded and re-emitted) |

Incoming pixels arrive by any kitty medium -- direct, file, temp file, or
POSIX shared memory -- all accepted at the window endpoint. Outgoing, the
medium goblin-view hands the host is `GOBLIN_VIEW_KITTY_MEDIUM` (`shm`
default, or `tempfile`/`file`/`direct`); shm keeps large images out of the
escape stream, and `direct` is the safe choice across a remote link.

A program's kitty image is usually PNG, so kitty-to-sixel needs a PNG
decoder; `src/png.cc` is an original one (zlib does the inflate) for the
non-interlaced colour types the terminal-drawing tools emit. `src/sixel.cc`
decodes and encodes sixel. The decision is per client on the paint side,
so a kitty and a sixel client attached at once each get the picture in
their own protocol, and a terminal that can show neither blanks the image
cells. Reattaching from a different terminal re-evaluates.

OSC 66 text sizing rides the same per-client gate as kitty graphics: a
kitty-graphics host is assumed to speak text sizing (kitty and Ghostty
do), so the scaled run is re-emitted to it; a plain host gets the text
unscaled. The window always models the run's cell footprint and advances
the cursor by the scaled width, so a program's CPR-based support probe
sees the movement and layout stays aligned regardless of the host.

Because a sixel host is painted at the window's cell-grid resolution
(the source is scaled down to that before encoding, never up), the size
we report for graphics via `XTSMGRAPHICS` is that same canvas. A program
that sizes its render to the reported graphics geometry then draws what
sixel will actually show, instead of a large image we would only throw
away in the downscale.
