// Holly: system bus (SB) registers + interrupt controller, Maple, G1/G2/PVR DMA,
// PowerVR2 core registers, SPG (video timing), TA FIFO parsing and per-frame recording.
#include "hw.h"
#include <cstring>
#include <cstdlib>
#include <map>
#include <string>

namespace hw {

uint32_t vblank_count, render_count;
void (*render_hook)(const FrameRecord&, uint32_t) = nullptr;

// ------------------------------------------------------------------ SB registers
static uint32_t sb[0x1800 / 4];              // 0x5F6800-0x5F7FFF
static inline uint32_t& SB(uint32_t addr) { return sb[((addr & 0x1FFF) - 0x800) / 4]; }
enum : uint32_t {
    C2DSTAT = 0x5F6800, C2DLEN = 0x5F6804, C2DST = 0x5F6808, SDSTAW = 0x5F6810, LMMODE0 = 0x5F6884, LMMODE1 = 0x5F6888,
    FFST = 0x5F688C, SFRES = 0x5F6890, SBREV = 0x5F689C,
    ISTNRM = 0x5F6900, ISTEXT = 0x5F6904, ISTERR = 0x5F6908,
    IML2NRM = 0x5F6910, IML2EXT = 0x5F6914, IML2ERR = 0x5F6918,
    IML4NRM = 0x5F6920, IML4EXT = 0x5F6924, IML4ERR = 0x5F6928,
    IML6NRM = 0x5F6930, IML6EXT = 0x5F6934, IML6ERR = 0x5F6938,
    MDSTAR = 0x5F6C04, MDTSEL = 0x5F6C10, MDEN = 0x5F6C14, MDST = 0x5F6C18, MSYS = 0x5F6C80, MDAPRO = 0x5F6C8C, MMSEL = 0x5F6CE8,
    GDSTAR = 0x5F7404, GDLEN = 0x5F7408, GDDIR = 0x5F740C, GDEN = 0x5F7414, GDST = 0x5F7418, GDSTARD = 0x5F74F4, GDLEND = 0x5F74F8,
    ADSTAG = 0x5F7800, ADSTAR = 0x5F7804, ADLEN = 0x5F7808, ADDIR = 0x5F780C, ADEN = 0x5F7814, ADST = 0x5F7818,
    PDSTAP = 0x5F7C00, PDSTAR = 0x5F7C04, PDLEN = 0x5F7C08, PDDIR = 0x5F7C0C, PDEN = 0x5F7C14, PDST = 0x5F7C18,
};
static uint32_t istnrm, istext, isterr;

void holly_raise_nrm(uint32_t bits) { istnrm |= bits; if (cpu) cpu->irq_pending = 1; }
void holly_raise_ext(uint32_t bits) { istext |= bits; if (cpu) cpu->irq_pending = 1; }
void holly_clear_ext(uint32_t bits) { istext &= ~bits; }
static uint32_t nrm_view() { return (istnrm & 0x3FFFFF) | (istext ? 0x40000000u : 0) | (isterr ? 0x80000000u : 0); }
int holly_irl_level(uint32_t* ev) {
    uint32_t n = nrm_view();
    struct { int lvl; uint32_t mn, me, mr; } L[3] = {
        {6, IML6NRM, IML6EXT, IML6ERR}, {4, IML4NRM, IML4EXT, IML4ERR}, {2, IML2NRM, IML2EXT, IML2ERR}};
    for (auto& l : L)
        if ((n & SB(l.mn)) || (istext & SB(l.me)) || (isterr & SB(l.mr))) { *ev = 0x200 + (15 - l.lvl) * 0x20; return l.lvl; }
    return 0;
}

// ------------------------------------------------------------------ Maple
struct PadState { uint16_t buttons = 0xFFFF; uint8_t rt = 0, lt = 0, x = 0x80, y = 0x80; };
PadState pad[4];
extern bool maple_port_connected[4];
bool maple_port_connected[4] = {true, false, false, false};

static void maple_device(int port, uint32_t* frame, uint32_t recv) {
    uint32_t hdr = frame[0];
    uint32_t cmd = hdr & 0xFF, dst = (hdr >> 8) & 0xFF, src = (hdr >> 16) & 0xFF;
    bool main_unit = (dst & 0x20) != 0;
    static const bool maplelog = getenv("DB_MAPLELOG") != nullptr;
    if (maplelog && !main_unit) { static int n; if (n++ < 20) std::printf("[maple] port %d sub-unit dst %02x cmd %u (no device)\n", port, dst, cmd); }
    if (!maple_port_connected[port] || !main_unit) { bus_write32(recv, 0xFFFFFFFF); return; }
    static const bool vmu_probe = getenv("DB_VMUPROBE") != nullptr;   // advertise a VMU in sub-unit 1 (does the game look for one?)
    uint32_t me = (port << 6) | 0x20 | (vmu_probe && port == 0 ? 0x01 : 0);
    std::vector<uint32_t> out;
    uint32_t rcmd;
    auto str = [&](const char* s, int n) {   // pack ASCII into words (byte order as stored in memory)
        std::vector<uint8_t> b(n, ' ');
        for (int i = 0; i < n && s[i]; ++i) b[i] = (uint8_t)s[i];
        return b;
    };
    switch (cmd) {
    case 1: case 2: {   // device info / all status
        std::vector<uint8_t> b;
        auto w32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i))); };
        w32(0x01000000);              // function: controller
        w32(0xFE060F00); w32(0); w32(0);
        b.push_back(0xFF); b.push_back(0);
        auto name = str("Dreamcast Controller", 30); b.insert(b.end(), name.begin(), name.end());
        auto lic = str("Produced By or Under License From SEGA ENTERPRISES,LTD.", 60); b.insert(b.end(), lic.begin(), lic.end());
        b.push_back(0xAE); b.push_back(0x01); b.push_back(0xF4); b.push_back(0x01);
        while (b.size() % 4) b.push_back(0);
        for (size_t i = 0; i < b.size(); i += 4) out.push_back(b[i] | b[i + 1] << 8 | b[i + 2] << 16 | (uint32_t)b[i + 3] << 24);
        rcmd = (cmd == 1) ? 5 : 6;
        break;
    }
    case 3: case 4: rcmd = 7; break;   // reset / shutdown -> ack
    case 9: {           // get condition
        { static int n; static const bool ml = getenv("DB_MAPLELOG") != nullptr; if (ml && n++ < 3) std::printf("[maple] get condition port %d -> recv %08x\n", port, recv); }
        const PadState& p = pad[port];
        out.push_back(0x01000000);
        out.push_back((uint32_t)p.buttons | ((uint32_t)p.rt << 16) | ((uint32_t)p.lt << 24));
        out.push_back((uint32_t)p.x | ((uint32_t)p.y << 8) | (0x80u << 16) | (0x80u << 24));
        rcmd = 8;
        break;
    }
    default:
        log_unhandled("maple cmd", cmd, port, true);
        rcmd = 0xFD;   // unknown command
        break;
    }
    uint32_t rh = rcmd | (src << 8) | (me << 16) | ((uint32_t)out.size() << 24);
    bus_write32(recv, rh);
    for (size_t i = 0; i < out.size(); ++i) bus_write32(recv + 4 + 4 * i, out[i]);
}

static uint64_t maple_frames;
void maple_dma_start() {
    uint32_t a = SB(MDSTAR);
    for (int guard = 0; guard < 64; ++guard) {
        uint32_t d0 = bus_read32(a), recv = bus_read32(a + 4);
        uint32_t len = d0 & 0xFF, port = (d0 >> 16) & 3, pattern = (d0 >> 8) & 7;
        if (pattern == 0) {
            uint32_t frame[256];
            for (uint32_t i = 0; i <= len && i < 256; ++i) frame[i] = bus_read32(a + 8 + 4 * i);
            maple_device(port, frame, recv);
            a += 8 + 4 * (len + 1);
        } else {
            a += 8;   // other patterns (light-gun mode / NOP): no transfer
        }
        if (d0 & 0x80000000u) break;
    }
    maple_frames++;
    sched_add(EV_MAPLE_DMA, now + 200000);   // ~1 ms
}

// ------------------------------------------------------------------ PVR core registers / SPG
static uint32_t pvr[0x2000 / 4];
uint32_t palette_version;
uint32_t* pvr_regs_ptr() { return pvr; }
static inline uint32_t& PVR(uint32_t off) { return pvr[(off & 0x1FFF) / 4]; }
enum : uint32_t {
    P_ID = 0x000, P_REV = 0x004, P_SOFTRESET = 0x008, P_STARTRENDER = 0x014, P_PARAM_BASE = 0x020, P_REGION_BASE = 0x02C,
    P_FB_R_CTRL = 0x044, P_FB_R_SOF1 = 0x050, P_SPG_HBLANK_INT = 0x0C8, P_SPG_VBLANK_INT = 0x0CC, P_SPG_CONTROL = 0x0D0,
    P_SPG_LOAD = 0x0D8, P_SPG_STATUS = 0x10C, P_TA_OL_BASE = 0x124, P_TA_ISP_BASE = 0x128, P_TA_NEXT_OPB = 0x134,
    P_TA_ITP_CURRENT = 0x138, P_TA_LIST_INIT = 0x144, P_TA_LIST_CONT = 0x160, P_TA_NEXT_OPB_INIT = 0x164,
};
static uint64_t frame_start;
static uint64_t frame_cycles() { return SH4_HZ / 60; }
static uint32_t total_lines() { uint32_t l = ((PVR(P_SPG_LOAD) >> 16) & 0x3FF) + 1; return l < 2 ? 525 : l; }
static void frame_sync() { while (now - frame_start >= frame_cycles()) frame_start += frame_cycles(); }
static uint32_t cur_line() {
    frame_sync();
    uint64_t t = now - frame_start;
    uint32_t l = (uint32_t)(t * total_lines() / frame_cycles());
    return l % total_lines();
}
static uint64_t line_time(uint32_t line) {   // next absolute time (> now) at which `line` starts
    frame_sync();
    uint64_t t = frame_start + frame_cycles() * (line % total_lines()) / total_lines();
    if (t <= now) t += frame_cycles();
    return t;
}
static void schedule_vblank() {
    uint32_t vin = PVR(P_SPG_VBLANK_INT) & 0x3FF, vout = (PVR(P_SPG_VBLANK_INT) >> 16) & 0x3FF;
    sched_add(EV_VBLANK_IN, line_time(vin));
    sched_add(EV_VBLANK_OUT, line_time(vout));
}

void pvr_init() {
    PVR(P_ID) = 0x17FD11DB; PVR(P_REV) = 0x11;
    PVR(P_SPG_LOAD) = (524u << 16) | 857;
    PVR(P_SPG_VBLANK_INT) = (21u << 16) | 260;   // typical BIOS values: out at 21, in at 260
    PVR(0x0DC) = (21u << 16) | 520;              // SPG_VBLANK
    frame_start = now;
    schedule_vblank();
}

uint32_t pvr_read(uint32_t addr, int size) {
    uint32_t off = addr & 0x1FFF;
    if (off == P_SPG_STATUS) {
        uint32_t l = cur_line();
        uint32_t vin = PVR(0x0DC) & 0x3FF, vout = (PVR(0x0DC) >> 16) & 0x3FF;
        bool vb = (vin > vout) ? (l >= vin || l < vout) : (l >= vin && l < vout);
        return l | (vb ? (1u << 13) : 0) | ((vblank_count & 1) << 10);
    }
    return PVR(off);
}

// ------------------------------------------------------------------ TA
// TA parameter stream since TA_LIST_INIT: a plain append buffer (the hot path appends one 32-byte unit per SQ flush)
static uint32_t* ta_buf; static size_t ta_len, ta_cap;
static void ta_stale_check(const char* what);
static bool ta_stale;
// Native writers that fill reserved TA units on a worker thread (native_vtx.cpp) must be finished before the
// stream buffer moves (growth), is reset (TA_LIST_INIT) or is read (STARTRENDER): ta_sync_hook waits for them.
void (*ta_sync_hook)();
static inline void ta_sync() { if (ta_sync_hook) ta_sync_hook(); }
static void ta_grow() { ta_sync(); ta_cap = ta_cap ? ta_cap * 2 : (1u << 18); ta_buf = (uint32_t*)std::realloc(ta_buf, ta_cap * 4); }
static inline void ta_append(const uint32_t* d, int words) {
    if (__builtin_expect(ta_stale && ta_len, 0)) ta_stale_check("append");
    if (ta_len + words > ta_cap) { ta_grow(); }
    std::memcpy(ta_buf + ta_len, d, (size_t)words * 4); ta_len += words;
}
static int ta_list = -1;            // current list type, -1 = none
static int ta_pending_words = 0;    // remaining words of a 64-byte parameter
static uint32_t ta_obj_ctrl;        // last polygon header's object control bits
static bool ta_in_modvol = false;
static uint32_t lists_ended;
static uint64_t ta_params, ta_bytes;

static int vertex_words(uint32_t pcw) {
    if (ta_in_modvol) return 16;
    uint32_t para = pcw >> 29;
    (void)para;
    uint32_t oc = ta_obj_ctrl;
    bool sprite = ((oc >> 31) & 1) != 0;   // we store para type 5 marker in bit 31 of ta_obj_ctrl
    if (sprite) return 16;
    uint32_t col = (oc >> 4) & 3; bool tex = (oc >> 3) & 1; bool vol = (oc >> 6) & 1;
    if (!vol) {
        if (!tex) return 8;
        return (col == 1) ? 16 : 8;
    }
    if (!tex) return 8;
    return 16;
}
static int header_words(uint32_t pcw) {
    uint32_t para = pcw >> 29;
    if (para == 5) return 8;
    uint32_t col = (pcw >> 4) & 3; bool vol = (pcw >> 6) & 1; bool off = (pcw >> 2) & 1;
    bool modvol_list = (ta_list == 1 || ta_list == 3) || (ta_list < 0 && (((pcw >> 24) & 7) == 1 || ((pcw >> 24) & 7) == 3));
    if (modvol_list) return 8;
    if (!vol) return (col == 2 && off) ? 16 : 8;
    return (col == 2) ? 16 : 8;
}

// one 8-word TA unit: track list state; the full stream is recorded separately for the renderer
static void ta_unit(const uint32_t* u) {
    if (ta_pending_words > 0) { ta_pending_words -= 8; return; }
    uint32_t pcw = u[0];
    uint32_t para = pcw >> 29;
    ta_params++;
    switch (para) {
    case 0: {   // end of list
        static const uint32_t bits[5] = {HN_TA_OPAQUE, HN_TA_OPAQUE_MOD, HN_TA_TRANS, HN_TA_TRANS_MOD, HN_TA_PUNCH};
        if (ta_list >= 0 && ta_list < 5) { holly_raise_nrm(bits[ta_list]); lists_ended |= 1u << ta_list; }
        ta_list = -1; ta_in_modvol = false;
        break;
    }
    case 1: case 2: break;   // user tile clip, object list set
    case 4: case 5: {
        if (ta_list < 0) ta_list = (pcw >> 24) & 7;
        ta_in_modvol = (ta_list == 1 || ta_list == 3);
        ta_obj_ctrl = (pcw & 0xFF) | (para == 5 ? 0x80000000u : 0);
        int hw_ = header_words(pcw);
        if (hw_ == 16) ta_pending_words = 8;
        break;
    }
    case 7: {
        int vw = vertex_words(pcw);
        if (vw == 16) ta_pending_words = 8;
        break;
    }
    default:
        log_unhandled("TA param type", para, pcw, true);
        break;
    }
}

static uint32_t partial[8]; static int partial_n;   // a TA unit split across writes (rare: SQ/DMA are 32-byte aligned)

// Direct unit submission for native store-queue writers: equivalent to an SQ flush of one 32-byte unit to the
// polygon path (ta_write with 8 words while no partial unit is pending). ta_unit_slot reserves the 8 words in
// the TA stream; ta_unit_commit records them.
bool ta_unit_direct_ok() { return partial_n == 0; }
uint32_t* ta_unit_slot() {
    if (__builtin_expect(ta_stale && ta_len, 0)) ta_stale_check("append");
    if (ta_len + 8 > ta_cap) { ta_grow(); }
    return ta_buf + ta_len;
}
size_t ta_stream_len() { return ta_len; }
// Bulk path for native vertex writers: units are written straight into reserved TA buffer space and committed
// in batches. Valid only for 8-word vertex parameters (para 7, vertex_words 8) with no partial unit or second
// half pending, which ta_fast_ok checks; committing n such units is exactly n ta_unit_commit calls.
bool ta_fast_ok(uint32_t pcw_a, uint32_t pcw_b) {
    return partial_n == 0 && ta_pending_words == 0 && !ta_stale &&
           (pcw_a >> 29) == 7 && (pcw_b >> 29) == 7 && vertex_words(pcw_a) == 8 && vertex_words(pcw_b) == 8;
}
uint32_t* ta_fast_reserve(size_t units, size_t* off) {
    while (ta_len + units * 8 > ta_cap) { ta_grow(); }
    *off = ta_len;
    return ta_buf + ta_len;
}
void ta_fast_commit(size_t units) { ta_len += units * 8; ta_bytes += units * 32; sq_ta_count += units; ta_params += units; }
const uint32_t* ta_stream_at(size_t off) { return ta_buf + off; }
void ta_unit_commit() {
    const uint32_t* u = ta_buf + ta_len;
    ta_len += 8; ta_bytes += 32; sq_ta_count++;
    ta_unit(u);
}

void ta_write(uint32_t addr, const uint32_t* data, int words) {
    uint32_t p = addr & 0x1FFFFFFF;
    if (p >= 0x11000000 && p < 0x14000000) {           // texture paths
        bool path32 = ((p >> 25) & 1) ? (SB(LMMODE1) & 1) : (SB(LMMODE0) & 1);
        for (int i = 0; i < words; ++i) {
            uint32_t a = p + 4 * i;
            uint32_t off = path32 ? vram32_to_64(a) : (a & 0x7FFFFC);
            std::memcpy(vram + off, &data[i], 4);
            vram_page_gen[off >> 12]++;
        }
        return;
    }
    if (p >= 0x10800000 && p < 0x11000000) { log_unhandled("TA YUV path", p, data[0], true); return; }
    // polygon path: record the stream, then parse it in 32-byte (8-word) units
    ta_append(data, words);
    ta_bytes += 4 * words;
    for (int i = 0; i < words;) {
        const uint32_t* u;
        if (partial_n == 0 && words - i >= 8) { u = data + i; i += 8; }
        else { partial[partial_n++] = data[i++]; if (partial_n < 8) continue; partial_n = 0; u = partial; }
        ta_unit(u);
    }
}

static std::vector<FrameRecord> frames;
// Draw skip: when the platform presents at most every other frame (draw_skip_mode), every other TA list is
// marked skipped. Native vertex writers then skip building/emitting vertices for it (guest-visible state is
// unchanged: same stream walk, counters, return values and poll points), and the list is not rendered.
int draw_skip_mode;          // 0 off, 1 skip every other list
bool ta_skip_list;           // the list being built will not be presented
static uint32_t list_counter;
bool (*ta_skip_query)();   // platform: true when a list started now could not be presented (Vita: a frame is already queued)
static void ta_list_init() {
    ta_sync();
    ta_skip_list = ta_skip_query ? ta_skip_query() : draw_skip_mode && (list_counter++ & 1);
    ta_len = 0; ta_list = -1; ta_pending_words = 0; ta_in_modvol = false; lists_ended = 0; ta_stale = false;
    PVR(P_TA_NEXT_OPB) = PVR(P_TA_NEXT_OPB_INIT);
    PVR(P_TA_ITP_CURRENT) = PVR(P_TA_ISP_BASE);
}
// Zero-copy handoff (Vita): the render hook takes the stream buffer itself and gives back a recycled one
// (ta_handoff_swap) instead of start_render copying ~1.5 MB per render. After a handoff the buffer no longer
// holds this list, so a render or an append without a new TA_LIST_INIT is reported (never seen so far).
bool ta_handoff;            // set by the platform: FrameRecord.ta stays empty, use ta_ptr/ta_words
void ta_handoff_swap(uint32_t** buf, size_t* cap) {
    std::swap(ta_buf, *buf); std::swap(ta_cap, *cap);
    ta_stale = true;
}
static void ta_stale_check(const char* what) {
    static bool warned;
    if (ta_stale && !warned) { warned = true; std::printf("[ta] %s after a zero-copy handoff without TA_LIST_INIT: earlier data of this list is gone\n", what); }
}
static void start_render() {
    ta_sync();
    ta_stale_check("render");
    FrameRecord fr;
    fr.frame = vblank_count; fr.time = now; fr.lists_ended = lists_ended;
    if (ta_handoff) { fr.ta_ptr = ta_buf; fr.ta_words = ta_len; }
    else { fr.ta.assign(ta_buf, ta_buf + ta_len); fr.ta_ptr = fr.ta.data(); fr.ta_words = fr.ta.size(); }
    fr.regs.assign(pvr, pvr + 0x200 / 4);
    dump_frame(fr);
    extern void (*render_hook)(const FrameRecord&, uint32_t);
    if (render_hook && !ta_skip_list) render_hook(fr, render_count);
    render_count++;
    sched_add(EV_RENDER_DONE, now + 400000);   // ~2 ms
}

void pvr_write(uint32_t addr, uint32_t v, int size) {
    uint32_t off = addr & 0x1FFF;
    if (off == P_ID || off == P_REV || off == P_SPG_STATUS) return;
    if (off >= 0x1000 && PVR(off) != v) palette_version++;
    PVR(off) = v;
    switch (off) {
    case P_STARTRENDER: start_render(); break;
    case P_TA_LIST_INIT: if (v & 0x80000000u) ta_list_init(); break;
    case P_TA_LIST_CONT: if (v & 0x80000000u) { ta_list = -1; } break;
    case P_SPG_VBLANK_INT: case P_SPG_LOAD: schedule_vblank(); break;
    case P_SOFTRESET: break;
    }
}

// ------------------------------------------------------------------ DMA engines
uint64_t ch2_dma_count, ch2_dma_bytes;
void ch2_dma_start() {
    ch2_dma_count++; ch2_dma_bytes += SB(C2DLEN);
    static const bool dmalog = getenv("DB_DMALOG") != nullptr;
    if (dmalog && ch2_dma_count < 6) std::printf("[ch2] src %08x len %u dst %08x vbl %u\n", dmac_reg(0xFFA00020), SB(C2DLEN), SB(C2DSTAT), vblank_count);
    uint32_t src = dmac_reg(0xFFA00020), len = SB(C2DLEN), dst = SB(C2DSTAT);
    // source in main RAM (the usual case): use it in place; otherwise gather through the bus
    static std::vector<uint32_t> buf;
    const uint32_t* data;
    uint32_t sp = src & 0x1FFFFFFF;
    if ((sp >> 26) == 3 && (sp & 0xFFFFFF) + len <= 0x1000000 && !(sp & 3)) data = (const uint32_t*)(ram + (sp & 0xFFFFFF));
    else { buf.resize(len / 4); for (uint32_t i = 0; i < len / 4; ++i) buf[i] = bus_read32(src + 4 * i); data = buf.data(); }
    const uint32_t nwords = len / 4;
    if ((dst & 0x1FFFFFFF) >= 0x10000000 && (dst & 0x1FFFFFFF) < 0x14000000) {
        uint32_t d = dst & 0x1FFFFFFF;
        if (d < 0x10800000) ta_write(d, data, (int)nwords);
        else for (uint32_t i = 0; i < nwords; i += 8) ta_write(d + 4 * i, &data[i], (int)std::min<uint32_t>(8, nwords - i));
        if (d >= 0x11000000) SB(C2DSTAT) = dst + len;
    } else {
        log_unhandled("ch2 dma dest", dst, len, true);
    }
    dmac_set(0xFFA00020, src + len);
    dmac_set(0xFFA00028, 0);
    dmac_set(0xFFA0002C, dmac_reg(0xFFA0002C) | 2);   // TE
    SB(C2DLEN) = 0; SB(C2DST) = 0;
    sched_add(EV_CH2_DMA, now + 2000);
}

static void aica_dma_start() {
    uint32_t aaddr = SB(ADSTAG), saddr = SB(ADSTAR), len = SB(ADLEN) & 0x7FFFFFFF, dir = SB(ADDIR) & 1;
    for (uint32_t i = 0; i < len; i += 4) {
        if (dir == 0) bus_write32(aaddr + i, bus_read32(saddr + i));
        else bus_write32(saddr + i, bus_read32(aaddr + i));
    }
    SB(ADSTAR) = saddr + len; SB(ADSTAG) = aaddr + len;
    if (SB(ADLEN) & 0x80000000u) SB(ADEN) = 0;
    SB(ADLEN) = 0; SB(ADST) = 0;
    sched_add(EV_AICA_DMA, now + 2000);
}

static void pvr_dma_start() {
    uint32_t vaddr = SB(PDSTAP), saddr = SB(PDSTAR), len = SB(PDLEN), dir = SB(PDDIR) & 1;
    for (uint32_t i = 0; i < len; i += 4) {
        if (dir == 0) bus_write32(vaddr + i, bus_read32(saddr + i));
        else bus_write32(saddr + i, bus_read32(vaddr + i));
    }
    SB(PDST) = 0;
    sched_add(EV_PVR_DMA, now + 2000);
}

uint32_t sb_read(uint32_t addr, int size) {
    uint32_t a = addr & 0x1FFFFFFF;
    switch (a) {
    case ISTNRM: return nrm_view();
    case ISTEXT: return istext;
    case ISTERR: return isterr;
    case FFST: return 0;
    case SBREV: return 0x0B;
    case MDST: return SB(MDST);
    }
    if (a >= 0x5F7000 && a < 0x5F7100) { log_unhandled("GD-ROM ATA register read", a, 0, false); return 0; }
    return SB(a);
}

void sb_write(uint32_t addr, uint32_t v, int size) {
    uint32_t a = addr & 0x1FFFFFFF;
    switch (a) {
    case ISTNRM: istnrm &= ~(v & 0x3FFFFF); return;
    case ISTEXT: return;
    case ISTERR: isterr &= ~v; return;
    case C2DST: SB(C2DST) = v; if (v & 1) ch2_dma_start(); return;
    case MDST: SB(MDST) = v & 1; if ((v & 1) && (SB(MDEN) & 1)) maple_dma_start(); return;
    case ADST: SB(ADST) = v & 1; if ((v & 1) && (SB(ADEN) & 1)) aica_dma_start(); return;
    case PDST: SB(PDST) = v & 1; if ((v & 1) && (SB(PDEN) & 1)) pvr_dma_start(); return;
    case GDST: SB(GDST) = v & 1; if (v & 1) log_unhandled("G1 GD DMA start (no ATA model)", a, v, true); return;
    case SFRES: return;
    }
    if (a >= 0x5F7000 && a < 0x5F7100) { log_unhandled("GD-ROM ATA register write", a, v, true); return; }
    SB(a) = v;
}

// ------------------------------------------------------------------ events
void aica_timer_event();
void ev_fire(int id) {
    switch (id) {
    case EV_VBLANK_IN:
        vblank_count++;
        holly_raise_nrm(HN_VBLANK_IN);
        sched_add(EV_VBLANK_IN, line_time(PVR(P_SPG_VBLANK_INT) & 0x3FF));
        break;
    case EV_VBLANK_OUT:
        holly_raise_nrm(HN_VBLANK_OUT);
        sched_add(EV_VBLANK_OUT, line_time((PVR(P_SPG_VBLANK_INT) >> 16) & 0x3FF));
        break;
    case EV_RENDER_DONE: holly_raise_nrm(HN_RENDER_TSP | HN_RENDER_ISP | HN_RENDER_VIDEO); break;
    case EV_CH2_DMA: holly_raise_nrm(HN_CH2_DMA); break;
    case EV_MAPLE_DMA: SB(MDST) = 0; holly_raise_nrm(HN_MAPLE_DMA); break;
    case EV_AICA_DMA: holly_raise_nrm(HN_AICA_DMA); break;
    case EV_PVR_DMA: holly_raise_nrm(1u << 11); break;
    case EV_GD_DMA: holly_raise_nrm(HN_GD_DMA); break;
    case EV_AICA_TIMER: aica_timer_event(); break;
    }
}

// ------------------------------------------------------------------ frame dump
std::string frame_dir;
void dump_frame(const FrameRecord& fr) {
    static const bool tahash = getenv("DB_TAHASH") != nullptr;   // verification: hash of every frame's TA stream + guest RAM
    if (tahash) {
        uint64_t h = 1469598103934665603ull;
        static const bool mask_eol = getenv("DB_TAHASH_MASKEOL") != nullptr;   // ignore don't-care words of end-of-list params
        for (size_t i = 0; i < fr.ta.size(); ++i) {
            uint32_t w = fr.ta[i];
            if (mask_eol && (i & 7) && (fr.ta[i & ~(size_t)7] >> 29) == 0) w = 0;
            h = (h ^ w) * 1099511628211ull;
        }
        uint64_t hr = 1469598103934665603ull;
        for (size_t i = 0; i < (16u << 20); i += 8) { uint64_t w; std::memcpy(&w, ram + i, 8); hr = (hr ^ w) * 1099511628211ull; }
        std::printf("[tahash] %u%s %016llx %016llx\n", render_count, ta_skip_list ? "s" : "", (unsigned long long)h, (unsigned long long)hr);
    }
    if (frame_dir.empty()) return;
    static const int only = getenv("DB_DUMPRENDER") ? atoi(getenv("DB_DUMPRENDER")) : -1;
    if (only >= 0) { if ((int)render_count != only) return; }
    else if (render_count >= 4 && (render_count % 100) != 0) return;   // first frames + every 100th
    char path[512];
    std::snprintf(path, sizeof path, "%s/render_%05u.bin", frame_dir.c_str(), render_count);
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    uint32_t hdr[6] = {0x41544244 /* 'DBTA' */, 1, fr.frame, render_count, (uint32_t)fr.ta.size(), fr.lists_ended};
    std::fwrite(hdr, 4, 6, f);
    std::fwrite(fr.regs.data(), 4, fr.regs.size(), f);
    std::fwrite(fr.ta.data(), 4, fr.ta.size(), f);
    std::fclose(f);
}
uint64_t ta_param_count() { return ta_params; }

void holly_state(SaveIO& s) {
    s.v(vblank_count); s.v(render_count); s.v(sb); s.v(istnrm); s.v(istext); s.v(isterr); s.v(maple_frames);
    s.v(pvr); s.v(palette_version); s.v(frame_start);
    uint64_t len = ta_len; s.v(len);
    if (s.load) { while (len > ta_cap) ta_grow(); ta_len = (size_t)len; ta_stale = false; }
    s.raw(ta_buf, ta_len * 4);
    s.v(ta_list); s.v(ta_pending_words); s.v(ta_obj_ctrl); s.v(ta_in_modvol); s.v(lists_ended); s.v(ta_params); s.v(ta_bytes);
    s.v(partial); s.v(partial_n); s.v(draw_skip_mode); s.v(ta_skip_list); s.v(list_counter);
    if (s.load) { palette_version++; for (auto& g : vram_page_gen) g++; }   // the texture cache re-checks every texture
}
} // namespace hw

int draw_skip_mode_set(int m) { int o = hw::draw_skip_mode; hw::draw_skip_mode = m; return o; }
