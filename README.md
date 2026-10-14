# GoblinView

A terminal multiplexer with overlapping windows and an Alt-tap menu, not
tmux's tiled panes. The command is `goblin-view`. It runs best in
**Ghostty**: Alt-tap via the kitty keyboard protocol, cell pixels via
XTWINOPS / mode 2048, kitty graphics on the wire. It runs anywhere a
terminal does; the graphics features light up only when the terminal you
attach from speaks kitty graphics (Ghostty, kitty). Inner VT will be
libghostty-vt; multiplexer-side image id remap still tracks `~/dev/tmux`.

Windows overlap on a stippled backdrop, each in its own frame. Open
places a new framed window on top; selecting one raises it. Menu `R`
then `M` moves, `R` resizes, `P` `1-9` snaps, and the **mouse** does the
same: click to select and raise, drag the frame to move, drag the
bottom-right corner to resize, and the wheel scrolls the window under the
pointer as a native terminal would: the mouse event to an app that wants
it, arrow keys to a pager on the alternate screen, and the window's own
scrollback at a shell prompt. Each window is its own terminal emulator
whose state the server keeps, so you can **detach and reattach** like
tmux and mosh. Inner VT is a small original parser; libghostty-vt can
replace it later.

## Install

Debian and Ubuntu packages (amd64, one build per release: Ubuntu 22.04,
24.04, 26.04, Debian 12, 13) are published in the signed Goblin Reactor
repository. Setup instructions and the package list are at
https://apt.goblinreactor.com/; afterwards:

```
sudo apt install goblin-view
```

`packaging/debian/README.md` describes how the packages are built and
published.

## Build

C++23. Host is Ghostty. Independent implementation (`docs/CLEANROOM.md`).

```
make
make test
```

Binary: `goblin-view`. `make install` (default prefix `~/.local`) also
installs the manual pages: `man goblin-view` for the command, keys, mouse
and environment, and `man goblin-view-protocols` for what a program in a
window can use.

```
goblin-view                          # attach, creating a session if needed
goblin-view new-session -d -x 132 -y 43 -- sleep 30
goblin-view new-window -- htop        # open a window (inside a session too)
goblin-view list-windows
goblin-view kill-server
```

`-L name` selects a socket (like tmux). `-D` keeps the server in the
foreground. `-m` (or `GOBLIN_VIEW_MONO=1`) paints this terminal in strict
black and white for an e-paper display: ink on paper, or reverse video
where a cell had a coloured background, with the backdrop and other
shaded areas as plain paper; other attached terminals keep their
colours.

For scripts: `send-keys [-w N] text` types into a window (C escapes
decoded, `-l` for literal), `capture-window [-w N] [-S lines|-]` prints
its text, `pipe-window [-w N] path` logs its raw output, and `menu key`
presses a menu key. `save` writes every session's windows (place, size,
title, the program's current directory and command) to
`~/.local/state/goblin-view/sessions` and `restore` recreates them.
`upgrade` replaces the running server with this program in place: the
server hands its descriptors and state to the new binary and executes it
in the same process, so windows and attached terminals carry on, screen
and scrollback included (images are redrawn by their programs). A newer
client attaching to an older server that can do this upgrades it by
itself, so after `make install` the next attach is enough.

Settings live in `~/.config/goblin-view/config`: `shell`, `scrollback`,
`download-dir`, `kitty-medium`, `term`, `backdrop`, `click-raises`,
`mono` and `menu-key`, with `[host "name"]` sections that apply to the
terminal whose XTVERSION name matches, so an e-paper terminal can get
`mono = yes` without any flags. `csi-limit` and `escape-limit` optionally
bound one CSI sequence and one OSC, APC or DCS; `0`, the default, means no
limit. The same numbers can be set with `GOBLIN_VIEW_CSI_LIMIT` and
`GOBLIN_VIEW_ESCAPE_LIMIT`. See `goblin-view --help` and `man goblin-view`.

Detach with Menu `D`; the session and every program in it keep running.
On the way out goblin-view puts the terminal back as it found it: the
screen you had before is shown again, or, on a terminal with no
alternate screen (mosh), the screen is cleared.
Reattach by running `goblin-view` again. Two clients can attach at once: the
desktop is sized to the **largest** of them, and a smaller client
scrolls its viewport to follow the focused window rather than forcing
everyone down to the smallest size.

Run `goblin-view` from inside a window and it talks to the current session
instead of nesting a new one, so `goblin-view new-window` or `goblin-view list-windows`
there act on the desktop you are in.

## Keys

The first window opens with a few lines of instruction painted inside
it. They are ordinary cells: the shell's prompt lands below them and
they scroll away.

The menu key is an isolated **Alt tap**: press Alt and let go with
nothing in between, and the menu opens. Alt+letter stays a chord for the
focused window. On macOS Alt is **Option (⌥)**, either side, so set
`macos-option-as-alt = true` in `~/.config/ghostty/config` for the tap
to reach goblin-view. `Ctrl-\\` is the same key for a terminal that
swallows the Alt tap. Menu `?` opens a help box with the keys, the mouse,
and this setup.

The menu key pops the menu. Then a letter: `O` open, `C` close, `Z`
zoom, `R` rearrange, `M` mark (copy mode), `T` transfer (paste the
mark), `H` hide, `S` switch (a filterable window list), `B` broadcast
typing to every window, `U` Unicode picker, `I` input method, `?` help,
`D` detach, `Q` quit goblin-view, `1`-`9` focus that window (a hidden
one comes back). The menu box spells out how to move and resize. `Q`
asks for a `y`/`n` before it closes everything. Esc or a second tap
cancels, and the letter never has to wait for the menu to be read.

Copy mode (`M`) puts a cursor in the window that moves over its screen
and scrollback with `hjkl` or the arrows, `0`/`$`, `g`/`G` and the page
keys; `v` starts a selection, `/` and `?` search (wrapping,
case-insensitive unless the pattern has a capital, `n`/`N` repeat), and
`y` copies the selection to the system clipboard and keeps it as the
mark that `T` pastes into a window. `q` leaves.

`1`-`9` select the first nine windows; the mouse selects any, and you
can open as many as you like. `docs/keys.md` has the full GoblinView
design, most of which is not wired up yet.

Drag inside a window to select text; releasing copies it to the system
clipboard. Double-click a title bar to rename that window. Menu `I` chooses an input method (Romaji, US-International,
Pinyin, Zhuyin, Wubi) that composes CJK and accented text the same way on every
terminal.

Menu `U` opens a Unicode picker: an overlay of glyph categories you
drive with the arrows and Enter or click with the mouse. Tab (or a click
on a tab) changes category; the chosen character is typed into the
focused window.

Inline images work whenever the terminal you attach from can show one,
by **kitty graphics** or by **sixel**. Each window is its own graphics
endpoint: a program draws with either protocol, its picture is decoded to
pixels and kept as a placement floating over the window's grid, and
goblin-view re-emits that placement in whatever the host speaks. A redraw
of the text beneath leaves the picture, scrolling carries it, clearing
the screen removes it, `c=`/`r=` and the source rectangle size it, and a
delete removes it by id.

The four cases, child protocol against host protocol:

- **child kitty, host kitty** — re-emitted as kitty, placed by Unicode
  placeholder cells, so cropping, occlusion by other windows and moving
  the window are ordinary cell compositing (gnuplot's `kittycairo`, the
  Emacs previewer).
- **child kitty, host sixel** — the kitty image (usually PNG) is decoded
  and rasterised to sixel at the window's cell-grid resolution; still
  occluded per cell, since only the visible cells are drawn.
- **child sixel, host kitty** — the sixel is decoded and re-emitted as
  kitty, so it moves and clips like any kitty image.
- **child sixel, host sixel** — decoded and re-emitted as sixel,
  positioned and clipped by goblin-view.

A child may transmit its pixels by any kitty medium -- inline (direct),
a file, a temp file, or POSIX shared memory -- and all are accepted. What
goblin-view hands the host is the `GOBLIN_VIEW_KITTY_MEDIUM` tunable
(`shm`, `tempfile`, `file`, or `direct`), defaulting to shared memory so
a large image does not bloat the escape stream; set it to `direct` when
the terminal is reached across a link where a local file or shm would not
be, such as a nested ssh session.

What a program is offered is decided per session: if any attached client
can show images (by either protocol), programs may draw, and a client
whose terminal can show neither blanks the image cells. It is per client
on the paint side too, so a kitty client and a sixel client attached at
once each get the picture in their own protocol and a text client blanks
it. Detach and reattach from a different terminal and the answer follows
it. Because a sixel host is drawn at the window's cell-grid resolution,
the size we report for graphics (XTSMGRAPHICS) is that canvas, so a
program renders to what sixel will actually show rather than a larger
image that is then thrown away.

A program that sets or reads the clipboard with OSC-52 talks to the real
terminal, wrapped for an outer tmux/mosh when goblin-view is itself nested; a
read's answer comes back to the window that asked. A drag-and-drop, which the host
delivers as a bracketed paste, goes to the focused window in that window's
own paste mode (wrapped, or the bare path for a program that did not
enable bracketed paste).

Big text works too: a program that uses the OSC 66 text-sizing protocol
(`ESC ] 66 ; s=2 ; ...`) has the run laid into the window's grid at the
scaled size, the cursor advanced by the scaled width, and the run
re-emitted to a host that speaks text sizing (kitty, Ghostty). A host
that does not gets the text unscaled, so it is never lost, and a program
detecting support by the cursor's movement sees it move.

Every window has its own screen model, and the server composes them all
onto one desktop each time anything changes: framed windows overlapping
on a backdrop in z-order, focused on top. Each attached client is then
sent only the cells that differ from what it last saw, wrapped in
synchronized output (`CSI ?2026h`/`l`), so a redraw never tears or
flashes. New windows start in the directory the client was run from —
the server daemonises into `/`, and inheriting that is not useful.

The menu is one more layer the compositor paints on top of the desktop:
a centred box when the desktop is at least `46x18`, otherwise a
reverse-video bar on the last row. Closing it just omits the layer from
the next frame, so whatever it covered comes back with no repaint dance
and no nested alternate screen. Inner windows keep running behind it and
their output lands on their own screens, not on the menu.

Ghostty reports a bare Option press/release under kitty keyboard flags
11 only when Option is Alt, so a macOS host needs this in
`~/Library/Application Support/com.mitchellh.ghostty/config.ghostty`:

```
macos-option-as-alt = true
```

`right` instead of `true` keeps left Option composing (option-e for
e-acute) at the cost of tapping only the right one. See
`docs/ghostty.md` for the host asks still outstanding.

The same commands go over the socket with no keystroke at all:

```
goblin-view -L name menu o
```

## What a program in a window can use

Each window is a terminal of its own, so the kitty protocol extensions
work per window, and whatever is best done by the real terminal is
handed to it and its answer routed back to the window that asked.

- **Keyboard**: the kitty keyboard protocol per window (`CSI > flags u`
  push, `CSI < u` pop, `CSI = flags ; mode u`, `CSI ? u`), with separate
  stacks for the main and alternate screens. The host reports every key
  as a structured event; a window that pushed flags gets it encoded for
  those flags (disambiguated escapes, key releases and repeats,
  alternate keys, all keys as escapes, associated text), and a window
  that pushed none gets what a legacy terminal sends.
- **Text**: styled and coloured underlines (`SGR 4:3`, `58`, `59`),
  independent bold/faint reset (`SGR 221`, `222`), attribute changes over
  rectangles (DECCARA, DECSACE), OSC 66 text sizing, unscroll
  (`CSI n + T`), clear-into-scrollback (`CSI 22 J`), and XTSAVE/XTRESTORE
  of private modes (`CSI ? Pm s` / `r`).
- **Silkie** (OSC 777 `caps`, `display`, `frame`, `gesture`, `haptic`,
  `ink`, `latency`, `scroll`, `tablet-pointer`): goblin-view answers
  these itself. A selector is offered only when goblin-view can carry
  it and an attached terminal listed it. A contact stays with the
  window it started on, translated into that window's cells. Picture
  formats are whatever every attached terminal shares. `goblin-pointer`
  and `goblin-download` are unchanged.
- **Mouse**: pixel-position reporting (mode 1016) with kitty's leave
  event (button 288) when the pointer moves off the window; hover motion
  for windows in mode 1003; pointer shapes (OSC 22) kept as a stack per
  window and shown by the host for the window under the pointer.
- **Colours**: OSC 4, 10, 11, 12, 21 (kitty colour control) and the
  push/pop of OSC 30001/30101 change only that window: the compositor
  remaps its cells, so two windows can redefine the palette differently
  and the host's own palette is never touched. Queries are answered from
  the window's state; the cursor colour is applied to the host for the
  focused window.
- **Clipboard and notifications**: OSC 52 and kitty's OSC 5522 go to the
  terminal, and desktop notifications (OSC 99) with them; replies come
  back to the window that asked by request id.
- **Extra cursors** (kitty multiple cursors, `CSI > shape ; type : coords
  SP q`, queries `100`, `101`): a host that draws them is sent the set
  translated to the desktop; one that cannot sees them painted as
  reverse video or underline.
- **Drag and drop** (OSC 72): the drop lands on the window and cell it
  was dropped into. A program that said it takes drops (`t=a`) gets the
  protocol translated into its own cells and its answers relayed;
  otherwise goblin-view accepts on the window's behalf, fetches the
  files' paths or the text, and pastes them into that window (bracketed
  when the program asked for it). A terminal that sends drops as a
  bracketed paste still gets the paste routed to the focused window.
- **File transfer**: kitty's protocol (OSC 5113) in both directions and
  goblin-mosh's background download (OSC 777 `goblin-download`). When
  the terminal you attach from speaks the protocol -- kitty for 5113,
  goblin-mosh for 777, known by their XTVERSION names -- the request is
  handed to it. Otherwise the buck stops here: a box asks whether to save
  into your downloads directory (`$GOBLIN_VIEW_DOWNLOAD_DIR`,
  `$XDG_DOWNLOAD_DIR`, or `~/Downloads`), or whether a program may read
  the files it named; files are staged, their sizes verified, published
  without overwriting (`report.pdf`, then `report.pdf.1`), never opened
  or run, and the program is answered in its own protocol. A pasted "y"
  cannot approve anything.
- **Queries**: XTVERSION names the multiplexer; XTGETTCAP answers the
  capabilities a program can count on (`TN`, `Tc`, `RGB`, `Su`, `colors`,
  `setrgbf`/`setrgbb`, `Smulx`, `Setulc`, `Ms`) and says so for the rest.

`goblin-view list-clients` shows what each attached terminal is
(its XTVERSION name) and what it can do.

## License

GoblinView is licensed under the Apache License, Version 2.0 (`LICENSE`,
`NOTICE`). The bundled input-method dictionaries are generated from
MIT-, Apache-2.0- and Unicode-licensed data; `share/README.md` records
their provenance.

## Tracking tmux

Kitty protocol work lives in Adam's tmux `kitty-graphics` branch. Pin and
diff:

```
tools/track-tmux          # fail if ~/dev/tmux moved
tools/track-tmux pin      # after a deliberate port
```

See `docs/tmux-tracking.md`. Ported code belongs in `src/tmuxport/` only.


### Pen, fingers and cell-tiled drawing

Applications opt into [Goblin pointer v1](docs/GOBLIN_POINTER_PROTOCOL.md) independently
in each window. GoblinView translates content coordinates for the desktop viewport,
window frame and content offset, preserving fractional pixels, pressure, tilt,
barrel rotation when supplied, eraser identity and touch contacts. A contact remains
captured by its original window through release, even if it crosses another window.
Two-finger gestures require both contacts to belong to the same window. Resize,
disconnect, reset and application mode changes cancel unfinished contacts.
Applications using ordinary mouse tracking receive translated pen/finger events.

Kitty image storage, placements, and client display caches use dynamic hash maps,
with no fixed image-count or memory budget. Image pixel buffers grow dynamically,
including images read from files or shared memory. This supports Inkline's Goblin
Draw client, which uses one image per character cell and
updates only cells touched by a stroke. Window clipping still uses Kitty Unicode
placeholders; the updated Inkline renderer supports them. Client/server protocol 5
adds pointer capability metadata; rebuild both sides together. The explicit
`upgrade` command preserves per-window input modes.
