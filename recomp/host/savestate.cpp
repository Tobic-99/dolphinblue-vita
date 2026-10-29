// Save states. The translated game runs on the host's own call stack, which cannot be written to a file, so a
// state is taken and restored only at one point: the frame-wait idle skip (dispatch of 0x8C31E9B0 from
// f_8c320140), where the game's call chain is always the same (guest SP 0x8C00F3AC, same host depth). Loading
// boots normally, waits until that point is reached with the saved SP and PR, and swaps the whole machine state
// in; the translated callers reload their registers from the context after each call, so they continue exactly
// as the saved game did.
#include "hw.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace hw {
void aica_hold(bool on);   // aica.cpp: park / release the ARM7 thread (threaded mode)
static constexpr uint32_t kMagic = 0x53534244;   // "DBSS"
static constexpr uint32_t kVersion = 2;   // 2: + the disc reader's state (bios.cpp)
static std::string save_path, load_path;
static uint32_t save_at_vbl = ~0u;
void savestate_request_save(const char* path, uint32_t at_vblank) { save_path = path; save_at_vbl = at_vblank; }
void savestate_request_load(const char* path) { load_path = path; }
void (*savestate_done)(bool saved, bool ok);     // platform notification (e.g. a message on screen)

static bool run_io(SaveIO& s) {
    aica_hold(true);
    static const bool trace = std::getenv("DB_STATETRACE") != nullptr;   // file offset after each block (finding format changes)
    auto at = [&](const char* name) { if (trace) std::printf("[state] %-6s ends at %ld\n", name, s.mem ? (long)s.mem->size() : std::ftell(s.f)); };
    sh4_state(s); at("sh4"); holly_state(s); at("holly"); aica_state(s); at("aica"); aica_mix_state(s); at("mix");
    aica_dsp_state(s); at("dsp"); arm_state(s); at("arm"); bios_state(s); at("bios"); mem_state(s); at("mem");
    aica_hold(false);
    return s.ok;
}
// Saving: the state is copied into memory at the frame-wait point (fast), then written to the file by
// savestate_writer (platform: a background thread on the Vita; synchronous when null).
void (*savestate_writer)(std::vector<uint8_t>* data, const std::string& path);
static std::vector<uint8_t>* save_buf;
bool savestate_write_file(const std::vector<uint8_t>& data, const std::string& path) {
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    if (ok) { std::remove(path.c_str()); ok = std::rename(tmp.c_str(), path.c_str()) == 0; }
    return ok;
}
static void do_save(sh4::Ctx* c) {
    if (!save_buf) { save_buf = new std::vector<uint8_t>; save_buf->reserve(30u << 20); }
    save_buf->clear();
    uint32_t hdr[4] = {kMagic, kVersion, c->r[15], c->pr};
    SaveIO s{nullptr, false}; s.mem = save_buf; s.v(hdr);
    run_io(s);
    std::printf("[state] snapshot taken (%u bytes, vbl %u)\n", (unsigned)save_buf->size(), vblank_count);
    if (savestate_writer) { savestate_writer(save_buf, save_path); save_buf = nullptr; return; }   // the writer owns the buffer now
    bool ok = savestate_write_file(*save_buf, save_path);
    std::printf("[state] save %s: %s\n", save_path.c_str(), ok ? "ok" : "FAILED");
    if (savestate_done) savestate_done(true, ok);
}
static void do_load(sh4::Ctx* c) {
    FILE* f = std::fopen(load_path.c_str(), "rb");
    if (!f) { std::printf("[state] no state file %s\n", load_path.c_str()); load_path.clear(); return; }
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    uint32_t hdr[4]; SaveIO s{f, true}; s.v(hdr);
    s.version = hdr[1];
    if (!s.ok || hdr[0] != kMagic || hdr[1] < 1 || hdr[1] > kVersion) { std::fclose(f); std::printf("[state] %s: not a valid state\n", load_path.c_str()); load_path.clear(); return; }
    if (hdr[2] != c->r[15] || hdr[3] != c->pr) { std::fclose(f); return; }   // not the same call chain yet: try at a later frame
    bool ok = run_io(s);
    if (!ok) { long at = std::ftell(f); std::fseek(f, 0, SEEK_END); long fsz = std::ftell(f); std::printf("[state] INCONSISTENT: last read wanted %zu got %zu at %ld, file size %ld\n", s.fail_want, s.fail_got, at, fsz); }
    std::fclose(f);
    std::printf("[state] load %s: %s (now vbl %u)\n", load_path.c_str(), ok ? "ok" : "FAILED (state is inconsistent)", vblank_count);
    load_path.clear();
    if (savestate_done) savestate_done(false, ok);
}
// called from the idle skip (sh4p.cpp) with the guest at the frame-wait point
void savestate_poll(sh4::Ctx* c) {
    if (!load_path.empty()) { do_load(c); return; }
    if (!save_path.empty() && vblank_count >= save_at_vbl) { do_save(c); save_path.clear(); save_at_vbl = ~0u; }
}
} // namespace hw
