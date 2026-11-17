#!/usr/bin/env python3
"""End-to-end checks through a real odin-seat client on a pty.

A fake host (this script) answers the queries the client sends at
bootstrap the way a terminal would, then watches what the client paints.
`kitty_host` chooses whether the emulated terminal speaks kitty graphics:
Ghostty/kitty do, a plain xterm does not, and odin-seat must degrade to text on
the latter and re-light graphics when a kitty terminal (re)attaches.
"""
import base64
import fcntl
import os
import shutil
import pty
import re
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time

GV = os.environ.get("GV", os.path.join(os.path.dirname(__file__), "..", "odin-seat"))
L = "e2e%d" % os.getpid()
COLS, ROWS, CW, CH = 100, 30, 10, 20

# What a host answers at bootstrap. The kitty graphics query reply is what
# marks the host kitty-capable; a text host omits it (kitty_host=False).
SIZE_REPLIES = (
    (b"\x1b[16t", b"\x1b[6;%d;%dt" % (CH, CW)),
    (b"\x1b[18t", b"\x1b[8;%d;%dt" % (ROWS, COLS)),
    (b"\x1b[?2048h", b"\x1b[48;%d;%d;%d;%dt" % (ROWS, COLS, ROWS * CH, COLS * CW)),
    (b"\x1b[?u", b"\x1b[?0u"),
)
DA1 = b"\x1b[?62;22c"                        # answered last, ends bootstrap
DA1_SIXEL = b"\x1b[?62;4;22c"                # DA1 with the sixel bit (4)
KITTY_OK = b"\x1b_Gi=31;OK\x1b\\"            # a kitty host answers the a=q query
PH = "\U0010EEEE".encode()                   # a Unicode-placeholder cell
XTVERSION = {"kitty": b"\x1bP>|kitty(0.35.2)\x1b\\",
             "sixel": b"\x1bP>|foot(1.16.2)\x1b\\"}

failed = 0


def fail(msg):
    global failed
    failed += 1
    print("FAIL " + msg, file=sys.stderr)


def gob(*args):
    return subprocess.run([GV, "-L", L] + list(args), capture_output=True, text=True)


def visible(out):
    """Strip escape sequences, leaving the text the host would show."""
    s = re.sub(rb"\x1b_[^\x1b]*\x1b\\", b"", out)       # APC
    s = re.sub(rb"\x1b\][^\x07\x1b]*(\x07|\x1b\\)", b"", s)  # OSC
    s = re.sub(rb"\x1bP[^\x1b]*\x1b\\", b"", s)         # DCS
    s = re.sub(rb"\x1b\[[0-9;?<>=]*[A-Za-z@`]", b"", s)  # CSI
    s = re.sub(rb"\x1b.", b"", s)
    return s


def answer(fd, data, host):
    """Reply to bootstrap queries as a host would: sizes always; the kitty
    graphics query only on a kitty host; DA1 (with the sixel bit on a sixel
    host) ends the bootstrap. host is "kitty", "sixel", or "text"."""
    for q, a in SIZE_REPLIES:
        for _ in range(data.count(q)):
            os.write(fd, a)
    if host == "kitty" and b"a=q" in data and b"\x1b_G" in data:
        os.write(fd, KITTY_OK)
    if b"\x1b[>q" in data and host != "text":      # XTVERSION: who we are
        os.write(fd, XTVERSION[host])
    if host == "kitty" and b"\x1b]72;t=q" in data:  # drag and drop: yes
        os.write(fd, b"\x1b]72;t=q:i=gv;\x1b\\")
    if host == "kitty" and b"\x1b[> q" in data:     # extra cursors: yes
        os.write(fd, b"\x1b[>1;2;3;29;30;40;100;101 q")
    da = DA1_SIXEL if host == "sixel" else DA1
    for _ in range(data.count(b"\x1b[c")):
        os.write(fd, da)


def run_client(args, seconds, actions=(), host="kitty"):
    """Run a odin-seat client under a pty for `seconds`; return what it
    painted. `host` emulates the terminal: "kitty", "sixel", or "text".
    `actions` are (at_seconds, fn(fd)) pairs run once their time comes."""
    todo = sorted(actions, key=lambda a: a[0])
    pid, fd = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM="xterm-ghostty")
        os.execvpe(GV, [GV, "-L", L] + args, env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ,
                struct.pack("HHHH", ROWS, COLS, COLS * CW, ROWS * CH))
    out = b""
    start = time.time()
    deadline = start + seconds
    while time.time() < deadline:
        while todo and time.time() >= start + todo[0][0]:
            todo.pop(0)[1](fd)
        r, _, _ = select.select([fd], [], [], 0.05)
        if fd not in r:
            continue
        try:
            data = os.read(fd, 65536)
        except OSError:
            break
        if not data:
            break
        out += data
        answer(fd, data, host)
    return pid, fd, out


def finish(pid, fd):
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass


# pyenv's startup rehash waits up to 60 s for a stale lock; cap that so it
# cannot masquerade as a odin-seat stall.
os.environ["PYENV_REHASH_TIMEOUT"] = "1"
# These image tests assert the inline (direct) transmit bytes; the emit
# medium (default shared memory) is covered by the term unit test.
os.environ["ODIN_SEAT_KITTY_MEDIUM"] = "direct"

# 1. A first window's output shows up without a keypress.
gob("kill-server")
pid, fd, out = run_client(
    ["new-session", "--", "sh", "-c", "echo READY_MARK; sleep 30"], 3.0)
if b"READY_MARK" not in visible(out):
    fail("first window's output is painted without input (%d bytes)" % len(out))
    print(out[:1500], file=sys.stderr)
finish(pid, fd)
gob("kill-server")

# 2. The user's interactive shell: its prompt appears without a keypress.
shell = os.environ.get("SHELL", "/bin/zsh")
pid, fd, out = run_client(["new-session", "--", shell, "-i"], 5.0)
vis = visible(out)
if b"%" not in vis and b"$" not in vis and b"@" not in vis:
    fail("an interactive %s prompt is painted without input" % shell)
    print(vis[-800:], file=sys.stderr)
finish(pid, fd)
gob("kill-server")

# 3. The Emacs previewer's kitty flow, on a kitty host.
PNG = (b"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhf"
       b"DwAChwGA60e6kgAAAABJRU5ErkJggg==")  # a 1x1 PNG
# Its one pixel, translucent blue, re-packed: four bytes of RGBA beat the PNG.
PACKED = b"f=32,s=1,v=1,q=2,m=0;AAD/fw=="
script = (
    "printf '\\033_Ga=t,q=2,f=100,i=31,m=0;" + PNG.decode() + "\\033\\\\'; "
    "printf '\\0337\\033[3;3H\\033_Gq=2,a=p,i=31,p=1,c=4,r=2\\033\\\\\\0338'; "
    "printf '\\033[3;3H          '; "            # a redraw of the text beneath
    "printf '\\033[8;1HTEXT_AFTER\\n'; sleep 30"
)
pid, fd, out = run_client(["new-session", "--", "bash", "-c", script], 4.0)
vis = visible(out)
m = re.search(rb"\x1b_Ga=t,i=(\d+)," + re.escape(PACKED) + rb"\x1b\\", out)
if not m:
    fail("the image is transmitted to the host once, in its tightest form")
else:
    hid = m.group(1)
    if out.count(b"\x1b_Ga=t,i=" + hid) != 1:
        fail("the image is not re-sent while nothing changes")
    if b"\x1b_Ga=p,U=1,i=" + hid + b",p=1,c=4,r=2,q=2\x1b\\" not in out:
        fail("the host placement is c/r-sized (4x2), not the pixel size")
if out.count(PH) < 8:
    fail("8 placeholder cells painted for a 4x2 placement (got %d)"
         % out.count(PH))
if b"TEXT_AFTER" not in vis:
    fail("text after the image is painted")
finish(pid, fd)
gob("kill-server")

# A redraw must resend unchanged text and images, repeatedly. Exercise
# the real menu key (Ctrl-\\ fallback), both letter cases, and the script
# command. Nothing is sent to the child to make it repaint itself.
for redraw_host in ("kitty", "sixel", "text"):
    redraw_script_results = []

    def script_redraw(fd):
        redraw_script_results.append(gob("menu", "f").returncode)

    pid, fd, out = run_client(
        ["new-session", "--", "bash", "-c", script], 3.5, host=redraw_host,
        actions=[(1.2, lambda fd: os.write(fd, b"\x1cf")),
                 (1.8, lambda fd: os.write(fd, b"\x1cF")),
                 (2.4, script_redraw)])
    if visible(out).count(b"TEXT_AFTER") < 4:
        fail("repeated menu redraws resend unchanged text on a " + redraw_host + " host")
    if out.count(b"\x1b[0m\x1b[H\x1b[2J") < 4:
        fail("each redraw completely clears the " + redraw_host + " host screen")
    if redraw_host == "kitty" and out.count(PACKED) < 4:
        fail("menu redraw retransmits cached kitty images")
    if redraw_host == "sixel" and out.count(b"\x1bPq") < 4:
        fail("menu redraw re-emits sixel images")
    if redraw_script_results != [0]:
        fail("scripted screen redraw succeeds")
    finish(pid, fd)
    gob("kill-server")

# 4. A host WITHOUT kitty graphics: an image the program pushes is painted
# blank, never as image cells or an APC transmit; ordinary text is fine.
script = (
    "printf '\\033_Ga=T,f=32,s=32,v=32;QUJD\\033\\\\'; "
    "printf 'PLAINTEXT\\n'; sleep 30"
)
pid, fd, out = run_client(["new-session", "--", "bash", "-c", script], 3.0, host="text")
vis = visible(out)
if PH in out:
    fail("a text host is never sent placeholder image cells")
if b"\x1b_Ga=t" in out:
    fail("a text host is never sent a kitty transmit")
if b"PLAINTEXT" not in vis:
    fail("ordinary text still paints on a text host")
finish(pid, fd)
gob("kill-server")

# 5. Reconnect that changes status. A kitty client starts a session whose
# program draws an image and keeps running. A text client attached to the
# same session is painted blank; a kitty client attached next is re-sent
# the stored image. This is the per-client gate surviving a status change.
gob("new-session", "-s", "recon", "-d", "-x", "80", "-y", "24", "--", "bash",
   "-c", "printf '\\033[1;1H\\033_Ga=T,f=32,s=48,v=48,C=1;QUJD\\033\\\\'; "
   "printf '\\033[12;1HHELLO\\n'; sleep 60")   # image top-left, text well below
time.sleep(0.4)
# A text client attaches: blanks, no image transmit or cells, sees the text.
pidt, fdt, outt = run_client(["attach", "-s", "recon"], 2.0, host="text")
if b"\x1b_Ga=t" in outt or PH in outt:
    fail("a text client reconnecting gets no image, just blanks")
if b"HELLO" not in visible(outt):
    fail("the text client still sees the program's text after reconnect")
finish(pidt, fdt)
# A kitty client attaches to the same running session: the image returns.
pidk, fdk, outk = run_client(["attach", "-s", "recon"], 2.5, host="kitty")
if b"\x1b_Ga=t" not in outk:
    fail("a kitty client reconnecting is re-sent the stored image")
if PH not in outk:
    fail("a kitty client reconnecting gets placeholder cells")
finish(pidk, fdk)
gob("kill-server")

# 7. A kitty-speaking child on a SIXEL host: the child transmits a kitty
# image (PNG) and places it with c/r; the sixel client is sent a sixel DCS
# (ESC P q), never a kitty transmit, and rasterised at the cell grid.
PNG1 = ("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhf"
        "DwAChwGA60e6kgAAAABJRU5ErkJggg==")
script = (
    "printf '\\033_Ga=t,q=2,f=100,i=42,m=0;" + PNG1 + "\\033\\\\'; "
    "printf '\\0337\\033[2;2H\\033_Ga=p,i=42,p=1,c=6,r=3,q=2\\033\\\\\\0338'; "
    "printf '\\033[10;1HSIXOUT\\n'; sleep 30"
)
pid, fd, out = run_client(["new-session", "--", "bash", "-c", script], 4.0,
                          host="sixel")
vis = visible(out)
if b"\x1b_Ga=t" in out:
    fail("a sixel host is never sent a kitty transmit")
if b"\x1bPq" not in out:
    fail("a kitty child on a sixel host is downsampled to a sixel DCS")
if b"SIXOUT" not in vis:
    fail("text alongside the image still paints on a sixel host")
finish(pid, fd)
gob("kill-server")

# 8. A sixel-drawing child on a sixel host: its sixel is decoded and
# re-emitted as sixel (positioned/clipped by odin-seat), so the host
# still sees one protocol. A red 4-wide sixel band.
script = (
    "printf '\\033[3;3H\\033Pq#0;2;100;0;0#0!4~$-\\033\\\\'; "
    "printf '\\033[10;1HSIXIN\\n'; sleep 30"
)
pid, fd, out = run_client(["new-session", "--", "bash", "-c", script], 4.0,
                          host="sixel")
vis = visible(out)
if b"\x1bPq" not in out:
    fail("a sixel child on a sixel host yields a sixel DCS to the host")
if b"SIXIN" not in vis:
    fail("text alongside a sixel still paints")
finish(pid, fd)
gob("kill-server")

# 9. A sixel-drawing child on a KITTY host: its sixel is decoded and
# re-emitted as kitty graphics (transmit + placeholder cells), never sixel.
script = (
    "printf '\\033[3;3H\\033Pq#0;2;0;100;0#0!8~$-\\033\\\\'; "
    "printf '\\033[10;1HTOKITTY\\n'; sleep 30"
)
pid, fd, out = run_client(["new-session", "--", "bash", "-c", script], 4.0,
                          host="kitty")
vis = visible(out)
if b"\x1b_Ga=t" not in out:
    fail("a sixel child on a kitty host is converted to a kitty transmit")
if PH not in out:
    fail("the converted image shows as placeholder cells on a kitty host")
if b"\x1bPq" in out:
    fail("a kitty host is never sent sixel")
if b"TOKITTY" not in vis:
    fail("text alongside a converted sixel still paints")
finish(pid, fd)
gob("kill-server")

# 10. Native wheel scrolling of the focused window. An alternate-screen
# app with no mouse tracking gets arrow keys on the wheel (alternate
# scroll); a primary-screen app with no scrollback gets nothing. The echo
# program prints each received byte as <hh> so we can see what arrived.
ECHO = ("import os, tty, sys\n"
        "tty.setraw(sys.stdin.fileno())\n"   # a pager reads bytes raw, so arrows arrive
        "%s"
        "while True:\n"
        " b=os.read(0,1)\n"
        " if not b: break\n"
        " os.write(1,b'<%%02x>'%%b[0])\n")
alt = tempfile.NamedTemporaryFile("w", suffix=".py", delete=False)
alt.write(ECHO % "os.write(1,b'\\033[?1049h')\n")     # enter the alternate screen
alt.close()
pid, fd, out = run_client(["new-session", "-s", "scroll", "--", "python3", alt.name],
                          3.5, host="kitty",
                          actions=[(1.3, lambda fd: gob("mouse", "-s", "scroll", "wheelup", "6", "6")),
                                   (2.0, lambda fd: gob("mouse", "-s", "scroll", "wheeldown", "6", "6"))])
vis = visible(out)
if b"<1b><5b><41>" not in vis:
    fail("wheel up on an alt-screen app sends Up arrows (alternate scroll)")
if b"<1b><5b><42>" not in vis:
    fail("wheel down on an alt-screen app sends Down arrows")
finish(pid, fd)
os.unlink(alt.name)
gob("kill-server")

pri = tempfile.NamedTemporaryFile("w", suffix=".py", delete=False)
pri.write(ECHO % "")                                  # stays on the primary screen
pri.close()
pid, fd, out = run_client(["new-session", "-s", "noscroll", "--", "python3", pri.name],
                          3.0, host="kitty",
                          actions=[(1.3, lambda fd: gob("mouse", "-s", "noscroll", "wheelup", "6", "6"))])
if b"<1b><5b>" in visible(out):
    fail("wheel on a primary-screen app scrolls nothing (no scrollback, no arrows)")
finish(pid, fd)
os.unlink(pri.name)
gob("kill-server")

# 11. Native scrolling at a shell prompt: a primary-screen program prints
# more lines than fit, so early lines enter scrollback. The wheel scrolls
# back to reveal them, and returns to the live bottom.
NAME = "sb"
gob("new-session", "-s", NAME, "-d", "-x", "100", "-y", "30", "--", "bash",
    "-c", "for i in $(seq 1 100); do printf 'L%03d\\n' \"$i\"; done; sleep 60")
time.sleep(0.7)

def snapshot():
    pid, fd, out = run_client(["attach", "-s", NAME], 1.3, host="kitty")
    v = visible(out)
    finish(pid, fd)
    return v

base = snapshot()
if b"L100" not in base:
    fail("the live bottom shows the last line printed")
if b"L005" in base:
    fail("an early line has scrolled off the live screen")

for _ in range(30):                     # wheel up, three lines a notch
    gob("mouse", "-s", NAME, "wheelup", "6", "6")
back = snapshot()
if b"L005" not in back:
    fail("wheel up scrolls back into history and reveals an early line")

for _ in range(30):                     # wheel back down to the bottom
    gob("mouse", "-s", NAME, "wheeldown", "6", "6")
front = snapshot()
if b"L100" not in front:
    fail("wheel down returns to the live bottom")
if b"L005" in front:
    fail("back at the bottom, the early line is gone again")
gob("kill-server")

# 12. OSC 66 text sizing. A program emits a scaled run; a host that speaks
# text sizing (kitty-graphics, by proxy) is sent the OSC 66 sequence, and
# a plain host gets the text unscaled. Either way the text is present.
script = ("printf '\\033[1;1HT:\\033]66;s=2;BIG\\007'; "
          "printf '\\033[6;1Hafter\\n'; sleep 30")
pid, fd, out = run_client(["new-session", "--", "bash", "-c", script], 3.0,
                          host="kitty")
if b"\x1b]66;s=2;BIG" not in out:
    fail("a text-sizing host is sent the OSC 66 run")
if b"after" not in visible(out):
    fail("text after a scaled run still paints (kitty host)")
finish(pid, fd)
gob("kill-server")

pid, fd, out = run_client(["new-session", "--", "bash", "-c", script], 3.0,
                          host="text")
if b"\x1b]66" in out:
    fail("a plain host is never sent OSC 66")
if b"BIG" not in visible(out):
    fail("a plain host still gets the scaled text, unscaled")
finish(pid, fd)
gob("kill-server")

# 13. A font-size change is a resize with the SAME cols/rows but different
# cell pixels. Every child must still get SIGWINCH with the new pixel size,
# so its kitty/sixel machinery can re-query the cell size. The child prints
# its pixels-per-cell on each SIGWINCH.
win = tempfile.NamedTemporaryFile("w", suffix=".py", delete=False)
win.write("import signal, os, fcntl, termios, struct\n"
          "def rep(*a):\n"
          " ws=fcntl.ioctl(0,termios.TIOCGWINSZ,b'\\0'*8)\n"
          " r,c,xp,yp=struct.unpack('HHHH',ws)\n"
          " os.write(1,b'CELLW=%d\\n'%((xp//c) if c else 0))\n"
          "signal.signal(signal.SIGWINCH,rep)\n"
          "rep()\n"
          "while True: signal.pause()\n")
win.close()
pid, fd, out = run_client(
    ["new-session", "--", "python3", win.name], 3.5, host="kitty",
    actions=[(1.4, lambda fd: fcntl.ioctl(     # font grows: cols/rows same, cell 10->16
        fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, COLS * 16, ROWS * 32)))])
vis = visible(out)
if b"CELLW=10" not in vis:
    fail("the child sees the initial cell width")
if b"CELLW=16" not in vis:
    fail("a font-size change (same cols/rows) reaches the child with new cell pixels")
finish(pid, fd)
os.unlink(win.name)
gob("kill-server")

# 15. Drag-and-drop. The host sends a drop as a bracketed paste (the client
# enables DECSET 2004 on the host). odin-seat routes it to the focused
# window in that window's own paste mode: wrapped when the program enabled
# bracketed paste, the bare path when it did not. The echo program prints
# each received byte as <hh>.
DROP = b"\x1b[200~/tmp/drop.txt\x1b[201~"
def echo_prog(enable_2004):
    f = tempfile.NamedTemporaryFile("w", suffix=".py", delete=False)
    f.write("import os, tty, sys\n"
            "tty.setraw(sys.stdin.fileno())\n"
            + ("os.write(1,b'\\033[?2004h')\n" if enable_2004 else "")
            + "while True:\n"
              " b=os.read(0,1)\n"
              " if not b: break\n"
              " os.write(1,b'<%02x>'%b[0])\n")
    f.close()
    return f.name

# A program that accepts bracketed paste keeps the wrappers.
prog = echo_prog(True)
pid, fd, out = run_client(["new-session", "--", "python3", prog], 3.0,
                          host="kitty",
                          actions=[(1.4, lambda fd: os.write(fd, DROP))])
vis = visible(out)
if b"<1b><5b><32><30><30><7e>" not in vis:      # ESC [ 2 0 0 ~
    fail("a bracketed-paste program receives the drop's start marker")
if b"<1b><5b><32><30><31><7e>" not in vis:      # ESC [ 2 0 1 ~
    fail("a bracketed-paste program receives the drop's end marker")
if b"<2f><74><6d><70>" not in vis:              # /tmp
    fail("the dropped path reaches the window")
finish(pid, fd)
os.unlink(prog)
gob("kill-server")

# A program that did not enable bracketed paste gets the bare path.
prog = echo_prog(False)
pid, fd, out = run_client(["new-session", "--", "python3", prog], 3.0,
                          host="kitty",
                          actions=[(1.4, lambda fd: os.write(fd, DROP))])
vis = visible(out)
if b"<1b><5b><32><30><30>" in vis:
    fail("a non-bracketed program is not sent the paste markers")
if b"<2f><74><6d><70>" not in vis:
    fail("the dropped path still reaches a non-bracketed window")
finish(pid, fd)
os.unlink(prog)
gob("kill-server")

# 15b. A forwarded OSC's reply is routed back to the window that asked,
# not merely to whoever is focused. Window 2 (opened later, so focused and
# on top) asks nothing; window 1 issues a kitty clipboard read (OSC 5522)
# and a pointer query (OSC 22). The host's answers must land on window 1's
# pty; the program there prints a token per reply, and raising window 1 at
# the end makes them visible.
ASKER = r"""import os, tty, sys, time
tty.setraw(sys.stdin.fileno())
time.sleep(0.8)
os.write(1, b'\033]5522;type=read:id=r1;dGV4dC9wbGFpbg==\033\\')
os.write(1, b'\033]22;?\033\\')
buf = b''
while True:
    b = os.read(0, 1)
    if not b:
        break
    buf += b
    if buf.endswith(b'\033\\'):
        if b']5522;' in buf:
            os.write(1, b'\r\nGOT5522\r\n')
        if b']22;' in buf:
            os.write(1, b'\r\nGOT22\r\n')
        buf = b''
"""
f = tempfile.NamedTemporaryFile("w", suffix=".py", delete=False)
f.write(ASKER)
f.close()
prog = f.name
REPLY_5522 = b"\x1b]5522;type=read:status=OK:id=r1;\x1b\\"
REPLY_22 = b"\x1b]22;default\x1b\\"
pid, fd, out = run_client(["new-session", "--", "python3", prog], 3.4,
                          host="kitty",
                          actions=[(0.7, lambda fd: gob("new-window", "--",
                                                        "sleep", "30")),
                                   (2.2, lambda fd: os.write(fd, REPLY_5522 +
                                                             REPLY_22)),
                                   (3.0, lambda fd: gob("menu", "1"))])
if b"\x1b]5522;type=read:id=r1;" not in out:
    fail("a window's OSC 5522 read is forwarded to the host")
if b"\x1b]22;?\x1b\\" not in out:
    fail("a window's OSC 22 query is forwarded to the host")
finish(pid, fd)
# A fresh client is painted a full frame of the raised window, so the tokens
# are whole rather than split across the diff frames that painted them.
pid, fd, out = run_client(["attach"], 1.3, host="kitty")
vis = visible(out)
if b"GOT5522" not in vis:
    fail("the OSC 5522 reply is routed to the window that asked")
if b"GOT22" not in vis:
    fail("the OSC 22 reply is routed to the window that asked")
finish(pid, fd)
os.unlink(prog)
gob("kill-server")

# 15c. The client learns the terminal's name from XTVERSION at bootstrap
# and tells the server, which shows it in list-clients. A text host that
# does not answer is "?".
def clients_line(host):
    seen = {}
    pid, fd, out = run_client(["new-session", "--", "sleep", "30"], 1.6,
                              host=host,
                              actions=[(1.0, lambda fd: seen.__setitem__(
                                  "lc", gob("list-clients").stdout))])
    finish(pid, fd)
    gob("kill-server")
    return seen.get("lc", "")

lc = clients_line("kitty")
if "host kitty(0.35.2)" not in lc or "kitty-gfx" not in lc:
    fail("list-clients names a kitty host (got %r)" % lc)
lc = clients_line("sixel")
if "host foot(1.16.2)" not in lc or " sixel" not in lc:
    fail("list-clients names a sixel host (got %r)" % lc)
lc = clients_line("text")
if "host ?" not in lc:
    fail("an unnamed host shows as ? (got %r)" % lc)

# 15d. File transfer. On a terminal that cannot take files (a text host)
# odin-seat is the endpoint: a kitty OSC 5113 send raises a yes/no box,
# Menu y approves it, the program gets the protocol's replies and the
# file lands in the download directory. On a kitty host the request is
# forwarded and the host's reply comes back to the program.
XFER_DIR = tempfile.mkdtemp(prefix="gvdl")
os.environ["ODIN_SEAT_DOWNLOAD_DIR"] = XFER_DIR
SENDER = r"""import os, tty, sys
tty.setraw(sys.stdin.fileno())
def osc(s): os.write(1, b'\033]5113;' + s + b'\033\\')
def wait_status():
    buf = b''
    while True:
        b = os.read(0, 1)
        if not b: sys.exit(0)
        buf += b
        if buf.endswith(b'\033\\'):
            if b']5113;' in buf:
                return buf
            buf = b''
osc(b'ac=send;id=e1')
r = wait_status()
if b'st=T0s=' in r: os.write(1, b'\r\nGOTOK\r\n')
else: os.write(1, b'\r\nREFUSED\r\n'); sys.exit(0)
osc(b'ac=file;id=e1;fid=f1;n=ZTJlLnR4dA==;sz=6')     # e2e.txt
wait_status()                                       # STARTED
osc(b'ac=end_data;id=e1;fid=f1;d=aGVsbG8K')          # "hello\n"
r = wait_status()
if b'st=T0s=' in r: os.write(1, b'\r\nSAVED\r\n')
osc(b'ac=finish;id=e1')
while True:
    if not os.read(0, 1): break
"""
f = tempfile.NamedTemporaryFile("w", suffix=".py", delete=False)
f.write(SENDER)
f.close()
prog = f.name

# A text host: we are the endpoint.
pid, fd, out = run_client(["new-session", "--", "python3", prog], 3.2,
                          host="text",
                          actions=[(1.6, lambda fd: gob("menu", "y"))])
vis = visible(out)
if b"wants to send you files" not in vis:
    fail("a kitty send on a text host raises the yes/no box")
finish(pid, fd)
pid, fd, out = run_client(["attach"], 1.3, host="text")
vis = visible(out)
if b"GOTOK" not in vis:
    fail("menu y approves the transfer and the program sees st=OK")
if b"SAVED" not in vis:
    fail("the file's end_data is acknowledged with OK")
finish(pid, fd)
gob("kill-server")
try:
    saved = open(os.path.join(XFER_DIR, "e2e.txt"), "rb").read()
except OSError:
    saved = None
if saved != b"hello\n":
    fail("the file lands in the download directory (got %r)" % (saved,))

# A kitty host: forwarded verbatim, its reply routed back.
def kitty_host_reply(fd):
    os.write(fd, b"\x1b]5113;ac=status;id=e1;st=T0s=\x1b\\")
pid, fd, out = run_client(["new-session", "--", "python3", prog], 3.0,
                          host="kitty",
                          actions=[(1.6, kitty_host_reply)])
if b"\x1b]5113;ac=send;id=e1\x1b\\" not in out:
    fail("on a kitty host the send is forwarded to the terminal")
if b"wants to send you files" in visible(out):
    fail("a forwarded transfer asks nothing here")
finish(pid, fd)
pid, fd, out = run_client(["attach"], 1.3, host="kitty")
if b"GOTOK" not in visible(out):
    fail("the kitty host's status reply reaches the program")
finish(pid, fd)
gob("kill-server")
os.unlink(prog)
shutil.rmtree(XFER_DIR, ignore_errors=True)

# 15e. The kitty keyboard protocol per window. The host reports keys as
# CSI u; a window that pushed flags gets them encoded for those flags, a
# window that pushed none gets legacy bytes. The program prints a token
# per sequence it recognises.
KEYPROG = r"""import os, tty, sys
tty.setraw(sys.stdin.fileno())
if len(sys.argv) > 1 and sys.argv[1] == "push":
    os.write(1, b'\033[>1u')
if len(sys.argv) > 1 and sys.argv[1] == "mouse":
    os.write(1, b'\033[?1003h\033[?1016h')
buf = b''
while True:
    b = os.read(0, 1)
    if not b:
        break
    buf += b
    if buf == b'\033[97;5u':
        os.write(1, b'\r\nCSIU-CTRL-A\r\n'); buf = b''
    elif buf == b'\001':
        os.write(1, b'\r\nLEGACY-CTRL-A\r\n'); buf = b''
    elif buf.startswith(b'\033[<35;') and buf.endswith(b'M'):
        os.write(1, b'\r\nHOVER\r\n'); buf = b''
    elif buf.startswith(b'\033[<288;') and buf.endswith(b'M'):
        os.write(1, b'\r\nLEAVE\r\n'); buf = b''
    elif len(buf) > 16:
        buf = b''
"""
f = tempfile.NamedTemporaryFile("w", suffix=".py", delete=False)
f.write(KEYPROG)
f.close()
prog = f.name
CTRL_A = b"\x1b[97;5u"                       # what a kitty-keyboard host sends

def keyrun(mode, keys, seconds=2.6):
    pid, fd, out = run_client(["new-session", "--", "python3", prog, mode],
                              seconds, host="kitty",
                              actions=[(1.4, lambda fd: os.write(fd, keys))])
    finish(pid, fd)
    pid, fd, out = run_client(["attach"], 1.3, host="kitty")
    finish(pid, fd)
    gob("kill-server")
    return visible(out)

vis = keyrun("push", CTRL_A)
if b"CSIU-CTRL-A" not in vis:
    fail("a window that pushed flags gets ctrl-a as CSI u")
vis = keyrun("plain", CTRL_A)
if b"LEGACY-CTRL-A" not in vis:
    fail("a window with no flags gets ctrl-a as the legacy byte")

# Hover: the host reports motion (mode 1003); a window tracking any motion
# in pixel mode gets it, and kitty's leave event when the pointer leaves.
vis = keyrun("mouse", b"\x1b[<35;10;5M" + b"\x1b[<288;0;0M")
if b"HOVER" not in vis:
    fail("motion with no button reaches a window in mode 1003")
if b"LEAVE" not in vis:
    fail("the host's leave event reaches the hovered window as button 288")
os.unlink(prog)

# 15f. Drag and drop (OSC 72) is routed by where the drag is. A window
# whose program does not take drops gets the dropped files as a paste of
# their paths: odin-seat accepts on its behalf, asks the host for the
# text/uri-list and pastes it. A program that said t=a gets the protocol,
# translated to its own cells, and its answers reach the host.
DROPPROG = r"""import os, tty, sys
tty.setraw(sys.stdin.fileno())
os.write(1, b'\033[?2004h')
buf = b''
while True:
    b = os.read(0, 1)
    if not b:
        break
    buf += b
    if buf.endswith(b'\033[201~'):
        s = buf.find(b'\033[200~')
        os.write(1, b'\r\nDROPPED:' + buf[s + 6:-6] + b'\r\n')
        buf = b''
"""
DNDPROG = r"""import os, tty, sys, base64
tty.setraw(sys.stdin.fileno())
os.write(1, b'\033]72;t=a;text/plain\033\\')
buf = b''
while True:
    b = os.read(0, 1)
    if not b:
        break
    buf += b
    if not buf.endswith(b'\033\\'):
        continue
    if buf.startswith(b'\033]72;t=m:x='):
        os.write(1, b'\r\nDND-MOTION:' + buf[9:buf.index(b':X=')] + b'\r\n')
        os.write(1, b'\033]72;t=m:o=1;text/plain\033\\')
    elif buf.startswith(b'\033]72;t=M:x='):
        os.write(1, b'\r\nDND-DROP:' + buf[9:buf.index(b':X=')] + b'\r\n')
        os.write(1, b'\033]72;t=r:x=2\033\\')
    elif buf.startswith(b'\033]72;t=r:x=2:m=0;'):
        data = base64.b64decode(buf[len(b'\033]72;t=r:x=2:m=0;'):-2])
        os.write(1, b'\r\nDND-DATA:' + data + b'\r\n')
        os.write(1, b'\033]72;t=r:o=1\033\\')
    buf = b''
"""
MIMES = b"text/uri-list text/plain"
DND_MOVE = b"\x1b]72;t=m:x=5:y=3:X=55:Y=65:o=1;" + MIMES + b"\x1b\\"
DND_DROP = b"\x1b]72;t=M:x=5:y=3:X=55:Y=65:o=1;" + MIMES + b"\x1b\\"
URI_DATA = b"\x1b]72;t=r:x=1:m=0;" + base64.b64encode(
    b"file:///tmp/drop%20me.txt\r\n") + b"\x1b\\"
TEXT_DATA = b"\x1b]72;t=r:x=2:m=0;" + base64.b64encode(b"hello") + b"\x1b\\"

def dndrun(src, data):
    f = tempfile.NamedTemporaryFile("w", suffix=".py", delete=False)
    f.write(src)
    f.close()
    pid, fd, out = run_client(["new-session", "--", "python3", f.name], 3.0,
                              host="kitty",
                              actions=[(1.4, lambda fd: os.write(fd, DND_MOVE)),
                                       (1.8, lambda fd: os.write(fd, DND_DROP)),
                                       (2.3, lambda fd: os.write(fd, data))])
    finish(pid, fd)
    pid, fd, shown = run_client(["attach"], 1.3, host="kitty")
    finish(pid, fd)
    gob("kill-server")
    os.unlink(f.name)
    return out, visible(shown)

out, vis = dndrun(DROPPROG, URI_DATA)
if b"\x1b]72;t=m:o=1;text/uri-list text/plain\x1b\\" not in out:
    fail("over a window that takes no drops, odin-seat accepts the drag")
if b"\x1b]72;t=r:x=1\x1b\\" not in out:
    fail("on the drop it asks the host for the uri-list")
if b"DROPPED:'/tmp/drop me.txt'" not in vis:
    fail("the dropped file's path is pasted into that window (got %r)" % vis[-200:])
if b"\x1b]72;t=r:o=1\x1b\\" not in out:
    fail("the host is told the drop was taken")

out, vis = dndrun(DNDPROG, TEXT_DATA)
if b"\x1b]72;t=a;text/uri-list text/plain text/plain\x1b\\" not in out:
    fail("a program's accepted types are advertised to the host")
# The first window is framed at 1,1: host cell 5,3 is its cell 3,1.
if b"DND-MOTION:x=3:y=1" not in vis:
    fail("an accepting program gets the drag in its own cells (got %r)" %
         vis[vis.find(b"DND-MOTION"):vis.find(b"DND-MOTION") + 24])
if b"\x1b]72;t=m:o=1;text/plain\x1b\\" not in out:
    fail("the program's acceptance reaches the host")
if b"DND-DROP:x=3:y=1" not in vis or b"\x1b]72;t=r:x=2\x1b\\" not in out:
    fail("the drop and the program's data request are relayed")
if b"DND-DATA:hello" not in vis:
    fail("the host's data reaches the program")

# 15g. Extra cursors (kitty multiple cursors) are drawn by a host that can:
# the window's set is sent, translated to the desktop, after a clear.
pid, fd, out = run_client(["new-session", "--", "bash", "-c",
                           "printf '\\033[>2;2:4:5 q'; sleep 30"], 2.0,
                          host="kitty")
# The window's row 4, column 5 is desktop row 6, column 7 (framed at 1,1).
if b"\x1b[>0;4 q\x1b[>2;2:6:7 q" not in out:
    fail("a capable host is sent the window's extra cursors, translated")
finish(pid, fd)
gob("kill-server")

# 15h. Version safety. A display client attaching to a server that does
# not introduce itself (an older build, here simulated) leaves at once
# with a message instead of typing into the void; a current server is
# attached to silently.
os.environ["ODIN_SEAT_TEST_NOHELLO"] = "1"
gob("new-session", "-s", "old", "-d", "-x", "80", "-y", "24", "--", "sleep", "30")
del os.environ["ODIN_SEAT_TEST_NOHELLO"]
time.sleep(0.4)
pid, fd, out = run_client(["attach", "-s", "old"], 1.5, host="kitty")
finish(pid, fd)
if b"older odin-seat" not in out or b"kill-server" not in out:
    fail("a client of a server that cannot introduce itself says so and leaves")
gob("kill-server")
# A server of another protocol (here posing as protocol 1) is refused too,
# naming both versions and the socket, before a single key is typed.
os.environ["ODIN_SEAT_TEST_PROTO"] = "1"
gob("new-session", "-s", "other", "-d", "-x", "80", "-y", "24", "--", "sleep", "30")
del os.environ["ODIN_SEAT_TEST_PROTO"]
time.sleep(0.4)
pid, fd, out = run_client(["attach", "-s", "other"], 1.5, host="kitty")
finish(pid, fd)
if b"(protocol 1)" not in out or b"kill-server" not in out:
    fail("a client of a server speaking another protocol says so and leaves")
if b"\x1b[?2026h" in out:
    fail("no frame is painted from a server of another protocol")
gob("kill-server")
pid, fd, out = run_client(["new-session", "--", "sleep", "30"], 1.5, host="kitty")
finish(pid, fd)
if b"older odin-seat" in out or b"protocol" in out:
    fail("a current server is attached to without complaint")
gob("kill-server")

# 15i. On the way out (a detach here) the client clears the screen and only
# then leaves the alternate screen, as its very last bytes: a terminal with
# an alternate screen shows the saved one, and one without is left clean.
pid, fd, out = run_client(["new-session", "--", "sleep", "30"], 2.5, host="kitty",
                          actions=[(1.0, lambda fd: gob("menu", "d"))])
finish(pid, fd)
if not out.endswith(b"\x1b[2J\x1b[H\x1b[?1049l"):
    fail("the client's last bytes clear the screen and leave the alternate screen (got %r)"
         % out[-40:])
if b"\x1b]22;\x1b\\" not in out[-200:] or b"\x1b]112\x1b\\" not in out[-200:]:
    fail("the pointer shape and cursor colour are reset on the way out")
gob("kill-server")

# 15j. A black-and-white client (-m, or ODIN_SEAT_MONO=1) is painted with
# default colours and reverse video only, whatever the programs draw; a
# normal client sees the colours.
COLOURS = re.compile(rb"\x1b\[[0-9;:]*m")
def colour_params(out):
    bad = []
    for m in COLOURS.finditer(out):
        for tok in m.group(0)[2:-1].split(b";"):
            p = int(tok.split(b":")[0] or b"0") if tok.split(b":")[0].isdigit() else -1
            if p not in (0, 1, 3, 4, 7, 9, 39, 49):
                bad.append(p)
    return bad
PAINT = "printf '\\033[31;44mRED\\033[0m \\033[92mGREEN\\033[0m'; sleep 30"
pid, fd, out = run_client(["-m", "new-session", "--", "bash", "-c", PAINT], 2.0, host="text")
finish(pid, fd)
gob("kill-server")
if colour_params(out):
    fail("a -m client is painted without colours (saw SGR params %s)" % sorted(set(colour_params(out)))[:8])
if b"\xe2\x96\x91" in out:
    fail("the backdrop is plain paper on a -m client, not a stipple")
if b"RED" not in visible(out) or b"GREEN" not in visible(out):
    fail("the text itself still reaches a -m client")
if not re.search(rb"\x1b\[(?:[0-9;:]*;)?7(?:;|m)", out):
    fail("the title bar is reverse video on a -m client")
os.environ["ODIN_SEAT_MONO"] = "1"
pid, fd, out = run_client(["new-session", "--", "bash", "-c", PAINT], 2.0, host="text")
finish(pid, fd)
gob("kill-server")
del os.environ["ODIN_SEAT_MONO"]
if colour_params(out):
    fail("ODIN_SEAT_MONO=1 paints without colours")
pid, fd, out = run_client(["new-session", "--", "bash", "-c", PAINT], 2.0, host="text")
finish(pid, fd)
gob("kill-server")
if not colour_params(out):
    fail("a normal client still sees the colours")

# 15k. The config file. A backdrop glyph and a per-host section: the
# server paints the configured backdrop; a client whose terminal names
# itself "kitty" gets mono from its [host] section; and menu-key = none
# keeps the Alt tap from opening the menu.
cfgdir = tempfile.mkdtemp(prefix="gvcfg")
cfgfile = os.path.join(cfgdir, "config")
open(cfgfile, "w").write('backdrop = "."\nscrollback = 50\n[host "kitty"]\nmono = yes\n[host "foot"]\nmenu-key = none\n')
os.environ["ODIN_SEAT_CONFIG"] = cfgfile
pid, fd, out = run_client(["new-session", "--", "sleep", "30"], 1.8, host="kitty")
finish(pid, fd)
gob("kill-server")
if colour_params(out):
    fail("a [host kitty] section turns mono on for a kitty terminal")
if b"\xe2\x96\x91" in out or b"..." in out:
    fail("a mono client's backdrop is paper, neither the stipple nor the configured glyph")
pid, fd, out = run_client(["new-session", "--", "sleep", "30"], 2.4, host="sixel",
                          actions=[(1.0, lambda fd: os.write(fd, b"\x1b[57443;3u")),
                                   (1.1, lambda fd: os.write(fd, b"\x1b[57443;1:3u"))])
finish(pid, fd)
gob("kill-server")
if b"Open Window" in visible(out):
    fail("menu-key = none for the foot host keeps the Alt tap from opening the menu")
if not colour_params(out):
    fail("the foot host is not mono (its section says nothing about it)")
if b"\xe2\x96\x91" in out or out.count(b"...") < 5:
    fail("the configured backdrop glyph is painted instead of the stipple")
del os.environ["ODIN_SEAT_CONFIG"]
shutil.rmtree(cfgdir, ignore_errors=True)

# 15l. Scripting: send-keys types into a window (C escapes decoded unless
# -l), capture-window prints its text, pipe-window logs its raw output.
logdir = tempfile.mkdtemp(prefix="gvpipe")
logfile = os.path.join(logdir, "w1.log")
gob("new-session", "-s", "sk", "-d", "-x", "100", "-y", "30", "--", "bash", "--norc", "--noprofile")
time.sleep(0.5)
r = gob("pipe-window", "-s", "sk", "-w", "1", logfile)
if r.returncode != 0:
    fail("pipe-window starts a log: %s" % r.stdout.strip())
gob("send-keys", "-s", "sk", "-w", "1", "echo MARKER_$((6*7))\\n")
time.sleep(0.6)
cap = gob("capture-window", "-s", "sk", "-w", "1").stdout
if "MARKER_42" not in cap:
    fail("send-keys reaches the window and capture-window shows the result (got %r)" % cap[-200:])
gob("send-keys", "-s", "sk", "-l", "echo LITERAL\\n")
gob("send-keys", "-s", "sk", "\\n")
time.sleep(0.6)
cap = gob("capture-window", "-s", "sk").stdout
if "echo LITERAL\\n" not in cap:
    fail("send-keys -l sends the backslash-n literally (got %r)" % cap[-200:])
gob("pipe-window", "-s", "sk", "-w", "1")           # stop
gob("send-keys", "-s", "sk", "echo AFTER_STOP\\n")
time.sleep(0.5)
logged = open(logfile, "rb").read()
if b"MARKER_42" not in logged:
    fail("pipe-window logged the window's output")
if b"AFTER_STOP" in logged:
    fail("pipe-window without a path stops the log")
r = gob("capture-window", "-s", "sk", "-w", "9")
if r.returncode == 0:
    fail("capture-window of a missing window fails")
gob("kill-server")
shutil.rmtree(logdir, ignore_errors=True)

# 15m. Menu H hides the focused window (focus moves to the next one on
# top; its number or the S list brings it back); S is a filterable list;
# B broadcasts typing to every window.
gob("new-session", "-s", "hs", "-d", "-x", "100", "-y", "30", "--", "sleep", "30")
gob("new-window", "-s", "hs", "--", "sleep", "30")
gob("menu", "-s", "hs", "h")
lw = gob("list-windows", "-s", "hs").stdout
if "hs:2:" not in lw or "hidden" not in [l for l in lw.splitlines() if "hs:2:" in l][0]:
    fail("menu H hides the focused window (got %r)" % lw)
if "focused" not in [l for l in lw.splitlines() if "hs:1:" in l][0]:
    fail("after hiding, the window beneath is focused")
gob("menu", "-s", "hs", "2")
lw = gob("list-windows", "-s", "hs").stdout
l2 = [l for l in lw.splitlines() if "hs:2:" in l][0]
if "hidden" in l2 or "focused" not in l2:
    fail("its number brings a hidden window back, focused (got %r)" % lw)
gob("menu", "-s", "hs", "s"); gob("menu", "-s", "hs", "1"); gob("menu", "-s", "hs", "enter")
lw = gob("list-windows", "-s", "hs").stdout
if "focused" not in [l for l in lw.splitlines() if "hs:1:" in l][0]:
    fail("the switch list: filter 1, Enter focuses window 1 (got %r)" % lw)
gob("kill-server")

pid, fd, out = run_client(["new-session", "--", "bash", "--norc", "--noprofile"], 4.0, host="kitty",
                          actions=[(0.8, lambda fd: gob("new-window", "--", "bash", "--norc", "--noprofile")),
                                   (1.6, lambda fd: gob("menu", "s")),
                                   (2.0, lambda fd: gob("menu", "esc")),
                                   (2.2, lambda fd: gob("menu", "b")),
                                   (2.6, lambda fd: os.write(fd, b"echo BCAST_$((1+1))\n")),
                                   (3.4, lambda fd: None)])
vis = visible(out)
if b"Switch window" not in vis:
    fail("menu S shows the switch-window list")
if b"BROADCAST" not in vis:
    fail("menu B shows the broadcast status line")
c1 = gob("capture-window", "-w", "1").stdout
c2 = gob("capture-window", "-w", "2").stdout
if "BCAST_2" not in c1 or "BCAST_2" not in c2:
    fail("broadcast typing reaches both windows (1:%r 2:%r)" % (c1[-80:], c2[-80:]))
finish(pid, fd)
gob("kill-server")

# 15n. Copy mode: menu M, a search into the scrollback, a selection, y
# copies to the clipboard (OSC 52 to the host) and keeps the mark; menu T
# pastes the mark into the window.
def m(*keys):
    for k in keys:
        gob("menu", k)
pid, fd, out = run_client(["new-session", "--", "bash", "--norc", "--noprofile"], 5.0, host="kitty",
                          actions=[(0.8, lambda fd: gob("send-keys", "seq 1 200\\n")),
                                   (1.8, lambda fd: m("m")),
                                   (2.0, lambda fd: m("/", "1", "5", "0", "enter")),
                                   (2.6, lambda fd: m("v", "$", "y")),
                                   (3.2, lambda fd: m("t")),
                                   (3.8, lambda fd: None)])
vis = visible(out)
if b"COPY" not in vis:
    fail("menu M shows the copy-mode status line")
if b"\x1b]52;c;MTUw\x1b\\" not in out:
    fail("y copies the searched, selected text to the host clipboard (base64 of 150)")
cap = gob("capture-window").stdout
if "150" not in cap.splitlines()[-1] and "150" not in cap:
    fail("menu T pastes the mark into the window (got %r)" % cap[-120:])
finish(pid, fd)
gob("kill-server")

# 15o. save writes every window's place, flags, live working directory and
# command; restore recreates them in a fresh session.
savedir = tempfile.mkdtemp(prefix="gvsave")
savefile = os.path.join(savedir, "sessions")
gob("new-session", "-s", "sv", "-d", "-x", "100", "-y", "30", "--", "sleep", "30")
gob("new-window", "-s", "sv", "--", "bash", "-c", "cd /tmp; exec sleep 30")
time.sleep(0.5)
gob("menu", "-s", "sv", "r"); gob("menu", "-s", "sv", "m"); gob("menu", "-s", "sv", "right", "5"); gob("menu", "-s", "sv", "enter")
gob("menu", "-s", "sv", "h")                       # hide window 2 (focus goes to 1)
before = gob("list-windows", "-s", "sv").stdout
r = gob("save", "-f", savefile)
if r.returncode != 0 or "2 windows" not in r.stdout:
    fail("save reports what it wrote (got %r)" % r.stdout)
saved = open(savefile).read()
if "arg bash" not in saved or "arg cd /tmp; exec sleep 30" not in saved:
    fail("the command line is saved argument by argument")
if not re.search(r"^cwd .*tmp$", saved, re.M):
    fail("the live working directory of the program is saved (got %r)" % saved[:400])
gob("kill-session", "sv")
r = gob("restore", "-f", savefile)
if r.returncode != 0 or "2 windows" not in r.stdout:
    fail("restore reports what it made (got %r)" % r.stdout)
time.sleep(0.5)
after = gob("list-windows", "-s", "sv").stdout
def geoms(text):
    return sorted(re.findall(r":(\d+): (\S+) \((\d+x\d+)\) (-?\d+,-?\d+) z=(\d+)(.*)$", text, re.M))
if geoms(before) != geoms(after):
    fail("restore reproduces the windows' places, sizes, order and flags:\n%s\n%s" % (before, after))
r = gob("restore", "-f", savefile)
if "exists, left alone" not in r.stdout:
    fail("restore leaves an existing session alone")
gob("kill-server")
shutil.rmtree(savedir, ignore_errors=True)

# 15p. Live upgrade: the server execs the given (here: the same) binary in
# place. The programs keep running (same pid), the screen, scrollback and
# window layout survive, and a terminal attached across it keeps painting.
pid, fd, out = run_client(["new-session", "--", "bash", "--norc", "--noprofile"], 6.0, host="kitty",
                          actions=[(0.8, lambda fd: gob("send-keys", "echo PID=$$; seq 1 40\\n")),
                                   (1.8, lambda fd: globals().__setitem__("upg", gob("upgrade"))),
                                   (2.8, lambda fd: gob("send-keys", "echo LIVE_$$\\n")),
                                   (3.6, lambda fd: None)])
if upg.returncode != 0 or "upgraded" not in upg.stdout:
    fail("upgrade replaces the server in place (got rc=%d %r)" % (upg.returncode, upg.stdout))
vis = visible(out)
if b"older odin-seat" in out or b"protocol" in out:
    fail("a current client attached across the upgrade is not told off")
if b"LIVE_" not in vis:
    fail("the attached terminal keeps receiving frames after the upgrade")
cap = gob("capture-window", "-S", "-").stdout
pids = re.findall(r"PID=(\d+)", cap)
live = re.findall(r"LIVE_(\d+)", cap)
if not pids or not live or pids[0] != live[0]:
    fail("the program in the window is the same process after the upgrade (%r %r)" % (pids, live))
if "1\n2\n3\n" not in cap.replace("\r", ""):
    fail("the scrollback survives the upgrade")
finish(pid, fd)
r = gob("upgrade", "/nonexistent/odin-seat")
if r.returncode == 0 or "nonexistent" not in r.stdout:
    fail("an upgrade to a missing binary fails cleanly")
if gob("list-windows").returncode != 0:
    fail("the server is still there after a failed upgrade")
gob("kill-server")

# 16. CSI 8 ; rows ; cols t resizes the window. It is capped to the desktop
# (largest that fits) and shifted in when it would run past an edge.
import re as _re
def win1_geom(sess):
    out = gob("list-windows", "-s", sess).stdout
    m = _re.search(r":1:.*\((\d+)x(\d+)\)\s+(-?\d+),(-?\d+)", out)
    return tuple(int(x) for x in m.groups()) if m else None

gob("new-session", "-s", "rz", "-d", "-x", "100", "-y", "30", "--", "bash",
    "-c", "printf '\\033[8;12;50t'; sleep 30")
time.sleep(0.6)
g = win1_geom("rz")
if g != (50, 12, None) and (g is None or (g[0], g[1]) != (50, 12)):
    fail("CSI 8 t resizes the window to the requested size (got %s)" % (g,))
gob("kill-server")

# Larger than the desktop is capped to the largest that fits (100x30 desk,
# framed, so 98x28) and positioned on-screen.
gob("new-session", "-s", "rzcap", "-d", "-x", "100", "-y", "30", "--", "bash",
    "-c", "printf '\\033[8;999;999t'; sleep 30")
time.sleep(0.6)
g = win1_geom("rzcap")
if g is None or g[0] != 98 or g[1] != 28:
    fail("an oversized request is capped to the desktop (got %s)" % (g,))
if g is not None and (g[2] != 0 or g[3] != 0):
    fail("the capped window is shifted fully on-screen")
gob("kill-server")

# A size that fits but runs off the right edge is shifted in, not capped:
# 80 cols fits on a 100-wide desktop (80+2 frame <= 100), so it stays 80 and
# the window is moved so its whole frame is on-screen.
gob("new-session", "-s", "rzmv", "-d", "-x", "100", "-y", "30", "--", "bash",
    "-c", "printf '\033[8;10;80t'; sleep 30")
time.sleep(0.6)
g = win1_geom("rzmv")
if g is None or g[0] != 80:
    fail("a size that fits is not capped (got %s)" % (g,))
if g is not None and g[2] + g[0] + 2 > 100:
    fail("a window running off the edge is shifted fully on-screen")
gob("kill-server")

# 6. The Unicode picker: every category tab fits inside the box (they wrap
# onto a second row), and Tab moves to the Math category, whose grid is
# the whole Mathematical Operators block, starting at U+2200.
pid, fd, out = run_client(
    ["new-session", "--", "sh", "-c", "sleep 30"], 3.5,
    actions=[(1.0, lambda fd: gob("menu", "u")),
             (2.2, lambda fd: os.write(fd, b"\t"))])
vis = visible(out)
for name in (b"Arrows", b"Math", b"Emoji", b"Hiragana", b"Katakana",
             b"Hangul", b"Hanzi"):
    if name not in vis:
        fail("picker tab %s is painted whole" % name.decode())
if "∀".encode() not in vis:
    fail("Tab moves to Math, whose grid starts at U+2200")
if "←".encode() not in vis:
    fail("the Arrows page shows U+2190 first")
finish(pid, fd)
gob("kill-server")

if failed:
    print("regress/e2e.py: FAILED", file=sys.stderr)
    sys.exit(1)
print("regress/e2e.py: ok")
