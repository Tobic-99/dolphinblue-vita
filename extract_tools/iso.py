import sys,struct,os
D=os.environ.get("DB_DISC", "disc") + "/"   # folder with your own disc's .gdi tracks
tracks=[(45000,D+"track03.iso"),(450000,D+"track04.iso")]
fh={p:open(p,'rb') for _,p in tracks}
def rd(lba,n=1):
    for start,p in tracks:
        sz=os.path.getsize(p)//2048
        if start<=lba<start+sz:
            f=fh[p];f.seek((lba-start)*2048);return f.read(n*2048)
    raise Exception("LBA %d not in any track"%lba)
pvd=rd(45016)
assert pvd[1:6]==b'CD001',pvd[:8]
print("vol id",pvd[40:72],"sys",pvd[8:40]," vol space",struct.unpack('<I',pvd[80:84]))
print("app",pvd[574:702].strip(), "date", pvd[813:830])
root=pvd[156:190]
out=[]
def walk(lba,size,path):
    data=b''.join(rd(lba+i) for i in range((size+2047)//2048))
    i=0
    while i<len(data):
        l=data[i]
        if l==0: i=(i//2048+1)*2048; continue
        r=data[i:i+l]; el=struct.unpack('<I',r[2:6])[0]; sz=struct.unpack('<I',r[10:14])[0]; fl=r[25]; nl=r[32]; nm=r[33:33+nl]
        i+=l
        if nm in (b'\0',b'\1'): continue
        nm=nm.decode('ascii','replace').split(';')[0]
        if fl&2: walk(el,sz,path+nm+'/')
        else: out.append((path+nm,el,sz))
walk(struct.unpack('<I',root[2:6])[0],struct.unpack('<I',root[10:14])[0],'/')
dest=sys.argv[1] if len(sys.argv)>1 else None
tot=0
for p,el,sz in out:
    trk=3 if el<450000 else 4
    print("%-40s %10d  LBA %6d  track%02d"%(p,sz,el,trk)); tot+=sz
    if dest:
        fp=os.path.join(dest,p.lstrip('/'));os.makedirs(os.path.dirname(fp),exist_ok=True)
        with open(fp,'wb') as o:
            n=(sz+2047)//2048; o.write(rd(el,n)[:sz]) if n else None
print(len(out),"files",tot,"bytes")
