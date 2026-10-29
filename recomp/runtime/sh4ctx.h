// SH-4 guest context and inline semantics helpers for the Dolphin Blue static recompilation.
// Own implementation from the SH-4 software manual (Renesas SH-4 programming manual,
// REJ09B0318). Flycast was consulted only as a behavioural reference (no code copied).
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>

namespace sh4 {

enum : uint32_t {
    SR_T = 1u << 0, SR_S = 1u << 1, SR_IMASK = 0xF0u, SR_Q = 1u << 8, SR_M = 1u << 9,
    SR_FD = 1u << 15, SR_BL = 1u << 28, SR_RB = 1u << 29, SR_MD = 1u << 30,
    FPSCR_RM = 3u, FPSCR_DN = 1u << 18, FPSCR_PR = 1u << 19, FPSCR_SZ = 1u << 20, FPSCR_FR = 1u << 21,
};

struct Ctx {
    uint32_t r[16];        // current general registers (R0-R7 of the active bank)
    uint32_t rbank[8];     // the inactive R0-R7 bank
    uint32_t t;            // SR.T kept separately (0/1)
    uint32_t sr;           // SR without T (Q, M, S, IMASK, BL, RB, MD, FD)
    uint32_t gbr, vbr, ssr, spc, sgr, dbr;
    uint32_t mach, macl, pr;
    uint32_t fpscr, fpul;
    union { float f[2][16]; uint32_t u[2][16]; } fr;   // [bank][reg]; FR bank = fpscr.FR, XF = other
    // runtime bookkeeping
    volatile uint32_t irq_pending;  // set by the hardware layer; polled at back-edges and calls
    int32_t slice;                  // polls left in the current scheduler slice
    uint32_t cur_func;              // last translated function entered (diagnostics)
};

// ---------------------------------------------------------------------------------------------
// Runtime interface (implemented by the hardware/system layer, not by the translator).
uint32_t mmio_r8(uint32_t a);
uint32_t mmio_r16(uint32_t a);
uint32_t mmio_r32(uint32_t a);
void mmio_w8(uint32_t a, uint32_t v);
void mmio_w16(uint32_t a, uint32_t v);
void mmio_w32(uint32_t a, uint32_t v);
void dispatch(Ctx* c, uint32_t target);         // indirect call; unknown target -> fatal
[[noreturn]] void fatal_jump(Ctx* c, uint32_t from, uint32_t target);
void pref_sq(Ctx* c, uint32_t addr);             // store-queue flush (addr in 0xE0000000-0xE3FFFFFF)
static inline void pref(Ctx* c, uint32_t addr) { if ((addr >> 26) == 0x38) pref_sq(c, addr); }   // else: cache hint, no-op
void trapa(Ctx* c, uint32_t imm, uint32_t pc);
void sleep(Ctx* c, uint32_t pc);
void rte(Ctx* c);
void ldtlb(Ctx* c);
void poll_irq(Ctx* c);                           // deliver a pending interrupt
void set_sr(Ctx* c, uint32_t v);                 // handles RB bank swap and IMASK change
extern uint8_t ram[];                            // 16 MB main RAM (area 3, mirrored); fixed address, so no pointer reloads
extern uint32_t sq_mem[2][8];                    // SH-4 store queues SQ0/SQ1 (written inline by wr32)

#ifdef SH4_DEBUG
void dbg_enter(Ctx* c, uint32_t addr, uint32_t fpmask, uint32_t fpval);
void dbg_leave(Ctx* c);
void dbg_watch(uint32_t a, uint32_t v, int size);
extern uint32_t watch_lo, watch_hi;
#define SH4_WATCH(a, v, n) do { if ((uint32_t)((a) - ::sh4::watch_lo) < ::sh4::watch_hi - ::sh4::watch_lo) ::sh4::dbg_watch(a, v, n); } while (0)
#else
#define SH4_WATCH(a, v, n) do { } while (0)
#endif

// ---------------------------------------------------------------------------------------------
// Memory accessors. Fast path: area 3 (main RAM, 0x0C000000-0x0FFFFFFF incl. P0/P1/P2/P3 mirrors).
static inline bool is_ram(uint32_t a) { return (a & 0x1C000000u) == 0x0C000000u && (a >> 29) != 7; }
static inline uint32_t rd8(uint32_t a)  { if (is_ram(a)) return ram[a & 0xFFFFFF]; return mmio_r8(a); }
static inline uint32_t rd16(uint32_t a) { if (is_ram(a)) { uint16_t v; std::memcpy(&v, ram + (a & 0xFFFFFF), 2); return v; } return mmio_r16(a); }
static inline uint32_t rd32(uint32_t a) { if (is_ram(a)) { uint32_t v; std::memcpy(&v, ram + (a & 0xFFFFFF), 4); return v; } return mmio_r32(a); }
static inline void wr8(uint32_t a, uint32_t v)  { SH4_WATCH(a, v, 1); if (is_ram(a)) { ram[a & 0xFFFFFF] = (uint8_t)v; return; } mmio_w8(a, v); }
static inline void wr16(uint32_t a, uint32_t v) { SH4_WATCH(a, v, 2); if (is_ram(a)) { uint16_t x = (uint16_t)v; std::memcpy(ram + (a & 0xFFFFFF), &x, 2); return; } mmio_w16(a, v); }
static inline void wr32(uint32_t a, uint32_t v) {
    SH4_WATCH(a, v, 4);
    if (is_ram(a)) { std::memcpy(ram + (a & 0xFFFFFF), &v, 4); return; }
    if ((a >> 26) == 0x38) { sq_mem[(a >> 5) & 1][(a >> 2) & 7] = v; return; }   // store queue (TA submission)
    mmio_w32(a, v);
}
// FMAC: fused on the desktop; on the Vita (VFPv3, no fused multiply-add) exactly what newlib's fmaf computes,
// inline instead of a library call
static inline float fmac(float a, float b, float c) {
#if defined(__arm__)
    return (float)((double)a * (double)b + (double)c);
#else
    return std::fma(a, b, c);
#endif
}
static inline uint32_t sx8(uint32_t v)  { return (uint32_t)(int32_t)(int8_t)v; }
static inline uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }

// ---------------------------------------------------------------------------------------------
// Integer helpers (semantics per the SH-4 manual pseudo-code).
static inline void addc(Ctx* c, uint32_t n, uint32_t m) {
    uint32_t t0 = c->r[n], t1 = c->r[n] + c->r[m];
    c->r[n] = t1 + c->t;
    c->t = (t0 > t1) | (t1 > c->r[n]);
}
static inline void subc(Ctx* c, uint32_t n, uint32_t m) {
    uint32_t t0 = c->r[n], t1 = c->r[n] - c->r[m];
    c->r[n] = t1 - c->t;
    c->t = (t0 < t1) | (t1 < c->r[n]);
}
static inline void negc(Ctx* c, uint32_t n, uint32_t m) {
    uint32_t tmp = 0u - c->r[m];
    c->r[n] = tmp - c->t;
    c->t = (0u < tmp) | (tmp < c->r[n]);
}
static inline void addv(Ctx* c, uint32_t n, uint32_t m) {
    int32_t a = (int32_t)c->r[n], b = (int32_t)c->r[m];
    int32_t r = (int32_t)((uint32_t)a + (uint32_t)b);
    c->t = ((a >= 0) == (b >= 0)) && ((r >= 0) != (a >= 0));
    c->r[n] = (uint32_t)r;
}
static inline void subv(Ctx* c, uint32_t n, uint32_t m) {
    int32_t a = (int32_t)c->r[n], b = (int32_t)c->r[m];
    int32_t r = (int32_t)((uint32_t)a - (uint32_t)b);
    c->t = ((a >= 0) != (b >= 0)) && ((r >= 0) != (a >= 0));
    c->r[n] = (uint32_t)r;
}
static inline uint32_t cmpstr(uint32_t a, uint32_t b) {
    uint32_t x = a ^ b;
    return ((x & 0xFF000000u) == 0) | ((x & 0x00FF0000u) == 0) | ((x & 0x0000FF00u) == 0) | ((x & 0xFFu) == 0);
}
static inline void div0s(Ctx* c, uint32_t n, uint32_t m) {
    uint32_t q = c->r[n] >> 31, mm = c->r[m] >> 31;
    c->sr = (c->sr & ~(SR_Q | SR_M)) | (q << 8) | (mm << 9);
    c->t = q ^ mm;
}
static inline void div0u(Ctx* c) { c->sr &= ~(SR_Q | SR_M); c->t = 0; }
static inline void div1(Ctx* c, uint32_t n, uint32_t m) {
    uint32_t old_q = (c->sr >> 8) & 1, M = (c->sr >> 9) & 1;
    uint32_t Q = c->r[n] >> 31;
    uint32_t tmp2 = c->r[m];
    uint32_t rn = (c->r[n] << 1) | c->t;
    uint32_t tmp0 = rn, tmp1;
    if (old_q == M) {            // subtract
        rn -= tmp2; tmp1 = rn > tmp0;
        Q = (old_q == 0) ? (Q ? !tmp1 : tmp1) : (Q ? tmp1 : !tmp1);
    } else {                    // add
        rn += tmp2; tmp1 = rn < tmp0;
        Q = (old_q == 0) ? (Q ? tmp1 : !tmp1) : (Q ? !tmp1 : tmp1);
    }
    c->r[n] = rn;
    c->sr = (c->sr & ~SR_Q) | (Q << 8);
    c->t = (Q == M);
}
static inline void shad(Ctx* c, uint32_t n, uint32_t m) {
    int32_t s = (int32_t)c->r[m];
    if (s >= 0) c->r[n] <<= (s & 31);
    else if ((s & 31) == 0) c->r[n] = ((int32_t)c->r[n] < 0) ? 0xFFFFFFFFu : 0;
    else c->r[n] = (uint32_t)((int32_t)c->r[n] >> ((~s & 31) + 1));
}
static inline void shld(Ctx* c, uint32_t n, uint32_t m) {
    int32_t s = (int32_t)c->r[m];
    if (s >= 0) c->r[n] <<= (s & 31);
    else if ((s & 31) == 0) c->r[n] = 0;
    else c->r[n] >>= ((~s & 31) + 1);
}
static inline void dmuls(Ctx* c, uint32_t n, uint32_t m) {
    int64_t r = (int64_t)(int32_t)c->r[n] * (int64_t)(int32_t)c->r[m];
    c->mach = (uint32_t)((uint64_t)r >> 32); c->macl = (uint32_t)r;
}
static inline void dmulu(Ctx* c, uint32_t n, uint32_t m) {
    uint64_t r = (uint64_t)c->r[n] * (uint64_t)c->r[m];
    c->mach = (uint32_t)(r >> 32); c->macl = (uint32_t)r;
}
static inline void macl(Ctx* c, uint32_t n, uint32_t m) {
    // operands read in order @Rn then @Rm per manual; both post-incremented by 4
    int32_t a = (int32_t)rd32(c->r[n]); c->r[n] += 4;
    int32_t b = (int32_t)rd32(c->r[m]); c->r[m] += 4;
    int64_t mul = (int64_t)a * (int64_t)b;
    int64_t acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl);
    int64_t r = (int64_t)((uint64_t)acc + (uint64_t)mul);
    if (c->sr & SR_S) {         // saturate to 48 bits
        const int64_t mx = 0x00007FFFFFFFFFFFLL, mn = -0x0000800000000000LL;
        if (r > mx) r = mx; else if (r < mn) r = mn;
    }
    c->mach = (uint32_t)((uint64_t)r >> 32); c->macl = (uint32_t)r;
}
static inline void macw(Ctx* c, uint32_t n, uint32_t m) {
    int32_t a = (int16_t)rd16(c->r[n]); c->r[n] += 2;
    int32_t b = (int16_t)rd16(c->r[m]); c->r[m] += 2;
    int32_t mul = a * b;
    if (c->sr & SR_S) {         // 32-bit saturating accumulate into MACL, MACH bit0 = overflow flag
        int64_t r = (int64_t)(int32_t)c->macl + mul;
        if (r > 0x7FFFFFFFLL) { r = 0x7FFFFFFF; c->mach |= 1; }
        else if (r < -0x80000000LL) { r = -0x80000000LL; c->mach |= 1; }
        c->macl = (uint32_t)(int32_t)r;
    } else {
        int64_t acc = (int64_t)(((uint64_t)c->mach << 32) | c->macl);
        int64_t r = (int64_t)((uint64_t)acc + (uint64_t)(int64_t)mul);
        c->mach = (uint32_t)((uint64_t)r >> 32); c->macl = (uint32_t)r;
    }
}
static inline void tas(Ctx* c, uint32_t n) {
    uint32_t v = rd8(c->r[n]);
    c->t = (v == 0);
    wr8(c->r[n], v | 0x80);
}
static inline uint32_t get_sr(Ctx* c) { return c->sr | c->t; }

// ---------------------------------------------------------------------------------------------
// FPU helpers. Register bank b = FPSCR.FR (FR regs), b^1 = XF regs.
static inline uint32_t frb(Ctx* c) { return (c->fpscr >> 21) & 1; }
static inline double getdr(Ctx* c, uint32_t b, uint32_t n) {   // DRn: FR(n) high word, FR(n+1) low word
    uint64_t v = ((uint64_t)c->fr.u[b][n] << 32) | c->fr.u[b][n + 1];
    double d; std::memcpy(&d, &v, 8); return d;
}
static inline void setdr(Ctx* c, uint32_t b, uint32_t n, double d) {
    uint64_t v; std::memcpy(&v, &d, 8);
    c->fr.u[b][n] = (uint32_t)(v >> 32); c->fr.u[b][n + 1] = (uint32_t)v;
}
static inline uint32_t ftrc_s(float f) {
    if (std::isnan(f)) return 0x80000000u;
    if (f >= 2147483648.0f) return 0x7FFFFFFFu;
    if (f < -2147483648.0f) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}
static inline uint32_t ftrc_d(double f) {
    if (std::isnan(f)) return 0x80000000u;
    if (f >= 2147483648.0) return 0x7FFFFFFFu;
    if (f <= -2147483649.0) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}
static inline void fipr(Ctx* c, uint32_t b, uint32_t n, uint32_t m) {
    const float* x = c->fr.f[b];
    double r = (double)x[n] * x[m] + (double)x[n + 1] * x[m + 1] + (double)x[n + 2] * x[m + 2] + (double)x[n + 3] * x[m + 3];
    c->fr.f[b][n + 3] = (float)r;
}
static inline void ftrv(Ctx* c, uint32_t b, uint32_t n) {
    const float* xf = c->fr.f[b ^ 1];
    float* v = c->fr.f[b];
    double r[4];
    for (int i = 0; i < 4; ++i)
        r[i] = (double)xf[i] * v[n] + (double)xf[4 + i] * v[n + 1] + (double)xf[8 + i] * v[n + 2] + (double)xf[12 + i] * v[n + 3];
    for (int i = 0; i < 4; ++i) v[n + i] = (float)r[i];
}
static inline void fsca(Ctx* c, uint32_t b, uint32_t n) {
    // NOTE: hardware uses a ROM table; this is the analytic approximation (TODO: exact table)
    double ang = (double)(c->fpul & 0xFFFF) * (2.0 * 3.14159265358979323846 / 65536.0);
    c->fr.f[b][n] = (float)std::sin(ang);
    c->fr.f[b][n + 1] = (float)std::cos(ang);
}
static inline float fsrra(float x) { return (float)(1.0 / std::sqrt((double)x)); }

} // namespace sh4

// Debug hooks (compiled out by default)
#ifdef SH4_DEBUG
struct Sh4Frame {
    sh4::Ctx* c;
    Sh4Frame(sh4::Ctx* c_, uint32_t a, uint32_t m, uint32_t v) : c(c_) { sh4::dbg_enter(c, a, m, v); }
    ~Sh4Frame() { sh4::dbg_leave(c); }
};
#define SH4_ENTRY_CHECK(c, addr, fpmask, fpval) Sh4Frame sh4frame_(c, addr, fpmask, fpval)
#endif
#ifndef SH4_ENTRY_CHECK
#define SH4_ENTRY_CHECK(c, addr, fpmask, fpval) do { (c)->cur_func = (addr); } while (0)
#endif
#ifndef SH4_POLL
#define SH4_POLL(c) do { if (--(c)->slice <= 0 || (c)->irq_pending) ::sh4::poll_irq(c); } while (0)
#endif
