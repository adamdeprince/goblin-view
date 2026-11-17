#!/usr/bin/env python3
"""Notifications and focus through a real Odin Seat client/server and PTYs.

The fake host captures notices and injects activation replies, without opening
desktop notifications. Run after make, or set GV to the binary under test.
"""
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time

GV = os.path.abspath(os.environ.get('GV', Path(__file__).resolve().parents[1] / 'odin-seat'))
ST = b'\x1b\\'
CHILD = '''import os,select,sys,tty
tty.setraw(0)
fd=os.open(sys.argv[1],os.O_RDWR)
log=open(sys.argv[2],'wb',buffering=0)
os.write(1,b'\\x1b[?1004h')
open(sys.argv[2]+'.ready','w').close()
while True:
    for source in select.select([0,fd],[],[])[0]:
        data=os.read(source,65536)
        if source==fd:os.write(1,data)
        else:log.write(data)
'''


def main():
    with tempfile.TemporaryDirectory(prefix='odin-notifications-', dir='/tmp') as directory:
        root = Path(directory)
        env = dict(os.environ, TERM='xterm-kitty', SHELL='/bin/sh',
                   XDG_CONFIG_HOME=str(root / 'config'), ODIN_SEAT_KITTY_MEDIUM='direct')
        env.pop('ODIN_SEAT_SOCK', None)
        env.pop('ODIN_SEAT_SESSION', None)
        address = root / 'socket'
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 100, 1000, 600))
        seen, answered = bytearray(), set()
        client = None

        def command(*args):
            result = subprocess.run([GV, '-S', str(address), *args], env=env,
                                    capture_output=True, timeout=10)
            assert result.returncode == 0, (args, result.stdout, result.stderr)
            return result.stdout

        def pump():
            if select.select([master], [], [], .02)[0]:
                seen.extend(os.read(master, 1 << 20))
            for query, response in ((b'\x1b[16t', b'\x1b[6;20;10t'),
                                    (b'\x1b[18t', b'\x1b[8;30;100t'),
                                    (b'\x1b[c', b'\x1b[?62;22c')):
                if query in seen and query not in answered:
                    os.write(master, response)
                    answered.add(query)

        def wait(predicate):
            deadline = time.monotonic() + 8
            while not predicate():
                assert time.monotonic() < deadline, ('timed out', bytes(seen[-500:]))
                pump()

        def child(number, first=False):
            fifo, log = root / f'fifo{number}', root / f'log{number}'
            os.mkfifo(fifo)
            args = ['new-session', '-d', '-x', '100', '-y', '30'] if first else ['new-window']
            command(*args, '--', sys.executable, str(root / 'child.py'), str(fifo), str(log))
            wait(lambda: Path(str(log) + '.ready').exists())
            return os.open(fifo, os.O_WRONLY), log

        def notices():
            return re.findall(rb'\x1b\]99;([^;\x1b]*);([^\x1b]*)\x1b\\', seen)

        def notice(text):
            wait(lambda: any(payload == text for _, payload in notices()))
            return next(metadata for metadata, payload in reversed(notices()) if payload == text)

        def click(metadata, actions=b'focus,report', button=b''):
            fields = dict(part.split(b'=', 1) for part in metadata.split(b':'))
            fields[b'a'] = actions
            fields[b'r'] += b',silkie/test/12'
            os.write(master, b'\x1b]99;' + b':'.join(k + b'=' + v for k, v in fields.items()) + b';' + button + ST)

        def focused(number):
            return any(line.startswith(f'0:{number}:'.encode()) and b'focused' in line
                       for line in command('list-windows').splitlines())

        (root / 'child.py').write_text(CHILD)
        fds = []
        try:
            one, log1 = child(1, True)
            two, log2 = child(2)
            fds.extend((one, two))
            client = subprocess.Popen([GV, '-S', str(address), 'attach'], env=env,
                                      stdin=slave, stdout=slave, stderr=slave)
            wait(lambda: b'\x1b[c' in seen)
            wait(lambda: b'\x1b[I' in log2.read_bytes())
            assert focused(2)
            os.write(master, b'\x1b[O')
            wait(lambda: log2.read_bytes().endswith(b'\x1b[O'))
            os.write(master, b'\x1b[I')
            wait(lambda: log2.read_bytes().endswith(b'\x1b[I'))

            os.write(one, b'\x1b]9;Codex is done' + ST)
            converted = notice(b'Codex is done')
            assert b'odin-seat/' in converted and b'.1.c.' in converted
            click(converted)
            wait(lambda: focused(1))
            wait(lambda: log1.read_bytes().endswith(b'\x1b[I'))
            assert b']99;' not in log1.read_bytes()  # Codex did not request a reply.

            for fd, title in ((one, b'first'), (two, b'second')):
                os.write(fd, b'\x1b]99;i=foo:a=-focus,report;' + title + ST)
            first, second = notice(b'first'), notice(b'second')
            assert first != second
            click(second, b'report', b'2')
            wait(lambda: b'i=foo' in log2.read_bytes())
            assert focused(1)
            assert b'odin-seat/' in log2.read_bytes() and b'silkie/test/12' in log2.read_bytes()
            assert b';2' + ST in log2.read_bytes()
            assert b'i=foo' not in log1.read_bytes()

            command('menu', 'h')
            wait(lambda: focused(2))
            click(converted)
            wait(lambda: focused(1))

            # Existing notifications still route after exec-based server upgrade.
            command('upgrade')
            click(first)
            wait(lambda: b'i=foo' in log1.read_bytes())
            assert focused(1)
            os.write(master, b'\x1b[O')
            wait(lambda: log1.read_bytes().endswith(b'\x1b[O'))
            os.write(master, b'\x1b[I')
            wait(lambda: log1.read_bytes().endswith(b'\x1b[I'))
            command('kill-window', '-f', '-w', '1')
            wait(lambda: focused(2))
            before = log2.read_bytes()
            click(first)
            for _ in range(10):
                pump()
            assert log2.read_bytes() == before, 'stale click reached another window'
            print('notifications: focus, conversion, reply routing, upgrade, and stale clicks passed')
        finally:
            for fd in fds:
                os.close(fd)
            if client:
                client.terminate()
                client.wait(timeout=5)
            if address.exists():
                command('kill-server')
            os.close(slave)
            os.close(master)


if __name__ == '__main__':
    main()
