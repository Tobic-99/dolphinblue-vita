// AICA effect DSP (reverb/delay effects): runs the 128-step microprogram the driver loads (MPRO/COEF/MADRS)
// once per sample. Inputs are the per-channel sends (ISEL/IMXL -> MIXS); the 16 EFREG outputs are mixed back
// with their EFSDL/EFPAN levels. The work memory is a ring buffer in sound RAM (RBP/RBL).
// Own implementation from the AICA DSP documentation: field layout, 24-bit datapath, 13-bit coefficients,
// 16-bit float memory format, the two-step memory read pipeline and the ring-buffer address counter.
#include "hw.h"
#include <cstring>
#include <array>
#include <utility>
#include <cstdio>
#include <cstdlib>

namespace hw {
extern uint16_t* const aica_regs_ptr;
uint16_t* aica_regs();
bool aica_dsp_dirty = true;   // set by aica.cpp on writes to MPRO
uint8_t* aica_aram();

namespace {
struct Dsp {
    int32_t temp[128], mems[32], mixs[16], efreg[16];
    int32_t memval[4];
    uint32_t mdec_ct = 1;
} d;
const int32_t kZero = 0;
struct Step {   // decoded MPRO instruction
    uint8_t tra, twa, ira, iwa, ewa, masa, ysel, shift;
    uint8_t kind;                 // ALU shape: XSEL | YSEL<<1 | BMODE<<3 (0 zero, 1 ACC, 2 TEMP) | NEGB<<5 | SHIFT<<6
    uint8_t in_shift; const int32_t* in_ptr;   // INPUTS source (MEMS, MIXS << 4, or zero)
    bool side;                    // any TWT/IWT/YRL/FRCL/memory/ADRL/EWT work after the ALU
    bool empty, twt, xsel, iwt, table, mwt, mrd, ewt, adrl, frcl, yrl, negb, zero, bsel, adreb, nxadr;
};
Step prog[128];
bool prog_has_output;
int prog_end;                // 1 + last non-empty step: later (empty) steps only touch per-sample ACC
bool prog_skip[128];         // empty step followed by another empty step: its ACC is overwritten unread   // any TWT/EWT/MWT/IWT/FRCL/ADRL: otherwise the DSP has no effect
void select_compiled();
void decode() {
    prog_has_output = false;
    for (int step = 0; step < 128; ++step) {
        uint32_t base = 0x3400 + step * 16;
        uint16_t i0 = aica_regs_ptr[base / 4], i1 = aica_regs_ptr[(base + 4) / 4], i2 = aica_regs_ptr[(base + 8) / 4], i3 = aica_regs_ptr[(base + 12) / 4];
        Step& p = prog[step];
        p.empty = !(i0 | i1 | i2 | i3);
        p.tra = (i0 >> 9) & 0x7F; p.twa = (i0 >> 1) & 0x7F; p.twt = i0 & 0x100;
        p.xsel = i1 & 0x8000; p.ysel = (i1 >> 13) & 3; p.ira = (i1 >> 7) & 0x3F; p.iwa = (i1 >> 1) & 0x1F; p.iwt = i1 & 0x40;
        p.table = i2 & 0x8000; p.mwt = i2 & 0x4000; p.mrd = i2 & 0x2000; p.ewt = i2 & 0x1000; p.ewa = (i2 >> 8) & 15;
        p.adrl = i2 & 0x80; p.frcl = i2 & 0x40; p.shift = (i2 >> 4) & 3; p.yrl = i2 & 8; p.negb = i2 & 4; p.zero = i2 & 2; p.bsel = i2 & 1;
        p.masa = (i3 >> 9) & 0x3F; p.adreb = i3 & 0x100; p.nxadr = i3 & 0x80;
        if (p.twt || p.ewt || p.mwt || p.iwt) prog_has_output = true;
        uint32_t bmode = p.zero ? 0 : (p.bsel ? 1 : 2);
        p.kind = (uint8_t)(p.xsel | (p.ysel << 1) | (bmode << 3) | ((p.negb && !p.zero) << 5) | (p.shift << 6));
        if (p.ira <= 0x1F) { p.in_ptr = &d.mems[p.ira]; p.in_shift = 0; }
        else if (p.ira <= 0x2F) { p.in_ptr = &d.mixs[p.ira - 0x20]; p.in_shift = 4; }
        else { p.in_ptr = &kZero; p.in_shift = 0; }
        p.side = p.twt || p.iwt || p.yrl || p.frcl || ((step & 1) && (p.mrd || p.mwt)) || p.adrl || p.ewt;
    }
    prog_end = 0;
    for (int i = 0; i < 128; ++i) { if (!prog[i].empty) prog_end = i + 1; prog_skip[i] = prog[i].empty && (i + 1 < 128 && prog[i + 1].empty); }
    select_compiled();
    // DB_DSPPROG=dir (desktop): dump each distinct program (MPRO 0x3400-0x3BFF, 16-bit words) for tools/dsprec.py
    if (const char* dir = getenv("DB_DSPPROG")) {
        uint16_t w[128 * 4]; for (int i = 0; i < 128 * 4; ++i) w[i] = aica_regs_ptr[(0x3400 + i * 4) / 4];
        uint64_t h = 1469598103934665603ull; for (uint16_t x : w) h = (h ^ x) * 1099511628211ull;
        char path[512]; std::snprintf(path, sizeof path, "%s/prog_%016llx.bin", dir, (unsigned long long)h);
        if (FILE* f = std::fopen(path, "wb")) { std::fwrite(w, 2, 128 * 4, f); std::fclose(f); }
    }
    static const bool dbg = getenv("DB_DSPDBG") != nullptr;
    if (dbg) {
        int empty = 0, mem = 0, last = -1;
        for (int i = 0; i < 128; ++i) { if (prog[i].empty) ++empty; else last = i; if (prog[i].mrd || prog[i].mwt) ++mem; }
        std::printf("[dsp] program decoded: %d empty steps, %d memory steps, last non-empty step %d\n", empty, mem, last);
    }
}


inline uint16_t REG(uint32_t off) { return aica_regs_ptr[off / 4]; }
inline int32_t sx24(int32_t v) { return (v << 8) >> 8; }

// 16-bit DSP float (sign, 4-bit exponent, 11-bit mantissa) <-> 24-bit integer
uint16_t pack(int32_t v) {
    int sign = (v >> 23) & 1;
    uint32_t t = (uint32_t)(v ^ (v << 1)) & 0xFFFFFF;
    int e = t ? __builtin_clz(t << 8) : 12;   // leading zeros in the 24-bit field, at most 12
    if (e > 12) e = 12;
    v = e < 12 ? (v << e) & 0x3FFFFF : v << 11;
    v >>= 11;
    return (uint16_t)((v & 0x7FF) | (sign << 15) | (e << 11));
}
int32_t unpack(uint16_t v) {
    int sign = (v >> 15) & 1, e = (v >> 11) & 15;
    int32_t m = (v & 0x7FF) << 11;
    if (e > 11) { e = 11; m |= sign << 22; } else m |= (sign ^ 1) << 22;
    m |= sign << 23;
    return sx24(m) >> e;
}
// ahead-of-time compiled versions of the game's DSP programs (tools/dsprec.py), selected by MPRO hash
#include "aica_dsp_progs.inc"
void (*compiled_prog)();
void select_compiled() {   // an ahead-of-time compiled version of the current program, if there is one
    static const bool interp_only = getenv("DB_DSPINTERP") != nullptr;
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 128 * 4; ++i) h = (h ^ aica_regs_ptr[(0x3400 + i * 4) / 4]) * 1099511628211ull;
    compiled_prog = nullptr;
    if (!interp_only) for (const DspProg& pr : kDspProgs) if (pr.hash == h) compiled_prog = pr.fn;
}
} // namespace

void aica_dsp_input(int sel, int32_t v) { d.mixs[sel & 15] += v; }   // 20-bit send from a channel

// ALU of one step, specialised on its shape K (see Step::kind): the shifter output of the previous ACC, then
// ACC = X * Y >> 12 + B (26-bit). Branch-free for a given K.
struct AluState { int64_t acc; int32_t inputs, frc, yreg, shifted; uint32_t mdec; };
template <unsigned K> void alu(AluState& s, const Step& p, int32_t coef) {
    constexpr bool XSEL = K & 1, NEGB = (K >> 5) & 1;
    constexpr unsigned YSEL = (K >> 1) & 3, BMODE = (K >> 3) & 3, SHIFT = (K >> 6) & 3;
    int32_t sh = (SHIFT == 0 || SHIFT == 3) ? (int32_t)s.acc : (int32_t)(s.acc << 1);
    if (SHIFT < 2) sh = sh < -0x800000 ? -0x800000 : sh > 0x7FFFFF ? 0x7FFFFF : sh;
    else sh = sx24(sh);
    int32_t t = d.temp[(p.tra + s.mdec) & 0x7F];
    int32_t b = BMODE == 0 ? 0 : BMODE == 1 ? (int32_t)s.acc : t;
    if (NEGB) b = -b;
    int32_t x = XSEL ? s.inputs : t;
    int32_t y = YSEL == 0 ? s.frc : YSEL == 1 ? coef : YSEL == 2 ? s.yreg >> 11 : (s.yreg >> 4) & 0x0FFF;
    s.shifted = sh;
    int64_t a = (((int64_t)x * y) >> 12) + b;
    s.acc = (a << 38) >> 38;
}
using AluFn = void (*)(AluState&, const Step&, int32_t);
template <unsigned... K> constexpr std::array<AluFn, sizeof...(K)> alu_table(std::integer_sequence<unsigned, K...>) { return {{alu<K>...}}; }
const std::array<AluFn, 256> kAlu = alu_table(std::make_integer_sequence<unsigned, 256>{});

void aica_dsp_step_impl() {
    if (aica_dsp_dirty) { decode(); aica_dsp_dirty = false; }
    if (compiled_prog) { compiled_prog(); return; }
    if (!prog_has_output) { std::memset(d.mixs, 0, sizeof d.mixs); return; }   // program cannot affect the output
    uint16_t rbreg = REG(0x2804);
    uint32_t rbl = (8192u << ((rbreg >> 13) & 3)) - 1;            // ring length in words - 1
    uint32_t rbp = (uint32_t)(rbreg & 0xFFF) * 2048;               // byte address
    uint8_t* ram = aica_aram();
    AluState s{0, 0, 0, 0, 0, d.mdec_ct};
    uint32_t adrs = 0;
    bool any = false;
    for (int step = 0; step < prog_end; ++step) {
        if (prog_skip[step]) continue;
        const Step& p = prog[step];
        if (p.empty) {                                             // empty step: X = TEMP[DEC], Y = FRC, B = X
            int32_t x = d.temp[d.mdec_ct & 0x7F];
            s.acc = (((int64_t)x * s.frc) >> 12) + x;
            continue;
        }
        any = true;
        s.inputs = sx24(*p.in_ptr << p.in_shift);
        if (p.iwt) d.mems[p.iwa] = d.memval[step & 3];             // memory value read two steps earlier
        kAlu[p.kind](s, p, (int16_t)aica_regs_ptr[(0x3000 + step * 4) / 4] >> 3);   // COEF: 13 bits in 15:3
        if (!p.side) continue;
        if (p.yrl) s.yreg = s.inputs;
        int32_t shifted = s.shifted;
        if (p.twt) d.temp[(p.twa + d.mdec_ct) & 0x7F] = shifted;
        if (p.frcl) s.frc = p.shift == 3 ? (shifted & 0x0FFF) : (shifted >> 11);
        if ((step & 1) && (p.mrd || p.mwt)) {                      // memory access on odd steps
            uint32_t addr = REG(0x3200 + p.masa * 4);
            if (p.adreb) addr += adrs & 0x0FFF;
            if (p.nxadr) addr++;
            if (!p.table) { addr += d.mdec_ct; addr &= rbl; } else addr &= 0xFFFF;
            uint32_t ba = (rbp + (addr << 1)) & 0x1FFFFE;
            if (p.mrd) { uint16_t w; std::memcpy(&w, ram + ba, 2); d.memval[(step + 2) & 3] = unpack(w); }
            if (p.mwt) { uint16_t w = pack(shifted); std::memcpy(ram + ba, &w, 2); }
        }
        if (p.adrl) adrs = p.shift == 3 ? (uint32_t)(shifted >> 12) : (uint32_t)(s.inputs >> 16);
        if (p.ewt) d.efreg[p.ewa] = shifted >> 8;
    }
    if (--d.mdec_ct == 0) d.mdec_ct = rbl + 1;
    std::memset(d.mixs, 0, sizeof d.mixs);
    if (!any) std::memset(d.efreg, 0, sizeof d.efreg);
}

void aica_dsp_step() { aica_dsp_step_impl(); }

// effect outputs mixed with EFSDL/EFPAN (0x2000 + slot * 4: EFSDL 11:8, EFPAN 4:0)
void aica_dsp_output(float* l, float* r, const float* step3_gain) {
    for (int i = 0; i < 16; ++i) {
        int32_t v = (int16_t)d.efreg[i];
        if (!v) continue;
        uint16_t reg = REG(0x2000 + i * 4);
        int sdl = (reg >> 8) & 15, pan = reg & 31;
        if (!sdl) continue;
        float g = step3_gain[15 - sdl], gl = g, gr = g;
        int pa = pan & 15;
        float pg = pa == 15 ? 0.0f : step3_gain[pa];
        if (pan & 16) gl *= pg; else gr *= pg;
        *l += v * gl; *r += v * gr;
    }
}
void aica_dsp_state(SaveIO& s) { s.v(d); if (s.load) aica_dsp_dirty = true; }
} // namespace hw
