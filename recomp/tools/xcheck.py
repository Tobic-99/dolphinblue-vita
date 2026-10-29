"""Compare own decoder against Ghidra's disassembly on sampled functions."""
import os
EXTRACT = os.environ.get('DB_EXTRACT', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'extract'))   # your own disc's files
import re, collections, sh4dec as S, sh4fmt as F
img = S.Image(EXTRACT + '/1ST_READ.BIN')
def norm(t):
    t = t.lower().lstrip('_').replace(' ', '')
    t = re.sub(r'-?0x[0-9a-f]+', lambda m: str(int(m.group(0), 16)), t)
    t = t.replace('fmov.s', 'fmov')
    return t
tot = ok = 0; bad = collections.Counter(); ex = {}
for line in open('../build/ghidra_insns.txt'):
    if line.startswith('NOFUNC'): continue
    a, txt, w = line.rstrip('\n').split('\t')
    a = int(a, 16); ins = S.decode(img.u16(a)); tot += 1
    mine = F.fmt(ins, a) if ins else "INVALID"
    if norm(mine) == norm(txt): ok += 1
    else:
        k = (ins.op if ins else None); bad[k] += 1; ex.setdefault(k, (hex(a), txt, mine))
print("compared", tot, "match", ok)
for k, v in bad.most_common(): print(k, v, ex[k])
