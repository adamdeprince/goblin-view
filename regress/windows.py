#!/usr/bin/env python3
"""Window splitting, job-control termination and growing descriptor sets."""
import os
from pathlib import Path
import re
import resource
import signal
import socket
import subprocess
import sys
import tempfile
import time

GV = os.path.abspath(os.environ.get("GV", Path(__file__).resolve().parents[1] / "odin-seat"))


def main():
    with tempfile.TemporaryDirectory(prefix="odin-windows-", dir="/tmp") as directory:
        root = Path(directory)
        address = root / "socket"
        env = dict(os.environ, SHELL="/bin/sh", XDG_CONFIG_HOME=str(root / "config"))
        env.pop("ODIN_SEAT_SOCK", None)
        env.pop("ODIN_SEAT_SESSION", None)

        def command(*args, success=True):
            result = subprocess.run([GV, "-S", str(address), *args], env=env,
                                    capture_output=True, text=True, timeout=10)
            assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
            return result.stdout

        def windows(session="0"):
            result = {}
            for line in command("list-windows", "-s", session).splitlines():
                match = re.match(r"[^:]+:(\d+): .* \((\d+)x(\d+)\) (-?\d+),(-?\d+) z=", line)
                assert match, line
                result[int(match[1])] = tuple(map(int, match.groups()[1:]))
            return result

        def wait_for(predicate):
            deadline = time.monotonic() + 5
            while not predicate():
                assert time.monotonic() < deadline, "Timed out waiting for test process"
                time.sleep(0.01)

        def alive(pid):
            state = subprocess.run(["ps", "-p", str(pid), "-o", "stat="],
                                   capture_output=True, text=True).stdout.strip()
            return bool(state) and not state.startswith("Z")

        def check_split(before, after, source, created, below=False):
            width, height, x, y = before[source]
            a, b = after[source], after[created]
            assert a[2:] == (x, y)
            if below:
                assert a[0] == b[0] == width and b[2] == x
                assert a[1] + b[1] + 2 == height and b[3] == y + a[1] + 2
            else:
                assert a[1] == b[1] == height and b[3] == y
                assert a[0] + b[0] + 2 == width and b[2] == x + a[0] + 2
            assert all(after[key] == value for key, value in before.items() if key != source)

        # Increase only the fixture process's descriptor allowance, up to the
        # existing OS hard limit. This is not an application capacity setting.
        def fixture_limits():
            soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
            resource.setrlimit(resource.RLIMIT_NOFILE, (max(soft, min(hard, 1024)), hard))

        server = subprocess.Popen([GV, "-S", str(address), "new-session", "-d",
                                   "-x", "160", "-y", "60", "--", "sleep", "120"], env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                  preexec_fn=fixture_limits)
        sockets = []
        pids = set()
        try:
            out, err = server.communicate(timeout=10)
            assert server.returncode == 0, out + err
            command("new-window", "--", "sleep", "120")
            before = windows()
            command("split-window", "-w", "1", "--", "sleep", "120")
            check_split(before, windows(), 1, 3)
            before = windows()
            command("split-window", "-v", "-w", "3", "--", "sleep", "120")
            check_split(before, windows(), 3, 4, below=True)
            before = windows()
            command("split-window", "-h", "-w", "4", "--", "sleep", "120")
            check_split(before, windows(), 4, 5)
            command("menu", "1")
            command("menu", "z")
            command("split-window", "-v", "-w", "1", "--", "sleep", "120")
            zoomed = windows()
            assert zoomed[1] == (158, 28, 0, 0) and zoomed[6] == (158, 28, 0, 30)
            before = windows()
            command("menu", "e")
            command("menu", "b")
            check_split(before, windows(), 6, 7, below=True)
            before = windows()
            command("menu", "e")
            command("menu", "esc")
            assert windows() == before
            command("new-session", "-s", "tiny", "-d", "-x", "1", "-y", "1", "--", "sleep", "120")
            before = windows("tiny")
            command("split-window", "-s", "tiny", success=False)
            assert windows("tiny") == before
            for value in ("0", "-1", "not-a-number", "4294967296"):
                command("split-window", "-w", value, success=False)
            command("kill-window", "-w", "9999", success=False)

            # A session leader and a separate foreground job, both refusing
            # ordinary hangups/termination. Force-kill must reach both groups.
            job = root / "job.py"
            job.write_text('''import os, signal, sys
from pathlib import Path
root = Path(sys.argv[1])
signal.signal(signal.SIGHUP, signal.SIG_IGN)
signal.signal(signal.SIGTERM, signal.SIG_IGN)
signal.signal(signal.SIGTTOU, signal.SIG_IGN)
child = os.fork()
if child == 0:
    os.setpgid(0, 0)
    while True: signal.pause()
os.setpgid(child, child)
os.tcsetpgrp(0, child)
(root / "pids").write_text(str(os.getpid()) + " " + str(child))
while True: signal.pause()
''')
            for mode in ("menu", "command"):
                pidfile = root / "pids"
                pidfile.unlink(missing_ok=True)
                command("new-session", "-s", mode, "-d", "--", sys.executable, str(job), str(root))
                wait_for(pidfile.exists)
                current = set(map(int, pidfile.read_text().split()))
                pids.update(current)
                assert all(alive(pid) for pid in current)
                if mode == "menu":
                    command("menu", "-s", mode, "k")
                    command("menu", "-s", mode, "n")
                    assert all(alive(pid) for pid in current)
                    command("menu", "-s", mode, "k")
                    # Another client changes focus while the question is up;
                    # the confirmation must still address the original id.
                    command("new-window", "-s", mode, "--", "sleep", "120")
                    command("menu", "-s", mode, "y")
                    assert set(windows(mode)) == {2}
                else:
                    command("kill-window", "-f", "-s", mode, "-w", "1")
                wait_for(lambda: not any(alive(pid) for pid in current))
                pids.difference_update(current)

            # A client launched inside a named session must act on that
            # session, while an explicit -s still overrides the inheritance.
            command("new-session", "-s", "nested", "-d", "--", "sleep", "120")
            nested_env = dict(env, ODIN_SEAT_SOCK=str(address), ODIN_SEAT_SESSION="nested")
            before_default = windows()
            subprocess.run([GV, "split-window", "--", "sleep", "120"], env=nested_env,
                           capture_output=True, text=True, timeout=10, check=True)
            assert len(windows("nested")) == 2 and windows() == before_default
            subprocess.run([GV, "kill-window", "-w", "2"], env=nested_env,
                           capture_output=True, text=True, timeout=10, check=True)
            assert set(windows("nested")) == {1}
            subprocess.run([GV, "kill-window", "-s", "menu", "-w", "2"], env=nested_env,
                           capture_output=True, text=True, timeout=10, check=True)
            assert set(windows("nested")) == {1}

            # These peers have not identified themselves. A command connecting
            # after all of them still needs to be polled, beyond the old 256.
            def connect_peer():
                peer = socket.socket(socket.AF_UNIX)
                peer.settimeout(5)
                try:
                    peer.connect(str(address))
                except (ConnectionRefusedError, BlockingIOError):
                    # A full listen backlog refuses the connection on macOS
                    # and answers EAGAIN on Linux, though the server is still
                    # accepting: try again.
                    peer.close()
                    return False
                sockets.append(peer)
                return True

            for _ in range(288):
                wait_for(connect_peer)
            assert "tiny:" in command("list-sessions")
            print("windows: splits, zoom, tiny screens, confirmations, foreground jobs and >256 peers passed")
        finally:
            for peer in sockets:
                peer.close()
            for pid in pids:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if address.exists():
                command("kill-server")
            server.wait(timeout=10)


if __name__ == "__main__":
    main()
