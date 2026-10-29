// BIOS stand-ins (HLE) and disc access. The game (fan conversion) reads its G1ROM image
// through the BIOS GD-ROM vector at 0x8C0000C0 ("gd2"): REQ_CMD(28 = DMA read request,
// params {FAD, sectors}), REQ_DMA_TRANS {dest, size}, GET_CMD_STAT, CHECK_DMA_TRANS, ...
// Semantics follow the documented Dreamcast BIOS GD syscalls; Flycast's reios was used as a
// behavioural reference only.
#include "hw.h"
#include <cstring>
#include <atomic>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <dirent.h>

namespace sh4 { void dispatch_cache_clear(); }
namespace hw {

// ------------------------------------------------------------------ HLE registry
static std::map<uint32_t, HleFn> hle;
void hle_register(uint32_t addr, HleFn fn) { hle[addr] = fn; sh4::dispatch_cache_clear(); }
HleFn hle_lookup(uint32_t addr) {
    if ((addr & 0x1C000000u) == 0x0C000000u) addr = (addr & 0x1FFFFFFFu) | 0x80000000u;
    auto it = hle.find(addr);
    return it == hle.end() ? nullptr : it->second;
}

// ------------------------------------------------------------------ disc (GDI)
struct Track { uint32_t lba, secsize; std::string path; uint64_t size; };
static std::vector<Track> tracks;
uint64_t disc_sectors_read;

static std::vector<FILE*> track_files;
bool disc_open(const std::string& dir) {
    // find the .gdi in dir (portable: dirent works on Linux and the Vita newlib)
    std::string gdi;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && (n.substr(n.size() - 4) == ".gdi" || n.substr(n.size() - 4) == ".GDI")) { gdi = dir + "/" + n; break; }
        }
        closedir(d);
    }
    if (gdi.empty()) return false;
    std::ifstream f(gdi);
    int n; f >> n;
    for (int i = 0; i < n; ++i) {
        int num, type; uint32_t lba, ss; std::string file; long off;
        f >> num >> lba >> type >> ss >> file >> off;
        Track t{lba, ss, dir + "/" + file, 0};
        FILE* tf = std::fopen(t.path.c_str(), "rb");
        if (tf) { std::fseek(tf, 0, SEEK_END); t.size = (uint64_t)std::ftell(tf); }
        tracks.push_back(t);
        track_files.push_back(tf);
    }
    return !tracks.empty();
}

// Read-ahead: one file read fetches up to kAhead raw sectors (a seek + a 2 KB read per sector was the cost on the
// Vita, where every sceIo call is slow; the game streams from the disc while it plays).
static constexpr uint32_t kAhead = 32;
static struct { int track = -1; uint32_t first = 0, n = 0; std::vector<uint8_t> data; } rcache;
bool disc_read_fad(uint32_t fad, uint8_t* dst) {
    uint32_t lba = fad - 150;
    for (size_t i = 0; i < tracks.size(); ++i) {
        auto& t = tracks[i];
        uint64_t secs = t.size / t.secsize;
        if (lba >= t.lba && lba < t.lba + secs) {
            FILE* f = track_files[i];
            if (!f) return false;
            uint32_t rel = lba - t.lba, hdr = t.secsize == 2352 ? 16 : 0;
            if (rcache.track != (int)i || rel < rcache.first || rel >= rcache.first + rcache.n) {
                uint32_t n = (uint32_t)std::min<uint64_t>(kAhead, secs - rel);
                rcache.data.resize((size_t)n * t.secsize);
                std::fseek(f, (long)((uint64_t)rel * t.secsize), SEEK_SET);
                size_t got = std::fread(rcache.data.data(), t.secsize, n, f);
                rcache.track = got ? (int)i : -1; rcache.first = rel; rcache.n = (uint32_t)got;
                if (!got) return false;
            }
            std::memcpy(dst, rcache.data.data() + (size_t)(rel - rcache.first) * t.secsize + hdr, 2048);
            disc_sectors_read++;
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------ GD syscalls
enum { GDC_ERR = -1, GDC_OK = 0, GDC_BUSY = 1, GDC_COMPLETE = 2, GDC_CONTINUE = 3 };
struct GdState {
    int32_t status = GDC_OK;
    uint32_t cmd = 0, req = 0, next_req = 1;
    uint32_t params[4] = {};
    uint32_t result[4] = {};
    uint32_t sector = 0, count = 0, total = 0, offset = 0;
    uint32_t cb = 0, cb_arg = 0;
    bool dma_ended = false;
};
static GdState gd;
uint64_t gd_calls[16];

uint64_t (*gd_clock)();   // platform microsecond clock: logs slow disc transfers (Vita)
// Asynchronous reads (Vita): a DMA_READ_REQ starts reading all its sectors on a worker thread (gd_async runs the
// job there) and the drive reports BUSY until they are in memory, like a real GD-ROM drive, so the game keeps running
// instead of stalling in the transfer. Without gd_async (desktop) reads stay synchronous and deterministic.
void (*gd_async)(void (*job)());
static struct { uint32_t sector = 0, num = 0; std::vector<uint8_t> data; std::atomic<int> state{0}; bool pending = false; } pf;   // state 0 none, 1 reading, 2 ready
static void pf_job() {
    for (uint32_t i = 0; i < pf.num; ++i)
        if (!disc_read_fad(pf.sector + i, pf.data.data() + (size_t)i * 2048)) std::memset(pf.data.data() + (size_t)i * 2048, 0, 2048);
    pf.state.store(2, std::memory_order_release);
}
static void pf_wait() { while (pf.state.load(std::memory_order_acquire) == 1) {} }
static void gd_xfer(uint32_t dest, uint32_t size) {
    size = std::min(size, gd.count);
    uint64_t t0 = gd_clock ? gd_clock() : 0; uint32_t req = size;
    uint8_t buf[2048];
    while (size > 0) {
        if (pf.state.load(std::memory_order_acquire) == 2 && gd.sector >= pf.sector && gd.sector < pf.sector + pf.num)
            std::memcpy(buf, pf.data.data() + (size_t)(gd.sector - pf.sector) * 2048, 2048);   // read ahead by the worker
        else if (!disc_read_fad(gd.sector, buf)) fatal("disc read failed at FAD %u", gd.sector);
        uint32_t n = std::min<uint32_t>(size, 2048 - gd.offset);
        if (sh4::is_ram(dest) && sh4::is_ram(dest + n - 1) && ((dest ^ (dest + n - 1)) & 0xFC000000u) == 0)
            std::memcpy(ram + (dest & 0xFFFFFF), buf + gd.offset, n);   // main RAM (the usual case): one copy
        else for (uint32_t i = 0; i < n; ++i) sh4::wr8(dest + i, buf[gd.offset + i]);
        dest += n; size -= n; gd.count -= n; gd.offset += n;
        if (gd.offset >= 2048) { gd.offset = 0; gd.sector++; }
    }
    if (gd_clock) { uint64_t dt = gd_clock() - t0; if (dt > 20000) std::printf("[gd] %u KB transfer took %.0f ms (vbl %u)\n", req >> 10, dt / 1e3, vblank_count); }
    gd.result[2] = 2048;
    gd.result[3] = gd.count > 0 ? 1 : 0;
    gd.dma_ended = true;
    if (gd.count == 0) gd.status = GDC_COMPLETE;
    sched_add(EV_GD_DMA, now + 20000);
}

static void gd_exec() {
    switch (gd.cmd) {
    case 24: gd.cb = 0; gd.count = 0; gd.status = GDC_COMPLETE; break;           // INIT
    case 28: {                                                                    // DMA_READ_REQ
        gd.sector = gd.params[0] & 0xFFFFFF;
        uint32_t num = gd.params[1];
        trace("[gd] read request FAD %u x %u sectors (G1ROM offset 0x%x)\n", gd.sector, num, (gd.sector - 45450) * 2048);
        gd.count = gd.total = num * 2048; gd.offset = 0;
        if (gd_async && num) {   // read on the worker; stay BUSY until the data is in memory (EXEC_SERVER below)
            pf_wait();
            pf.sector = gd.sector; pf.num = num; pf.data.resize((size_t)num * 2048);
            pf.state.store(1, std::memory_order_release); pf.pending = true;
            gd_async(pf_job);
            break;
        }
        gd.status = GDC_CONTINUE;
        gd.result[2] = 2048; gd.result[3] = num ? 1 : 0;
        break;
    }
    default:
        fatal("GD HLE: command %u not implemented (params %08x %08x)", gd.cmd, gd.params[0], gd.params[1]);
    }
}

static void hle_gd2(sh4::Ctx* c) {
    uint32_t fn = c->r[7];
    if (fn < 16) gd_calls[fn]++;
    switch (fn) {
    case 0: {   // REQ_CMD r4=cmd r5=params
        if (gd.status != GDC_OK) { c->r[0] = 0; break; }
        for (int i = 0; i < 4; ++i) gd.params[i] = c->r[5] ? sh4::rd32(c->r[5] + 4 * i) : 0;
        std::memset(gd.result, 0, sizeof gd.result);
        if (gd.next_req == 0 || gd.next_req == ~0u) gd.next_req = 1;
        gd.req = c->r[0] = gd.next_req++;
        gd.cmd = c->r[4];
        gd.status = GDC_BUSY;
        gd.count = 0;
        break;
    }
    case 1: {   // GET_CMD_STAT r4=req r5=result[4]
        if (gd.status == GDC_BUSY && pf.pending && pf.state.load(std::memory_order_acquire) == 2) {   // asynchronous read finished
            pf.pending = false; gd.status = GDC_CONTINUE; gd.result[2] = 2048; gd.result[3] = 1;
        }
        for (int i = 0; i < 4; ++i) sh4::wr32(c->r[5] + 4 * i, gd.result[i]);
        if (gd.status == GDC_OK || gd.status == GDC_BUSY) c->r[0] = (uint32_t)gd.status;
        else if (c->r[4] != gd.req) c->r[0] = GDC_OK;
        else {
            c->r[0] = (uint32_t)gd.status;
            if (gd.status == GDC_CONTINUE && gd.count == 0) { gd.status = GDC_COMPLETE; gd.result[3] = 0; }
            else if (gd.status != GDC_CONTINUE) { gd.status = GDC_OK; gd.req = ~0u; }
        }
        break;
    }
    case 2:                                                                     // EXEC_SERVER
        if (gd.status == GDC_BUSY) {
            if (pf.pending) {   // an asynchronous read: done once the worker has the sectors
                if (pf.state.load(std::memory_order_acquire) == 2) { pf.pending = false; gd.status = GDC_CONTINUE; gd.result[2] = 2048; gd.result[3] = 1; }
            } else gd_exec();
        }
        break;
    case 3: gd = GdState(); break;                                             // INIT_SYSTEM
    case 4: sh4::wr32(c->r[4], gd.status == GDC_CONTINUE ? 3 : 1); sh4::wr32(c->r[4] + 4, 0x80); c->r[0] = 0; break;  // GET_DRV_STAT
    case 5: {   // G1_DMA_END r4=callback r5=arg
        gd.cb = c->r[4]; gd.cb_arg = c->r[5];
        c->r[0] = GDC_OK;
        holly_raise_nrm(0);
        if (gd.cb && gd.dma_ended) {
            gd.dma_ended = false;
            c->r[4] = gd.cb_arg;
            sh4::dispatch(c, gd.cb);     // the syscall "returns" into the callback
        }
        break;
    }
    case 6: {   // REQ_DMA_TRANS r4=req r5={dest,size}
        uint32_t dest = sh4::rd32(c->r[5]), size = sh4::rd32(c->r[5] + 4);
        if (gd.status != GDC_CONTINUE || size > gd.count || size == 0) { c->r[0] = (uint32_t)GDC_ERR; break; }
        gd_xfer(dest, size);
        c->r[0] = GDC_OK;
        break;
    }
    case 7:     // CHECK_DMA_TRANS r4=req r5=&size
        sh4::wr32(c->r[5], gd.count);
        c->r[0] = gd.status == GDC_CONTINUE ? GDC_OK : GDC_BUSY;
        break;
    case 8: gd.count = 0; gd.status = GDC_OK; c->r[0] = 0; break;              // READ_ABORT
    case 9: gd.req = ~0u; gd.status = GDC_OK; break;                            // RESET
    case 10: c->r[0] = 0; break;                                                // CHANGE_DATA_TYPE
    default: fatal("GD HLE: syscall function %u not implemented", fn);
    }
}

void bios_install(sh4::Ctx* c) {
    const uint32_t GD2_ENTRY = 0x8C0010F0;     // HLE entry point the vector points to
    sh4::wr32(0x8C0000C0, GD2_ENTRY);
    hle_register(GD2_ENTRY, hle_gd2);
}

// ------------------------------------------------------------------ Atomiswave I/O (0x00600000)
uint32_t aw_coin_input = 0xF;      // active low coin bits
static uint32_t aw_devs, aw_out;
uint32_t awio_read(uint32_t addr, int size) {
    uint32_t off = addr & 0x7FF;
    switch (off) {
    case 0x280: return aw_coin_input;
    case 0x284: return aw_devs;
    case 0x288: return 0;
    case 0x28C: return aw_out;
    case 0x000: case 0x004: return 0;   // modem/ID probe by f_8c342788: nothing attached
    }
    log_unhandled("AW I/O read", addr, 0, false);
    return 0xFF;
}
void awio_write(uint32_t addr, uint32_t v, int size) {
    uint32_t off = addr & 0x7FF;
    switch (off) {
    case 0x284: aw_devs = v & 0xF0; return;
    case 0x288: return;
    case 0x28C: aw_out = v & 0xFF; return;
    }
    log_unhandled("AW I/O write", addr, v, true);
}

void bios_state(SaveIO& s) {
    s.v(gd); s.v(aw_coin_input); s.v(aw_devs); s.v(aw_out);
    pf_wait();   // a state never contains a read in flight
    if (s.version < 2) { if (s.load) { pf.state.store(0); pf.pending = false; } return; }   // version 1 files have no reader state
    int st = pf.state.load(); s.v(pf.sector); s.v(pf.num); s.v(st); s.v(pf.pending);
    if (s.load && (pf.num > 1024 || st < 0 || st > 2)) { s.ok = false; pf.state.store(0); pf.pending = false; return; }   // not a state from this build (e.g. made on the PC)
    if (s.load) pf.data.resize((size_t)pf.num * 2048);
    if (st == 2) s.raw(pf.data.data(), pf.data.size());
    if (s.load) pf.state.store(st == 1 ? 0 : st);
    if (s.load) rcache.track = -1;
}
} // namespace hw
