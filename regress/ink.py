#!/usr/bin/env python3
"""Terminal-drawn ink through the multiplexer, end to end.

The fake terminal keeps a screen, and inks as the protocol's page says a
terminal does: only when the pen comes down on a cell showing the brush's
image, with points worked out from what it shows, and it applies the records
it is sent. So this checks what no unit test sees: which cells the compositor
gives the terminal to ink through, that a stroke comes out the same in the
terminal, in the table and in the program, which works its points out from its
own reports, and that two windows whose programs chose the same ids stay apart
while a brush follows the pen between them."""
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

ROOT = Path(__file__).resolve().parents[1]
GV = os.environ.get("GV", str(ROOT / "odin-seat"))
LABEL = "ink-" + str(os.getpid())
ST = b"\x1b\\"
CW, CH = 10, 20
COLS, ROWS = 100, 30
DIACRITICS = [
    int(c, 16)
    for c in re.findall(
        r"0x([0-9A-Fa-f]+)",
        re.search(r"placeholder_diacritics\[\] = \{(.*?)\};",
                  (ROOT / "src/term.cc").read_text(), re.S).group(1))
]
TOKEN = re.compile(
    rb"\x1b\[([0-?]*)[ -/]*([@-~])|\x1b([\]_P^X])(.*?)(?:\x1b\\|\x07)|\x1b[^\[\]_P^X]",
    re.S)


def command(*args):
    result = subprocess.run([GV, "-L", LABEL, *args], capture_output=True, timeout=10)
    assert result.returncode == 0, (args, result.stderr, result.stdout)
    return result.stdout


def geometry(index):
    listing = command("list-windows")
    match = re.search(rb":%d: .*\((\d+)x(\d+)\) (-?\d+),(-?\d+)" % index, listing)
    assert match, listing
    return tuple(map(int, match.groups()))


def pts(record):
    values = list(map(float, record.split("pts=")[1].split()[0].split(",")))
    return [tuple(values[i:i + 3]) for i in range(0, len(values), 3)]


class Terminal:
    """A screen, and ink as a terminal draws it."""

    def __init__(self, argv):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", ROWS, COLS, COLS * CW, ROWS * CH))
        env = dict(os.environ, TERM="xterm-kitty", ODIN_SEAT_KITTY_MEDIUM="direct",
                   PYENV_REHASH_TIMEOUT="1")
        self.proc = subprocess.Popen([GV, "-L", LABEL, *argv], stdin=slave,
                                     stdout=slave, stderr=slave, env=env)
        os.close(slave)
        self.seen = bytearray()
        self.todo = bytearray()
        self.done = set()
        self.cells = {}          # (x, y) -> [host id, row, column]
        self.at = [0, 0]
        self.fg = 0
        self.last = None
        self.images = {}         # host id -> (s, v)
        self.placed = {}         # host id -> (c, r, sx, sy, sw, sh)
        self.brush = None
        self.mode = 0
        self.patches = []        # the records it was sent
        self.scenes = {}         # host id -> {element id -> [(x, y, width)]}
        self.stroke = None       # (host id, element id, look)
        self.answer_sync = True
        self.unanswered = []

    def bootstrap(self):
        table = ((b"\x1b[16t", b"\x1b[6;%d;%dt" % (CH, CW)),
                 (b"\x1b[18t", b"\x1b[8;%d;%dt" % (ROWS, COLS)),
                 (b"a=q", b"\x1b_Gi=31;OK" + ST),
                 (b"\x1b[>q", b"\x1bP>|silkie(0.50)" + ST),
                 (b"caps;v=1:op=query:id=17349523",
                  b"\x1b]777;caps;v=1:op=cap:id=17349523:caps=3:"
                  b"ext=caps.1,ink.1,tablet-pointer.1:media=d,f,t,s:format=24,32,100,200" + ST),
                 (b"ink;v=1:op=query:id=17349528",
                  b"\x1b]777;ink;v=1:op=cap:id=17349528:caps=1:mode=0" + ST),
                 (b"tablet-pointer;v=1:op=query:id=17349531",
                  b"\x1b]777;tablet-pointer;v=1:op=cap:id=17349531:caps=1:mode=0" + ST),
                 (b"\x1b[c", b"\x1b[?62;22c"))
        for query, answer in table:
            if query in self.seen and query not in self.done:
                os.write(self.master, answer)
                self.done.add(query)

    def apply(self, image, text, edit=True):
        scene = self.scenes.setdefault(image, {})
        for line in text.splitlines():
            if edit:
                self.patches.append((image, line))
            found = re.match(r"([kax]) id=(\d+)", line)
            if found is None:
                continue
            kind, number = found.group(1), int(found.group(2))
            if kind == "x":
                scene.pop(number, None)
            elif kind == "k":
                scene[number] = pts(line)
            elif number in scene:
                scene[number] += pts(line)

    def graphics(self, body):
        head, _, payload = body[1:].partition(b";")
        keys = dict(k.split(b"=", 1) for k in head.split(b",") if b"=" in k)
        num = lambda k, d=0: int(keys.get(k, d))
        act, image = keys.get(b"a", b"t"), num(b"i")
        if act in (b"t", b"T") and b"s" in keys:
            self.images[image] = (num(b"s"), num(b"v"))
            self.scenes[image] = {}
            if payload:
                self.apply(image, base64.b64decode(payload).decode(), False)
        elif act == b"p" and b"U" in keys:
            s, v = self.images[image]
            self.placed[image] = (num(b"c"), num(b"r"), num(b"x"), num(b"y"),
                                  num(b"w", s), num(b"h", v))
        elif act == b"e":
            self.apply(image, base64.b64decode(payload).decode())
        elif act == b"d":
            self.placed.pop(image, None)

    def ink_frame(self, body):
        fields = dict(k.split(b"=", 1) for k in body.split(b":"))
        if fields.get(b"op") == b"brush":
            self.brush = fields
            self.mode = 1
        elif fields.get(b"op") == b"mode" and b"mode" in fields:
            self.mode = int(fields[b"mode"])
            if not self.mode:
                self.stroke = None
        elif fields.get(b"op") == b"query":
            answer = b"\x1b]777;ink;v=1:op=cap:id=%s:caps=1:mode=%d" % (
                fields[b"id"], self.mode) + ST
            if self.answer_sync:
                os.write(self.master, answer)
            else:
                self.unanswered.append(answer)

    def sequence(self, match):
        if match.group(2) == b"H":
            parts = [int(n or 1) for n in match.group(1).split(b";")]
            parts += [1] * (2 - len(parts))
            self.at = [parts[1] - 1, parts[0] - 1]
        elif match.group(2) == b"J":
            self.cells.clear()
        elif match.group(2) == b"m":
            colour = re.search(rb"38;2;(\d+);(\d+);(\d+)", match.group(1))
            if colour:
                self.fg = sum(int(c) << s for c, s in zip(colour.groups(), (16, 8, 0)))
        elif match.group(3) == b"_" and match.group(4).startswith(b"G"):
            self.graphics(match.group(4))
        elif match.group(3) == b"]" and match.group(4).startswith(b"777;ink;"):
            self.ink_frame(match.group(4)[8:])

    def text(self, data):
        for ch in data.decode("utf-8", "replace"):
            cp = ord(ch)
            if cp == 0x10EEEE:
                self.last = [self.fg, None, None]
                self.cells[tuple(self.at)] = self.last
                self.at[0] += 1
            elif cp in DIACRITICS and self.last is not None:
                # Row, column, then the top byte of the image's id.
                if self.last[1] is None:
                    self.last[1] = DIACRITICS.index(cp)
                elif self.last[2] is None:
                    self.last[2] = DIACRITICS.index(cp)
                else:
                    self.last[0] |= DIACRITICS.index(cp) << 24
            elif cp >= 0x20:
                self.cells.pop(tuple(self.at), None)
                self.last = None
                self.at[0] += 1

    def feed(self):
        while self.todo:
            esc = self.todo.find(b"\x1b")
            if esc != 0:
                run = bytes(self.todo if esc < 0 else self.todo[:esc])
                # A character cut off at the end of a read waits for the rest.
                keep = 0
                if esc < 0:
                    for n in range(1, min(4, len(run)) + 1):
                        if run[-n] >= 0xC0:
                            need = 2 + (run[-n] >= 0xE0) + (run[-n] >= 0xF0)
                            keep = n if n < need else 0
                            break
                self.text(run[:len(run) - keep])
                del self.todo[:len(run) - keep]
                if keep:
                    return
                continue
            match = TOKEN.match(bytes(self.todo))
            if match is None:
                return
            self.sequence(match)
            del self.todo[:match.end()]

    def pump(self, seconds=0.1):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            if not select.select([self.master], [], [], 0.02)[0]:
                continue
            try:
                data = os.read(self.master, 1 << 20)
            except OSError:
                break
            self.seen.extend(data)
            self.todo.extend(data)
            self.bootstrap()
            self.feed()

    def report(self, phase, x, y, pressure, ink=b""):
        os.write(self.master,
                 b"\x1b]777;tablet-pointer;v=1:op=event:kind=1:phase=%d:device=3:t=1:contact=0:"
                 b"x=%d:y=%d:buttons=1:mods=0:tool=1:valid=1:p=%d%s" %
                 (phase, x * 256, y * 256, pressure, ink) + ST)

    def showing(self, image):
        return {at: cell for at, cell in self.cells.items() if cell[0] == image}

    def point(self, x, y, pressure):
        """The page's formulas, from what this terminal has on its screen."""
        image, _, look = self.stroke
        c, r, sx, sy, sw, sh = self.placed[image]
        at, cell = next(iter(self.showing(image).items()))
        col0, row0 = at[0] - cell[2], at[1] - cell[1]
        scale = min(c * CW / sw, r * CH / sh)
        W, H = sw * scale, sh * scale
        L = col0 * CW + (c * CW - W) / 2
        T = row0 * CH + (r * CH - H) / 2
        w0, w1 = float(look[b"w0"]), float(look[b"w1"])
        return (sx + (x - L) * sw / W, sy + (y - T) * sh / H,
                w0 + (w1 - w0) * pressure / 65535)

    def down(self, x, y, pressure):
        """Inks if the pen is on the brush's image: the element's id, or None."""
        image = int(self.brush[b"i"]) if self.mode and self.brush else None
        cell = self.cells.get((x // CW, y // CH))
        if image is None or cell is None or cell[0] != image or int(self.brush[b"n"]) == 0:
            self.report(1, x, y, pressure)
            return None
        number = int(self.brush[b"k"])
        self.stroke = (image, number, dict(self.brush))
        self.scenes.setdefault(image, {})[number] = [self.point(x, y, pressure)]
        self.brush[b"k"] = b"%d" % (number + 1)
        self.brush[b"n"] = b"%d" % (int(self.brush[b"n"]) - 1)
        self.report(1, x, y, pressure, b":ink=%d" % number)
        return number

    def move(self, phase, x, y, pressure):
        if self.stroke:
            self.scenes[self.stroke[0]][self.stroke[1]].append(self.point(x, y, pressure))
        self.report(phase, x, y, pressure)
        if phase == 3:
            self.stroke = None

    def close(self):
        os.close(self.master)
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()


# A program that takes a vector image and a brush from k up, which it can turn
# off and on. The big one is 1200 by 900 and asks for 120 by 40 cells at its
# window's corner, far more than the window has. By the protocol's table it
# takes the image to fill those cells, while a terminal given a U=1 placement
# fits it inside them: a stroke is the same only if its reports say so.
CHILD = r"""
import base64,os,re,select,sys,time,tty
log=open(sys.argv[1],'ab',buffering=0);tty.setraw(0);k=int(sys.argv[2])
ST=b'\x1b\\'
os.write(1,b'\x1b]777;tablet-pointer;v=1:op=mode:mode=1'+ST)
scene=base64.b64encode(b'p id=1 d=M0,0L1,1\n')
if k==5000:
    os.write(1,b'\x1b[1;1H\x1b_Ga=T,f=200,i=9,s=1200,v=900,c=120,r=40,C=1,q=2;'+scene+ST)
else:
    os.write(1,b'\x1b[6;6H\x1b_Ga=T,f=200,i=9,s=100,v=100,c=10,r=5,C=1,q=2;'+scene+ST)
os.write(1,b'\x1b]777;ink;v=1:op=brush:i=9:k=%d:n=10:fill=#112233:w0=1:w1=5'%k+ST)
got=bytearray()
while True:
    if select.select([0],[],[],.05)[0]:
        data=os.read(0,1<<16)
        if data==b'q':os._exit(0)
        if data==b'o':os.write(1,b'\x1b]777;ink;v=1:op=mode:mode=0'+ST);continue
        if data==b'n':os.write(1,b'\x1b]777;ink;v=1:op=mode:mode=1'+ST);continue
        got.extend(data)
    for m in re.finditer(rb'tablet-pointer;(v=1:op=event[^\x1b]*)\x1b\\',bytes(got)):
        log.write(m.group(1)+b'\n')
    end=bytes(got).rfind(ST)
    if end>=0:del got[:end+2]
"""


def reports(path):
    """The reports of strokes a program got, without those of a hovering pen."""
    lines = path.read_bytes().splitlines() if path.exists() else []
    return [line for line in lines if b":phase=0:" not in line]


def fields(line):
    return dict(k.split(b"=", 1) for k in line.split(b":"))


def believed(line, width0=1, width1=5):
    """What the program works out: its image in its 120 by 40 cells."""
    f = fields(line)
    x, y = int(f[b"x"]) / 256, int(f[b"y"]) / 256
    return (x * 1200 / (120 * CW), y * 900 / (40 * CH),
            width0 + (width1 - width0) * int(f[b"p"]) / 65535)


def close_to(a, b, slack=0.02):
    return all(abs(p - q) < slack for p, q in zip(a, b))


def wait(until, *terminals, tries=60):
    for _ in range(tries):
        for terminal in terminals:
            terminal.pump(0.05)
        if until():
            return True
    return False


with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    child = root / "child.py"
    child.write_text(CHILD)
    log1, log2 = root / "one", root / "two"
    t1 = Terminal(["new-session", "--", sys.executable, str(child), str(log1), "5000"])
    t2 = None
    try:
        assert wait(lambda: t1.brush, t1, tries=100), bytes(t1.seen[-600:])
        image = int(t1.brush[b"i"])
        assert image != 9, "the terminal knows the placement by its own id"
        assert int(t1.brush[b"k"]) != 5000, "and the strokes by ids of ours"
        assert wait(lambda: t1.showing(image), t1), bytes(t1.seen[-600:])
        assert t1.placed[image][:2] == (120, 40), t1.placed

        # What the terminal can ink through: the image's part in the window, all of it.
        cols, rows, wx, wy = geometry(1)
        ox, oy = wx + 1, wy + 1
        assert cols + 2 + wx <= COLS and rows < 40, (cols, rows)
        want = {(x, y): [image, y - oy, x - ox]
                for x in range(ox, ox + cols) for y in range(oy, oy + rows)}
        # A slow machine can be read between the first cell of a frame and its last.
        wait(lambda: t1.showing(image) == want, t1)
        assert t1.showing(image) == want, (sorted(t1.showing(image))[:6], sorted(want)[:6])

        t2 = Terminal(["attach"])
        wait(lambda: t2.showing(image) == want, t2, t1)
        assert t2.showing(image) == want, sorted(t2.showing(image))[:6]

        # A stroke from inside the window to outside it, on the terminal's own numbers.
        px, py = (ox + 10) * CW + 3, (oy + 5) * CH + 7
        outside = (wx + cols + 5) * CW
        assert outside < COLS * CW, (wx, cols)
        token = t1.down(px, py, 13107)
        assert token is not None, "the pen is on the image"
        t1.move(2, px + 40, py + 15, 39321)
        t1.move(2, outside, py + 30, 39321)
        assert wait(lambda: len(t2.patches) == 3, t1, t2), t2.patches
        assert not t1.patches, ("not sent what it is drawing", t1.patches)
        t1.move(3, outside + 20, py + 30, 0)
        drawn = list(t1.scenes[image][token])
        assert wait(lambda: len(reports(log1)) == 4 and len(t2.patches) == 4, t1, t2), reports(log1)
        told = reports(log1)
        assert b":ink=5000" in told[0], told
        assert all(b"ink=" not in r for r in told[1:]), told
        assert t2.patches[0][1].startswith("k id=5000 ") and "fill=#112233" in t2.patches[0][1]
        kept = [p for _, record in t2.patches for p in pts(record)]
        for mine, theirs, program in zip(drawn, kept, map(believed, told)):
            assert close_to(mine, theirs, 1e-6), ("table", drawn, kept)
            assert close_to(mine, program), ("program", drawn, list(map(believed, told)))
        # The terminal's element has become the program's: the next record for
        # 5000 will find it. Nothing else of it was sent, or anything of ours.
        assert wait(lambda: 5000 in t1.scenes[image], t1), t1.patches
        assert token not in t1.scenes[image], t1.scenes[image].keys()
        assert close_to([v for p in t1.scenes[image][5000] for v in p],
                        [v for p in drawn for v in p], 1e-6), (t1.scenes[image][5000], drawn)
        assert [line.split()[0] for _, line in t1.patches] == ["x", "k"], t1.patches
        assert not any(str(token) in line for _, line in t2.patches), t2.patches
        # The last points are outside the window and inside the image: kept,
        # and nothing shows them, since no cell there shows the image.
        assert 0 < drawn[2][0] < 1200 and (outside // CW, (py + 30) // CH) not in t1.showing(image)
        assert t1.showing(image) == want, "a stroke leaves the cells as they were"

        # Another window comes up over it, and its program happens to choose
        # the ids the first has left. Cells it covers stop showing the first's
        # image, and the terminal is given the brush of the one now focused.
        t1.answer_sync = False
        old = dict(t1.brush)
        unspent = int(t1.brush[b"k"])
        command("new-window", "--", sys.executable, str(child), str(log2), "5001")
        assert wait(lambda: t1.brush[b"i"] != old[b"i"] and t1.mode == 1 and
                    len(t1.showing(image)) < len(want), t1, t2), (t1.brush, len(t1.showing(image)))
        second = int(t1.brush[b"i"])
        cols2, rows2, x2, y2 = geometry(2)
        for x, y in t1.showing(image):
            assert not (x2 <= x < x2 + cols2 + 2 and y2 <= y < y2 + rows2 + 2), (x, y)
        assert t1.showing(image) and set(t1.showing(image)) < set(want), "partly covered"
        assert wait(lambda: t1.showing(second), t1)

        # The report of a stroke that terminal began on the first's image, but
        # for a window it was not on: that window hears no id, and the ink goes.
        x, y = next(iter(t1.showing(second)))
        before = len(t1.patches)
        t1.report(1, x * CW, y * CH, 100, b":ink=%d" % unspent)
        t1.report(3, x * CW, y * CH, 0)
        assert wait(lambda: len(reports(log2)) == 2, t1), reports(log2)
        assert all(b"ink" not in r for r in reports(log2)), reports(log2)
        assert len(reports(log1)) == 4, "the window it was not for hears nothing"
        assert wait(lambda: (image, "x id=%d" % unspent) in t1.patches[before:], t1), t1.patches[before:]
        # Answered: those ids are done with, and a report of one is no one's.
        t1.answer_sync = True
        for answer in t1.unanswered:
            os.write(t1.master, answer)
        t1.pump(0.2)

        # A stroke on each, with the same id from each program.
        x, y = next(iter(t1.showing(second)))
        px, py = x * CW + 2, y * CH + 2
        t1.report(0, px, py, 0)
        token = t1.down(px, py, 65535)
        assert token is not None, "the pen is on the focused window's image"
        t1.move(3, px + 1, py, 0)
        assert wait(lambda: len(reports(log2)) == 4, t1), reports(log2)
        assert b":ink=5001" in reports(log2)[2], reports(log2)
        assert wait(lambda: 5001 in t1.scenes[second], t1), t1.scenes[second].keys()

        # The pen goes to the first window, not focused. The brush goes with it.
        sx, sy = min(t1.showing(image))
        px, py = sx * CW + 2, sy * CH + 2
        t1.report(0, px, py, 0)
        assert wait(lambda: t1.brush[b"i"] == old[b"i"], t1), t1.brush
        token = t1.down(px, py, 65535)
        assert token is not None, "a window need not be focused to be inked"
        t1.move(2, px + 1, py, 65535)
        # Its program turns ink off during the stroke: this terminal must stop,
        # though the brush it would hold next is the other window's.
        command("send-keys", "-w", "1", "-l", "o")
        assert wait(lambda: t1.stroke is None and t1.brush[b"i"] != old[b"i"], t1), (t1.stroke, t1.brush)
        assert t1.mode == 1, "and ink is on for the window that wants it"
        t1.move(2, px + 2, py, 65535)
        t1.move(3, px + 3, py, 0)
        assert wait(lambda: 5001 in t1.scenes[image], t1), t1.scenes[image].keys()
        assert len(t1.scenes[image][5001]) == 2, "no point after ink was turned off"
        assert token not in t1.scenes[image]
        assert len(reports(log1)) == 8, reports(log1)
        assert b":ink=5001" in reports(log1)[4], reports(log1)
        command("send-keys", "-w", "1", "-l", "n")

        # It closes: ink is back on the first, whose ids are as it left them.
        t1.report(5, px, py, 0)
        t1.pump(0.1)
        command("send-keys", "-w", "2", "-l", "q")
        assert wait(lambda: t1.mode == 1 and t1.brush[b"i"] == old[b"i"], t1), (t1.mode, t1.brush)
        t1.pump(0.3)
        assert t1.showing(image) == want, "uncovered, all of it again"

        # A terminal attaching later gets the strokes, under the program's ids.
        t2.close()
        t2 = None
        late = Terminal(["attach"])
        assert wait(lambda: 5001 in late.scenes.get(image, {}) and
                    5000 in late.scenes.get(image, {}), late, t1), late.scenes.keys()
        assert set(late.scenes[image]) == {5000, 5001}, late.scenes[image].keys()
        assert close_to([v for p in late.scenes[image][5000] for v in p],
                        [v for p in drawn for v in p], 1e-6), (late.scenes[image][5000], drawn)
        late.close()
    finally:
        subprocess.run([GV, "-L", LABEL, "kill-server"], capture_output=True, timeout=5)
        t1.close()
        if t2:
            t2.close()
print("regress/ink.py: ok")
