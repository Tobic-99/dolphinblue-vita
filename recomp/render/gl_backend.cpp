// Desktop backend: headless OpenGL 3.3 core (EGL surfaceless, Mesa) consuming DrawLists.
// Mirrors the planned libgxm backend: one fixed shader family selected by ShaderKey bits,
// blend/depth state per batch, textures uploaded once by the texture cache.
#include "draw.h"
#include "egl_min.h"
#include <GL/glcorearb.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstddef>

namespace render {

#define GLFUNCS(X) \
    X(PFNGLGENTEXTURESPROC, glGenTextures) X(PFNGLBINDTEXTUREPROC, glBindTexture) X(PFNGLTEXIMAGE2DPROC, glTexImage2D) \
    X(PFNGLTEXPARAMETERIPROC, glTexParameteri) X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
    X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) X(PFNGLVIEWPORTPROC, glViewport) \
    X(PFNGLCLEARCOLORPROC, glClearColor) X(PFNGLCLEARDEPTHPROC, glClearDepth) X(PFNGLCLEARPROC, glClear) \
    X(PFNGLENABLEPROC, glEnable) X(PFNGLDISABLEPROC, glDisable) X(PFNGLDEPTHFUNCPROC, glDepthFunc) \
    X(PFNGLDEPTHMASKPROC, glDepthMask) X(PFNGLBLENDFUNCPROC, glBlendFunc) X(PFNGLREADPIXELSPROC, glReadPixels) \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM1FPROC, glUniform1f) \
    X(PFNGLUNIFORM2FPROC, glUniform2f) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLDRAWARRAYSPROC, glDrawArrays) X(PFNGLDRAWELEMENTSBASEVERTEXPROC, glDrawElementsBaseVertex) X(PFNGLGETERRORPROC, glGetError) X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
    X(PFNGLPIXELSTOREIPROC, glPixelStorei) X(PFNGLUNIFORM3FPROC, glUniform3f) X(PFNGLUNIFORM1FVPROC, glUniform1fv) X(PFNGLGETSTRINGPROC, glGetString)
#define DECL(t, n) static t n;
GLFUNCS(DECL)

static const char* VS = R"(#version 330 core
// Screen-space positions with w = 1; perspective correction is done explicitly (attributes are
// pre-multiplied by 1/w and divided back per fragment), exactly like the PVR's per-pixel 1/w.
layout(location=0) in vec3 a_pos; layout(location=1) in vec2 a_uv; layout(location=2) in vec4 a_base; layout(location=3) in vec4 a_offs;
uniform vec2 u_scale;
out vec3 v_uvq; out vec4 v_base; out vec4 v_offs; out float v_invw;
void main() {
    float invw = a_pos.z;
    vec2 ndc = vec2(a_pos.x * u_scale.x - 1.0, 1.0 - a_pos.y * u_scale.y);
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uvq = vec3(a_uv * invw, invw);
    v_base = a_base * invw; v_offs = a_offs * invw; v_invw = invw;
})";
static const char* FS = R"(#version 330 core
in vec3 v_uvq; in vec4 v_base; in vec4 v_offs; in float v_invw;
uniform sampler2D u_tex; uniform int u_key; uniform float u_alpha_ref;
uniform float u_fog_density; uniform vec3 u_fog_ram; uniform vec3 u_fog_vert; uniform float u_fog_hi[128]; uniform float u_fog_lo[128];
float table_fog(float invw) {
    float z = clamp(u_fog_density * invw, 1.0, 255.9999);
    float e = floor(log2(z));
    float m = z * 16.0 / exp2(e) - 16.0;
    int idx = int(floor(m) + e * 16.0);
    return mix(u_fog_hi[idx], u_fog_lo[idx], m - floor(m));
}
out vec4 o_col;
void main() {
    bool textured = (u_key & 1) != 0, offs = (u_key & 2) != 0, atest = (u_key & 4) != 0;
    bool ign_ta = (u_key & 8) != 0, use_a = (u_key & 16) != 0; int shading = (u_key >> 5) & 3;
    float q = 1.0 / v_invw;
    vec2 v_uv = v_uvq.xy * q;
    vec4 base = v_base * q; vec4 offc = v_offs * q;
    if (!use_a) base.a = 1.0;
    vec4 c = base;
    if (textured) {
        vec4 t = texture(u_tex, v_uv); if (ign_ta) t.a = 1.0;
        if (shading == 0) c = t;
        else if (shading == 1) c = vec4(t.rgb * base.rgb, t.a);
        else if (shading == 2) c = vec4(mix(base.rgb, t.rgb, t.a), base.a);
        else c = t * base;
        if (offs) c.rgb += offc.rgb;
    }
    c = clamp(c, 0.0, 1.0);
    int fog = (u_key >> 7) & 3;
    if (fog == 0) c.rgb = mix(c.rgb, u_fog_ram, table_fog(v_invw));
    else if (fog == 1 && offs) c.rgb = mix(c.rgb, u_fog_vert, offc.a);
    else if (fog == 3) c = vec4(u_fog_ram, table_fog(v_invw));
    if (atest && c.a < u_alpha_ref) discard;
    o_col = c;
    gl_FragDepth = v_invw / (1.0 + v_invw);
})";

static EGLDisplay dpy;
static GLuint prog, vao, vbo, fbo, color_tex, depth_rb;
static int fb_w, fb_h;
static GLint loc_scale, loc_key, loc_aref, loc_tex, loc_fd, loc_fram, loc_fvert, loc_fhi, loc_flo;

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type); glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log); std::fprintf(stderr, "shader: %s\n", log); }
    return s;
}
static void make_fb(int w, int h) {
    if (fbo && w == fb_w && h == fb_h) return;
    fb_w = w; fb_h = h;
    if (!fbo) { glGenFramebuffers(1, &fbo); glGenTextures(1, &color_tex); glGenRenderbuffers(1, &depth_rb); }
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) std::fprintf(stderr, "FBO incomplete\n");
}

bool backend_init(int w, int h) {
    dpy = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint a, b;
    if (!eglInitialize(dpy, &a, &b)) return false;
    EGLint attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig cfg = nullptr; EGLint n = 0;
    eglChooseConfig(dpy, attr, &cfg, 1, &n);
    eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, n ? cfg : nullptr, EGL_NO_CONTEXT, ca);
    if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) return false;
#define LOADF(t, nm) nm = (t)eglGetProcAddress(#nm); if (!nm) { std::fprintf(stderr, "missing GL %s\n", #nm); return false; }
    GLFUNCS(LOADF)
    std::printf("[render] GL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, VS)); glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(prog);
    loc_scale = glGetUniformLocation(prog, "u_scale"); loc_key = glGetUniformLocation(prog, "u_key");
    loc_aref = glGetUniformLocation(prog, "u_alpha_ref"); loc_tex = glGetUniformLocation(prog, "u_tex");
    loc_fd = glGetUniformLocation(prog, "u_fog_density"); loc_fram = glGetUniformLocation(prog, "u_fog_ram");
    loc_fvert = glGetUniformLocation(prog, "u_fog_vert"); loc_fhi = glGetUniformLocation(prog, "u_fog_hi"); loc_flo = glGetUniformLocation(prog, "u_fog_lo");
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, x));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, u));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, base));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (void*)offsetof(Vertex, offs));
    make_fb(w, h);
    return true;
}

bool backend_texture_pressure() { return false; }
void backend_texture_flush() {}
uint32_t backend_upload_texture(uint32_t id, int w, int h, const uint32_t* argb, int) {
    GLuint t = id;
    if (!t) glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, argb);
    return t;
}

static GLenum blend_src(int c) {
    static const GLenum m[8] = {GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    return m[c & 7];
}
static GLenum blend_dst(int c) {
    static const GLenum m[8] = {GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    return m[c & 7];
}
static const GLenum depth_funcs[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};

void backend_render(const DrawList& dl, std::vector<uint32_t>& out) {
    make_fb(dl.width, dl.height);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, dl.width, dl.height);
    uint32_t c = dl.clear_color;
    glClearColor(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, 1.f);
    float cd = dl.clear_depth > 0 ? dl.clear_depth / (1 + dl.clear_depth) : 0;
    glClearDepth(cd);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog);
    glUniform2f(loc_scale, 2.0f / dl.width, 2.0f / dl.height);
    glUniform1f(loc_aref, dl.alpha_ref);
    glUniform1i(loc_tex, 0);
    glUniform1f(loc_fd, dl.fog_density);
    auto rgb = [](uint32_t c, float* f) { f[0] = ((c >> 16) & 255) / 255.f; f[1] = ((c >> 8) & 255) / 255.f; f[2] = (c & 255) / 255.f; };
    float fr[3], fv[3]; rgb(dl.fog_col_ram, fr); rgb(dl.fog_col_vert, fv);
    glUniform3f(loc_fram, fr[0], fr[1], fr[2]); glUniform3f(loc_fvert, fv[0], fv[1], fv[2]);
    float hi[128], lo[128]; for (int k = 0; k < 128; ++k) { hi[k] = dl.fog_hi[k] / 255.f; lo[k] = dl.fog_lo[k] / 255.f; }
    glUniform1fv(loc_fhi, 128, hi); glUniform1fv(loc_flo, 128, lo);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, dl.verts.size() * sizeof(Vertex), dl.verts.data(), GL_STREAM_DRAW);
    static GLuint ebo; if (!ebo) glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, dl.idx.size() * 2, dl.idx.data(), GL_STREAM_DRAW);
    glEnable(GL_DEPTH_TEST);
    static const int order[3] = {L_OPAQUE, L_PUNCH, L_TRANS};
    static int only = getenv("DB_ONLYLIST") ? atoi(getenv("DB_ONLYLIST")) : -1;
    for (int li : order) {
        if (only >= 0 && li != only) continue;
        static int onlyb = getenv("DB_ONLYBATCH") ? atoi(getenv("DB_ONLYBATCH")) : -1;
        int bi = -1;
        for (const Batch& b : dl.lists[li]) {
            ++bi;
            if (onlyb >= 0 && bi != onlyb) continue;
            if (!b.count) continue;
            if (li == L_TRANS) { glEnable(GL_BLEND); glBlendFunc(blend_src(b.src_blend), blend_dst(b.dst_blend)); }
            else glDisable(GL_BLEND);
            glDepthFunc(getenv("DB_TR_ALWAYS") && li == L_TRANS ? GL_ALWAYS : depth_funcs[b.depth_func & 7]);
            glDepthMask(b.depth_write ? GL_TRUE : GL_FALSE);
            glUniform1i(loc_key, (int)b.key.bits());
            glUniform1f(loc_aref, b.aref >= 0 ? b.aref : dl.alpha_ref);
            if (b.key.textured && b.tex_id) {
                glBindTexture(GL_TEXTURE_2D, b.tex_id);
                GLint wu = b.clamp_u ? GL_CLAMP_TO_EDGE : b.flip_u ? GL_MIRRORED_REPEAT : GL_REPEAT;
                GLint wv = b.clamp_v ? GL_CLAMP_TO_EDGE : b.flip_v ? GL_MIRRORED_REPEAT : GL_REPEAT;
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wu);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wv);
                GLint f = b.bilinear ? GL_LINEAR : GL_NEAREST;
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
            }
            if (const char* tr = getenv("DB_TRIRANGE")) {
                int a0 = atoi(tr), a1 = atoi(strchr(tr, ',') + 1);
                int n = (int)b.count / 3; if (a1 > n) a1 = n;
                if (a0 < a1) glDrawElementsBaseVertex(GL_TRIANGLES, 3 * (a1 - a0), GL_UNSIGNED_SHORT, (void*)(uintptr_t)((b.first + 3 * a0) * 2), (GLint)b.vbase);
            } else
            glDrawElementsBaseVertex(GL_TRIANGLES, b.count, GL_UNSIGNED_SHORT, (void*)(uintptr_t)(b.first * 2), (GLint)b.vbase);
        }
    }
    if (const char* pk = getenv("DB_DEPTHAT")) {
        int px = atoi(pk), py = atoi(strchr(pk, ',') + 1); float dv = -1;
        glReadPixels(px, dl.height - 1 - py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &dv);
        std::printf("[depth] at %d,%d = %g (invw %g)\n", px, py, dv, dv / (1 - dv));
    }
    out.resize((size_t)dl.width * dl.height);
    std::vector<uint32_t> tmp(out.size());
    glReadPixels(0, 0, dl.width, dl.height, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    for (int y = 0; y < dl.height; ++y)
        std::memcpy(&out[(size_t)y * dl.width], &tmp[(size_t)(dl.height - 1 - y) * dl.width], dl.width * 4);
}

} // namespace render
