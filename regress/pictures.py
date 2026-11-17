#!/usr/bin/env python3
"""Pictures on any terminal, end to end. A program asks whether it may send a
vector scene (f=200), sends one, places it under its text and then edits it.
The same program runs under four fake terminals, each with its own server:

  kitty   kitty graphics, but no vector images and no Silkie extensions
  sixel   sixel only
  text    no pictures of any kind, as GNOME Terminal
  off     the same, with cell-pictures = no in the configuration

The program is told yes in the first three, since odin-seat draws the scene
itself for a terminal that cannot. The kitty terminal is sent it as pixels,
the right ones, and as pixels again after the edit, never the records; the
sixel terminal as sixel; the text terminal as coloured half-block cells, with
the program's text on the picture's colour. In the fourth the program is not
answered, as on such a terminal it never was.

The program also asks how wide a word will be and sets it in the scene. With
no terminal attached that draws scenes, odin-seat answers from its own text
layout and draws the word with the rest."""
import base64,fcntl,os,pty,re,select,struct,subprocess,sys,tempfile,termios,time,zlib
from pathlib import Path
GV=os.environ.get('GV',str(Path(__file__).resolve().parents[1]/'odin-seat'))
ST=b'\x1b\\'
UPPER='▀'.encode()

class Terminal:
    def __init__(self,kind,argv,label,config):
        self.kind,self.label=kind,label
        self.cols,self.rows,self.cw,self.ch=100,30,10,20
        self.master,slave=pty.openpty()
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',self.rows,self.cols,self.cols*self.cw,self.rows*self.ch))
        env=dict(os.environ,TERM='xterm-256color',COLORTERM='truecolor',ODIN_SEAT_KITTY_MEDIUM='direct',
            ODIN_SEAT_CONFIG=config,PYENV_REHASH_TIMEOUT='1')
        self.proc=subprocess.Popen([GV,'-L',label,*argv],stdin=slave,stdout=slave,stderr=slave,env=env)
        os.close(slave)
        self.seen=bytearray();self.done=set()
    def bootstrap(self):
        table=[(b'\x1b[16t',b'\x1b[6;%d;%dt'%(self.ch,self.cw)),(b'\x1b[18t',b'\x1b[8;%d;%dt'%(self.rows,self.cols)),
            (b'\x1b[>q',b'\x1bP>|plain(1.0)'+ST),
            (b'\x1b[c',b'\x1b[?62;4;22c' if self.kind=='sixel' else b'\x1b[?62;22c')]
        if self.kind=='kitty':table.insert(0,(b'a=q',b'\x1b_Gi=31;OK'+ST))
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
    def command(self,*args):
        r=subprocess.run([GV,'-L',self.label,*args],capture_output=True,timeout=10)
        assert r.returncode==0,(args,r.stderr,r.stdout)
        return r.stdout.decode()
    def wait(self,what,start=0,seconds=8):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            self.pump(.05)
            if what in self.seen[start:]:return
        raise AssertionError((self.kind,what,bytes(self.seen[-400:])))
    def close(self):
        subprocess.run([GV,'-L',self.label,'kill-server'],capture_output=True,timeout=5)
        os.close(self.master)
        try:self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:self.proc.kill()

def images(data):
    """Every image transmitted: (keys, bytes), chunks joined, deflate undone."""
    out=[]
    for m in re.finditer(rb'\x1b_G(a=t,[^;]*);([^\x1b]*)\x1b\\((?:\x1b_Gm=[01];[^\x1b]*\x1b\\)*)',data):
        raw=base64.b64decode(m.group(2)+b''.join(re.findall(rb'\x1b_Gm=[01];([^\x1b]*)\x1b\\',m.group(3))))
        if b'o=z' in m.group(1):raw=zlib.decompress(raw)
        out.append((m.group(1),raw))
    return out

def half_blocks(data):
    """Cells of upper half block, a repeated one (REP) counted as many."""
    return data.count(UPPER)+sum(int(n) for n in re.findall(UPPER+rb'\x1b\[(\d+)b',data))

def pixel(image,x,y):
    keys,raw=image
    w=int(re.search(rb's=(\d+)',keys).group(1));n=3 if b'f=24' in keys else 4
    return tuple(raw[(y*w+x)*n:(y*w+x)*n+3])

# 60 by 40 pixels, six cells by two: the left half red, the right half green.
SCENE=b'p id=1 d=M0,0L30,0L30,40L0,40Z fill=#ff0000\np id=2 d=M30,0L60,0L60,40L30,40Z fill=#00ff00\n'
CHILD=r'''
import base64,json,os,sys,tty
tty.setraw(0)
def gfx(k,p=b''):os.write(1,b'\x1b_G'+k+b';'+p+b'\x1b\\')
os.write(1,b'\x1b[2J')     # the window's greeting would be text over the picture
# May a scene be sent? The answer, if any, comes before the device attributes.
def ask(k,p):
    gfx(k,p);os.write(1,b'\x1b[c')
    got=b''
    while not got.endswith(b'c') or b'\x1b[?' not in got:
        got+=os.read(0,1)
    return got
yes=b'_Gi=31;OK' in ask(b'i=31,a=q,f=200,s=1,v=1',base64.b64encode(b'p id=1 d=M0,0L1,0L1,1Z\n'))
os.write(1,b'\x1b[6;1H'+(b'VECTOR-YES' if yes else b'VECTOR-NO'))
if yes:
    # How wide will a word be? Asked of whoever draws the scenes.
    got=ask(b'a=v,f=200,i=41',base64.b64encode(b'{"op":"measure","text":"Hi","size":20,"role":"sans"}'))
    wide=0
    if b'_Gi=41;OK;' in got:
        reply=json.loads(base64.b64decode(got.split(b'_Gi=41;OK;')[1].split(b'\x1b')[0]))
        wide=reply['width']
    # Text in a scene is drawn only where the service lists "text" (an
    # odin-seat built with a HarfBuzz older than 4.0 measures it only).
    got=ask(b'a=v,f=200,i=42',base64.b64encode(b'{"op":"fonts"}'))
    if b'_Gi=42;OK;' not in got or 'text' not in json.loads(base64.b64decode(
            got.split(b'_Gi=42;OK;')[1].split(b'\x1b')[0])).get('features',[]):
        wide=0
    os.write(1,b'\x1b[5;1H'+(b'MEASURED-%d' % round(wide) if wide else b'UNMEASURED'))
    text=b't id=3 text='+base64.b64encode(b'{"text":"Hi","size":20,"role":"sans","x":32,"y":28,"fill":"#000000"}')+b'\n'
    gfx(b'a=t,f=200,i=2,s=60,v=40,q=2',base64.b64encode(sys.argv[1].encode()+(text if wide else b'')))
    os.write(1,b'\x1b[1;1H');gfx(b'a=p,i=2,p=1,z=-1,C=1,q=2')
    os.write(1,b'\x1b[1;2HHI\x1b[7;1HDRAWN')
    os.read(0,1)
    gfx(b'a=e,i=2,q=2',base64.b64encode(b'p id=1 d=M0,0L30,0L30,40L0,40Z fill=#0000ff\n'))
    os.write(1,b'\x1b[7;1HEDITS')
os.read(0,1)
'''

with tempfile.TemporaryDirectory() as temp:
    child=Path(temp)/'child.py';child.write_text(CHILD)
    on=Path(temp)/'on';on.write_text('')
    off=Path(temp)/'off';off.write_text('cell-pictures = no\n')
    for kind in ('kitty','sixel','text','off'):
        t=Terminal('text' if kind=='off' else kind,['new-session','--',sys.executable,str(child),SCENE.decode()],
            'pictures-%s-%d'%(kind,os.getpid()),str(off if kind=='off' else on))
        try:
            if kind=='off':
                t.wait(b'VECTOR-NO',seconds=20)
                assert UPPER not in t.seen,'with cell-pictures off a terminal with no pictures is drawn none'
                continue
            t.wait(b'VECTOR-YES',seconds=20)
            t.wait(b'DRAWN');t.pump(.4)
            data=bytes(t.seen)
            # No terminal here draws scenes, so the measuring is done by
            # odin-seat, in a font of this machine if it has one: a word of
            # two letters at size 20 is between 8 and 30 wide.
            m=re.search(rb'MEASURED-(\d+)',data);fonts=m is not None
            assert fonts or b'UNMEASURED' in data,(kind,'the program is answered about its text')
            assert not fonts or 8<=int(m.group(1))<=30,(kind,'with a width a word could have',m.group(1))
            assert b'f=200' not in data and b'\x1b_Ga=e' not in data,(kind,'no terminal here draws scenes: none is sent the records')
            if kind=='kitty':
                sent=images(data)
                assert len(sent)==1 and b's=60,v=40' in sent[0][0] and (b'f=24' in sent[0][0] or b'f=32' in sent[0][0]),[k for k,_ in sent]
                assert pixel(sent[0],5,20)==(255,0,0) and pixel(sent[0],58,3)==(0,255,0),'the scene is sent drawn, as its pixels'
                inked=sum(1 for y in range(40) for x in range(30,60) if pixel(sent[0],x,y)==(0,0,0))
                assert not fonts or inked>=15,('with its text set in black on the green half',inked)
                assert all(pixel(sent[0],x,y)==(255,0,0) for y in range(0,40,3) for x in range(0,28,3)),'and none of it on the red'
                assert re.search(rb'\x1b_Ga=p,i=\d+,p=1,x=0,y=0,w=60,h=40,z=-1,C=1,q=2',data),'and placed under the text like any picture'
                assert b'HI' in data
            elif kind=='sixel':
                assert re.search(rb'\x1bP[0-9;]*q',data) and b'\x1b_Ga=t' not in data,'a sixel terminal is sent the scene drawn, as sixel'
                # A sixel names its colours in percent: the scene's red and green.
                assert re.search(rb'#\d+;2;100;0;0',data) and re.search(rb'#\d+;2;0;100;0',data),'in the scene\'s colours'
            else:
                assert b'\x1b_Ga=t' not in data and not re.search(rb'\x1bP[0-9;]*q',data),'a terminal with no pictures is sent no pixels'
                assert half_blocks(data)==6*2-2,('the scene is cells of half blocks, but the two with text',half_blocks(data))
                # Red is all picture; the scene's text may ink every green
                # cell a little, depending on the fonts of the machine, so
                # green need only be one half of a cell.
                assert b'38;2;255;0;0;48;2;255;0;0' in data and (b'38;2;0;255;0;' in data or b'48;2;0;255;0' in data),'in the scene\'s colours'
                assert re.search(rb'48;2;255;0;0m?[^\x1b]*HI',data),'with the program\'s text on the colour of the picture under it'
            # The edit: the left half blue.
            n=len(t.seen);t.command('send-keys','x');t.wait(b'EDITS',n);t.pump(.5)
            new=bytes(t.seen[n:])
            assert b'\x1b_Ga=e' not in new,(kind,'an edit of records is not sent to a terminal holding pixels')
            if kind=='kitty':
                sent=images(new)
                assert sent and pixel(sent[-1],5,20)==(0,0,255) and pixel(sent[-1],58,3)==(0,255,0),'an edited scene is drawn and sent again'
            elif kind=='sixel':
                assert re.search(rb'\x1bP[0-9;]*q',new) and re.search(rb'#\d+;2;0;0;100',new),'an edited scene is drawn again as sixel, in its new colour'
                assert b'\x1b[2J' not in new,'and only what changed of it: the screen is not erased'
            else:
                assert b'38;2;0;0;255;48;2;0;0;255' in new,'an edited scene changes the cells that show it'
        finally:
            t.close()
print('regress/pictures.py: ok')
