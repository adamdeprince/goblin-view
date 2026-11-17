#!/usr/bin/env python3
"""Real PTY client/server pointer routing, capture and image-tile regression."""
import fcntl,os,pty,re,select,signal,struct,subprocess,sys,tempfile,termios,time
from pathlib import Path
GV=os.environ.get('GV',str(Path(__file__).resolve().parents[1]/'odin-seat'))
LABEL='pointer-'+str(os.getpid())
ESC=b'\x1b';ST=ESC+b'\\';PREFIX=ESC+b']777;goblin-pointer;v=1:op='
def frame(op,**kw):return PREFIX+op.encode()+b''.join(b':'+k.encode()+b'='+str(v).encode() for k,v in kw.items())+ST
def command(*args):
    result=subprocess.run([GV,'-L',LABEL,*args],capture_output=True,timeout=5)
    assert result.returncode==0,(args,result.stderr,result.stdout)
    return result.stdout

def records(path):
    data=path.read_bytes() if path.exists() else b''
    return [dict(item.split('=',1) for item in match.decode().split(':')) for match in re.findall(rb'\x1b\]777;goblin-pointer;(.*?)\x1b\\',data)]

with tempfile.TemporaryDirectory() as temp:
    root=Path(temp); log=root/'input'; log2=root/'input2'
    child_script=root/'child.py'
    child_script.write_text('''import os,sys,tty
f=open(sys.argv[1],'wb',buffering=0);tty.setraw(0)
os.write(1,b'\\x1b]777;goblin-pointer;v=1:op=query:id=99\\x1b\\\\\\x1b]777;goblin-pointer;v=1:op=mode:mode=1\\x1b\\\\POINTER_READY')
while True:
 d=os.read(0,8192)
 if not d:break
 f.write(d)
 if d==b'L':os.write(1,b'\\x1b]777;goblin-pointer;v=1:op=mode:mode=0\\x1b\\\\\\x1b[?1002h\\x1b[?1006h\\x1b[?1016hLEGACY_READY')
 if d==b'E':os.write(1,b'\\x1b]777;goblin-pointer;v=1:op=mode:mode=1\\x1b\\\\ENHANCED_READY')
''')
    master,slave=pty.openpty();fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',30,100,1000,600))
    env=dict(os.environ,TERM='xterm-kitty',ODIN_SEAT_KITTY_MEDIUM='direct',PYENV_REHASH_TIMEOUT='1')
    child=subprocess.Popen([GV,'-L',LABEL,'new-session','--',sys.executable,str(child_script),str(log)],stdin=slave,stdout=slave,stderr=slave,env=env)
    pending=bytearray();seen=set()
    def pump(seconds=.1):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            if not select.select([master],[],[],.02)[0]:continue
            try:data=os.read(master,65536)
            except OSError:break
            pending.extend(data)
            replies=((b'\x1b[16t',b'\x1b[6;20;10t'),(b'\x1b[18t',b'\x1b[8;30;100t'),
                (b'a=q',b'\x1b_Gi=31;OK'+ST),(b'\x1b[>q',b'\x1bP>|Inkline(0.4.19)'+ST),
                (b'op=query:id=17349522',frame('cap',id=17349522,caps=7,w=1000,h=600,cw=10,ch=20)),
                (b'\x1b[c',b'\x1b[?62;22c'))
            for query,reply in replies:
                if query in pending and query not in seen:os.write(master,reply);seen.add(query)
    try:
        for _ in range(50):
            pump()
            if b'POINTER_READY' in pending:break
        assert b'POINTER_READY' in pending,bytes(pending[-1200:])
        caps=[r for r in records(log) if r['op']=='cap'];assert caps and caps[0]['caps']=='7',records(log)
        def geometry(window):
            listing=command('list-windows').decode()
            match=re.search(r':'+str(window)+r': .*\((\d+)x(\d+)\) (-?\d+),(-?\d+)',listing)
            assert match,listing
            return tuple(map(int,match.groups()))
        cols,rows,x,y=geometry(1)
        def pen(phase,px,py,contact=0):
            packet=frame('event',kind=1,phase=phase,device=4,contact=contact,x=px,y=py,buttons=1 if phase in (1,2) else 0,tool=2,valid=7,p=43000 if phase in (1,2) else 0,tx=-1200,ty=2100,twist=27001)
            for part in (packet[:1],packet[1:8],packet[8:]):os.write(master,part)
            pump(.08)
        # Every normal window has a one-cell frame. Preserve fractional pixels.
        pen(1,(x+1)*10*256+12345,(y+1)*20*256+6789)
        # A second window takes focus while the first stroke is down. Capture
        # must keep the move/release in the first PTY, even outside its frame.
        command('new-window','--',sys.executable,str(child_script),str(log2));pump(.4)
        pen(2,-1000,-2000);pen(3,-1000,-2000)
        first=[r for r in records(log) if r['op']=='event'];second=[r for r in records(log2) if r['op']=='event']
        assert [r['phase'] for r in first]==['1','2','3'],first
        assert first[0]['x']=='12345' and first[0]['y']=='6789',first
        assert first[1]['x']==str(-1000-(x+1)*10*256),first
        assert first[0]['tool']=='2' and first[0]['twist']=='27001' and first[0]['tx']=='-1200',first
        assert not second,second
        # Legacy pixel tracking keeps sub-cell accuracy and eraser/right releases.
        _,_,lx,ly=geometry(2)
        command('send-keys','L')
        for _ in range(30):
            pump(.05)
            if b'LEGACY_READY' in pending:break
        assert b'LEGACY_READY' in pending,bytes(pending[-300:])
        pen(1,(lx+1)*10*256+40*256+123,(ly+1)*20*256+50*256+123)
        pen(3,(lx+1)*10*256+40*256+123,(ly+1)*20*256+50*256+123)
        raw=log2.read_bytes();assert b'\x1b[<2;41;51M\x1b[<2;41;51m' in raw,raw
        for phase,cid in ((1,1),(1,2),(3,2),(3,1)):
            os.write(master,frame('event',kind=2,phase=phase,device=5,contact=cid,x=(lx+2)*10*256,y=(ly+2)*20*256,tool=0))
        pump(.15)
        assert log2.read_bytes().count(b'\x1b[<0;11;21M')==1,log2.read_bytes()
        assert log2.read_bytes().count(b'\x1b[<0;11;21m')==1,log2.read_bytes()
        command('send-keys','E')
        for _ in range(30):
            pump(.05)
            if b'ENHANCED_READY' in pending:break
        assert b'ENHANCED_READY' in pending,bytes(pending[-300:])
        # Simultaneous finger contacts and their gesture target the raised window.
        _,_,x2,y2=geometry(2)
        for cid in (1,2):
            os.write(master,frame('event',kind=2,phase=1,device=5,contact=cid,x=(x2+2+cid)*10*256,y=(y2+2)*20*256,tool=0))
        os.write(master,frame('event',kind=3,phase=1,device=5,contact=1,peer=2,x=(x2+3)*10*256,y=(y2+2)*20*256,dx=0,dy=0,scale=10000,angle=0,tool=0));pump(.15)
        second=[r for r in records(log2) if r['op']=='event'];assert [r['kind'] for r in second]==['2','2','3'],second
        # A simultaneous legacy eraser stroke must get a matching right-button
        # release at the exact pixel position when the host disconnects.
        log3=root/'input3'
        command('new-window','--',sys.executable,str(child_script),str(log3));pump(.4)
        command('send-keys','L');pump(.2)
        _,_,x3,y3=geometry(3)
        pen(1,(x3+1)*10*256+40*256+123,(y3+1)*20*256+50*256+123)
        assert b'\x1b[<2;41;51M' in log3.read_bytes(),log3.read_bytes()
        # Disconnect cancels every unfinished contact in the original window.
        child.terminate();child.wait(timeout=3);pump(.2)
        assert log3.read_bytes().endswith(b'\x1b[<2;41;51m'),log3.read_bytes()
        second=[r for r in records(log2) if r['op']=='event'];assert sum(r['phase']=='4' for r in second)==3,second
        print('PTY pointer negotiation, subpixel window offsets, cross-window capture, gestures and disconnect cancellation passed.')
    finally:
        if child.poll() is None:child.terminate();child.wait(timeout=3)
        subprocess.run([GV,'-L',LABEL,'kill-server'],capture_output=True,timeout=5)
        os.close(master);os.close(slave)
