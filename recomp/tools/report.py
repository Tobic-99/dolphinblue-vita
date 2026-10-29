import json, collections, sys
import discover as D, sh4dec as S
d, rounds = D.main()
B = "../build/"
img = d.img
json.dump(rounds, open(B + "discover_rounds.json", "w"))
fm = open(B + "funcmap.txt", "w")
fm.write("# entry     end(excl)  span  ninsns  provenance\n")
tot_unres = collections.Counter(); unres_lines = []
nsw = collections.Counter()
for e in sorted(d.funcs):
    f = d.funcs[e]
    hi = max(f.insns) + 2
    fm.write("%08x  %08x  %6d  %6d  %s\n" % (e, hi, hi - e, len(f.insns), ",".join(sorted(d.prov[e]))))
    for a, k in f.indirect_unresolved:
        tot_unres[k] += 1
        ins = f.insns[a]
        unres_lines.append("%08x  in %08x  %-6s @r%d" % (a, e, ins.op, ins.m))
    for a, how in f.switch_how.items(): nsw[how] += 1
fm.close()
# unresolved tail jumps (jmp/braf with unknown target)
with open(B + "unresolved.txt", "w") as u:
    u.write("# Indirect control transfers whose target is not statically known.\n")
    u.write("# They go through the runtime dispatch table (unknown target = fatal).\n")
    u.write("# counts: %s\n" % dict(tot_unres))
    for l in unres_lines: u.write(l + "\n")
cov = d.code_bytes()
print("functions", len(d.funcs))
print("provenance", collections.Counter(p for e in d.funcs for p in d.prov[e]))
print("instructions (unique addrs)", len(cov), "code bytes", 2 * len(cov))
print("switch tables", dict(nsw), "cases", sum(len(set(v)) for f in d.funcs.values() for v in f.switch.values()))
print("unresolved indirect", dict(tot_unres))
print("calls direct", sum(1 for f in d.funcs.values() for t in f.calls.values() if t is not None))
print("rounds", len(rounds), [r[3] for r in rounds])
ext = collections.Counter()
for f in d.funcs.values():
    for t in list(f.calls.values()) + list(f.tails.values()):
        if t is not None and not d.valid_code_addr(t): ext["%08x" % t] += 1
print("known targets outside image", ext.most_common(10))
