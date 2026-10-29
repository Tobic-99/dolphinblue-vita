import sys; sys.path.insert(0,'.')
import collections, capstone as C, discover as D, sh4dec as S, sh4fmt as F
d,_=D.main(); img=d.img
md=C.Cs(C.CS_ARCH_SH,C.CS_MODE_SH4|C.CS_MODE_SHFPU|C.CS_MODE_LITTLE_ENDIAN)
cov=sorted(d.code_bytes())
bad=collections.Counter(); ex={}; ops=collections.Counter()
norm=lambda t:t.replace('fmov.s','fmov').replace('fmov.d','fmov')
for a in cov:
    ins=S.decode(img.u16(a)); ops[ins.op]+=1
    cs=list(md.disasm(img.data[a-img.base:a-img.base+2],a))
    cm=cs[0].mnemonic.lower() if cs else "INVALID"
    mm=F.fmt(ins,a).split()[0]
    if norm(cm)!=norm(mm): bad[(mm,cm)]+=1; ex.setdefault((mm,cm),(hex(a),cs[0].op_str if cs else ''))
print("insns",len(cov),"mnemonic mismatches",sum(bad.values()))
for k,v in bad.most_common(20): print(k,v,ex[k])
print("distinct ops used",len(ops))
open('../build/op_histogram.txt','w').write("\n".join("%-12s %d"%kv for kv in ops.most_common())+"\n")
