"""Function discovery for Dolphin Blue 1ST_READ.BIN (SH-4, load 0x8C010000).

Sources of function entries (each recorded with its provenance):
  entry      : image entry point
  call       : bsr / bsrf / jsr with a statically known target (constant propagation)
  tail       : jmp / bra to an address that is otherwise a known function entry
  vbr        : exception/interrupt handlers from ldc Rm,VBR with known value (+0x100/+0x400/+0x600)
  dataptr    : 32-bit words in non-code data pointing at an address that decodes as a valid function
               (validated: closed control flow, no invalid/slot-illegal instruction, no overlap with literals)

Intra-function indirect jumps: switch tables (mova + mov.w/mov.l @(R0,Rx) + braf/jmp) with bounds
from a preceding cmp/hs|cmp/hi|cmp/gt|cmp/ge against a constant.

Outputs (JSON + text) in ../build/: functions.json, funcmap.txt, unresolved.txt
"""
import os
EXTRACT = os.environ.get('DB_EXTRACT', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'extract'))   # your own disc's files
import sys, json, collections
import sh4dec as S

M32 = 0xffffffff
CALLER_SAVED = range(0, 8)      # SHC/Katana ABI: r0-r7 scratch, r8-r15 preserved


class Func:
    def __init__(s, entry):
        s.entry = entry
        s.insns = {}            # addr -> Insn
        s.succ = {}             # addr -> list of successor addrs (intra-function)
        s.calls = {}            # addr -> target or None
        s.tails = {}            # addr -> target or None (jmp/bra leaving the function)
        s.switch = {}           # addr -> list of targets
        s.switch_how = {}       # addr -> "bounded" | "heuristic"
        s.literals = set()      # addresses of literal data read via PC-relative loads (start addr, size)
        s.errors = []
        s.indirect_unresolved = []  # (addr, kind)


class Discover:
    def __init__(s, img, code_hi=None):
        s.img = img
        s.funcs = {}
        s.prov = collections.defaultdict(set)
        s.literal = {}           # addr -> size
        s.code_hi = code_hi or img.end
        s.vbr = set()

    # ------------------------------------------------------------------
    def dec(s, a):
        if not s.img.has(a, 2) or a & 1:
            return None
        return S.decode(s.img.u16(a))

    def walk(s, entry, known_entries):
        """Explore one function body. Branches to other known entries are tail calls."""
        f = Func(entry)
        work = [entry]
        seen = set()
        while work:
            a = work.pop()
            if a in seen:
                continue
            seen.add(a)
            while True:
                ins = s.dec(a)
                if ins is None:
                    f.errors.append((a, "invalid or out of image"))
                    break
                f.insns[a] = ins
                op = ins.op
                lit = S.pcrel_addr(ins, a)
                if lit is not None:
                    f.literals.add((lit, 4 if op in ("movl_pc", "mova") else 2))
                if op in S.DELAY:
                    slot = s.dec(a + 2)
                    if slot is None or slot.op in S.DELAY or slot.op in ("bt", "bf", "trapa"):
                        f.errors.append((a + 2, "bad delay slot"))
                        break
                    f.insns[a + 2] = slot
                    sl = S.pcrel_addr(slot, a + 2)
                    if sl is not None:
                        f.literals.add((sl, 4 if slot.op in ("movl_pc", "mova") else 2))
                    if op in ("rts", "rte"):
                        f.succ[a] = []
                    elif op in ("bra",):
                        t = S.branch_target(ins, a)
                        if t != entry and t in known_entries:
                            f.tails[a] = t; f.succ[a] = []
                        else:
                            f.succ[a] = [t]; work.append(t)
                    elif op in ("bts", "bfs"):
                        t = S.branch_target(ins, a)
                        f.succ[a] = [t, a + 4]; work += [t, a + 4]
                    elif op in ("bsr", "bsrf", "jsr"):
                        f.calls[a] = S.branch_target(ins, a) if op == "bsr" else None
                        f.succ[a] = [a + 4]; work.append(a + 4)
                    elif op in ("jmp", "braf"):
                        f.succ[a] = []       # resolved later (switch or tail)
                        f.tails[a] = None
                    break
                if op in ("bt", "bf"):
                    t = S.branch_target(ins, a)
                    f.succ[a] = [t, a + 2]; work += [t, a + 2]
                    break
                if op in ("sleep",):
                    pass
                if op == "trapa":
                    pass
                f.succ[a] = [a + 2]
                a += 2
                if a in seen:
                    break
                seen.add(a)
        return f

    # ------------------------------------------------------------------
    def constprop(s, f):
        """Forward const propagation over the function CFG (per instruction state).
        Returns dict addr -> state (dict reg->value) at instruction entry.
        Values: int (const) or tuple ('tbl', base, size, signed, add) for switch-table loads."""
        img = s.img
        preds = collections.defaultdict(list)
        for a, ss in f.succ.items():
            for b in ss:
                preds[b].append(a)
        # straight-line successors for delay-slot pairs are handled in transfer of the branch insn
        state_in = {f.entry: {}}
        work = [f.entry]
        TOP = object()

        def meet(x, y):
            out = {}
            for k in x:
                if k in y and x[k] == y[k]:
                    out[k] = x[k]
            return out

        def rd(st, r):
            return st.get(r)

        def xfer(st, ins, a):
            st = dict(st)
            op, n, m = ins.op, ins.n, ins.m
            def setr(r, v):
                if v is None: st.pop(r, None)
                else: st[r] = v & M32 if isinstance(v, int) else v
            if op == "movi":
                setr(n, S.s8(ins.imm))
            elif op == "movl_pc":
                la = S.pcrel_addr(ins, a)
                setr(n, img.u32(la) if img.has(la, 4) else None)
            elif op == "movw_pc":
                la = S.pcrel_addr(ins, a)
                setr(n, img.s16(la) if img.has(la, 2) else None)
            elif op == "mova":
                setr(0, S.pcrel_addr(ins, a))
            elif op == "mov":
                setr(n, rd(st, m))
            elif op == "addi":
                v = rd(st, n)
                if isinstance(v, int): setr(n, v + S.s8(ins.imm))
                elif isinstance(v, tuple): setr(n, v[:4] + (v[4] + S.s8(ins.imm),))
                else: setr(n, None)
            elif op == "add":
                x, y = rd(st, n), rd(st, m)
                if isinstance(x, int) and isinstance(y, int): setr(n, x + y)
                elif isinstance(x, tuple) and isinstance(y, int): setr(n, x[:4] + (x[4] + y,))
                elif isinstance(y, tuple) and isinstance(x, int): setr(n, y[:4] + (y[4] + x,))
                else: setr(n, None)
            elif op in ("shll", "shll2", "shll8", "shll16", "shal"):
                v = rd(st, n); sh = {"shll": 1, "shal": 1, "shll2": 2, "shll8": 8, "shll16": 16}[op]
                setr(n, (v << sh) if isinstance(v, int) else None)
            elif op in ("shlr", "shlr2", "shlr8", "shlr16"):
                v = rd(st, n); sh = {"shlr": 1, "shlr2": 2, "shlr8": 8, "shlr16": 16}[op]
                setr(n, (v >> sh) if isinstance(v, int) else None)
            elif op in ("or", "and", "xor"):
                x, y = rd(st, n), rd(st, m)
                if isinstance(x, int) and isinstance(y, int):
                    setr(n, x | y if op == "or" else x & y if op == "and" else x ^ y)
                else: setr(n, None)
            elif op in ("ori", "andi", "xori"):
                x = rd(st, 0)
                if isinstance(x, int):
                    setr(0, x | ins.imm if op == "ori" else x & ins.imm if op == "andi" else x ^ ins.imm)
                else: setr(0, None)
            elif op in ("movw_l0", "movl_l0", "movb_l0"):
                base = rd(st, 0)
                sz = {"movb_l0": 1, "movw_l0": 2, "movl_l0": 4}[op]
                if isinstance(base, int) and not isinstance(rd(st, m), int):
                    setr(n, ("tbl", base, sz, True, 0))
                else:
                    setr(n, None)
            elif op in ("extuw", "extub", "extsw", "extsb"):
                v = rd(st, m)
                if isinstance(v, int):
                    v &= M32
                    if op == "extuw": v &= 0xffff
                    elif op == "extub": v &= 0xff
                    elif op == "extsw": v = (v & 0xffff) - (0x10000 if v & 0x8000 else 0)
                    else: v = (v & 0xff) - (0x100 if v & 0x80 else 0)
                    setr(n, v)
                elif isinstance(v, tuple) and op in ("extuw", "extub"):
                    setr(n, v[:3] + (False, v[4]))
                else: setr(n, None)
            else:
                # generic: kill destination registers we know this op writes
                for r in writes(ins):
                    st.pop(r, None)
            return st

        out = {}
        it = 0
        while work:
            it += 1
            if it > 200000: break
            a = work.pop()
            st = state_in[a]
            ins = f.insns.get(a)
            if ins is None:
                continue
            out[a] = st
            op = ins.op
            if op in S.DELAY:
                # target register value is read before the slot executes
                slot = f.insns[a + 2]
                out[a + 2] = st
                st2 = xfer(st, slot, a + 2)
                if op in ("bsr", "bsrf", "jsr"):
                    for r in CALLER_SAVED: st2.pop(r, None)
                nxt = f.succ.get(a, [])
            else:
                st2 = xfer(st, ins, a)
                nxt = f.succ.get(a, [])
            for b in nxt:
                if b not in f.insns: continue
                if b in state_in:
                    nm = meet(state_in[b], st2)
                    if nm != state_in[b] or b not in out:
                        state_in[b] = nm; work.append(b)
                else:
                    state_in[b] = st2; work.append(b)
        return out

    # ------------------------------------------------------------------
    def resolve(s, f, known_entries):
        """Resolve call/jump targets via const propagation; detect switch tables.
        Returns list of new code addresses that must be added to this function (switch cases)."""
        st = s.constprop(f)
        new_intra = []
        for a, ins in list(f.insns.items()):
            op = ins.op
            if a not in st:
                continue
            if op in ("jsr", "jmp", "braf", "bsrf"):
                v = st[a].get(ins.m if op in ("jsr", "jmp") else ins.m)
                if op in ("braf", "bsrf") and isinstance(v, int):
                    v = (a + 4 + v) & M32
                v = s.norm(v)
                if isinstance(v, int):
                    if op in ("jsr", "bsrf"): f.calls[a] = v
                    else: f.tails[a] = v
                elif isinstance(v, tuple) and op in ("braf", "jmp"):
                    tg, how = s.switch_targets(f, a, st, v, op)
                    if tg:
                        f.switch[a] = tg; f.switch_how[a] = how
                        f.tails.pop(a, None)
                        f.succ[a] = list(dict.fromkeys(tg))
                        new_intra += tg
                    else:
                        f.indirect_unresolved.append((a, op + " table-unbounded"))
                else:
                    if op in ("jsr", "bsrf"):
                        f.indirect_unresolved.append((a, op))
                    else:
                        f.indirect_unresolved.append((a, op))
            if op == "ldc_vbr":
                v = st[a].get(ins.m)
                if isinstance(v, int):
                    s.vbr.add(v)
        return new_intra

    def switch_targets(s, f, a, st, v, op):
        _, base, size, signed, add = v
        # find bound: walk back up to 24 insns along fallthrough/predecessor chain for cmp vs const
        bound = None
        preds = collections.defaultdict(list)
        for x, ss in f.succ.items():
            for y in ss: preds[y].append(x)
        cur, steps = a, 0
        while steps < 24 and bound is None:
            ps = preds.get(cur, [])
            if not ps: break
            cur = ps[0]; steps += 1
            ins = f.insns.get(cur)
            if ins is None: break
            if ins.op in ("cmphs", "cmphi", "cmpgt", "cmpge"):
                c = st.get(cur, {}).get(ins.m)
                if isinstance(c, int):
                    c = c & M32
                    if c >= 0x80000000: c -= 1 << 32
                    bound = c if ins.op in ("cmphs", "cmpge") else c + 1
            elif ins.op == "cmpim" and False:
                pass
        targets = []
        how = "bounded"
        n = bound if bound is not None and 0 < bound <= 1024 else None
        if n is None:
            how = "heuristic"; n = 256
        for i in range(n):
            ea = base + i * size
            if not s.img.has(ea, size): break
            e = {1: lambda: s.img.data[ea - s.img.base], 2: lambda: s.img.u16(ea), 4: lambda: s.img.u32(ea)}[size]()
            if signed and size < 4 and e & (1 << (size * 8 - 1)): e -= 1 << (size * 8)
            t = ((a + 4) if op == "braf" else 0) + e + add
            t &= M32
            if how == "heuristic" and (s.dec(t) is None or not (s.img.base <= t < s.code_hi) or t & 1):
                break
            targets.append(t)
            s.literal[ea] = size
        if how == "heuristic" and not targets:
            return None, None
        return targets, how

    # ------------------------------------------------------------------
    def analyze_func(s, entry, known):
        f = s.walk(entry, known)
        for _ in range(8):
            new = s.resolve(f, known)
            new = [t for t in new if t not in f.insns]
            if not new:
                break
            # extend function with switch-case code
            g = s.walk_more(f, new, known)
        # tail jmp to known entries
        return f

    def walk_more(s, f, starts, known):
        tmp = s.walk(starts[0], known)  # cheap: walk each start, merge
        for st in starts:
            g = s.walk(st, known | {f.entry})
            for k, v in g.insns.items(): f.insns.setdefault(k, v)
            for k, v in g.succ.items(): f.succ.setdefault(k, v)
            for k, v in g.calls.items(): f.calls.setdefault(k, v)
            for k, v in g.tails.items(): f.tails.setdefault(k, v)
            f.literals |= g.literals
            f.errors += g.errors

    # ------------------------------------------------------------------
    def run(s, entry):
        s.prov[entry].add("entry")
        return s.run_from(entry, {entry})

    def run_from(s, entry, known):
        known.add(entry)
        queue = [entry]
        while queue:
            e = queue.pop()
            if e in s.funcs:
                continue
            f = s.analyze_func(e, known)
            s.funcs[e] = f
            for lit, sz in f.literals:
                s.literal[lit] = sz
            for kind, d in (("call", f.calls), ("tail", f.tails)):
                for site, t in d.items():
                    if t is None: continue
                    if kind == "tail" and t in f.insns and t != e:
                        continue  # intra-function jump
                    if not s.valid_code_addr(t):
                        continue
                    s.prov[t].add(kind)
                    if t not in known:
                        known.add(t); queue.append(t)
            for v in list(s.vbr):
                for off in (0x100, 0x400, 0x600):
                    t = v + off
                    if s.valid_code_addr(t) and t not in known:
                        s.prov[t].add("vbr"); known.add(t); queue.append(t)
        return known

    def valid_code_addr(s, t):
        return s.img.base <= t < s.code_hi and not (t & 1)

    @staticmethod
    def norm(v):
        """Fold P0/P1/P2/P3 mirrors of main RAM onto the P1 (0x8Cxxxxxx) view."""
        if isinstance(v, int) and (v & 0x1C000000) == 0x0C000000 and (v >> 29) in (0, 4, 5, 6):
            return (v & 0x1FFFFFFF) | 0x80000000
        return v

    def code_bytes(s):
        cov = set()
        for f in s.funcs.values():
            cov.update(f.insns.keys())
        return cov

    def scan_dataptrs(s):
        """Scan aligned words outside code/known-literal for pointers to plausible new functions."""
        cov = s.code_bytes()
        lit = set()
        for a, sz in s.literal.items():
            for i in range(0, sz, 2): lit.add(a + i)
        cands = collections.Counter()
        s.ref_sites = collections.defaultdict(list)
        for off in range(0, len(s.img.data) - 3, 4):
            a = s.img.base + off
            if a in cov or (a + 2) in cov:
                continue
            v = s.img.u32(a)
            if not s.valid_code_addr(v):
                continue
            cands[v] += 1
            s.ref_sites[v].append(a)
        return cands

    def validate_candidate(s, t, known):
        """A pointer target (literal-pool or data word) is accepted as a function entry only if:
          * its body decodes cleanly with closed control flow (no invalid word, legal delay slots),
          * no instruction of the body overlaps a known literal,
          * the body has no supervisor-only/unusual ops (ldtlb, sleep, rte, clrs, sets, bank ops),
          * and the entry sits in code context: right after a return/branch+slot, a literal pool,
            nop/zero padding, on an instruction boundary of already-proven code, or within
            512 bytes of code proven by the call graph."""
        if t in known:
            return True, "known"
        f = s.walk(t, known)
        if f.errors:
            return False, "decode error"
        for a, ins in f.insns.items():
            if a in s._litset:
                return False, "overlaps literal"
            if ins.op in BAD_IN_PTR_FUNC:
                return False, "suspicious op " + ins.op
        img = s.img
        w2 = img.u16(t - 4) if img.has(t - 4, 2) else None
        w1 = img.u16(t - 2) if img.has(t - 2, 2) else None
        d2 = S.decode(w2) if w2 is not None else None
        if d2 is not None and d2.op in ("bra", "jmp", "braf", "rte", "rts"):
            return True, "ok"
        if w1 in (0x0009, 0x0000) or (t - 2) in s._litset or t in s._cov or (t - 2) in s._cov:
            return True, "ok"
        # literal-pool context: the aligned word before the entry is a pointer into the image
        if t % 4 == 0 and img.has(t - 4, 4) and img.base <= img.u32(t - 4) < img.end:
            return True, "ok"
        for x in range(t - 512, t + 512, 2):
            if x in s._strongcov:
                return True, "ok"
        # table context: the pointer sits in a data table next to pointers to already-proven functions
        for site in getattr(s, "ref_sites", {}).get(t, []):
            for dlt in (-8, -4, 4, 8):
                w = site + dlt
                if img.has(w, 4) and img.u32(w) in known and img.u32(w) != t:
                    return True, "ok"
        return False, "no code context"


MANUAL_ENTRIES = [0x8C32980C, 0x8C329944, 0x8C32964C]
# Data-referenced entries rejected as "no code context" whose code starts with a real function prologue
# (push of r8-r14 / sts.l pr). 0x8C086638 is a scene handler the attract loop installs after ~90 s without
# input (found as an unknown dispatch target at runtime); the others share the same rejection reason.
PROLOGUE_ENTRIES = [0x8C026266, 0x8C026392, 0x8C02653E, 0x8C02667E, 0x8C03AEFE, 0x8C048D96, 0x8C086638,
                    0x8C027240]   # 8C027240: leaf without a prologue, only referenced from data tables (stage-2 crash)

BAD_IN_PTR_FUNC = {"ldtlb", "sleep", "rte", "clrs", "sets", "ldc_bank", "ldcl_bank", "stc_bank",
                   "stcl_bank", "ldc_dbr", "ldcl_dbr", "stc_dbr", "stcl_dbr", "stc_sgr", "stcl_sgr",
                   "ldc_ssr", "ldcl_ssr", "ldc_spc", "ldcl_spc"}


def writes(ins):
    """Registers (0-15) written by an instruction (for const propagation kill)."""
    op, n, m = ins.op, ins.n, ins.m
    if op in ("movb_p", "movw_p", "movl_p"):
        return [n, m]
    if op in ("movb_m", "movw_m", "movl_m", "fmov_m", "stsl_mach", "stsl_macl", "stsl_pr", "stsl_fpul",
              "stsl_fpscr", "stcl_sr", "stcl_gbr", "stcl_vbr", "stcl_ssr", "stcl_spc", "stcl_sgr",
              "stcl_dbr", "stcl_bank"):
        return [n]
    if op in ("ldsl_mach", "ldsl_macl", "ldsl_pr", "ldsl_fpul", "ldsl_fpscr", "ldcl_sr", "ldcl_gbr",
              "ldcl_vbr", "ldcl_ssr", "ldcl_spc", "ldcl_dbr", "ldcl_bank", "fmov_p"):
        return [m]
    if op in ("macl", "macw"):
        return [n, m]
    if op in ("movb_l4", "movw_l4", "movb_lg", "movw_lg", "movl_lg", "mova", "cmpim", "tsti",
              "andi", "ori", "xori"):
        return [0]
    if op in ("stc_sr", "stc_gbr", "stc_vbr", "stc_ssr", "stc_spc", "stc_sgr", "stc_dbr", "stc_bank",
              "sts_mach", "sts_macl", "sts_pr", "sts_fpul", "sts_fpscr", "movt", "dt", "shll", "shlr",
              "shal", "shar", "shll2", "shlr2", "shll8", "shlr8", "shll16", "shlr16", "rotl", "rotr",
              "rotcl", "rotcr", "movb_l", "movw_l", "movl_l", "movl_l4", "movb_l0", "movw_l0", "movl_l0",
              "mov", "not", "swapb", "swapw", "negc", "neg", "extub", "extuw", "extsb", "extsw",
              "addi", "add", "addc", "addv", "sub", "subc", "subv", "and", "or", "xor", "xtrct",
              "div1", "shad", "shld", "movi", "movl_pc", "movw_pc"):
        return [n]
    if op in ("ldc_bank",):
        return []
    return []


def main():
    img = S.Image(EXTRACT + "/1ST_READ.BIN")
    d = Discover(img)
    d.run(img.base)
    # Exception/interrupt entry points proven by analysing the VBR install routine f_8c30b9ec:
    # it patches 0x8C32977C/0x8C3297D8 (literals of the VBR+0x100/+0x600 stubs) with one of
    # these two dispatchers, and fills the INTEVT handler table with 0x8C32964C as default.
    for e in MANUAL_ENTRIES:
        d.prov[e].add("manual-vbr")
        d.run_from(e, set(d.funcs) | {e})
    for e in PROLOGUE_ENTRIES:
        d.prov[e].add("dataptr-prologue")
        d.run_from(e, set(d.funcs) | {e})
    d._strongcov = d.code_bytes()
    rounds = []
    for rnd in range(60):
        d._cov = d.code_bytes()
        d._litset = set()
        for a, sz in d.literal.items():
            for i in range(0, sz, 2): d._litset.add(a + i)
        known = set(d.funcs)
        # literal-pool code pointers (value of a PC-relative mov.l)
        litc = collections.Counter()
        for f in d.funcs.values():
            for lit, sz in f.literals:
                if sz == 4 and img.has(lit, 4):
                    v = img.u32(lit)
                    if d.valid_code_addr(v): litc[v] += 1
        datc = d.scan_dataptrs()
        added = []
        rej = collections.Counter()
        for src_kind, cands in (("litptr", litc), ("dataptr", datc)):
            for t in sorted(cands):
                ok, why = d.validate_candidate(t, known)
                if ok:
                    d.prov[t].add(src_kind)
                    if why == "ok":
                        known.add(t); added.append(t)
                else:
                    rej[src_kind + ":" + why] += 1
        rounds.append((rnd, len(litc), len(datc), len(added), dict(rej)))
        if not added:
            break
        for t in added:
            d.run_from(t, known)
    final = set(d.funcs)
    for e in sorted(final):
        d.funcs[e] = d.analyze_func(e, final)
    return d, rounds


if __name__ == "__main__":
    d, rounds = main()
    out = {}
    for e, f in sorted(d.funcs.items()):
        hi = max(f.insns) + 2 if f.insns else e
        out["%08x" % e] = {
            "size_span": hi - e, "ninsns": len(f.insns), "prov": sorted(d.prov[e]),
            "calls": {"%08x" % k: (None if v is None else "%08x" % v) for k, v in f.calls.items()},
            "tails": {"%08x" % k: (None if v is None else "%08x" % v) for k, v in f.tails.items()},
            "switch": {"%08x" % k: ["%08x" % t for t in v] for k, v in f.switch.items()},
            "switch_how": {"%08x" % k: v for k, v in f.switch_how.items()},
            "unresolved": [("%08x" % a, k) for a, k in f.indirect_unresolved],
            "errors": [("%08x" % a, k) for a, k in f.errors],
        }
    json.dump(out, open("../build/functions.json", "w"), indent=0)
    print("rounds", rounds)
    print("functions", len(out))
