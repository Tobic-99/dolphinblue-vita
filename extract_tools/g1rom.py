# Parse the "G1ROM FILESYSTEM" image found at track03 sector 300 (LBA 45300)
import sys,struct,os
T=os.environ.get("DB_DISC", "disc") + "/track03.iso"   # your own disc's track03
f=open(T,'rb'); base=300*2048
f.seek(base); hdr=f.read(64)
n=struct.unpack('<I',hdr[16:20])[0]
f.seek(base+64); ents=[]
def readdir(off,size,path,out):
    f.seek(base+off); d=f.read(size)
    for i in range(0,len(d),64):
        e=d[i:i+64]; w=struct.unpack('<4I',e[:16]); nm=e[16:].split(b'\0')[0].decode('ascii','replace')
        if nm in ('.','..') or not nm: continue
        if w[1]&0x40000000 and not (w[0]&0xffff)==0x7d3: out.append(('D',path+nm,w)); 
        else: out.append(('F',path+nm,w))
    return out
out=readdir(64,n*64,'/',[])
dest=sys.argv[1] if len(sys.argv)>1 else None
tot=0;cnt=0;dirs=0
for k,p,w in out:
    if k=='D': dirs+=1; print("DIR ",p,["%08x"%x for x in w]); continue
    off,sz=w[2],w[3]; tot+=sz;cnt+=1
    y=w[0]&0xffff; mo=(w[0]>>16)&0xff; dd=w[0]>>24
    print("%-36s %9d  off 0x%08x  %04d-%02d-%02d"%(p,sz,off,y,mo,dd))
    if dest:
        fp=os.path.join(dest,p.lstrip('/')); os.makedirs(os.path.dirname(fp),exist_ok=True)
        f.seek(base+off); open(fp,'wb').write(f.read(sz))
print(cnt,"files",dirs,"dirs",tot,"bytes; image end 0x%x"%max(w[2]+w[3] for k,p,w in out if k=='F'))
