// Minimal headless libretro frontend used only to run Flycast as the comparison reference.
// usage: lr_front <core.so> <game.gdi> <frames> <system_dir>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include "libretro.h"

static const char* sysdir;
static retro_hw_context_reset_t hw_reset;
static void log_cb(enum retro_log_level level, const char* fmt, ...) {
    if (level < (getenv("LR_VERBOSE") ? RETRO_LOG_DEBUG : RETRO_LOG_WARN)) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
static bool env_cb(unsigned cmd, void* data) {
    switch (cmd & 0xFFFF) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char**)data = sysdir; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((retro_log_callback*)data)->log = log_cb; return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {                       // accepted, but no context is ever created
        retro_hw_render_callback* cb = (retro_hw_render_callback*)data;
        hw_reset = cb->context_reset;
        cb->get_current_framebuffer = []() -> uintptr_t { return 0; };
        cb->get_proc_address = [](const char* name) -> retro_proc_address_t {
            if (!strcmp(name, "glGetString")) return (retro_proc_address_t)+[](unsigned) -> const char* { return ""; };
            return (retro_proc_address_t)+[]() -> uintptr_t { return 0; };   // GL calls become no-ops
        };
        return true;
    }
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: *(unsigned*)data = RETRO_HW_CONTEXT_OPENGL_CORE; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        retro_variable* v = (retro_variable*)data;
        if (!strcmp(v->key, "reicast_hle_bios") || !strcmp(v->key, "flycast_hle_bios")) { v->value = "enabled"; return true; }
        if (!strcmp(v->key, "reicast_threaded_rendering") || !strcmp(v->key, "flycast_threaded_rendering")) { v->value = "disabled"; return true; }
        v->value = nullptr; return false;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
    }
    return false;
}
static void video_cb(const void*, unsigned, unsigned, size_t) {}
static FILE* wav; static uint32_t wav_frames;   // LR_WAV=file: raw 44.1 kHz stereo s16 capture (header written at exit)
static void audio_cb(int16_t l, int16_t r) { if (wav) { int16_t s[2] = {l, r}; fwrite(s, 2, 2, wav); wav_frames++; } }
static size_t audio_batch_cb(const int16_t* d, size_t f) { if (wav) { fwrite(d, 4, f, wav); wav_frames += f; } return f; }
static void wav_finish() {
    if (!wav) return;
    uint32_t data = wav_frames * 4, riff = 36 + data, fl = 16, rate = 44100, br = 44100 * 4; uint16_t pcm = 1, ch = 2, al = 4, bits = 16;
    fseek(wav, 0, SEEK_SET); fwrite("RIFF", 1, 4, wav); fwrite(&riff, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav); fwrite(&fl, 4, 1, wav);
    fwrite(&pcm, 2, 1, wav); fwrite(&ch, 2, 1, wav); fwrite(&rate, 4, 1, wav); fwrite(&br, 4, 1, wav); fwrite(&al, 2, 1, wav); fwrite(&bits, 2, 1, wav);
    fwrite("data", 1, 4, wav); fwrite(&data, 4, 1, wav); fclose(wav);
}
static void input_poll_cb() {}
static int16_t input_state_cb(unsigned, unsigned, unsigned, unsigned) { return 0; }

#define LOAD(sym) auto p_##sym = (decltype(&sym))dlsym(h, #sym); if (!p_##sym) { fprintf(stderr, "missing %s\n", #sym); return 2; }
int main(int argc, char** argv) {
    if (argc < 5) return 1;
    sysdir = argv[4];
    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    LOAD(retro_set_environment) LOAD(retro_set_video_refresh) LOAD(retro_set_audio_sample) LOAD(retro_set_audio_sample_batch)
    LOAD(retro_set_input_poll) LOAD(retro_set_input_state) LOAD(retro_init) LOAD(retro_load_game) LOAD(retro_run)
    p_retro_set_environment(env_cb);
    p_retro_set_video_refresh(video_cb); p_retro_set_audio_sample(audio_cb); p_retro_set_audio_sample_batch(audio_batch_cb);
    p_retro_set_input_poll(input_poll_cb); p_retro_set_input_state(input_state_cb);
    p_retro_init();
    retro_game_info gi{}; gi.path = argv[2];
    if (!p_retro_load_game(&gi)) { fprintf(stderr, "load_game failed\n"); return 3; }
    if (hw_reset) hw_reset();   // lets the core load its GL entry points (all stubs here)
    if (const char* w = getenv("LR_WAV")) { wav = fopen(w, "wb"); if (wav) fseek(wav, 44, SEEK_SET); }
    int frames = atoi(argv[3]);
    for (int i = 0; i < frames; ++i) { p_retro_run(); if (i % 60 == 0) fprintf(stderr, "[lr] frame %d\n", i); }
    fprintf(stderr, "[lr] done %d frames\n", frames);
    wav_finish();
    return 0;
}
