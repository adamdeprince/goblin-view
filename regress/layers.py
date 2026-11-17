#!/usr/bin/env python3
"""Layered windows end to end: a fake terminal on a PTY, a real client and
server, and a program that draws as a sprite engine does. Its pictures go
under its text, part way into cells and over one another, which placeholder
cells cannot say, so the terminal must be sent real placements: each image
once, each placement at its z, cut to the cells where the window shows. The
terminal's stream is replayed into a model of what it would hold, and the
pixels that model shows are compared with the window's visible cells: whole
when nothing covers it, in pieces under another window's frame, none when it
is covered, and scaled to its own cell on a second terminal whose cells are
half the size, while the desktop shrinks to the smaller of the two.
Also here: a synchronized update (mode 2026) reaching the terminal as one
frame, one that never ends not freezing the window, and more sized-text runs
than the old table held, with their fractional scale."""
import base64,fcntl,os,pty,re,select,struct,subprocess,sys,tempfile,termios,time,zlib
from pathlib import Path
GV=os.environ.get('GV',str(Path(__file__).resolve().parents[1]/'odin-seat'))
LABEL='layers-'+str(os.getpid())
ST=b'\x1b\\'
HOLDER='\U0010eeee'.encode()

def command(*args):
    r=subprocess.run([GV,'-L',LABEL,*args],capture_output=True,timeout=10)
    assert r.returncode==0,(args,r.stderr,r.stdout)
    return r.stdout.decode()

def windows():
    found={}
    for line in command('list-windows').splitlines():
        m=re.match(r'[^:]+:(\d+): .* \((\d+)x(\d+)\) (-?\d+),(-?\d+) z=(-?\d+)',line)
        assert m,line
        found[int(m[1])]=tuple(map(int,m.groups()[1:]))
    return found

class Terminal:
    """Answers the bootstrap as Silkie does, and keeps what it was sent."""
    def __init__(self,argv,cols=100,rows=30,cw=10,ch=20):
        self.cols,self.rows,self.cw,self.ch=cols,rows,cw,ch
        self.master,slave=pty.openpty()
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',rows,cols,cols*cw,rows*ch))
        env=dict(os.environ,TERM='xterm-kitty',ODIN_SEAT_KITTY_MEDIUM='direct',PYENV_REHASH_TIMEOUT='1')
        self.proc=subprocess.Popen([GV,'-L',LABEL,*argv],stdin=slave,stdout=slave,stderr=slave,env=env)
        os.close(slave)
        self.seen=bytearray();self.done=set()
    def bootstrap(self):
        cap=lambda sel,qid,f:b'\x1b]777;'+sel+b';v=1:op=cap:id='+qid+b':'+f+ST
        table=((b'\x1b[16t',b'\x1b[6;%d;%dt'%(self.ch,self.cw)),(b'\x1b[18t',b'\x1b[8;%d;%dt'%(self.rows,self.cols)),
            (b'a=q',b'\x1b_Gi=31;OK'+ST),(b'\x1b[>q',b'\x1bP>|silkie(0.50)'+ST),
            (b'caps;v=1:op=query:id=17349523',cap(b'caps',b'17349523',b'caps=3:ext=caps.1,display.1:media=d,f,t,s:format=24,32,64,100,200')),
            (b'display;v=1:op=query:id=17349524',cap(b'display',b'17349524',b'mode=0:kind=lcd:color=rgb:bits=8:gamut=srgb:headroom=1:scale=2:period=8333:ppi=254')),
            (b'\x1b[c',b'\x1b[?62;22c'))
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
    def close(self):
        os.close(self.master)
        try:self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:self.proc.kill()

TOKEN=re.compile(rb'\x1b\[(\d+);(\d+)H|\x1b\[2J|\x1b_G([^;\x1b]*)(?:;[^\x1b]*)?\x1b\\')
def model(data):
    """What a terminal sent this holds: images, and placements by (image, id)
    with the cell they were made at. Every command must name an image it has."""
    row=col=0;images=set();places={};edits=[]
    for m in TOKEN.finditer(data):
        if m.group(1):
            row,col=int(m.group(1))-1,int(m.group(2))-1;continue
        if m.group(0)==b'\x1b[2J':
            places.clear();continue        # erasing the screen takes its placements
        keys=dict(k.split(b'=',1) for k in m.group(3).split(b',') if b'=' in k)
        act=keys.get(b'a',b't');image=int(keys.get(b'i',0))
        if act==b't' and image:
            images.add(image)
            for key in [k for k in places if k[0]==image]:del places[key]
        elif act==b'p' and b'U' not in keys:
            assert image in images,('a placement of an image the terminal was not sent',m.group(0))
            assert keys.get(b'C')==b'1' and keys.get(b'q')==b'2',m.group(0)
            places[(image,int(keys[b'p']))]=dict(row=row,col=col,**{k.decode():int(v) for k,v in keys.items() if k in (b'x',b'y',b'w',b'h',b'X',b'Y',b'z',b'c',b'r')})
        elif act==b'd':
            if keys.get(b'd')==b'I':
                images.discard(image)
                for key in [k for k in places if k[0]==image]:del places[key]
            elif keys.get(b'd')==b'i' and b'p' in keys:
                places.pop((image,int(keys[b'p'])),None)
        elif act==b'e':
            assert image in images,('an edit of an image the terminal was not sent',m.group(0)[:80])
            edits.append(image)
    return images,places,edits

def shown(t,places,z):
    """Screen pixels covered at this z: {(x, y): (image, source x, source y)}."""
    out={}
    for (image,pid),p in places.items():
        if p.get('z',0)!=z:continue
        assert 'c' not in p and 'r' not in p and 'w' in p,p
        left=p['col']*t.cw+p.get('X',0);top=p['row']*t.ch+p.get('Y',0)
        for dy in range(p['h']):
            for dx in range(p['w']):
                assert (left+dx,top+dy) not in out,'two pieces of one placement overlap'
                out[(left+dx,top+dy)]=(image,p['x']+dx,p['y']+dy)
    return out

def visible_cells(win,others,cols,rows):
    """The cells of a window's inside that no later frame covers."""
    w,h,x,y,z=win;cells=set()
    for r in range(y+1,y+1+h):
        for c in range(x+1,x+1+w):
            if not (0<=c<cols and 0<=r<rows):continue
            if any(oz>z and ox<=c<ox+ow+2 and oy<=r<oy+oh+2 for ow,oh,ox,oy,oz in others):continue
            cells.add((c,r))
    return cells

def expect(t,win,others,image_at,size,sprite=None):
    """The pixels a picture should show: its rectangle, in the window's visible cells."""
    w,h,x,y,z=win;cells=visible_cells(win,others,t.cols,t.rows)
    col,row,X,Y=image_at;sx,sy,sw,sh=sprite or (0,0)+size
    left=(x+1+col)*t.cw+X;top=(y+1+row)*t.ch+Y;out={}
    for dy in range(sh):
        for dx in range(sw):
            px,py=left+dx,top+dy
            if (px//t.cw,py//t.ch) in cells:out[(px,py)]=(sx+dx,sy+dy)
    return out

def rgba(w,h):
    return base64.b64encode(zlib.compress(bytes([40,90,160,200])*(w*h)))

CHILD=r'''
import base64,os,sys,time,tty
tty.setraw(0)
def gfx(k,p=b''):os.write(1,b'\x1b_G'+k+b';'+p+b'\x1b\\')
def key():return os.read(0,1)
PIX=sys.argv[1].encode()
# A map under everything, a sprite over it part way into a cell, text over both.
gfx(b'a=t,f=32,s=120,v=80,o=z,i=1,q=2',PIX)
os.write(1,b'\x1b[1;1H');gfx(b'a=p,i=1,p=1,z=-2000000000,C=1,q=2')
os.write(1,b'\x1b[2;2H');gfx(b'a=p,i=1,p=7,x=20,y=10,w=33,h=27,X=3,Y=7,z=-1990000000,C=1,q=2')
os.write(1,b'\x1b[2;2HLABEL\x1b[20;1HDRAWN')
key()
# A vector scene, edited where it stands.
scene=base64.b64encode(b'p id=1 d=M0,0L9,9\n')
gfx(b'a=t,f=200,i=2,s=60,v=40,q=2',scene)
os.write(1,b'\x1b[10;40H');gfx(b'a=p,i=2,p=1,z=-5,C=1,q=2')
os.write(1,b'\x1b[20;1HSCENE')
key()
gfx(b'a=e,i=2,q=2',base64.b64encode(b'p id=2 d=M1,1L3,3\n'))
os.write(1,b'\x1b[20;1HEDIT ')
key()
# While covered: the sprite moves. Nothing of it should be sent.
os.write(1,b'\x1b[1;3H');gfx(b'a=p,i=1,p=7,x=20,y=10,w=33,h=27,X=1,Y=2,z=-1990000000,C=1,q=2')
os.write(1,b'\x1b[20;1HMOVED')
key()
# One update, drawn in two halves with a pause between.
# (Spinning, not sleeping: a sleep can run long, and an update is only
# held for so long.)
os.write(1,b'\x1b[?2026h\x1b[18;1HHALF1')
spin=time.monotonic()
while time.monotonic()-spin<.04:pass
os.write(1,b' HALF2\x1b[?2026l')
key()
# Sized text: more runs than fit the old table, at four fifths the size.
for i in range(60):
    os.write(1,b'\x1b[%d;%dH\x1b]66;n=4:d=5:w=2:v=2;%02d\x1b\\'%(12+i//20,1+(i%20)*2,i))
os.write(1,b'\x1b[20;1HRUNS!')
key()
# The scene again, while a second terminal holds it scaled: one edit of an
# element, one of the root group that a scaled copy has its own of.
gfx(b'a=e,i=2,q=2',base64.b64encode(b'p id=3 d=M2,2L4,4\n'))
os.write(1,b'\x1b[20;1HEDIT2')
key()
gfx(b'a=e,i=2,q=2',base64.b64encode(b'g id=0 t=2,0,0,2,-10,-5\n'))
os.write(1,b'\x1b[20;1HZOOMS')
key()
# An update that never ends must not freeze the window.
os.write(1,b'\x1b[?2026h\x1b[19;1HSTUCK')
time.sleep(600)
'''

def wait(t,what,start=0,seconds=6,others=()):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        t.pump(.05)
        for o in others:o.pump(.01)
        if what in t.seen[start:]:return
    raise AssertionError((what,bytes(t.seen[-600:])))

with tempfile.TemporaryDirectory() as temp:
    child=Path(temp)/'child.py';child.write_text(CHILD)
    t1=Terminal(['new-session','--',sys.executable,str(child),rgba(120,80).decode()])
    t2=None
    try:
        wait(t1,b'DRAWN',seconds=20);t1.pump(.3)
        win=windows()[1]
        images,places,_=model(bytes(t1.seen))
        assert HOLDER not in t1.seen,'a layered window has no placeholder cells'
        assert len(images)==1 and len(places)==2,(images,places)
        assert len(re.findall(rb'\x1b_Ga=t',bytes(t1.seen)))==1,'the image is sent once, whatever its placements'
        hid=next(iter(images))
        # Whole: the map and the sprite, each exactly where the program put it.
        got=shown(t1,places,-2000000000);want=expect(t1,win,[],(0,0,0,0),(120,80))
        assert {k:v[1:] for k,v in got.items()}==want,'the map shows whole, pixel for pixel'
        got=shown(t1,places,-1990000000);want=expect(t1,win,[],(1,1,3,7),(120,80),(20,10,33,27))
        assert {k:v[1:] for k,v in got.items()}==want,'the sprite keeps its source rectangle and its offset in the cell'
        assert b'LABEL' in t1.seen,'the text over the pictures is text'

        # A vector scene, then an edit of it: forwarded, not sent again.
        n=len(t1.seen);command('send-keys','a');wait(t1,b'SCENE',n);t1.pump(.2)
        images,places,_=model(bytes(t1.seen));assert len(images)==2 and len(places)==3,(images,places)
        scene=next(i for i in images if i!=hid)
        n=len(t1.seen);command('send-keys','b');wait(t1,b'EDIT',n);t1.pump(.2)
        _,_,edits=model(bytes(t1.seen))
        assert edits==[scene],('an edit goes to the one copy of the scene',edits)
        assert len(re.findall(rb'\x1b_Ga=t,i=%d,'%scene,bytes(t1.seen)))==1,'an edited scene is not sent again'
        m=re.search(rb'\x1b_Ga=e,i=%d,q=2,m=0;([^\x1b]*)'%scene,bytes(t1.seen))
        assert m and base64.b64decode(m.group(1))==b'p id=2 d=M1,1L3,3\n',m

        # Another window's frame over part of it: the pieces that show, exactly.
        command('new-window','--','sleep','300')
        for _ in range(40):
            t1.pump(.05)
            if len(windows())==2:break
        t1.pump(.4)
        ws=windows();win,other=ws[1],ws[2];assert other[4]>win[4],ws
        images,places,_=model(bytes(t1.seen))
        whole=expect(t1,win,[],(0,0,0,0),(120,80));want=expect(t1,win,[other],(0,0,0,0),(120,80))
        assert 0<len(want)<len(whole),('the new window covers part of the map, not all',len(want),len(whole),ws)
        got=shown(t1,places,-2000000000)
        assert {k:v[1:] for k,v in got.items()}==want,'under a frame the map is cut to the cells that show'
        assert sum(1 for k in places if k[0]==hid and places[k].get('z')==-2000000000)>1,'in more than one piece'
        assert len(re.findall(rb'\x1b_Ga=t,i=%d,'%hid,bytes(t1.seen)))==1,'cutting sends no pixels'
        got=shown(t1,places,-1990000000);want=expect(t1,win,[other],(1,1,3,7),(120,80),(20,10,33,27))
        assert {k:v[1:] for k,v in got.items()}==want and 0<len(want)<33*27,'and the sprite to the part of it that shows'

        # Wholly covered: nothing is shown, and what it draws sends nothing.
        command('menu','z');t1.pump(.5)
        images,places,_=model(bytes(t1.seen));assert not places,('a covered window shows none of its pictures',places)
        n=len(t1.seen);command('send-keys','-w','1','c');t1.pump(.5)
        assert b'\x1b_G' not in t1.seen[n:],'a covered window sends the terminal no graphics'
        command('menu','z');t1.pump(.5)
        images,places,_=model(bytes(t1.seen))
        got=shown(t1,places,-1990000000);want=expect(t1,win,[windows()[2]],(2,0,1,2),(120,80),(20,10,33,27))
        assert {k:v[1:] for k,v in got.items()}==want and 0<len(want)<33*27,'uncovered, the sprite is where it moved to meanwhile'
        assert len(re.findall(rb'\x1b_Ga=t,i=%d,'%hid,bytes(t1.seen)))==1,'and is placed from the image the terminal holds'
        # The window over it closes: whole again, in one piece each.
        command('kill-window','-w','2')
        for _ in range(40):
            t1.pump(.05)
            if len(windows())==1:break
        t1.pump(.4)
        images,places,_=model(bytes(t1.seen));win=windows()[1]
        got=shown(t1,places,-2000000000);want=expect(t1,win,[],(0,0,0,0),(120,80))
        assert {k:v[1:] for k,v in got.items()}==want and len(places)==3,'with nothing over it each picture is one placement again'

        # A synchronized update is one frame, however slowly it is drawn.
        n=len(t1.seen);command('send-keys','-w','1','d');wait(t1,b'HALF2',n);t1.pump(.2)
        new=bytes(t1.seen[n:]);a=new.index(b'HALF1');b=new.index(b'HALF2')
        assert b'\x1b[?2026l' not in new[a:b],'the two halves of an update reach the terminal in one frame'

        # Sized text: every run, with its fractional scale.
        n=len(t1.seen);command('send-keys','-w','1','e');wait(t1,b'RUNS!',n);t1.pump(.3)
        runs=re.findall(rb'\x1b\]66;s=1:w=2:n=4:d=5:v=2;(\d\d)\x1b\\',bytes(t1.seen[n:]))
        assert sorted(set(int(r) for r in runs))==list(range(60)),('every run, past the 32 of the old table',len(runs))

        # A second terminal, smaller and with cells half the size. Rows and
        # columns follow the smaller of the two, so both show the whole
        # desktop; the cell the programs draw for stays the larger, and the
        # terminal with the smaller is sent each picture scaled to its own,
        # as real placements in its own pixels. Nobody gets placeholder cells.
        assert '100x30' in command('list-sessions'),command('list-sessions')
        before=windows()[1]
        n1=len(t1.seen)
        t2=Terminal(['attach'],cols=90,rows=28,cw=5,ch=10)
        for _ in range(80):
            t2.pump(.05);t1.pump(.02)
            if '90x28' in command('list-sessions') and len(model(bytes(t2.seen))[1])>=3:break
        t2.pump(.4);t1.pump(.2)
        assert '90x28' in command('list-sessions'),('the desktop is the smaller terminal in each dimension',command('list-sessions'))
        win=windows()[1]
        assert win[0]+2<=90 and win[1]+2<=28,('the window fits the desktop both show',win)
        images2,places2,_=model(bytes(t2.seen))
        assert HOLDER not in t2.seen and HOLDER not in t1.seen[n1:],'neither terminal is shown the pictures as cells'
        sent=re.findall(rb'\x1b_Ga=t,i=%d,([^;]*);([^\x1b]*)'%hid,bytes(t2.seen))
        assert len(sent)==1 and b's=60,v=40' in sent[0][0],('the map goes to it at half the size',[k for k,_ in sent])
        keys,payload=sent[0];raw=base64.b64decode(payload)
        if b'o=z' in keys:raw=zlib.decompress(raw)
        assert b'f=32' in keys and raw==bytes([40,90,160,200])*(60*40),'and its pixels are the picture, scaled'
        got=shown(t2,places2,-2000000000);want=expect(t2,win,[],(0,0,0,0),(60,40))
        assert {k:v[1:] for k,v in got.items()}==want,'the scaled map covers the cells the map covers'
        got=shown(t2,places2,-1990000000);want=expect(t2,win,[],(2,0,1,1),(60,40),(10,5,17,14))
        assert {k:v[1:] for k,v in got.items()}==want,'the sprite is the same part of it, at its scaled offset'
        # A vector scene has no pixels to scale: it goes as the same records
        # in half the size, its root group scaling everything to that.
        def scenes(data):
            return [(k,base64.b64decode(b''.join(re.findall(rb';([^\x1b]*)\x1b',body))))
                for k,body in re.findall(rb'\x1b_Ga=t,i=%d,([^;]*)((?:;[^\x1b]*\x1b\\(?:\x1b_Gm=[01])?)+)'%scene,data)]
        sent=scenes(bytes(t2.seen))
        assert len(sent)==1 and b's=30,v=20' in sent[0][0],('the scene goes to it at half the size',[k for k,_ in sent])
        assert sent[0][1]==b'g id=0 t=0.5,0,0,0.5,0,0\np id=1 d=M0,0L9,9\np id=2 d=M1,1L3,3\n',sent[0][1]
        placed=[p for (i,_),p in places2.items() if i==scene]
        assert len(placed)==1 and (placed[0]['w'],placed[0]['h'])==(30,20) and 'c' not in placed[0],('and is placed at that size, like any picture',placed)
        # An edit of an element is the same edit for both terminals.
        n=len(t1.seen);n2=len(t2.seen);command('send-keys','-w','1','f');wait(t1,b'EDIT2',n,others=(t2,));t2.pump(.3)
        for t,start in ((t1,n),(t2,n2)):
            m=re.search(rb'\x1b_Ga=e,i=%d,q=2,m=0;([^\x1b]*)'%scene,bytes(t.seen[start:]))
            assert m and base64.b64decode(m.group(1))==b'p id=3 d=M2,2L4,4\n','an edit of an element goes to both as it is'
        assert len(scenes(bytes(t2.seen)))==1,'and the scene is not sent again for it'
        # An edit of the root group is not: the scaled copy has its own. The
        # terminal holding it scaled is sent the scene again, with the
        # program's transform followed by the scaling.
        n=len(t1.seen);n2=len(t2.seen);command('send-keys','-w','1','g');wait(t1,b'ZOOMS',n,others=(t2,))
        for _ in range(40):
            t2.pump(.05);t1.pump(.02)
            if len(scenes(bytes(t2.seen)))==2:break
        m=re.search(rb'\x1b_Ga=e,i=%d,q=2,m=0;([^\x1b]*)'%scene,bytes(t1.seen[n:]))
        assert m and base64.b64decode(m.group(1))==b'g id=0 t=2,0,0,2,-10,-5\n','the terminal with the scene as it is takes the edit as it is'
        assert not re.search(rb'\x1b_Ga=e,i=%d,'%scene,bytes(t2.seen[n2:])),'the one with it scaled is not sent that edit'
        sent=scenes(bytes(t2.seen))
        assert len(sent)==2 and b's=30,v=20' in sent[1][0],('it is sent the scene again',len(sent))
        assert b'g id=0 t=1,0,0,1,-5,-2.5\n' in sent[1][1] and b'p id=3 d=M2,2L4,4\n' in sent[1][1],sent[1][1]
        _,places2,_=model(bytes(t2.seen))
        assert [p for (i,_),p in places2.items() if i==scene],'and it is placed again'
        # The first terminal still has the pictures as they were drawn.
        images,places,_=model(bytes(t1.seen))
        got=shown(t1,places,-2000000000);want=expect(t1,win,[],(0,0,0,0),(120,80))
        assert {k:v[1:] for k,v in got.items()}==want,'the terminal whose cell the programs draw for is unchanged'
        # It leaves: the desktop is the one terminal's again.
        t2.close();t2=None
        for _ in range(80):
            t1.pump(.05)
            if '100x30' in command('list-sessions'):break
        assert '100x30' in command('list-sessions'),('a terminal that leaves gives its rows and columns back',command('list-sessions'))
        assert windows()[1][:4]==before[:4],('and the window is where it was before the other attached',windows()[1],before)

        # An update that never ends is shown all the same, after a moment.
        n=len(t1.seen);command('send-keys','-w','1','h');wait(t1,b'STUCK',n,seconds=3)
    finally:
        subprocess.run([GV,'-L',LABEL,'kill-server'],capture_output=True,timeout=5)
        t1.close()
        if t2:t2.close()
print('regress/layers.py: ok')
