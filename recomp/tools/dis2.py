import os
EXTRACT = os.environ.get('DB_EXTRACT', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'extract'))   # your own disc's files
import sys, sh4dec as S, sh4fmt as F
img=S.Image(EXTRACT + '/1ST_READ.BIN')
a=int(sys.argv[1],16); n=int(sys.argv[2])
for k in range(n):
    x=a+2*k; i=S.decode(img.u16(x)); t=F.fmt(i,x) if i else ".word 0x%04x"%img.u16(x)
    if i and i.op in ('movl_pc','movw_pc'):
        la=S.pcrel_addr(i,x); t+="    ; =0x%x"%(img.u32(la) if i.op=='movl_pc' else img.u16(la))
    print("%08x: %s"%(x,t))
