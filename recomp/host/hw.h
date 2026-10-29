// Dolphin Blue host runtime: shared hardware-layer declarations (Linux desktop build).
// Own implementation. Hardware behaviour follows the SH7750 / Holly / AICA documentation;
// Flycast (GPL-2) was consulted only as a behavioural reference, no code was copied.
#pragma once
#include "sh4ctx.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hw {

// ---- memories --------------------------------------------------------------------------------
extern uint8_t* ram;      // 16 MB main RAM (area 3)
extern uint8_t* vram;     // 8 MB video RAM, stored in 64-bit-path (texture) layout
extern uint8_t* aram;     // 2 MB AICA sound RAM
extern uint8_t* ocram;    // 8 KB operand-cache RAM (0x7C000000)
extern uint8_t* sram;     // 128 KB area-0 SRAM/flash (0x00200000)
constexpr uint32_t RAM_SIZE = 16u << 20, VRAM_SIZE = 8u << 20, ARAM_SIZE = 2u << 20;

uint32_t vram32_to_64(uint32_t off32);
extern uint32_t vram_page_gen[2048];       // bumped on every VRAM write (texture cache invalidation)
extern uint32_t palette_version;
uint32_t* pvr_regs_ptr();    // 32-bit path offset -> storage offset
uint32_t bus_read32(uint32_t a);          // full memory map (for DMA engines)
void bus_write32(uint32_t a, uint32_t v);
uint32_t dmac_reg(uint32_t a);
void dmac_set(uint32_t a, uint32_t v);
size_t unhandled_count();
uint64_t irq_count(uint32_t ev);

// ---- time ------------------------------------------------------------------------------------
constexpr uint64_t SH4_HZ = 200000000ull;
extern uint64_t now;                       // SH-4 cycles of virtual time
constexpr int POLL_CYCLES = 64;            // virtual cycles charged per SH4_POLL
constexpr int SLICE_POLLS = 64;            // polls per scheduler slice
void sched_add(int id, uint64_t when);     // one-shot event at absolute time
void sched_cancel(int id);
void sched_run();                          // run due events
enum EvId { EV_VBLANK_IN, EV_VBLANK_OUT, EV_HBLANK, EV_RENDER_DONE, EV_CH2_DMA, EV_MAPLE_DMA,
            EV_GD_DMA, EV_AICA_TIMER, EV_PVR_DMA, EV_AICA_DMA, EV_COUNT };

// ---- interrupts ------------------------------------------------------------------------------
// Holly normal/external/error interrupt bits
enum : uint32_t {
    HN_RENDER_VIDEO = 1u << 0, HN_RENDER_ISP = 1u << 1, HN_RENDER_TSP = 1u << 2,
    HN_VBLANK_IN = 1u << 3, HN_VBLANK_OUT = 1u << 4, HN_HBLANK = 1u << 5,
    HN_TA_OPAQUE = 1u << 7, HN_TA_OPAQUE_MOD = 1u << 8, HN_TA_TRANS = 1u << 9, HN_TA_TRANS_MOD = 1u << 10,
    HN_MAPLE_DMA = 1u << 12, HN_MAPLE_VBL = 1u << 13, HN_GD_DMA = 1u << 14, HN_AICA_DMA = 1u << 15,
    HN_EXT1_DMA = 1u << 16, HN_EXT2_DMA = 1u << 17, HN_DEV_DMA = 1u << 18, HN_CH2_DMA = 1u << 19,
    HN_PVR_SORT_DMA = 1u << 20, HN_TA_PUNCH = 1u << 21,
    HE_GDROM = 1u << 0, HE_AICA = 1u << 1, HE_MODEM = 1u << 2, HE_EXTDEV = 1u << 3,
};
void holly_raise_nrm(uint32_t bits);
void holly_raise_ext(uint32_t bits);
void holly_clear_ext(uint32_t bits);
int holly_irl_level(uint32_t* intevt);      // highest pending IRL level (0 = none)
void sh4_raise_internal(uint32_t intevt);   // TMU/DMAC/... pending flags (checked against IPR)
void sh4_clear_internal(uint32_t intevt);

// ---- register blocks -------------------------------------------------------------------------
uint32_t sb_read(uint32_t addr, int size);              // 0x005F6800-0x005F7FFF
void sb_write(uint32_t addr, uint32_t v, int size);
uint32_t pvr_read(uint32_t addr, int size);             // 0x005F8000-0x005F9FFF
void pvr_write(uint32_t addr, uint32_t v, int size);
void ta_write(uint32_t addr, const uint32_t* data, int words);   // area 4 polygon/texture path
uint32_t aica_read(uint32_t addr, int size);            // 0x00700000-0x0071FFFF
void aica_write(uint32_t addr, uint32_t v, int size);
uint32_t p4_read(uint32_t addr, int size);              // 0xFF000000+
void p4_write(uint32_t addr, uint32_t v, int size);
uint32_t awio_read(uint32_t addr, int size);            // 0x00600000-0x006007FF (Atomiswave I/O)
void awio_write(uint32_t addr, uint32_t v, int size);
void maple_dma_start();
void pvr_init();
void aica_init();
void aica_update();
void aica_set_threaded(bool on);                    // Vita: ARM7 on its own thread (see aica.cpp)
bool aica_arm_thread_step(uint32_t max_backlog);
void aica_set_realtime(bool on);                    // Vita: the ARM thread owns the 44.1 kHz clock (wall time)
void aica_arm_thread_run(uint32_t samples);
void sh4p_init();
void ch2_dma_start();

// ---- BIOS / disc -----------------------------------------------------------------------------
bool disc_open(const std::string& gdi_dir);
bool disc_read_fad(uint32_t fad, uint8_t* dst2048);
void bios_install(sh4::Ctx* c);             // writes vectors + registers HLE entry points
using HleFn = void (*)(sh4::Ctx*);
void hle_register(uint32_t addr, HleFn fn);
HleFn hle_lookup(uint32_t addr);

// ---- frame recording -------------------------------------------------------------------------
struct FrameRecord {
    uint32_t frame;                 // vblank count at STARTRENDER
    uint64_t time;
    std::vector<uint32_t> ta;       // raw TA parameter stream (32-byte units) since TA_LIST_INIT (empty with ta_handoff)
    const uint32_t* ta_ptr = nullptr; size_t ta_words = 0;   // the stream (fr.ta's data, or the live buffer with ta_handoff)
    std::vector<uint32_t> regs;     // PVR register snapshot 0x000-0x1FF (128 words)
    uint32_t lists_ended;           // bitmask of list-end interrupts raised
};
extern uint32_t vblank_count;
extern uint64_t sq_ta_count;
extern uint32_t render_count;
void dump_frame(const FrameRecord& fr);

// ---- diagnostics -----------------------------------------------------------------------------
extern sh4::Ctx* cpu;
// Save states (savestate.cpp): every module writes/reads its state through one SaveIO, in a fixed order.
struct SaveIO {
    FILE* f; bool load; bool ok = true;
    uint32_t version = 2;                  // format version being read or written (old files are still readable)
    std::vector<uint8_t>* mem = nullptr;   // save into memory instead of a file (written later on another thread)
    size_t fail_want = 0, fail_got = 0;
    void raw(void* p, size_t n) {
        if (!n) return;
        if (mem) { const uint8_t* b = (const uint8_t*)p; mem->insert(mem->end(), b, b + n); return; }
        size_t got = load ? std::fread(p, 1, n, f) : std::fwrite(p, 1, n, f);
        if (got != n) { ok = false; if (!fail_want) { fail_want = n; fail_got = got; } }
    }
    template <class T> void v(T& x) { raw((void*)&x, sizeof x); }
};
void sh4_state(SaveIO& s); void holly_state(SaveIO& s); void aica_state(SaveIO& s); void aica_mix_state(SaveIO& s);
void aica_dsp_state(SaveIO& s); void arm_state(SaveIO& s); void bios_state(SaveIO& s); void mem_state(SaveIO& s);
extern bool strict;                          // unhandled MMIO -> fatal
[[noreturn]] void fatal(const char* fmt, ...);
void log_unhandled(const char* what, uint32_t addr, uint32_t v, bool write);
void trace(const char* fmt, ...);
extern int trace_level;

} // namespace hw
