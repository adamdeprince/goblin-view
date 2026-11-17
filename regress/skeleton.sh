#!/bin/sh
# Client/server smoke: detached session, measured size (not assumed 80x25),
# list-windows, kill-server.
set -e

DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
GV="${GV:-$DIR/odin-seat}"
L="t$$"
export GV

cleanup() {
	"$GV" -L "$L" kill-server 2>/dev/null || true
}
trap cleanup EXIT INT TERM

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

"$GV" -L "$L" new-session -d -x 132 -y 43 -- sleep 30 || fail "new-session"

out=$("$GV" -L "$L" list-windows) || fail "list-windows"
echo "$out" | grep -q 'focused' || fail "first window should be focused"
# Window 1 is a framed float now, sized from the desktop, never a baked 80x24.
echo "$out" | grep -q '(80x24)' && {
	echo "$out"
	fail "window must scale to the desktop, not a baked 80x24"
}
echo "$out" | grep -q ':1:' | grep -q zoomed && fail "first window is framed, not zoomed"

"$GV" -L "$L" list-sessions | grep -q '132x43' || fail "list-sessions size"

"$GV" -L "$L" menu o || fail "menu open"
out=$("$GV" -L "$L" list-windows) || fail "list-windows after open"
echo "$out" | grep -q ':2:' || {
	echo "$out"
	fail "menu O should open window 2"
}
echo "$out" | grep ':2:' | grep -q zoomed && fail "window 2 should be framed, not zoomed"
"$GV" -L "$L" menu r || fail "menu rearrange"
"$GV" -L "$L" menu m || fail "menu move"
before=$("$GV" -L "$L" list-windows | grep ':2:')
# Default snap is the centred float, so there is room to move.
"$GV" -L "$L" menu right 5 || fail "menu move right"
"$GV" -L "$L" menu enter || fail "menu enter"
after=$("$GV" -L "$L" list-windows | grep ':2:')
[ "$before" != "$after" ] || {
	echo "before $before"
	echo "after  $after"
	fail "move should change window 2 geometry"
}

# Closing a non-last window (here window 1, the first) must NOT quit odin-seat:
# the other windows stay and the server lives on.
"$GV" -L "$L" menu o || fail "menu open 3"
"$GV" -L "$L" menu 1 || fail "focus window 1"
"$GV" -L "$L" menu c || fail "close window 1"
"$GV" -L "$L" list-sessions | grep -q '132x43' ||
	fail "closing a non-last window must not tear down the server"
left=$("$GV" -L "$L" list-windows | grep -cE '^0:[0-9]+:')
[ "$left" -ge 2 ] || fail "other windows must remain (got $left)"
if "$GV" -L "$L" list-windows | grep -q '^0:1:'; then
	fail "window 1 should be gone after closing it"
fi

# Windows start where the client is, not in the daemon's "/".
tmp=$(mktemp -d)
real=$(cd "$tmp" && pwd -P)   # macOS /var is a symlink
(cd "$tmp" && "$GV" -L "$L" new-session -d -s cwd -- \
    sh -c 'pwd > pwd.out; sleep 30') || fail "cwd session"
i=0
while [ ! -s "$tmp/pwd.out" ] && [ "$i" -lt 50 ]; do
	sleep 0.1
	i=$((i + 1))
done
got=$(cat "$tmp/pwd.out" 2>/dev/null || true)
[ "$got" = "$real" ] || fail "window cwd is '$got', want '$real'"
rm -rf "$tmp"

"$GV" -L "$L" new-session -d -x 200 -y 60 -s wide -- sleep 30 ||
	fail "wide session"

"$GV" -L "$L" list-windows -s wide >/dev/null || fail "list-windows wide"
# The desktop is 200x60; the framed window scales from it.
"$GV" -L "$L" list-sessions | grep -q 'wide: 200x60' || fail "expected 200x60 desktop"

# --- new-window and mouse-driven move/resize -------------------------------
"$GV" -L "$L" new-session -d -x 100 -y 40 -s mouse -- sleep 60 ||
	fail "mouse session"
"$GV" -L "$L" new-window -s mouse -- sleep 60 || fail "new-window"
"$GV" -L "$L" list-windows -s mouse | grep -q ':2:' ||
	fail "new-window should open window 2"

# Move window 2 by dragging its top-left frame cell.
geom() {
	"$GV" -L "$L" list-windows -s mouse | grep ":$1:" | \
	    sed -n 's/.*(\([0-9]*\)x\([0-9]*\)) \(-\{0,1\}[0-9]*\),\(-\{0,1\}[0-9]*\) z=.*/\1 \2 \3 \4/p'
}
set -- $(geom 2)
c=$1; r=$2; x=$3; y=$4
px=$((x + 1)); py=$((y + 1))			# 1-based frame top-left
"$GV" -L "$L" mouse -s mouse press $px $py || fail "mouse press"
"$GV" -L "$L" mouse -s mouse drag $((px + 7)) $((py + 3)) || fail "mouse drag"
"$GV" -L "$L" mouse -s mouse release $((px + 7)) $((py + 3)) || fail "mouse release"
set -- $(geom 2)
[ "$3" = "$((x + 7))" ] && [ "$4" = "$((y + 3))" ] ||
	fail "drag should move window 2 to $((x+7)),$((y+3)) (got $3,$4)"

# Resize window 2 by dragging its bottom-right corner inward.
set -- $(geom 2)
c=$1; r=$2; x=$3; y=$4
cx=$((x + c + 2)); cy=$((y + r + 2))		# 1-based frame bottom-right
"$GV" -L "$L" mouse -s mouse press $cx $cy || fail "corner press"
"$GV" -L "$L" mouse -s mouse drag $((cx - 10)) $((cy - 5)) || fail "corner drag"
"$GV" -L "$L" mouse -s mouse release $((cx - 10)) $((cy - 5)) || fail "corner release"
set -- $(geom 2)
[ "$1" = "$((c - 10))" ] && [ "$2" = "$((r - 5))" ] ||
	fail "corner drag should resize window 2 to $((c-10))x$((r-5)) (got $1x$2)"

# Q asks before it quits.
"$GV" -L "$L" menu q || fail "menu q"
"$GV" -L "$L" list-sessions | grep -q '132x43' || fail "q must ask, not quit"
"$GV" -L "$L" menu n || fail "menu n"
"$GV" -L "$L" list-sessions | grep -q '132x43' || fail "n cancels the quit"

# Alt-U picker opens and takes navigation without error.
"$GV" -L "$L" menu u || fail "menu u"
"$GV" -L "$L" menu right || fail "picker right"
"$GV" -L "$L" menu enter || fail "picker enter (type)"
"$GV" -L "$L" menu esc || fail "picker esc"

"$GV" -L "$L" kill-server || fail "kill-server"
# A second kill should fail to connect (server gone). Start nothing.
if "$GV" -L "$L" list-sessions 2>/dev/null | grep -q .; then
	# server auto-starts on connect; kill it — that's OK, but the
	# previous session must be gone.
	out=$("$GV" -L "$L" list-sessions || true)
	echo "$out" | grep -q '132x43' && fail "session survived kill-server"
	"$GV" -L "$L" kill-server 2>/dev/null || true
fi

echo "regress/skeleton.sh: ok"
