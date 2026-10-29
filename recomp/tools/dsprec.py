#!/usr/bin/env python3
"""AOT translator for the AICA DSP programs the game loads (dumped with DB_DSPPROG=dir on the desktop).

Each program becomes one straight-line C++ function with every decoded field resolved at generation time,
mirroring aica_dsp_step_impl() in host/aica_dsp.cpp step for step (empty-step shortcut, dead-step skipping,
2-step memory read pipeline, ring-buffer counter). COEF and MADRS stay live register reads (the driver can
change them without touching MPRO). The generated file is included by aica_dsp.cpp; programs are selected by
the FNV-1a hash of MPRO, and anything else runs in the interpreter.

usage: dsprec.py DUMP_DIR OUT.inc
"""
import glob, os, struct, sys

def decode(w):
    steps = []
    for st in range(128):
        i0, i1, i2, i3 = w[st * 4:st * 4 + 4]
        steps.append(dict(
            empty=not (i0 | i1 | i2 | i3),
            tra=(i0 >> 9) & 0x7F, twa=(i0 >> 1) & 0x7F, twt=bool(i0 & 0x100),
            xsel=bool(i1 & 0x8000), ysel=(i1 >> 13) & 3, ira=(i1 >> 7) & 0x3F, iwa=(i1 >> 1) & 0x1F, iwt=bool(i1 & 0x40),
            table=bool(i2 & 0x8000), mwt=bool(i2 & 0x4000), mrd=bool(i2 & 0x2000), ewt=bool(i2 & 0x1000), ewa=(i2 >> 8) & 15,
            adrl=bool(i2 & 0x80), frcl=bool(i2 & 0x40), shift=(i2 >> 4) & 3, yrl=bool(i2 & 8), negb=bool(i2 & 4),
            zero=bool(i2 & 2), bsel=bool(i2 & 1),
            masa=(i3 >> 9) & 0x3F, adreb=bool(i3 & 0x100), nxadr=bool(i3 & 0x80)))
    return steps

def gen(name, steps):
    out = []
    has_output = any(p["twt"] or p["ewt"] or p["mwt"] or p["iwt"] for p in steps if not p["empty"])
    end = 0
    for i, p in enumerate(steps):
        if not p["empty"]:
            end = i + 1
    skip = [steps[i]["empty"] and i + 1 < 128 and steps[i + 1]["empty"] for i in range(128)]
    out.append("static void %s() {" % name)
    if not has_output:
        out.append("    std::memset(d.mixs, 0, sizeof d.mixs);")
        out.append("}")
        return out
    out.append("    uint16_t rbreg = REG(0x2804);")
    out.append("    uint32_t rbl = (8192u << ((rbreg >> 13) & 3)) - 1;")
    out.append("    uint32_t rbp = (uint32_t)(rbreg & 0xFFF) * 2048;")
    out.append("    uint8_t* ram = aica_aram();")
    out.append("    const uint32_t mdec = d.mdec_ct;")
    out.append("    int64_t acc = 0; int32_t inputs = 0, frc = 0, yreg = 0, shifted = 0; uint32_t adrs = 0; (void)adrs; (void)yreg;")
    any_step = False
    for st in range(end):
        if skip[st]:
            continue
        p = steps[st]
        out.append("    // step %d" % st)
        if p["empty"]:
            out.append("    { int32_t x = d.temp[mdec & 0x7F]; acc = (((int64_t)x * frc) >> 12) + x; }")
            continue
        any_step = True
        ira = p["ira"]
        if ira <= 0x1F:
            out.append("    inputs = sx24(d.mems[%d]);" % ira)
        elif ira <= 0x2F:
            out.append("    inputs = sx24(d.mixs[%d] << 4);" % (ira - 0x20))
        else:
            out.append("    inputs = 0;")
        if p["iwt"]:
            out.append("    d.mems[%d] = d.memval[%d];" % (p["iwa"], st & 3))
        # ALU (same order as the interpreter: shifter on the previous ACC, then the new ACC)
        sh = p["shift"]
        if sh in (0, 3):
            out.append("    { int32_t sh = (int32_t)acc;")
        else:
            out.append("    { int32_t sh = (int32_t)(acc << 1);")
        if sh < 2:
            out.append("      sh = sh < -0x800000 ? -0x800000 : sh > 0x7FFFFF ? 0x7FFFFF : sh;")
        else:
            out.append("      sh = sx24(sh);")
        need_t = (not p["zero"] and not p["bsel"]) or not p["xsel"]
        if need_t:
            out.append("      int32_t t = d.temp[(%du + mdec) & 0x7F];" % p["tra"])
        if p["zero"]:
            b = "0"
        elif p["bsel"]:
            b = "(int32_t)acc"
        else:
            b = "t"
        if not p["zero"] and p["negb"]:
            b = "-(%s)" % b
        x = "inputs" if p["xsel"] else "t"
        y = {0: "frc", 1: "((int16_t)REG(0x%x) >> 3)" % (0x3000 + st * 4), 2: "(yreg >> 11)", 3: "((yreg >> 4) & 0x0FFF)"}[p["ysel"]]
        out.append("      int32_t b = %s; int32_t x = %s; int32_t y = %s;" % (b, x, y))
        out.append("      shifted = sh;")
        out.append("      int64_t a = (((int64_t)x * y) >> 12) + b; acc = (a << 38) >> 38; }")
        if p["yrl"]:
            out.append("    yreg = inputs;")
        if p["twt"]:
            out.append("    d.temp[(%du + mdec) & 0x7F] = shifted;" % p["twa"])
        if p["frcl"]:
            out.append("    frc = %s;" % ("(shifted & 0x0FFF)" if sh == 3 else "(shifted >> 11)"))
        if (st & 1) and (p["mrd"] or p["mwt"]):
            out.append("    { uint32_t addr = REG(0x%x);" % (0x3200 + p["masa"] * 4))
            if p["adreb"]:
                out.append("      addr += adrs & 0x0FFF;")
            if p["nxadr"]:
                out.append("      addr++;")
            if not p["table"]:
                out.append("      addr += mdec; addr &= rbl;")
            else:
                out.append("      addr &= 0xFFFF;")
            out.append("      uint32_t ba = (rbp + (addr << 1)) & 0x1FFFFE;")
            if p["mrd"]:
                out.append("      { uint16_t w; std::memcpy(&w, ram + ba, 2); d.memval[%d] = unpack(w); }" % ((st + 2) & 3))
            if p["mwt"]:
                out.append("      { uint16_t w = pack(shifted); std::memcpy(ram + ba, &w, 2); }")
            out.append("    }")
        if p["adrl"]:
            out.append("    adrs = %s;" % ("(uint32_t)(shifted >> 12)" if sh == 3 else "(uint32_t)(inputs >> 16)"))
        if p["ewt"]:
            out.append("    d.efreg[%d] = shifted >> 8;" % p["ewa"])
    out.append("    if (--d.mdec_ct == 0) d.mdec_ct = rbl + 1;")
    out.append("    std::memset(d.mixs, 0, sizeof d.mixs);")
    if not any_step:
        out.append("    std::memset(d.efreg, 0, sizeof d.efreg);")
    out.append("}")
    return out

def main():
    src, dst = sys.argv[1], sys.argv[2]
    out = ["// Generated by tools/dsprec.py from the game's DSP programs - do not edit."]
    table = []
    for f in sorted(glob.glob(os.path.join(src, "prog_*.bin"))):
        w = struct.unpack("<512H", open(f, "rb").read())
        h = 1469598103934665603
        for x in w:
            h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        name = "dspprog_%016x" % h
        out += gen(name, decode(w))
        table.append((h, name))
    out.append("struct DspProg { uint64_t hash; void (*fn)(); };")
    out.append("static const DspProg kDspProgs[] = {")
    for h, n in table:
        out.append("    {0x%016xull, %s}," % (h, n))
    out.append("};")
    open(dst, "w").write("\n".join(out) + "\n")
    print("compiled %d DSP programs" % len(table))

if __name__ == "__main__":
    main()
