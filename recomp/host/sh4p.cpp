// SH-4 on-chip side: P4 control registers, store queues, TMU, INTC, interrupt delivery,
// and the runtime hooks used by the translated code (dispatch, rte, set_sr, pref, ...).
#if !defined(DB_VITA) && defined(__linux__)
#include <execinfo.h>
#endif
#include "hw.h"
#include <cstdarg>
#include <cstring>
#include <map>
#include <cstdlib>

namespace hw { bool ta_unit_direct_ok(); uint32_t* ta_unit_slot(); void ta_unit_commit(); }
namespace sh4 { void (*lookup(uint32_t a))(Ctx*); }
namespace hw { void savestate_poll(sh4::Ctx* c); }
extern void (*poll_hook)();

namespace hw {

sh4::Ctx* cpu;
uint64_t now;
uint64_t sq_ta_count;
bool strict = false;
int trace_level = 0;

// ------------------------------------------------------------------ diagnostics
}
namespace sh4 { void dbg_print_stack(); extern uint32_t sq_mem[2][8]; }
namespace hw {
void fatal(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::fprintf(stderr, "FATAL: "); std::vfprintf(stderr, fmt, ap); std::fprintf(stderr, "\n");
    va_end(ap);
    if (cpu) {
        std::fprintf(stderr, "  cur_func=%08x pr=%08x sr=%08x t=%u vblank=%u time=%llu\n", cpu->cur_func, cpu->pr,
                     cpu->sr, cpu->t, vblank_count, (unsigned long long)now);
        for (int i = 0; i < 16; ++i) std::fprintf(stderr, "  r%-2d=%08x%s", i, cpu->r[i], (i % 4 == 3) ? "\n" : "");
    }
    sh4::dbg_print_stack();
#if !defined(DB_VITA) && defined(__linux__)
    { void* bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2); }   // host call chain (generated functions)
#endif
    std::fflush(stdout);
    std::exit(3);
}
void trace(const char* fmt, ...) {
    if (!trace_level) return;
    va_list ap; va_start(ap, fmt); std::vfprintf(stdout, fmt, ap); va_end(ap);
}
static std::map<std::pair<uint32_t, bool>, uint64_t> unhandled;
void log_unhandled(const char* what, uint32_t addr, uint32_t v, bool write) {
    auto& n = unhandled[{addr, write}];
    if (n++ == 0)
        std::printf("[unhandled] %s %08x%s%08x (func %08x)\n", what, addr, write ? " <- " : " ", v, cpu ? cpu->cur_func : 0);
    if (strict) fatal("unhandled MMIO %s %08x", what, addr);
}
size_t unhandled_count() { return unhandled.size(); }

// ------------------------------------------------------------------ scheduler
static uint64_t ev_time[EV_COUNT];
static bool ev_on[EV_COUNT];
void ev_fire(int id);   // holly.cpp / aica.cpp
void sched_add(int id, uint64_t when) { ev_time[id] = when; ev_on[id] = true; }
void sched_cancel(int id) { ev_on[id] = false; }
void sched_run() {
    for (bool again = true; again;) {
        again = false;
        for (int i = 0; i < EV_COUNT; ++i)
            if (ev_on[i] && ev_time[i] <= now) { ev_on[i] = false; ev_fire(i); again = true; }
    }
}
static uint64_t next_event() {
    uint64_t t = ~0ull;
    for (int i = 0; i < EV_COUNT; ++i) if (ev_on[i] && ev_time[i] < t) t = ev_time[i];
    return t;
}

// ------------------------------------------------------------------ P4 registers
static std::map<uint32_t, uint32_t> p4regs_far;   // P4 registers outside the flat table (rare)
static uint32_t p4flat[256 * 256];                  // [area byte (a >> 16)][word offset < 0x400]
static inline uint32_t& p4ref(uint32_t a) {
    if ((a & 0xFC00) == 0) return p4flat[((a >> 16) & 0xFF) << 8 | ((a >> 2) & 0xFF)];
    return p4regs_far[a];
}
static uint32_t (&sq)[2][8] = sh4::sq_mem;   // store queues (sh4ctx.h: written inline by wr32)
static uint32_t intevt_reg, expevt_reg;
static uint32_t pctra, pdtra;
static uint32_t rfcr;

// TMU
struct Tmu { uint32_t tcor = 0xFFFFFFFF, tcnt = 0xFFFFFFFF, tcr = 0; uint64_t last = 0; double frac = 0; };
static Tmu tmu[3];
static uint32_t tstr;
static const int tmu_div[8] = {4, 16, 64, 256, 1024, 1024, 1024, 1024};
static void tmu_update() {
    for (int i = 0; i < 3; ++i) {
        Tmu& t = tmu[i];
        if (!(tstr & (1u << i))) { t.last = now; continue; }
        uint64_t pticks = (now - t.last) / 4;         // P-clock = CPU / 4
        t.last += pticks * 4;
        uint64_t div = tmu_div[t.tcr & 7];
        t.frac += (double)pticks / div;
        uint64_t steps = (uint64_t)t.frac; t.frac -= steps;
        while (steps) {
            if (steps <= t.tcnt) { t.tcnt -= (uint32_t)steps; break; }
            steps -= (uint64_t)t.tcnt + 1;
            t.tcnt = t.tcor;
            t.tcr |= 0x100;                              // UNF
            if (t.tcr & 0x20) sh4_raise_internal(0x400 + 0x20 * i);
            if (t.tcor == 0) break;
        }
    }
}

// INTC
static uint32_t ipra, iprb, iprc, icr;
static std::map<uint32_t, bool> internal_pending;
void sh4_raise_internal(uint32_t ev) { internal_pending[ev] = true; if (cpu) cpu->irq_pending = 1; }
void sh4_clear_internal(uint32_t ev) { internal_pending.erase(ev); }
static int internal_prio(uint32_t ev) {
    if (ev == 0x400) return (ipra >> 12) & 15;
    if (ev == 0x420) return (ipra >> 8) & 15;
    if (ev == 0x440 || ev == 0x460) return (ipra >> 4) & 15;
    if (ev >= 0x480 && ev <= 0x4C0) return ipra & 15;
    if (ev >= 0x640 && ev <= 0x6C0) return (iprc >> 8) & 15;
    if (ev >= 0x700 && ev <= 0x760) return (iprc >> 4) & 15;
    return 0;
}

static bool p4_known(uint32_t a) {
    static const uint32_t known[] = {
        0xFF000000, 0xFF000004, 0xFF000008, 0xFF00000C, 0xFF000010, 0xFF000014, 0xFF000018, 0xFF00001C,
        0xFF000020, 0xFF000034, 0xFF000038, 0xFF00003C, 0xFF000030, 0xFF000044, 0xFF000048,
        0xFF200000, 0xFF200004, 0xFF200008, 0xFF20000C, 0xFF200010, 0xFF200014, 0xFF200018, 0xFF20001C, 0xFF200020,
        0xFF800000, 0xFF800004, 0xFF800008, 0xFF80000C, 0xFF800010, 0xFF800014, 0xFF800018, 0xFF80001C,
        0xFF800020, 0xFF800024, 0xFF800034, 0xFF800038, 0xFF80003C, 0xFF800040, 0xFF800044, 0xFF900000,
        0xFFC00000, 0xFFC00004, 0xFFC00008, 0xFFC0000C, 0xFFC00010,
        0xFFE80000, 0xFFE80004, 0xFFE80008, 0xFFE8000C, 0xFFE80018, 0xFFE80020, 0xFFE80024,
        0xFFA00000, 0xFFA00004, 0xFFA00008, 0xFFA0000C, 0xFFA00010, 0xFFA00014, 0xFFA00018, 0xFFA0001C,
        0xFFA00020, 0xFFA00024, 0xFFA00028, 0xFFA0002C, 0xFFA00030, 0xFFA00034, 0xFFA00038, 0xFFA0003C, 0xFFA00040,
        0xFFC80000, 0xFFC80004, 0xFFC80008, 0xFFC8000C, 0xFFC80010, 0xFFC80014, 0xFFC80018, 0xFFC8001C,
        0xFFC80020, 0xFFC80024, 0xFFC80028, 0xFFC8002C, 0xFFC80030, 0xFFC80034, 0xFFC80038, 0xFFC8003C,
        0xFFD00010, 0xFFD80000};
    for (uint32_t k : known) if (k == a) return true;
    if (a >= 0xFF900000 && a < 0xFFA00000) return true;   // SDMR2/3 (SDRAM mode set by write address)
    return false;
}

uint32_t p4_read(uint32_t a, int size) {
    if (a >= 0xE0000000u && a < 0xE4000000u) return sq[(a >> 5) & 1][(a >> 2) & 7];
    // Operand-cache address array reads: no valid (V=0) / no dirty (U=0) lines, since the host has no cache.
    if (a >= 0xF4000000u && a < 0xF6000000u) return 0;
    if (a < 0xFF000000u) { log_unhandled("P4 cache-array read", a, 0, false); return 0; }
    switch (a) {
    case 0xFF000024: return expevt_reg;
    case 0xFF000028: return intevt_reg;
    case 0xFF800028: return (rfcr += 17) & 0x3FF;                 // refresh count: advancing
    case 0xFF80002C: return pctra;
    case 0xFF800030: {                                            // PDTRA: cable / port bits
        uint32_t v = 0;
        if ((pctra & 0xF) == 0x8 || (pctra & 0xF) == 0xB) v = 3;
        if ((pctra & 0xF) == 0xB && (pdtra & 0xF) == 2) v = 0;
        else if ((pctra & 0xF) == 0xC && (pdtra & 0xF) == 2) v = 3;
        v |= 0u << 8;                                             // cable type 0 = VGA
        return v;
    }
    case 0xFFD00000: return icr;
    case 0xFFD00004: return ipra;
    case 0xFFD00008: return iprb;
    case 0xFFD0000C: return iprc;
    case 0xFFD80004: tmu_update(); return tstr;
    case 0xFFE80010: return 0x60;                                 // SCFSR2: TDFE|TEND
    case 0xFFE80014: return 0;
    case 0xFFE8001C: return 0;                                    // SCFDR2
    }
    if (a >= 0xFFD80008 && a <= 0xFFD8002C) {
        tmu_update();
        int ch = (a - 0xFFD80008) / 12, r = ((a - 0xFFD80008) % 12) / 4;
        if (ch > 2) return p4ref(a);
        return r == 0 ? tmu[ch].tcor : r == 1 ? tmu[ch].tcnt : tmu[ch].tcr;
    }
    if (!p4_known(a)) log_unhandled("P4 read", a, 0, false);
    return p4ref(a);
}

void p4_write(uint32_t a, uint32_t v, int size) {
    if (a >= 0xE0000000u && a < 0xE4000000u) { sq[(a >> 5) & 1][(a >> 2) & 7] = v; return; }
    // Operand-cache address/data arrays (0xF4000000-0xF5FFFFFF): the game invalidates the cache through
    // the address array (syCache). The host has no cache, so these writes are architecturally no-ops.
    if (a >= 0xF4000000u && a < 0xF6000000u) return;
    if (a < 0xFF000000u) { log_unhandled("P4 cache-array write", a, v, true); return; }
    switch (a) {
    case 0xFF000024: expevt_reg = v; return;
    case 0xFF000028: intevt_reg = v; return;
    case 0xFF80002C: pctra = v; return;
    case 0xFF800030: pdtra = v; return;
    case 0xFFD00000: icr = v; return;
    case 0xFFD00004: ipra = v & 0xFFFF; return;
    case 0xFFD00008: iprb = v & 0xFFFF; return;
    case 0xFFD0000C: iprc = v & 0xFFFF; return;
    case 0xFFD80004: tmu_update(); tstr = v & 7; for (auto& t : tmu) t.last = now; return;
    case 0xFFE8000C: std::putchar((int)(v & 0xFF)); return;      // SCIF transmit -> stdout
    }
    if (a >= 0xFFD80008 && a <= 0xFFD8002C) {
        tmu_update();
        int ch = (a - 0xFFD80008) / 12, r = ((a - 0xFFD80008) % 12) / 4;
        if (ch > 2) { p4ref(a) = v; return; }
        if (r == 0) tmu[ch].tcor = v;
        else if (r == 1) tmu[ch].tcnt = v;
        else {
            // UNF is cleared by writing 0
            uint32_t unf = (tmu[ch].tcr & 0x100) & (v & 0x100);
            tmu[ch].tcr = (v & 0x2FF) | unf;
            if (!(tmu[ch].tcr & 0x100)) sh4_clear_internal(0x400 + 0x20 * ch);
        }
        return;
    }
    if (!p4_known(a)) log_unhandled("P4 write", a, v, true);
    p4ref(a) = v;
}
uint32_t dmac_reg(uint32_t a) { return p4ref(a); }
void dmac_set(uint32_t a, uint32_t v) { p4ref(a) = v; }

void sh4p_init() {
    ipra = iprb = iprc = icr = 0;
    p4ref(0xFF00001C) = 0;   // CCR
    p4ref(0xFFA00040) = 0;   // DMAOR
}

// ------------------------------------------------------------------ interrupt delivery

static void take_interrupt(sh4::Ctx* c, uint32_t ev) {
    intevt_reg = ev;
    c->ssr = sh4::get_sr(c);
    c->spc = 0xDEAD0000u;
    c->sgr = c->r[15];
    sh4::set_sr(c, sh4::get_sr(c) | sh4::SR_MD | sh4::SR_BL | sh4::SR_RB);
    // VBR+0x600 stub installed by the game (copied from image 0x8C329780, literals patched at install):
    //   push r1; save r0 to the stub's slot; store INTEVT to the variable named by its literal;
    //   reload r0; jump to the handler named by its literal.
    uint32_t stub = c->vbr + 0x600;
    uint32_t slot = stub + (0x8C3297D0u - 0x8C329780u);
    uint32_t var = sh4::rd32(stub + (0x8C3297CCu - 0x8C329780u));
    uint32_t target = sh4::rd32(stub + (0x8C3297D8u - 0x8C329780u));
    if (sh4::rd16(stub + 0x26) != 0x2F16 /* mov.l r1,@-r15 */ || sh4::rd16(stub + 0x40) != 0x412B /* jmp @r1 */)
        fatal("VBR+0x600 stub not the expected Katana/SystemX stub (vbr=%08x)", c->vbr);
    c->r[15] -= 4; sh4::wr32(c->r[15], c->r[1]);
    sh4::wr32(slot, c->r[0]);
    sh4::wr32(var, ev);
    c->r[1] = target;
    // r0 unchanged (reloaded from slot)
    sh4::dispatch(c, target);
}

static uint64_t irq_counts[0x800 / 0x20];
uint64_t interrupts_taken;   // all interrupts delivered to the guest (idle-loop skip)
uint64_t irq_count(uint32_t ev) { return irq_counts[(ev >> 5) & 63]; }

static void check_interrupts(sh4::Ctx* c) {
    c->irq_pending = 0;
    if (c->sr & sh4::SR_BL) return;
    int imask = (c->sr >> 4) & 15;
    uint32_t best_ev = 0; int best = 0;
    uint32_t irl_ev = 0;
    int lvl = holly_irl_level(&irl_ev);
    if (lvl > imask) { best = lvl; best_ev = irl_ev; }
    for (auto& kv : internal_pending) {
        int p = internal_prio(kv.first);
        if (p > imask && p > best) { best = p; best_ev = kv.first; }
    }
    if (!best_ev) return;
    // internal interrupt sources are level-type: TMU stays pending until UNF cleared
    irq_counts[(best_ev >> 5) & 63]++; interrupts_taken++;
    take_interrupt(c, best_ev);
}

void sh4_state(SaveIO& s) {
    s.raw((void*)cpu, sizeof *cpu);
    s.v(now); s.v(sq_ta_count); s.v(ev_time); s.v(ev_on); s.v(p4flat);
    uint32_t n = (uint32_t)p4regs_far.size(); s.v(n);
    if (s.load) { p4regs_far.clear(); for (uint32_t i = 0; i < n; ++i) { uint32_t k, val; s.v(k); s.v(val); p4regs_far[k] = val; } }
    else for (auto& kv : p4regs_far) { uint32_t k = kv.first, val = kv.second; s.v(k); s.v(val); }
    s.v(intevt_reg); s.v(expevt_reg); s.v(pctra); s.v(pdtra); s.v(rfcr); s.v(tmu); s.v(tstr);
    s.v(ipra); s.v(iprb); s.v(iprc); s.v(icr); s.v(interrupts_taken); s.v(sh4::sq_mem);
}
} // namespace hw

// ====================================================================== runtime hooks
namespace sh4 {
using namespace hw;

uintptr_t stack_base, stack_max_depth;   // diagnostics: deepest host stack seen at a poll
void poll_irq(Ctx* c) {
    { char probe; uintptr_t d = stack_base - (uintptr_t)&probe; if (stack_base && d > stack_max_depth) stack_max_depth = d; }
    if (c->slice <= 0) {
        now += (uint64_t)POLL_CYCLES * SLICE_POLLS;
        c->slice = SLICE_POLLS;
        tmu_update();
        sched_run();
        hw::aica_update();
        if (poll_hook) poll_hook();
    }
    check_interrupts(c);
}

void set_sr(Ctx* c, uint32_t v) {
    uint32_t old = c->sr;
    if ((v ^ old) & SR_RB)
        for (int i = 0; i < 8; ++i) { uint32_t t = c->r[i]; c->r[i] = c->rbank[i]; c->rbank[i] = t; }
    c->t = v & 1;
    c->sr = v & 0x700083F2u;
    if ((old & (SR_BL | SR_IMASK)) != (c->sr & (SR_BL | SR_IMASK))) c->irq_pending = 1;
}

void rte(Ctx* c) {
    set_sr(c, c->ssr);
    c->irq_pending = 1;
}

// direct-mapped cache of resolved targets (HLE hook or translated function); hle_register clears it
struct DispSlot { uint32_t addr; bool hle; void* fn; };
static DispSlot disp_cache[4096];
void dispatch_cache_clear() { std::memset(disp_cache, 0, sizeof disp_cache); }
// Idle-loop skip: the frame loop in f_8c320140 spins calling an empty callback (0x8C31E9B0, through a
// function pointer) until an interrupt handler updates its flags. Instead of iterating, run whole scheduler
// slices (timers, events, AICA, pacing) until an interrupt is taken - where the spinning loop would take it,
// since interrupts only become pending at slice boundaries. DB_NOIDLESKIP=1 disables it.
uint64_t idle_slices_skipped;
using hw::interrupts_taken;
static void idle_skip(Ctx* c) {
    hw::savestate_poll(c);   // save states are taken / restored only here (see savestate.cpp)
    static const bool idlelog = getenv("DB_IDLELOG") != nullptr;
    if (idlelog) { static uint64_t n; if ((n++ % 600) == 0) { char probe_; std::printf("[idle] vbl %u r15 %08x pr %08x host_sp_depth %zu\n", hw::vblank_count, c->r[15], c->pr, (size_t)(stack_base - (uintptr_t)&probe_)); } }
    uint64_t before = interrupts_taken;
    for (int n = 0; n < 4096 && interrupts_taken == before; ++n) {
        c->slice = 0;
        poll_irq(c);
        idle_slices_skipped++;
    }
}
// One whole scheduler slice for a guest busy-wait on a timer (see f_8c2f0ece in native_vtx.cpp): emulated time
// advances by a slice per poll instead of by the few cycles of one loop pass. false when DB_NOIDLESKIP is set.
bool idle_wait_slice(Ctx* c) {
    static const bool no_idle_skip = getenv("DB_NOIDLESKIP") != nullptr;
    if (no_idle_skip) return false;
    c->slice = 0;
    poll_irq(c);
    idle_slices_skipped++;
    return true;
}
void dispatch(Ctx* c, uint32_t target) {
    static const bool no_idle_skip = getenv("DB_NOIDLESKIP") != nullptr;
    if (target == 0x8c31e9b0u && (c->pr == 0x8c3202a0u || c->pr == 0x8c3202e0u) && !no_idle_skip) { idle_skip(c); return; }
    static const bool loopdbg = getenv("DB_LOOPDBG") != nullptr;   // which callbacks the frame-wait loop dispatches
    if (loopdbg && c->pr >= 0x8c320140u && c->pr < 0x8c320400u) { static std::map<uint64_t, uint64_t> m; static uint64_t n;
        m[((uint64_t)target << 32) | c->pr]++;
        if (++n % 2000 == 0) { for (auto& e : m) std::printf("[loop] target %08x pr %08x: %llu\n", (uint32_t)(e.first >> 32), (uint32_t)e.first, (unsigned long long)e.second); } }
    DispSlot& d = disp_cache[(target >> 1) & 4095];
    if (d.fn && d.addr == target) { if (d.hle) ((HleFn)d.fn)(c); else ((void (*)(Ctx*))d.fn)(c); return; }
    if (HleFn h = hle_lookup(target)) { d = {target, true, (void*)h}; h(c); return; }
    if (auto f = lookup(target)) { d = {target, false, (void*)f}; f(c); return; }
    fatal("dispatch: unknown target %08x (from func %08x, pr=%08x)", target, c->cur_func, c->pr);
}

void fatal_jump(Ctx* c, uint32_t from, uint32_t target) {
    fatal("jump table at %08x: target %08x not among proven cases", from, target);
}

uint32_t qacr(int q) { return p4ref(0xFF000038 + 4 * (q & 1)); }
// true if a flush of the store queue at SQ address addr goes to the TA polygon path (QACR mapping)
bool sq_to_ta_poly(uint32_t addr) {
    uint32_t qacr = p4ref(0xFF000038 + 4 * ((addr >> 5) & 1));
    uint32_t p = (((qacr & 0x1C) << 24) | (addr & 0x03FFFFE0)) & 0x1FFFFFFF;
    return p >= 0x10000000 && p < 0x10800000;
}
void pref_sq(Ctx* c, uint32_t addr) {
    int q = (addr >> 5) & 1;
    uint32_t qacr = p4ref(0xFF000038 + 4 * q);
    uint32_t ext = ((qacr & 0x1C) << 24) | (addr & 0x03FFFFE0);
    uint32_t p = ext & 0x1FFFFFFF;
    static int logged;
    static const bool sqlog = getenv("DB_SQLOG") != nullptr;
    if (sqlog && logged < 40 && (p >> 26) != 4) { logged++; std::printf("[sq] pref q%d -> %08x  w0..7 %08x %08x %08x %08x %08x %08x %08x %08x\n", q, ext,
        sq[q][0], sq[q][1], sq[q][2], sq[q][3], sq[q][4], sq[q][5], sq[q][6], sq[q][7]); }
    if ((p >> 26) == 4) {
        if (p < 0x10800000 && hw::ta_unit_direct_ok()) {   // TA polygon path, no partial unit: one 32-byte unit
            std::memcpy(hw::ta_unit_slot(), sq[q], 32); hw::ta_unit_commit();   // (ta_unit_commit counts sq_ta_count)
            return;
        }
        hw::sq_ta_count++; ta_write(p, sq[q], 8); return;
    }
    if ((p >> 26) == 3) { std::memcpy(hw::ram + (p & 0xFFFFE0), sq[q], 32); return; }   // SQ copy to main RAM (hot)
    for (int i = 0; i < 8; ++i) hw::bus_write32(ext + 4 * i, sq[q][i]);
}

void trapa(Ctx* c, uint32_t imm, uint32_t pc) { fatal("trapa #%x at %08x", imm, pc); }
void sleep(Ctx* c, uint32_t pc) {
    // idle until the next scheduled event
    uint64_t t = next_event();
    if (t != ~0ull && t > now) now = t;
    c->slice = 0;
    poll_irq(c);
}
void ldtlb(Ctx* c) { log_unhandled("ldtlb", 0, 0, true); }

} // namespace sh4

uintptr_t stack_max_depth_get() { return sh4::stack_max_depth; }
void stack_base_set(uintptr_t b) { sh4::stack_base = b; }
