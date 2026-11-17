#!/usr/bin/env python3
"""Tiny windows survive program, mouse, and host resizes through a real PTY."""
import fcntl
import itertools
import json
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

GV = os.environ.get("GV", str(Path(__file__).resolve().parents[1] / "odin-seat"))
LABEL = "resize-" + str(os.getpid())


def command(*args):
    result = subprocess.run([GV, "-L", LABEL, *args], capture_output=True, timeout=5)
    assert result.returncode == 0, (args, result.stderr, result.stdout)
    return result.stdout


def geometry():
    listing = command("list-windows")
    match = re.search(rb":1: .*\((\d+)x(\d+)\) (-?\d+),(-?\d+)", listing)
    assert match, listing
    return tuple(map(int, match.groups()))


with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    log = root / "size.json"
    script = root / "child.py"
    script.write_text(r'''
import fcntl, json, os, signal, struct, sys, termios, tty
from pathlib import Path
tty.setraw(0)
signal.signal(signal.SIGWINCH, lambda *args: None)
path = Path(sys.argv[1])
path.write_text('"ready"')
os.write(1, b'\033[?1000h\033[?1006h')
for line in sys.stdin:
    path.with_suffix('.input').write_bytes(line.encode())
    args = line.split()
    if args[0] == 'size':
        os.write(1, ('\033[8;%s;%st' % (args[2], args[1])).encode())
    else:
        rows, cols, _, _ = struct.unpack('HHHH', fcntl.ioctl(0, termios.TIOCGWINSZ, b'\0' * 8))
        path.write_text(json.dumps([cols, rows]))
        os.write(1, b'\033[2J\033[H' + args[1].encode() + b'\033[H')
''')
    master, slave = pty.openpty()
    cols, rows = 100, 30

    def set_size(c, r):
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", r, c, c * 10, r * 20))

    set_size(cols, rows)
    client = subprocess.Popen(
        [GV, "-L", LABEL, "new-session", "--", sys.executable, str(script), str(log)],
        stdin=slave, stdout=slave, stderr=slave,
        env=dict(os.environ, TERM="xterm", PYENV_REHASH_TIMEOUT="1"))
    output = bytearray()
    answered = set()
    markers = itertools.cycle((b"@", b"%"))

    def pump():
        if not select.select([master], [], [], 0.02)[0]:
            return
        output.extend(os.read(master, 65536))
        replies = (
            (b"\x1b[16t", b"\x1b[6;20;10t"),
            (b"\x1b[18t", b"\x1b[8;30;100t"),
            (b"\x1b[c", b"\x1b[?62;22c"),
        )
        for query, reply in replies:
            if query in output and query not in answered:
                os.write(master, reply)
                answered.add(query)

    def until(check):
        deadline = time.monotonic() + 5
        while not check():
            assert time.monotonic() < deadline, ("timed out", bytes(output[-1000:]))
            pump()

    def repaint():
        # Input traverses the attached client, and the child reports its PTY
        # size before painting a cell with no newline or deferred wrap.
        log.unlink(missing_ok=True)
        output.clear()
        marker = next(markers)
        os.write(master, b"paint " + marker + b"\n")
        until(lambda: marker in output and log.exists())
        assert tuple(json.loads(log.read_text())) == geometry()[:2]
        assert marker in command("capture-window")

    try:
        until(lambda: log.exists())
        for c, r in ((1, 1), (1, 2), (2, 1)):
            command("send-keys", "-l", f"size {c} {r}\n")
            until(lambda: geometry()[:2] == (c, r))
            repaint()

        # Drag the corner past the opposite edge: size stops at 1x1.
        c, r, x, y = geometry()
        command("mouse", "press", str(x + c + 2), str(y + r + 2))
        command("mouse", "drag", "1", "1")
        command("mouse", "release", "1", "1")
        assert geometry()[:2] == (1, 1), geometry()
        repaint()

        # Drag past the desktop and then shrink it until the old window
        # lies completely outside. Repeat after regrowing the desktop.
        for mode in ("m", "r", "m"):
            c, r, x, y = geometry()
            command("mouse", "press", str(x + 1), str(y + 2))
            command("mouse", "drag", "1000", "1000")
            command("mouse", "release", "1000", "1000")
            assert geometry() == (1, 1, 97, 27), geometry()
            command("menu", "r")
            command("menu", mode)
            for cols, rows in ((40, 12), (1, 1), (1, 2), (2, 1), (3, 3), (100, 30)):
                set_size(cols, rows)
                # Popen has no controlling tty, so deliver the signal a real
                # foreground terminal would send along with TIOCSWINSZ.
                client.send_signal(signal.SIGWINCH)
                until(lambda: f"0: {cols}x{rows}".encode() in command("list-sessions"))
                if (cols, rows) == (40, 12):
                    # Cancel must clamp the saved position to the new screen.
                    command("menu", "esc")
                c, r, x, y = geometry()
                border = 2 if cols >= 3 and rows >= 3 else 0
                assert c >= 1 and r >= 1 and x >= 0 and y >= 0, geometry()
                assert x + c + border <= cols and y + r + border <= rows, geometry()
                if border == 0:
                    command("mouse", "press", str(x + 1), str(y + 1))
                    command("mouse", "release", str(x + 1), str(y + 1))
                repaint()
                if border == 0:
                    assert b"\x1b[<0;1;1M\x1b[<0;1;1m" in log.with_suffix(".input").read_bytes()
        print("regress/resize.py: ok")
    finally:
        subprocess.run([GV, "-L", LABEL, "kill-server"], capture_output=True, timeout=5)
        if client.poll() is None:
            client.terminate()
            try:
                client.wait(timeout=5)
            except subprocess.TimeoutExpired:
                client.kill()
                client.wait(timeout=5)
        os.close(master)
        os.close(slave)
