# Clean room

GoblinView (the `goblin-view` command) is an independent implementation.
It is not a fork of tmux, screen, or zellij, and it does not contain
their source.

## Allowed

- Public specifications: POSIX, the [Kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/),
  the [Kitty keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/),
  XTWINOPS, the sixel graphics format (DEC VT330/VT340 manuals),
  Silkie's published extension protocols (the documents, not the C
  sources), and
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
  published protocol.
- Checking tmux C into this tree as a submodule for compilation.
- Using decompiled or leaked binaries of any multiplexer as a source of
  code.

If a multiplexer-side kitty behaviour is first seen in tmux (id remap,
query ownership, crop on scroll), re-derive it from the public protocol
and write original C++23 here.

`tools/track-tmux` reports when watched tmux files *change* so we can
re-read the spec and our tests. It does not import those files.
