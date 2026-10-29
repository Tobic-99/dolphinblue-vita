// Headless libretro frontend with a real (Mesa llvmpipe, EGL surfaceless) GL context: runs the Flycast
// reference core and saves its output frames as PNG for visual comparison. Desktop-only harness.
// usage: lr_shot <core.so> <game.gdi> <frames> <system_dir> <out_dir> <every>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <vector>
#include "libretro.h"
#include "../render/egl_min.h"
#include <GL/glcorearb.h>
namespace render { bool write_png(const char* path, int w, int h, const uint32_t* rgba); }

static const char* sysdir; static const char* outdir;
static retro_hw_render_callback hw;
static GLuint fbo, ctex, drb;
static int frame_no, every = 10;
static unsigned* render_count;
static std::vector<unsigned> want; static std::vector<bool> got;
static PFNGLREADPIXELSPROC p_glReadPixels; static PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer;
static void log_cb(enum retro_log_level level, const char* fmt, ...) {
    if (level < RETRO_LOG_WARN) return;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}
static uintptr_t get_fb() { return fbo; }
static retro_proc_address_t get_proc(const char* n) { return (retro_proc_address_t)eglGetProcAddress(n); }
static bool env_cb(unsigned cmd, void* data) {
    switch (cmd & 0xFFFF) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)data = sysdir; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((retro_log_callback*)data)->log = log_cb; return true;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: *(unsigned*)data = RETRO_HW_CONTEXT_OPENGL_CORE; return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        retro_hw_render_callback* cb = (retro_hw_render_callback*)data;
        if (cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE && cb->context_type != RETRO_HW_CONTEXT_OPENGL) return false;
        cb->get_current_framebuffer = get_fb; cb->get_proc_address = get_proc;
        hw = *cb; return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        retro_variable* v = (retro_variable*)data;
        if (strstr(v->key, "hle_bios")) { v->value = "enabled"; return true; }
        if (strstr(v->key, "threaded_rendering")) { v->value = "disabled"; return true; }
        if (strstr(v->key, "internal_resolution")) { v->value = "640x480"; return true; }
        v->value = nullptr; return false;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
    }
    return false;
}
static void video_cb(const void* data, unsigned w, unsigned h, size_t) {
    frame_no++;
    if (data != RETRO_HW_FRAME_BUFFER_VALID) return;
    char p[512];
    if (!want.empty()) {   // capture by render index (matches the recompiled runtime's render_NNNNN)
        unsigned idx = *render_count - 1; int k = -1;
        for (size_t i = 0; i < want.size(); ++i) if (want[i] == idx && !got[i]) k = (int)i;
        if (k < 0) return;
        got[k] = true;
        snprintf(p, sizeof p, "%s/fc_render_%05u.png", outdir, idx);
    } else {
        if (frame_no % every) return;
        snprintf(p, sizeof p, "%s/fc_frame_%05d.png", outdir, frame_no);
    }
    std::vector<uint32_t> px((size_t)w * h), out((size_t)w * h);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    p_glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    for (unsigned y = 0; y < h; ++y) memcpy(&out[(size_t)y * w], &px[(size_t)(hw.bottom_left_origin ? h - 1 - y : y) * w], w * 4);
    render::write_png(p, w, h, out.data());
}
static void audio_cb(int16_t, int16_t) {}
static size_t audio_batch_cb(const int16_t*, size_t f) { return f; }
static void input_poll_cb() {}
static int run_frame;
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    // LR_START="a-b,c-d": press START on port 0 during these retro_run frame ranges
    static const char* spec = getenv("LR_START");
    if (!spec || port != 0 || dev != RETRO_DEVICE_JOYPAD || id != RETRO_DEVICE_ID_JOYPAD_START) return 0;
    const char* p = spec;
    while (*p) { int a = atoi(p); const char* d = strchr(p, '-'); if (!d) break; int b = atoi(d + 1);
        if (run_frame >= a && run_frame <= b) return 1;
        const char* c = strchr(d, ','); if (!c) break; p = c + 1; }
    return 0;
}

#define LOAD(sym) auto p_##sym = (decltype(&sym))dlsym(h, #sym); if (!p_##sym) { fprintf(stderr, "missing %s\n", #sym); return 2; }
int main(int argc, char** argv) {
    if (argc < 7) return 1;
    sysdir = argv[4]; outdir = argv[5];
    if (strchr(argv[6], ',')) { char* t = strtok(argv[6], ","); while (t) { want.push_back(atoi(t)); got.push_back(false); t = strtok(nullptr, ","); } }
    else every = atoi(argv[6]);
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint a, b; eglInitialize(d, &a, &b);
    EGLint attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg; EGLint n = 0; eglChooseConfig(d, attr, &cfg, 1, &n);
    eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext c = eglCreateContext(d, n ? cfg : nullptr, EGL_NO_CONTEXT, ca);
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c);
    auto glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)eglGetProcAddress("glGenFramebuffers");
    auto glGenTextures = (PFNGLGENTEXTURESPROC)eglGetProcAddress("glGenTextures");
    auto glBindTexture = (PFNGLBINDTEXTUREPROC)eglGetProcAddress("glBindTexture");
    auto glTexImage2D = (PFNGLTEXIMAGE2DPROC)eglGetProcAddress("glTexImage2D");
    auto glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)eglGetProcAddress("glFramebufferTexture2D");
    auto glGenRenderbuffers = (PFNGLGENRENDERBUFFERSPROC)eglGetProcAddress("glGenRenderbuffers");
    auto glBindRenderbuffer = (PFNGLBINDRENDERBUFFERPROC)eglGetProcAddress("glBindRenderbuffer");
    auto glRenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEPROC)eglGetProcAddress("glRenderbufferStorage");
    auto glFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)eglGetProcAddress("glFramebufferRenderbuffer");
    p_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)eglGetProcAddress("glBindFramebuffer");
    p_glReadPixels = (PFNGLREADPIXELSPROC)eglGetProcAddress("glReadPixels");
    glGenFramebuffers(1, &fbo); glGenTextures(1, &ctex); glGenRenderbuffers(1, &drb);
    glBindTexture(GL_TEXTURE_2D, ctex); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindRenderbuffer(GL_RENDERBUFFER, drb); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 640, 480);
    p_glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, drb);

    void* h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    LOAD(retro_set_environment) LOAD(retro_set_video_refresh) LOAD(retro_set_audio_sample) LOAD(retro_set_audio_sample_batch)
    LOAD(retro_set_input_poll) LOAD(retro_set_input_state) LOAD(retro_init) LOAD(retro_load_game) LOAD(retro_run)
    render_count = (unsigned*)dlsym(h, "db_render_count_export");
    if (!want.empty() && !render_count) { fprintf(stderr, "core lacks db_render_count_export\n"); return 2; }
    p_retro_set_environment(env_cb);
    p_retro_set_video_refresh(video_cb); p_retro_set_audio_sample(audio_cb); p_retro_set_audio_sample_batch(audio_batch_cb);
    p_retro_set_input_poll(input_poll_cb); p_retro_set_input_state(input_state_cb);
    p_retro_init();
    retro_game_info gi{}; gi.path = argv[2];
    if (!p_retro_load_game(&gi)) { fprintf(stderr, "load_game failed\n"); return 3; }
    if (hw.context_reset) hw.context_reset();
    int frames = atoi(argv[3]);
    for (int i = 0; i < frames; ++i) { run_frame = i; p_retro_run(); }
    fprintf(stderr, "[lr] done %d frames\n", frames);
    return 0;
}
