// Dolphin Blue desktop runner: loads 1ST_READ.BIN into guest RAM, installs the BIOS stand-ins,
// and runs the translated program headless for a given number of video frames.
//
// usage: db_run [--frames N] [--disc DIR] [--dump DIR] [--trace] [--strict] [--input SCRIPT]
//   input script lines: "<from_frame> <to_frame> <button>" (button: start|a|b|c|x|y|up|down|left|right|coin)
#include <thread>
#include <chrono>
#include "hw.h"
static void wav_close();
namespace hw { void write_hist_print(); namespace arm { void print_pc_hist(); void dump_leaders(); extern uint64_t instr_count, idle_skipped; } }
#include "../render/draw.h"
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <map>
#include <algorithm>

void f_8c010000(sh4::Ctx* c);
extern void (*poll_hook)();

namespace hw {
extern std::string frame_dir;
struct PadState { uint16_t buttons = 0xFFFF; uint8_t rt = 0, lt = 0, x = 0x80, y = 0x80; };
extern PadState pad[4];
extern uint32_t aw_coin_input;
extern uint64_t ch2_dma_count, ch2_dma_bytes, sq_ta_count;
extern uint64_t disc_sectors_read, gd_calls[16], aica_samples;
uint64_t ta_param_count();
bool aica_arm_running();
}
using namespace hw;

static uint32_t max_frames = 600;
static std::string png_dir;
static uint32_t png_every = 0;
static std::vector<uint32_t> png_list;
static bool render_ready;
namespace hw { extern void (*render_hook)(const FrameRecord&, uint32_t); }
namespace hw { extern void (*gd_async)(void (*)()); }
namespace hw { void savestate_request_save(const char*, uint32_t); void savestate_request_load(const char*); }
void savestate_req_save(const char* p, uint32_t v) { hw::savestate_request_save(p, v); }
void savestate_req_load(const char* p) { hw::savestate_request_load(p); }
static const char* g_msave_dir;   // DB_MISSION_SAVE=DIR: write DIR/m<N>.bin at each mission start
static uint8_t g_msave_prev = 0xFF, g_msave_sub = 0xFF;
namespace hw { extern uint8_t aw_switches; }
void aw_switch_test(uint8_t v) { hw::aw_switches = v; }
uint64_t decode_bench_ns, decode_bench_frames, decode_bench_verts;
static void on_render(const hw::FrameRecord& fr, uint32_t idx) {
    static const bool bench = getenv("DB_DECODEBENCH") != nullptr;   // time build_drawlist for every render (no GL)
    if (bench) {
        static render::DrawList bdl;
        if (!render_ready) { if (!render::backend_init(640, 480)) fatal("render backend init failed"); render_ready = true; }
        auto t0 = std::chrono::steady_clock::now();
        render::build_drawlist(fr.ta, hw::pvr_regs_ptr(), bdl);
        decode_bench_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        decode_bench_frames++; decode_bench_verts += bdl.verts.size();
        return;
    }
    bool want = false;
    for (uint32_t x : png_list) want |= x == idx;
    if (png_every && idx % png_every == 0) want = true;
    if (!want) return;
    if (!render_ready) { if (!render::backend_init(640, 480)) fatal("render backend init failed"); render_ready = true; }
    render::DrawList dl;
    render::build_drawlist(fr.ta, hw::pvr_regs_ptr(), dl);
    std::vector<uint32_t> px;
    if (getenv("DB_DLDUMP")) {
        for (int li = 0; li < render::L_COUNT; ++li) for (auto& b : dl.lists[li]) {
            const render::Vertex& v = render::batch_vertex(dl, b, 0);
            float u0 = 1e9, u1 = -1e9, v0 = 1e9, v1 = -1e9, x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
            for (uint32_t k = 0; k < b.count; ++k) { auto& q = render::batch_vertex(dl, b, k); u0 = std::min(u0, q.u); u1 = std::max(u1, q.u); v0 = std::min(v0, q.v); v1 = std::max(v1, q.v);
                x0 = std::min(x0, q.x); x1 = std::max(x1, q.x); y0 = std::min(y0, q.y); y1 = std::max(y1, q.y); }
            std::printf("  bbox [%.0f..%.0f]x[%.0f..%.0f]", x0, x1, y0, y1);
            double area = 0;   // total triangle area, clipped to the 640x480 screen, in screens (overdraw)
            for (uint32_t k = 0; k + 2 < b.count; k += 3) {
                auto cl = [](float x, float lim) { return x < 0 ? 0.f : x > lim ? lim : x; };
                const render::Vertex t_[3] = {render::batch_vertex(dl, b, k), render::batch_vertex(dl, b, k + 1), render::batch_vertex(dl, b, k + 2)}; const render::Vertex* t = t_;
                float ax = cl(t[0].x, 640), ay = cl(t[0].y, 480), bx = cl(t[1].x, 640), by = cl(t[1].y, 480), cx = cl(t[2].x, 640), cy = cl(t[2].y, 480);
                area += std::fabs((bx - ax) * (cy - ay) - (cx - ax) * (by - ay)) * 0.5;
            }
            std::printf("  screens %.1f", area / (640.0 * 480.0));
            std::printf("  uv [%g..%g]x[%g..%g] ", u0, u1, v0, v1);
            std::printf("  tsp %08x isp %08x fog %u | list %d key %02x tex %u depth %u zw %d blend %u/%u n %u sortz %.4g v0 (%.1f,%.1f,%.4g) base %08x tcw %08x obj %02x\n", b.tsp, b.isp, b.fog, li, b.key.bits(), b.tex_id,
                b.depth_func, b.depth_write, b.src_blend, b.dst_blend, b.count, b.sort_z, v.x, v.y, v.invw, v.base, b.tcw, b.pcw);
        }
        std::printf("  clear %08x depth %g presort %d\n", dl.clear_color, dl.clear_depth, dl.presort);
        int neg = 0, far = 0; float mn = 1e30f, mx = -1e30f;
        for (auto& v : dl.verts) { if (v.invw <= 0) neg++; if (std::fabs(v.x) > 4000 || std::fabs(v.y) > 4000) far++; mn = std::min(mn, v.invw); mx = std::max(mx, v.invw); }
        if (const char* tt = getenv("DB_TRI")) {
            const auto& b = dl.lists[0][0]; const render::Vertex v_[3] = {render::batch_vertex(dl, b, 3 * atoi(tt)), render::batch_vertex(dl, b, 3 * atoi(tt) + 1), render::batch_vertex(dl, b, 3 * atoi(tt) + 2)}; const render::Vertex* v = v_;
            for (int k = 0; k < 3; ++k) std::printf("  tri v%d x %.9g y %.9g invw %.9g u %g v %g\n", k, v[k].x, v[k].y, v[k].invw, v[k].u, v[k].v);
        }
        if (getenv("DB_BIGZ")) for (int li = 0; li < render::L_COUNT; ++li) for (auto& b : dl.lists[li]) for (uint32_t t = 0; t + 2 < b.count; t += 3) {
            const render::Vertex v_[3] = {render::batch_vertex(dl, b, t), render::batch_vertex(dl, b, t + 1), render::batch_vertex(dl, b, t + 2)}; const render::Vertex* v = v_;
            float m = std::max(v[0].invw, std::max(v[1].invw, v[2].invw));
            if (m > 0.01f && li != render::L_TRANS) std::printf("  bigz list %d tri (%.1f,%.1f,%g) (%.1f,%.1f,%g) (%.1f,%.1f,%g)\n", li,
                v[0].x, v[0].y, v[0].invw, v[1].x, v[1].y, v[1].invw, v[2].x, v[2].y, v[2].invw);
        }
        if (const char* pk = getenv("DB_PICK")) {
            float px = std::atof(pk), py = std::atof(std::strchr(pk, ',') + 1);
            for (int li = 0; li < render::L_COUNT; ++li) for (auto& b : dl.lists[li]) for (uint32_t t = 0; t + 2 < b.count; t += 3) {
                const render::Vertex v_[3] = {render::batch_vertex(dl, b, t), render::batch_vertex(dl, b, t + 1), render::batch_vertex(dl, b, t + 2)}; const render::Vertex* v = v_;
                auto sgn = [&](const render::Vertex& p1, const render::Vertex& p2) { return (px - p2.x) * (p1.y - p2.y) - (p1.x - p2.x) * (py - p2.y); };
                float d1 = sgn(v[0], v[1]), d2 = sgn(v[1], v[2]), d3 = sgn(v[2], v[0]);
                bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
                if (!(neg && pos))
                    std::printf("  pick list %d tex %u tri (%.1f,%.1f,%g) (%.1f,%.1f,%g) (%.1f,%.1f,%g) isp %08x\n", li, b.tex_id,
                        v[0].x, v[0].y, v[0].invw, v[1].x, v[1].y, v[1].invw, v[2].x, v[2].y, v[2].invw, b.isp);
            }
        }
        std::printf("  verts %zu invw<=0: %d  |xy|>4000: %d  invw range %g..%g\n", dl.verts.size(), neg, far, mn, mx);
    }
    static const bool nopng = getenv("DB_NOPNG") != nullptr;   // decode + textures only (profiling the Vita's render work)
    if (nopng) return;
    render::backend_render(dl, px);
    char path[600];
    std::snprintf(path, sizeof path, "%s/render_%05u_vbl%05u.png", png_dir.c_str(), idx, fr.frame);
    render::write_png(path, dl.width, dl.height, px.data());
    auto ts = render::texture_stats();
    std::printf("[png] %s  verts %zu  op %zu pt %zu tr %zu batches  modvol skipped %u  tex conv %llu hits %llu\n", path, dl.verts.size(),
        dl.lists[render::L_OPAQUE].size(), dl.lists[render::L_PUNCH].size(), dl.lists[render::L_TRANS].size(), dl.skipped_modvol,
        (unsigned long long)ts.conversions, (unsigned long long)ts.hits);
}
struct Press { uint32_t from, to; uint16_t mask; bool coin; };
static std::vector<Press> script;
static uint32_t last_frame_seen;
static uint64_t func_entries;

namespace hw { extern std::map<uint32_t, uint64_t> read_hist; extern bool read_hist_on; }
static void summary(const char* why) {
    { extern void write_hist_print(); if (getenv("DB_WHIST")) write_hist_print(); }
    if (getenv("DB_ARMHIST")) hw::arm::print_pc_hist();
    hw::arm::dump_leaders();
    wav_close();
    { extern uint64_t nv_count[3][2]; if (getenv("DB_VTXSTAT")) std::printf("[vtx] b2a0 calls %llu | ba84 calls %llu verts %llu | b710 calls %llu verts %llu\n", (unsigned long long)nv_count[0][0], (unsigned long long)nv_count[1][0], (unsigned long long)nv_count[1][1], (unsigned long long)nv_count[2][0], (unsigned long long)nv_count[2][1]); }
    if (const char* rd = getenv("DB_RAMDUMP")) { if (FILE* f = std::fopen(rd, "wb")) { std::fwrite(hw::ram, 1, 16u << 20, f); std::fclose(f); } }
    if (const char* dd = getenv("DB_DSPDUMP")) { extern uint16_t* aica_regs_export(); if (FILE* f = std::fopen(dd, "wb")) { std::fwrite(aica_regs_export(), 2, 0x8000 / 4, f); std::fclose(f); } }
    { extern uintptr_t stack_max_depth_get(); std::printf("max host stack depth at polls: %zu KB\n", (size_t)(stack_max_depth_get() / 1024)); }
    if (const char* ad = getenv("DB_ARAMDUMP")) { if (FILE* f = std::fopen(ad, "wb")) { std::fwrite(hw::aram, 1, 2u << 20, f); std::fclose(f); } }
    std::printf("\n=== run summary (%s) ===\n", why);
    std::printf("vblanks %u  renders(STARTRENDER) %u  TA params %llu  virtual time %.3f s\n", vblank_count, render_count,
                (unsigned long long)ta_param_count(), (double)now / SH4_HZ);
    std::printf("interrupts: vbl-irl6 %llu irl4 %llu irl2 %llu tmu0 %llu tmu1 %llu tmu2 %llu\n",
                (unsigned long long)irq_count(0x320), (unsigned long long)irq_count(0x360), (unsigned long long)irq_count(0x3A0),
                (unsigned long long)irq_count(0x400), (unsigned long long)irq_count(0x420), (unsigned long long)irq_count(0x440));
    std::printf("GD syscalls: req %llu stat %llu exec %llu init %llu dmatrans %llu chk %llu  sectors read %llu\n",
                (unsigned long long)gd_calls[0], (unsigned long long)gd_calls[1], (unsigned long long)gd_calls[2],
                (unsigned long long)gd_calls[3], (unsigned long long)gd_calls[6], (unsigned long long)gd_calls[7],
                (unsigned long long)disc_sectors_read);
    std::printf("TA input: ch2 DMA %llu transfers / %llu bytes, store-queue bursts %llu\n", (unsigned long long)ch2_dma_count,
                (unsigned long long)ch2_dma_bytes, (unsigned long long)sq_ta_count);
    if (decode_bench_frames) std::printf("[decodebench] %llu frames, %.3f ms/frame, %.1f ns/vertex (%llu vertices)\n", (unsigned long long)decode_bench_frames, decode_bench_ns / 1e6 / decode_bench_frames, (double)decode_bench_ns / (decode_bench_verts ? decode_bench_verts : 1), (unsigned long long)decode_bench_verts);
    std::printf("AICA samples %llu  ARM7 released: %s  ARM7 interpreted %llu, idle-skipped %llu of 128/sample\n", (unsigned long long)aica_samples, aica_arm_running() ? "yes" : "no",
                (unsigned long long)arm::instr_count, (unsigned long long)arm::idle_skipped);
    std::printf("distinct unhandled MMIO addresses: %zu\n", unhandled_count());
    std::vector<std::pair<uint64_t, uint32_t>> h;
    for (auto& kv : read_hist) h.push_back({kv.second, kv.first});
    std::sort(h.rbegin(), h.rend());
    std::printf("most-read MMIO addresses in the last 60 frames:");
    for (size_t i = 0; i < h.size() && i < 12; ++i) std::printf(" %08x:%llu", h[i].second, (unsigned long long)h[i].first);
    std::printf("\n");
    std::fflush(stdout);
}

static void apply_input() {
    uint16_t m = 0xFFFF; bool coin = false;
    for (auto& p : script) if (vblank_count >= p.from && vblank_count <= p.to) { m &= ~p.mask; coin |= p.coin; }
    pad[0].buttons = m;
    aw_coin_input = coin ? 0xE : 0xF;
}

namespace sh4 { void poll_irq(Ctx* c); }

static double cpu_ms() { timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }   // DB_PERFLOG: CPU time, not wall time
// wrap the scheduler: stop after max_frames vblanks
static void on_poll_check() {
    if (vblank_count != last_frame_seen) {
        last_frame_seen = vblank_count;
        apply_input();
        if (vblank_count % 60 == 0) { std::printf("[frame %u] renders %u\n", vblank_count, render_count); std::fflush(stdout); }
        if (getenv("DB_PERFLOG") && vblank_count % 60 == 0) {   // per-second load: host ms per vblank, renders, TA params per render
            static double t0 = cpu_ms(); static uint32_t r0; static uint64_t p0;
            double t1 = cpu_ms(); uint64_t p1 = ta_param_count();
            double ms = (t1 - t0) / 60.0; uint32_t dr = render_count - r0;
            std::printf("[perf] vbl %u m%u ms/vbl %.2f renders %u params/render %llu\n", vblank_count, (unsigned)sh4::ram[0x3c1516] + 1, ms, dr,
                        (unsigned long long)(dr ? (p1 - p0) / dr : 0));
            t0 = t1; r0 = render_count; p0 = p1;
        }
        if (getenv("DB_STAGEPROBE")) { static uint8_t pa = 0xFF, pb = 0xFF; uint8_t a = sh4::ram[0x3c1516], b = sh4::ram[0x3f33de];
            if (a != pa || b != pb) { std::printf("[stage] vbl %u  3c1516=%u  3f33de=%u\n", vblank_count, a, b); pa = a; pb = b; } }
        if (g_msave_dir) {   // mission-start savestates for the Vita mission-jump slots
            uint8_t mission = sh4::ram[0x3c1516], sub = sh4::ram[0x3f33de];
            if (g_msave_prev == 0xFF) { g_msave_prev = mission; g_msave_sub = sub; }
            uint8_t started = 0xFF;
            if (mission > g_msave_prev) started = mission;
            else if (mission == 0 && g_msave_sub == 0 && sub == 1) started = 0;
            if (started != 0xFF) {
                char path[256]; std::snprintf(path, sizeof path, "%s/m%u.bin", g_msave_dir, (unsigned)started + 1);
                std::printf("[mission-save] mission %u at vbl %u -> %s\n", (unsigned)started + 1, vblank_count, path);
                savestate_req_save(path, 0);
            }
            g_msave_prev = mission; g_msave_sub = sub;
        }
        if (vblank_count + 60 == max_frames) { read_hist.clear(); read_hist_on = true; }
        if (vblank_count >= max_frames) { summary("frame limit reached"); std::exit(0); }
    }
}

static void hle_vbr0(sh4::Ctx* c) {
    // VBR+0 code installed by the game is "rte; nop" (copied from image 0x8C3297DC)
    if (sh4::rd16(c->vbr) != 0x002B) fatal("VBR+0 is not rte (%04x)", sh4::rd16(c->vbr));
    sh4::rte(c);
}

static bool load(const char* path, uint8_t* dst, size_t max) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    size_t n = std::fread(dst, 1, max, f);
    std::fclose(f);
    return n > 0;
}

void write_hist_print();
void write_hist_print() { hw::write_hist_print(); }
namespace hw { uint16_t* aica_regs(); }
uint16_t* aica_regs_export() { return hw::aica_regs(); }
void stack_base_set(uintptr_t b);
// DB_WAV=file: write the AICA output as a 44.1 kHz stereo WAV (header patched at exit)
namespace hw { extern void (*aica_audio_sink)(int16_t, int16_t); }
static FILE* wav_file; static uint32_t wav_samples;
static void wav_sink(int16_t l, int16_t r) { int16_t s[2] = {l, r}; std::fwrite(s, 2, 2, wav_file); wav_samples++; }
static void wav_close() {
    if (!wav_file) return;
    uint32_t data = wav_samples * 4, riff = 36 + data, fmt_len = 16, rate = 44100, brate = 44100 * 4;
    uint16_t pcm = 1, chans = 2, align = 4, bits = 16;
    std::fseek(wav_file, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, wav_file); std::fwrite(&riff, 4, 1, wav_file); std::fwrite("WAVEfmt ", 1, 8, wav_file);
    std::fwrite(&fmt_len, 4, 1, wav_file); std::fwrite(&pcm, 2, 1, wav_file); std::fwrite(&chans, 2, 1, wav_file);
    std::fwrite(&rate, 4, 1, wav_file); std::fwrite(&brate, 4, 1, wav_file); std::fwrite(&align, 2, 1, wav_file); std::fwrite(&bits, 2, 1, wav_file);
    std::fwrite("data", 1, 4, wav_file); std::fwrite(&data, 4, 1, wav_file);
    std::fclose(wav_file); wav_file = nullptr;
}
int main(int argc, char** argv) {
    { extern int draw_skip_mode_set(int); if (getenv("DB_DRAWSKIP")) draw_skip_mode_set(1); }
    if (const char* w = getenv("DB_WAV")) { wav_file = std::fopen(w, "wb"); if (wav_file) { std::fseek(wav_file, 44, SEEK_SET); hw::aica_audio_sink = wav_sink; } }
    { char base; stack_base_set((uintptr_t)&base); }
    std::string base = getenv("DB_BASE") ? std::string(getenv("DB_BASE")) : (std::string(std::getenv("HOME")) + "/dolphinblue");
    std::string disc = base + "/extract/disc";
    std::string input;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) max_frames = std::atoi(argv[++i]);
        else if (a == "--disc" && i + 1 < argc) disc = argv[++i];
        else if (a == "--dump" && i + 1 < argc) frame_dir = argv[++i];
        else if (a == "--trace") trace_level = 1;
        else if (a == "--strict") strict = true;
        else if (a == "--input" && i + 1 < argc) input = argv[++i];
        else if (a == "--png" && i + 1 < argc) png_dir = argv[++i];
        else if (a == "--png-every" && i + 1 < argc) png_every = std::atoi(argv[++i]);
        else if (a == "--png-renders" && i + 1 < argc) { std::stringstream ss(argv[++i]); std::string t; while (std::getline(ss, t, ',')) png_list.push_back(std::atoi(t.c_str())); }
#ifdef SH4_DEBUG
        else if (a == "--watch" && i + 2 < argc) { sh4::watch_lo = std::strtoul(argv[i + 1], 0, 16); sh4::watch_hi = std::strtoul(argv[i + 2], 0, 16); i += 2; }
#endif
    }
    if (getenv("DB_XFASYNC") || getenv("DB_XFDEFER")) {   // transform worker: a thread, or deferred to the waits (finds early readers)
        extern void nv_xf_enable(void (*kick)(), bool strips); extern bool nv_worker_step(); extern bool nv_defer;
        nv_xf_enable(nullptr, false);
        if (getenv("DB_XFDEFER")) nv_defer = true;
        else std::thread([] { for (;;) if (!nv_worker_step()) std::this_thread::yield(); }).detach();
    }
    if (getenv("DB_NVASYNC")) {   // test the strip-writer worker on the desktop (output must stay bit-identical)
        extern void nv_async_enable(void (*kick)()); extern bool nv_worker_step();
        nv_async_enable(nullptr);
        std::thread([] { for (;;) if (!nv_worker_step()) std::this_thread::yield(); }).detach();
    }
    if (getenv("DB_GDASYNC")) hw::gd_async = [](void (*job)()) { std::thread(job).detach(); };   // test: asynchronous disc reads on a worker
    {   // save states (desktop test): DB_SAVE_AT=vblank:path, DB_LOAD=path, DB_MISSION_SAVE=DIR (per-mission slots)
        extern void savestate_req_save(const char*, uint32_t); extern void savestate_req_load(const char*);
        if (const char* sa = getenv("DB_SAVE_AT")) { const char* col = std::strchr(sa, ':'); if (col) savestate_req_save(col + 1, (uint32_t)std::atoi(sa)); }
        if (const char* ld = getenv("DB_LOAD")) savestate_req_load(ld);
        g_msave_dir = getenv("DB_MISSION_SAVE");
    }
    if (const char* sw = getenv("DB_AWSWITCH")) { extern void aw_switch_test(uint8_t); aw_switch_test((uint8_t)std::strtoul(sw, nullptr, 0)); }   // test: hold switches
    if (!input.empty()) {
        std::ifstream f(input); std::string line;
        static const struct { const char* n; uint16_t m; } btn[] = {{"c", 1 << 0}, {"b", 1 << 1}, {"a", 1 << 2}, {"start", 1 << 3},
            {"up", 1 << 4}, {"down", 1 << 5}, {"left", 1 << 6}, {"right", 1 << 7}, {"z", 1 << 8}, {"y", 1 << 9}, {"x", 1 << 10},
            {"service", 1 << 13}, {"test", 1 << 14}};
        while (std::getline(f, line)) {
            std::istringstream ss(line); Press p{}; std::string b;
            if (!(ss >> p.from >> p.to >> b) || line[0] == '#') continue;
            if (b == "coin") p.coin = true;
            for (auto& x : btn) if (b == x.n) p.mask = x.m;
            if (b.compare(0, 3, "bit") == 0) p.mask = (uint16_t)(1u << std::atoi(b.c_str() + 3));   // raw pad bit
            script.push_back(p);
        }
    }
    static std::vector<uint8_t> m_vram(VRAM_SIZE), m_aram(ARAM_SIZE), m_oc(8192), m_sram(128 * 1024, 0xFF);
    hw::ram = sh4::ram; vram = m_vram.data(); aram = m_aram.data(); ocram = m_oc.data(); sram = m_sram.data();

    if (!load((base + "/extract/IP.BIN").c_str(), ram + 0x8000, 0x8000)) fatal("cannot load IP.BIN");
    if (!load((base + "/extract/1ST_READ.BIN").c_str(), ram + 0x10000, RAM_SIZE - 0x10000)) fatal("cannot load 1ST_READ.BIN");
    if (!disc_open(disc)) fatal("cannot open GDI in %s", disc.c_str());

    static sh4::Ctx c{};
    cpu = &c;
    c.sr = 0x700000F0u & ~1u; c.t = 0;
    c.r[15] = 0x8C00F400u;
    c.vbr = 0x8C00F400u;
    c.fpscr = 0x00040001u;
    c.slice = SLICE_POLLS;

    sh4p_init(); pvr_init(); aica_init();
    if (getenv("DB_ARM_THREAD")) {   // test the Vita's threaded ARM7 mode on the desktop (not deterministic)
        hw::aica_set_threaded(true);
        std::thread([] { for (;;) if (!hw::aica_arm_thread_step(44100 / 4)) std::this_thread::sleep_for(std::chrono::microseconds(500)); }).detach();
    }
    bios_install(&c);
    hle_register(0x8C00F400u, hle_vbr0);

    poll_hook = on_poll_check;
    if (!png_dir.empty() || getenv("DB_DECODEBENCH")) hw::render_hook = on_render;
    std::printf("starting at 0x8C010000, frame limit %u\n", max_frames);
    f_8c010000(&c);
    summary("entry function returned");
    return 1;
}

void (*poll_hook)() = nullptr;
