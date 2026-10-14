# Keys

The menu key is an isolated **Alt tap** (press Alt, release it without
another key), not a chord. Applications keep Ctrl/Alt combinations.

goblin-view runs best on **Ghostty**. On attach we push kitty keyboard flags 11
(disambiguate + event types + report-all) so Ghostty reports Alt
press and release. Alt press then Alt release with nothing in between
opens the menu. Alt+letter as a chord is forwarded to the focused window.

Ctrl-\\ is the same key for tests and for a terminal that swallows the Alt
tap. On macOS set `macos-option-as-alt = true` in `~/.config/ghostty/config`
so the Option tap reaches goblin-view. Menu `?` shows a help box with the
keys, the mouse, and this setup.

Esc dismisses an open menu. A lone ESC is indistinguishable from the
start of a CSI, so the client holds it for `DV_ESC_TIMEOUT` (50 ms) and
then delivers it; a kitty graphics reply is exempt and keeps
accumulating.

The menu key then a letter does not wait for the menu to be read, but
the menu is drawn now: see **Drawing** below.

## Main menu (after the menu key)

Wired up today: `O`, `C`, `R` (rearrange), `Z`, `M` (mark: copy mode),
`T` (transfer), `H` (hide), `S` (switch list), `B` (broadcast), `U`,
`I`, `?`, `D` (detach), `Q` and `1`-`9`. Windows overlap: the compositor paints every visible window in
z-order with a frame, focused on top, on a stippled backdrop. Rearrange:
`R` then `M` move / `R` resize / `P` then `1-9` snap. Arrows nudge one
cell, Shift-arrows five. Enter keeps it, Esc restores.

| key | command |
| --- | --- |
| O | Open Window |
| S | Switch Windows (a filterable list; Enter raises, also unhides) |
| C | Close Window |
| R | Rearrange |
| Z | Zoom |
| M | Mark: copy mode (hjkl/arrows, v select, / ? search, y copies) |
| T | Transfer: paste the mark into the focused window |
| H | Hide the focused window (its number or S brings it back) |
| B | Broadcast typing to every window (toggle) |
| U | Unicode keyboard (browse & insert a glyph) |
| I | Input method (chooser) |
| ? | Help |
| D | Detach (leave the session running) |
| Q | Quit goblin-view (asks y/n, then closes everything) |
| 1–9 | Switch to that window (first nine; mouse selects any) |

The menu key then a letter works without waiting for the menu to be read.

## Unicode picker (Menu U)

An overlay of glyph categories — arrows, math, punctuation, currency,
box drawing, Greek, accented Latin, emoji. Arrows move the highlight,
Tab (or `[`/click a tab) changes category, Enter or Space types the
highlighted glyph into the focused window, and a mouse click picks a
glyph directly. Esc closes it. `src/unicode.cc` holds the tables and the
one layout function that both the painter and the click hit-test use.

A program can resize its own window with XTWINOPS `CSI 8 ; rows ; cols t`
(what many terminals accept to resize the window). goblin-view resizes the
window to match, shifting the top-left in if it would run past the right or
bottom edge, and capping at the desktop size when the request is larger
than fits.

## Text selection and copy

Drag inside a window's body to select text; the frame border still moves
the window and the bottom-right corner still resizes it, so selection and
window management do not collide. If the program inside grabbed the mouse,
hold Shift to select over it. On release the selection is copied to the
system clipboard as OSC-52 (see below), so it reaches the real terminal's
clipboard through any outer multiplexer. A plain click clears the
selection.

A drag-and-drop onto the terminal arrives from the host as a bracketed
paste (the client turns on DECSET 2004 there). goblin-view delivers it to
the focused window in that window's own paste mode: wrapped in
`ESC[200~`/`ESC[201~` when the program enabled bracketed paste, and as the
bare path when it did not, so the markers never leak into a program that
did not ask for them. The host reports no drop coordinates, so the drop
goes to the focused window rather than whichever window sits under the
pointer.

The wheel scrolls the window under the pointer (or the focused one over
the backdrop), the way a native terminal does. A program that tracks the
mouse gets the wheel event. Otherwise, on the alternate screen -- a
pager, an editor -- each notch becomes a few arrow keys (alternate
scroll, DECCKM-aware, and an app can turn it off with DECRST 1007). On
the primary screen the wheel scrolls the window's own scrollback: lines
that scroll off the top are kept (a thousand of them), and the wheel
moves a viewport back into that history. New output, or a keypress to
the window, snaps back to the live bottom, and the cursor is frozen while
you are scrolled back. Scrolling never raises or refocuses a window.

## Input methods (Menu I)

Menu `I` opens a chooser; a letter picks the method, and typing then
composes into a preedit shown on the bottom bar before it commits Unicode
into the focused window. Doing this in the multiplexer keeps entry
identical on every terminal. All conversion tables are hand-authored
original data (no third-party datasets), so provenance is clean and
coverage is a common subset.

| key | method |
| --- | --- |
| O | Off |
| R | Romaji -> hiragana (algorithmic) |
| U | US-International (dead keys: `'` `` ` `` `^` `"` `~` then a letter) |
| P | Pinyin -> hanzi (type a syllable, Space/Enter or 1-9 picks a candidate; `.`/`,` page) |
| Z | Zhuyin (bopomofo keys) -> pinyin -> hanzi |
| W | Wubi 86 (shape code a-z) -> hanzi/words |

Esc drops the preedit; Backspace edits it. Pinyin and Wubi use complete
tables loaded at runtime from `share/pinyin.dict` and `share/wubi.dict`,
built by `tools/gen-imdata.py` from permissively-licensed sources
(mozillazg/pinyin-data + Unicode Unihan; KyleBing/rime-wubi86-jidian,
Apache-2.0) -- see `share/README.md` for provenance. Cangjie and Flick
are not yet wired.

## Graphics and clipboard

Inner programs are terminal emulators of their own, so their kitty
graphics and clipboard escapes are captured per window and replayed to
the host. The full list of kitty protocol extensions a window's program
can use -- keyboard, colours, pointer shapes, extra cursors, drag and
drop, file transfer -- is in the README under "What a program in a
window can use".

- **Kitty graphics** (`ESC _ G … ST`), in `src/tmuxport/kitty.cc`. Each
  window is a kitty *endpoint*: it reassembles the transmission (`m=1` …
  `m=0`, the way gnuplot's `kittycairo` sends a PNG in ~4 KB pieces),
  answers the program's queries, and keeps the pixels. The program's
  bytes never reach the host. goblin-view is in turn a kitty *client* of the host:
  it re-transmits the stored pixels under a remapped, host-global id
  (`DV_KITTY_FIRST_ID` + window, as tmux remaps) and issues a placement
  where the window sits. Moving the window re-issues only the placement,
  so the image follows with no pixels re-sent; losing focus, hiding,
  switching to the alternate screen, or scrolling off a viewport takes it
  down. The window must stay alive to keep showing it, so a one-shot
  `gnuplot -e 'plot ...'` that exits at once takes its window (and image)
  with it; plot from an interactive session or add a `pause`. One image
  per window today; z-order cropping and multiple images are future
  work, tracked against `~/dev/tmux` in `docs/tmux-tracking.md`.
- **OSC-52** clipboard, both directions. A *set* is forwarded to every
  attached terminal verbatim. A *read* (`52;<sel>;?`) is sent to one
  attached terminal, and goblin-view remembers which window asked; the host's
  reply is recognised as an OSC (not keystrokes), carried whole even
  when it arrives in pieces, and delivered to that window -- whatever is
  focused by then. A read with nobody attached is answered at once with
  an empty clipboard for the same selection so the program never blocks.
  If goblin-view is nested inside tmux/goblin-tmux (`$TMUX` set) the client wraps
  outgoing sequences in the tmux passthrough so they reach the outermost
  terminal; replies come back through it untouched.

## Title editing

Double-click a framed window's title bar to rename it: type the new name,
Enter keeps it, Esc cancels. A user-set name is remembered and the
program's own title escapes (OSC 0/2) no longer override it. `list-windows`
shows the current title.

## Mouse

The client turns on drag tracking (`?1002`) and SGR coordinates
(`?1006`) on the host. Events reach the server, which decides what a
click means:

- **Body click** selects and raises the window. If the program inside
  asked for the mouse, the event is translated into that window's own
  coordinates and forwarded to it (SGR, or legacy X10 as a fallback).
- **Frame drag** moves the window.
- **Bottom-right corner drag** resizes it; the inner PTY is resized to
  match.
- **Body drag** selects text (see above); the frame border and corner keep
  moving/resizing. Pushing a window past the right or bottom edge shrinks
  it so its far edge stays on-screen.

The same is scriptable over the socket: `goblin-view mouse [-s name]
press|drag|release x y`.

## Detach and reattach

Each window is a terminal emulator whose grid, cursor and modes live in
the server, so the session survives with everything running when no
client is attached. Menu `D` detaches; `goblin-view` reattaches. `Q` tears the
whole server down. With two clients attached the desktop is sized to the
**largest**; a smaller client scrolls a viewport that follows the
focused window.

## Rearrange

| key | command |
| --- | --- |
| M | Move (arrows, Shift=5, Enter accept, Esc cancel) |
| R | Resize (arrows, top-left of the inner area stays put) |
| S | Scroll (viewport into a locked logical size) |
| P | Position (snap 1–9, computed from the **current** desktop size) |
| F | Freeze (`SIGSTOP`) |
| H | Hide |
| A | Put Aside |

Snaps 1–9 are not 80×25 coordinates. They are recipes of the live desktop:
top/bottom halves, centre ~2/3, left/right halves, four quadrants.

## Drawing

The menu is drawn by the compositor (`src/menu.cc` supplies the cells,
`src/compose.cc` paints them), not by the client. It is a layer on top
of the composed desktop, so it needs no alternate screen and no repaint
tricks:

- **Box.** A centred box, when the desktop is at least `46x18`.
- **Bar.** A single reverse-video row on the last line, when the
  desktop is smaller than that.

`struct dv_session.menu_open` holds the state; the client only reports
the Alt tap (`menu menu`). Closing the menu clears the flag, and the
next composed frame simply does not contain it — the diff repaints the
cells it covered. Inner windows keep running behind it; their output
updates their own screen model and is composited normally.
