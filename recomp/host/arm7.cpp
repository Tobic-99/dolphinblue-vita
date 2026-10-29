// ARM7DI interpreter for the AICA sound CPU (runs MANATEE.DRV from sound RAM).
// Own implementation of the ARM architecture (ARMv3 ARM state, plus the ARMv4 halfword/long-multiply
// encodings for safety). AICA wiring (FIQ via the e68k-style interrupt controller, L/M registers)
// follows the AICA documentation; Flycast was used only as a behavioural reference.
#include "hw.h"
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <map>
#include <cstring>

namespace hw {

uint32_t aica_reg_read_arm(uint32_t off, int size);
void aica_reg_write_arm(uint32_t off, uint32_t v, int size);

namespace arm {

static uint32_t r[16];
static uint32_t cpsr;
static inline uint32_t aram32(uint32_t a);   // DB_ARMWATCH: last block-start PCs
static uint32_t spsr_fiq, spsr_irq, spsr_svc, spsr_abt, spsr_und;
static uint32_t bank_usr[7], bank_fiq[7];     // r8-r14 user/system and FIQ
static uint32_t bank_irq[2], bank_svc[2], bank_abt[2], bank_und[2];
static bool running;
static int idle_sig = -1;   // idle skip: the driver's main loop is the expected code (checked once per reset)
static int alloc_sig = -1;  // voice allocator fix (alloc_fix): the driver's allocator is the expected code
static bool fiq_line;          // e68k output (FIQ request)
uint64_t instr_count, unknown_ops;

enum { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1B, M_SYS = 0x1F };
static inline bool N() { return cpsr >> 31; }
static inline bool Z() { return (cpsr >> 30) & 1; }
static inline bool C() { return (cpsr >> 29) & 1; }
static inline bool V() { return (cpsr >> 28) & 1; }
static inline void setNZ(uint32_t v) { cpsr = (cpsr & 0x3FFFFFFF) | (v & 0x80000000u) | (v == 0 ? 0x40000000u : 0); }
static inline void setC(bool c) { cpsr = (cpsr & ~0x20000000u) | (c ? 0x20000000u : 0); }
static inline void setV(bool v) { cpsr = (cpsr & ~0x10000000u) | (v ? 0x10000000u : 0); }

// ------------------------------------------------------------------ memory
static constexpr uint32_t kCodeLimit = 0x9040;   // driver code size (MANATEE.DRV loaded at ARAM 0)
static uint8_t code_used[kCodeLimit / 4];         // 1 = a translated instruction lives here
static bool blocks_ok;                            // ARAM holds the translated driver image unchanged
// a store into translated code (by the ARM or the SH-4) disables the blocks; the interpreter takes over
static void code_hit(uint32_t a, uint32_t v, bool by_arm) {   // a store into translated code: the blocks are off from here
    if (blocks_ok && getenv("DB_ARMWATCH"))
        std::printf("[armwatch] vbl %u: %s wrote %08x to driver code at %06x (arm pc %06x lr %06x sp %06x cpsr %08x)\n", vblank_count, by_arm ? "ARM7" : "SH-4", v, a, r[15], r[14], r[13], cpsr);
    blocks_ok = false;
}
void code_written(uint32_t a) { a &= 0x00FFFFFF; if (a < kCodeLimit && code_used[a >> 2]) code_hit(a, 0, false); }
static uint32_t rd32(uint32_t a) {
    a &= 0x00FFFFFF;
    if (a < 0x800000) { uint32_t v; std::memcpy(&v, aram + (a & (ARAM_SIZE - 1) & ~3u), 4);
        uint32_t rot = (a & 3) * 8; return rot ? (v >> rot) | (v << (32 - rot)) : v; }
    if (a < 0x808000) return aica_reg_read_arm(a & 0x7FFF, 4);
    return 0;
}
static uint32_t rd16(uint32_t a) {
    a &= 0x00FFFFFF;
    if (a < 0x800000) { uint16_t v; std::memcpy(&v, aram + (a & (ARAM_SIZE - 1) & ~1u), 2); return v; }
    if (a < 0x808000) return aica_reg_read_arm(a & 0x7FFF, 2) & 0xFFFF;
    return 0;
}
static uint32_t rd8(uint32_t a) {
    a &= 0x00FFFFFF;
    if (a < 0x800000) return aram[a & (ARAM_SIZE - 1)];
    if (a < 0x808000) return aica_reg_read_arm(a & 0x7FFF, 1) & 0xFF;
    return 0;
}
static void wr32(uint32_t a, uint32_t v) {
    a &= 0x00FFFFFF;
    if (a < kCodeLimit && code_used[a >> 2]) code_hit(a, v, true);
    if (a < 0x800000) { std::memcpy(aram + (a & (ARAM_SIZE - 1) & ~3u), &v, 4); return; }
    if (a < 0x808000) aica_reg_write_arm(a & 0x7FFC, v, 4);
}
static void wr16(uint32_t a, uint32_t v) {
    a &= 0x00FFFFFF;
    if (a < kCodeLimit && code_used[a >> 2]) code_hit(a, v, true);
    if (a < 0x800000) { uint16_t x = (uint16_t)v; std::memcpy(aram + (a & (ARAM_SIZE - 1) & ~1u), &x, 2); return; }
    if (a < 0x808000) aica_reg_write_arm(a & 0x7FFE, v & 0xFFFF, 2);
}
static void wr8(uint32_t a, uint32_t v) {
    a &= 0x00FFFFFF;
    if (a < kCodeLimit && code_used[a >> 2]) code_hit(a, v, true);
    if (a < 0x800000) { aram[a & (ARAM_SIZE - 1)] = (uint8_t)v; return; }
    if (a < 0x808000) aica_reg_write_arm(a & 0x7FFF, v & 0xFF, 1);
}

// ------------------------------------------------------------------ modes
static uint32_t* spsr_ptr(uint32_t mode) {
    switch (mode) {
    case M_FIQ: return &spsr_fiq; case M_IRQ: return &spsr_irq; case M_SVC: return &spsr_svc;
    case M_ABT: return &spsr_abt; case M_UND: return &spsr_und;
    }
    return nullptr;
}
static void save_bank(uint32_t mode) {
    if (mode == M_FIQ) { for (int i = 0; i < 7; ++i) bank_fiq[i] = r[8 + i]; return; }
    for (int i = 0; i < 5; ++i) bank_usr[i] = r[8 + i];
    uint32_t* b = mode == M_IRQ ? bank_irq : mode == M_SVC ? bank_svc : mode == M_ABT ? bank_abt : mode == M_UND ? bank_und : nullptr;
    if (b) { b[0] = r[13]; b[1] = r[14]; } else { bank_usr[5] = r[13]; bank_usr[6] = r[14]; }
}
static void load_bank(uint32_t mode) {
    if (mode == M_FIQ) { for (int i = 0; i < 7; ++i) r[8 + i] = bank_fiq[i]; return; }
    for (int i = 0; i < 5; ++i) r[8 + i] = bank_usr[i];
    uint32_t* b = mode == M_IRQ ? bank_irq : mode == M_SVC ? bank_svc : mode == M_ABT ? bank_abt : mode == M_UND ? bank_und : nullptr;
    if (b) { r[13] = b[0]; r[14] = b[1]; } else { r[13] = bank_usr[5]; r[14] = bank_usr[6]; }
}
static void switch_mode(uint32_t newmode) {
    uint32_t old = cpsr & 0x1F;
    if (old == newmode) return;
    save_bank(old);
    cpsr = (cpsr & ~0x1Fu) | newmode;
    load_bank(newmode);
}
static void set_cpsr(uint32_t v) {
    switch_mode(v & 0x1F);
    cpsr = v;
}

static void exception(uint32_t vector, uint32_t mode, uint32_t ret, bool disable_fiq) {
    uint32_t old = cpsr;
    switch_mode(mode);
    *spsr_ptr(mode) = old;
    r[14] = ret;
    cpsr |= 0x80;                 // I
    if (disable_fiq) cpsr |= 0x40;
    r[15] = vector;
}

// ------------------------------------------------------------------ shifter
static uint32_t shift(uint32_t v, uint32_t type, uint32_t amt, bool reg_amt, bool& carry) {
    carry = C();
    switch (type) {
    case 0:  // LSL
        if (amt == 0) return v;
        if (amt < 32) { carry = (v >> (32 - amt)) & 1; return v << amt; }
        carry = amt == 32 ? (v & 1) : 0; return 0;
    case 1:  // LSR
        if (amt == 0) { if (reg_amt) return v; carry = v >> 31; return 0; }
        if (amt < 32) { carry = (v >> (amt - 1)) & 1; return v >> amt; }
        carry = amt == 32 ? (v >> 31) : 0; return 0;
    case 2:  // ASR
        if (amt == 0) { if (reg_amt) return v; carry = v >> 31; return (int32_t)v >> 31; }
        if (amt < 32) { carry = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
        carry = v >> 31; return (uint32_t)((int32_t)v >> 31);
    default: // ROR / RRX
        if (amt == 0) {
            if (reg_amt) return v;
            bool c = C(); carry = v & 1; return (v >> 1) | ((uint32_t)c << 31);
        }
        amt &= 31;
        if (amt == 0) { carry = v >> 31; return v; }
        carry = (v >> (amt - 1)) & 1; return (v >> amt) | (v << (32 - amt));
    }
}

static bool cond(uint32_t c) {
    switch (c) {
    case 0: return Z(); case 1: return !Z(); case 2: return C(); case 3: return !C();
    case 4: return N(); case 5: return !N(); case 6: return V(); case 7: return !V();
    case 8: return C() && !Z(); case 9: return !C() || Z(); case 10: return N() == V(); case 11: return N() != V();
    case 12: return !Z() && N() == V(); case 13: return Z() || N() != V(); case 14: return true;
    }
    return false;
}

static void undefined(uint32_t op, uint32_t pc) {
    if (unknown_ops++ < 8) trace("[arm7] undefined/unsupported op %08x at %06x\n", op, pc);
    exception(0x04, M_UND, pc + 4, false);
}

// ------------------------------------------------------------------ execute one instruction
// condition pass table: bit c of cond_lut[NZCV] = condition c passes with those flags
static uint16_t cond_lut[16];
static bool cond_lut_init = [] {
    for (uint32_t f = 0; f < 16; ++f) {
        bool n = f & 8, z = f & 4, c = f & 2, v = f & 1; uint16_t m = 0;
        bool t[16] = {z, !z, c, !c, n, !n, v, !v, c && !z, !c || z, n == v, n != v, !z && n == v, z || n != v, true, false};
        for (int k = 0; k < 16; ++k) if (t[k]) m |= 1u << k;
        cond_lut[f] = m;
    }
    return true;
}();
// Executes one instruction (r[15] already advanced to pc + 4). Always inlined: the translated driver blocks
// (arm7_blocks.inc) call it with constant op/pc so the compiler folds the decoding away.
static inline __attribute__((always_inline)) void exec(uint32_t op, uint32_t pc) {
    if (!((cond_lut[cpsr >> 28] >> (op >> 28)) & 1)) return;
    auto reg = [&](uint32_t n) { return n == 15 ? pc + 8 : r[n]; };

    uint32_t cls = (op >> 25) & 7;
    if (cls <= 1) {
        // multiply / swap / halfword / PSR / BX / data processing
        if (cls == 0 && (op & 0x0FC000F0) == 0x00000090) {          // MUL/MLA
            uint32_t rd = (op >> 16) & 15, rn = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
            uint32_t res = r[rm] * r[rs] + ((op >> 21) & 1 ? r[rn] : 0);
            r[rd] = res;
            if (op & (1u << 20)) setNZ(res);
            return;
        }
        if (cls == 0 && (op & 0x0F8000F0) == 0x00800090) {          // UMULL/UMLAL/SMULL/SMLAL
            uint32_t hi = (op >> 16) & 15, lo = (op >> 12) & 15, rs = (op >> 8) & 15, rm = op & 15;
            uint64_t res = (op & (1u << 22)) ? (uint64_t)((int64_t)(int32_t)r[rm] * (int64_t)(int32_t)r[rs])
                                               : (uint64_t)r[rm] * r[rs];
            if (op & (1u << 21)) res += ((uint64_t)r[hi] << 32) | r[lo];
            r[lo] = (uint32_t)res; r[hi] = (uint32_t)(res >> 32);
            if (op & (1u << 20)) { cpsr = (cpsr & 0x3FFFFFFF) | ((res >> 32) & 0x80000000u) | (res == 0 ? 0x40000000u : 0); }
            return;
        }
        if (cls == 0 && (op & 0x0FB00FF0) == 0x01000090) {          // SWP/SWPB
            uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, rm = op & 15;
            uint32_t a = r[rn];
            if (op & (1u << 22)) { uint32_t t = rd8(a); wr8(a, r[rm]); r[rd] = t; }
            else { uint32_t t = rd32(a); wr32(a, r[rm]); r[rd] = t; }
            return;
        }
        if (cls == 0 && (op & 0x0FFFFFF0) == 0x012FFF10) {          // BX
            uint32_t t = r[op & 15];
            if (t & 1) { undefined(op, pc); return; }               // no Thumb on ARM7DI
            r[15] = t & ~3u; return;
        }
        if (cls == 0 && (op & 0x90) == 0x90 && (op & 0x60)) {       // halfword / signed transfers (v4)
            bool P = (op >> 24) & 1, U = (op >> 23) & 1, I = (op >> 22) & 1, W = (op >> 21) & 1, L = (op >> 20) & 1;
            uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15, sh = (op >> 5) & 3;
            uint32_t off = I ? (((op >> 4) & 0xF0) | (op & 15)) : r[op & 15];
            uint32_t base = reg(rn), a = P ? (U ? base + off : base - off) : base;
            if (L) {
                uint32_t v = sh == 1 ? rd16(a) : sh == 2 ? (uint32_t)(int32_t)(int8_t)rd8(a) : (uint32_t)(int32_t)(int16_t)rd16(a);
                if (!P) r[rn] = U ? base + off : base - off; else if (W) r[rn] = a;
                r[rd] = v;
            } else {
                if (sh == 1) wr16(a, reg(rd));
                if (!P) r[rn] = U ? base + off : base - off; else if (W) r[rn] = a;
            }
            return;
        }
        uint32_t opc = (op >> 21) & 15;
        bool S = (op >> 20) & 1;
        if (!S && opc >= 8 && opc <= 11) {                          // PSR transfer
            bool spsr = (op >> 22) & 1;
            if (!(op & (1u << 21))) {                               // MRS
                uint32_t* sp = spsr_ptr(cpsr & 0x1F);
                r[(op >> 12) & 15] = spsr ? (sp ? *sp : cpsr) : cpsr;
            } else {                                                // MSR
                uint32_t v = (op & (1u << 25)) ? ([&] { uint32_t imm = op & 0xFF, rot = ((op >> 8) & 15) * 2;
                                                         return rot ? (imm >> rot) | (imm << (32 - rot)) : imm; })()
                                               : r[op & 15];
                uint32_t mask = 0;
                if (op & (1u << 19)) mask |= 0xFF000000u;
                if (op & (1u << 16)) mask |= 0x000000FFu;
                if ((cpsr & 0x1F) == M_USR) mask &= 0xFF000000u;
                if (spsr) { uint32_t* sp = spsr_ptr(cpsr & 0x1F); if (sp) *sp = (*sp & ~mask) | (v & mask); }
                else set_cpsr((cpsr & ~mask) | (v & mask));
            }
            return;
        }
        // data processing
        uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
        bool carry;
        uint32_t op2;
        if (op & (1u << 25)) {
            uint32_t imm = op & 0xFF, rot = ((op >> 8) & 15) * 2;
            op2 = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
            carry = rot ? (op2 >> 31) : C();
        } else {
            uint32_t rm = op & 15, type = (op >> 5) & 3;
            if (op & 0x10) {
                uint32_t amt = r[(op >> 8) & 15] & 0xFF;
                uint32_t v = rm == 15 ? pc + 12 : r[rm];
                op2 = shift(v, type, amt, true, carry);
                if (rn == 15) rn = 16;  // marker: PC reads +12 below
            } else {
                op2 = shift(reg(rm), type, (op >> 7) & 31, false, carry);
            }
        }
        uint32_t a = rn == 16 ? pc + 12 : reg(rn);
        uint32_t res = 0; bool write = true, logic = false;
        uint64_t wide;
        switch (opc) {
        case 0: res = a & op2; logic = true; break;
        case 1: res = a ^ op2; logic = true; break;
        case 2: res = a - op2; if (S) { setC(a >= op2); setV(((a ^ op2) & (a ^ res)) >> 31); } break;
        case 3: res = op2 - a; if (S) { setC(op2 >= a); setV(((op2 ^ a) & (op2 ^ res)) >> 31); } break;
        case 4: wide = (uint64_t)a + op2; res = (uint32_t)wide; if (S) { setC(wide >> 32); setV((~(a ^ op2) & (a ^ res)) >> 31); } break;
        case 5: wide = (uint64_t)a + op2 + C(); res = (uint32_t)wide; if (S) { setC(wide >> 32); setV((~(a ^ op2) & (a ^ res)) >> 31); } break;
        case 6: { uint64_t sub = (uint64_t)op2 + (C() ? 0 : 1); res = a - op2 - (C() ? 0 : 1);
                  if (S) { setC((uint64_t)a >= sub); setV(((a ^ op2) & (a ^ res)) >> 31); } break; }
        case 7: { uint64_t sub = (uint64_t)a + (C() ? 0 : 1); res = op2 - a - (C() ? 0 : 1);
                  if (S) { setC((uint64_t)op2 >= sub); setV(((op2 ^ a) & (op2 ^ res)) >> 31); } break; }
        case 8: res = a & op2; logic = true; write = false; break;
        case 9: res = a ^ op2; logic = true; write = false; break;
        case 10: res = a - op2; write = false; setC(a >= op2); setV(((a ^ op2) & (a ^ res)) >> 31); break;
        case 11: wide = (uint64_t)a + op2; res = (uint32_t)wide; write = false; setC(wide >> 32); setV((~(a ^ op2) & (a ^ res)) >> 31); break;
        case 12: res = a | op2; logic = true; break;
        case 13: res = op2; logic = true; break;
        case 14: res = a & ~op2; logic = true; break;
        case 15: res = ~op2; logic = true; break;
        }
        if (S) { setNZ(res); if (logic) setC(carry); }
        if (write) {
            r[rd] = res;
            if (rd == 15) {
                if (S) { uint32_t* sp = spsr_ptr(cpsr & 0x1F); if (sp) set_cpsr(*sp); }
                r[15] &= ~3u;
            }
        } else if (rd == 15 && S) {   // TSTP/TEQP/CMPP/CMNP (26-bit legacy): ignore
        }
        return;
    }
    if (cls == 2 || cls == 3) {                                     // LDR/STR
        if (cls == 3 && (op & 0x10)) { undefined(op, pc); return; }
        bool P = (op >> 24) & 1, U = (op >> 23) & 1, B = (op >> 22) & 1, W = (op >> 21) & 1, L = (op >> 20) & 1;
        uint32_t rn = (op >> 16) & 15, rd = (op >> 12) & 15;
        uint32_t off;
        if (cls == 2) off = op & 0xFFF;
        else { bool c; off = shift(r[op & 15] , (op >> 5) & 3, (op >> 7) & 31, false, c); }
        uint32_t base = reg(rn);
        uint32_t a = P ? (U ? base + off : base - off) : base;
        if (L) {
            uint32_t v = B ? rd8(a) : rd32(a);
            if (!P) r[rn] = U ? base + off : base - off; else if (W) r[rn] = a;
            r[rd] = v;
            if (rd == 15) r[15] &= ~3u;
        } else {
            uint32_t v = rd == 15 ? pc + 12 : r[rd];
            if (B) wr8(a, v); else wr32(a, v);
            if (!P) r[rn] = U ? base + off : base - off; else if (W) r[rn] = a;
        }
        return;
    }
    if (cls == 4) {                                                 // LDM/STM
        bool P = (op >> 24) & 1, U = (op >> 23) & 1, S = (op >> 22) & 1, W = (op >> 21) & 1, L = (op >> 20) & 1;
        uint32_t rn = (op >> 16) & 15, list = op & 0xFFFF;
        int n = __builtin_popcount(list);
        uint32_t base = r[rn];
        uint32_t start = U ? (P ? base + 4 : base) : (P ? base - 4 * n : base - 4 * n + 4);
        uint32_t newbase = U ? base + 4 * n : base - 4 * n;
        bool user_bank = S && !(L && (list & 0x8000));
        uint32_t mode = cpsr & 0x1F;
        if (user_bank) switch_mode(M_USR);
        uint32_t a = start;
        if (L) {
            if (W) r[rn] = newbase;
            for (int i = 0; i < 16; ++i) if (list & (1u << i)) { r[i] = rd32(a); a += 4; }
            if (list & 0x8000) {
                r[15] &= ~3u;
                if (S) { uint32_t* sp = spsr_ptr(mode); if (sp) { if (user_bank) switch_mode(mode); set_cpsr(*sp); user_bank = false; } }
            }
        } else {
            for (int i = 0; i < 16; ++i) if (list & (1u << i)) { wr32(a, i == 15 ? pc + 12 : r[i]); a += 4; }
            if (W) r[rn] = newbase;
        }
        if (user_bank) switch_mode(mode);
        return;
    }
    if (cls == 5) {                                                 // B/BL
        int32_t off = (int32_t)(op << 8) >> 6;
        if (op & (1u << 24)) r[14] = pc + 4;
        r[15] = pc + 8 + off;
        return;
    }
    if ((op & 0x0F000000) == 0x0F000000) {                          // SWI
        exception(0x08, M_SVC, pc + 4, false);
        return;
    }
    undefined(op, pc);                                              // coprocessor
}

static void step() {
    uint32_t pc = r[15];
    uint32_t op;
    if ((pc & 0x00FFFFFF) < 0x800000) std::memcpy(&op, aram + (pc & (ARAM_SIZE - 1) & ~3u), 4);   // code lives in ARAM
    else op = rd32(pc);
    r[15] = pc + 4;
    instr_count++;
    exec(op, pc);
}

// ------------------------------------------------------------------ translated driver blocks
#include "arm7_blocks.inc"
static_assert(kCodeEnd == kCodeLimit, "arm7_blocks.inc code size");
static void blocks_validate() {
    blocks_ok = true;
    for (uint32_t i = 0; i < kUsedCount; ++i) {
        uint32_t w; std::memcpy(&w, aram + kUsed[i][0], 4);
        code_used[kUsed[i][0] >> 2] = 1;
        if (w != kUsed[i][1]) blocks_ok = false;
    }
    trace("[arm7] translated driver blocks %s\n", blocks_ok ? "enabled" : "disabled (ARAM code differs)");
}

void reset() {
    if (getenv("DB_ARMWATCH")) std::printf("[armwatch] vbl %u: ARM7 reset\n", vblank_count);
    blocks_validate();
    idle_sig = -1; alloc_sig = -1;
    std::memset(r, 0, sizeof r);
    std::memset(bank_usr, 0, sizeof bank_usr); std::memset(bank_fiq, 0, sizeof bank_fiq);
    cpsr = M_SVC | 0xC0;
    r[15] = 0;
    running = true;
}
void stop() { if (getenv("DB_ARMWATCH")) std::printf("[armwatch] vbl %u: ARM7 stopped at pc %06x\n", vblank_count, r[15]); running = false; }
bool is_running() { return running; }
void set_fiq(bool on) { fiq_line = on; }

static std::map<uint32_t, uint64_t> pc_hist;   // DB_ARMHIST: executed instructions per 16-byte PC bucket (profiling)
static const bool armhist = getenv("DB_ARMHIST") != nullptr;
static uint64_t op_hist[16];
static const char* op_names[16] = {"dp-imm", "dp-reg-imm-shift", "dp-reg-reg-shift", "ldr/str-imm", "ldr/str-reg", "ldm/stm", "b", "bl",
                                   "mul", "halfword", "psr", "bx/swp/other", "cond-fail?", "", "", ""};
static int op_class(uint32_t op) {
    uint32_t cls = (op >> 25) & 7;
    if (cls == 0 && (op & 0x90) == 0x90) return (op & 0x60) ? 9 : ((op & 0x0F800000) == 0 || (op & 0x0F800000) == 0x00800000 ? 8 : 11);
    if (cls <= 1 && ((op >> 23) & 3) == 2 && !((op >> 20) & 1)) return 10;
    if (cls == 1) return 0;
    if (cls == 0) return (op & 0x10) ? 2 : 1;
    if (cls == 2) return 3;
    if (cls == 3) return 4;
    if (cls == 4) return 5;
    if (cls == 5) return (op >> 24) & 1 ? 7 : 6;
    return 11;
}
void print_pc_hist() {
    std::vector<std::pair<uint64_t, uint32_t>> v; uint64_t tot = 0;
    for (auto& e : pc_hist) { v.push_back({e.second, e.first}); tot += e.second; }
    std::sort(v.rbegin(), v.rend());
    { uint64_t t = 0; for (auto x : op_hist) t += x; for (int k = 0; k < 12; ++k) if (op_hist[k]) std::printf("[armops] %-18s %5.1f%%\n", op_names[k], 100.0 * op_hist[k] / t); }
    for (size_t i = 0; i < v.size() && i < 16; ++i) std::printf("[armhist] pc %06x..+f %5.1f%%  %08x %08x %08x %08x\n", v[i].second, 100.0 * v[i].first / (double)tot, rd32(v[i].second), rd32(v[i].second + 4), rd32(v[i].second + 8), rd32(v[i].second + 12));
}
static std::map<uint32_t, uint64_t> leaders;   // DB_ARMLEADERS: PCs reached by a control transfer (block starts)
static const char* leaders_file = getenv("DB_ARMLEADERS");
void dump_leaders() {
    if (!leaders_file) return;
    if (FILE* f = std::fopen(leaders_file, "w")) { for (auto& e : leaders) std::fprintf(f, "%06x %llu\n", e.first, (unsigned long long)e.second); std::fclose(f); }
}
static const bool rec_off = [] { const char* e = getenv("DB_ARMREC"); return e && e[0] == '0'; }();
// Idle skip (MANATEE.DRV main loop, opt-in): the driver's main loop at ARAM 0x194 polls one channel monitor
// per pass (ARAM 0xac0 cycles MSLC 0-63) and drains the SH-4 command ring and its own event queue. When a
// pass ends with both queues empty and no timer flag set, the rest of this sample's instruction budget would
// only poll more channels: the sample ends there. While the driver stays idle, a pass starts only every
// kIdlePasses samples (a pass is ~250 instructions, longer than one sample's budget).
constexpr uint32_t kIdleHead = 0x194;
constexpr uint32_t kIdlePasses = 4;   // while idle, one pass every 4 samples (each channel monitored every ~5.8 ms)
static uint32_t idle_wait;
bool idle_skip = getenv("DB_ARMIDLE") != nullptr;   // the Vita turns it on
uint64_t idle_skipped;
static inline uint32_t aram32(uint32_t a) { uint32_t v; std::memcpy(&v, aram + a, 4); return v; }
static bool driver_idle() {
    int& sig = idle_sig;
    if (sig < 0) sig = aram32(0x194) == 0xeb000249u && aram32(0x198) == 0xeb0007e1u && aram32(0x1c8) == 0x0a000002u &&
                       aram32(0x1e0) == 0x0affffebu && aram32(0x2188) == 0x0001242cu && aram32(0x218c) == 0x00012428u && aram32(0x1c58) == 0x00012004u;
    if (!sig) return false;
    return aram[0x228] == 0 && aram[0x22a] == 0 &&
           aram32(0x1242c) == aram32(0x12428) &&      // SH-4 -> driver command ring empty
           aram32(0x12000) == aram32(0x12004);        // driver event queue empty
}
// MANATEE.DRV voice allocator bug (0x16e8): r1 is pointed at its result slot (0x1808) only once some voice
// passes the first priority test (0x1738). When all 64 voices are busy and outrank the new sound, r1 is left at
// 0x1814 from the slot clearing, and 0x17f0 returns the word there as a voice: the caller then writes through it
// into the driver's own code and the sound CPU crashes (sound stops, then the game hangs or resets). With the
// ARM7 idle skip on (Vita) the driver reached that case within minutes. Pointing r1 at the cleared slot makes
// the allocator report "no voice", which the caller handles (that sound is not played).
constexpr uint32_t kAllocResult = 0x17f0;
static inline void alloc_fix() {
    if (alloc_sig < 0) alloc_sig = aram32(0x1700) == 0xe28f1c01u && aram32(0x1708) == 0xe4810004u && aram32(0x1738) == 0xe28f10c8u &&
                                   aram32(0x17f0) == 0xe5916000u && aram32(0x17f4) == 0xe5918004u;
    if (alloc_sig && r[1] != 0x1808) {
        static const bool watch = getenv("DB_ARMWATCH") != nullptr;
        if (watch) std::printf("[armwatch] vbl %u: no voice free, allocator result fixed (r1 %06x)\n", vblank_count, r[1]);
        r[1] = 0x1808;
    }
}
void run(int instrs) {
    if (!running) return;
    static uint32_t prev_next = ~0u;
    if (blocks_ok && !rec_off && !leaders_file && !armhist) {
        // translated blocks: a block runs only if it fits the remaining budget, so instruction counts (and with
        // them timing and FIQ points) are exactly the interpreter's
        for (int i = 0; i < instrs;) {
            if (fiq_line && !(cpsr & 0x40)) exception(0x1C, M_FIQ, r[15] + 4, true);
            uint32_t pc = r[15];
            if (pc == kIdleHead && idle_skip && (i > 0 || idle_wait > 0) && driver_idle()) {
                if (i > 0) idle_wait = kIdlePasses - 1; else --idle_wait;
                idle_skipped += instrs - i; return;
            }
            if (pc == kAllocResult) alloc_fix();   // no block starts here: the next step() runs it
            if (pc < kCodeEnd && blocks_ok) {
                const ArmBlock& b = kBlocks[pc >> 2];
                if (b.fn && b.len <= instrs - i) { i += b.fn(); continue; }
            }
            step(); ++i;
        }
        return;
    }
    for (int i = 0; i < instrs; ++i) {
        if (leaders_file) { if (r[15] != prev_next) leaders[r[15]]++; }
        if (fiq_line && !(cpsr & 0x40)) exception(0x1C, M_FIQ, r[15] + 4, true);
        if (leaders_file) prev_next = r[15] + 4;
        if (armhist) { pc_hist[r[15] & ~15u]++; uint32_t op_; std::memcpy(&op_, aram + (r[15] & 0x1FFFFC), 4); op_hist[op_class(op_)]++; }
        if (r[15] == kIdleHead && idle_skip && (i > 0 || idle_wait > 0) && driver_idle()) {   // same skip as the blocks path
            if (i > 0) idle_wait = kIdlePasses - 1; else --idle_wait;
            idle_skipped += instrs - i; return;
        }
        if (r[15] == kAllocResult) alloc_fix();
        step();
    }
}
uint32_t pc() { return r[15]; }

void state(SaveIO& s) {
    s.v(r); s.v(cpsr); s.v(spsr_fiq); s.v(spsr_irq); s.v(spsr_svc); s.v(spsr_abt); s.v(spsr_und);
    s.v(bank_usr); s.v(bank_fiq); s.v(bank_irq); s.v(bank_svc); s.v(bank_abt); s.v(bank_und);
    s.v(running); s.v(fiq_line); s.v(idle_wait);
    if (s.load) { idle_sig = -1; alloc_sig = -1; }
}
} // namespace arm
void arm_state(SaveIO& s) { arm::state(s); }
} // namespace hw
