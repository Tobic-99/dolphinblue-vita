// Memory map: everything that is not main RAM (main RAM is the inline fast path in sh4ctx.h).
#include "hw.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>
#include <cstring>
#include <map>

namespace sh4 { alignas(64) uint8_t ram[16u << 20]; uint32_t sq_mem[2][8]; }

namespace hw {

uint8_t *ram, *vram, *aram, *ocram, *sram;

// The 64-bit path interleaves 32-bit words of the two 4 MB banks: storage (64-bit layout)
// offset = (off32 & 0x3FFFFC) * 2 + bank * 4 + (off32 & 3), where bank = bit 22 of off32.
uint32_t vram32_to_64(uint32_t off32) {
    off32 &= 0x7FFFFF;
    return ((off32 & 0x3FFFFC) << 1) | ((off32 >> 20) & 4) | (off32 & 3);
}

static inline uint32_t rdn(const uint8_t* p, int size) {
    if (size == 1) return *p;
    if (size == 2) { uint16_t v; std::memcpy(&v, p, 2); return v; }
    uint32_t v; std::memcpy(&v, p, 4); return v;
}
static inline void wrn(uint8_t* p, uint32_t v, int size) {
    if (size == 1) *p = (uint8_t)v;
    else if (size == 2) { uint16_t x = (uint16_t)v; std::memcpy(p, &x, 2); }
    else std::memcpy(p, &v, 4);
}

static uint32_t vram_read(uint32_t p, int size) {
    if (((p >> 24) & 1) == 0)  // 0x04/0x06: 64-bit path
        return rdn(vram + (p & 0x7FFFFF), size);
    if (size == 4) return rdn(vram + vram32_to_64(p), 4);
    uint32_t base = vram32_to_64(p & ~3u);
    return rdn(vram + base + (p & 3), size);
}
uint32_t vram_page_gen[2048];
static void vram_write(uint32_t p, uint32_t v, int size) {
    if (((p >> 24) & 1) == 0) { wrn(vram + (p & 0x7FFFFF), v, size); vram_page_gen[(p & 0x7FFFFF) >> 12]++; return; }
    uint32_t base = vram32_to_64(p & ~3u);
    wrn(vram + base + (p & 3), v, size);
    vram_page_gen[base >> 12]++;
}

static uint32_t area0_read(uint32_t p, int size) {
    p &= 0x01FFFFFF;
    if (p < 0x00200000) { log_unhandled("bios-rom read", p, 0, false); return 0; }
    if (p < 0x00220000) return rdn(sram + (p & 0x1FFFF), size);
    if (p >= 0x005F6800 && p < 0x005F8000) return sb_read(p, size);
    if (p >= 0x005F8000 && p < 0x005FA000) return pvr_read(p, size);
    if (p >= 0x00600000 && p < 0x00600800) return awio_read(p, size);
    if (p >= 0x00700000 && p < 0x00720000) return aica_read(p, size);
    if (p >= 0x00800000 && p < 0x01000000) return rdn(aram + (p & (ARAM_SIZE - 1)), size);
    // G2 external-device area: the expansion probe (syExtChk, f_8c342788) reads ID words here;
    // no expansion device is attached, so the bus reads as 0.
    if (p >= 0x01000000 && p < 0x02000000) return 0;
    log_unhandled("area0 read", p, 0, false);
    return 0;
}
uint64_t rom_writes;
static void area0_write(uint32_t p, uint32_t v, int size) {
    p &= 0x01FFFFFF;
    if (p < 0x00200000) {   // BIOS ROM: writes are ignored by the hardware (counted for the report)
        if (rom_writes++ < 4) std::printf("[note] write to BIOS ROM %08x <- %08x ignored (func %08x)\n", p, v, cpu ? cpu->cur_func : 0);
        return;
    }
    if (p < 0x00220000) {
        static const bool sramlog = getenv("DB_SRAMLOG") != nullptr;
        if (sramlog) { static int n; if (n++ < 30) std::printf("[sram] write %08x <- %08x (%d) func %08x vbl %u\n", p, v, size, cpu ? cpu->cur_func : 0, vblank_count); }
        wrn(sram + (p & 0x1FFFF), v, size); return;
    }
    if (p >= 0x005F6800 && p < 0x005F8000) { sb_write(p, v, size); return; }
    if (p >= 0x005F8000 && p < 0x005FA000) { pvr_write(p, v, size); return; }
    if (p >= 0x00600000 && p < 0x00600800) { awio_write(p, v, size); return; }
    if (p >= 0x00700000 && p < 0x00720000) {
        if (getenv("DB_AICA2C") && (p & 0x7FFF) == 0x2C00)
            std::printf("[aica2c] write %08x <- %04x func %08x pr %08x vbl %u\n", p, v, cpu ? cpu->cur_func : 0, cpu ? cpu->pr : 0, vblank_count);
        aica_write(p, v, size); return; }
    if (p >= 0x00800000 && p < 0x01000000) { wrn(aram + (p & (ARAM_SIZE - 1)), v, size); return; }
    log_unhandled("area0 write", p, v, true);
}

// store queues (SQ0/SQ1, 8 words each) live with the P4 code; area-4 TA writes from plain stores:
static void area4_write(uint32_t p, uint32_t v, int size) {
    if (size != 4) { log_unhandled("TA write size!=4", p, v, true); return; }
    ta_write(p, &v, 1);
}

std::map<uint32_t, uint64_t> read_hist;
bool read_hist_on = false;
static uint32_t mem_read(uint32_t a, int size) {
    if (read_hist_on) read_hist[a]++;
    if (a >= 0xE0000000u) return p4_read(a, size);
    if ((a & 0xFC000000u) == 0x7C000000u) return rdn(ocram + (((a >> 1) & 0x1000) | (a & 0xFFF)), size);
    uint32_t p = a & 0x1FFFFFFF;
    switch (p >> 26) {
    case 0: return area0_read(p, size);
    case 1: return vram_read(p, size);
    case 3: return rdn(ram + (p & 0xFFFFFF), size);
    case 4: log_unhandled("TA area read", p, 0, false); return 0;
    default: log_unhandled("unmapped read", a, 0, false); return 0;
    }
}
static std::map<uint32_t, uint64_t> write_hist;   // DB_WHIST: MMIO writes per 1 MB region and size (profiling)
static const bool whist_on = getenv("DB_WHIST") != nullptr;
void write_hist_print() {
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (auto& e : write_hist) v.push_back({e.second, e.first});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 12; ++i) std::printf("[whist] region %08x size %u: %llu\n", (v[i].second >> 4) << 20, v[i].second & 15, (unsigned long long)v[i].first);
}
static void mem_write(uint32_t a, uint32_t v, int size) {
    if (whist_on) write_hist[((a >> 20) << 4) | size]++;
    if (a >= 0xE0000000u) { p4_write(a, v, size); return; }
    if ((a & 0xFC000000u) == 0x7C000000u) { wrn(ocram + (((a >> 1) & 0x1000) | (a & 0xFFF)), v, size); return; }
    uint32_t p = a & 0x1FFFFFFF;
    switch (p >> 26) {
    case 0: area0_write(p, v, size); return;
    case 1: vram_write(p, v, size); return;
    case 3: wrn(ram + (p & 0xFFFFFF), v, size); return;
    case 4: area4_write(p, v, size); return;
    default: log_unhandled("unmapped write", a, v, true); return;
    }
}

// generic access used by DMA engines (physical or virtual addresses)
uint32_t bus_read32(uint32_t a) { return mem_read(a, 4); }
void bus_write32(uint32_t a, uint32_t v) { mem_write(a, v, 4); }

void mem_state(SaveIO& s) {
    s.raw(sh4::ram, 16u << 20); s.raw(vram, VRAM_SIZE); s.raw(aram, ARAM_SIZE); s.raw(sram, 128 * 1024); s.raw(ocram, 8192);
}
} // namespace hw

namespace sh4 {
uint32_t mmio_r8(uint32_t a) { return hw::mem_read(a, 1); }
uint32_t mmio_r16(uint32_t a) { return hw::mem_read(a, 2); }
uint32_t mmio_r32(uint32_t a) { return hw::mem_read(a, 4); }
void mmio_w8(uint32_t a, uint32_t v) { hw::mem_write(a, v, 1); }
void mmio_w16(uint32_t a, uint32_t v) { hw::mem_write(a, v, 2); }
void mmio_w32(uint32_t a, uint32_t v) {
    if ((a >> 26) == 0x38) { sh4::sq_mem[(a >> 5) & 1][(a >> 2) & 7] = v; return; }   // store queue (hot: TA submission)
    hw::mem_write(a, v, 4);
}
}
