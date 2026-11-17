#!/usr/bin/env python3
"""Program status (OSC 7501) and ConEmu's OSC 9 through a real Odin Seat.

A fake terminal answers the OSC 7501 feature query, so the windows' records
are exported to it beneath w<id>, with its root record standing for the most
urgent of them; the frames show them too. Run after make, or set GV to the
binary under test.
"""
import base64
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
open(sys.argv[2]+'.ready','w').close()
while True:
    for source in select.select([0,fd],[],[])[0]:
        data=os.read(source,65536)
        if source==fd:os.write(1,data)
        else:log.write(data)
'''


def b64(text):
    return base64.b64encode(text.encode()).decode().rstrip('=').encode()


def main():
    with tempfile.TemporaryDirectory(prefix='odin-pstatus-', dir='/tmp') as directory:
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
            # The feature query is answered before DA1, as a terminal would.
            for query, response in ((b'\x1b]7501;?', b'\x1b]7501;?' + ST),
                                    (b'\x1b[16t', b'\x1b[6;20;10t'),
                                    (b'\x1b[18t', b'\x1b[8;30;100t'),
                                    (b'\x1b[c', b'\x1b[?62;22c')):
                if query in seen and query not in answered:
                    os.write(master, response)
                    answered.add(query)

        def wait(predicate, what=''):
            deadline = time.monotonic() + 8
            while not predicate():
                assert time.monotonic() < deadline, ('timed out', what, bytes(seen[-800:]))
                pump()

        def reports():
            return re.findall(rb'\x1b\]7501;(state=[^\x1b]*)\x1b\\', seen)

        def reported(body):
            wait(lambda: body in reports(), body)

        def child(number, first=False):
            fifo, log = root / f'fifo{number}', root / f'log{number}'
            os.mkfifo(fifo)
            args = ['new-session', '-d', '-x', '100', '-y', '30'] if first else ['new-window']
            command(*args, '--', sys.executable, str(root / 'child.py'), str(fifo), str(log))
            wait(lambda: Path(str(log) + '.ready').exists())
            return os.open(fifo, os.O_WRONLY), log

        (root / 'child.py').write_text(CHILD)
        fds = []
        try:
            one, log1 = child(1, True)
            two, log2 = child(2)
            fds.extend((one, two))
            client = subprocess.Popen([GV, '-S', str(address), 'attach'], env=env,
                                      stdin=slave, stdout=slave, stderr=slave)
            wait(lambda: b'\x1b[c' in seen)
            wait(lambda: b'state=clear' in b''.join(reports()), 'attach clears the terminal')

            # The feature query from a program is answered by odin-seat itself.
            os.write(two, b'\x1b]7501;?' + ST)
            wait(lambda: b'7501;?:states=' in log2.read_bytes())

            # A report goes to the terminal under the window's id; the root
            # stands for the most urgent record, titled by its window.
            msg = b64('Syncing photos')
            os.write(one, b'\x1b]7501;state=working:app=rsync:progress=40:msg=' + msg + ST)
            reported(b'state=working:id=w1:progress=40:app=rsync:msg=' + msg)
            wait(lambda: any(r.startswith(b'state=working:progress=40:app=rsync:title=')
                             for r in reports()), 'root follows window 1')
            # Children nest beneath the window's id.
            os.write(one, b'\x1b]7501;state=done:id=build/test' + ST)
            reported(b'state=done:id=w1/build/test')

            # Window 2 blocked on the user outranks it.
            os.write(two, b'\x1b]7501;state=blocked:kind=permission:app=terraform' + ST)
            reported(b'state=blocked:id=w2:kind=permission:app=terraform')
            wait(lambda: any(r.startswith(b'state=blocked:kind=permission:app=terraform:title=')
                             for r in reports()), 'root follows the blocked window')

            # A prompt in window 2 ends its blocked record, here and there.
            os.write(two, b'\x1b]133;A' + ST)
            reported(b'state=clear:id=w2')

            # Window 1 finished: its frame shows a check mark for its two
            # done records, and what the newest said along the bottom.
            os.write(one, b'\x1b]7501;state=done:app=rsync:msg=' + b64('Photos synced') + ST)
            wait(lambda: '✓×2'.encode() in seen, 'a done badge')
            # What the most urgent record says runs along the bottom border
            # (of window 2: window 1's is beneath it).
            os.write(two, b'\x1b]7501;state=error:app=terraform:msg=' + b64('Plan failed') + ST)
            wait(lambda: b'Plan failed' in seen, 'the message on the frame')
            wait(lambda: '\u2717'.encode() in seen, 'an error badge')

            # ConEmu: 9;4 is progress, never a notification; 9;2 is one.
            # (Once a window has sent a real report, 9;4 is not mapped until
            # a full reset, which also clears its records.)
            os.write(two, b'\x1bc')
            reported(b'state=clear:id=w2')
            os.write(two, b'\x1b]9;4;1;70' + ST)
            reported(b'state=working:id=w2:progress=70')
            os.write(two, b'\x1b]9;2;"Build finished"' + ST)
            wait(lambda: re.search(rb'\x1b\]99;[^;]*;Build finished\x1b\\', seen), '9;2 notice')
            assert not re.search(rb'\x1b\]99;[^;]*;4;1;70', seen), '9;4 became a notification'
            os.write(two, b'\x1b]9;3;Renamed' + ST)
            wait(lambda: any(line.startswith(b'0:2: Renamed ') for line in command('list-windows').splitlines()))

            # The records survive a live upgrade and are sent again.
            before = len(reports())
            command('upgrade')
            wait(lambda: b'state=working:id=w2:progress=70' in reports()[before:],
                 'records sent again after upgrade')

            # Closing a window clears its records on the terminal.
            command('kill-window', '-f', '-w', '1')
            reported(b'state=clear:id=w1')
            command('kill-window', '-f', '-w', '2')
            reported(b'state=clear:id=w2')
            wait(lambda: reports()[-1] == b'state=clear', 'the root is cleared at the end')
            print('pstatus: export, hierarchy, root, prompt, frame badge, ConEmu OSC 9 '
                  'and upgrade passed')
        finally:
            for fd in fds:
                os.close(fd)
            if client:
                client.terminate()
                client.wait(timeout=5)
            if address.exists():
                subprocess.run([GV, '-S', str(address), 'kill-server'], env=env,
                               capture_output=True, timeout=10)
            os.close(slave)
            os.close(master)


if __name__ == '__main__':
    main()
