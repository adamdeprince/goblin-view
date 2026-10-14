CXX      ?= c++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O2 -g
CPPFLAGS += -Isrc
CPPFLAGS += -DGOBLIN_VIEW_DATADIR='"$(CURDIR)/share"'
LDFLAGS  ?=

# Install locations. PREFIX defaults to a per-user prefix on PATH; override
# for a system install, e.g. `make install PREFIX=/usr/local`.
PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/goblin-view
MANDIR  ?= $(PREFIX)/share/man
# The data directory compiled into the binary as a fallback: the binary first
# looks beside itself (bin/../share/goblin-view). A relocatable build (a
# Homebrew bottle) passes a relative path, resolved against the executable.
DATADIR_COMPILED ?= $(DATADIR)
LIBS     += -lz

UNAME := $(shell uname -s)
ifeq ($(UNAME),Linux)
LIBS += -lutil
endif

SRCS = src/dv.cc src/client.cc src/server.cc src/session.cc src/window.cc \
       src/desktop.cc src/pty.cc src/sock.cc src/msg.cc src/buf.cc \
       src/xmalloc.cc src/log.cc src/host.cc src/menu.cc src/term.cc src/silkie.cc src/xfer.cc \
       src/kbd.cc src/config.cc src/saved.cc src/state.cc src/compose.cc src/unicode.cc src/im.cc src/sixel.cc src/png.cc \
       src/tmuxport/kitty.cc
OBJS = $(SRCS:.cc=.o)

.PHONY: all test clean track install uninstall

all: goblin-view

goblin-view: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

src/%.o: src/%.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

test-desktop: src/test-desktop.cc src/desktop.cc src/xmalloc.cc src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-desktop.cc src/desktop.cc src/xmalloc.cc src/log.cc

test-host: src/test-host.cc src/host.cc src/silkie.cc src/term.cc src/xfer.cc \
	    src/tmuxport/kitty.cc src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc \
	    src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-host.cc src/host.cc src/silkie.cc src/term.cc src/xfer.cc \
	    src/tmuxport/kitty.cc src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc \
	    src/log.cc $(LIBS)

test-menu: src/test-menu.cc src/menu.cc src/compose.cc src/unicode.cc \
	    src/im.cc src/desktop.cc src/term.cc src/silkie.cc src/xfer.cc src/tmuxport/kitty.cc \
	    src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-menu.cc src/menu.cc src/compose.cc src/unicode.cc \
	    src/im.cc src/desktop.cc src/term.cc src/silkie.cc src/xfer.cc src/tmuxport/kitty.cc \
	    src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc $(LIBS)

test-codec: src/test-codec.cc src/png.cc src/sixel.cc src/tmuxport/kitty.cc \
	    src/term.cc src/silkie.cc src/xfer.cc src/desktop.cc src/buf.cc src/xmalloc.cc src/log.cc \
	    src/im.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-codec.cc src/png.cc src/sixel.cc src/tmuxport/kitty.cc \
	    src/term.cc src/silkie.cc src/xfer.cc src/desktop.cc src/buf.cc src/xmalloc.cc src/log.cc \
	    src/im.cc $(LIBS)

test-config: src/test-config.cc src/config.cc src/host.cc src/silkie.cc src/term.cc \
	    src/xfer.cc src/tmuxport/kitty.cc src/sixel.cc src/png.cc src/buf.cc \
	    src/xmalloc.cc src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-config.cc src/config.cc src/host.cc src/silkie.cc src/term.cc \
	    src/xfer.cc src/tmuxport/kitty.cc src/sixel.cc src/png.cc src/buf.cc \
	    src/xmalloc.cc src/log.cc $(LIBS)

test-im: src/test-im.cc src/im.cc src/buf.cc src/xmalloc.cc src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-im.cc src/im.cc src/buf.cc src/xmalloc.cc src/log.cc

test-term: src/test-term.cc src/term.cc src/silkie.cc src/xfer.cc src/kbd.cc src/tmuxport/kitty.cc \
	    src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-term.cc src/term.cc src/silkie.cc src/xfer.cc src/kbd.cc src/tmuxport/kitty.cc \
	    src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc $(LIBS)

test: test-pointer test-silkie goblin-view test-desktop test-host test-menu test-term test-im test-codec test-config
	./test-pointer
	./test-silkie
	./test-desktop
	./test-config
	./test-host
	./test-menu
	./test-term
	./test-im
	./test-codec
	sh regress/skeleton.sh
	python3 regress/pointer.py
	python3 regress/e2e.py
	sh tools/track-tmux

track:
	sh tools/track-tmux

# Build a fresh binary with the runtime data dir baked in and install it
# plus the input-method dictionaries. Compiled in one shot so the dev
# object files (which bake the repo's share dir) are left untouched.
install:
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(DATADIR)"
	$(CXX) -Isrc -DGOBLIN_VIEW_DATADIR='"$(DATADIR_COMPILED)"' $(CXXFLAGS) $(LDFLAGS) \
	    -o "$(DESTDIR)$(BINDIR)/goblin-view" $(SRCS) $(LIBS)
	install -m 0644 share/pinyin.dict share/wubi.dict share/README.md \
	    "$(DESTDIR)$(DATADIR)/"
	install -d "$(DESTDIR)$(MANDIR)/man1" "$(DESTDIR)$(MANDIR)/man7"
	install -m 0644 man/goblin-view.1 "$(DESTDIR)$(MANDIR)/man1/"
	install -m 0644 man/goblin-view-protocols.7 "$(DESTDIR)$(MANDIR)/man7/"
	@echo "installed goblin-view -> $(DESTDIR)$(BINDIR)/goblin-view"
	@echo "           data       -> $(DESTDIR)$(DATADIR)"
	@echo "           manuals    -> $(DESTDIR)$(MANDIR)/man1, man7"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/goblin-view"
	rm -rf "$(DESTDIR)$(DATADIR)"
	rm -f "$(DESTDIR)$(MANDIR)/man1/goblin-view.1" \
	    "$(DESTDIR)$(MANDIR)/man7/goblin-view-protocols.7"

clean:
	rm -f goblin-view test-pointer test-silkie test-desktop test-host test-menu test-term test-im test-codec test-config $(OBJS) src/tmuxport/*.o .track-tmux-drift

test-pointer: src/test-pointer.cc src/term.cc src/silkie.cc src/xfer.cc src/tmuxport/kitty.cc src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc src/dv.hpp src/pointer_coordinates.hpp src/goblin_pointer.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ src/test-pointer.cc src/term.cc src/silkie.cc src/xfer.cc src/tmuxport/kitty.cc src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc $(LIBS)

test-silkie: src/test-silkie.cc src/silkie.cc src/term.cc src/xfer.cc src/tmuxport/kitty.cc src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc src/dv.hpp src/silkie.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ src/test-silkie.cc src/silkie.cc src/term.cc src/xfer.cc src/tmuxport/kitty.cc src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc $(LIBS)
