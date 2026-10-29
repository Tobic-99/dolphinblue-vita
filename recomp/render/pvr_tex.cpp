// PVR2 texture conversion: twiddled / VQ / palettised / stride / YUV422 textures in VRAM ->
// RGBA8 for the desktop backend (the Vita backend keeps the 16-bit formats). Converted once and
// cached, keyed by VRAM address + TCW + TSP size bits (+ palette version for PAL formats); a
// cache entry is re-converted only when a VRAM page it covers was written.
// Own implementation from the PVR2 texture format documentation.
#include "draw.h"
#include "../host/hw.h"
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#ifdef DB_VITA
#include <psp2/kernel/processmgr.h>
#endif

static inline uint64_t tex_now_us() {
#ifdef DB_VITA
    return sceKernelGetProcessTimeWide();
#else
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

namespace hw { extern uint32_t vram_page_gen[2048]; extern uint32_t palette_version; uint32_t* pvr_regs_ptr(); }

namespace render {

// PVR registers used for palettes and stride: build_drawlist points this at its register snapshot
static const uint32_t* pvr_src;
// per-frame memo: a texture validated once in this frame's build is returned directly for its later polygons
struct Memo { uint64_t key; uint32_t frame, id; int w, h; };
static Memo memo[512];
static uint32_t memo_frame = 1;
static void texture_cache_flush();
void texture_set_regs(const uint32_t* regs) {   // called once per build_drawlist (start of a frame, GPU idle)
    pvr_src = regs; ++memo_frame;
    if (backend_texture_pressure()) texture_cache_flush();   // out of texture memory: start over
}
static inline const uint32_t* pvr_now() { return pvr_src ? pvr_src : hw::pvr_regs_ptr(); }

struct Entry { uint32_t id = 0; int w = 0, h = 0; uint32_t first_page = 0, last_page = 0; uint64_t gen = 0; uint32_t pal = 0; uint64_t src_hash = 0; };
static std::unordered_map<uint64_t, Entry> cache;
static void texture_cache_flush() { backend_texture_flush(); cache.clear(); }
static TexStats stats;

static inline uint32_t twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t idx = 0, k = 0;
    for (uint32_t i = 0; (1u << i) < w || (1u << i) < h; ++i) {
        if ((1u << i) < h) idx |= ((y >> i) & 1) << k++;
        if ((1u << i) < w) idx |= ((x >> i) & 1) << k++;
    }
    return idx;
}
static inline uint32_t e5(uint32_t v) { return (v << 3) | (v >> 2); }
static inline uint32_t e6(uint32_t v) { return (v << 2) | (v >> 4); }
static inline uint32_t e4(uint32_t v) { return (v << 4) | v; }
// output: 0xAARRGGBB
static inline uint32_t c1555(uint32_t t) { return ((t & 0x8000) ? 0xFF000000u : 0) | e5((t >> 10) & 31) << 16 | e5((t >> 5) & 31) << 8 | e5(t & 31); }
static inline uint32_t c565(uint32_t t) { return 0xFF000000u | e5(t >> 11) << 16 | e6((t >> 5) & 63) << 8 | e5(t & 31); }
static inline uint32_t c4444(uint32_t t) { return e4(t >> 12) << 24 | e4((t >> 8) & 15) << 16 | e4((t >> 4) & 15) << 8 | e4(t & 15); }
static inline uint32_t conv16(uint32_t fmt, uint32_t t) {
    switch (fmt) { case 0: return c1555(t); case 1: return c565(t); case 2: return c4444(t); default: return 0xFF808080u; }
}
static uint32_t palette_entry(uint32_t i) {
    const uint32_t* pvr = pvr_now();
    uint32_t e = pvr[(0x1000 / 4) + (i & 1023)];
    switch (pvr[0x108 / 4] & 3) {
    case 0: return c1555(e & 0xFFFF);
    case 1: return c565(e & 0xFFFF);
    case 2: return c4444(e & 0xFFFF);
    default: return e;   // ARGB8888
    }
}
static inline uint32_t yuv(int y, int u, int v) {
    auto cl = [](int x) { return (uint32_t)(x < 0 ? 0 : x > 255 ? 255 : x); };
    int r = y + (int)(1.375f * (v - 128)), g = y - (int)(0.34375f * (u - 128) + 0.6875f * (v - 128)), b = y + (int)(1.71875f * (u - 128));
    return 0xFF000000u | cl(r) << 16 | cl(g) << 8 | cl(b);
}
static uint32_t mip_offset_texels(uint32_t w) {   // texels preceding the top level (incl. the 3-texel pad)
    uint32_t s = 3; for (uint32_t l = 1; l < w; l <<= 1) s += l * l; return s;
}

static uint64_t src_hash(uint32_t addr, uint32_t len) {   // 64-bit multiply-xor over the VRAM source bytes
    uint64_t h = 0x9E3779B97F4A7C15ull ^ len;
    uint32_t a = addr & 0x7FFFF8, end = std::min<uint32_t>(a + ((len + 7) & ~7u), 0x800000);
    for (; a < end; a += 8) { uint64_t w; std::memcpy(&w, hw::vram + a, 8); h = (h ^ w) * 0x100000001B3ull; h ^= h >> 29; }
    return h;
}

// bytes from the texture's base address to the end of the data convert() reads (same addressing as convert())
static uint32_t data_end(uint32_t tcw, int w, int h) {
    bool mip = tcw >> 31, vq = (tcw >> 30) & 1;
    uint32_t fmt = (tcw >> 27) & 7, n = (uint32_t)w * h;
    if (fmt == 5) return (mip ? mip_offset_texels(w) / 2 : 0) + n / 2;
    if (fmt == 6) return (mip ? mip_offset_texels(w) : 0) + n;
    if (vq) { uint32_t s = 0; if (mip) for (uint32_t l = 1; l < (uint32_t)w; l <<= 1) s += (l * l + 3) / 4; return 2048 + s + n / 4; }
    return (mip ? mip_offset_texels(w) * 2 : 0) + n * 2;
}

// twiddle(x, y) == twiddle(x, 0) | twiddle(0, y): each x/y bit maps to its own index bit, so two small tables
// replace the per-pixel bit loop
struct TwTab {
    std::vector<uint32_t> x, y;
    void build(uint32_t w, uint32_t h) {
        x.resize(w); y.resize(h);
        for (uint32_t i = 0; i < w; ++i) x[i] = twiddle(i, 0, w, h);
        for (uint32_t i = 0; i < h; ++i) y[i] = twiddle(0, i, w, h);
    }
    uint32_t operator()(uint32_t i, uint32_t j) const { return x[i] | y[j]; }
};

static void convert(uint32_t tcw, uint32_t tsp, int w, int h, std::vector<uint32_t>& out, uint32_t& bytes) {
    const uint8_t* vram = hw::vram;
    static TwTab tw, tw2;
    tw.build(w, h); if (w >= 2 && h >= 2) tw2.build(w >> 1, h >> 1);
    uint32_t addr = (tcw & 0x1FFFFF) << 3;
    bool mip = tcw >> 31, vq = (tcw >> 30) & 1, scan = (tcw >> 26) & 1;
    uint32_t fmt = (tcw >> 27) & 7;
    out.assign((size_t)w * h, 0xFFFF00FFu);
    auto rd16 = [&](uint32_t a) { a &= 0x7FFFFE; return (uint32_t)(vram[a] | vram[a + 1] << 8); };
    auto rd8 = [&](uint32_t a) { return (uint32_t)vram[a & 0x7FFFFF]; };
    if (fmt == 5 || fmt == 6) {   // palettised (always twiddled)
        uint32_t palsel = fmt == 5 ? ((tcw >> 21) & 63) << 4 : ((tcw >> 25) & 3) << 8;
        if (mip) addr += fmt == 5 ? mip_offset_texels(w) / 2 : mip_offset_texels(w);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint32_t t = tw(x, y), idx;
            if (fmt == 5) { uint32_t b = rd8(addr + t / 2); idx = (t & 1) ? b >> 4 : b & 15; }
            else idx = rd8(addr + t);
            out[(size_t)y * w + x] = palette_entry(palsel + idx);
        }
        bytes = fmt == 5 ? w * h / 2 : w * h;
        return;
    }
    if (vq) {
        uint32_t cb = addr, idx_base = addr + 2048;
        if (mip) { uint32_t s = 0; for (uint32_t l = 1; l < (uint32_t)w; l <<= 1) s += (l * l + 3) / 4; idx_base += s; }
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            uint32_t e = rd8(idx_base + tw2(x >> 1, y >> 1));
            uint32_t k = (y & 1) | ((x & 1) << 1);
            uint32_t t = rd16(cb + e * 8 + k * 2);
            out[(size_t)y * w + x] = fmt == 3 ? 0xFF808080u : conv16(fmt, t);
        }
        bytes = 2048 + w * h / 4;
        return;
    }
    if (mip) addr += mip_offset_texels(w) * 2;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += (fmt == 3 ? 2 : 1)) {
        uint32_t off = scan ? (uint32_t)(y * w + x) * 2 : tw(x, y) * 2;
        if (fmt == 3) {   // YUV422: two texels per 32 bits (U Y0 V Y1)
            uint32_t a = scan ? off : tw(x, y) * 2;
            uint32_t t0 = rd16(addr + a), t1 = scan ? rd16(addr + a + 2) : rd16(addr + tw(x + 1, y) * 2);
            int u = t0 & 255, y0 = t0 >> 8, v = t1 & 255, y1 = t1 >> 8;
            out[(size_t)y * w + x] = yuv(y0, u, v);
            if (x + 1 < w) out[(size_t)y * w + x + 1] = yuv(y1, u, v);
            continue;
        }
        out[(size_t)y * w + x] = conv16(fmt, rd16(addr + off));
    }
    bytes = w * h * 2;
}

#ifdef DB_VITA
// Vita: textures whose texels are 16-bit ARGB1555/RGB565/ARGB4444 (direct, VQ, or palettised with a 16-bit palette)
// are already in a GXM 16-bit format: detwiddle/de-VQ/look up straight into 16-bit texels, with no 32-bit
// round trip. Returns the GXM-side format (0-2), or -1 when the texture needs the 32-bit path.
static int convert16(uint32_t tcw, int w, int h, std::vector<uint16_t>& out) {
    const uint8_t* vram = hw::vram;
    uint32_t fmt = (tcw >> 27) & 7, addr = (tcw & 0x1FFFFF) << 3;
    bool mip = tcw >> 31, vq = (tcw >> 30) & 1, scan = (tcw >> 26) & 1;
    auto rd16 = [&](uint32_t a) { a &= 0x7FFFFE; return (uint16_t)(vram[a] | vram[a + 1] << 8); };
    auto rd8 = [&](uint32_t a) { return (uint32_t)vram[a & 0x7FFFFF]; };
    static TwTab tw, tw2;
    out.resize((size_t)w * h);
    size_t W = (size_t)w;
    if (fmt == 5 || fmt == 6) {
        const uint32_t* pvr = pvr_now();
        uint32_t pf = pvr[0x108 / 4] & 3;
        if (pf == 3) return -1;   // ARGB8888 palette
        tw.build(w, h);
        uint32_t palsel = fmt == 5 ? ((tcw >> 21) & 63) << 4 : ((tcw >> 25) & 3) << 8;
        if (mip) addr += fmt == 5 ? mip_offset_texels(w) / 2 : mip_offset_texels(w);
        const uint32_t* pal = pvr + 0x1000 / 4;
        uint16_t pal16[256]; uint32_t ncol = fmt == 5 ? 16 : 256;
        for (uint32_t j = 0; j < ncol; ++j) pal16[j] = (uint16_t)pal[palsel + j];
        for (int y = 0; y < h; ++y) {
            uint32_t yt = tw.y[y]; uint16_t* row = out.data() + (size_t)y * W;
            if (fmt == 5) { for (int x = 0; x < w; ++x) { uint32_t t = tw.x[x] | yt, b = rd8(addr + (t >> 1)); row[x] = pal16[(t & 1) ? (b >> 4) : (b & 15)]; } }
            else { for (int x = 0; x < w; ++x) row[x] = pal16[rd8(addr + (tw.x[x] | yt))]; }
        }
        return (int)pf;
    }
    if (fmt > 2) return -1;
    if (vq) {
        if (w >= 2 && h >= 2) tw2.build(w >> 1, h >> 1);
        uint32_t cb = addr, idx_base = addr + 2048;
        if (mip) { uint32_t s = 0; for (uint32_t l = 1; l < (uint32_t)w; l <<= 1) s += (l * l + 3) / 4; idx_base += s; }
        if ((w & 1) == 0 && (h & 1) == 0) {
            uint16_t cb16[1024];   // 256-entry codebook pre-decoded: texel reads hit L1
            for (uint32_t i = 0; i < 1024; ++i) cb16[i] = rd16(cb + i * 2);
            for (int by = 0; by < h; by += 2) {
                uint32_t yt = tw2.y[by >> 1]; uint16_t* row0 = out.data() + (size_t)by * W; uint16_t* row1 = row0 + W;
                for (int bx = 0; bx < w; bx += 2) {
                    uint32_t e = rd8(idx_base + (tw2.x[bx >> 1] | yt)) * 4;
                    row0[bx] = cb16[e]; row0[bx + 1] = cb16[e + 2];
                    row1[bx] = cb16[e + 1]; row1[bx + 1] = cb16[e + 3];
                }
            }
        } else {
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                uint32_t e = rd8(idx_base + tw2(x >> 1, y >> 1));
                out[(size_t)y * W + x] = rd16(cb + e * 8 + ((y & 1) | ((x & 1) << 1)) * 2);
            }
        }
        return (int)fmt;
    }
    if (mip) addr += mip_offset_texels(w) * 2;
    if (scan) { for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) out[(size_t)y * W + x] = rd16(addr + (uint32_t)(y * W + x) * 2); }
    else { tw.build(w, h); for (int y = 0; y < h; ++y) { uint32_t yt = tw.y[y]; uint16_t* row = out.data() + (size_t)y * W; for (int x = 0; x < w; ++x) row[x] = rd16(addr + (tw.x[x] | yt) * 2); } }
    return (int)fmt;
}
#endif
static uint32_t texture_get_slow(uint64_t key, uint32_t tcw, uint32_t tsp, int w, int h, uint32_t fmt);
uint32_t texture_get(uint32_t tcw, uint32_t tsp, int* pw, int* ph) {
    const uint32_t* pvr = pvr_now();
    int w = 8 << ((tsp >> 3) & 7), h = 8 << (tsp & 7);
    bool scan = (tcw >> 26) & 1, stride = (tcw >> 25) & 1;
    uint32_t fmt = (tcw >> 27) & 7;
    if (scan && stride && fmt != 5 && fmt != 6) w = (pvr[0x0E4 / 4] & 31) * 32;
    if (w <= 0) w = 8;
    uint64_t key = ((uint64_t)tcw << 32) | (tsp & 0x3F) | (stride && scan ? (pvr[0x0E4 / 4] & 31) << 8 : 0);
    Memo& mo = memo[(key ^ (key >> 32) ^ (key >> 7)) & 511];
    if (mo.frame == memo_frame && mo.key == key) { stats.hits++; *pw = mo.w; *ph = mo.h; return mo.id; }
    uint32_t id = texture_get_slow(key, tcw, tsp, w, h, fmt);
    mo = {key, memo_frame, id, w, h};
    *pw = w; *ph = h;
    return id;
}
static uint32_t texture_get_slow(uint64_t key, uint32_t tcw, uint32_t tsp, int w, int h, uint32_t fmt) {
    Entry& e = cache[key];
    uint32_t addr = (tcw & 0x1FFFFF) << 3;
    unsigned vq = (tcw >> 30) & 1, mip = tcw >> 31, scan = (tcw >> 26) & 1;
    uint32_t span = data_end(tcw, w, h);   // exact bytes read by convert(), so writes next to the texture don't invalidate it
    uint32_t fp = addr >> 12, lp = std::min<uint32_t>((addr + span - 1) >> 12, 2047);
    uint64_t gen = 0; for (uint32_t p = fp; p <= lp; ++p) gen += hw::vram_page_gen[p] * (uint64_t)(p + 1);
    uint32_t pal = (fmt == 5 || fmt == 6) ? hw::palette_version : 0;
    if (e.id && e.gen == gen && e.pal == pal && e.w == w && e.h == h) { stats.hits++; return e.id; }
    // A covered page was written, but often only by a neighbouring texture sharing the 4 KB page: compare a hash
    // of the source bytes and skip the conversion if they are unchanged.
    uint64_t sh = src_hash(addr, span);
    if (e.id && e.src_hash == sh && e.pal == pal && e.w == w && e.h == h) { e.gen = gen; stats.hits++; return e.id; }
    uint64_t t0 = tex_now_us();
#ifdef DB_VITA
    {   // 16-bit texels straight to the GPU (no 32-bit round trip, no per-texture allocation)
        static std::vector<uint16_t> t16;
        int gf = backend_tex32() ? -1 : convert16(tcw, w, h, t16);
        if (gf >= 0) {
            e.id = backend_upload_texture16(e.id, w, h, t16.data(), gf);
            e.w = w; e.h = h; e.gen = gen; e.pal = pal; e.first_page = fp; e.last_page = lp; e.src_hash = sh;
            uint64_t dt = tex_now_us() - t0; stats.conversions++; stats.convert_us += dt; stats.live = cache.size();
            if (dt > 2000) std::printf("[texconv] %dx%d fmt %u -> 16-bit in %.2f ms (vq %u mip %u scan %u)\n", w, h, fmt, dt / 1e3, vq, mip, scan);
            return e.id;
        }
    }
#endif
    static std::vector<uint32_t> rgba; uint32_t bytes = 0;
    convert(tcw, tsp, w, h, rgba, bytes);
#ifndef DB_VITA
    static const char* texdump = getenv("DB_TEXDUMP");
    if (texdump) {
        std::vector<uint32_t> o(rgba.size());
        for (size_t i = 0; i < o.size(); ++i) { uint32_t c = rgba[i]; o[i] = (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16); }
        char p[256]; std::snprintf(p, sizeof p, "%s/tex_%08x_%08x.png", texdump, tcw, tsp);
        write_png(p, w, h, o.data());
    }
#endif
    static const bool texlog = getenv("DB_TEXLOG") != nullptr;
    if (texlog) { size_t mag = 0, a0 = 0; for (uint32_t px : rgba) { mag += px == 0xFFFF00FFu; a0 += (px >> 24) == 0; } std::printf("[texfill] %zu/%zu alpha0 %zu ", mag, rgba.size(), a0); }
    if (texlog) std::printf("[tex] tcw %08x tsp %08x %dx%d fmt %u vq %u mip %u scan %u\n", tcw, tsp, w, h, (tcw >> 27) & 7, (tcw >> 30) & 1, tcw >> 31, (tcw >> 26) & 1);
    uint32_t pfmt = (tcw >> 27) & 7;
    e.id = backend_upload_texture(e.id, w, h, rgba.data(), (pfmt <= 2) ? (int)pfmt : -1);
    e.w = w; e.h = h; e.gen = gen; e.pal = pal; e.first_page = fp; e.last_page = lp; e.src_hash = sh;
    uint64_t dt = tex_now_us() - t0; stats.conversions++; stats.convert_us += dt;
    stats.live = cache.size();
    if (dt > 2000) std::printf("[texconv] %dx%d fmt %u -> 32-bit in %.2f ms (vq %u mip %u scan %u)\n", w, h, fmt, dt / 1e3, vq, mip, scan);
    return e.id;
}
void texture_frame_begin() {}
TexStats texture_stats() { stats.live = cache.size(); return stats; }

} // namespace render
