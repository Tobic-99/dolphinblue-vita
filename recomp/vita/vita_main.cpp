// Dolphin Blue - PS Vita platform layer: boot, file locations, input, frame pacing, presentation.
// Game data is NOT part of the VPK; it is read from ux0:data/dolphinblue/ (see README.md).
#include "hw.h"
#include "draw.h"
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/audioout.h>
#include <atomic>
#include <psp2/kernel/modulemgr.h>
#include <psp2/io/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

int _newlib_heap_size_user = 128 * 1024 * 1024;   // real HW: this + the GXM blocks must fit ScePhyMemPartGame

void f_8c010000(sh4::Ctx* c);
void (*poll_hook)() = nullptr;

namespace hw {
extern void (*render_hook)(const FrameRecord&, uint32_t);
struct PadState { uint16_t buttons = 0xFFFF; uint8_t rt = 0, lt = 0, x = 0x80, y = 0x80; };
extern PadState pad[4];
extern uint32_t aw_coin_input;
}
using namespace hw;

static const char* BASE = "ux0:data/dolphinblue";

// ---- audio: the ARM7 thread mixes one stereo sample per AICA sample into this ring; the audio thread plays it
namespace hw { extern void (*aica_audio_sink)(int16_t, int16_t); extern bool aica_mix_off; extern int draw_skip_mode; }
static constexpr uint32_t AUDIO_RING = 1u << 14, AUDIO_GRAIN = 512, AUDIO_MAX_LAG = 8192, AUDIO_TARGET = 2048;   // frames
static uint32_t audio_ring[AUDIO_RING];                  // packed L | R << 16
static std::atomic<uint64_t> audio_w, audio_r;   // 64-bit: 32-bit wrapped after ~27 h causing fill check to see ring as always-full, silencing audio
static void audio_push(int16_t l, int16_t r) {
    uint64_t w = audio_w.load(std::memory_order_relaxed);
    if (w - audio_r.load(std::memory_order_acquire) >= AUDIO_RING) return;   // full: drop
    audio_ring[w & (AUDIO_RING - 1)] = (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
    audio_w.store(w + 1, std::memory_order_release);
}
static uint32_t prof_audio_underruns;
static uint64_t prof_worst_us, prof_slow_frames, prof_last_guest_us, prof_gpu_last_us;
static int audio_thread(SceSize, void*) {
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_GRAIN, 44100, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) { std::printf("[audio] open port failed %08x\n", port); return 0; }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    std::printf("[audio] 44.1 kHz stereo output on port %d\n", port);
    static uint32_t buf[2][AUDIO_GRAIN];
    for (int b = 0;; b ^= 1) {
        uint64_t r = audio_r.load(std::memory_order_relaxed), w = audio_w.load(std::memory_order_acquire);
        if (w - r > AUDIO_MAX_LAG) r = w - AUDIO_GRAIN;          // too far behind: drop the backlog (latency cap)
        uint32_t n = w - r < AUDIO_GRAIN ? w - r : AUDIO_GRAIN;
        for (uint32_t i = 0; i < n; ++i) buf[b][i] = audio_ring[(r + i) & (AUDIO_RING - 1)];
        if (n < AUDIO_GRAIN) { prof_audio_underruns++; for (uint32_t i = n; i < AUDIO_GRAIN; ++i) buf[b][i] = 0; }
        audio_r.store(r + n, std::memory_order_release);
        sceAudioOutOutput(port, buf[b]);                         // blocks until the hardware takes the grain
    }
    return 0;
}
namespace render { bool backend_dump_last(const char* path); void backend_format_test(); }
static volatile bool pacing_rebase;   // real time went on while paused: restart real-time pacing from here
namespace hw { extern uint64_t (*gd_clock)(); extern void (*gd_async)(void (*)()); }
// Disc reader (host/bios.cpp gd_async): GD-ROM reads run here while the drive reports BUSY, so the game keeps
// running during disc streaming instead of stalling in a synchronous read.
static SceUID gd_sem = -1; static void (*volatile gd_job)();
static int gd_reader_thread(SceSize, void*) { for (;;) { sceKernelWaitSema(gd_sem, 1, nullptr); if (auto j = gd_job) { gd_job = nullptr; j(); } } return 0; }
namespace hw {
void savestate_request_save(const char* path, uint32_t at_vblank); void savestate_request_load(const char* path);
extern void (*savestate_writer)(std::vector<uint8_t>* data, const std::string& path);
extern void (*savestate_done)(bool saved, bool ok);
bool savestate_write_file(const std::vector<uint8_t>& data, const std::string& path);
}
// Save states (host/savestate.cpp): ux0:data/dolphinblue/state.bin. The snapshot is taken at the game's frame-wait
// point and written to the file by a background thread, so the game does not stop while it writes (~28 MB).
static const char* kStatePath = "ux0:data/dolphinblue/state.bin";
static bool savestates_on;   // per-mission resume; ux0:data/dolphinblue/nosavestates turns it off
static std::vector<uint8_t>* state_pending; static std::string state_pending_path;
static std::atomic<bool> state_writing{false};
static SceUID state_sem = -1;
static void state_writer(std::vector<uint8_t>* data, const std::string& path) {
    if (state_writing.load()) { delete data; return; }   // the previous save is still being written
    state_pending = data; state_pending_path = path; state_writing = true;
    sceKernelSignalSema(state_sem, 1);
}
static int state_writer_thread(SceSize, void*) {
    for (;;) {
        sceKernelWaitSema(state_sem, 1, nullptr);
        uint64_t t = sceKernelGetProcessTimeWide();
        bool ok = hw::savestate_write_file(*state_pending, state_pending_path);
        std::printf("[state] %s written in %.1f s: %s\n", state_pending_path.c_str(), (sceKernelGetProcessTimeWide() - t) / 1e6, ok ? "ok" : "FAILED");
        delete state_pending; state_pending = nullptr; state_writing = false;
    }
    return 0;
}
extern bool nv_fast_xf; extern void nv_async_enable(void (*kick)()); extern void nv_xf_enable(void (*kick)(), bool strips); extern bool nv_worker_step(); extern uint64_t nv_async_calls, nv_async_units, nv_async_fallbacks, nv_xf_calls, nv_xf_verts; extern uint64_t (*nv_clock)(); extern uint64_t nv_time[4], nv_units[2], nv_count[3][2];
static uint64_t nv_clock_vita() { return sceKernelGetProcessTimeWide(); }
namespace hw { void aica_thread_state(uint64_t* due, uint64_t* done); namespace arm { uint32_t pc(); extern bool idle_skip; } }
// Optional test script ux0:data/dolphinblue/autotest.txt (not needed for normal play):
//   "start A B"  hold START from guest vblank A to B      "shot N"  dump the frame presented after vblank N
#ifdef DB_PGO_GEN
extern "C" void db_pgo_dump(void);
extern "C" void __gcov_exit(void) {}   // the vitasdk libgcov has no exit hook; the profile is written by "pgodump"
#endif
static uint32_t script_pgo_dump;   // autotest "pgodump N": write the PGO profile at vblank N (instrumented builds)
struct Press { uint32_t a, b, mask; }; static std::vector<Press> script_start; static std::vector<uint32_t> script_shots; static uint32_t script_shot_every;
static void load_autotest() {
    FILE* f = std::fopen("ux0:data/dolphinblue/autotest.txt", "r");
    if (!f) return;
    // lines: "FROM TO BUTTON" (the desktop --input format: start|a|b|c|x|y|up|down|left|right), "start A B" (old),
    // "shot N" (screenshot after vblank N), "shotevery N" (a screenshot every N vblanks)
    static const struct { const char* n; uint16_t m; } btn[] = {{"c", 1 << 0}, {"b", 1 << 1}, {"a", 1 << 2}, {"start", 1 << 3},
        {"up", 1 << 4}, {"down", 1 << 5}, {"left", 1 << 6}, {"right", 1 << 7}, {"z", 1 << 8}, {"y", 1 << 9}, {"x", 1 << 10},
        {"service", 1 << 13}, {"test", 1 << 14}};
    char line[128];
    while (std::fgets(line, sizeof line, f)) {
        char w[32]; unsigned a, b;
        if (std::sscanf(line, "%u %u %31s", &a, &b, w) == 3) {
            for (auto& x : btn) if (!std::strcmp(w, x.n)) script_start.push_back({a, b, x.m});
        } else if (std::sscanf(line, "start %u %u", &a, &b) == 2) script_start.push_back({a, b, 1u << 3});
        else if (std::sscanf(line, "shotevery %u", &a) == 1) script_shot_every = a;
        else if (std::sscanf(line, "pgodump %u", &a) == 1) script_pgo_dump = a;
        else if (std::sscanf(line, "shot %u", &a) == 1) script_shots.push_back(a);
    }
    std::fclose(f);
    if (script_shot_every) sceIoMkdir("ux0:data/dolphinblue/shots", 0777);
    std::printf("[vita] autotest: %u presses, %u shots, shot every %u\n", (unsigned)script_start.size(), (unsigned)script_shots.size(), script_shot_every);
}
uint64_t guest_busy_us_total;   // guest-thread time excluding pacing sleeps (draw-skip policy)
static volatile int guest_sleeping;   // sampler: do not count pacing sleeps as guest work
static uint64_t t0_us;
static uint32_t last_vbl;
static uint64_t frames_presented;
uint64_t frames_presented_dbg;

static void update_input() {
    SceCtrlData c{};
    sceCtrlPeekBufferPositive(0, &c, 1);
    uint16_t m = 0xFFFF;   // Dreamcast pad bits, active low
    auto on = [&](uint32_t vita, int bit) { if (c.buttons & vita) m &= ~(1u << bit); };
    on(SCE_CTRL_CIRCLE, 1); on(SCE_CTRL_CROSS, 2); on(SCE_CTRL_START, 3);
    on(SCE_CTRL_UP, 4); on(SCE_CTRL_DOWN, 5); on(SCE_CTRL_LEFT, 6); on(SCE_CTRL_RIGHT, 7);
    on(SCE_CTRL_SQUARE, 10); on(SCE_CTRL_TRIANGLE, 9); on(SCE_CTRL_LTRIGGER, 0); on(SCE_CTRL_RTRIGGER, 8);
    // left stick doubles as the d-pad
    if (c.lx < 64) m &= ~(1u << 6); if (c.lx > 192) m &= ~(1u << 7);
    if (c.ly < 64) m &= ~(1u << 4); if (c.ly > 192) m &= ~(1u << 5);
    for (auto& p : script_start) if (vblank_count >= p.a && vblank_count <= p.b) m &= ~p.mask;
#ifdef DB_PGO_GEN
    if (script_pgo_dump && vblank_count == script_pgo_dump) { std::printf("[pgo] writing the profile\n"); db_pgo_dump(); std::printf("[pgo] profile written\n"); std::fflush(stdout); }
#endif
    {   // save states: automatic save at the start of each mission. The game's mission counter (RAM 0x8C3C1516,
        // 0 on the title screen) goes up by one when a new mission starts, so a resume begins at that mission.
        static uint8_t mission_prev = 0xFF;
        if (savestates_on) {
            uint8_t mission = ram[0x3C1516];
            if (mission_prev != 0xFF && mission > mission_prev && !state_writing) {
                hw::savestate_request_save(kStatePath, 0); std::printf("[state] mission %u reached: saving\n", (unsigned)mission + 1);
            }
            mission_prev = mission;
        }
    }
    pad[0].buttons = m; pad[0].x = c.lx; pad[0].y = c.ly;
    aw_coin_input = 0xF;   // (no coin button: free play; SELECT pauses)
}

namespace hw { void aica_hold(bool on); }
// SELECT (short press, alone): pause / resume. The guest and the sound CPU stop; the last frame stays on screen.
// SELECT + START held 2 s: delete the save state (the next launch starts fresh).
static void pause_and_combos() {
    SceCtrlData c{}; sceCtrlPeekBufferPositive(0, &c, 1);
    static bool sel_down, sel_other; static uint64_t sel_t0, reset_t0;
    uint64_t now = sceKernelGetProcessTimeWide();
    bool sel = c.buttons & SCE_CTRL_SELECT, others = c.buttons & ~SCE_CTRL_SELECT & 0xFFFF;
    // reset the save state
    if (sel && (c.buttons & SCE_CTRL_START)) {
        if (!reset_t0) reset_t0 = now;
        else if (now - reset_t0 > 2000000) {
            reset_t0 = ~0ull >> 1;   // once per hold
            int r = std::remove(kStatePath);
            std::printf("[state] SELECT+START: save state deleted (%08x)\n", (unsigned)r);
        }
    } else reset_t0 = 0;
    // pause on a short, lone SELECT press (combos with other buttons don't pause)
    if (sel && !sel_down) { sel_down = true; sel_other = false; sel_t0 = now; }
    if (sel && others) sel_other = true;
    if (!sel && sel_down) {
        sel_down = false;
        if (!sel_other && now - sel_t0 < 700000) {
            std::printf("[vita] paused\n"); std::fflush(stdout);
            hw::aica_hold(true);
            for (bool released = false;;) {   // wait for the next SELECT press
                sceKernelDelayThread(16667);
                if ((sceKernelGetProcessTimeWide() / 16667) % 32 == 0) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
                SceCtrlData p{}; sceCtrlPeekBufferPositive(0, &p, 1);
                bool s = p.buttons & SCE_CTRL_SELECT;
                if (!s) released = true; else if (released) { while (true) { sceKernelDelayThread(16667); sceCtrlPeekBufferPositive(0, &p, 1); if (!(p.buttons & SCE_CTRL_SELECT)) break; } break; }
            }
            hw::aica_hold(false);
            pacing_rebase = true;   // real time went on while paused
            std::printf("[vita] resumed\n"); std::fflush(stdout);
        }
    }
    // periodic save state every 3 minutes of play (on top of the save at each new mission)
    static uint32_t last_periodic;
    if (savestates_on && vblank_count > 2400 && vblank_count - last_periodic >= 10800 && !state_writing) {
        last_periodic = vblank_count; hw::savestate_request_save(kStatePath, 0); std::printf("[state] 3-minute save\n");
    }
}
static void on_poll() {
    if (vblank_count == last_vbl) return;
    last_vbl = vblank_count;
    pause_and_combos();
    update_input();
    if ((vblank_count & 31) == 0) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);   // reset every idle timer (screen dim/off and suspend) while the game runs
    {   // High scores: the game keeps them only in RAM (it never writes SRAM, flash or a VMU), 10 entries of 16 bytes
        // at 0x8C23F4E4, initialised at boot from the defaults at 0x8C23F594. Persisted in ux0:data/dolphinblue/hiscores.bin.
        static constexpr uint32_t kLive = 0x23F4E4, kDefault = 0x23F594, kLen = 160;
        static uint8_t saved[kLen]; static bool have_saved, loaded, restored;
        if (!loaded) {
            loaded = true;
            if (FILE* f = std::fopen("ux0:data/dolphinblue/hiscores.bin", "rb")) { have_saved = std::fread(saved, 1, kLen, f) == kLen; std::fclose(f); }
            std::printf("[save] hiscores.bin %s\n", have_saved ? "loaded" : "not found (new)");
        }
        uint8_t* live = ram + kLive;
        bool live_is_default = std::memcmp(live, ram + kDefault, kLen) == 0 && ((const uint32_t*)live)[0] != 0;
        if (have_saved && !restored && live_is_default) {   // the game has just set up its table: put the saved one in
            std::memcpy(live, saved, kLen); restored = true; std::printf("[save] high scores restored\n");
        }
        if ((vblank_count % 120) == 0 && ((const uint32_t*)live)[0] != 0 && (restored || !have_saved) && std::memcmp(live, saved, kLen) != 0) {
            if (FILE* f = std::fopen("ux0:data/dolphinblue/hiscores.bin", "wb")) {
                bool ok = std::fwrite(live, 1, kLen, f) == kLen; std::fclose(f);
                if (ok) { std::memcpy(saved, live, kLen); have_saved = true; restored = true; std::printf("[save] high scores written\n"); }
            }
        }
    }
    if ((vblank_count % 300) == 0) {   // every ~5 s: write the SRAM back if the game changed it
        static uint64_t saved_hash;
        uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < 128 * 1024; i += 8) { uint64_t w; std::memcpy(&w, sram + i, 8); h = (h ^ w) * 1099511628211ull; }
        if (!saved_hash) saved_hash = h;   // first check: the state just loaded
        else if (h != saved_hash) {
            if (FILE* f = std::fopen("ux0:data/dolphinblue/sram.bin.tmp", "wb")) {
                bool ok = std::fwrite(sram, 1, 128 * 1024, f) == 128 * 1024; std::fclose(f);
                if (ok) { std::remove("ux0:data/dolphinblue/sram.bin"); std::rename("ux0:data/dolphinblue/sram.bin.tmp", "ux0:data/dolphinblue/sram.bin"); saved_hash = h; std::printf("[save] sram.bin written\n"); }
            }
        }
    }   // keep the screen on and the Vita awake while the game runs
    // pace guest VBlanks to real time (60 Hz); if we are behind, run as fast as possible
    if (pacing_rebase) { pacing_rebase = false; t0_us = sceKernelGetProcessTimeWide() - (uint64_t)vblank_count * 16667ull; }   // real time went on while paused
    uint64_t target = t0_us + (uint64_t)vblank_count * 16667ull;
    uint64_t now_us = sceKernelGetProcessTimeWide();
    // Adaptive draw skip (holly.cpp): when the guest falls behind real time, present every other frame and let
    // the native vertex writers skip the frames that will not be shown; back to every frame when it is light.
    {
        static uint32_t window, late;
        static uint64_t guest_mark;
        if (now_us > target + 2000) ++late;
        if (++window == 120) {
            uint64_t busy = guest_busy_us_total - guest_mark; guest_mark = guest_busy_us_total;
            double per_vbl_ms = busy / 1000.0 / 120.0;
            // renders per vblank: a 60 Hz scene renders every vblank (~1.0); a 30 Hz scene every 2nd (~0.5). Skipping
            // every other list of a 30 Hz scene would show 15 fps, so the skip is only used for 60 Hz scenes.
            static uint32_t renders_mark; double rate = (render_count - renders_mark) / 120.0; renders_mark = render_count;
            if (hw::draw_skip_mode && rate < 0.7) hw::draw_skip_mode = 0;
            else if (!hw::draw_skip_mode && late > 12 && rate >= 0.7) hw::draw_skip_mode = 1;
            else if (hw::draw_skip_mode && late == 0 && per_vbl_ms < 11.0) hw::draw_skip_mode = 0;
            window = late = 0;
        }
    }
    static uint64_t busy_from;
    if (busy_from) guest_busy_us_total += now_us - busy_from;     // guest time since the last pacing point
    if (now_us + 1000 < target) { guest_sleeping = 1; sceKernelDelayThread((SceUInt)(target - now_us)); guest_sleeping = 0; }
    else if (now_us > target + 50000) t0_us = now_us - (uint64_t)vblank_count * 16667ull;   // behind by more than 3 frames: drop the lost time instead of a sped-up catch-up burst
    busy_from = sceKernelGetProcessTimeWide();
}

// Render thread (core 1): TA decode, texture conversion and GXM submission run beside the guest (core 0).
// The guest hands over each STARTRENDER's TA stream and continues; it only waits if the previous frame is
// still being built (one frame in flight). Textures are read from live VRAM; a page the guest rewrites
// meanwhile bumps vram_page_gen, so a texture converted mid-write is re-converted next frame.
// Timing (printed every 60 presents): guest = guest-thread time per frame excluding handover waits,
// wait = guest blocked on the render thread, build = TA decode + textures, gpu = sceGxmFinish wait.
uint64_t prof_gpu_us;
static volatile uint64_t prof_arm_busy_us;   // ARM7 thread (core 2) time spent running the driver
namespace render { void backend_wait_gpu(); }
static uint64_t prof_guest_last, prof_guest, prof_handover, prof_build, prof_submit, prof_t0, prof_verts, prof_batches;
static FrameRecord pending;           // owned by the render thread from sem_work until it goes idle (build_idle)
static uint32_t pending_idx, pending_vbl;
static SceUID sem_work;
volatile int render_stage; volatile uint32_t render_dbg_verts, render_dbg_batches;   // watchdog: 0 idle, 1 gpu wait, 2 build, 3 submit, 4 shots/prof (backend sets 10+ inside submit)

static uint32_t prof_skipped;
static std::vector<uint32_t> pending_regs(0x2000 / 4);   // PVR regs incl. fog table (0x200) and palette (0x1000)
namespace hw { extern bool ta_handoff; void ta_handoff_swap(uint32_t** buf, size_t* cap); extern bool (*ta_skip_query)(); }
static uint32_t* pend_ta; static size_t pend_cap, pend_words;   // the frame the render thread draws (TA stream handed off by holly)
struct NextSlot { uint32_t* ta = nullptr; size_t cap = 0, words = 0; std::vector<uint32_t> regs = std::vector<uint32_t>(0x2000 / 4); uint32_t frame = 0, idx = 0, vbl = 0; };
static NextSlot next_slot; static volatile bool next_valid; static bool build_idle = true;  /* both under next_lock */ static std::atomic_flag next_lock = ATOMIC_FLAG_INIT;   // one queued frame
static void on_render(const FrameRecord& fr, uint32_t idx) {
    uint64_t a = sceKernelGetProcessTimeWide();
    if (prof_guest_last) { prof_guest += a - prof_guest_last; prof_last_guest_us = a - prof_guest_last; }
    // Render thread busy: queue this frame in the "next" slot (replacing an older queued one, which is the only
    // frame ever dropped); the render thread starts it as soon as it finishes. The guest never waits.
    // The idle check and the queueing happen under next_lock, the same lock the build thread holds when it
    // decides to go idle; otherwise a frame queued just as it went idle stayed queued forever (and with the
    // demand draw skip every later list was skipped: the picture froze while the game ran on).
    while (next_lock.test_and_set(std::memory_order_acquire)) {}
    bool idle = build_idle; if (idle) build_idle = false;
    if (!idle) {
        static std::vector<uint32_t> staging(0x2000 / 4);
        std::memcpy(staging.data(), pvr_regs_ptr(), 0x2000);
        hw::ta_handoff_swap(&next_slot.ta, &next_slot.cap);   // holly gets the dropped (older) queued buffer back
        next_slot.words = fr.ta_words; next_slot.regs.swap(staging);
        next_slot.frame = fr.frame; next_slot.idx = idx; next_slot.vbl = vblank_count;
        bool replaced = next_valid; next_valid = true;
        next_lock.clear(std::memory_order_release);
        if (replaced) prof_skipped++;
        prof_guest_last = sceKernelGetProcessTimeWide();
        return;
    }
    next_lock.clear(std::memory_order_release);
    uint64_t b = sceKernelGetProcessTimeWide();
    prof_handover += b - a;
    hw::ta_handoff_swap(&pend_ta, &pend_cap);   // zero-copy: take the live TA buffer, give back the one the render thread finished
    pend_words = fr.ta_words;
    std::memcpy(pending_regs.data(), pvr_regs_ptr(), 0x2000);   // the render thread must not read live registers
    pending.frame = fr.frame;
    pending_idx = idx; pending_vbl = vblank_count;
    sceKernelSignalSema(sem_work, 1);
    prof_guest_last = sceKernelGetProcessTimeWide();
}

// Rendering is split in two threads with two frame slots between them (Grarak's "pipelined draw"):
//   build thread (core 2): TA stream -> draw list (decode + textures) for frame N+1,
//   submit thread (core 1): waits for the GPU, uploads and submits frame N.
// A frame then costs max(build, GPU + submit) instead of their sum.
struct BuiltFrame { render::DrawList dl; uint32_t vbl = 0, idx = 0; uint64_t build_us = 0, gpu_us = 0; };
static BuiltFrame built[2];
static SceUID sem_slot_free, sem_slot_ready;
static std::atomic<int> slots_busy{0};
namespace render { extern void (*backend_flush_barrier)(); }
static void flush_barrier() {   // the texture cache is about to be dropped: no frame may be pending or on the GPU
    while (slots_busy.load(std::memory_order_acquire) > 0) sceKernelDelayThread(500);
    render::backend_wait_gpu();
}
static int build_thread(SceSize, void*) {
    for (unsigned k = 0;; k ^= 1) {
        render_stage = 0;
        static bool have_next;
        if (!have_next) sceKernelWaitSema(sem_work, 1, nullptr);
        sceKernelWaitSema(sem_slot_free, 1, nullptr);
        render_stage = 2;
        uint64_t a = sceKernelGetProcessTimeWide();
        if (!prof_t0) prof_t0 = a;
        render::build_drawlist(pend_ta, pend_words, pending_regs.data(), built[k].dl);
        built[k].build_us = sceKernelGetProcessTimeWide() - a;
        built[k].vbl = pending_vbl; built[k].idx = pending_idx;
        render_dbg_verts = (uint32_t)built[k].dl.verts.size();
        slots_busy.fetch_add(1);
        sceKernelSignalSema(sem_slot_ready, 1);
        // a frame queued while this one was drawn: take it now instead of going idle
        while (next_lock.test_and_set(std::memory_order_acquire)) {}
        have_next = next_valid;
        if (have_next) {
            std::swap(pend_ta, next_slot.ta); std::swap(pend_cap, next_slot.cap); pend_words = next_slot.words;
            pending_regs.swap(next_slot.regs);
            pending.frame = next_slot.frame; pending_idx = next_slot.idx; pending_vbl = next_slot.vbl;
            next_valid = false;
        } else build_idle = true;   // decided under next_lock (see on_render)
        next_lock.clear(std::memory_order_release);
    }
    return 0;
}
static int render_thread(SceSize, void*) {   // the submit thread
    static std::vector<uint32_t> dummy;
    for (unsigned k = 0;; k ^= 1) {
        sceKernelWaitSema(sem_slot_ready, 1, nullptr);
        render::DrawList& dl = built[k].dl;
        const uint32_t pending_vbl = built[k].vbl, pending_idx = built[k].idx; (void)pending_idx;
        uint64_t a = sceKernelGetProcessTimeWide();
        { uint64_t g0 = sceKernelGetProcessTimeWide(); render::backend_wait_gpu(); prof_gpu_last_us = sceKernelGetProcessTimeWide() - g0; }
        render_stage = 3;
        render_dbg_batches = (uint32_t)(dl.lists[0].size() + dl.lists[1].size() + dl.lists[2].size() + dl.lists[3].size() + dl.lists[4].size());
        uint64_t b = a + built[k].build_us;   // (for the stats below: build time of this frame)
        uint64_t sb = sceKernelGetProcessTimeWide();
        render::backend_render(dl, dummy);
        uint64_t c = sceKernelGetProcessTimeWide();
        render_stage = 4;
        prof_build += built[k].build_us + prof_gpu_last_us; prof_submit += c - sb;
        (void)b;
        prof_verts += dl.verts.size(); for (auto& l : dl.lists) prof_batches += l.size();
        frames_presented++; frames_presented_dbg = frames_presented;
        if (script_shot_every) {   // periodic screenshots (autoplay bug review), named by vblank
            static uint32_t next_shot = script_shot_every;
            if (pending_vbl >= next_shot) {
                char p[128]; std::snprintf(p, sizeof p, "ux0:data/dolphinblue/shots/v%06u.rgba", pending_vbl);
                render::backend_dump_last(p);
                next_shot = pending_vbl + script_shot_every;
            }
        }
        for (auto& n : script_shots)
            if (n && pending_vbl >= n) {
                char p[128]; std::snprintf(p, sizeof p, "ux0:data/dolphinblue/shot_%05u.rgba", n);
                render::backend_dump_last(p);
                std::printf("[vita] screenshot %s (render %u)\n", p, pending_idx);
                n = 0;
            }
        slots_busy.fetch_sub(1);
        sceKernelSignalSema(sem_slot_free, 1);
    }
    return 0;
}

static bool load(const std::string& path, uint8_t* dst, size_t max) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    size_t n = std::fread(dst, 1, max, f);
    std::fclose(f);
    return n > 0;
}

static int game_thread(SceSize, void*) {
    stdout = stderr;
    static std::vector<uint8_t> m_vram(VRAM_SIZE), m_aram(ARAM_SIZE), m_oc(8192), m_sram(128 * 1024, 0xFF);
    hw::ram = sh4::ram; vram = m_vram.data(); aram = m_aram.data(); ocram = m_oc.data(); sram = m_sram.data();
    // Saves: the board's 128 KB SRAM (scores, settings) persists in ux0:data/dolphinblue/sram.bin
    if (FILE* f = std::fopen("ux0:data/dolphinblue/sram.bin", "rb")) { size_t n = std::fread(sram, 1, m_sram.size(), f); std::fclose(f); std::printf("[save] sram.bin loaded (%u bytes)\n", (unsigned)n); }
    std::string base = BASE;
    if (!load(base + "/IP.BIN", ram + 0x8000, 0x8000)) fatal("missing %s/IP.BIN", BASE);
    if (!load(base + "/1ST_READ.BIN", ram + 0x10000, RAM_SIZE - 0x10000)) fatal("missing %s/1ST_READ.BIN", BASE);
    if (!disc_open(base + "/disc")) fatal("no .gdi found in %s/disc", BASE);
    if (!render::backend_init(640, 480)) fatal("GXM init failed");
    { SceIoStat st_; if (sceIoGetstat("ux0:data/dolphinblue/fmttest", &st_) >= 0) { render::backend_format_test(); } }
    // ARM7 sound CPU (thread created below on core 1): runs the samples the guest's clock has made due, and mixes the audio
    aica_set_threaded(true); aica_set_realtime(true);
    {   // save states: written by a background thread (cores 1+2) so the game does not stall on the ~28 MB write
        state_sem = sceKernelCreateSema("statesave", 0, 0, 1, nullptr);
        SceUID wt = sceKernelCreateThread("statewriter", state_writer_thread, 0x70, 64 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2, nullptr);
        if (state_sem >= 0 && wt >= 0) { sceKernelStartThread(wt, 0, nullptr); hw::savestate_writer = state_writer; }
        SceCtrlData cd{}; sceCtrlPeekBufferPositive(0, &cd, 1);
        SceIoStat st_;
        savestates_on = sceIoGetstat("ux0:data/dolphinblue/nosavestates", &st_) < 0;   // mission saves on (the file nosavestates turns them off)
        if (savestates_on && sceIoGetstat(kStatePath, &st_) >= 0) {
            if (cd.buttons & SCE_CTRL_SELECT) std::printf("[state] SELECT held: starting fresh (state.bin kept until the next save)\n");
            else { hw::savestate_request_load(kStatePath); std::printf("[state] resuming from state.bin\n"); }
            hw::savestate_done = [](bool saved, bool ok) { if (!saved && ok) pacing_rebase = true; };
        }
    }
    hw::arm::idle_skip = true;   // MANATEE.DRV main-loop idle skip (arm7.cpp); ux0:data/dolphinblue/noarmidle turns it off
    { SceIoStat st_; if (sceIoGetstat("ux0:data/dolphinblue/noarmidle", &st_) >= 0) { hw::arm::idle_skip = false; std::printf("[audio] noarmidle: ARM7 idle skip off\n"); } }
    nv_clock = nv_clock_vita;   // [vtx] timing of the native vertex ports
    hw::gd_clock = nv_clock_vita;
    {   // asynchronous disc reads (ux0:data/dolphinblue/gdsync keeps them synchronous)
        SceIoStat st_;
        if (sceIoGetstat("ux0:data/dolphinblue/gdsync", &st_) < 0) {
            gd_sem = sceKernelCreateSema("gdread", 0, 0, 1, nullptr);
            SceUID t = sceKernelCreateThread("gdreader", gd_reader_thread, 0x58, 64 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
            if (gd_sem >= 0 && t >= 0) { sceKernelStartThread(t, 0, nullptr); hw::gd_async = [](void (*job)()) { gd_job = job; sceKernelSignalSema(gd_sem, 1); }; std::printf("[gd] asynchronous disc reads\n"); }
        }
    }

    { SceIoStat st_; if (sceIoGetstat("ux0:data/dolphinblue/fastxf", &st_) >= 0) { nv_fast_xf = true; std::printf("[vtx] fastxf: single-precision transforms\n"); } }   // MANATEE.DRV main-loop idle skip (see arm7.cpp)
    hw::aica_audio_sink = audio_push;
    { if (FILE* f = std::fopen("ux0:data/dolphinblue/nomix", "rb")) { std::fclose(f); hw::aica_mix_off = true; std::printf("[audio] nomix: mixing disabled\n"); } }
    // Core layout: core 0 the game, core 1 render + audio output + ARM7 sound, core 2 the build thread.
    // Nothing runs on core 3 (SCE_KERNEL_CPU_MASK_SYSTEM): that is the OS's own core, and app threads there got
    // preempted by the system (audio loss) and could stall it in heavy scenes. The ARM7 shares core 1 with the
    // render/audio threads, which block on the GPU and the audio hardware, so it gets their idle time.
    SceUID aut = sceKernelCreateThread("audio", audio_thread, 0x40, 64 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    std::printf("[vita] sound threads: audio + arm7 on core 1, build on core 2\n");
    if (aut >= 0) sceKernelStartThread(aut, 0, nullptr); else std::printf("[audio] thread create failed %08x\n", aut);
    static SceKernelThreadEntry arm7_entry = [](SceSize, void*) -> int {
        // real time: keep about AUDIO_TARGET frames queued for the audio thread, whatever the guest's speed
        for (;;) {
            uint64_t fill = audio_w.load(std::memory_order_acquire) - audio_r.load(std::memory_order_acquire);
            if (fill >= AUDIO_TARGET && !hw::aica_mix_off) { sceKernelDelayThread(2000); continue; }
            uint64_t t = sceKernelGetProcessTimeWide();
            aica_arm_thread_run(64);
            prof_arm_busy_us += sceKernelGetProcessTimeWide() - t;
            if (hw::aica_mix_off) sceKernelDelayThread(1451);   // no ring to pace on: 64 samples of wall time
        }
        return 0;
    };
    // ARM7 on core 1 (not core 2): the build thread keeps core 2 busy with TA decode + textures, while core 1's
    // render/audio threads block on the GPU and audio hardware, leaving the idle time the ARM7 mixer needs.
    SceUID at = sceKernelCreateThread("arm7", arm7_entry, 0x50, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    if (at < 0) fatal("arm7 thread create failed %08x", at);
    sceKernelStartThread(at, 0, nullptr);
    // vertex worker on core 2 (native_vtx.cpp): offloads the strip writer (strip_async) and/or the 3D vertex
    // transformers (xf_async) to the otherwise idle core 2. Switches: ux0:data/dolphinblue/nvasync (strips only),
    // ux0:data/dolphinblue/xfasync (transforms only); both flags offload all three.
    { SceIoStat st_;
      bool strips = sceIoGetstat("ux0:data/dolphinblue/nvasync", &st_) >= 0;
      bool xf     = sceIoGetstat("ux0:data/dolphinblue/xfasync", &st_) >= 0;
      if (strips || xf) {   // opt-in; the worker shares core 2 with the build thread
        static SceUID nv_sem = sceKernelCreateSema("nvwork", 0, 0, 1, nullptr);
        SceUID wt = sceKernelCreateThread("vtxworker", [](SceSize, void*) -> int {
            for (;;) if (!nv_worker_step()) sceKernelWaitSema(nv_sem, 1, nullptr);
            return 0;
        }, 0x52, 64 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
        if (nv_sem >= 0 && wt >= 0) {
            sceKernelStartThread(wt, 0, nullptr);
            if (xf && strips) nv_xf_enable([] { sceKernelSignalSema(nv_sem, 1); }, true);
            else if (xf)      nv_xf_enable([] { sceKernelSignalSema(nv_sem, 1); }, false);
            else              nv_async_enable([] { sceKernelSignalSema(nv_sem, 1); });
            std::printf("[vtx] vertex worker on core 2 (strips=%d xf=%d)\n", (int)strips, (int)xf);
        }
        else std::printf("[vtx] worker thread create failed %08x %08x\n", (unsigned)nv_sem, (unsigned)wt);
      } }
    sem_work = sceKernelCreateSema("render_work", 0, 0, 1, nullptr);
    sem_slot_free = sceKernelCreateSema("slot_free", 0, 2, 2, nullptr);
    sem_slot_ready = sceKernelCreateSema("slot_ready", 0, 0, 2, nullptr);
    render::backend_flush_barrier = flush_barrier;
    {   // the build thread on core 2 (pipelined with the render/submit thread on core 1)
        SceUID bt = sceKernelCreateThread("build", build_thread, 0x48, 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
        if (bt < 0) fatal("build thread create failed %08x", bt);
        sceKernelStartThread(bt, 0, nullptr);
    }
    SceUID rt = sceKernelCreateThread("render", render_thread, 0x48, 8 * 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);   // 8 MB: vitashark compiles shader variants here
    if (rt < 0) fatal("render thread create failed %08x", rt);
    sceKernelStartThread(rt, 0, nullptr);

    static sh4::Ctx c{};
    cpu = &c;
    c.sr = 0x700000F0u; c.t = 0;
    c.r[15] = 0x8C00F400u; c.vbr = 0x8C00F400u; c.fpscr = 0x00040001u; c.slice = SLICE_POLLS;
    sh4p_init(); pvr_init(); aica_init();
    bios_install(&c);
    hle_register(0x8C00F400u, [](sh4::Ctx* cc) { if (sh4::rd16(cc->vbr) != 0x002B) fatal("VBR+0 not rte"); sh4::rte(cc); });
    load_autotest();
    poll_hook = on_poll;
    render_hook = on_render; hw::ta_handoff = true;
    // Draw skip by demand: a TA list whose frame could not be shown anyway (the renderer already has a finished
    // frame waiting and the display is capped at 30) is walked without building vertices. The screen keeps
    // getting every frame it can show; the game thread saves the work of the others.
    hw::ta_skip_query = [] { return (bool)next_valid; };
    t0_us = sceKernelGetProcessTimeWide();
    std::printf("[vita] starting guest at 0x8C010000\n"); std::fflush(stdout);
    f_8c010000(&c);
    fatal("guest entry returned");
    return 0;
}

int main() {
    sceIoMkdir(BASE, 0777);
    stdout = stderr;   // stderr reaches the TTY (Vita3K log / console)
    std::printf("Dolphin Blue (native SH-4 recompilation) - Vita build\n");
    scePowerSetArmClockFrequency(444); scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222); scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    // lowest priority of our threads: GXM's display-queue thread (created by sceGxmInitialize on this thread, so it
    // shares its core) and the system must be able to preempt the always-busy guest, or flips stall
    SceUID th = sceKernelCreateThread("guest", game_thread, 0x60, 16 * 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_0, nullptr);
    if (th < 0) { std::printf("thread create failed %08x\n", th); return 1; }
    sceKernelStartThread(th, 0, nullptr);
    sceKernelWaitThreadEnd(th, nullptr, nullptr);
    sceKernelExitProcess(0);
    return 0;
}
// vita-elf-create needs room after segment 0 for its import stubs; some layouts end just short of a page.
// Resize this if a build stops with "Cannot allocate ... SCE data at end of segment 0; segment 1 overlaps".
__attribute__((used)) static const unsigned char db_seg0_pad_main[4096] = {1};
