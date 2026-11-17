# Tracking `~/dev/tmux`

The host terminal is Ghostty (`docs/ghostty.md`). Odin Seat is still a new
multiplexer, not a tmux fork. Kitty graphics *as a multiplexer* (id remap,
per-client transmit, crop, query ownership) is taken from Adam's tmux tree
(`github.com:adamdeprince/tmux.git`, branch `kitty-graphics`) and must be
re-ported as that tree moves.

Clean-room: do **not** copy tmux source. When this tool reports drift,
re-read the public kitty/keyboard specs and extend *our* tests and
original C++. See `docs/CLEANROOM.md`.

## How to check

```sh
tools/track-tmux              # exit 1 if SHA or watched files moved
TMUX_SRC=/path/to/tmux tools/track-tmux
tools/track-tmux pin          # rewrite the manifest after a deliberate port
```

The pin lives in `docs/tmux-tracking.manifest`. Last synced commit:

`76038e6e91fc0a6bc7e8350d7ac5e72b2d1df2c0` (`kitty-graphics`)

## Watched paths

| tmux file | why |
| --- | --- |
| `image-kitty.c` | APC parse, id remap, placements, virtual U=1, crop, transmit |
| `image.c` | image list, fallback, type switch |
| `tty.c` | per-client transmit cache, delete, place-seen sweep |
| `tty-keys.c` | XTWINOPS 16/18/14 (`tty_keys_winsz`), kitty graphics query reply |
| `tty-features.c` | `TERM_KITTY` feature flag |
| `input.c` | APC delivery into the kitty parser |
| `tmux.h` | structs, `DEFAULT_XPIXEL`/`DEFAULT_YPIXEL`, `TERM_KITTY` |
| `resize.c` | pixel-only WINCH when cells did not change |
| `regress/kitty-graphics.sh` | query/transmit/place/quiet/passthrough reply ownership |

## Behaviour that a port must keep

- Image identifiers are terminal-global. Keep the id the application asked
  for when no other window has it; otherwise map onto ids starting at
  `KITTY_FIRST_ID` (`0x10000000`). Unicode placeholders cannot be rewritten.
- Quiet queries (`q=2`) for multiplexer-owned traffic. Only consume the
  reply to **our** graphics query; other `ESC _ G` replies belong to the
  pane (passthrough).
- Chunk payload at 4096. The tracked tmux implementation limits storage to
  16 MiB / image, 32 MiB total, and 64 images; odin-seat dynamically allocates
  image storage without fixed image-count or memory budgets.
- Direct placements: `a=p,C=1` with source crop when partially visible.
- Virtual placements (`U=1`) are re-emitted whenever the image is
  transmitted.
- Cell size: `CSI 16 t` is authoritative (`TTY_HAVECELLSIZE`). `CSI 14 t`
  only estimates pixels until then. Fallback 16×32 until measured, then a
  second WINCH.
- `ws_xpixel` / `ws_ypixel` on the PTY are **window** pixels (`cell * cols`),
  matching tmux `spawn.c` / `window.c`.

## Where ports live

Ported C goes in `src/tmuxport/` only. Odin Seat window ids, the overlapping
compositor, and menus must not be mixed into those files. Glue is
`src/kitty.c` (when it exists).

A re-sync is: `tools/track-tmux` → port the delta into `src/tmuxport/` →
extend regress → `tools/track-tmux pin`.

## Overlap (Odin Seat, not tmux)

tmux floating panes still live on a tiled spine. Kitty images paint **above**
cells in the host terminal, so covering a cell with a space does not hide an
image. Odin Seat must clip placements by z-order (crop or delete) when another
window occludes them. That logic stays outside `src/tmuxport/`.
