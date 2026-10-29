"""Text formatter (Ghidra-like SH-4 syntax) for cross-checking the decoder."""
import sh4dec as S
def h(v):
    return ("-0x%x" % -v) if v < 0 else ("0x%x" % v)
R = lambda r: "r%d" % r
FR = lambda r: "fr%d" % r
SIMPLE2 = {  # op: (mnemonic, src, dst) with m,n registers
 "mov":"mov","add":"add","addc":"addc","addv":"addv","sub":"sub","subc":"subc","subv":"subv",
 "and":"and","or":"or","xor":"xor","tst":"tst","cmpeq":"cmp/eq","cmphs":"cmp/hs","cmpge":"cmp/ge",
 "cmphi":"cmp/hi","cmpgt":"cmp/gt","cmpstr":"cmp/str","div0s":"div0s","div1":"div1","dmuls":"dmuls.l",
 "dmulu":"dmulu.l","mull":"mul.l","mulsw":"muls.w","muluw":"mulu.w","neg":"neg","negc":"negc","not":"not",
 "swapb":"swap.b","swapw":"swap.w","extsb":"exts.b","extsw":"exts.w","extub":"extu.b","extuw":"extu.w",
 "xtrct":"xtrct","shad":"shad","shld":"shld"}
ONE = {"shll":"shll","shlr":"shlr","shal":"shal","shar":"shar","shll2":"shll2","shlr2":"shlr2","shll8":"shll8",
 "shlr8":"shlr8","shll16":"shll16","shlr16":"shlr16","rotl":"rotl","rotr":"rotr","rotcl":"rotcl","rotcr":"rotcr",
 "dt":"dt","cmppz":"cmp/pz","cmppl":"cmp/pl","movt":"movt"}
NOARG = {"clrt","clrmac","clrs","sett","sets","nop","sleep","rte","rts","ldtlb","div0u","fschg","frchg"}
def fmt(i, pc):
    o, n, m = i.op, i.n, i.m
    if o in NOARG: return o
    if o in SIMPLE2: return "%s %s,%s" % (SIMPLE2[o], R(m), R(n))
    if o in ONE: return "%s %s" % (ONE[o], R(n))
    sz = {"b":"b","w":"w","l":"l"}
    if o.startswith("mov") and o[3:4] in ("b","w","l") and o[4:5]=="_":
        s = o[3]; k = o[5:]
        if k == "s": return "mov.%s %s,@%s" % (s, R(m), R(n))
        if k == "l": return "mov.%s @%s,%s" % (s, R(m), R(n))
        if k == "m": return "mov.%s %s,@-%s" % (s, R(m), R(n))
        if k == "p": return "mov.%s @%s+,%s" % (s, R(m), R(n))
        if k == "s0": return "mov.%s %s,@(r0,%s)" % (s, R(m), R(n))
        if k == "l0": return "mov.%s @(r0,%s),%s" % (s, R(m), R(n))
        sc = {"b":1,"w":2,"l":4}[s]
        if k == "s4":
            if s == "l": return "mov.l %s,@(%s,%s)" % (R(m), h(i.disp*4), R(n))
            return "mov.%s r0,@(%s,%s)" % (s, h(i.disp*sc), R(n))
        if k == "l4":
            if s == "l": return "mov.l @(%s,%s),%s" % (h(i.disp*4), R(m), R(n))
            return "mov.%s @(%s,%s),r0" % (s, h(i.disp*sc), R(m))
        if k == "sg": return "mov.%s r0,@(%s,gbr)" % (s, h(i.disp*sc))
        if k == "lg": return "mov.%s @(%s,gbr),r0" % (s, h(i.disp*sc))
        if k == "pc": return "mov.%s %s,%s" % (s, h(S.pcrel_addr(i, pc)), R(n))
    if o == "movi": return "mov #%s,%s" % (h(S.s8(i.imm)), R(n))
    if o == "addi": return "add #%s,%s" % (h(S.s8(i.imm)), R(n))
    if o == "cmpim": return "cmp/eq #%s,r0" % h(S.s8(i.imm))
    if o in ("tsti","andi","xori","ori"): return "%s #%s,r0" % (o[:-1], h(i.imm))
    if o in ("tstb","andb","xorb","orb"): return "%s.b #%s,@(r0,gbr)" % (o[:-1], h(i.imm))
    if o == "mova": return "mova %s,r0" % h(S.pcrel_addr(i, pc))
    if o == "trapa": return "trapa #%s" % h(i.imm)
    if o in ("bt","bf","bra","bsr"): return "%s %s" % (o, h(S.branch_target(i, pc)))
    if o in ("bts","bfs"): return "%s/s %s" % (o[:2], h(S.branch_target(i, pc)))
    if o in ("jmp","jsr"): return "%s @%s" % (o, R(m))
    if o in ("braf","bsrf"): return "%s %s" % (o, R(m))
    if o in ("pref","ocbi","ocbp","ocbwb","tas"): return "%s @%s" % ("tas.b" if o=="tas" else o, R(n))
    if o == "movca": return "movca.l r0,@%s" % R(n)
    if o in ("macl","macw"): return "mac.%s @%s+,@%s+" % (o[-1], R(m), R(n))
    sr = {"sr":"sr","gbr":"gbr","vbr":"vbr","ssr":"ssr","spc":"spc","sgr":"sgr","dbr":"dbr","mach":"mach","macl":"macl","pr":"pr","fpul":"fpul","fpscr":"fpscr"}
    for p in ("stc_","sts_"):
        if o.startswith(p):
            r = o[4:]
            if r == "bank": return "stc r%d_bank,%s" % (m, R(n))
            return "%s %s,%s" % (p[:3], r, R(n))
    for p in ("stcl_","stsl_"):
        if o.startswith(p):
            r = o[5:]
            if r == "bank": return "stc.l r%d_bank,@-%s" % (m, R(n))
            return "%s.l %s,@-%s" % (p[:3], r, R(n))
    for p in ("ldc_","lds_"):
        if o.startswith(p):
            r = o[4:]
            if r == "bank": return "ldc %s,r%d_bank" % (R(m), n)
            return "%s %s,%s" % (p[:3], R(m), r)
    for p in ("ldcl_","ldsl_"):
        if o.startswith(p):
            r = o[5:]
            if r == "bank": return "ldc.l @%s+,r%d_bank" % (R(m), n)
            return "%s.l @%s+,%s" % (p[:3], R(m), r)
    F2 = {"fadd":"fadd","fsub":"fsub","fmul":"fmul","fdiv":"fdiv","fcmpeq":"fcmp/eq","fcmpgt":"fcmp/gt","fmov":"fmov"}
    if o in F2: return "%s %s,%s" % (F2[o], FR(m), FR(n))
    if o == "fmac": return "fmac fr0,%s,%s" % (FR(m), FR(n))
    if o == "fmov_l": return "fmov.s @%s,%s" % (R(m), FR(n))
    if o == "fmov_s": return "fmov.s %s,@%s" % (FR(m), R(n))
    if o == "fmov_p": return "fmov.s @%s+,%s" % (R(m), FR(n))
    if o == "fmov_m": return "fmov.s %s,@-%s" % (FR(m), R(n))
    if o == "fmov_l0": return "fmov.s @(r0,%s),%s" % (R(m), FR(n))
    if o == "fmov_s0": return "fmov.s %s,@(r0,%s)" % (FR(m), R(n))
    if o == "fsts": return "fsts fpul,%s" % FR(n)
    if o == "flds": return "flds %s,fpul" % FR(m)
    if o == "float": return "float fpul,%s" % FR(n)
    if o == "ftrc": return "ftrc %s,fpul" % FR(m)
    if o in ("fneg","fabs","fsqrt","fsrra","fldi0","fldi1"): return "%s %s" % (o, FR(n))
    if o == "fcnvsd": return "fcnvsd fpul,dr%d" % n
    if o == "fcnvds": return "fcnvds dr%d,fpul" % m
    if o == "fipr": return "fipr fv%d,fv%d" % (m*4, n*4)
    if o == "ftrv": return "ftrv xmtrx,fv%d" % (n*4)
    if o == "fsca": return "fsca fpul,dr%d" % (n*2)
    return o + " ?"
