CXX      ?= c++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O2 -g
CPPFLAGS += -Isrc
CPPFLAGS += -DODIN_SEAT_DATADIR='"$(CURDIR)/share"'
LDFLAGS  ?=

# Install locations. PREFIX defaults to a per-user prefix on PATH; override
# for a system install, e.g. `make install PREFIX=/usr/local`.
PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/odin-seat
MANDIR  ?= $(PREFIX)/share/man
# The data directory compiled into the binary as a fallback: the binary first
# looks beside itself (bin/../share/odin-seat). A relocatable build (a
# Homebrew bottle) passes a relative path, resolved against the executable.
DATADIR_COMPILED ?= $(DATADIR)
LIBS     += -lz -lm

UNAME := $(shell uname -s)
ifeq ($(UNAME),Linux)
# glibc before 2.34 provides shm_open/shm_unlink in librt.
LIBS += -lutil -lrt
endif

SRCS = src/agent.cc src/dv.cc src/client.cc src/server.cc src/session.cc src/window.cc \
       src/desktop.cc src/pty.cc src/sock.cc src/msg.cc src/buf.cc \
       src/xmalloc.cc src/log.cc src/host.cc src/menu.cc src/term.cc src/silkie.cc src/pstatus.cc src/xfer.cc \
       src/kbd.cc src/config.cc src/saved.cc src/state.cc src/compose.cc src/unicode.cc src/im.cc src/sixel.cc src/png.cc \
       src/tmuxport/kitty.cc
OBJS = $(SRCS:.cc=.o)

# The vector rasterizer is C (src/vector/vector.c, from Silkie). It is built
# once and linked into the program and into every test that links kitty.cc.
CC       ?= cc
VECFLAGS ?= -std=c11 -O2 -g -Wall -Wextra
# Text in a scene is shaped by HarfBuzz (src/vtext.cc).
HB_CFLAGS := $(shell pkg-config --cflags harfbuzz)
HB_LIBS   := $(shell pkg-config --libs harfbuzz)
CPPFLAGS  += $(HB_CFLAGS)
LIBS      += $(HB_LIBS)
VECOBJ    = src/vector/vector.o src/vtext.o

.PHONY: all test test-installer clean track install uninstall

all: odin-seat

odin-seat: $(OBJS) $(VECOBJ)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(VECOBJ) $(LIBS)

src/vector/vector.o: src/vector/vector.c src/vector/vector.h src/vector/vector-color.h \
    src/vector/vector-port.h src/vector/verstable.h src/vtext.hpp
	$(CC) $(VECFLAGS) -c -o $@ src/vector/vector.c

src/vtext.o: src/vtext.cc src/vtext.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ src/vtext.cc

src/agent.o: src/agent.hpp src/ssh_agent.hpp

src/term.o src/server.o test-host test-menu test-codec test-config test-pointer test-silkie: src/notification_route.hpp

src/%.o: src/%.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

test-desktop: src/test-desktop.cc src/desktop.cc src/xmalloc.cc src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-desktop.cc src/desktop.cc src/xmalloc.cc src/log.cc

test-host: src/test-host.cc src/host.cc src/silkie.cc src/pstatus.cc src/term.cc src/xfer.cc \
	    src/tmuxport/kitty.cc $(VECOBJ) src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc \
	    src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-host.cc src/host.cc src/silkie.cc src/pstatus.cc src/term.cc src/xfer.cc \
	    src/tmuxport/kitty.cc $(VECOBJ) src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc \
	    src/log.cc $(LIBS)

test-menu: src/test-menu.cc src/menu.cc src/compose.cc src/unicode.cc \
	    src/im.cc src/desktop.cc src/term.cc src/silkie.cc src/pstatus.cc src/xfer.cc src/tmuxport/kitty.cc $(VECOBJ) \
	    src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-menu.cc src/menu.cc src/compose.cc src/unicode.cc \
	    src/im.cc src/desktop.cc src/term.cc src/silkie.cc src/pstatus.cc src/xfer.cc src/tmuxport/kitty.cc $(VECOBJ) \
	    src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc $(LIBS)

test-codec: src/test-codec.cc src/png.cc src/sixel.cc src/tmuxport/kitty.cc $(VECOBJ) \
	    src/term.cc src/silkie.cc src/pstatus.cc src/xfer.cc src/desktop.cc src/buf.cc src/xmalloc.cc src/log.cc \
	    src/im.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-codec.cc src/png.cc src/sixel.cc src/tmuxport/kitty.cc $(VECOBJ) \
	    src/term.cc src/silkie.cc src/pstatus.cc src/xfer.cc src/desktop.cc src/buf.cc src/xmalloc.cc src/log.cc \
	    src/im.cc $(LIBS)

test-config: src/test-config.cc src/config.cc src/host.cc src/silkie.cc src/pstatus.cc src/term.cc \
	    src/xfer.cc src/tmuxport/kitty.cc $(VECOBJ) src/sixel.cc src/png.cc src/buf.cc \
	    src/xmalloc.cc src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-config.cc src/config.cc src/host.cc src/silkie.cc src/pstatus.cc src/term.cc \
	    src/xfer.cc src/tmuxport/kitty.cc $(VECOBJ) src/sixel.cc src/png.cc src/buf.cc \
	    src/xmalloc.cc src/log.cc $(LIBS)

test-im: src/test-im.cc src/im.cc src/buf.cc src/xmalloc.cc src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-im.cc src/im.cc src/buf.cc src/xmalloc.cc src/log.cc

test-term: src/notification_route.hpp src/test-term.cc src/term.cc src/silkie.cc src/pstatus.cc src/xfer.cc src/kbd.cc src/tmuxport/kitty.cc $(VECOBJ) \
	    src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc src/dv.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ \
	    src/test-term.cc src/term.cc src/silkie.cc src/pstatus.cc src/xfer.cc src/kbd.cc src/tmuxport/kitty.cc $(VECOBJ) \
	    src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc $(LIBS)

test-notifications: src/test-notifications.cc src/notification_route.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ src/test-notifications.cc

test: test-notifications test-pointer test-silkie all test-desktop test-host test-menu test-term test-im test-codec test-config
	./test-notifications
	./test-pointer
	./test-silkie
	./test-desktop
	./test-config
	./test-host
	./test-menu
	./test-term
	./test-im
	./test-codec
	python3 regress/paths.py
	sh regress/skeleton.sh
	python3 regress/pointer.py
	python3 regress/silkie.py
	python3 regress/layers.py
	python3 regress/tiling.py
	python3 regress/pictures.py
	python3 regress/silkie-real.py
	python3 regress/ink.py
	python3 regress/resize.py
	python3 regress/notifications.py
	python3 regress/pstatus.py
	python3 regress/windows.py
	python3 regress/loop.py
	python3 regress/e2e.py
	sh tools/track-tmux

track:
	sh tools/track-tmux

test-installer:
	sh -n html/install.sh
	python3 regress/installer.py

# Build a fresh binary with the runtime data dir baked in and install it
# plus the input-method dictionaries. Compiled in one shot so the dev
# object files (which bake the repo's share dir) are left untouched.
install: $(VECOBJ)
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(DATADIR)"
	@set -eu; build_dir=$$(mktemp -d); \
	    trap 'rm -rf "$$build_dir"' EXIT HUP INT TERM; \
	    $(CXX) -Isrc $(HB_CFLAGS) -DODIN_SEAT_DATADIR='"$(DATADIR_COMPILED)"' $(CXXFLAGS) $(LDFLAGS) \
	    -o "$$build_dir/odin-seat" $(SRCS) $(VECOBJ) $(LIBS); \
	    install -m 0755 "$$build_dir/odin-seat" "$(DESTDIR)$(BINDIR)/odin-seat"
	install -m 0644 share/pinyin.dict share/wubi.dict share/README.md \
	    "$(DESTDIR)$(DATADIR)/"
	install -d "$(DESTDIR)$(MANDIR)/man1" "$(DESTDIR)$(MANDIR)/man7"
	install -m 0644 man/odin-seat.1 "$(DESTDIR)$(MANDIR)/man1/"
	install -m 0644 man/odin-seat-protocols.7 "$(DESTDIR)$(MANDIR)/man7/"
	@echo "installed odin-seat -> $(DESTDIR)$(BINDIR)/odin-seat"
	@echo "           data       -> $(DESTDIR)$(DATADIR)"
	@echo "           manuals    -> $(DESTDIR)$(MANDIR)/man1, man7"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/odin-seat"
	rm -rf "$(DESTDIR)$(DATADIR)"
	rm -f "$(DESTDIR)$(MANDIR)/man1/odin-seat.1" \
	    "$(DESTDIR)$(MANDIR)/man7/odin-seat-protocols.7"

clean:
	rm -f odin-seat test-notifications test-pointer test-silkie test-desktop test-host test-menu test-term test-im test-codec test-config $(OBJS) src/tmuxport/*.o src/vector/*.o src/vtext.o .track-tmux-drift

test-pointer: src/test-pointer.cc src/term.cc src/silkie.cc src/pstatus.cc src/xfer.cc src/tmuxport/kitty.cc $(VECOBJ) src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc src/dv.hpp src/pointer_coordinates.hpp src/goblin_pointer.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ src/test-pointer.cc src/term.cc src/silkie.cc src/pstatus.cc src/xfer.cc src/tmuxport/kitty.cc $(VECOBJ) src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc $(LIBS)

test-silkie: src/test-silkie.cc src/silkie.cc src/pstatus.cc src/term.cc src/xfer.cc src/tmuxport/kitty.cc $(VECOBJ) src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc src/dv.hpp src/silkie.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS) -o $@ src/test-silkie.cc src/silkie.cc src/pstatus.cc src/term.cc src/xfer.cc src/tmuxport/kitty.cc $(VECOBJ) src/sixel.cc src/png.cc src/buf.cc src/xmalloc.cc src/log.cc $(LIBS)
