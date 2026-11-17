#!/usr/bin/env python3
"""Arranging several windows at once, end to end: a fake terminal on a PTY, a
real client and server, four windows each running cat. A window goes to a
half of the desktop by a key (menu R then an arrow) or by the arrows on its
frame; windows are selected by a key, by the mark on their frame or in the
switch list; the selected ones -- every window when none is -- are laid out
together (menu L) stacked, as a grid, as rows, as columns or as two columns,
each layout sharing the desktop exactly; and broadcast (menu B) types to the
selected windows as a group, or to every window when none is selected. The
selection survives the server replacing itself."""
import fcntl,os,pty,re,select,struct,subprocess,sys,termios,time
from pathlib import Path
GV=os.environ.get('GV',str(Path(__file__).resolve().parents[1]/'odin-seat'))
LABEL='tiling-'+str(os.getpid())
ST=b'\x1b\\'
COLS,ROWS=100,30

def command(*args):
    r=subprocess.run([GV,'-L',LABEL,*args],capture_output=True,timeout=15)
    assert r.returncode==0,(args,r.stderr,r.stdout)
    return r.stdout.decode()

def windows():
    """{id: (cols, rows, x, y, z, selected)} of the inside of each window."""
    found={}
    for line in command('list-windows').splitlines():
        m=re.match(r'[^:]+:(\d+): .* \((\d+)x(\d+)\) (-?\d+),(-?\d+) z=(-?\d+)(.*)',line)
        assert m,line
        found[int(m[1])]=tuple(map(int,m.groups()[1:6]))+(' selected' in m[7],)
    return found

class Terminal:
    def __init__(self,argv):
        self.master,slave=pty.openpty()
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',ROWS,COLS,COLS*10,ROWS*20))
        env=dict(os.environ,TERM='xterm-kitty',PYENV_REHASH_TIMEOUT='1')
        self.proc=subprocess.Popen([GV,'-L',LABEL,*argv],stdin=slave,stdout=slave,stderr=slave,env=env)
        os.close(slave)
        self.seen=bytearray();self.done=set()
    def bootstrap(self):
        table=((b'\x1b[16t',b'\x1b[6;20;10t'),(b'\x1b[18t',b'\x1b[8;%d;%dt'%(ROWS,COLS)),
            (b'\x1b[>q',b'\x1bP>|fake(1.0)'+ST),(b'\x1b[c',b'\x1b[?62;22c'))
        for q,r in table:
            if q in self.seen and q not in self.done:
                os.write(self.master,r);self.done.add(q)
    def pump(self,seconds=.1):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            if not select.select([self.master],[],[],.02)[0]:continue
            try:data=os.read(self.master,1<<20)
            except OSError:break
            self.seen.extend(data);self.bootstrap()
    def type(self,text):
        os.write(self.master,text)
    def close(self):
        os.close(self.master)
        try:self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:self.proc.kill()

def menu(t,*keys):
    for k in keys:
        command('menu',k)
    t.pump(.15)

def frames(ws,ids):
    """Outer rectangles (x, y, w, h) of these windows."""
    return [(ws[i][2],ws[i][3],ws[i][0]+2,ws[i][1]+2) for i in ids]

def shares_desktop(rects):
    seen={}
    for x,y,w,h in rects:
        for r in range(y,y+h):
            for c in range(x,x+w):
                seen[(c,r)]=seen.get((c,r),0)+1
    return len(seen)==COLS*ROWS and all(v==1 for v in seen.values()) and \
        all(0<=c<COLS and 0<=r<ROWS for c,r in seen)

def has(win,text):
    return text in command('capture-window','-w',str(win))

def painted(t,text):
    """Is this on the screen? Asked of a repaint from scratch, since an
    update only sends the cells that changed."""
    n=len(t.seen);command('menu','f');t.pump(.3)
    return text in t.seen[n:]

def until(t,test,seconds=5):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        t.pump(.05)
        if test():return True
    return False

t=Terminal(['new-session','--','cat'])
try:
    assert until(t,lambda:len(windows())==1,20),'the session starts'
    for _ in range(3):command('new-window','--','cat')
    assert until(t,lambda:len(windows())==4),windows()
    ALL=[1,2,3,4]

    # A half of the desktop, by key: menu R, then an arrow.
    assert command('list-windows').splitlines()[3].endswith('focused'),'the newest window has the keyboard'
    for arrow,want in (('left',(48,28,0,0)),('right',(48,28,50,0)),('up',(98,13,0,0)),('down',(98,13,0,15))):
        menu(t,'r',arrow)
        assert windows()[4][:4]==want,('menu R then %s is that half'%arrow,windows()[4])
    # And by the arrows on its frame. It is the bottom half: a frame 100 wide
    # from row 15, so its controls are at columns 92-95 and 97 (0-based).
    assert '←↑↓→'.encode() in t.seen and '○'.encode() in t.seen,'a frame shows its controls'
    command('mouse','press','96','16');command('mouse','release','96','16');t.pump(.15)
    assert windows()[4][:4]==(48,28,50,0),('the right arrow on a frame sends it to the right half',windows()[4])
    command('mouse','press','93','1');command('mouse','release','93','1');t.pump(.15)   # its left arrow, at 50+50-8
    assert windows()[4][:4]==(48,28,0,0),('the left arrow to the left half',windows()[4])
    command('mouse','press','48','1');command('mouse','release','48','1');t.pump(.15)   # its mark, at 50-3
    assert windows()[4][5],'the mark on a frame selects the window'
    assert '●'.encode() in t.seen,'and shows it selected'
    command('mouse','press','48','1');command('mouse','release','48','1');t.pump(.15)
    assert not windows()[4][5],'and again unselects it'

    # Layouts, with nothing selected: every window, sharing the desktop.
    menu(t,'l','g');ws=windows()
    assert shares_desktop(frames(ws,ALL)),('a grid shares the desktop exactly',ws)
    assert frames(ws,ALL)==[(0,0,50,15),(50,0,50,15),(0,15,50,15),(50,15,50,15)],('four windows are two by two, in order',ws)
    menu(t,'l','h');ws=windows()
    assert shares_desktop(frames(ws,ALL)) and all(w==COLS for _,_,w,_ in frames(ws,ALL)),('horizontal bars are each the full width',ws)
    assert [y for _,y,_,_ in frames(ws,ALL)]==[0,7,15,22],ws
    menu(t,'l','v');ws=windows()
    assert shares_desktop(frames(ws,ALL)) and all(h==ROWS for _,_,_,h in frames(ws,ALL)),('vertical bars are each the full height',ws)
    assert [x for x,_,_,_ in frames(ws,ALL)]==[0,25,50,75],ws
    menu(t,'l','2');ws=windows()
    assert frames(ws,ALL)==[(0,0,50,15),(0,15,50,15),(50,0,50,15),(50,15,50,15)],('two columns, two windows in each',ws)
    menu(t,'l','f');ws=windows()
    assert frames(ws,ALL)==[(0,0,COLS,ROWS)]*4,('stacked, each is the whole desktop',ws)
    menu(t,'l','esc')
    assert frames(windows(),ALL)==[(0,0,COLS,ROWS)]*4,'Esc leaves the layout as it was'

    # A selection: two of the four, by key. Only they are laid out, and they
    # come above the two left alone.
    menu(t,'1','x','3','x');ws=windows()
    assert [ws[i][5] for i in ALL]==[True,False,True,False],('menu X selects the focused window',ws)
    menu(t,'l','v');ws=windows()
    assert frames(ws,[1,3])==[(0,0,50,ROWS),(50,0,50,ROWS)],('the selected two share the desktop',ws)
    assert frames(ws,[2,4])==[(0,0,COLS,ROWS)]*2,('the others stay where they were',ws)
    assert min(ws[1][4],ws[3][4])>max(ws[2][4],ws[4][4]),('and under the ones laid out',ws)

    # Broadcast: typing in one of the selected goes to them all, and to no
    # other; typing in a window outside the selection stays there.
    menu(t,'b','1')
    assert painted(t,b'the 2 selected windows'),'the status line says who typing reaches'
    t.type(b'to-the-group\r')
    assert until(t,lambda:has(1,'to-the-group') and has(3,'to-the-group')),'typing reaches every selected window'
    t.pump(.3)
    assert not has(2,'to-the-group') and not has(4,'to-the-group'),'and no window outside the selection'
    menu(t,'2')
    assert painted(t,b'this window is not selected'),'the status line says typing stays here'
    t.type(b'just-two\r')
    assert until(t,lambda:has(2,'just-two')),'typing in an unselected window goes to it'
    t.pump(.3)
    assert not any(has(i,'just-two') for i in (1,3,4)),'and nowhere else'

    # The switch list selects too: Tab on a row.
    menu(t,'s','down','tab','esc')
    assert [windows()[i][5] for i in ALL]==[True,True,True,False],('Tab in the switch list selects the row',windows())

    # The selection is the server's state: it survives the server replacing
    # itself, as broadcast does, so typing does not start reaching windows
    # it did not before.
    command('upgrade')
    assert until(t,lambda:len(windows())==4,10),'the server comes back'
    assert [windows()[i][5] for i in ALL]==[True,True,True,False],('the selection survives an upgrade',windows())
    menu(t,'1')
    t.type(b'after-upgrade\r')
    assert until(t,lambda:all(has(i,'after-upgrade') for i in (1,2,3))),'broadcast still reaches the selected'
    t.pump(.3)
    assert not has(4,'after-upgrade'),'and still not the window outside'

    # Menu A: every window that shows, then none. With none selected,
    # broadcast is to every window, as it always was.
    menu(t,'a')
    assert all(windows()[i][5] for i in ALL),('menu A selects every window',windows())
    menu(t,'a')
    assert not any(windows()[i][5] for i in ALL),('and again, none',windows())
    assert painted(t,b'typing goes to every window'),'the status line says broadcast is to every window'
    t.type(b'to-everyone\r')
    assert until(t,lambda:all(has(i,'to-everyone') for i in ALL)),'with none selected broadcast reaches every window'
    menu(t,'b')
    t.type(b'alone\r')
    assert until(t,lambda:has(1,'alone'))
    t.pump(.3)
    assert not any(has(i,'alone') for i in (2,3,4)),'with broadcast off typing goes to the focused window only'

    # A hidden window that is selected is one of them: laying out shows it.
    menu(t,'4','h','4');ws=windows()
    menu(t,'1','x','4','x','l','h');ws=windows()
    assert 'hidden' not in command('list-windows'),'a selected window that was hidden shows when laid out'
    assert frames(ws,[1,4])==[(0,0,COLS,15),(0,15,COLS,15)],('the two selected are two bars',ws)
finally:
    subprocess.run([GV,'-L',LABEL,'kill-server'],capture_output=True,timeout=5)
    t.close()
print('regress/tiling.py: ok')
