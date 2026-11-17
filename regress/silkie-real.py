#!/usr/bin/env python3
"""What odin-seat sends, judged by a real Silkie.

Every other regression here plays the terminal itself: a script that answers
the bootstrap and keeps what it was sent. That checks odin-seat against what
its author thinks a terminal does. This one checks it against Silkie: the
bytes odin-seat writes go into a Silkie screen -- Silkie's own parser and
graphics manager, run without a window by the installed Silkie's Python --
with the quiet flag taken off every graphics command, so that Silkie answers
each one, and its image and placement tables read afterwards.

It exists because of what the others missed: odin-seat hands a terminal
image data in shared memory by default, every other test asked for it
inline, and a vector scene in shared memory was refused by Silkie for the
padding that follows it in the last page. Nothing here names a medium, so
each case runs with the default one, and one of them again with a file.

Silkie is used as it is installed, as a program: nothing of it is linked or
copied (docs/CLEANROOM.md). Without a Silkie on this machine, or with one
whose screen cannot be driven this way, the test says so and passes."""
import json,os,shutil,subprocess,sys,tempfile
from pathlib import Path
GV=os.environ.get('GV',str(Path(__file__).resolve().parents[1]/'odin-seat'))

INNER=r'''
import base64,collections,fcntl,json,os,pty,re,select,struct,subprocess,sys,termios,time
GV,CHILD,CASE,MEDIUM,LABEL=sys.argv[1:6]
ROWS,COLS,CW,CH=30,100,10,20
ST=b'\x1b\\'
def out(**k):
    print('RESULT '+json.dumps(k));sys.stdout.flush()
try:
    from kitty.fast_data_types import Screen, set_options
    from kitty.options.types import Options, defaults
    from kitty.config import finalize_keys, finalize_mouse_mappings
    class Callbacks:
        """What the screen calls back into: its replies are kept, the rest ignored."""
        def __init__(self):self.wtcbuf=b''
        def write(self,data):self.wtcbuf+=bytes(data)
        def clear(self):self.wtcbuf=b''
        def __getattr__(self,name):
            if name.startswith('__'):raise AttributeError(name)
            return lambda *a,**k:None
    o=Options(dict(defaults._asdict()));finalize_keys(o,{});finalize_mouse_mappings(o,{});set_options(o)
    cb=Callbacks();scr=Screen(cb,ROWS,COLS,5,CW,CH,0,cb);cb.color_profile=scr.color_profile
    def parse(data):
        data=memoryview(data)
        while data:
            dest=scr.test_create_write_buffer()
            n=scr.test_commit_write_buffer(data,dest)
            data=data[n:]
            scr.test_parse_written_data()
    parse(b'')
except Exception as e:
    out(skip='this Silkie\'s screen cannot be driven here: %r'%(e,));raise SystemExit
master,slave=pty.openpty()
fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',ROWS,COLS,COLS*CW,ROWS*CH))
env=dict(os.environ,TERM='xterm-kitty',PYENV_REHASH_TIMEOUT='1')
env.pop('ODIN_SEAT_KITTY_MEDIUM',None)
if MEDIUM:env['ODIN_SEAT_KITTY_MEDIUM']=MEDIUM
proc=subprocess.Popen([GV,'-L',LABEL,'new-session','--',sys.executable if False else 'python3',CHILD,CASE],stdin=slave,stdout=slave,stderr=slave,env=env)
os.close(slave)
seen=bytearray();done=set();fed=0;codes=collections.Counter();errors=[];last={}
cap=lambda sel,qid,f:b'\x1b]777;'+sel+b';v=1:op=cap:id='+qid+b':'+f+ST
TABLE=((b'\x1b[16t',b'\x1b[6;%d;%dt'%(CH,CW)),(b'\x1b[18t',b'\x1b[8;%d;%dt'%(ROWS,COLS)),
    (b'a=q',b'\x1b_Gi=31;OK'+ST),(b'\x1b[>q',b'\x1bP>|kitty(0.49.2)'+ST),
    (b'caps;v=1:op=query:id=17349523',cap(b'caps',b'17349523',b'caps=3:ext=caps.1,display.1:media=d,f,t,s:format=24,32,64,100,200')),
    (b'display;v=1:op=query:id=17349524',cap(b'display',b'17349524',b'mode=0:kind=lcd:color=rgb:bits=8:gamut=srgb:headroom=1:scale=2:period=8333:ppi=254')),
    (b'\x1b[c',b'\x1b[?62;22c'))
APC=re.compile(rb'\x1b_G([^;\x1b]*)((?:;[^\x1b]*)?)\x1b\\')
def loud(m):
    keys=m.group(1)
    k=dict(x.split(b'=',1) for x in keys.split(b',') if b'=' in x)
    if b'i' in k:last[k[b'i']]=keys[:80].decode()
    return b'\x1b_G'+b','.join(x for x in keys.split(b',') if not x.startswith(b'q='))+m.group(2)+ST
def feed(rest=False):
    """Up to the last whole graphics command, or at the end all there is."""
    global fed
    buf=bytes(seen[fed:]);cut=buf.rfind(ST)
    if cut<0 and not rest:return
    part=buf if rest else buf[:cut+2];fed+=len(part)
    cb.clear();parse(APC.sub(loud,part))
    for keys,reply in re.findall(rb'\x1b_G([^;]*);([^\x1b]*)\x1b\\',bytes(cb.wtcbuf)):
        code=reply.split(b':')[0].decode();codes[code]+=1
        if code!='OK':
            k=dict(x.split(b'=',1) for x in keys.split(b',') if b'=' in x)
            errors.append([reply[:120].decode('utf8','replace'),last.get(k.get(b'i'),'?')])
def pump(seconds):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        if not select.select([master],[],[],.02)[0]:continue
        try:data=os.read(master,1<<20)
        except OSError:break
        seen.extend(data)
        for q,r in TABLE:
            if q in seen and q not in done:
                os.write(master,r);done.add(q)
        feed()
try:
    until=time.monotonic()+25
    while time.monotonic()<until and b'FINISHED' not in seen:pump(.1)
    pump(.6);feed(True)
    sent=[m.group(1).decode() for m in APC.finditer(bytes(seen)) if m.group(1).startswith(b'a=t')]
    placed=None
    try:
        scr.update_only_line_graphics_data()
        placed=len(scr.grman.update_layers(0,-1,1,2/COLS,2/ROWS,COLS,ROWS,CW,CH))
    except Exception:pass
    out(case=CASE,medium=MEDIUM or 'default',finished=b'FINISHED' in seen,codes=dict(codes),errors=errors[:6],
        images=scr.grman.image_count,placed=placed,sent=sent[:6])
finally:
    subprocess.run([GV,'-L',LABEL,'kill-server'],capture_output=True,timeout=5)
    os.close(master)
    try:proc.wait(timeout=3)
    except subprocess.TimeoutExpired:proc.kill()
'''

CHILD=r'''
import base64,os,sys,time,tty,zlib
tty.setraw(0)
def gfx(k,p=b''):os.write(1,b'\x1b_G'+k+b';'+base64.b64encode(p)+b'\x1b\\')
case=sys.argv[1]
SCENE=b'p id=1 d=M0,0L200,0L200,100L0,100Z fill=#ffffff\np id=2 d=M20,20L80,20L80,80L20,80Z fill=#ff0000\n'
os.write(1,b'\x1b[2J\x1b[1;1H')
if case=='plain':       # the vector spec's own example: shown where it is sent, fitted to cells
    gfx(b'a=T,f=200,i=1,s=200,v=100,c=40,r=10,q=2',SCENE)
elif case=='under':     # under the text: a layered window, a real placement
    gfx(b'a=t,f=200,i=1,s=200,v=100,q=2',SCENE)
    gfx(b'a=p,i=1,p=1,z=-1,q=2')
    os.write(1,b'\x1b[2;2Htext over it')
elif case=='edit':      # a scene, then an edit of it
    gfx(b'a=t,f=200,i=1,s=200,v=100,q=2',SCENE)
    gfx(b'a=p,i=1,p=1,z=-1,q=2')
    os.write(1,b'\x1b[14;1HDRAWN');time.sleep(.5)
    gfx(b'a=e,i=1,q=2',b'p id=2 d=M20,20L80,20L80,80L20,80Z fill=#0000ff\n')
elif case=='pixels':    # deflated pixels, and a sprite of them over the text
    gfx(b'a=t,f=32,s=64,v=64,o=z,i=1,q=2',zlib.compress(bytes([40,90,160,255])*(64*64)))
    gfx(b'a=p,i=1,p=1,z=-1,q=2')
    os.write(1,b'\x1b[2;3H');gfx(b'a=p,i=1,p=2,x=8,y=8,w=20,h=20,X=3,Y=5,z=4,q=2')
elif case=='text':      # a text record: Silkie shapes it itself
    gfx(b'a=t,f=200,i=1,s=200,v=100,q=2',SCENE+b't id=3 text='+base64.b64encode(b'{"text":"Hi","size":20,"x":90,"y":60}')+b'\n')
    gfx(b'a=p,i=1,p=1,z=-1,q=2')
os.write(1,b'\x1b[15;1HFINISHED')
time.sleep(60)
'''

def silkie():
    for c in (os.environ.get('SILKIE'),shutil.which('silkie'),'/Applications/silkie.app/Contents/MacOS/silkie',
              str(Path.home()/'Applications/silkie.app/Contents/MacOS/silkie')):
        if c and os.access(c,os.X_OK):return c
    return None

exe=silkie()
if exe is None:
    print('regress/silkie-real.py: skipped (no Silkie on this machine)');raise SystemExit
with tempfile.TemporaryDirectory() as temp:
    inner=Path(temp)/'inner.py';inner.write_text(INNER)
    child=Path(temp)/'child.py';child.write_text(CHILD)
    runs=[(c,'') for c in ('plain','under','edit','pixels','text')]+[('under','tempfile'),('pixels','tempfile'),('under','direct')]
    for n,(case,medium) in enumerate(runs):
        code="import sys,runpy; sys.argv=%r; runpy.run_path(%r,run_name='__main__')"%(
            ['inner.py',GV,str(child),case,medium,'silkie-real-%d-%d'%(os.getpid(),n)],str(inner))
        r=subprocess.run([exe,'+runpy',code],capture_output=True,timeout=90)
        line=[l for l in r.stdout.decode('utf8','replace').splitlines() if l.startswith('RESULT ')]
        if not line:
            print('regress/silkie-real.py: skipped (Silkie did not run the harness: %s)'%r.stderr.decode('utf8','replace').strip().splitlines()[-1:]);raise SystemExit
        res=json.loads(line[-1][7:])
        if 'skip' in res:
            print('regress/silkie-real.py: skipped (%s)'%res['skip']);raise SystemExit
        what=(case,medium or 'default medium')
        assert res['finished'],(what,'the program drew',res)
        assert not res['errors'] and set(res['codes'])=={'OK'},(what,'Silkie took every graphics command',res['errors'],res['codes'],res['sent'])
        assert res['images']>=1,(what,'and holds the image',res)
        if res['placed'] is not None:
            want=2 if case=='pixels' else 1
            assert res['placed']>=want,(what,'and would draw it',res['placed'],res['sent'])
        if not medium:
            assert any('t=s,S=' in s for s in res['sent']),(what,'shared memory is the default, sent with its size',res['sent'])
        if medium=='tempfile':
            assert any('t=t,S=' in s for s in res['sent']),(what,'a file is sent with its size',res['sent'])
print('regress/silkie-real.py: ok')
