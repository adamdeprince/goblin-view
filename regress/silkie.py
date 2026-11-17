#!/usr/bin/env python3
"""Silkie features end to end: a fake terminal on a PTY, a real client and
server, and a real program inside. Covers what unit tests cannot: bootstrap of
font.1, a vector reply crossing client to server at more than the old 1 MiB
message size, font styles in painted frames, session fonts and scenes replayed
to a second terminal, an edit reaching only those that hold the scene, a stroke
of terminal-drawn ink reaching the table and the other terminals, and styles
surviving a live server upgrade, a palette image, its palette update and
indexed colour expanded for a terminal that did not list them, a feature
query, and stereo depth on text and on a packed
image expanded for a terminal that did not list packed formats, with
--3d-focus bringing the focused window nearer on one terminal only."""
import base64,fcntl,json,os,pty,re,select,struct,subprocess,sys,tempfile,termios,time
from pathlib import Path
GV=os.environ.get('GV',str(Path(__file__).resolve().parents[1]/'odin-seat'))
LABEL='silkie-'+str(os.getpid())
ST=b'\x1b\\'
FORMATS=b'24,32,64,100,200'

def command(*args):
    r=subprocess.run([GV,'-L',LABEL,*args],capture_output=True,timeout=10)
    assert r.returncode==0,(args,r.stderr,r.stdout)
    return r.stdout

class Terminal:
    """Answers the bootstrap as Silkie does, and vector requests from a table."""
    def __init__(self,argv,huge=0):
        self.master,slave=pty.openpty()
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',30,100,1000,600))
        env=dict(os.environ,TERM='xterm-kitty',ODIN_SEAT_KITTY_MEDIUM='direct',PYENV_REHASH_TIMEOUT='1')
        self.proc=subprocess.Popen([GV,'-L',LABEL,*argv],stdin=slave,stdout=slave,stderr=slave,env=env)
        os.close(slave)
        self.seen=bytearray();self.done=set();self.huge=huge;self.scan=0;self.requests=[];self.partial={}
    def cap(self,sel,qid,fields):
        return b'\x1b]777;'+sel+b';v=1:op=cap:id='+qid+b':'+fields+ST
    def pointer(self):
        m=re.findall(rb'\x1b\]22;(image:\d+:\d+:\d+)\x1b\\',bytes(self.seen))
        if m:self.shape=m[-1]
        if b'22;?__current__' in self.seen and 'cur' not in self.done and getattr(self,'shape',None):
            os.write(self.master,b'\x1b]22;'+self.shape+ST);self.done.add('cur')
    def bootstrap(self):
        table=((b'\x1b[16t',b'\x1b[6;20;10t'),(b'\x1b[18t',b'\x1b[8;30;100t'),
            (b'a=q',b'\x1b_Gi=31;OK'+ST),(b'\x1b[>q',b'\x1bP>|silkie(0.50)'+ST),
            (b'caps;v=1:op=query:id=17349523',self.cap(b'caps',b'17349523',b'caps=3:ext=caps.1,font.1,ink.1,display.1,tablet-pointer.1,depth.1:media=d,f,t,s:format='+FORMATS)),
            (b'display;v=1:op=query:id=17349524',self.cap(b'display',b'17349524',b'mode=0:kind=eink:color=gray:bits=4:gamut=srgb:headroom=2.5:scale=2:period=0:ppi=0')),
            (b'font;v=1:op=query:id=17349532',self.cap(b'font',b'17349532',b'caps=1:roles=default,sans,serif:weight_min=100:weight_max=900:slant_min=-1500:slant_max=1500:synthetic_slant=1')),
            (b'ink;v=1:op=query:id=17349528',self.cap(b'ink',b'17349528',b'caps=1:mode=0')),
            (b'tablet-pointer;v=1:op=query:id=17349531',self.cap(b'tablet-pointer',b'17349531',b'caps=1:mode=0')),
            (b'\x1b[c',b'\x1b[?62;22c'))
        for q,r in table:
            if q in self.seen and q not in self.done:
                os.write(self.master,r);self.done.add(q)
    def service(self):
        """Reassemble a=v requests that want an answer, and answer them."""
        for m in re.finditer(rb'\x1b_G([^;\x1b]*);([^\x1b]*)\x1b\\',bytes(self.seen[self.scan:])):
            self.scan_to=self.scan+m.end()
            keys,payload=m.group(1),m.group(2)
            if keys.startswith(b'a=v'):
                self.cur=dict(k.split(b'=') for k in keys.split(b','));self.buf=payload
            elif keys.startswith(b'm=') and getattr(self,'cur',None) is not None:
                self.buf+=payload
            else:
                continue
            if b'm=1' in keys:continue
            req=json.loads(base64.b64decode(self.buf));hid=self.cur[b'i'];quiet=self.cur[b'q']==b'2';self.cur=None
            self.requests.append((req,quiet))
            if quiet:continue
            if req['op']=='upload':result={'id':req['id'],'units_per_em':1000}
            elif req['op']=='fonts':
                session=[r['id'] for r,_ in self.requests if r['op']=='upload']
                result={'version':1,'features':['opaque'],'fonts':[{'family':'F'*self.huge}],
                        'session':[{'id':i,'glyphs':1} for i in session]}
            else:result={'width':10,'font':req.get('font')}
            os.write(self.master,b'\x1b_Gi='+hid+b';OK;'+base64.b64encode(json.dumps(result).encode())+ST)
        if 'scan_to' in self.__dict__:self.scan=self.scan_to
    def pump(self,seconds=.1):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            if not select.select([self.master],[],[],.02)[0]:continue
            try:data=os.read(self.master,1<<20)
            except OSError:break
            self.seen.extend(data)
            self.bootstrap();self.pointer()
            if ST in data:self.service()
    def close(self):
        os.close(self.master)
        try:self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:self.proc.kill()

CHILD=r'''
import base64,os,re,select,sys,tty,time
log=open(sys.argv[1],'wb',buffering=0);tty.setraw(0)
sys.stderr=open(sys.argv[1]+'.err','w',buffering=1)
got=bytearray()
def wait(pat,secs=20):
    until=time.monotonic()+secs;fresh=True
    while time.monotonic()<until:
        m=re.search(pat,bytes(got)) if fresh else None
        if m:
            del got[:m.end()]
            return m
        fresh=False
        if select.select([0],[],[],.05)[0]:
            d=os.read(0,1<<20);got.extend(d);log.write(d[:200])
            # A reply is megabytes in small reads: only look when one may be whole.
            fresh=b'\x1b\\' in d or b'E' in d[:1] or len(d)<64
    log.write(b'\nTIMEOUT '+pat+b'\n');sys.exit(1)
ESC=b'\x1b';ST=b'\x1b\\'
def osc(b):os.write(1,b'\x1b]777;'+b+ST)
def b64(s):return base64.b64encode(s.encode())
def gfx(k,p=b''):os.write(1,b'\x1b_G'+k+b';'+p+ST)
osc(b'caps;v=1:op=query:id=1');wait(rb'op=cap:id=1:[^\x1b]*')
osc(b'font;v=1:op=query:id=2');m=wait(rb'op=cap:id=2:[^\x1b]*');log.write(b'\nFONTCAP '+m.group(0)+b'\n')
osc(b'display;v=1:op=query:id=3');m=wait(rb'op=cap:id=3:[^\x1b]*');log.write(b'\nDISPLAY '+m.group(0)+b'\n')
# XTGETTCAP headroom: 2.5
os.write(1,b'\x1bP+q6b697474792d71756572792d6864725f68656164726f6f6d\x1b\\');m=wait(rb'\x1bP1\+r[0-9a-f]+=([0-9a-f]+)');log.write(b'\nHEADROOM '+bytes.fromhex(m.group(1).decode())+b'\n')
osc(b'font;v=1:op=select:role=serif:weight=300:slant=-500')
os.write(1,b'\x1b[HSTYLED\x1b[1mBOLD\r\n')
os.write(1,b'\x1b[0mPLAIN\r\n')
os.write(1,b'\x1b[11mSLOT\x1b[0m\r\n')
# Stereo depth on text and on a packed one-bit image placed at it.
osc(b'depth;v=1:op=set:d=-150:y=0.25')
os.write(1,b'DEEP')
gfx(b'a=T,f=301,s=9,v=1,i=11,c=1,r=1,q=2',base64.b64encode(b'\x55\x80'))
osc(b'depth;v=1:op=reset')
os.write(1,b'FLAT\r\n')
# Silkie's palette image and kitty's indexed colour, for a terminal that
# lists neither, and feature queries for them and for 64-bit offsets.
os.write(1,b'\x1b7\x1b[1;40H')
gfx(b'a=T,i=12,f=402,s=3,v=2,c=1,r=1,q=2',b'AAAAAP8AAP8AAP+A/////2w0')
gfx(b'a=T,i=13,f=8,s=3,v=1,c=1,r=1,q=2',base64.b64encode(b'\x01\xf9\xf8'))
os.write(1,b'\x1b8')
gfx(b'a=q,i=40,e=t,f=402,t=f,O=4294967296');m=wait(rb'\x1b_Gi=40,[^;]*;OK');log.write(b'\nFEATURE '+m.group(0)[3:]+b'\n')
gfx(b'a=e,i=12,x=1,w=1',b'AP8A/w==');wait(rb'\x1b_Gi=12;OK')
# A reply far past the old message size.
gfx(b'a=v,f=200,i=77',b64('{"op":"fonts"}'));m=wait(rb'\x1b_Gi=77;OK;([A-Za-z0-9+/=]+)\x1b\\',30)
r=base64.b64decode(m.group(1));log.write(b'\nFONTSLEN %d\n'%len(r))
big='{"op":"upload","id":"body","data":"'+'Q'*20000+'"}'
big=b64(big)
gfx(b'a=v,f=200,i=78,m=1',big[:4096])
off=4096
while off<len(big):
    gfx(b'm=%d'%(off+4096<len(big)),big[off:off+4096]);off+=4096
m=wait(rb'\x1b_Gi=78;OK;([A-Za-z0-9+/=]+)\x1b\\');log.write(b'\nUPLOAD '+base64.b64decode(m.group(1))+b'\n')
gfx(b'a=v,f=200,i=79',b64('{"op":"fonts"}'));m=wait(rb'\x1b_Gi=79;OK;([A-Za-z0-9+/=]+)\x1b\\',30)
r=base64.b64decode(m.group(1));log.write(b'\nSESSION '+r[r.index(b'"session"'):]+b'\n')
text=b64('{"font":"@body","size":24,"text":"Window title","fill":"p3:1/0.3/0*200"}').decode()
scene='g id=100 box=0,0,400,240\nt id=102 g=100 text='+text+'\n'
gfx(b'a=T,f=200,i=1,s=400,v=240,c=20,r=5,T=1',b64(scene));wait(rb'\x1b_Gi=1;OK')
os.write(1,b'SCENE_READY\r\n')
# An image as the pointer: the terminal knows it by another id.
gfx(b'a=t,f=32,s=1,v=1,i=5,q=2',b'AAAAAA==')
os.write(1,b'\x1b]22;image:5:1:2'+ST)
os.write(1,b'\x1b]22;?__current__'+ST);m=wait(rb'\x1b\]22;([^\x1b]*)\x1b\\');log.write(b'\nCURRENT '+m.group(1)+b'\n')
os.write(1,b'POINTER_READY\r\n')
wait(rb'E',60)
gfx(b'a=e,i=1',b64('p id=103 g=100 d=M0,0L9,9 fill=#336699*200\n'));wait(rb'\x1b_Gi=1;OK')
os.write(1,b'\x1b[20;1HEDITED\r\n')
wait(rb'I\Z',60)
osc(b'tablet-pointer;v=1:op=mode:mode=1')
gfx(b'a=T,U=1,f=200,i=9,s=100,v=100,c=10,r=5,q=2',b64('p id=1 d=M0,0L1,1\n'))
os.write(1,b'\x1b[15;5H\x1b[38;2;0;0;9m\xf4\x8e\xbb\xae\xcc\x85\xcc\x85\x1b[39m')
osc(b'ink;v=1:op=brush:i=9:k=5000:n=10:fill=#112233:w0=1:w1=5')
os.write(1,b'\x1b[17;1HINK_READY')
for _ in range(3):
    m=wait(rb'tablet-pointer;(v=1:op=event[^\x1b]*)');log.write(b'\nREPORT '+m.group(1)+b'\n')
time.sleep(600)
'''

with tempfile.TemporaryDirectory() as temp:
    root=Path(temp);log=root/'input';child=root/'child.py';child.write_text(CHILD)
    t1=Terminal(['new-session','--',sys.executable,str(child),str(log)],huge=1536*1024)
    t2=None
    def child_log():
        err=Path(str(log)+'.err')
        return (log.read_bytes() if log.exists() else b'')+(b'\nTIMEOUT stderr '+err.read_bytes() if err.exists() and err.stat().st_size else b'')
    try:
        for _ in range(400):
            t1.pump()
            if b'POINTER_READY' in t1.seen or b'TIMEOUT' in child_log():break
        L=child_log()
        assert b'TIMEOUT' not in L,L[-600:]
        assert b'SCENE_READY' in t1.seen,(bytes(t1.seen[-1500:]),L[-600:])
        # discovery and what dv says of a display it merely relays
        assert re.search(rb'FONTCAP op=cap:id=2:caps=1:roles=default,sans,serif:weight_min=100:weight_max=900:slant_min=-1500:slant_max=1500:synthetic_slant=1',L),L[:800]
        assert b'kind=eink' in L and b'color=gray' in L,L[:800]
        assert b'HEADROOM 2.5' in L,L[:800]
        # styles in painted frames, re-selected after every SGR 0
        out=bytes(t1.seen)
        sel=b'\x1b]777;font;v=1:op=select:role=serif:weight=300:slant=-500'+ST
        assert sel in out,out[-1500:]
        before=out.split(b'STYLED')[0]
        assert before.rfind(sel)>before.rfind(b'\x1b[0'),'the style is selected after the SGR that would reset it'
        before=out.split(b'PLAIN')[0]
        assert before.rfind(b'\x1b[0')>before.rfind(b'op=select'),'the plain line is back in the default, by SGR 0'
        assert b'role=sans:weight=0:slant=auto' in out,'SGR 11 selects the first configured role'
        # stereo depth in painted frames, set after the SGR that would reset it
        dset=b'\x1b]777;depth;v=1:op=set:d=-150:y=0.25'+ST
        assert dset in out,out[-1500:]
        before=out.split(b'DEEP')[0]
        assert before.rfind(dset)>before.rfind(b'\x1b[0'),'depth follows the SGR that would reset it'
        before=out.split(b'FLAT')[0]
        assert max(before.rfind(b'777;depth;v=1:op=reset'),before.rfind(b'\x1b[0'))>before.rfind(dset),'the flat text is back on the plane'
        # a packed image this 4-bit gray terminal did not list: opaque gray as
        # RGB (no alpha), deflated if smaller, placed at the pen's depth
        m=re.search(rb'\x1b_Ga=t,i=(\d+),f=24,s=9,v=1(,o=z)?,q=2,m=0;([^\x1b]*)',out)
        assert m,'f=301 is expanded for a terminal that did not list it'
        px=base64.b64decode(m.group(3))
        if m.group(2):
            import zlib;px=zlib.decompress(px)
        assert len(px)==27 and all(b%17==0 for b in px),'only the 16 gray levels it shows'
        place=out.index(b'\x1b_Ga=p,U=1,i='+m.group(1)+b',')
        assert out.rfind(b'777;depth;',0,place)==out.rfind(dset,0,place)+2,'the placement is made at its depth'
        # a palette image and indexed colour it did not list: their colours
        assert re.search(rb'\x1b_Ga=t,i=\d+,f=32,s=3,v=2',out),'f=402 is expanded for a terminal that did not list it'
        assert re.search(rb'\x1b_Ga=t,i=\d+,f=24,s=3,v=1',out),'f=8 is expanded for a terminal that did not list it'
        assert b'FEATURE i=40,e=t,f=402,t=f,O=4294967296;OK' in L,L[-800:]
        # the palette update is in what it was last sent: slot 1, red, is now
        # green -- on this 4-bit gray display, gray 187 rather than 51
        t1.pump(.3)
        sent=re.findall(rb'\x1b_Ga=t,i=\d+,f=32,s=3,v=2(,o=z)?,q=2,m=0;([^\x1b]*)',bytes(t1.seen))
        z,px=sent[-1];px=base64.b64decode(px)
        if z:
            import zlib;px=zlib.decompress(px)
        assert px[0]==187 and px[3]==255,('the update is sent',px[:8])
        # a reply far beyond the old 1 MiB message limit arrived whole
        m=re.search(rb'FONTSLEN (\d+)',L);assert m and int(m.group(1))>1536*1024,L[:400]
        # the window's name for its font, the terminal's name for it
        assert b'"id": "body"' in L or b'"id":"body"' in L,L[-800:]
        assert b'SESSION "session"' in L and b'dvf' not in L,L[-400:]
        uploads=[r for r,q in t1.requests if r['op']=='upload' and not q]
        assert len(uploads)==1 and uploads[0]['id'].startswith('dvf') and len(uploads[0]['data'])==20000,uploads[:1]
        handle=uploads[0]['id']
        # the pointer image: named by the host's id, shown there, read back by ours
        m=re.search(rb'\x1b\]22;image:(\d+):1:2\x1b\\',out);assert m and m.group(1)!=b'5',out[-800:]
        t1.pump(.3);assert b'CURRENT image:5:1:2' in child_log(),child_log()[-400:]
        assert re.search(rb'\x1b_Ga=t,i='+m.group(1)+rb',f=32',bytes(t1.seen)),'the pointer image goes without a placement'
        # the scene: under the terminal's font name, with T=1 and paints intact
        def scenes(data):
            found=[]
            for m in re.finditer(rb'\x1b_Ga=t,i=(\d+),([^;]*);([^\x1b]*)\x1b\\',data):
                if b'f=200' not in m.group(2):continue
                found.append((m.group(1),m.group(2),base64.b64decode(m.group(3)).decode()))
            return found
        t1.pump(.5)
        sc=scenes(bytes(t1.seen));assert sc,bytes(t1.seen[-800:])
        assert b'T=1' in sc[0][1],sc[0][1]
        rec=[l for l in sc[0][2].splitlines() if l.startswith('t ')][0]
        layout=json.loads(base64.b64decode(rec.split('text=')[1].split()[0]))
        assert layout['font']=='@'+handle and layout['fill']=='p3:1/0.3/0*200',layout
        assert 'box=0,0,400,240' in sc[0][2]
        # A second terminal attaches: the font first, then the scene. It is not asked anything.
        t2=Terminal(['--3d-focus=4','attach'])
        for _ in range(60):
            t2.pump()
            t1.pump(.02)
            if scenes(bytes(t2.seen)):break
        data=bytes(t2.seen);sc2=scenes(data);assert sc2,data[-800:]
        repl=[(r,q) for r,q in t2.requests if r['op']=='upload']
        assert repl and repl[0][1] and repl[0][0]['id']==handle and len(repl[0][0]['data'])==20000,repl[:1]
        assert data.index(b'a=v,f=200')<data.index(b'a=t,i='+sc2[0][0]),'fonts go before the scene that needs them'
        assert 'font' in json.loads(base64.b64decode([l for l in sc2[0][2].splitlines() if l.startswith('t ')][0].split('text=')[1].split()[0]))
        assert sel in data,'the second terminal is painted the styles too'
        # --3d-focus=4 on this terminal only: the focused window 4 pixels nearer
        assert b'777;depth;v=1:op=set:d=-258:y=0.25'+ST in data,'deep text is nearer by 4 pixels (52000/1920 mdeg each)'
        assert b'777;depth;v=1:op=set:p=-4:y=0'+ST in data,'flat text in the focused window is 4 pixels nearer'
        assert dset not in data,'its own depth is not sent unshifted'
        # An edit: forwarded, not a new transmission, to everyone who holds it.
        n1=len(t1.seen);n2=len(t2.seen);before1=len(scenes(bytes(t1.seen)));before2=len(scenes(bytes(t2.seen)))
        command('send-keys','E')
        for _ in range(60):
            t1.pump(.05);t2.pump(.05)
            if b'EDITED' in t1.seen[n1:]:break
        assert b'EDITED' in t1.seen[n1:],child_log()[-400:]
        for t,n,b in ((t1,n1,before1),(t2,n2,before2)):
            new=bytes(t.seen[n:])
            m=re.search(rb'\x1b_Ga=e,i=(\d+),q=2,m=0;([^\x1b]*)',new);assert m,new[-600:]
            assert base64.b64decode(m.group(2))==b'p id=103 g=100 d=M0,0L9,9 fill=#336699*200\n'
            assert len(scenes(bytes(t.seen)))==b,'an edit does not send the scene again'
        # Ink: the terminal at the pen draws, the table and the others learn of it.
        n1=len(t1.seen);n2=len(t2.seen)
        command('send-keys','I')
        for _ in range(60):
            t1.pump(.05);t2.pump(.05)
            if b'INK_READY' in t1.seen[n1:] and b'op=brush' in t1.seen[n1:]:break
        assert b'op=brush' in t1.seen[n1:],(bytes(t1.seen[n1:])[-600:],child_log()[-400:])
        t1.pump(.2);t2.pump(.2)
        def patches(data):
            return [base64.b64decode(m.group(1)).decode() for m in re.finditer(rb'\x1b_Ga=e,i=\d+,q=2,m=0;([^\x1b]*)',data)]
        n1=len(t1.seen);n2=len(t2.seen)
        def report(phase,x,y,p,ink=b''):
            os.write(t1.master,b'\x1b]777;tablet-pointer;v=1:op=event:kind=1:phase=%d:device=3:t=1:contact=0:x=%d:y=%d:buttons=1:mods=0:tool=1:valid=1:p=%d%s'%(phase,x,y,p,ink)+ST)
        token=int(re.findall(rb'op=brush:i=\d+:k=(\d+)',bytes(t1.seen))[-1]);assert token!=5000,'the terminal has ids of ours'
        report(1,30*2560,12*5120,13107,b':ink=%d'%token)
        report(2,31*2560,12*5120,39321)
        report(3,32*2560,13*5120,0)
        for _ in range(60):
            t1.pump(.05);t2.pump(.05)
            if child_log().count(b'REPORT ')==3:break
        L=child_log();reports=re.findall(rb'REPORT ([^\n]*)',L);assert len(reports)==3,L[-600:]
        # The numbers from what the program was told, by the page's own formulas:
        # the placement's corner is four cells in and fourteen down, a pixel a user unit.
        def point(r):
            f=dict(k.split(b'=') for k in r.split(b':'))
            return int(f[b'x'])/256-40,int(f[b'y'])/256-280,1+4*int(f[b'p'])/65535
        want=[point(r) for r in reports]
        assert b':ink=5000' in reports[0],reports[0]
        recs=patches(bytes(t2.seen[n2:]));assert len(recs)==3,(recs,bytes(t2.seen[n2:])[-400:])
        assert recs[0].startswith('k id=5000 pts=') and recs[0].rstrip().endswith('fill=#112233'),recs
        assert all(r.startswith('a id=5000 pts=') for r in recs[1:]),recs
        got=[tuple(map(float,r.split('pts=')[1].split()[0].split(','))) for r in recs]
        for g,w in zip(got,want):
            assert all(abs(a-b)<1e-6 for a,b in zip(g,w)),(got,want)
        # The terminal that drew it is not sent its stroke until it is over, when its element becomes the program's.
        mine=patches(bytes(t1.seen[n1:]));assert len(mine)==1 and mine[0].splitlines()[0]=='x id=%d'%token and mine[0].splitlines()[1].startswith('k id=5000 pts='),mine
        # Attaching later: the stroke is in the scene it gets, every point of it.
        t2.close();t2=None
        late=Terminal(['attach'])
        for _ in range(60):
            late.pump();t1.pump(.02)
            if any('k id=5000' in sc[2] for sc in scenes(bytes(late.seen))):break
        sc=[sc for sc in scenes(bytes(late.seen)) if 'k id=5000' in sc[2]];assert sc,bytes(late.seen[-800:])
        line=[l for l in sc[0][2].splitlines() if l.startswith('k id=5000')][0]
        pts=list(map(float,line.split('pts=')[1].split()[0].split(',')))
        assert len(pts)==9 and all(abs(a-b)<1e-6 for a,b in zip(pts,[v for w in want for v in w])),(pts,want)
        late.close()
        # A live upgrade keeps the styles in the cells, and the table of them.
        n1=len(t1.seen)
        subprocess.run([GV,'-L',LABEL,'upgrade'],capture_output=True,timeout=15,check=True)
        t3=Terminal(['attach'])
        for _ in range(60):
            t3.pump();t1.pump(.02)
            if b'SLOT' in t3.seen:break
        # The first lines have scrolled out of this window by now.
        before=bytes(t3.seen).split(b'SLOT')[0]
        assert b'role=sans:weight=0:slant=auto' in before,'styles survive a server upgrade: %r'%before[-600:]
        t3.close()
    finally:
        subprocess.run([GV,'-L',LABEL,'kill-server'],capture_output=True,timeout=5)
        t1.close()
        if t2:t2.close()
print('regress/silkie.py: ok')
