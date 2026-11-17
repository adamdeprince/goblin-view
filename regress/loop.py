#!/usr/bin/env python3
"""The server's event loop: a burst of connections queues and is taken at
once, a flood of output is drawn a frame at a time instead of once a read
while a keystroke afterwards is still shown at once, and an idle server
sleeps instead of waking on a timer."""
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

GV = os.environ.get("GV", str(Path(__file__).resolve().parents[1] / "odin-seat"))


def server_pid(address):
    """The daemon serving this socket: the only process still naming it."""
    out = subprocess.run(["ps", "-ax", "-o", "pid=,command="], capture_output=True,
                         text=True, check=True).stdout
    pids = [int(line.split(None, 1)[0]) for line in out.splitlines()
            if str(address) in line and "odin-seat" in line]
    assert len(pids) == 1, out
    return pids[0]


def start(address, env, *argv):
    r = subprocess.run([GV, "-S", str(address), "new-session", "-d", *argv], env=env,
                       capture_output=True, text=True, timeout=10)
    assert r.returncode == 0, r.stdout + r.stderr


def command(address, env, *args):
    r = subprocess.run([GV, "-S", str(address), *args], env=env, capture_output=True,
                       text=True, timeout=10)
    assert r.returncode == 0, (args, r.stdout, r.stderr)
    return r.stdout


def burst(root, env):
    """Connections made while the server cannot accept wait in the backlog,
    many more than the 16 it once allowed, and none is refused."""
    address = root / "burst"
    start(address, env, "--", "sleep", "60")
    pid = server_pid(address)
    peers = []
    os.kill(pid, signal.SIGSTOP)
    try:
        for _ in range(100):	# under macOS's default SOMAXCONN of 128
            peer = socket.socket(socket.AF_UNIX)
            peer.settimeout(5)
            peer.connect(str(address))	# no retry: a refusal fails the test
            peers.append(peer)
    finally:
        os.kill(pid, signal.SIGCONT)
    assert "0:" in command(address, env, "list-sessions")
    for peer in peers:
        peer.close()
    command(address, env, "kill-server")


def idle(root, env):
    """A server with nothing to do makes no wakeups: no polling timer."""
    status = Path("/proc/self/status")
    if not status.exists():
        print("loop: idle wakeups not measured here (no /proc)")
        return
    address = root / "idle"
    start(address, env, "--", "sleep", "60")
    pid = server_pid(address)

    def switches():
        text = Path("/proc/%d/status" % pid).read_text()
        return int(re.search(r"voluntary_ctxt_switches:\s+(\d+)", text).group(1))

    time.sleep(1)			# let start-up settle
    before = switches()
    time.sleep(3)
    woke = switches() - before
    command(address, env, "kill-server")
    assert woke <= 1, "an idle server woke %d times in 3 s" % woke


def flood(root, env):
    """A program writing as fast as it can is drawn at most a frame per
    16 ms, everything it wrote still arrives, and a keystroke after it is
    echoed at once."""
    address = root / "flood"
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 1000, 600))
    # Numbered lines: every read changes the screen, so each pass would
    # have something to draw.
    script = ("printf 'READY\\n'; read go; seq 1 200000; "
              "printf 'DONE%s\\n' END; exec cat")
    client = subprocess.Popen([GV, "-S", str(address), "new-session", "--", "sh", "-c", script],
                              stdin=slave, stdout=slave, stderr=slave,
                              env=dict(env, TERM="xterm"))
    os.close(slave)
    out = bytearray()
    answered = set()

    def pump(wait=0.02):
        if select.select([master], [], [], wait)[0]:
            try:
                out.extend(os.read(master, 1 << 20))
            except OSError:
                return
        for query, reply in ((b"\x1b[16t", b"\x1b[6;20;10t"), (b"\x1b[18t", b"\x1b[8;30;100t"),
                             (b"\x1b[c", b"\x1b[?62;22c")):
            if query in out and query not in answered:
                os.write(master, reply)
                answered.add(query)

    def until(check, secs=20):
        deadline = time.monotonic() + secs
        while not check():
            assert time.monotonic() < deadline, ("timed out", bytes(out[-600:]))
            pump()

    try:
        until(lambda: b"READY" in out)
        time.sleep(0.3)
        pump()
        mark = len(out)
        began = time.monotonic()
        os.write(master, b"\r")
        until(lambda: b"DONEEND" in out[mark:])
        elapsed_ms = (time.monotonic() - began) * 1000
        frames = bytes(out[mark:]).count(b"\x1b[?2026h")
        allowed = elapsed_ms / 16 + 20
        assert frames <= allowed, "%d frames in %.0f ms (allowed %.0f)" % (frames, elapsed_ms, allowed)
        # A keystroke now: cat echoes it, shown without waiting for a frame.
        time.sleep(0.2)
        pump()
        mark = len(out)
        typed = time.monotonic()
        os.write(master, b"ZQX\r")
        until(lambda: bytes(out[mark:]).count(b"ZQX") >= 1, 2)
        latency = (time.monotonic() - typed) * 1000
        assert latency < 250, "a keystroke took %.0f ms to show" % latency
        print("loop: %d frames for a %.0f ms flood, echo in %.0f ms" % (frames, elapsed_ms, latency))
    finally:
        client.kill()
        client.wait(timeout=10)
        os.close(master)
        subprocess.run([GV, "-S", str(address), "kill-server"], env=env, capture_output=True,
                       timeout=10)


with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    env = dict(os.environ, ODIN_SEAT_CONFIG=str(root / "config"),
               XDG_STATE_HOME=str(root / "state"), PYENV_REHASH_TIMEOUT="1")
    env.pop("ODIN_SEAT_SOCK", None)
    burst(root, env)
    idle(root, env)
    flood(root, env)
    print("regress/loop.py: ok")
