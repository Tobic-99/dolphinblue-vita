"""Generate SH-4 instruction-semantics unit tests.

Each test is a small SH-4 program (assembled here from the decoder's own encoding table),
run through the real discovery + translator, and checked against expected values derived
from the SH-4 manual (and, where noted, Flycast's interpreter behaviour). Expected values
are written by hand as plain numbers, not computed by the translator's helpers.

Output: ../tests/gen/test_funcs.cpp, test_cases.inc, test_image.bin
"""
import os, struct, sys
import sh4dec as S
import discover as D
import codegen as G

BASE = 0x8C010000

# --- encoder from the decoder table -----------------------------------------------------------
ENC = {}
for pat, op in S.TABLE:
    ENC[op] = pat

def enc(op, n=0, m=0, i=0, d=0):
    pat = ENC[op]
    w = 0
    pos = {"n": n, "m": m, "i": i, "d": d}
    cnt = {k: pat.count(k) for k in "nmid"}
    used = {k: 0 for k in "nmid"}
    for idx, ch in enumerate(pat):
        bit = 15 - idx
        if ch in "01":
            w |= int(ch) << bit
        else:
            width = cnt[ch]
            k = width - 1 - used[ch]   # MSB first within the field
            used[ch] += 1
            w |= ((pos[ch] >> k) & 1) << bit
    dec = S.decode(w)
    assert dec is not None and dec.op == op, (op, hex(w), dec)
    return w


class Asm:
    def __init__(s, base):
        s.base = base; s.items = []; s.labels = {}
    def L(s, name): s.items.append(("label", name)); return s
    def I(s, op, **kw): s.items.append(("ins", op, kw)); return s
    def W16(s, v): s.items.append(("w16", v)); return s
    def W32(s, v): s.items.append(("w32", v)); return s
    def ALIGN4(s): s.items.append(("align4",)); return s
    def assemble(s):
        # pass 1
        pc = s.base
        for it in s.items:
            if it[0] == "label": s.labels[it[1]] = pc
            elif it[0] in ("ins", "w16"): pc += 2
            elif it[0] == "w32": pc += 4
            elif it[0] == "align4":
                if pc & 2: pc += 2
        # pass 2
        out = bytearray(); pc = s.base
        for it in s.items:
            if it[0] == "label": continue
            if it[0] == "align4":
                if pc & 2: out += struct.pack("<H", 0x0009); pc += 2
                continue
            if it[0] == "w16":
                v = it[1]
                if isinstance(v, tuple):  # ("rel", label, base_label) -> label - base
                    v = s.labels[v[1]] - s.labels[v[2]]
                out += struct.pack("<H", v & 0xFFFF); pc += 2; continue
            if it[0] == "w32":
                v = it[1]
                if isinstance(v, str): v = s.labels[v]
                out += struct.pack("<I", v & 0xFFFFFFFF); pc += 4; continue
            op, kw = it[1], dict(it[2])
            if "to" in kw:
                t = s.labels[kw.pop("to")]
                if op in ("bt", "bf", "bts", "bfs", "bra", "bsr"):
                    disp = (t - (pc + 4)) // 2
                    kw["d"] = disp & (0xFF if op in ("bt", "bf", "bts", "bfs") else 0xFFF)
                elif op in ("movl_pc", "mova"):
                    kw["d"] = (t - ((pc + 4) & ~3)) // 4
                    assert kw["d"] >= 0
                elif op == "movw_pc":
                    kw["d"] = (t - (pc + 4)) // 2
            if "i" in kw: kw["i"] &= 0xFF
            out += struct.pack("<H", enc(op, **kw)); pc += 2
        return bytes(out)


# --- tests ----------------------------------------------------------------------------------
# Each test: name, builder(asm) emitting the function body starting at label <name>,
# init: list of C++ statements, checks: list of (cpp_expr, expected_value_cpp)
TESTS = []
def test(name, init, checks):
    def deco(fn):
        TESTS.append((name, fn, init, checks)); return fn
    return deco

def ret(a): a.I("rts").I("nop")

@test("addc_carry", ["c->r[1]=0xFFFFFFFFu; c->r[2]=1; c->t=0;"], [("c->r[1]", "0u"), ("c->t", "1u")])
def _(a): a.I("addc", n=1, m=2); ret(a)

@test("addc_tin", ["c->r[1]=0xFFFFFFFEu; c->r[2]=1; c->t=1;"], [("c->r[1]", "0u"), ("c->t", "1u")])
def _(a): a.I("addc", n=1, m=2); ret(a)

@test("subc_borrow", ["c->r[1]=0; c->r[2]=1; c->t=0;"], [("c->r[1]", "0xFFFFFFFFu"), ("c->t", "1u")])
def _(a): a.I("subc", n=1, m=2); ret(a)

@test("negc", ["c->r[2]=0; c->t=1;"], [("c->r[1]", "0xFFFFFFFFu"), ("c->t", "1u")])
def _(a): a.I("negc", n=1, m=2); ret(a)

@test("addv_ovf", ["c->r[1]=0x7FFFFFFFu; c->r[2]=1;"], [("c->r[1]", "0x80000000u"), ("c->t", "1u")])
def _(a): a.I("addv", n=1, m=2); ret(a)

@test("subv_ovf", ["c->r[1]=0x80000000u; c->r[2]=1;"], [("c->r[1]", "0x7FFFFFFFu"), ("c->t", "1u")])
def _(a): a.I("subv", n=1, m=2); ret(a)

# Unsigned 32/16 division (manual example): R1 / R0, quotient in R1 (low 16 bits)
@test("div_unsigned_100_7", ["c->r[1]=100; c->r[0]=7;"], [("c->r[1]", "14u")])
def _(a):
    a.I("shll16", n=0).I("div0u")
    for _ in range(16): a.I("div1", n=1, m=0)
    a.I("rotcl", n=1).I("extuw", n=1, m=1); ret(a)

# Signed 16-bit division (manual example): R1 (dividend) / R0 (divisor)
@test("div_signed_m100_7", ["c->r[1]=(uint32_t)-100; c->r[0]=7;"], [("c->r[1]", "(uint32_t)-14")])
def _(a):
    a.I("shll16", n=0).I("extsw", n=1, m=1).I("xor", n=2, m=2).I("mov", n=3, m=1).I("rotcl", n=3)
    a.I("subc", n=1, m=2).I("div0s", n=1, m=0)
    for _ in range(16): a.I("div1", n=1, m=0)
    a.I("extsw", n=1, m=1).I("rotcl", n=1).I("addc", n=1, m=2).I("extsw", n=1, m=1); ret(a)

@test("shad_neg31", ["c->r[1]=0x80000000u; c->r[2]=(uint32_t)-31;"], [("c->r[1]", "0xFFFFFFFFu")])
def _(a): a.I("shad", n=1, m=2); ret(a)

@test("shad_neg32", ["c->r[1]=0x80000000u; c->r[2]=0xFFFFFFE0u;"], [("c->r[1]", "0xFFFFFFFFu")])
def _(a): a.I("shad", n=1, m=2); ret(a)

@test("shld_neg1", ["c->r[1]=0x80000000u; c->r[2]=(uint32_t)-1;"], [("c->r[1]", "0x40000000u")])
def _(a): a.I("shld", n=1, m=2); ret(a)

@test("shad_pos4", ["c->r[1]=1; c->r[2]=4;"], [("c->r[1]", "16u")])
def _(a): a.I("shad", n=1, m=2); ret(a)

@test("rotcr_rotcl", ["c->r[1]=1; c->r[2]=0x80000000u; c->t=0;"],
      [("c->r[1]", "0u"), ("c->r[2]", "1u"), ("c->t", "0u")])
def _(a):  # rotcr r1: T<-1, r1=0 ; rotcl r2: r2=(0<<1)|1=1, T<-1 ; then clrt? no: check T after rotcl=1 -> use shlr to clear
    a.I("rotcr", n=1).I("rotcl", n=2).I("clrt"); ret(a)

@test("cmpstr", ["c->r[1]=0x41424344u; c->r[2]=0x11224344u;"], [("c->t", "1u")])
def _(a): a.I("cmpstr", n=1, m=2); ret(a)

@test("cmpstr_none", ["c->r[1]=0x41424344u; c->r[2]=0x11111111u;"], [("c->t", "0u")])
def _(a): a.I("cmpstr", n=1, m=2); ret(a)

@test("dt_loop", ["c->r[1]=3; c->r[2]=0;"], [("c->r[1]", "0u"), ("c->r[2]", "3u"), ("c->t", "1u")])
def _(a):
    a.L("dt_loop_top").I("addi", n=2, i=1).I("dt", n=1).I("bf", to="dt_loop_top"); ret(a)

@test("bts_slot", ["c->r[1]=5; c->r[3]=0; c->r[4]=0;"], [("c->r[3]", "7u"), ("c->r[4]", "0u")])
def _(a):
    a.I("cmpim", i=5)  # r0 vs 5: r0 init 0 -> use r1
    # T = (r1 == r1) -> 1
    a.I("cmpeq", n=1, m=1).I("bts", to="bts_slot_t").I("movi", n=3, i=7)
    a.I("movi", n=4, i=1)       # skipped
    a.L("bts_slot_t"); ret(a)

@test("bfs_not_taken", ["c->r[3]=0; c->r[4]=0;"], [("c->r[3]", "7u"), ("c->r[4]", "1u")])
def _(a):
    a.I("sett").I("bfs", to="bfs_nt_t").I("movi", n=3, i=7)
    a.I("movi", n=4, i=1)
    a.L("bfs_nt_t"); ret(a)

@test("bsr_rts", ["c->r[0]=0;"], [("c->r[0]", "43u")])
def _(a):
    a.I("stsl_pr", n=15).I("bsr", to="bsr_callee").I("nop").I("addi", n=0, i=1)
    a.I("ldsl_pr", m=15); ret(a)
    a.L("bsr_callee").I("rts").I("movi", n=0, i=42)

@test("jsr_pr_value", [], [("c->r[5]", "0x%08xu" % 0)])  # placeholder, patched below
def _(a):
    a.I("stsl_pr", n=15).I("movl_pc", n=1, to="jsr_lit").I("jsr", m=1).I("nop")
    a.I("ldsl_pr", m=15); ret(a)
    a.ALIGN4().L("jsr_lit").W32("jsr_callee")
    a.L("jsr_callee").I("rts").I("sts_pr", n=5)

@test("movl_predec_same_reg", ["c->r[1]=0x8C100010u;"],
      [("c->r[1]", "0x8C10000Cu"), ("::sh4::rd32(0x8C10000Cu)", "0x8C100010u")])
def _(a): a.I("movl_m", n=1, m=1); ret(a)

@test("movl_postinc_same_reg", ["c->r[1]=0x8C100020u; ::sh4::wr32(0x8C100020u, 0x1234u);"], [("c->r[1]", "0x1234u")])
def _(a): a.I("movl_p", n=1, m=1); ret(a)

@test("movw_signext", ["c->r[1]=0x8C100030u; ::sh4::wr16(0x8C100030u, 0x8001u);"], [("c->r[2]", "0xFFFF8001u")])
def _(a): a.I("movw_l", n=2, m=1); ret(a)

@test("swap_xtrct", ["c->r[1]=0x11223344u; c->r[4]=0xAABBCCDDu;"],
      [("c->r[2]", "0x11224433u"), ("c->r[3]", "0x33441122u"), ("c->r[4]", "0x3344AABBu")])
def _(a): a.I("swapb", n=2, m=1).I("swapw", n=3, m=1).I("xtrct", n=4, m=1); ret(a)

@test("dmuls", ["c->r[1]=(uint32_t)-2; c->r[2]=3;"], [("c->mach", "0xFFFFFFFFu"), ("c->macl", "0xFFFFFFFAu")])
def _(a): a.I("dmuls", n=1, m=2); ret(a)

@test("dmulu", ["c->r[1]=0xFFFFFFFFu; c->r[2]=2;"], [("c->mach", "1u"), ("c->macl", "0xFFFFFFFEu")])
def _(a): a.I("dmulu", n=1, m=2); ret(a)

@test("mulsw_muluw", ["c->r[1]=0xFFFFu; c->r[2]=2;"], [("c->r[3]", "0xFFFFFFFEu"), ("c->macl", "0x1FFFEu")])
def _(a): a.I("mulsw", n=1, m=2).I("sts_macl", n=3).I("muluw", n=1, m=2); ret(a)

@test("tas", ["c->r[1]=0x8C100040u; ::sh4::wr8(0x8C100040u, 0);"], [("c->t", "1u"), ("::sh4::rd8(0x8C100040u)", "0x80u")])
def _(a): a.I("tas", n=1); ret(a)

@test("movt_ext_neg", ["c->r[1]=0x80u; c->r[2]=0xFFFF8000u;"],
      [("c->r[3]", "0xFFFFFF80u"), ("c->r[4]", "0x8000u"), ("c->r[5]", "0x7FFFFFFFu"), ("c->r[6]", "1u"), ("c->r[7]", "0x8000u")])
def _(a):
    a.I("extsb", n=3, m=1).I("extuw", n=4, m=2).I("movi", n=5, i=0).I("addi", n=5, i=-1).I("shlr", n=5)
    a.I("movt", n=6).I("neg", n=7, m=2); ret(a)

@test("float_fadd_ftrc", ["c->fpul=3; c->fpscr=0x00040000u;"], [("c->fpul", "4u"), ("c->fr.u[0][1]", "0x40800000u")])
def _(a):
    a.I("fldi1", n=0).I("float", n=1).I("fadd", n=1, m=0).I("ftrc", m=1); ret(a)

@test("ftrc_saturate", ["c->fpscr=0x00040000u; c->fr.f[0][1]=3e9f; c->fr.f[0][2]=-3e9f; c->fr.u[0][3]=0x7FC00000u;"],
      [("c->r[1]", "0x7FFFFFFFu"), ("c->r[2]", "0x80000000u"), ("c->r[3]", "0x80000000u")])
def _(a):
    a.I("ftrc", m=1).I("sts_fpul", n=1).I("ftrc", m=2).I("sts_fpul", n=2).I("ftrc", m=3).I("sts_fpul", n=3); ret(a)

@test("fmac", ["c->fpscr=0x00040000u; c->fr.f[0][0]=2.0f; c->fr.f[0][1]=3.0f; c->fr.f[0][2]=1.0f;"], [("c->fr.u[0][2]", "0x40E00000u")])
def _(a): a.I("fmac", n=2, m=1); ret(a)

@test("fmov64_sz", ["c->fpscr=0x00040000u; c->r[1]=0x8C100050u; ::sh4::wr32(0x8C100050u,0x11111111u); ::sh4::wr32(0x8C100054u,0x22222222u);"],
      [("c->fr.u[0][2]", "0x11111111u"), ("c->fr.u[0][3]", "0x22222222u"), ("c->fr.u[1][0]", "0x11111111u"),
       ("c->fr.u[1][1]", "0x22222222u"), ("c->r[1]", "0x8C100058u"), ("c->fpscr", "0x00040000u")])
def _(a):
    a.I("fschg").I("fmov_p", n=2, m=1).I("fmov", n=1, m=2).I("fschg"); ret(a)   # fmov dr2,xd0 : n=1 (XD0), m=2 (DR2)

@test("double_pr1", ["c->fpscr=0x00040000u; c->fpul=5;"], [("c->fpul", "10u"), ("c->fpscr", "0x00040000u")])
def _(a):
    a.I("movl_pc", n=1, to="dpr_lit1").I("lds_fpscr", m=1).I("float", n=0).I("fadd", n=0, m=0).I("ftrc", m=0)
    a.I("movl_pc", n=1, to="dpr_lit2").I("lds_fpscr", m=1); ret(a)
    a.ALIGN4().L("dpr_lit1").W32(0x000C0000).L("dpr_lit2").W32(0x00040000)

@test("ftrv_scale2", ["c->fpscr=0x00040000u; for(int i=0;i<16;i++) c->fr.f[1][i]=0; for(int i=0;i<4;i++) c->fr.f[1][i*5]=2.0f;"
                      " c->fr.f[0][4]=1; c->fr.f[0][5]=2; c->fr.f[0][6]=3; c->fr.f[0][7]=4;"],
      [("c->fr.u[0][4]", "0x40000000u"), ("c->fr.u[0][5]", "0x40800000u"), ("c->fr.u[0][6]", "0x40C00000u"), ("c->fr.u[0][7]", "0x41000000u")])
def _(a): a.I("ftrv", n=1); ret(a)

@test("ftrv_layout", ["c->fpscr=0x00040000u; for(int i=0;i<16;i++) c->fr.f[1][i]=0; c->fr.f[1][4]=1.0f;"
                      " c->fr.f[0][0]=0; c->fr.f[0][1]=5; c->fr.f[0][2]=0; c->fr.f[0][3]=0;"],
      [("c->fr.u[0][0]", "0x40A00000u"), ("c->fr.u[0][1]", "0u")])
def _(a): a.I("ftrv", n=0); ret(a)   # XF4 is row0,col1: out0 = XF4*FV1 = 5

@test("fipr", ["c->fpscr=0x00040000u; for(int i=0;i<8;i++) c->fr.f[0][i]=(float)(i+1);"], [("c->fr.u[0][7]", "0x428C0000u")])
def _(a): a.I("fipr", n=1, m=0); ret(a)   # fipr fv0,fv4 -> fr7 = 1*5+2*6+3*7+4*8 = 70

@test("fsca_90", ["c->fpscr=0x00040000u; c->fpul=0x4000;"], [("c->fr.u[0][2]", "0x3F800000u"), ("(fabsf(c->fr.f[0][3]) < 1e-6f)", "1")])
def _(a): a.I("fsca", n=1); ret(a)

@test("fneg_fabs_fcmp", ["c->fpscr=0x00040000u; c->fr.f[0][1]=2.5f; c->fr.f[0][2]=-1.0f;"],
      [("c->fr.u[0][1]", "0xC0200000u"), ("c->fr.u[0][2]", "0x3F800000u"), ("c->t", "1u")])
def _(a): a.I("fneg", n=1).I("fabs", n=2).I("fcmpgt", n=2, m=1); ret(a)

@test("frchg_bank", ["c->fpscr=0x00040000u; c->fr.f[1][3]=9.0f;"], [("c->fr.u[0][5]", "0x41100000u"), ("c->fpscr", "0x00040000u")])
def _(a): a.I("frchg").I("fmov", n=4, m=3).I("frchg").I("fmov", n=5, m=4).I("frchg").I("fmov", n=5, m=4).I("frchg"); ret(a)
# after 1st frchg: FR bank=1, so fr4(bank1)=fr3(bank1)=9; back to bank0: fr5(b0)=fr4(b0) (=0); bank1: fr5(b1)=fr4(b1)=9; back
# => check bank0 fr5? it gets fr4(b0)=0. Adjust: expected listed for bank0 fr5 is wrong -> fixed in TEST_FIX below.

@test("switch_braf", ["c->r[0]=2; c->r[2]=0;"], [("c->r[2]", "30u")])
def _(a):
    a.I("movi", n=1, i=3).I("cmphs", n=0, m=1).I("bt", to="sw_def")
    a.I("shll", n=0).I("mov", n=1, m=0).I("mova", to="sw_tab").I("movw_l0", n=0, m=1).I("braf", m=0).I("nop").L("sw_base")
    a.ALIGN4().L("sw_tab").W16(("rel", "sw_c0", "sw_base")).W16(("rel", "sw_c1", "sw_base")).W16(("rel", "sw_c2", "sw_base"))
    a.L("sw_c0").I("movi", n=2, i=10).I("bra", to="sw_end").I("nop")
    a.L("sw_c1").I("movi", n=2, i=20).I("bra", to="sw_end").I("nop")
    a.L("sw_c2").I("movi", n=2, i=30).I("bra", to="sw_end").I("nop")
    a.L("sw_def").I("movi", n=2, i=99)
    a.L("sw_end"); ret(a)

@test("indirect_dispatch", ["c->r[0]=0; ::sh4::wr32(0x8C100060u, 0); /* patched with callee addr */"], [("c->r[0]", "77u")])
def _(a):
    a.I("stsl_pr", n=15).I("movl_pc", n=1, to="ind_lit").I("movl_l", n=3, m=1).I("jsr", m=3).I("nop")
    a.I("ldsl_pr", m=15); ret(a)
    a.ALIGN4().L("ind_lit").W32(0x8C100060)
    a.L("ind_callee").I("rts").I("movi", n=0, i=77)
    a.ALIGN4().L("ind_ptr").W32("ind_callee")   # data pointer so discovery proves the callee


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    a = Asm(BASE)
    for name, fn, init, checks in TESTS:
        a.ALIGN4(); a.L(name); fn(a)
    blob = a.assemble()
    img_path = os.path.join(outdir, "test_image.bin")
    open(img_path, "wb").write(blob)
    img = S.Image(img_path, BASE)
    d = D.Discover(img)
    known = set(a.labels[n] for n, *_ in TESTS)
    for n, *_ in TESTS:
        d.prov[a.labels[n]].add("entry")
    for e in sorted(known):
        d.run_from(e, known)
    # data pointer proof for the indirect callee (same rule as the real image: data word -> function)
    cal = a.labels["ind_callee"]
    if cal not in d.funcs:
        d.prov[cal].add("dataptr"); d.run_from(cal, set(d.funcs) | {cal})
    final = set(d.funcs)
    for e in sorted(final):
        d.funcs[e] = d.analyze_func(e, final)
    lines = ['#include "sh4ctx.h"', '#include "test_funcs.h"', ""]
    protos = []
    for e in sorted(d.funcs):
        f = d.funcs[e]
        g = G.FuncGen(d, f, {}, d.constprop(f))
        g.tailcalls = set(); g.dyn_mode = 0; g.ext_calls = set()
        lines += g.gen(); lines.append("")
        assert not g.untranslated, g.untranslated
        protos.append("void %s(sh4::Ctx* c);" % G.fname(e))
    open(os.path.join(outdir, "test_funcs.cpp"), "w").write("\n".join(lines) + "\n")
    open(os.path.join(outdir, "test_funcs.h"), "w").write("#pragma once\n#include \"sh4ctx.h\"\n" + "\n".join(protos) + "\n")
    disp = ['#include "test_funcs.h"', "namespace testdisp {", "struct E { uint32_t a; void (*f)(sh4::Ctx*); };",
            "const E table[] = {"]
    for e in sorted(d.funcs): disp.append("  {0x%08xu, %s}," % (e, G.fname(e)))
    disp += ["};", "const unsigned count = %d;" % len(d.funcs), "}"]
    open(os.path.join(outdir, "test_dispatch.inc"), "w").write("\n".join(disp) + "\n")
    cases = []
    for name, fn, init, checks in TESTS:
        e = a.labels[name]
        init = list(init)
        if name == "indirect_dispatch":
            init.append("::sh4::wr32(0x8C100060u, 0x%08xu);" % a.labels["ind_callee"])
        if name == "jsr_pr_value":
            # pr inside the callee must be the jsr address + 4
            jsr_addr = e + 2 + 2   # stsl_pr, movl_pc, jsr
            checks = [("c->r[5]", "0x%08xu" % (jsr_addr + 4))]
        if name == "frchg_bank":
            checks = [("c->fr.u[1][4]", "0x41100000u"), ("c->fr.u[1][5]", "0x41100000u"), ("c->fr.u[0][5]", "0u"), ("c->fpscr", "0x00040000u")]
        cases.append('  { "%s", %s, [](sh4::Ctx* c){ %s }, [](sh4::Ctx* c, int& fails){ %s } },' % (
            name, G.fname(e), " ".join(init),
            " ".join('CHECK(%s, %s);' % (x, v) for x, v in checks)))
    open(os.path.join(outdir, "test_cases.inc"), "w").write("\n".join(cases) + "\n")
    print("tests", len(TESTS), "functions", len(d.funcs))


if __name__ == "__main__":
    main()
