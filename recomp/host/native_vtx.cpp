// Native ports of hot translated functions (they override the weak generated versions at link time).
// Each port keeps the guest-visible behaviour of the translation exactly: memory writes (including stack
// slots below SP), store-queue contents and flushes, the registers at every poll point and at return, and
// the poll points themselves (interrupts can only be taken there). Verified with DB_TAHASH runs.
#include "sh4ctx.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <vector>

namespace hw {
extern bool ta_skip_list;
bool ta_unit_direct_ok(); uint32_t* ta_unit_slot(); void ta_unit_commit(); size_t ta_stream_len(); const uint32_t* ta_stream_at(size_t off);
bool ta_fast_ok(uint32_t pcw_a, uint32_t pcw_b); uint32_t* ta_fast_reserve(size_t units, size_t* off); void ta_fast_commit(size_t units);
}
namespace sh4 { uint32_t qacr(int q); }
uint64_t (*nv_clock)();   // Vita: microsecond clock for per-function timing (null on desktop)
uint64_t nv_time[4], nv_units[2];   // [b2a0 full, b2a0 skip, ba84, b710] microseconds; b2a0 [full, skip] TA units
struct NvTimer {
    uint64_t* acc; uint64_t t0;
    explicit NvTimer(uint64_t* a) : acc(nv_clock ? a : nullptr), t0(acc ? nv_clock() : 0) {}
    ~NvTimer() { if (acc) *acc += nv_clock() - t0; }
};
uint64_t nv_count[3][2];   // [b2a0, ba84, b710][calls, vertices] (DB_VTXSTAT, desktop)

namespace {
inline float f32(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint32_t u32(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
// one 32-byte store-queue unit at SQ address a (words in address order), as the four paired fmov stores write it
inline void sq_unit(uint32_t a, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, uint32_t w4, uint32_t w5, uint32_t w6, uint32_t w7) {
    if ((a >> 26) == 0x38) {
        uint32_t* q = sh4::sq_mem[(a >> 5) & 1];
        q[0] = w0; q[1] = w1; q[2] = w2; q[3] = w3; q[4] = w4; q[5] = w5; q[6] = w6; q[7] = w7;
    } else {
        ::sh4::wr32(a, w0); ::sh4::wr32(a + 4, w1); ::sh4::wr32(a + 8, w2); ::sh4::wr32(a + 12, w3);
        ::sh4::wr32(a + 16, w4); ::sh4::wr32(a + 20, w5); ::sh4::wr32(a + 24, w6); ::sh4::wr32(a + 28, w7);
    }
}
inline int32_t s16(uint32_t a) { return (int16_t)::sh4::rd16(a); }

// Unit emitter: when the store-queue address maps to the TA polygon path (QACR), the unit is built directly in
// the TA stream (identical to an SQ write + flush); otherwise it goes through the store queue and pref.
// sync_sq() leaves each store queue holding its last unit, as the hardware path would.
struct UnitEmitter {
    uint32_t base[2]; bool direct; size_t last_off[2]; bool have[2];
    // bulk mode (ta_fast_*): units go into reserved TA buffer space [blk, blk_end) and are committed in batches
    uint32_t pcw_a = 0, pcw_b = 0; bool fast = false;
    uint32_t *blk = nullptr, *out = nullptr, *blk_end = nullptr; size_t blk_off = 0;
    void refresh() {   // QACR can only change at a poll (interrupt handler)
        base[0] = (sh4::qacr(0) & 0x1C) << 24; base[1] = (sh4::qacr(1) & 0x1C) << 24;
        direct = hw::ta_unit_direct_ok();
        fast = pcw_a && hw::ta_fast_ok(pcw_a, pcw_b);
    }
    void begin(uint32_t pa = 0, uint32_t pb = 0) { have[0] = have[1] = false; pcw_a = pa; pcw_b = pb; refresh(); }
    void flush() {   // commit the bulk units written so far
        if (out != blk) { hw::ta_fast_commit((size_t)(out - blk) / 8); blk_off += (size_t)(out - blk); blk = out; }
    }
    void release() { flush(); blk = out = blk_end = nullptr; }   // reserved space is only valid until the next TA write
    __attribute__((always_inline)) void emit(sh4::Ctx* c, uint32_t a, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, uint32_t w4, uint32_t w5, uint32_t w6, uint32_t w7) {
        uint32_t q = (a >> 5) & 1;
        uint32_t p = (base[q] | (a & 0x03FFFFE0)) & 0x1FFFFFFF;
        bool to_ta = (a >> 26) == 0x38 && p >= 0x10000000 && p < 0x10800000;
        if (fast && to_ta) {
            if (__builtin_expect(out == blk_end, 0)) { flush(); out = blk = hw::ta_fast_reserve(256, &blk_off); blk_end = blk + 256 * 8; }
            uint32_t* __restrict o = out;   // (-fno-strict-aliasing: without restrict every store reloads this object's fields)
            __builtin_prefetch(o + 32, 1);
            o[0] = w0; o[1] = w1; o[2] = w2; o[3] = w3; o[4] = w4; o[5] = w5; o[6] = w6; o[7] = w7;
            last_off[q] = blk_off + (size_t)(o - blk); have[q] = true;
            out = o + 8;
            return;
        }
        emit_slow(c, a, q, to_ta, w0, w1, w2, w3, w4, w5, w6, w7);
    }
    __attribute__((noinline)) void emit_slow(sh4::Ctx* c, uint32_t a, uint32_t q, bool to_ta, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3, uint32_t w4, uint32_t w5, uint32_t w6, uint32_t w7) {
        release();
        if (direct && to_ta) {
            uint32_t* u = hw::ta_unit_slot();
            u[0] = w0; u[1] = w1; u[2] = w2; u[3] = w3; u[4] = w4; u[5] = w5; u[6] = w6; u[7] = w7;
            last_off[q] = hw::ta_stream_len(); have[q] = true;
            hw::ta_unit_commit();
            return;
        }
        sync_sq();
        sq_unit(a, w0, w1, w2, w3, w4, w5, w6, w7);
        ::sh4::pref(c, a);
        direct = hw::ta_unit_direct_ok();
        fast = pcw_a && hw::ta_fast_ok(pcw_a, pcw_b);
    }
    void sync_sq() {
        release();
        for (int q = 0; q < 2; ++q) if (have[q]) { std::memcpy(sh4::sq_mem[q], hw::ta_stream_at(last_off[q]), 32); have[q] = false; }
    }
};
// FAST: the caller proved the addresses are main RAM, so read it directly (no MMIO fallback in the code path)
// the same with the RAM base held in a local (the global sh4::ram is reloaded after every store otherwise)
template <bool FAST> inline int32_t S16b(const uint8_t* rb, uint32_t a) {
    if (FAST) { int16_t v; std::memcpy(&v, rb + (a & 0xFFFFFF), 2); return v; }
    return (int16_t)::sh4::rd16(a);
}
template <bool FAST> inline uint32_t RD32b(const uint8_t* rb, uint32_t a) {
    if (FAST) { uint32_t v; std::memcpy(&v, rb + (a & 0xFFFFFF), 4); return v; }
    return ::sh4::rd32(a);
}
template <bool FAST> inline int32_t S16(uint32_t a) {
    if (FAST) { int16_t v; std::memcpy(&v, sh4::ram + (a & 0xFFFFFF), 2); return v; }
    return (int16_t)::sh4::rd16(a);
}
template <bool FAST> inline uint32_t RD32(uint32_t a) {
    if (FAST) { uint32_t v; std::memcpy(&v, sh4::ram + (a & 0xFFFFFF), 4); return v; }
    return ::sh4::rd32(a);
}
// Cache prefetch of guest RAM (FAST paths only; the address is masked into the 16 MB buffer, so a guess past the
// end of a table is harmless). The guest's own pref hints are no-ops here and the A9 has no L1 stream prefetcher,
// so without these every new 32-byte line of a vertex stream stalls on main memory.
template <bool FAST> inline void PF(uint32_t a) { if (FAST) __builtin_prefetch(sh4::ram + (a & 0xFFFFFF)); }
template <bool FAST> inline void PFW(uint32_t a) { if (FAST) __builtin_prefetch(sh4::ram + (a & 0xFFFFFF), 1); }
// RAM address a store-queue flush of SQ address a lands at (0 when it is not main RAM)
inline uint32_t sq_ram_target(uint32_t a) {
    if ((a >> 26) != 0x38) return a;
    uint32_t p = (((sh4::qacr((a >> 5) & 1) & 0x1C) << 24) | (a & 0x03FFFFE0)) & 0x1FFFFFFF;
    return (p >> 26) == 3 ? p : 0;
}
// a RAM range [a - before, a + after] that stays inside area 3 (so masking to 16 MB matches is_ram's mirror)
inline bool ram_window(uint32_t a, uint32_t before, uint32_t after) {
    return ::sh4::is_ram(a) && ::sh4::is_ram(a - before) && ::sh4::is_ram(a + after) &&
           ((a - before) & 0xFC000000u) == (a & 0xFC000000u) && ((a + after) & 0xFC000000u) == (a & 0xFC000000u);
}
} // namespace

// Register file at a poll point / at return. Macros rather than lambdas: capturing the locals by reference
// would force them all into memory.
#define NV_SYNC() do { \
        c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->r[4] = R4; c->r[5] = R5; c->r[6] = R6; c->r[7] = R7; \
        c->r[8] = R8; c->r[9] = R9; c->r[10] = R10; c->r[15] = R15; c->t = t; c->fpul = fpul; \
        c->fr.u[0][0] = F0; c->fr.u[0][1] = F1; c->fr.u[0][2] = F2; c->fr.u[0][3] = F3; c->fr.u[0][5] = F5; \
        c->fr.f[0][6] = F6; c->fr.f[0][7] = F7; c->fr.u[0][8] = F8; c->fr.f[0][9] = F9; c->fr.u[0][10] = F10; c->fr.f[0][11] = F11; \
    } while (0)
#define NV_RELOAD() do { \
        R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; R4 = c->r[4]; R5 = c->r[5]; R6 = c->r[6]; R7 = c->r[7]; \
        R8 = c->r[8]; R9 = c->r[9]; R10 = c->r[10]; R15 = c->r[15]; t = c->t; fpul = c->fpul; \
        F0 = c->fr.u[0][0]; F1 = c->fr.u[0][1]; F2 = c->fr.u[0][2]; F3 = c->fr.u[0][3]; \
        F6 = c->fr.f[0][6]; F7 = c->fr.f[0][7]; F8 = c->fr.u[0][8]; F9 = c->fr.f[0][9]; \
    } while (0)
// SKIP walks do not load the vertices; where the registers become visible (polls, return) give them the values
// the full path has there: the current vertex in F1-F3/F8, its scaled u/v in F6/F7, and F9 = F8 (-1.0 once emitted)
#define SKIP_FIX_F(emitted) do { if (SKIP) { F1 = RD32<FAST>(va); F2 = RD32<FAST>(va + 4); F3 = RD32<FAST>(va + 8); F8 = RD32<FAST>(va + 12); \
        F6 = (float)i6 * F4; F7 = (float)i7 * F4; F9 = (emitted) ? f32(F8) - F11 : f32(F8); } } while (0)
#define NV_POLL(emitted) do { if (--c->slice <= 0 || c->irq_pending) { em.sync_sq(); SKIP_SQ_SYNC(); SKIP_FIX_F(emitted); NV_SYNC(); ::sh4::poll_irq(c); NV_RELOAD(); em.refresh(); c->cur_func = 0x8c31b2a0u; } } while (0)

// 0x8C31B2A0: sprite-strip vertex writer. Per strip: a signed length and (index, u, v) triples; each vertex is
// [PCW, x, y, z, u*scale, v*scale, colour, colour-1.0] from the 16-byte vertex at r5 + index*16, written to the
// store queue at r7 and flushed; the last vertex of a strip uses the end-of-strip PCW.
template <bool FAST, bool SKIP> static void vtx_8c31b2a0(sh4::Ctx* c) {
    const uint8_t* const rb = sh4::ram;
    uint32_t R0, R1, R2, R3, R4 = c->r[4], R5 = c->r[5], R6 = c->r[6], R7 = c->r[7], R8 = c->r[8], R9 = c->r[9], R10 = c->r[10], R15 = c->r[15];
    uint32_t F0 = c->fr.u[0][0], F1 = c->fr.u[0][1], F2 = c->fr.u[0][2], F3 = c->fr.u[0][3], F8 = c->fr.u[0][8];
    float F6 = c->fr.f[0][6], F7 = c->fr.f[0][7], F9 = c->fr.f[0][9];
    const float F4 = c->fr.f[0][4];
    uint32_t t, fpul = c->fpul;
    // push r10, r9, r8
    R15 -= 4; ::sh4::wr32(R15, R10);
    R15 -= 4; ::sh4::wr32(R15, R9);
    R15 -= 4; ::sh4::wr32(R15, R8);
    R3 = R6 & ::sh4::rd32(0x8c31b3a0u);
    R6 >>= 14;                                           // shld r2(-14), r6
    R2 = 3; R1 = 0;
    R6 &= R2;
    const uint32_t F5 = ::sh4::rd32(0x8c31b3a4u);      // vertex PCW
    t = R6 >> 31; R6 <<= 1;
    const uint32_t F10 = ::sh4::rd32(0x8c31b3a8u);     // end-of-strip PCW
    R0 = 0;
    const float F11 = 1.0f;
    UnitEmitter em; em.begin(F5, F10);
    // SKIP: the store queues must end up holding the units the full path would leave (other writers reuse
    // stale SQ words), so remember the inputs of the last unit per queue and build only those
    int32_t i6 = 0, i7 = 0; uint32_t va = 0;
    struct LastUnit { bool have; uint32_t pcw, va; int32_t i6, i7; } lastu[2] = {{false, 0, 0, 0, 0}, {false, 0, 0, 0, 0}};
    auto rec = [&lastu](uint32_t a, uint32_t pcw, uint32_t va_, int32_t i6_, int32_t i7_) { lastu[(a >> 5) & 1] = {true, pcw, va_, i6_, i7_}; };
    (void)rec; (void)i6; (void)i7; (void)va;
#define SKIP_SQ_SYNC() do { if (SKIP) for (int q_ = 0; q_ < 2; ++q_) if (lastu[q_].have) { \
        const LastUnit& L = lastu[q_]; uint32_t* qq = sh4::sq_mem[q_]; \
        uint32_t c8 = RD32b<FAST>(rb, L.va + 12); \
        qq[0] = L.pcw; qq[1] = RD32b<FAST>(rb, L.va); qq[2] = RD32b<FAST>(rb, L.va + 4); qq[3] = RD32b<FAST>(rb, L.va + 8); \
        qq[4] = u32((float)L.i6 * F4); qq[5] = u32((float)L.i7 * F4); qq[6] = c8; qq[7] = u32(f32(c8) - F11); lastu[q_].have = false; } } while (0)
strip:                                                   // L_8c31b2c2
    R2 = (uint32_t)S16b<FAST>(rb, R4); R4 += 2;
    R8 = (uint32_t)S16b<FAST>(rb, R4) << 4; R4 += 2;
    R9 = (uint32_t)S16b<FAST>(rb, R4); R4 += 2;
    fpul = R9;
    t = (int32_t)R2 >= 0;
    if (!SKIP) F6 = (float)(int32_t)fpul; else i6 = (int32_t)fpul;
    R8 += R5;
    if (!SKIP) F6 = F6 * F4;
    if (!t) R2 = 0u - R2;
    R9 = (uint32_t)S16b<FAST>(rb, R4); R4 += 2;                     // L_8c31b2dc
    R0 += R2;
    fpul = R9;
    if (!SKIP) { F1 = RD32b<FAST>(rb, R8); F7 = (float)(int32_t)fpul; F2 = RD32b<FAST>(rb, R8 + 4); F7 = F7 * F4; F3 = RD32b<FAST>(rb, R8 + 8); F8 = RD32b<FAST>(rb, R8 + 12); } else { i7 = (int32_t)fpul; va = R8; }
    R8 += 16;
    R1 += R2;
    R2 += 0xfffffffeu;
    R8 = ((uint32_t)S16b<FAST>(rb, R4) << 4) + R5; R4 += 2;
    if (FAST && !SKIP) __builtin_prefetch(sh4::ram + (R8 & 0xFFFFFF));   // the guest's pref of the next vertex
    F0 = F5;
    if (!SKIP) { F9 = f32(F8) - F11; em.emit(c, R7, F0, F1, F2, F3, u32(F6), u32(F7), F8, u32(F9)); } else rec(R7, F0, va, i6, i7);
    R9 = (uint32_t)S16b<FAST>(rb, R4); R4 += 2;
    fpul = R9;
    if (!SKIP) { F6 = (float)(int32_t)fpul; F6 = F6 * F4; } else i6 = (int32_t)fpul;
    R7 += 0x20;
    R9 = (uint32_t)S16b<FAST>(rb, R4); R4 += 2;
    fpul = R9;
    if (!SKIP) { F1 = RD32b<FAST>(rb, R8); F2 = RD32b<FAST>(rb, R8 + 4); F7 = (float)(int32_t)fpul; F3 = RD32b<FAST>(rb, R8 + 8); F7 = F7 * F4; F8 = RD32b<FAST>(rb, R8 + 12); F9 = f32(F8); } else { i7 = (int32_t)fpul; va = R8; }
    R8 += 16;
vertex:                                                  // L_8c31b32e
    if (FAST && !SKIP) { PF<FAST>(R4 + 96); PF<FAST>(((uint32_t)S16b<FAST>(rb, R4 + 24) << 4) + R5); }   // 4 vertices ahead
    R8 = (uint32_t)S16b<FAST>(rb, R4) << 4; R4 += 2;
    R9 = (uint32_t)S16b<FAST>(rb, R4); R4 += 2;
    R10 = (uint32_t)S16b<FAST>(rb, R4); R4 += 2;
    R8 += R5;
    if (FAST && !SKIP) __builtin_prefetch(sh4::ram + (R8 & 0xFFFFFF));   // the guest's pref of the next vertex
    if (!SKIP) { F9 = F9 - F11; em.emit(c, R7, F0, F1, F2, F3, u32(F6), u32(F7), F8, u32(F9)); } else rec(R7, F0, va, i6, i7);
    fpul = R9;
    if (!SKIP) { F6 = (float)(int32_t)fpul; F6 = F6 * F4; } else i6 = (int32_t)fpul;
    fpul = R10;
    R4 += R6;
    if (!SKIP) { F7 = (float)(int32_t)fpul; F1 = RD32b<FAST>(rb, R8); F2 = RD32b<FAST>(rb, R8 + 4); F7 = F7 * F4; F3 = RD32b<FAST>(rb, R8 + 8); F8 = RD32b<FAST>(rb, R8 + 12); F9 = f32(F8); } else { i7 = (int32_t)fpul; va = R8; }
    R8 += 16;
    R7 += 0x20;
    R2 -= 1; t = R2 == 0;
    if (!t) { NV_POLL(false); goto vertex; }
    F0 = F10;                                            // last vertex: end-of-strip PCW
    R3 -= 1; t = R3 == 0;
    if (!SKIP) { F9 = F9 - F11; em.emit(c, R7, F0, F1, F2, F3, u32(F6), u32(F7), F8, u32(F9)); } else rec(R7, F0, va, i6, i7);
    R7 += 0x20;
    if (!t) { NV_POLL(true); goto strip; }
    em.sync_sq();
    SKIP_SQ_SYNC();
    R2 = 0x8c3f21d0u; R3 = 0x8c3f21ccu;
    R4 = ::sh4::rd32(R2) + R0;
    R5 = ::sh4::rd32(R3) + R1;
    ::sh4::wr32(R2, R4);
    ::sh4::wr32(R3, R5);
    R0 = R7;
    R8 = ::sh4::rd32(R15); R15 += 4;
    R9 = ::sh4::rd32(R15); R15 += 4;
    R10 = ::sh4::rd32(R15); R15 += 4;
    SKIP_FIX_F(true);
    NV_SYNC();
}
// ---------------------------------------------------------------------------------------------------------
// Strip-writer worker ("guest worker", Grarak's MH3 layout): the vertex units of a 0x8C31B2A0 call are built
// on another core. The guest thread parses the strip lengths, copies the index stream and the referenced span
// of the vertex table (both just written, so cached), reserves and commits the TA units in stream order, and
// runs the SKIP walk for everything else the call does (polls, counters, stack slots, the store queues' final
// units). The worker then fills the reserved units with exactly the words the inline path would write. Holly
// waits for the worker (ta_sync_hook) before the TA buffer moves, is reset or is read.
namespace hw { extern void (*ta_sync_hook)(); size_t ta_stream_len(); }
struct NvJob { virtual void run() = 0; virtual ~NvJob() {} };   // work for the worker thread (strips, transforms)
struct StripJob : NvJob {
    void run() override;
    uint32_t* dst; std::vector<uint8_t> stream, table;
    uint32_t table_base;   // guest address of table[0]
    uint32_t r5, strips, skip, pcw, pcw_end; float scale;
};
static constexpr unsigned kJobs = 256;
static NvJob* job_ring[kJobs];
static std::atomic<unsigned> job_head{0}, job_tail{0};   // head: next push (guest), tail: next to run (worker)
bool nv_async;                 // set by the platform when a worker thread runs nv_worker_step
void (*nv_worker_kick)();      // wake the worker
uint64_t nv_async_calls, nv_async_units, nv_async_fallbacks;
void StripJob::run() { const StripJob& j = *this;
    const uint8_t* p = j.stream.data(); uint32_t* o = j.dst;
    auto s16 = [&p]() { int16_t v; std::memcpy(&v, p, 2); p += 2; return (int32_t)v; };
    for (uint32_t s = 0; s < j.strips; ++s) {
        int32_t len = s16(); if (len < 0) len = -len;
        for (int32_t k = 0; k < len; ++k) {
            int32_t idx = s16(), u = s16(), v = s16();
            if (k >= 2) p += j.skip;
            const uint8_t* vx = j.table.data() + ((j.r5 + ((uint32_t)idx << 4)) - j.table_base);
            uint32_t w[4]; std::memcpy(w, vx, 16);
            o[0] = k == len - 1 ? j.pcw_end : j.pcw; o[1] = w[0]; o[2] = w[1]; o[3] = w[2];
            o[4] = u32((float)u * j.scale); o[5] = u32((float)v * j.scale); o[6] = w[3]; o[7] = u32(f32(w[3]) - 1.0f);
            o += 8;
        }
    }
}
bool nv_worker_step() {   // worker thread: run every queued job; false when there was nothing to do
    unsigned t = job_tail.load(std::memory_order_relaxed);
    if (t == job_head.load(std::memory_order_acquire)) return false;
    while (t != job_head.load(std::memory_order_acquire)) {
        NvJob* j = job_ring[t % kJobs];
        j->run();
        delete j;
        job_tail.store(++t, std::memory_order_release);
    }
    return true;
}
bool nv_defer;
bool nv_xf_async;   // transform worker on (see xf_async)   // desktop test (DB_XFDEFER): no worker thread, queued jobs run only when the guest waits for them
static void nv_drain() {   // guest thread: wait until the worker has filled every reserved unit
    if (nv_defer) { nv_worker_step(); return; }
    unsigned h = job_head.load(std::memory_order_relaxed);
    while (job_tail.load(std::memory_order_acquire) != h) { if (nv_worker_kick) nv_worker_kick(); }
}
static void nv_push(NvJob* j) {
    if (job_head.load(std::memory_order_relaxed) - job_tail.load(std::memory_order_acquire) >= kJobs) nv_drain();
    unsigned h = job_head.load(std::memory_order_relaxed);
    job_ring[h % kJobs] = j;
    job_head.store(h + 1, std::memory_order_release);
    if (nv_worker_kick) nv_worker_kick();
}
// The call's strips: stream length, unit count and index range; false if anything is outside the simple case.
static bool strip_scan(uint32_t r4, uint32_t strips, uint32_t skip, uint32_t& bytes, uint32_t& units, int32_t& lo, int32_t& hi) {
    uint32_t a = r4; units = 0; lo = 0x7FFFFFFF; hi = -0x7FFFFFFF;
    for (uint32_t s = 0; s < strips; ++s) {
        int32_t len = S16<true>(a); a += 2; if (len < 0) len = -len;
        if (len < 3 || len > 4096) return false;
        for (int32_t k = 0; k < len; ++k) {
            int32_t idx = S16<true>(a); a += 6; if (k >= 2) a += skip;
            lo = idx < lo ? idx : lo; hi = idx > hi ? idx : hi;
        }
        units += (uint32_t)len;
        if (a - r4 > 0x100000) return false;
    }
    bytes = a - r4;
    return units > 0;
}
static bool strip_async(sh4::Ctx* c) {
    const uint32_t r4 = c->r[4], r5 = c->r[5], r6 = c->r[6], r7 = c->r[7];
    const uint32_t strips = r6 & ::sh4::rd32(0x8c31b3a0u), skip = ((r6 >> 14) & 3) << 1;
    const uint32_t pcw = ::sh4::rd32(0x8c31b3a4u), pcw_end = ::sh4::rd32(0x8c31b3a8u);
    if (!strips || !hw::ta_fast_ok(pcw, pcw_end) || !hw::ta_unit_direct_ok()) return false;
    uint32_t bytes, units; int32_t lo, hi;
    if (!strip_scan(r4, strips, skip, bytes, units, lo, hi)) return false;
    // every unit must go to the TA polygon path (both queues' QACR, the whole SQ address range)
    for (uint32_t a : {r7, r7 + 0x20, r7 + (units - 1) * 0x20}) {
        uint32_t q = (a >> 5) & 1, p = (((sh4::qacr(q) & 0x1C) << 24) | (a & 0x03FFFFE0)) & 0x1FFFFFFF;
        if ((a >> 26) != 0x38 || p < 0x10000000 || p >= 0x10800000) return false;
    }
    if (((r7 & 0x03FFFFFF) + units * 0x20) > 0x04000000) return false;
    uint32_t tb = r5 + ((uint32_t)lo << 4), tlen = (uint32_t)(hi - lo + 1) * 16;
    if (tlen > 0x40000 || !ram_window(tb, 0, tlen)) return false;
    StripJob* j = new StripJob;
    j->stream.resize(bytes); std::memcpy(j->stream.data(), sh4::ram + (r4 & 0xFFFFFF), bytes);
    j->table.resize(tlen); std::memcpy(j->table.data(), sh4::ram + (tb & 0xFFFFFF), tlen);
    j->table_base = tb; j->r5 = r5; j->strips = strips; j->skip = skip; j->pcw = pcw; j->pcw_end = pcw_end; j->scale = c->fr.f[0][4];
    size_t off; hw::ta_fast_reserve(units, &off);   // may wait for the worker (buffer growth)
    hw::ta_fast_commit(units);
    size_t len_before = hw::ta_stream_len();
    // everything else the call does, without building vertices
    vtx_8c31b2a0<true, true>(c);
    if (hw::ta_stream_len() != len_before) { static bool w; if (!w) { w = true; std::printf("[vtx] TA written during an async strip call (interrupt handler)\n"); } }
    j->dst = const_cast<uint32_t*>(hw::ta_stream_at(off));   // after the walk: a handler's TA write may have moved the buffer
    nv_push(j);
    nv_async_calls++; nv_async_units += units;
    return true;
}
void nv_async_enable(void (*kick)()) { nv_worker_kick = kick; hw::ta_sync_hook = nv_drain; nv_async = true; }
void nv_xf_enable(void (*kick)(), bool strips) { nv_worker_kick = kick; hw::ta_sync_hook = nv_drain; nv_xf_async = true; nv_async = strips; }

static void xf_wait();
void f_8c31b2a0(sh4::Ctx* c) {
    SH4_ENTRY_CHECK(c, 0x8c31b2a0u, 0x00380000u, 0x00000000u);
    xf_wait();   // the vertex table may still be being written by the transform worker
    nv_count[0][0]++;
    // index stream (r4, up to 1 MB of strips) and vertex table (r5 + s16 index * 16) in RAM: direct reads
    // a TA list that will not be presented: walk the strips without building vertices (hw::ta_skip_list)
    if (hw::ta_skip_list && ram_window(c->r[4], 0, 0x100000)) {
        NvTimer tm(&nv_time[1]); uint32_t r7 = c->r[7];
        vtx_8c31b2a0<true, true>(c); nv_units[1] += (c->r[0] - r7) >> 5; return;
    }
    NvTimer tm(&nv_time[0]); uint32_t r7 = c->r[7];
    if (nv_async && ram_window(c->r[4], 0, 0x100000) && ram_window(c->r[5], 0x80000, 0x80000)) {
        if (strip_async(c)) { nv_units[0] += (c->r[0] - r7) >> 5; return; }
        nv_async_fallbacks++;
    }
    if (ram_window(c->r[4], 0, 0x100000) && ram_window(c->r[5], 0x80000, 0x80000)) vtx_8c31b2a0<true, false>(c);
    else vtx_8c31b2a0<false, false>(c);
    nv_units[0] += (c->r[0] - r7) >> 5;
}
#undef NV_SYNC
#undef NV_RELOAD
#undef NV_POLL

// ---------------------------------------------------------------------------------------------------------
// 0x8C31BA84: 3D vertex transformer. Transforms the light vector by XMTRX (fipr), then per vertex: position by
// XMTRX (ftrv), lighting = clamp(normal . light), 1/w and screen x/y (fmac), written as 16-byte entries
// [sx, sy, 1/w, light] through the store queue at r2 (flushed every two vertices). Arithmetic and order
// exactly as the translation (double-precision vector ops as in the sh4ctx.h helpers, ::sh4::fmac).
namespace {
inline void sq_half(uint32_t a, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {   // 16 bytes at SQ address a
    if ((a >> 26) == 0x38) {
        uint32_t* q = sh4::sq_mem[(a >> 5) & 1] + ((a >> 2) & 7);
        q[0] = w0; q[1] = w1; q[2] = w2; q[3] = w3;
    } else { ::sh4::wr32(a, w0); ::sh4::wr32(a + 4, w1); ::sh4::wr32(a + 8, w2); ::sh4::wr32(a + 12, w3); }
}
inline float dot4(float a0, float a1, float a2, float a3, float b0, float b1, float b2, float b3) {
    double r = (double)a0 * b0 + (double)a1 * b1 + (double)a2 * b2 + (double)a3 * b3;
    return (float)r;
}
} // namespace


// Fast-math transform (NV_FAST_TRANSFORM): XMTRX * (x, y, z, 1) in single precision as column multiply-adds,
// NEON on the Vita. Not bit-identical to the double-precision translation (nor is the SH-4's FTRV unit).
// Runtime switch (Vita: ux0:data/dolphinblue/fastxf, desktop: DB_FASTXF=1). An earlier compile-time test showed no
// gain while memory stalls dominated; re-test now that the vertex streams are prefetched.
bool nv_fast_xf = getenv("DB_FASTXF") != nullptr;
#define NV_FAST_TRANSFORM nv_fast_xf
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
static inline void ftrv_fast(const float* X, float x, float y, float z, float& o0, float& o1, float& o2, float& o3) {
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    float32x4_t r = vmulq_n_f32(vld1q_f32(X), x);
    r = vmlaq_n_f32(r, vld1q_f32(X + 4), y);
    r = vmlaq_n_f32(r, vld1q_f32(X + 8), z);
    r = vaddq_f32(r, vld1q_f32(X + 12));
    o0 = vgetq_lane_f32(r, 0); o1 = vgetq_lane_f32(r, 1); o2 = vgetq_lane_f32(r, 2); o3 = vgetq_lane_f32(r, 3);
#else
    o0 = X[0] * x + X[4] * y + X[8] * z + X[12];
    o1 = X[1] * x + X[5] * y + X[9] * z + X[13];
    o2 = X[2] * x + X[6] * y + X[10] * z + X[14];
    o3 = X[3] * x + X[7] * y + X[11] * z + X[15];
#endif
}

#define BA_SYNC() do { \
        c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->r[4] = R4; c->r[5] = R5; c->r[6] = R6; c->r[7] = R7; c->r[15] = R15; c->t = t; \
        c->fr.f[0][0] = F0; c->fr.f[0][1] = F1; c->fr.f[0][2] = F2; c->fr.f[0][3] = F3; c->fr.f[0][4] = F4; c->fr.f[0][5] = F5; c->fr.f[0][6] = F6; c->fr.f[0][7] = F7; \
        c->fr.f[0][8] = F8; c->fr.f[0][9] = F9; c->fr.f[0][10] = F10; c->fr.f[0][11] = F11; c->fr.f[0][12] = F12; c->fr.f[0][13] = F13; c->fr.f[0][14] = F14; c->fr.f[0][15] = F15; \
    } while (0)
#define BA_RELOAD() do { \
        R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; R4 = c->r[4]; R5 = c->r[5]; R6 = c->r[6]; R7 = c->r[7]; R15 = c->r[15]; t = c->t; \
        F0 = c->fr.f[0][0]; F1 = c->fr.f[0][1]; F2 = c->fr.f[0][2]; F3 = c->fr.f[0][3]; F4 = c->fr.f[0][4]; F5 = c->fr.f[0][5]; F6 = c->fr.f[0][6]; F7 = c->fr.f[0][7]; \
        F8 = c->fr.f[0][8]; F9 = c->fr.f[0][9]; F10 = c->fr.f[0][10]; F11 = c->fr.f[0][11]; F12 = c->fr.f[0][12]; F13 = c->fr.f[0][13]; F14 = c->fr.f[0][14]; F15 = c->fr.f[0][15]; \
        for (int i_ = 0; i_ < 16; ++i_) { X[i_] = c->fr.f[1][i_]; Xd[i_] = (double)X[i_]; } \
    } while (0)

// FAST transformers: the destination table is written through the store queues (two 16-byte entries per
// 32-byte unit, flushed with pref). When the SQ area maps to main RAM, each entry is written straight to RAM;
// the store queues are brought to the state the flushes would leave (each queue's last complete unit) at polls
// and at the end, and an odd final entry takes the original SQ path (its unit is flushed half-stale).
struct DirectDst {
    bool on = false, plain = false; uint32_t sq0 = 0, ram0 = 0, qacr0 = 0, qacr1 = 0, last_blk[2] = {0, 0}; bool have[2] = {false, false};
    void begin(uint32_t r2, uint32_t n) {
        qacr0 = sh4::qacr(0); qacr1 = sh4::qacr(1);
        if ((r2 >> 26) != 0x38) {   // a plain RAM table: the translation writes it with wr32 and pref is a no-op
            if (!(r2 & 3) && ram_window(r2, 0, n * 16 + 32)) { sq0 = r2; ram0 = r2 & 0xFFFFFF; on = plain = true; }
            return;
        }
        if ((r2 >> 26) != 0x38 || (r2 & 0x1F) || ((qacr0 ^ qacr1) & 0x1C)) return;
        uint32_t p = sq_ram_target(r2);
        if (!p || (p & 0xFFFFFF) + (uint64_t)n * 16 + 32 > 0x1000000 || (r2 & 0x03FFFFFF) + (uint64_t)n * 16 + 32 > 0x04000000) return;
        sq0 = r2; ram0 = p & 0xFFFFFF; on = true;
    }
    uint32_t* at(uint32_t r2) const { return (uint32_t*)(sh4::ram + ram0 + (r2 - sq0)); }
    void half(uint32_t r2, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
        uint32_t* d = at(r2); d[0] = w0; d[1] = w1; d[2] = w2; d[3] = w3;
        if (!plain && (r2 & 0x10)) { uint32_t q = (r2 >> 5) & 1; last_blk[q] = r2 & ~0x1Fu; have[q] = true; }
    }
    void sync() { for (int q = 0; q < 2; ++q) if (have[q]) { std::memcpy(sh4::sq_mem[q], at(last_blk[q]), 32); have[q] = false; } }
    void after_poll(uint32_t r2, uint32_t r0) {   // an interrupt handler changed QACR: back to the SQ path
        if (plain) return;
        if (sh4::qacr(0) == qacr0 && sh4::qacr(1) == qacr1) return;
        if (r0 & 1) std::memcpy(sh4::sq_mem[((r2 - 16) >> 5) & 1], at((r2 - 16) & ~0x1Fu), 32);   // the half already written
        on = false;
    }
};

template <bool FAST> static void tr_8c31ba84(sh4::Ctx* c) {
    const uint8_t* const rb = sh4::ram;
    uint32_t R0, R1, R2, R3, R4 = c->r[4], R5, R6, R7, R15 = c->r[15], t;
    float F0, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12 = c->fr.f[0][12], F13 = c->fr.f[0][13], F14 = c->fr.f[0][14], F15 = c->fr.f[0][15];
    float X[16]; for (int i = 0; i < 16; ++i) X[i] = c->fr.f[1][i];   // XMTRX (XF bank)
    double Xd[16]; for (int i = 0; i < 16; ++i) Xd[i] = (double)X[i];   // converted once, not per vertex (same values)
    // push fr15..fr12
    R15 -= 4; ::sh4::wr32(R15, u32(F15));
    R15 -= 4; ::sh4::wr32(R15, u32(F14));
    R15 -= 4; ::sh4::wr32(R15, u32(F13));
    R15 -= 4; ::sh4::wr32(R15, u32(F12));
    F12 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    F13 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    F14 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    F15 = f32(0);
    F0 = X[0]; F1 = X[1]; F2 = X[2]; F3 = X[3];
    F3 = dot4(F0, F1, F2, F3, F12, F13, F14, F15);
    F4 = X[4]; F5 = X[5]; F6 = X[6]; F7 = X[7];
    F7 = dot4(F4, F5, F6, F7, F12, F13, F14, F15);
    F8 = X[8]; F9 = X[9]; F10 = X[10]; F11 = X[11];
    F11 = dot4(F8, F9, F10, F11, F12, F13, F14, F15);
    F12 = F3; F13 = F7; F14 = F11;
    F0 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    F15 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    R1 = RD32b<FAST>(rb, R4); R4 += 4;
    F12 = F12 * F0;
    R3 = RD32b<FAST>(rb, R4); R4 += 4;
    F13 = F13 * F0;
    R7 = RD32b<FAST>(rb, R4); R4 += 4;
    ::sh4::pref(c, R3);
    R2 = RD32b<FAST>(rb, R4); R4 += 4;
    R5 = R3;
    F14 = F14 * F0;
    R5 += 0x18;
    F8 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    F9 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    F10 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    F11 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    ::sh4::pref(c, R5);
    R0 = 0; R6 = 0;
    const uint32_t dst_sq0 = R2 & ~0x1Fu, dst_ram = sq_ram_target(dst_sq0);   // destination table in RAM (for prefetch)
    DirectDst dd; if (FAST) dd.begin(R2, R1);
vertex:                                                  // L_8c31bad6
    PF<FAST>(R3 + 0xC0); if (FAST) PFW<FAST>(dst_ram + (R2 & 0x1F) + ((R2 - dst_sq0) & ~0x1Fu) + 0x80);
    F4 = f32(RD32b<FAST>(rb, R3)); R3 += 4;
    F5 = f32(RD32b<FAST>(rb, R3)); R3 += 4;
    F6 = f32(RD32b<FAST>(rb, R3)); R3 += 4;
    F7 = 1.0f;
    if (NV_FAST_TRANSFORM) ftrv_fast(X, F4, F5, F6, F4, F5, F6, F7);   // (F7 == 1)
    else {   // ftrv XMTRX, fv4
        double t0 = Xd[0] * F4 + Xd[4] * F5 + Xd[8] * F6 + Xd[12] * F7;
        double t1 = Xd[1] * F4 + Xd[5] * F5 + Xd[9] * F6 + Xd[13] * F7;
        double t2 = Xd[2] * F4 + Xd[6] * F5 + Xd[10] * F6 + Xd[14] * F7;
        double t3 = Xd[3] * F4 + Xd[7] * F5 + Xd[11] * F6 + Xd[15] * F7;
        F4 = (float)t0; F5 = (float)t1; F6 = (float)t2; F7 = (float)t3;
    }
    F0 = f32(RD32b<FAST>(rb, R3)); R3 += 4;
    F1 = f32(RD32b<FAST>(rb, R3)); R3 += 4;
    F2 = f32(RD32b<FAST>(rb, R3)); R3 += 4;
    F3 = 1.0f;
    F3 = NV_FAST_TRANSFORM ? (F0 * F12 + F1 * F13 + F2 * F14 + F3 * F15) : dot4(F0, F1, F2, F3, F12, F13, F14, F15);
    F0 = 1.0f;
    R5 += 0x18;
    F0 = F0 / F6;
    ::sh4::pref(c, R5);
    F1 = 1.0f;
    t = F3 > F15;
    R2 += 0x10;
    if (!t) F3 = F15;
    t = F6 > F1;
    F1 = F3;
    if (!t) R6 += 1;
    F2 = F10;
    F4 = F4 * F8;
    F3 = F11;
    F5 = F5 * F9;
    F2 = NV_FAST_TRANSFORM ? F0 * F4 + F2 : ::sh4::fmac(F0, F4, F2);   // fast: single precision (VFP vmla)
    R0 += 1;
    F3 = NV_FAST_TRANSFORM ? F0 * F5 + F3 : ::sh4::fmac(F0, F5, F3);
    t = (R0 & 1) == 0;
    R2 -= 16;
    if (FAST && dd.on && (t || R1 != 1 || dd.plain)) { dd.half(R2, u32(F2), u32(F3), u32(F0), u32(F1)); if (t) R7 += 0x20; }
    else {
        if (FAST && dd.on) { dd.sync(); dd.on = false; }
        sq_half(R2, u32(F2), u32(F3), u32(F0), u32(F1));   // [sx, sy, 1/w, light]
        if (t) { R7 += 0x20; ::sh4::pref(c, R2); }
    }
    R1 -= 1; t = R1 == 0;
    R2 += 0x10;
    if (!t) {
        if (--c->slice <= 0 || c->irq_pending) { if (FAST && dd.on) dd.sync(); BA_SYNC(); ::sh4::poll_irq(c); BA_RELOAD(); c->cur_func = 0x8c31ba84u; if (FAST && dd.on) dd.after_poll(R2, R0); }
        goto vertex;
    }
    if (FAST && dd.on) dd.sync();
    t = (R0 & 1) == 0;
    if (!t) ::sh4::pref(c, R2);
    F12 = f32(::sh4::rd32(R15)); R15 += 4;
    F13 = f32(::sh4::rd32(R15)); R15 += 4;
    F14 = f32(::sh4::rd32(R15)); R15 += 4;
    F15 = f32(::sh4::rd32(R15)); R15 += 4;
    R0 = R6;
    BA_SYNC();
}
#undef BA_SYNC
#undef BA_RELOAD

// ---------------------------------------------------------------------------------------------------------
// Transform worker: the vertex loop of 0x8C31BA84 / 0x8C31B710 runs on another core. The guest thread copies
// the source vertices, counts the vertices with w <= 1 (the return value: only the z row is needed), computes
// the last vertices itself (the registers at return and the store queues' final units), and queues the table
// for the worker. Unlike the inline path it does not poll inside the loop (interrupts are taken at the
// caller's next poll). The table's only reader is the strip writer (0x8C31B2A0), which waits for the worker,
// as do TA syncs and the next inline transform. Switch: ux0:data/dolphinblue/xfasync, desktop DB_XFASYNC=1.
uint64_t nv_xf_calls, nv_xf_verts;
static unsigned xf_last_job;    // job_head after the most recent transform job
static void xf_wait() { if (nv_xf_async && (int)(job_tail.load(std::memory_order_acquire) - xf_last_job) < 0) nv_drain(); }
struct XfOut { float f[8]; uint32_t w[4]; };
struct XfJob : NvJob {
    bool light, fast; uint32_t n, stride;
    float X[16], P[4], L[4]; double Xd[16];   // XMTRX; F8-F11 (scale x/y, offset x/y); light F12-F15
    std::vector<uint8_t> src; uint32_t* dst;
    void vertex(const uint8_t* s, XfOut& o) const {
        float F0, F1, F2, F3, F4, F5, F6, F7; uint32_t attr = 0;
        std::memcpy(&F4, s, 4); std::memcpy(&F5, s + 4, 4); std::memcpy(&F6, s + 8, 4); F7 = 1.0f;
        if (fast) ftrv_fast(X, F4, F5, F6, F4, F5, F6, F7);
        else {
            double t0 = Xd[0] * F4 + Xd[4] * F5 + Xd[8] * F6 + Xd[12] * F7, t1 = Xd[1] * F4 + Xd[5] * F5 + Xd[9] * F6 + Xd[13] * F7;
            double t2 = Xd[2] * F4 + Xd[6] * F5 + Xd[10] * F6 + Xd[14] * F7, t3 = Xd[3] * F4 + Xd[7] * F5 + Xd[11] * F6 + Xd[15] * F7;
            F4 = (float)t0; F5 = (float)t1; F6 = (float)t2; F7 = (float)t3;
        }
        if (light) {
            std::memcpy(&F0, s + 12, 4); std::memcpy(&F1, s + 16, 4); std::memcpy(&F2, s + 20, 4); F3 = 1.0f;
            F3 = fast ? (F0 * L[0] + F1 * L[1] + F2 * L[2] + F3 * L[3]) : dot4(F0, F1, F2, F3, L[0], L[1], L[2], L[3]);
            if (!(F3 > L[3])) F3 = L[3];
            F1 = F3;
        } else { std::memcpy(&attr, s + 24, 4); std::memcpy(&F1, &attr, 4); }
        F0 = 1.0f; F0 = F0 / F6;
        F4 = F4 * P[0]; F5 = F5 * P[1];
        F2 = fast ? F0 * F4 + P[2] : ::sh4::fmac(F0, F4, P[2]);
        F3 = fast ? F0 * F5 + P[3] : ::sh4::fmac(F0, F5, P[3]);
        o.f[0] = F0; o.f[1] = F1; o.f[2] = F2; o.f[3] = F3; o.f[4] = F4; o.f[5] = F5; o.f[6] = F6; o.f[7] = F7;
        o.w[0] = u32(F2); o.w[1] = u32(F3); o.w[2] = u32(F0); o.w[3] = light ? u32(F1) : attr;
    }
    void run() override {
        const uint8_t* s = src.data(); XfOut o;
        for (uint32_t i = 0; i < n; ++i, s += stride) { vertex(s, o); std::memcpy(dst + 4 * i, o.w, 16); }
    }
};
// r4: parameter block; light: 0x8C31BA84 (else 0x8C31B710). False: not handled, nothing changed.
static bool xf_async(sh4::Ctx* c, bool light) {
    const uint32_t r4 = c->r[4], pb = light ? r4 + 12 : r4 + 0x14;   // pb: [scale (BA84 only), F15 (BA84 only)], then n, src, r7, dst, F8-F11
    const uint32_t base = light ? pb + 8 : pb;
    if (!ram_window(r4, 0, 64)) return false;
    const uint32_t n = ::sh4::rd32(base), src = ::sh4::rd32(base + 4), r7 = ::sh4::rd32(base + 8), r2 = ::sh4::rd32(base + 12);
    const uint32_t stride = light ? 0x18 : 0x1C;
    if (n < 8 || n >= 0x10000 || !ram_window(src, 0, n * stride + 0x20)) return false;
    DirectDst dd; dd.begin(r2, n);
    if (!dd.on || (!dd.plain && (n & 1))) return false;
    XfJob* j = new XfJob;
    j->light = light; j->fast = NV_FAST_TRANSFORM; j->n = n; j->stride = stride;
    for (int i = 0; i < 16; ++i) { j->X[i] = c->fr.f[1][i]; j->Xd[i] = (double)j->X[i]; }
    for (int i = 0; i < 4; ++i) j->P[i] = f32(::sh4::rd32(base + 16 + 4 * i));
    const float o12 = c->fr.f[0][12], o13 = c->fr.f[0][13], o14 = c->fr.f[0][14], o15 = c->fr.f[0][15];
    if (light) {   // as the inline prologue: light vector by XMTRX rows (fipr), times the scale; F15 from the block
        const float* X = j->X; float lx = f32(::sh4::rd32(r4)), ly = f32(::sh4::rd32(r4 + 4)), lz = f32(::sh4::rd32(r4 + 8)), z = f32(0);
        float a = dot4(X[0], X[1], X[2], X[3], lx, ly, lz, z), b = dot4(X[4], X[5], X[6], X[7], lx, ly, lz, z), d = dot4(X[8], X[9], X[10], X[11], lx, ly, lz, z);
        float sc = f32(::sh4::rd32(pb)); j->L[0] = a * sc; j->L[1] = b * sc; j->L[2] = d * sc; j->L[3] = f32(::sh4::rd32(pb + 4));
        uint32_t sp = c->r[15];   // the inline path pushes fr15..fr12 below SP and pops them again
        ::sh4::wr32(sp - 4, u32(o15)); ::sh4::wr32(sp - 8, u32(o14)); ::sh4::wr32(sp - 12, u32(o13)); ::sh4::wr32(sp - 16, u32(o12));
    }
    xf_wait();   // a previous table may share memory with this one
    j->src.resize((size_t)n * stride); std::memcpy(j->src.data(), sh4::ram + (src & 0xFFFFFF), j->src.size());
    j->dst = (uint32_t*)(sh4::ram + dd.ram0 + (r2 - dd.sq0));
    // return value: vertices with w <= 1 (the z row, as the inline arithmetic computes it)
    uint32_t cnt = 0; const uint8_t* s = j->src.data(); const float* X = j->X; const double* Xd = j->Xd;
    for (uint32_t i = 0; i < n; ++i, s += stride) {
        float x, y, zz, w; std::memcpy(&x, s, 4); std::memcpy(&y, s + 4, 4); std::memcpy(&zz, s + 8, 4);
        if (j->fast) { w = X[2] * x; w = w + X[6] * y; w = w + X[10] * zz; w = w + X[14]; }
        else w = (float)(Xd[2] * x + Xd[6] * y + Xd[10] * zz + Xd[14] * 1.0f);
        cnt += !(w > 1.0f);
    }
    // the last vertices: registers at return, and (store-queue destination) each queue's last unit
    XfOut o[4]; const uint32_t k0 = n - (dd.plain ? 1 : 4);
    for (uint32_t i = k0; i < n; ++i) j->vertex(j->src.data() + (size_t)i * stride, o[i - k0]);
    if (!dd.plain)
        for (uint32_t b = 0; b < 2; ++b) {   // units n/2-2 and n/2-1 (n >= 8): SQ address r2 + 32 * unit
            uint32_t unit = n / 2 - 2 + b, q = ((r2 + 32 * unit) >> 5) & 1;
            std::memcpy(sh4::sq_mem[q], o[2 * b].w, 16); std::memcpy(sh4::sq_mem[q] + 4, o[2 * b + 1].w, 16);
        }
    const XfOut& L = o[dd.plain ? 0 : 3];
    nv_push(j); xf_last_job = job_head.load(std::memory_order_relaxed);
    nv_xf_calls++; nv_xf_verts += n;
    c->r[0] = cnt; c->r[1] = 0; c->r[2] = r2 + n * 16; c->r[3] = src + n * stride; c->r[4] = base + 32;
    c->r[5] = src + (light ? 0x18 : 0x10) + n * stride; c->r[6] = cnt; c->r[7] = r7 + 0x20 * (n / 2); c->t = (n & 1) == 0;
    for (int i = 0; i < 8; ++i) c->fr.f[0][i] = L.f[i];
    if (!light) c->fr.u[0][1] = L.w[3];
    for (int i = 0; i < 4; ++i) c->fr.f[0][8 + i] = j->P[i];
    if (light) { c->fr.f[0][12] = o12; c->fr.f[0][13] = o13; c->fr.f[0][14] = o14; c->fr.f[0][15] = o15; }
    return true;
}

void f_8c31ba84(sh4::Ctx* c) {
    SH4_ENTRY_CHECK(c, 0x8c31ba84u, 0x00380000u, 0x00000000u);
    nv_count[1][0]++; nv_count[1][1] += ::sh4::rd32(c->r[4] + 20);
    // parameter block at r4 (52 bytes) and the source vertices (count at +20, pointer at +24, 0x18 bytes each)
    uint32_t r4 = c->r[4];
    bool fast = ram_window(r4, 0, 64);
    if (fast) {
        uint32_t n = ::sh4::rd32(r4 + 20), src = ::sh4::rd32(r4 + 24);
        fast = n < 0x10000 && ram_window(src, 0, n * 0x18 + 0x20);
    }
    // (no draw skip here: the game reuses these screen-space tables in later frames)
    NvTimer tm(&nv_time[2]);
    if (nv_xf_async && fast && xf_async(c, true)) return;
    xf_wait();
    if (fast) tr_8c31ba84<true>(c); else tr_8c31ba84<false>(c);
}

// ---------------------------------------------------------------------------------------------------------
// 0x8C31B710: 3D vertex transformer without lighting. Per vertex: position by XMTRX (ftrv), 1/w, screen x/y
// (fmac) and the vertex's attribute word, written as 16-byte entries [sx, sy, 1/w, attr] through the store
// queue at r2 (flushed every two vertices). Source vertices are 0x1C bytes. Exact as the translation.
#define B7_SYNC() do { \
        c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->r[4] = R4; c->r[5] = R5; c->r[6] = R6; c->r[7] = R7; c->t = t; \
        c->fr.f[0][0] = F0; c->fr.u[0][1] = F1; c->fr.f[0][2] = F2; c->fr.f[0][3] = F3; c->fr.f[0][4] = F4; c->fr.f[0][5] = F5; c->fr.f[0][6] = F6; c->fr.f[0][7] = F7; \
        c->fr.f[0][8] = F8; c->fr.f[0][9] = F9; c->fr.f[0][10] = F10; c->fr.f[0][11] = F11; \
    } while (0)
#define B7_RELOAD() do { \
        R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; R4 = c->r[4]; R5 = c->r[5]; R6 = c->r[6]; R7 = c->r[7]; t = c->t; \
        F0 = c->fr.f[0][0]; F1 = c->fr.u[0][1]; F2 = c->fr.f[0][2]; F3 = c->fr.f[0][3]; F4 = c->fr.f[0][4]; F5 = c->fr.f[0][5]; F6 = c->fr.f[0][6]; F7 = c->fr.f[0][7]; \
        F8 = c->fr.f[0][8]; F9 = c->fr.f[0][9]; F10 = c->fr.f[0][10]; F11 = c->fr.f[0][11]; \
        for (int i_ = 0; i_ < 16; ++i_) { X[i_] = c->fr.f[1][i_]; Xd[i_] = (double)X[i_]; } \
    } while (0)

template <bool FAST> static void tr_8c31b710(sh4::Ctx* c) {
    const uint8_t* const rb = sh4::ram;
    uint32_t R0, R1, R2, R3, R4 = c->r[4], R5, R6, R7, t = c->t;
    float F0 = c->fr.f[0][0], F2 = c->fr.f[0][2], F3 = c->fr.f[0][3], F4, F5, F6, F7, F8, F9, F10, F11;
    uint32_t F1 = c->fr.u[0][1];
    float X[16]; for (int i = 0; i < 16; ++i) X[i] = c->fr.f[1][i];
    double Xd[16]; for (int i = 0; i < 16; ++i) Xd[i] = (double)X[i];   // converted once, not per vertex (same values)
    R4 += 0x14;
    R1 = RD32b<FAST>(rb, R4); R4 += 4;
    R3 = RD32b<FAST>(rb, R4); R4 += 4;
    R7 = RD32b<FAST>(rb, R4); R4 += 4;
    ::sh4::pref(c, R3);
    R2 = RD32b<FAST>(rb, R4); R4 += 4;
    R5 = R3; R5 += 0x10;
    F8 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    F9 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    F10 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    F11 = f32(RD32b<FAST>(rb, R4)); R4 += 4;
    ::sh4::pref(c, R5);
    R0 = 0; R6 = 0;
    const uint32_t dst_sq0 = R2 & ~0x1Fu, dst_ram = sq_ram_target(dst_sq0);   // destination table in RAM (for prefetch)
    DirectDst dd; if (FAST) dd.begin(R2, R1);
vertex:                                                  // L_8c31b72e
    PF<FAST>(R3 + 0xE0); if (FAST) PFW<FAST>(dst_ram + (R2 & 0x1F) + ((R2 - dst_sq0) & ~0x1Fu) + 0x80);
    F4 = f32(RD32b<FAST>(rb, R3)); R3 += 4;
    F5 = f32(RD32b<FAST>(rb, R3)); R3 += 4;
    R5 += 0x1c;
    F6 = f32(RD32b<FAST>(rb, R3)); R3 += 4;
    F7 = 1.0f;
    if (NV_FAST_TRANSFORM) ftrv_fast(X, F4, F5, F6, F4, F5, F6, F7);   // (F7 == 1)
    else {   // ftrv XMTRX, fv4
        double t0 = Xd[0] * F4 + Xd[4] * F5 + Xd[8] * F6 + Xd[12] * F7;
        double t1 = Xd[1] * F4 + Xd[5] * F5 + Xd[9] * F6 + Xd[13] * F7;
        double t2 = Xd[2] * F4 + Xd[6] * F5 + Xd[10] * F6 + Xd[14] * F7;
        double t3 = Xd[3] * F4 + Xd[7] * F5 + Xd[11] * F6 + Xd[15] * F7;
        F4 = (float)t0; F5 = (float)t1; F6 = (float)t2; F7 = (float)t3;
    }
    ::sh4::pref(c, R5);
    F0 = 1.0f;
    R3 += 0xc;
    F0 = F0 / F6;
    F2 = 1.0f;
    R2 += 0x10;
    t = F6 > F2;
    F1 = RD32b<FAST>(rb, R3); R3 += 4;
    if (!t) R6 += 1;
    F2 = F10;
    F4 = F4 * F8;
    F3 = F11;
    F5 = F5 * F9;
    F2 = NV_FAST_TRANSFORM ? F0 * F4 + F2 : ::sh4::fmac(F0, F4, F2);   // fast: single precision (VFP vmla)
    R0 += 1;
    F3 = NV_FAST_TRANSFORM ? F0 * F5 + F3 : ::sh4::fmac(F0, F5, F3);
    t = (R0 & 1) == 0;
    R2 -= 16;
    if (FAST && dd.on && (t || R1 != 1 || dd.plain)) { dd.half(R2, u32(F2), u32(F3), u32(F0), F1); if (t) R7 += 0x20; }
    else {
        if (FAST && dd.on) { dd.sync(); dd.on = false; }
        sq_half(R2, u32(F2), u32(F3), u32(F0), F1);         // [sx, sy, 1/w, attr]
        if (t) { R7 += 0x20; ::sh4::pref(c, R2); }
    }
    R1 -= 1; t = R1 == 0;
    R2 += 0x10;
    if (!t) {
        if (--c->slice <= 0 || c->irq_pending) { if (FAST && dd.on) dd.sync(); B7_SYNC(); ::sh4::poll_irq(c); B7_RELOAD(); c->cur_func = 0x8c31b710u; if (FAST && dd.on) dd.after_poll(R2, R0); }
        goto vertex;
    }
    if (FAST && dd.on) dd.sync();
    t = (R0 & 1) == 0;
    if (!t) ::sh4::pref(c, R2);
    R0 = R6;
    B7_SYNC();
}
#undef B7_SYNC
#undef B7_RELOAD

void f_8c31b710(sh4::Ctx* c) {
    SH4_ENTRY_CHECK(c, 0x8c31b710u, 0x00380000u, 0x00000000u);
    nv_count[2][0]++; nv_count[2][1] += ::sh4::rd32(c->r[4] + 0x14);
    // parameter block at r4+0x14 (count, source, r7, destination, 4 floats) and 0x1C-byte source vertices
    uint32_t r4 = c->r[4];
    bool fast = ram_window(r4, 0, 64);
    if (fast) {
        uint32_t n = ::sh4::rd32(r4 + 0x14), src = ::sh4::rd32(r4 + 0x18);
        fast = n < 0x10000 && ram_window(src, 0, n * 0x1C + 0x20);
    }
    // (no draw skip here: the game reuses these screen-space tables in later frames)
    NvTimer tm(&nv_time[3]);
    if (nv_xf_async && fast && xf_async(c, false)) return;
    xf_wait();
    if (fast) tr_8c31b710<true>(c); else tr_8c31b710<false>(c);
}

// ---------------------------------------------------------------------------------------------------------
// 0x8C2F0ECE: returns ~TCNT0 (TMU channel 0, counting down) as an up-counter. f_8c2f2126 calls it in a
// frame-pacing busy-wait (call site pr 0x8C2F2154) until 0x28870 ticks have passed since the frame started.
// Each pass of that loop advanced emulated time by only a few cycles, so the wait cost real CPU time (mission 2:
// 12% of the guest core, and 2 fps in places). At that call site a whole scheduler slice runs first (time passes
// and interrupts are taken as they would during the spin), then the counter is read as usual.
namespace sh4 { bool idle_wait_slice(Ctx* c); }
void f_8c2f0ece(sh4::Ctx* c) {
    SH4_ENTRY_CHECK(c, 0x8c2f0eceu, 0x00380000u, 0x00000000u);
    if (c->pr == 0x8c2f2154u) { sh4::idle_wait_slice(c); c->cur_func = 0x8c2f0eceu; }
    uint32_t r3 = ::sh4::rd32(0xffd8000cu);
    c->r[0] = 0xffffffffu - r3; c->r[2] = 0xffd8000cu; c->r[3] = r3;
}

// ---------------------------------------------------------------------------------------------------------
// 0x8C0E3B00: Atomiswave operator switches. The arcade code reads the switch register (0xA0600280, active low)
// into the debounce state at 0x8C3C2330; the Dreamcast conversion patched the read to a constant 0, which made
// the system menu (test mode, game/coin settings, bookkeeping, SAVE&EXIT) unreachable. This restores the read
// from hw::aw_switches (bit set = switch pressed), which the platform sets from its buttons.
namespace hw { uint8_t aw_switches; }
void f_8c0e3b00(sh4::Ctx* c) {
    SH4_ENTRY_CHECK(c, 0x8c0e3b00u, 0x00380000u, 0x00000000u);
    const uint32_t b = 0x8c3c2330u;
    uint8_t prev = (uint8_t)::sh4::rd8(b + 1);
    ::sh4::wr8(b, prev);
    uint8_t cur = hw::aw_switches & 0xF;
    ::sh4::wr8(b + 1, cur);
    uint8_t rise = (uint8_t)((prev ^ cur) & cur), fall = (uint8_t)((cur ^ prev) & prev);
    ::sh4::wr8(b + 2, rise);
    ::sh4::wr8(b + 3, fall);
    ::sh4::wr8(b, (uint8_t)(fall | rise));
    c->r[0] = (uint32_t)(int32_t)(int8_t)(fall | rise); c->r[2] = (uint32_t)(int32_t)(int8_t)fall; c->r[3] = 0xa0600280u; c->r[4] = b;
    c->r[5] = (uint32_t)(int32_t)(int8_t)prev;
}
