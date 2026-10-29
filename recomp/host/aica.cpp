// AICA register model (no sound output yet): channel/common/DSP register
// storage, timers A/B/C with SH-4 (MCIPD) and ARM (SCIPD) pending bits, Holly external
// AICA interrupt, and the AICA RTC.
#include "hw.h"
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <atomic>

namespace hw {
namespace arm { void reset(); void stop(); bool is_running(); void set_fiq(bool); void run(int); extern uint64_t instr_count; }
constexpr int ARM_INSTR_PER_SAMPLE = 128;     // ~22.6 MHz ARM7 at ~4 cycles per instruction

static uint16_t regs[0x8000 / 4];        // 16-bit registers at 4-byte stride
static inline uint16_t& R(uint32_t off) { return regs[(off & 0x7FFF) / 4]; }
static uint64_t last_update;
static double sample_frac;
static uint32_t rtc_hi, rtc_lo;
static bool arm_running;
uint64_t aica_samples;
uint16_t* aica_regs() { return regs; }
extern uint16_t* const aica_regs_ptr;
uint16_t* const aica_regs_ptr = regs;   // inlined access for the mixer/DSP (extern: const globals are internal by default)
extern bool aica_dsp_dirty;
uint8_t* aica_aram() { return aram; }
void aica_mix_kyonex();
void aica_mix_reg_written(uint32_t off);
void aica_mix_sample(int32_t* l, int32_t* r);
uint32_t aica_mix_monitor(uint32_t off);
void (*aica_audio_sink)(int16_t l, int16_t r);   // platform output (desktop WAV, Vita ring buffer); null = discard
bool aica_mix_off;   // debug/perf switch: skip mixing (and the DSP) entirely
static inline void mix_one() {
    if (aica_mix_off) return;
    int32_t l, r; aica_mix_sample(&l, &r);
    if (aica_audio_sink) aica_audio_sink((int16_t)l, (int16_t)r);
}

// Threaded mode (Vita): the ARM7 runs on its own core. The SH-4 thread still owns the sample clock and the
// timers; it publishes samples_due and the ARM thread runs ARM_INSTR_PER_SAMPLE instructions per sample up
// to it (never ahead). Register state is shared under a spinlock; actions that belong to the other thread
// (ARM -> SH-4 interrupt, ARM reset/stop requested by the SH-4) are passed as flags. Desktop runs stay
// single-threaded and deterministic.
static bool threaded;
static bool realtime;   // threaded + real-time: the sound side runs at 44.1 kHz of wall time, not of SH-4 time
static std::atomic_flag reg_lock = ATOMIC_FLAG_INIT;
struct RegGuard {
    RegGuard() { if (threaded) while (reg_lock.test_and_set(std::memory_order_acquire)) {} }
    ~RegGuard() { if (threaded) reg_lock.clear(std::memory_order_release); }
};
static std::atomic<uint64_t> samples_due;
static uint64_t samples_done;
static std::atomic<int> arm_request;          // 1 = reset, 2 = stop
static std::atomic<bool> sh4_irq_dirty;
static std::atomic<int> arm_hold{0}, arm_held{0};   // save states: the ARM7 thread parks while the state is copied
static void arm_park() { if (arm_hold.load(std::memory_order_acquire)) { arm_held = 1; while (arm_hold.load(std::memory_order_acquire)) {} arm_held = 0; } }
void aica_hold(bool on) {
    if (!threaded) return;
    if (on) { arm_hold = 1; for (long i = 0; !arm_held.load() && i < 400000000L; ++i) {} }
    else arm_hold = 0;
}

// ARM-side interrupt controller (e68k style): lowest pending enabled SCI bit -> level L -> FIQ
static uint32_t reg_L;
static bool e68k_out, sci_pending;
static uint32_t pend_L;
static void update_e68k() {
    if (!e68k_out && sci_pending) { e68k_out = true; reg_L = pend_L; }
    arm::set_fiq(e68k_out);
}
static void update_arm_irq() {
    uint32_t p = R(0x289C) & R(0x28A0) & 0x7FF;
    sci_pending = p != 0;
    if (p) {
        int bit = __builtin_ctz(p);
        uint32_t b = bit > 7 ? 7 : bit;    // SCILV registers cover bits 0-7; bits 8-10 share level of bit 7
        pend_L = ((R(0x28A8) >> b) & 1) | (((R(0x28AC) >> b) & 1) << 1) | (((R(0x28B0) >> b) & 1) << 2);
    }
    update_e68k();
}

static void update_sh4_irq() {
    if (R(0x28B8) & R(0x28B4)) holly_raise_ext(HE_AICA);
    else holly_clear_ext(HE_AICA);
}
// from the ARM thread, Holly's interrupt state is the SH-4 thread's: defer to its next aica_update
static void sh4_irq(bool from_arm) { if (threaded && from_arm) sh4_irq_dirty = true; else update_sh4_irq(); }
static uint32_t aica_read_locked(uint32_t addr, int size);
static void aica_write_locked(uint32_t addr, uint32_t v, int size, bool from_arm);

static uint32_t pre[3];   // timer prescalers (samples toward the next count)
static void timer_tick(int samples, bool from_arm = false) {
    for (int t = 0; t < 3; ++t) {
        (void)0;
        uint32_t off = 0x2890 + 4 * t;
        uint32_t ctl = (R(off) >> 8) & 7, cnt = R(off) & 0xFF;
        // counter increments every 2^ctl samples; track the fractional prescaler in a static
        pre[t] += samples;
        uint32_t step = 1u << ctl;
        uint32_t inc = pre[t] / step; pre[t] %= step;
        if (!inc) continue;
        uint32_t ncnt = cnt + inc;
        if (ncnt > 0xFF) {
            R(0x28A0) |= (uint16_t)(0x40 << t);   // SCIPD
            R(0x28B8) |= (uint16_t)(0x40 << t);   // MCIPD
            ncnt &= 0xFF;
        }
        R(off) = (uint16_t)((R(off) & 0x700) | ncnt);
    }
    sh4_irq(from_arm);
    update_arm_irq();
}

void aica_update() {
    uint64_t dt = now - last_update;
    last_update = now;
    sample_frac += (double)dt * 44100.0 / (double)SH4_HZ;
    int s = (int)sample_frac; sample_frac -= s;
    if (threaded && realtime) {   // the ARM thread owns the sample clock: only deliver its interrupt changes
        if (sh4_irq_dirty.load(std::memory_order_relaxed)) { RegGuard g; if (sh4_irq_dirty.exchange(false)) update_sh4_irq(); }
        return;
    }
    if (threaded) {
        if (!s && !sh4_irq_dirty.load(std::memory_order_relaxed)) return;   // nothing to do: skip the lock
        RegGuard g;
        if (sh4_irq_dirty.exchange(false)) update_sh4_irq();
        if (s) { aica_samples += s; samples_due.fetch_add(s, std::memory_order_release); timer_tick(s); }
        return;
    }
    for (int i = 0; i < s; ++i) {
        aica_samples++;
        if (arm::is_running()) arm::run(ARM_INSTR_PER_SAMPLE);
        mix_one();
        timer_tick(1);
    }
}

void aica_set_threaded(bool on) { threaded = on; }
void aica_thread_state(uint64_t* due, uint64_t* done) { *due = samples_due.load(); *done = samples_done; }   // watchdog

// ARM thread body: one call runs the samples that are due; returns false when there was nothing to do.
// A backlog larger than max_backlog samples (the ARM thread fell far behind) is dropped.
bool aica_arm_thread_step(uint32_t max_backlog) {
    arm_park();
    if (int req = arm_request.exchange(0)) { if (req == 1) arm::reset(); else arm::stop(); }
    uint64_t due = samples_due.load(std::memory_order_acquire);
    if (due - samples_done > max_backlog) samples_done = due - max_backlog;
    if (samples_done >= due) return false;
    if (!arm::is_running()) { samples_done = due; return false; }
    while (samples_done < due) { arm::run(ARM_INSTR_PER_SAMPLE); mix_one(); samples_done++; }
    return true;
}
// Real-time mode (Vita): the ARM thread advances the AICA clock itself, paced by the audio output, so sound
// keeps playing at the right speed (no crackle) even when the SH-4 side runs slower than real time.
void aica_set_realtime(bool on) { realtime = on; }
void aica_arm_thread_run(uint32_t n) {
    arm_park();
    if (int req = arm_request.exchange(0)) { if (req == 1) arm::reset(); else arm::stop(); }
    { RegGuard g; aica_samples += n; timer_tick((int)n, true); }
    bool run = arm::is_running();
    for (uint32_t i = 0; i < n; ++i) { if (run) arm::run(ARM_INSTR_PER_SAMPLE); mix_one(); }
    samples_done += n;
}
void aica_timer_event() {}

void aica_init() {
    std::memset(regs, 0, sizeof regs);
    // RTC: seconds since 1950-01-01
    std::time_t now_s = getenv("DB_TAHASH") ? (std::time_t)1700000000 : std::time(nullptr);   // fixed clock for hash verification runs
    uint32_t secs = (uint32_t)(now_s + 631152000u);   // 1950->1970 offset (20 years incl. leap days)
    rtc_hi = secs >> 16; rtc_lo = secs & 0xFFFF;
    R(0x2C00) = 1;   // ARM held in reset
}

uint32_t aica_read(uint32_t addr, int size) {
    RegGuard g;
    return aica_read_locked(addr, size);
}
static uint32_t aica_read_locked(uint32_t addr, int size) {
    uint32_t p = addr & 0x1FFFFFFF;
    if (p >= 0x00710000) {
        if (p == 0x00710000) return rtc_hi;
        if (p == 0x00710004) return rtc_lo;
        return 0;
    }
    uint32_t off = p & 0x7FFF;
    if (off == 0x2800) return R(off) | 0x10;              // VER = 1 (read-only field)
    if (off == 0x2808) return R(off) | 0x0800;            // MIEMP: MIDI input FIFO empty
    static const bool nomon = getenv("DB_NOMON") != nullptr;   // debug: monitor registers read 0 (pre-mixer behaviour)
    if (off == 0x2810 || off == 0x2814) return nomon ? 0 : aica_mix_monitor(off);   // monitored channel LP/EG, position (CA)
    uint32_t v = R(off);
    if (size == 1) return (off & 1) ? (v >> 8) : (v & 0xFF);
    return v;
}

void aica_write(uint32_t addr, uint32_t v, int size) {
    RegGuard g;
    aica_write_locked(addr, v, size, false);
}
static void aica_write_locked(uint32_t addr, uint32_t v, int size, bool from_arm) {
    uint32_t p = addr & 0x1FFFFFFF;
    if (p >= 0x00710000) {
        if (p == 0x00710000) rtc_hi = v & 0xFFFF;
        else if (p == 0x00710004) rtc_lo = v & 0xFFFF;
        return;
    }
    uint32_t off = p & 0x7FFF;
    uint16_t val = (uint16_t)v;
    if (size == 1) {
        uint16_t old = R(off & ~1u);
        val = (off & 1) ? (uint16_t)((old & 0x00FF) | ((v & 0xFF) << 8)) : (uint16_t)((old & 0xFF00) | (v & 0xFF));
        off &= ~1u;
    }
    switch (off & ~3u) {
    case 0x289C: R(0x289C) = val; update_arm_irq(); return;              // SCIEB
    case 0x28A0: R(0x28A0) |= (val & 0x20); update_arm_irq(); return;     // SCIPD: only bit 5 settable (SH4->ARM)
    case 0x28A4: R(0x28A0) &= ~val; update_arm_irq(); return;             // SCIRE
    case 0x28B4: R(0x28B4) = val; sh4_irq(from_arm); return;               // MCIEB
    case 0x28B8: R(0x28B8) |= (val & 0x20); sh4_irq(from_arm); return;     // MCIPD
    case 0x28BC: R(0x28B8) &= ~val; sh4_irq(from_arm); return;             // MCIRE
    case 0x2890: case 0x2894: case 0x2898: R(off & ~3u) = val & 0x7FF; return;
    case 0x2C00: {
        bool was = arm_running;
        R(0x2C00) = val;
        arm_running = !(val & 1);
        if (arm_running && !was) { trace("[aica] ARM7 released from reset\n"); if (threaded) arm_request = 1; else arm::reset(); }
        if (!arm_running && was) { if (threaded) arm_request = 2; else arm::stop(); }
        return;
    }
    }
    if ((off & ~3u) < 0x2000 && (off & 0x7C) == 0 && (val & 0x8000)) {   // channel reg 0 with KYONEX: apply all KYONB
        R(off & ~3u) = val & 0x7FFF;
        aica_mix_reg_written(off & ~3u);
        aica_mix_kyonex();
        return;
    }
    if ((off & ~3u) >= 0x3400 && (off & ~3u) < 0x3C00) aica_dsp_dirty = true;   // DSP program changed: re-decode
    R(off & ~3u) = val;
    aica_mix_reg_written(off & ~3u);
}

bool aica_arm_running() { return arm_running; }

// ARM-side register access (the ARM sees the registers at 0x800000)
uint32_t aica_reg_read_arm(uint32_t off, int size) {
    RegGuard g;
    if ((off & ~3u) == 0x2D00) return reg_L;
    if ((off & ~3u) == 0x2D04) return 0;
    return aica_read_locked(0x00700000 + off, size);
}
void aica_reg_write_arm(uint32_t off, uint32_t v, int size) {
    RegGuard g;
    if ((off & ~3u) == 0x2D00) return;
    if ((off & ~3u) == 0x2D04) { if (v & 1) { e68k_out = false; update_e68k(); } return; }
    switch (off & ~3u) {
    case 0x28A0: R(0x28A0) |= (uint16_t)(v & 0x7F9); update_arm_irq(); return;    // SCIPD set (ARM side)
    case 0x28B8: R(0x28B8) |= (uint16_t)(v & 0x7FF); sh4_irq(true); return;       // MCIPD: ARM -> SH4 interrupt
    }
    aica_write_locked(0x00700000 + off, v, size, true);
}

void aica_state(SaveIO& s) {
    s.v(regs); s.v(last_update); s.v(sample_frac); s.v(rtc_hi); s.v(rtc_lo); s.v(arm_running); s.v(aica_samples);
    s.v(samples_done); s.v(reg_L); s.v(e68k_out); s.v(sci_pending); s.v(pend_L); s.v(pre);
    uint64_t due = samples_due.load(); s.v(due); if (s.load) { samples_due.store(due); sh4_irq_dirty = true; }
}
} // namespace hw
