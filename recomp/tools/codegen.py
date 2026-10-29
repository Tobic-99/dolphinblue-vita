import re
"""SH-4 -> C++ translator (one C++ function per guest function).

Usage: python3 codegen.py <outdir> [--funcs-per-tu N]
Reads the image, runs discovery (discover.main), analyses FPU modes, emits:
  <outdir>/db_funcs.h          prototypes
  <outdir>/db_funcs_XXX.cpp    translated functions
  <outdir>/db_dispatch.cpp     sorted (addr -> fn) table + lookup
  <outdir>/translate_report.txt
"""
import sys, os, collections
import sh4dec as S
import discover as D

M32 = 0xFFFFFFFF
UNK = None
ENTRY_MODE = (0, 0, 0)   # (PR, SZ, FR) assumed at function entry unless callers say otherwise


def fname(a):
    return "f_%08x" % a


def h(v):
    return "0x%08xu" % (v & M32)



# ---------------------------------------------------------------------------------------------------------
# Register caching: the guest registers a function uses live in C++ locals instead of the Ctx, so guest
# memory stores (which may alias the Ctx as far as the compiler knows) no longer force reloads. The Ctx is
# synchronised (store all locals -> line -> reload all) around any line that hands `c` to code that may read
# or write registers (calls, dispatch, helpers taking register numbers, FR-bank-indexed accesses), before
# returns, and inside SH4_POLL's rarely taken branch.
_RE_R = re.compile(r"c->r\[(\d+)\]")
_RE_FR = re.compile(r"c->fr\.([uf])\[(\d+)\]\[(\d+)\]")
_RE_S = re.compile(r"c->(t|pr|fpul|macl|mach)(?![A-Za-z_0-9])")
_SAFE_CALLS = ("::sh4::pref(c,", "SH4_ENTRY_CHECK(c,", "SH4_POLL(c)")

def _is_sync(line):
    t = line
    for k in _SAFE_CALLS:
        t = t.replace(k, "")
    if re.search(r"\(c[,)]", t):
        return True
    if re.search(r"c->fr\.[uf](?!\[\d+\]\[\d+\])", t):   # bank-indexed or whole-array FPU access
        return True
    return False

_RE_FIPR = re.compile(r"::sh4::fipr\(c, (\d+), (\d+), (\d+)\);")
_RE_FTRV = re.compile(r"::sh4::ftrv\(c, (\d+), (\d+)\);")

def _expand_vec(line):
    """Inline fipr/ftrv with the exact expressions of the sh4ctx.h helpers, so register caching applies."""
    def fr(b, k):
        return "c->fr.f[%d][%d]" % (b, k)
    def fipr(m):
        b, n, k = int(m.group(1)), int(m.group(2)), int(m.group(3))
        terms = " + ".join("(double)%s * %s" % (fr(b, n + i), fr(b, k + i)) for i in range(4))
        return "{ double r_ = %s; %s = (float)r_; }" % (terms, fr(b, n + 3))
    def ftrv(m):
        b, n = int(m.group(1)), int(m.group(2))
        rs = []
        for i in range(4):
            rs.append("t%d_ = " % i + " + ".join("(double)%s * %s" % (fr(b ^ 1, 4 * j + i), fr(b, n + j)) for j in range(4)))
        return "{ double " + ", ".join(rs) + "; " + " ".join("%s = (float)t%d_;" % (fr(b, n + i), i) for i in range(4)) + " }"
    line = _RE_FIPR.sub(fipr, line)
    return _RE_FTRV.sub(ftrv, line)

def cache_regs(out):
    out = [out[0]] + [_expand_vec(l) for l in out[1:-1]] + [out[-1]]
    head, body, tail = out[0], out[1:-1], out[-1]
    entry = re.search(r"void f_([0-9a-f]{8})\(", head).group(1)
    sync = [_is_sync(l) for l in body]
    rs, frs, ss = set(), set(), set()
    for l, sy in zip(body, sync):
        if sy:
            continue
        rs.update(int(m) for m in _RE_R.findall(l))
        frs.update((int(b), int(k)) for _, b, k in _RE_FR.findall(l))
        ss.update(_RE_S.findall(l))
    if not (rs or frs or ss):
        return out
    rs, frs, ss = sorted(rs), sorted(frs), sorted(ss)
    # registers this function assigns: only these can differ from their Ctx copy, so only these are stored at
    # sync points (a never-assigned local is either the entry value or a value just reloaded from the Ctx)
    wr_r, wr_f, wr_s = set(), set(), set()
    _ASG = r"\s*(=(?!=)|\+=|-=|\*=|/=|&=|\|=|\^=|<<=|>>=)"
    for l, sy in zip(body, sync):
        if sy:
            continue
        l2 = _RE_R.sub(lambda m: "R" + m.group(1), _RE_FR.sub(lambda m: "F%s_%s.%s" % (m.group(2), m.group(3), m.group(1)), _RE_S.sub(lambda m: "S_" + m.group(1), l)))
        wr_r.update(int(m[0]) for m in re.findall(r"\bR(\d+)" + _ASG, l2))
        wr_f.update((int(b), int(k)) for b, k, _ in re.findall(r"\bF(\d+)_(\d+)\.[uf]" + _ASG, l2))
        wr_s.update(m for m, _ in re.findall(r"\bS_(\w+)" + _ASG, l2))
        if re.search(r"\+\+|--", l2.replace("--c->slice", "")):
            raise RuntimeError("increment operator in generated line (store elision needs assignments): " + l2)
    store = " ".join(["c->r[%d] = R%d;" % (n, n) for n in rs if n in wr_r] + ["c->fr.u[%d][%d] = F%d_%d.u;" % (b, k, b, k) for b, k in frs if (b, k) in wr_f] +
                     ["c->%s = S_%s;" % (n, n) for n in ss if n in wr_s])
    load = " ".join(["R%d = c->r[%d];" % (n, n) for n in rs] + ["F%d_%d.u = c->fr.u[%d][%d];" % (b, k, b, k) for b, k in frs] +
                    ["S_%s = c->%s;" % (n, n) for n in ss])
    res = [head]
    decl = []
    if rs: decl.append("    uint32_t " + ", ".join("R%d = c->r[%d]" % (n, n) for n in rs) + ";")
    for b, k in frs: decl.append("    union { uint32_t u; float f; } F%d_%d; F%d_%d.u = c->fr.u[%d][%d];" % (b, k, b, k, b, k))
    if ss: decl.append("    uint32_t " + ", ".join("S_%s = c->%s" % (n, n) for n in ss) + ";")
    res += decl
    for l, sy in zip(body, sync):
        if sy:
            res.append("    { " + store + " }")
            res.append(l)
            # after a call returns, this function is executing again (keeps the Vita sampler's attribution right)
            back = (" c->cur_func = 0x%sU;" % entry) if re.search(r"\bf_[0-9a-f]{8}\(c\)|::sh4::dispatch\(c,", l) else ""
            res.append("    { " + load + back + " }")
            continue
        l = _RE_R.sub(lambda m: "R" + m.group(1), l)
        l = _RE_FR.sub(lambda m: "F%s_%s.%s" % (m.group(2), m.group(3), m.group(1)), l)
        l = _RE_S.sub(lambda m: "S_" + m.group(1), l)
        l = l.replace("SH4_POLL(c);", "if (--c->slice <= 0 || c->irq_pending) { " + store + " ::sh4::poll_irq(c); " + load + " c->cur_func = 0x%sU; }" % entry)
        l = re.sub(r"\breturn;", "{ " + store + " return; }", l)
        res.append(l)
    res.append("    " + store)
    res.append(tail)
    return res

class FuncGen:
    def __init__(s, d, f, entry_modes, consts):
        s.d, s.f, s.img = d, f, d.img
        s.entry_modes = entry_modes
        s.consts = consts
        s.untranslated = collections.Counter()
        s.lines = []

    # -------------------------------------------------------------- FPU mode analysis
    @staticmethod
    def fp_modes(f, entry_mode, consts):
        """Forward dataflow of (PR,SZ,FR) per instruction address. Values 0/1/None."""
        modes = {f.entry: entry_mode}
        work = [f.entry]
        res = {}
        def meet(x, y):
            return tuple(a if a == b else None for a, b in zip(x, y))
        def xfer(m, ins, a):
            pr, sz, fr = m
            op = ins.op
            if op == "fschg": sz = None if sz is None else sz ^ 1
            elif op == "frchg": fr = None if fr is None else fr ^ 1
            elif op == "lds_fpscr":
                v = consts.get(a, {}).get(ins.m)
                if isinstance(v, int):
                    pr, sz, fr = (v >> 19) & 1, (v >> 20) & 1, (v >> 21) & 1
                else: pr = sz = fr = None
            elif op == "ldsl_fpscr":
                pr = sz = fr = None
            return (pr, sz, fr)
        it = 0
        while work and it < 500000:
            it += 1
            a = work.pop()
            m = modes[a]
            ins = f.insns.get(a)
            if ins is None: continue
            res[a] = m
            if ins.op in S.DELAY:
                res[a + 2] = m
                m2 = xfer(m, f.insns[a + 2], a + 2)
            else:
                m2 = xfer(m, ins, a)
            for b in f.succ.get(a, []):
                if b not in f.insns: continue
                if b in modes:
                    nm = meet(modes[b], m2)
                    if nm != modes[b] or b not in res:
                        modes[b] = nm; work.append(b)
                else:
                    modes[b] = m2; work.append(b)
        return res

    # -------------------------------------------------------------- emission helpers
    def emit(s, line, ind=1):
        s.lines.append("    " * ind + line)

    def gen(s):
        f = s.f
        s.consts_f = s.consts
        s.modes = FuncGen.fp_modes(f, s.entry_modes.get(f.entry, ENTRY_MODE), s.consts)
        addrs = sorted(f.insns)
        # addresses that are only delay slots of a preceding branch
        slot_of = set()
        for a in addrs:
            if f.insns[a].op in S.DELAY:
                slot_of.add(a + 2)
        targets = set()
        for a, ss in f.succ.items():
            ins = f.insns.get(a)
            if ins is None: continue
            for b in ss:
                targets.add(b)
        # instruction sequence to emit: everything except pure delay slots (unless also a target)
        seq = [a for a in addrs if a not in slot_of or a in targets_branch(f, a)]
        labels = set([f.entry])
        for a in addrs:
            ins = f.insns[a]
            if ins.op in ("bt", "bf", "bts", "bfs", "bra"):
                t = S.branch_target(ins, a)
                if t in f.insns: labels.add(t)
            if a in f.switch:
                for t in f.switch[a]: labels.add(t)
        # fallthrough gaps need labels
        nxt = {}
        for i, a in enumerate(seq):
            nxt[a] = seq[i + 1] if i + 1 < len(seq) else None
        s.labels = labels
        s.nxt = nxt
        body = []
        s.lines = body
        em = s.entry_modes.get(f.entry, ENTRY_MODE)
        mask = 0; val = 0
        for bit, v in zip((19, 20, 21), em):
            if v is not None: mask |= 1 << bit; val |= v << bit
        s.emit("SH4_ENTRY_CHECK(c, %s, %s, %s);" % (h(f.entry), h(mask), h(val)))
        if seq[0] != f.entry:
            s.emit("goto L_%08x;" % f.entry)
        # make sure every needed fallthrough target is labelled (second pass decides)
        out_seq = []
        for a in seq:
            out_seq.append(a)
        s.need_labels = set(labels)
        chunks = []
        for a in out_seq:
            s.lines = []
            ft = s.gen_insn(a)
            chunks.append((a, s.lines, ft))
        # resolve fallthrough gotos
        final = []
        for idx, (a, lines, ft) in enumerate(chunks):
            final.append((a, lines))
            if ft is not None:
                nexta = chunks[idx + 1][0] if idx + 1 < len(chunks) else None
                if nexta != ft:
                    s.need_labels.add(ft)
                    final.append((None, ["    goto L_%08x;" % ft]))
        out = []
        out.append("__attribute__((weak)) void %s(sh4::Ctx* c) {   // weak: host code may override with a native port" % fname(f.entry))
        out += body
        for a, lines in final:
            if a is not None and a in s.need_labels:
                out.append("L_%08x:;" % a)
            out += lines
        out.append("}")
        out = cache_regs(out)
        missing = [t for t in s.need_labels if t not in f.insns]
        if missing:
            raise RuntimeError("missing label targets %s in %08x" % ([hex(x) for x in missing], f.entry))
        return out

    # -------------------------------------------------------------- per instruction
    def gen_insn(s, a):
        """Emit code for the instruction at a. Returns the fallthrough address or None."""
        f = s.f
        ins = f.insns[a]
        op = ins.op
        if op in S.DELAY:
            return s.gen_branch(a, ins)
        if op in ("bt", "bf"):
            t = S.branch_target(ins, a)
            cond = "c->t" if op == "bt" else "!c->t"
            s.emit("if (%s) { %sgoto L_%08x; }" % (cond, "SH4_POLL(c); " if t <= a else "", t))
            s.need_labels.add(t)
            return a + 2
        s.gen_simple(a, ins)
        return a + 2

    def gen_branch(s, a, ins):
        f = s.f
        op = ins.op
        slot_a = a + 2
        slot = f.insns[slot_a]
        def do_slot():
            s.gen_simple(slot_a, slot)
        if op in ("bts", "bfs"):
            t = S.branch_target(ins, a)
            s.emit("{ uint32_t cond_ = %s;" % ("c->t" if op == "bts" else "!c->t"))
            do_slot()
            s.emit("if (cond_) { %sgoto L_%08x; } }" % ("SH4_POLL(c); " if t <= a else "", t))
            s.need_labels.add(t)
            return a + 4
        if op == "bra":
            t = S.branch_target(ins, a)
            do_slot()
            if a in f.tails and f.tails[a] is not None:
                s.emit(s.call_stmt(f.tails[a], True))
                return None
            if t <= a: s.emit("SH4_POLL(c);")
            s.emit("goto L_%08x;" % t)
            s.need_labels.add(t)
            return None
        if op in ("bsr", "bsrf", "jsr"):
            tgt = f.calls.get(a)
            if tgt is None:
                if op == "jsr": s.emit("{ uint32_t tgt_ = c->r[%d];" % ins.m)
                else: s.emit("{ uint32_t tgt_ = %s + c->r[%d];" % (h(a + 4), ins.m))
            else:
                s.emit("{")
            s.emit("c->pr = %s;" % h(a + 4))
            do_slot()
            if tgt is None:
                s.emit("::sh4::dispatch(c, tgt_); }")
            else:
                s.emit(s.call_stmt(tgt, False) + " }")
            s.emit("SH4_POLL(c);")
            return a + 4
        if op in ("rts", "rte"):
            do_slot()
            if op == "rte": s.emit("::sh4::rte(c);")
            s.emit("return;")
            return None
        if op in ("jmp", "braf"):
            if a in f.switch:
                if op == "jmp": s.emit("{ uint32_t tgt_ = c->r[%d];" % ins.m)
                else: s.emit("{ uint32_t tgt_ = %s + c->r[%d];" % (h(a + 4), ins.m))
                do_slot()
                s.emit("switch (tgt_) {")
                for t in sorted(set(f.switch[a])):
                    s.emit("case %s: goto L_%08x;" % (h(t), t))
                    s.need_labels.add(t)
                s.emit("default: ::sh4::fatal_jump(c, %s, tgt_); } }" % h(a))
                return None
            tgt = f.tails.get(a)
            if tgt is not None:
                do_slot()
                if tgt in f.insns and tgt != f.entry and tgt not in s.d.funcs:
                    s.emit("goto L_%08x;" % tgt); s.need_labels.add(tgt)
                else:
                    s.emit(s.call_stmt(tgt, True))
                return None
            if op == "jmp": s.emit("{ uint32_t tgt_ = c->r[%d];" % ins.m)
            else: s.emit("{ uint32_t tgt_ = %s + c->r[%d];" % (h(a + 4), ins.m))
            do_slot()
            s.emit("::sh4::dispatch(c, tgt_); return; }")
            return None
        raise RuntimeError(op)

    def call_stmt(s, t, tail):
        """Direct call when t is a translated function, otherwise runtime dispatch (e.g. BIOS vectors)."""
        if t in s.d.funcs:
            s.tailcalls.add(t)
            return "%s(c);%s" % (fname(t), " return;" if tail else "")
        s.ext_calls.add(t)
        return "::sh4::dispatch(c, %s);%s" % (h(t), " return;" if tail else "")

    def mode(s, a):
        return s.modes.get(a, (None, None, None))

    def gen_simple(s, a, ins):
        op, n, m = ins.op, ins.n, ins.m
        E = s.emit
        R = lambda r: "c->r[%d]" % r
        imm = ins.imm; disp = ins.disp
        # ---------------- moves / loads / stores
        if op == "nop": return
        if op == "mov": E("%s = %s;" % (R(n), R(m))); return
        if op == "movi": E("%s = %s;" % (R(n), h(S.s8(imm)))); return
        if op == "movl_pc":
            la = S.pcrel_addr(ins, a)
            E("%s = %s; /* lit @%08x */" % (R(n), h(s.img.u32(la)), la)); return
        if op == "movw_pc":
            la = S.pcrel_addr(ins, a)
            E("%s = %s; /* lit @%08x */" % (R(n), h(s.img.s16(la)), la)); return
        if op == "mova": E("c->r[0] = %s;" % h(S.pcrel_addr(ins, a))); return
        sizes = {"b": ("rd8", "wr8", "::sh4::sx8", 1), "w": ("rd16", "wr16", "::sh4::sx16", 2), "l": ("rd32", "wr32", "", 4)}
        if op.startswith("mov") and len(op) > 4 and op[3] in "bwl" and op[4] == "_":
            rd, wr, sx, sz = sizes[op[3]]
            k = op[5:]
            ld = lambda addr: "%s(::sh4::%s(%s))" % (sx, rd, addr) if sx else "::sh4::%s(%s)" % (rd, addr)
            if k == "s": E("::sh4::%s(%s, %s);" % (wr, R(n), R(m))); return
            if k == "l": E("%s = %s;" % (R(n), ld(R(m)))); return
            if k == "m":
                E("{ uint32_t v_ = %s, a_ = %s - %d; ::sh4::%s(a_, v_); %s = a_; }" % (R(m), R(n), sz, wr, R(n))); return
            if k == "p":
                if n == m: E("%s = %s;" % (R(n), ld(R(m))))
                else: E("%s = %s; %s += %d;" % (R(n), ld(R(m)), R(m), sz))
                return
            if k == "s0": E("::sh4::%s(%s + c->r[0], %s);" % (wr, R(n), R(m))); return
            if k == "l0": E("%s = %s;" % (R(n), ld("%s + c->r[0]" % R(m)))); return
            if k == "s4":
                if op == "movl_s4": E("::sh4::wr32(%s + %d, %s);" % (R(n), disp * 4, R(m)))
                else: E("::sh4::%s(%s + %d, c->r[0]);" % (wr, R(n), disp * sz))
                return
            if k == "l4":
                if op == "movl_l4": E("%s = %s;" % (R(n), ld("%s + %d" % (R(m), disp * 4))))
                else: E("c->r[0] = %s;" % ld("%s + %d" % (R(m), disp * sz)))
                return
            if k == "sg": E("::sh4::%s(c->gbr + %d, c->r[0]);" % (wr, disp * sz)); return
            if k == "lg": E("c->r[0] = %s;" % ld("c->gbr + %d" % (disp * sz))); return
        # ---------------- arithmetic / logic
        bin_ops = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}
        if op in bin_ops: E("%s %s= %s;" % (R(n), bin_ops[op], R(m))); return
        if op == "addi": E("%s += %s;" % (R(n), h(S.s8(imm)))); return
        if op in ("andi", "ori", "xori"):
            E("c->r[0] %s= 0x%x;" % ({"andi": "&", "ori": "|", "xori": "^"}[op], imm)); return
        if op in ("andb", "orb", "xorb"):
            o = {"andb": "&", "orb": "|", "xorb": "^"}[op]
            E("{ uint32_t a_ = c->gbr + c->r[0]; ::sh4::wr8(a_, ::sh4::rd8(a_) %s 0x%x); }" % (o, imm)); return
        if op == "tstb": E("c->t = (::sh4::rd8(c->gbr + c->r[0]) & 0x%x) == 0;" % imm); return
        if op == "tst": E("c->t = (%s & %s) == 0;" % (R(n), R(m))); return
        if op == "tsti": E("c->t = (c->r[0] & 0x%x) == 0;" % imm); return
        if op == "cmpim": E("c->t = c->r[0] == %s;" % h(S.s8(imm))); return
        cmps = {"cmpeq": "%s == %s", "cmphs": "%s >= %s", "cmphi": "%s > %s",
                "cmpge": "(int32_t)%s >= (int32_t)%s", "cmpgt": "(int32_t)%s > (int32_t)%s"}
        if op in cmps: E("c->t = %s;" % (cmps[op] % (R(n), R(m)))); return
        if op == "cmppz": E("c->t = (int32_t)%s >= 0;" % R(n)); return
        if op == "cmppl": E("c->t = (int32_t)%s > 0;" % R(n)); return
        if op == "cmpstr": E("c->t = ::sh4::cmpstr(%s, %s);" % (R(n), R(m))); return
        if op in ("addc", "subc", "negc", "addv", "subv", "div0s", "div1", "shad", "shld", "dmuls", "dmulu", "macl", "macw"):
            E("::sh4::%s(c, %d, %d);" % (op, n, m)); return
        if op == "div0u": E("::sh4::div0u(c);"); return
        if op == "neg": E("%s = 0u - %s;" % (R(n), R(m))); return
        if op == "not": E("%s = ~%s;" % (R(n), R(m))); return
        if op == "extub": E("%s = %s & 0xFFu;" % (R(n), R(m))); return
        if op == "extuw": E("%s = %s & 0xFFFFu;" % (R(n), R(m))); return
        if op == "extsb": E("%s = ::sh4::sx8(%s);" % (R(n), R(m))); return
        if op == "extsw": E("%s = ::sh4::sx16(%s);" % (R(n), R(m))); return
        if op == "swapb": E("%s = (%s & 0xFFFF0000u) | ((%s & 0xFFu) << 8) | ((%s >> 8) & 0xFFu);" % (R(n), R(m), R(m), R(m))); return
        if op == "swapw": E("%s = (%s << 16) | (%s >> 16);" % (R(n), R(m), R(m))); return
        if op == "xtrct": E("%s = (%s >> 16) | (%s << 16);" % (R(n), R(n), R(m))); return
        if op == "mull": E("c->macl = %s * %s;" % (R(n), R(m))); return
        if op == "mulsw": E("c->macl = (uint32_t)((int32_t)(int16_t)%s * (int32_t)(int16_t)%s);" % (R(n), R(m))); return
        if op == "muluw": E("c->macl = (uint32_t)(uint16_t)%s * (uint32_t)(uint16_t)%s;" % (R(n), R(m))); return
        if op == "dt": E("%s -= 1; c->t = %s == 0;" % (R(n), R(n))); return
        if op == "movt": E("%s = c->t;" % R(n)); return
        if op == "clrt": E("c->t = 0;"); return
        if op == "sett": E("c->t = 1;"); return
        if op == "clrmac": E("c->mach = c->macl = 0;"); return
        if op == "clrs": E("c->sr &= ~::sh4::SR_S;"); return
        if op == "sets": E("c->sr |= ::sh4::SR_S;"); return
        shifts = {"shll2": "<< 2", "shll8": "<< 8", "shll16": "<< 16", "shlr2": ">> 2", "shlr8": ">> 8", "shlr16": ">> 16"}
        if op in shifts: E("%s = %s %s;" % (R(n), R(n), shifts[op])); return
        if op in ("shll", "shal"): E("c->t = %s >> 31; %s <<= 1;" % (R(n), R(n))); return
        if op == "shlr": E("c->t = %s & 1; %s >>= 1;" % (R(n), R(n))); return
        if op == "shar": E("c->t = %s & 1; %s = (uint32_t)((int32_t)%s >> 1);" % (R(n), R(n), R(n))); return
        if op == "rotl": E("c->t = %s >> 31; %s = (%s << 1) | c->t;" % (R(n), R(n), R(n))); return
        if op == "rotr": E("c->t = %s & 1; %s = (%s >> 1) | (c->t << 31);" % (R(n), R(n), R(n))); return
        if op == "rotcl": E("{ uint32_t t_ = %s >> 31; %s = (%s << 1) | c->t; c->t = t_; }" % (R(n), R(n), R(n))); return
        if op == "rotcr": E("{ uint32_t t_ = %s & 1; %s = (%s >> 1) | (c->t << 31); c->t = t_; }" % (R(n), R(n), R(n))); return
        if op == "tas": E("::sh4::tas(c, %d);" % n); return
        # ---------------- system / control registers
        creg = {"gbr": "c->gbr", "vbr": "c->vbr", "ssr": "c->ssr", "spc": "c->spc", "sgr": "c->sgr", "dbr": "c->dbr",
                "mach": "c->mach", "macl": "c->macl", "pr": "c->pr", "fpul": "c->fpul", "fpscr": "c->fpscr"}
        if op in ("stc_sr",): E("%s = ::sh4::get_sr(c);" % R(n)); return
        if op in ("stcl_sr",): E("%s -= 4; ::sh4::wr32(%s, ::sh4::get_sr(c));" % (R(n), R(n))); return
        if op == "ldc_sr": E("::sh4::set_sr(c, %s);" % R(m)); return
        if op == "ldcl_sr": E("{ uint32_t v_ = ::sh4::rd32(%s); %s += 4; ::sh4::set_sr(c, v_); }" % (R(m), R(m))); return
        if op in ("stc_bank",): E("%s = c->rbank[%d];" % (R(n), m)); return
        if op in ("stcl_bank",): E("%s -= 4; ::sh4::wr32(%s, c->rbank[%d]);" % (R(n), R(n), m)); return
        if op in ("ldc_bank",): E("c->rbank[%d] = %s;" % (n, R(m))); return
        if op in ("ldcl_bank",): E("c->rbank[%d] = ::sh4::rd32(%s); %s += 4;" % (n, R(m), R(m))); return
        for pre in ("stc_", "sts_"):
            if op.startswith(pre) and op[4:] in creg:
                E("%s = %s;" % (R(n), creg[op[4:]])); return
        for pre in ("stcl_", "stsl_"):
            if op.startswith(pre) and op[5:] in creg:
                E("%s -= 4; ::sh4::wr32(%s, %s);" % (R(n), R(n), creg[op[5:]])); return
        for pre in ("ldc_", "lds_"):
            if op.startswith(pre) and op[4:] in creg:
                E("%s = %s;" % (creg[op[4:]], R(m))); return
        for pre in ("ldcl_", "ldsl_"):
            if op.startswith(pre) and op[5:] in creg:
                if n == m or True:
                    E("{ uint32_t v_ = ::sh4::rd32(%s); %s += 4; %s = v_; }" % (R(m), R(m), creg[op[5:]]))
                return
        if op == "pref": E("::sh4::pref(c, %s);" % R(n)); return
        if op in ("ocbi", "ocbp", "ocbwb"): return  # cache ops: no-op on host
        if op == "movca": E("::sh4::wr32(%s, c->r[0]);" % R(n)); return
        if op == "trapa": E("::sh4::trapa(c, 0x%x, %s);" % (imm, h(a))); return
        if op == "sleep": E("::sh4::sleep(c, %s);" % h(a)); return
        if op == "ldtlb": E("::sh4::ldtlb(c);"); return
        if op == "fschg": E("c->fpscr ^= ::sh4::FPSCR_SZ;"); return
        if op == "frchg": E("c->fpscr ^= ::sh4::FPSCR_FR;"); return
        if op.startswith("f"):
            return s.gen_fpu(a, ins)
        s.untranslated[op] += 1
        E("#error untranslated %s at %08x" % (op, a))

    # -------------------------------------------------------------- FPU
    def gen_fpu(s, a, ins):
        pr, sz, fr = s.mode(a)
        op = ins.op
        needs_pr = op in ("fadd", "fsub", "fmul", "fdiv", "fcmpeq", "fcmpgt", "float", "ftrc", "fneg", "fabs",
                          "fsqrt", "fcnvsd", "fcnvds", "fldi0", "fldi1", "fmac", "fipr", "ftrv", "fsrra", "fsca")
        needs_sz = op in ("fmov", "fmov_l", "fmov_s", "fmov_p", "fmov_m", "fmov_l0", "fmov_s0")
        B = str(fr) if fr is not None else "::sh4::frb(c)"
        if needs_pr and pr is None:
            s.emit("if (c->fpscr & ::sh4::FPSCR_PR) {")
            s.gen_fpu_k(a, ins, 1, sz, B, 2)
            s.emit("} else {")
            s.gen_fpu_k(a, ins, 0, sz, B, 2)
            s.emit("}")
            s.dyn_mode += 1
            return
        if needs_sz and sz is None:
            s.emit("if (c->fpscr & ::sh4::FPSCR_SZ) {")
            s.gen_fpu_k(a, ins, pr, 1, B, 2)
            s.emit("} else {")
            s.gen_fpu_k(a, ins, pr, 0, B, 2)
            s.emit("}")
            s.dyn_mode += 1
            return
        s.gen_fpu_k(a, ins, pr, sz, B, 1)

    def gen_fpu_k(s, a, ins, pr, sz, B, ind):
        op, n, m = ins.op, ins.n, ins.m
        E = lambda l: s.emit(l, ind)
        XB = ("(%s ^ 1)" % B) if not B.isdigit() else str(int(B) ^ 1)
        F = lambda r: "c->fr.f[%s][%d]" % (B, r)
        U = lambda r: "c->fr.u[%s][%d]" % (B, r)
        R = lambda r: "c->r[%d]" % r
        def U64(r):   # 64-bit register operand for SZ=1 moves: even -> DR (bank B), odd -> XD (other bank)
            return (B if r % 2 == 0 else XB), r & ~1
        if op in ("fadd", "fsub", "fmul", "fdiv"):
            o = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/"}[op]
            if pr == 0: E("%s = %s %s %s;" % (F(n), F(n), o, F(m)))
            else: E("::sh4::setdr(c, %s, %d, ::sh4::getdr(c, %s, %d) %s ::sh4::getdr(c, %s, %d));" % (B, n & ~1, B, n & ~1, o, B, m & ~1))
            return
        if op in ("fcmpeq", "fcmpgt"):
            o = "==" if op == "fcmpeq" else ">"
            if pr == 0: E("c->t = %s %s %s;" % (F(n), o, F(m)))
            else: E("c->t = ::sh4::getdr(c, %s, %d) %s ::sh4::getdr(c, %s, %d);" % (B, n & ~1, o, B, m & ~1))
            return
        if op == "fmac":
            if pr == 0: E("%s = ::sh4::fmac(%s, %s, %s);" % (F(n), F(0), F(m), F(n)))
            else: E("::sh4::fatal_jump(c, %s, 0xFFFFFFFFu); /* fmac with PR=1 is undefined */" % h(a))
            return
        if op == "fmov":
            if sz == 0: E("%s = %s;" % (U(n), U(m)))
            else:
                bn, rn = U64(n); bm, rm = U64(m)
                E("c->fr.u[%s][%d] = c->fr.u[%s][%d]; c->fr.u[%s][%d] = c->fr.u[%s][%d];" % (bn, rn, bm, rm, bn, rn + 1, bm, rm + 1))
            return
        def ld64(addr, r):
            b, rr = U64(r)
            return "{ uint32_t a_ = %s; c->fr.u[%s][%d] = ::sh4::rd32(a_); c->fr.u[%s][%d] = ::sh4::rd32(a_ + 4); }" % (addr, b, rr, b, rr + 1)
        def st64(addr, r):
            b, rr = U64(r)
            return "{ uint32_t a_ = %s; ::sh4::wr32(a_, c->fr.u[%s][%d]); ::sh4::wr32(a_ + 4, c->fr.u[%s][%d]); }" % (addr, b, rr, b, rr + 1)
        if op == "fmov_l":
            E("%s = ::sh4::rd32(%s);" % (U(n), R(m)) if sz == 0 else ld64(R(m), n)); return
        if op == "fmov_l0":
            E("%s = ::sh4::rd32(%s + c->r[0]);" % (U(n), R(m)) if sz == 0 else ld64("%s + c->r[0]" % R(m), n)); return
        if op == "fmov_p":
            if sz == 0: E("%s = ::sh4::rd32(%s); %s += 4;" % (U(n), R(m), R(m)))
            else: E(ld64(R(m), n) + " %s += 8;" % R(m))
            return
        if op == "fmov_s":
            E("::sh4::wr32(%s, %s);" % (R(n), U(m)) if sz == 0 else st64(R(n), m)); return
        if op == "fmov_s0":
            E("::sh4::wr32(%s + c->r[0], %s);" % (R(n), U(m)) if sz == 0 else st64("%s + c->r[0]" % R(n), m)); return
        if op == "fmov_m":
            if sz == 0: E("%s -= 4; ::sh4::wr32(%s, %s);" % (R(n), R(n), U(m)))
            else: E("%s -= 8; " % R(n) + st64(R(n), m))
            return
        if op == "fsts": E("%s = c->fpul;" % U(n)); return
        if op == "flds": E("c->fpul = %s;" % U(m)); return
        if op == "float":
            if pr == 0: E("%s = (float)(int32_t)c->fpul;" % F(n))
            else: E("::sh4::setdr(c, %s, %d, (double)(int32_t)c->fpul);" % (B, n & ~1))
            return
        if op == "ftrc":
            if pr == 0: E("c->fpul = ::sh4::ftrc_s(%s);" % F(m))
            else: E("c->fpul = ::sh4::ftrc_d(::sh4::getdr(c, %s, %d));" % (B, m & ~1))
            return
        if op == "fneg":
            E("%s ^= 0x80000000u;" % U(n if pr == 0 else n & ~1)); return
        if op == "fabs":
            E("%s &= 0x7FFFFFFFu;" % U(n if pr == 0 else n & ~1)); return
        if op == "fsqrt":
            if pr == 0: E("%s = std::sqrt(%s);" % (F(n), F(n)))
            else: E("::sh4::setdr(c, %s, %d, std::sqrt(::sh4::getdr(c, %s, %d)));" % (B, n & ~1, B, n & ~1))
            return
        if op == "fsrra":
            if pr == 0: E("%s = ::sh4::fsrra(%s);" % (F(n), F(n)))
            return
        if op in ("fldi0", "fldi1"):
            if pr == 0: E("%s = %s;" % (U(n), "0x00000000u" if op == "fldi0" else "0x3F800000u"))
            return
        if op == "fcnvsd":
            if pr == 1: E("{ float f_; std::memcpy(&f_, &c->fpul, 4); ::sh4::setdr(c, %s, %d, (double)f_); }" % (B, n & ~1))
            return
        if op == "fcnvds":
            if pr == 1: E("{ float f_ = (float)::sh4::getdr(c, %s, %d); std::memcpy(&c->fpul, &f_, 4); }" % (B, m & ~1))
            return
        if op == "fipr":
            if pr == 0: E("::sh4::fipr(c, %s, %d, %d);" % (B, n * 4, m * 4))
            return
        if op == "ftrv":
            if pr == 0: E("::sh4::ftrv(c, %s, %d);" % (B, n * 4))
            return
        if op == "fsca":
            if pr == 0: E("::sh4::fsca(c, %s, %d);" % (B, n * 2))
            return
        s.untranslated[op] += 1
        E("#error untranslated %s at %08x" % (op, a))


def targets_branch(f, a):
    """Addresses that are explicit branch targets (so a delay slot that is also a target is emitted)."""
    if not hasattr(f, "_btargets"):
        bt = set([f.entry])
        for x, ins in f.insns.items():
            if ins.op in ("bt", "bf", "bts", "bfs", "bra"):
                bt.add(S.branch_target(ins, x))
            if x in f.switch:
                bt.update(f.switch[x])
        f._btargets = bt
    return f._btargets


def main():
    outdir = sys.argv[1]
    per_tu = 120
    if "--funcs-per-tu" in sys.argv:
        per_tu = int(sys.argv[sys.argv.index("--funcs-per-tu") + 1])
    os.makedirs(outdir, exist_ok=True)
    d, rounds = D.main()
    funcs = d.funcs
    consts = {}
    for e, f in funcs.items():
        consts[e] = d.constprop(f)
    # interprocedural entry-mode propagation
    entry_modes = {e: ENTRY_MODE for e in funcs}
    for it in range(6):
        callsite = collections.defaultdict(list)
        for e, f in funcs.items():
            md = FuncGen.fp_modes(f, entry_modes[e], consts[e])
            for site, t in list(f.calls.items()) + list(f.tails.items()):
                if t in funcs and site in md:
                    callsite[t].append(md[site])
        changed = 0
        for t, ms in callsite.items():
            new = ENTRY_MODE
            # functions reached through pointers keep the ABI default as one of their "callers"
            ptr = bool(d.prov[t] & {"dataptr", "litptr", "vbr", "entry"})
            vals = ms + ([ENTRY_MODE] if ptr else [])
            new = tuple(vals[0][i] if all(v[i] == vals[0][i] for v in vals) else None for i in range(3))
            if new != entry_modes[t]:
                entry_modes[t] = new; changed += 1
        if not changed: break
    # generate
    order = sorted(funcs)
    stats = collections.Counter()
    untrans = collections.Counter()
    protos = []
    tu = []
    dyn_total = 0
    ext_calls = set()
    for i in range(0, len(order), per_tu):
        chunk = order[i:i + per_tu]
        lines = ['#include "sh4ctx.h"', '#include "db_funcs.h"', ""]
        for e in chunk:
            g = FuncGen(d, funcs[e], entry_modes, consts[e])
            g.tailcalls = set(); g.dyn_mode = 0; g.ext_calls = ext_calls
            lines += g.gen(); lines.append("")
            untrans.update(g.untranslated)
            dyn_total += g.dyn_mode
            stats["insns"] += len(funcs[e].insns)
        path = os.path.join(outdir, "db_funcs_%03d.cpp" % (i // per_tu))
        open(path, "w").write("\n".join(lines) + "\n")
        tu.append(path)
    with open(os.path.join(outdir, "db_funcs.h"), "w") as hf:
        hf.write("#pragma once\n#include \"sh4ctx.h\"\n")
        for e in order:
            hf.write("void %s(sh4::Ctx* c);\n" % fname(e))
    with open(os.path.join(outdir, "db_dispatch.cpp"), "w") as df:
        df.write('#include "db_funcs.h"\n#include <algorithm>\nnamespace sh4 {\n')
        df.write("struct DispEntry { uint32_t addr; void (*fn)(Ctx*); };\n")
        df.write("extern const DispEntry g_dispatch[] = {\n")
        for e in order:
            df.write("  {0x%08xu, %s},\n" % (e, fname(e)))
        df.write("};\nextern const unsigned g_dispatch_count = %d;\n" % len(order))
        df.write("void (*lookup(uint32_t a))(Ctx*) {\n"
                 "  if ((a & 0x1C000000u) == 0x0C000000u) a = (a & 0x1FFFFFFFu) | 0x80000000u;\n"
                 "  const DispEntry* e = std::lower_bound(g_dispatch, g_dispatch + g_dispatch_count, a,\n"
                 "      [](const DispEntry& x, uint32_t v) { return x.addr < v; });\n"
                 "  return (e != g_dispatch + g_dispatch_count && e->addr == a) ? e->fn : nullptr;\n}\n}\n")
    mode_hist = collections.Counter(entry_modes[e] for e in order)
    rep = ["functions %d" % len(order), "translation units %d" % len(tu),
           "instructions translated (with duplication) %d" % stats["insns"],
           "untranslated %s" % dict(untrans),
           "fpu instructions needing runtime mode test %d" % dyn_total,
           "known call targets outside translated code (via dispatch) %s" % sorted("%08x" % x for x in ext_calls),
           "entry FPU modes (PR,SZ,FR) %s" % dict(mode_hist)]
    open(os.path.join(outdir, "translate_report.txt"), "w").write("\n".join(rep) + "\n")
    print("\n".join(rep))


if __name__ == "__main__":
    main()
