# Clean room

Odin Seat (the `odin-seat` command) is an independent implementation.
It is not a fork of tmux, screen, or zellij, and it does not contain
their source.

## Allowed

- Public specifications: POSIX, the [Kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/),
  the [Kitty keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/),
  XTWINOPS, the sixel graphics format (DEC VT330/VT340 manuals),
  Silkie's published extension protocols (the documents, not the C
  sources), the [Program Status Protocol](https://www.superlogical.com/rex/docs/build/program-status)
  (OSC 7501), ConEmu's published [escape code list](https://conemu.github.io/en/AnsiEscapeCodes.html)
  (its OSC 9 sub-commands), and
  Ghostty's published C API for libghostty-vt. The sixel decoder in
  `src/sixel.cc` is original code written from that public description.
- The *behaviour* of the classic DOS windowing multiplexers as described
  in their user manuals and contemporary reviews (Alt-tap menu,
  overlapping windows, snap 1–9, zoom, hide, mark/transfer).
- Linking **libghostty-vt** as a library (headers + static/shared lib).
- Watching `~/dev/tmux` for *feature* drift (new protocol cases to
  cover). That tree is a checklist, not a source of copy-paste.

## Not allowed

- Copying, translating, or mechanically porting tmux/screen/zellij source
  into this repository (`src/tmuxport/` is not a dump of tmux files).
- Linking Silkie or kitty, or copying their protocol sources. The
  Silkie commands in this tree are an original implementation of the
  published protocol. There is one exception, below.
- Checking tmux C into this tree as a submodule for compilation.
- Using decompiled or leaked binaries of any multiplexer as a source of
  code.

## The one exception: the vector rasterizer

`src/vector/vector.c`, `vector.h` and `vector-color.h` are copied from
Silkie (`kitty/vector.c`, `kitty/vector.h`, `kitty/vector-color.h` at
commit `50487665`, 2026-11-16). They draw Silkie's vector images
(`f=200`) so that odin-seat can show them on a terminal that cannot.

They are here because their author is odin-seat's author: every line of
those files in Silkie's history is by Adam DePrince, who holds the
copyright in them and put them in this tree under the Apache License
2.0. They are not kitty's code, and nothing of kitty's came with them:

- Silkie's text records are laid out by Python that asks kitty's font
  code for fonts. `compile_text()` was replaced by a call into
  `src/vtext.cc`, which is the same exception again: its layout and
  shaping are ported from Silkie's `kitty/fonts/vector.py` and
  `kitty/vector-text.h` (same commit), both by the same author and both
  written against HarfBuzz alone. What those files took from kitty --
  finding the fonts of the machine, the configured font roles -- was not
  ported: `vtext.cc` finds fonts by reading the font files in the font
  directories itself, and a role is a short list of common families.
- kitty's wrapper around the hash table (`kitty-verstable.h`) was not
  copied. `src/vector/verstable.h` is Jackson Allan's Verstable itself,
  MIT licensed, and `src/vector/vector-port.h`, which stands in for the
  rest of what `vector.c` took from Silkie, is original to this tree.

Each change from Silkie's file is marked `Odin Seat:` in the source. No
other Silkie or kitty source may be copied on the strength of this.

If a multiplexer-side kitty behaviour is first seen in tmux (id remap,
query ownership, crop on scroll), re-derive it from the public protocol
and write original C++23 here.

`tools/track-tmux` reports when watched tmux files *change* so we can
re-read the spec and our tests. It does not import those files.
