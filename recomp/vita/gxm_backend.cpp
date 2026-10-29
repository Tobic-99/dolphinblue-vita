// PS Vita backend for the PVR2 draw lists (libgxm). Same interface as the desktop GL backend:
// backend_init / backend_upload_texture / backend_render (render + flip in one).
// Shaders are Cg, compiled at first run with vitashark (libshacccg) and cached as .gxp files;
// a prebuilt bank shipped in app0:shaders/ is used first when present.
// The GXM setup follows the standard VitaSDK sample structure.
#include "draw.h"
#include <psp2/kernel/processmgr.h>
#include <psp2/gxm.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <vitashark.h>
#include <cstdio>
#include <string>
#include <set>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <map>
#include <vector>

extern uint64_t prof_gpu_us;   // vita_main.cpp
extern uint64_t frames_presented_dbg;
extern volatile int render_stage;
namespace render {

// Mission-select menu overlay (set by the game thread in vita_main.cpp, read here on the render thread).
volatile uint32_t ui_menu = 0;    // 1 while the mission-select menu is open
volatile uint32_t ui_cursor = 0;  // selected slot (0-based)
volatile uint32_t ui_count = 0;   // number of selectable missions
volatile uint32_t ui_slot[16];    // mission number (1-based) shown for each slot

static constexpr int SCR_W = 960, SCR_H = 544, BUFS = 3;    // triple buffering: with the 30 fps cap a finished frame waits for its turn on screen while the next is built
static const char* kCacheDir = "ux0:data/dolphinblue/shaders";

static inline size_t align(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }
static void* gpu_alloc(SceKernelMemBlockType type, size_t size, SceGxmMemoryAttribFlags attr) {
    size = align(size, type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256 * 1024 : 4096);
    SceUID uid = sceKernelAllocMemBlock("gpu", type, size, nullptr);
    if (uid < 0) { std::printf("[gxm] memblock alloc failed (%u bytes) %08x\n", (unsigned)size, uid); return nullptr; }
    void* p = nullptr; sceKernelGetMemBlockBase(uid, &p);
    if (sceGxmMapMemory(p, size, attr) < 0) return nullptr;
    return p;
}
static void* usse_alloc(size_t size, bool frag, unsigned int* off) {
    size = align(size, 4096);
    SceUID uid = sceKernelAllocMemBlock("usse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, nullptr);
    void* p = nullptr; sceKernelGetMemBlockBase(uid, &p);
    if (frag) sceGxmMapFragmentUsseMemory(p, size, off); else sceGxmMapVertexUsseMemory(p, size, off);
    return p;
}
static void* patcher_host_alloc(void*, unsigned int size) { return malloc(size); }
static void patcher_host_free(void*, void* p) { free(p); }

struct DisplayData { void* addr; };
static bool fps_cap = true;   // set in backend_init (ux0:data/dolphinblue/nocap)
static void display_cb(const void* data) {
    const DisplayData* d = (const DisplayData*)data;
    SceDisplayFrameBuf fb{}; fb.size = sizeof fb; fb.base = d->addr; fb.pitch = SCR_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8; fb.width = SCR_W; fb.height = SCR_H;
    // 30 fps cap: each frame stays on screen for at least 2 refreshes, so the frame rate is even instead of
    // swinging between 20 and 60 (ux0:data/dolphinblue/nocap turns it off)
    // (runs on GXM's display-queue thread: no file I/O or stdio here, the switch is read in backend_init)
    static unsigned last_v;
    if (fps_cap) while (sceDisplayGetVcount() - last_v < 1) sceDisplayWaitVblankStart();   // + the flip below = 2 refreshes per frame
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
    last_v = sceDisplayGetVcount();
}

static SceGxmContext* ctx;
static SceGxmRenderTarget* rt;
static SceGxmShaderPatcher* patcher;
static void* fb_mem[BUFS];
static SceGxmColorSurface surf[BUFS];
static SceGxmSyncObject* sync_obj[BUFS];
static SceGxmDepthStencilSurface depth;
static int back = 0, front = BUFS - 1;
static SceGxmShaderPatcherId vs_id;
static const SceGxmProgram *vs_gxp;
static SceGxmVertexProgram* vprog;
static std::map<uint32_t, SceGxmFragmentProgram*> fprogs;   // (variant << 9) | blend key
struct FVariant { const SceGxmProgram* gxp = nullptr; SceGxmShaderPatcherId id{}; const SceGxmProgramParameter *aref, *fogd, *fogram, *fogvert; };
static std::map<uint32_t, FVariant> fvariants;
static bool gpu_busy;
static const SceGxmProgramParameter *p_scale;
static uint16_t* indices;
static uint8_t* vbuf; static constexpr size_t VBUF_SIZE = 8u << 20;
static uint16_t* ibuf; static constexpr size_t IBUF_SIZE = 4u << 20;   // the frame's triangle indices (per batch, relative to its vbase)
static SceGxmTexture fog_tex; static uint32_t* fog_mem;

// texture memory: one mapped CDRAM block (LPDDR fallback), bump allocation with size-bucketed reuse
struct Tex { SceGxmTexture t; void* mem; size_t size; int w, h; SceGxmTextureFormat fmt; };
static std::vector<Tex> textures(1);
static uint8_t* tex_heap; static size_t tex_heap_size = 64u << 20, tex_heap_used;
static std::multimap<size_t, void*> tex_free;
// The build thread (textures) and the submit thread (draws) run concurrently: the texture table is shared under
// tex_lock, and texture memory replaced or freed while the GPU may still read it is released only once every frame
// submitted before that point has finished (tex_pending, by submit serial).
static std::atomic_flag tex_lock = ATOMIC_FLAG_INIT;
struct TexLock { TexLock() { while (tex_lock.test_and_set(std::memory_order_acquire)) {} } ~TexLock() { tex_lock.clear(std::memory_order_release); } };
static std::vector<std::pair<uint64_t, std::pair<size_t, void*>>> tex_pending;
static uint64_t submit_serial;
static void tex_retire(size_t size, void* mem) { tex_pending.push_back({submit_serial, {align(size, 256), mem}}); }
static void* tex_alloc(size_t n) {
    n = align(n, 256);
    auto it = tex_free.lower_bound(n);
    if (it != tex_free.end() && it->first < n * 2) { void* p = it->second; tex_free.erase(it); return p; }
    if (tex_heap_used + n > tex_heap_size) return nullptr;
    void* p = tex_heap + tex_heap_used; tex_heap_used += n; return p;
}

static const char* VS_SRC =
    "void main(float3 a_pos, float2 a_uv, float4 a_base, float4 a_offs, uniform float4 u_scale,\n"
    "  float4 out o_pos : POSITION, float3 out v_uvq : TEXCOORD0, float4 out v_base : TEXCOORD1,\n"
    "  float4 out v_offs : TEXCOORD2, float out v_w : TEXCOORD3)\n"
    "{\n"
    "  float invw = a_pos.z;\n"
    "  float w = 1.0 / max(invw, 1e-12);\n"
    "  float2 ndc = float2(a_pos.x * u_scale.x - 1.0, 1.0 - a_pos.y * u_scale.y) * u_scale.zw;\n"
    // a real w: the GPU interpolates the attributes perspective-correctly itself (nothing to divide per pixel)
    "  o_pos = float4(ndc * w, clamp(invw, 0.0, 1.0) * w, w);\n"
    "  v_uvq = float3(a_uv, 0.0);\n"
    "  v_base = a_base.zyxw; v_offs = a_offs.zyxw;\n"
    // w interpolated perspective-correctly = 1 / (1/w interpolated linearly on screen): the PVR's per-pixel 1/w for fog
    "  v_w = w;\n"
    "}\n";
// Fragment shader template, specialised per state with #defines (no uniform branches, discard only with ATEST):
// TEX, SHADE (0 decal, 1 modulate, 2 decal-alpha, 3 modulate-alpha), OFFS, IGN_TA, USE_ALPHA, FOG (0 table,
// 1 vertex, 2 none, 3 table mode 2), ATEST.
static const char* FS_SRC =
    "float4 main(float3 v_uvq : TEXCOORD0, float4 v_base : TEXCOORD1, float4 v_offs : TEXCOORD2, float v_w : TEXCOORD3,\n"
    "  uniform sampler2D u_tex : TEXUNIT0, uniform sampler2D u_fogtab : TEXUNIT1,\n"
    "  uniform float u_alpha_ref, uniform float u_fog_density, uniform float3 u_fog_ram, uniform float3 u_fog_vert) : COLOR\n"
    "{\n"
    "  float4 base = v_base;\n"
    "#if !USE_ALPHA\n"
    "  base.a = 1.0;\n"
    "#endif\n"
    "  float4 c = base;\n"
    "#if OFFS\n"
    "  float4 offc = v_offs;\n"
    "#endif\n"
    "#if TEX\n"
    "  float4 t = tex2D(u_tex, v_uvq.xy);\n"
    "#if IGN_TA\n"
    "  t.a = 1.0;\n"
    "#endif\n"
    "#if SHADE == 0\n"
    "  c = t;\n"
    "#elif SHADE == 1\n"
    "  c = float4(t.rgb * base.rgb, t.a);\n"
    "#elif SHADE == 2\n"
    "  c = float4(lerp(base.rgb, t.rgb, t.a), base.a);\n"
    "#else\n"
    "  c = t * base;\n"
    "#endif\n"
    "#if OFFS\n"
    "  c.rgb += offc.rgb;\n"
    "#endif\n"
    "  c = saturate(c);\n"
    "#endif\n"
    "#if FOG == 0 || FOG == 3\n"
    "  float z = clamp(u_fog_density / v_w, 1.0, 255.9999);\n"
    "  float e = floor(log2(z));\n"
    "  float m = z * 16.0 / exp2(e) - 16.0;\n"
    "  float idx = floor(m) + e * 16.0;\n"
    "  float4 ft = tex2D(u_fogtab, float2((idx + 0.5) / 128.0, 0.5));\n"
    "  float fv = lerp(ft.r, ft.g, m - floor(m));\n"
    "#if FOG == 0\n"
    "  c.rgb = lerp(c.rgb, u_fog_ram, fv);\n"
    "#else\n"
    "  c = float4(u_fog_ram, fv);\n"
    "#endif\n"
    "#elif FOG == 1 && OFFS\n"
    "  c.rgb = lerp(c.rgb, u_fog_vert, offc.a);\n"
    "#endif\n"
    "#if ATEST\n"
    "  if (c.a < u_alpha_ref) discard;\n"
    "#endif\n"
    "  return c;\n"
    "}\n";

static uint32_t fnv(const char* s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
static const SceGxmProgram* load_or_compile(const char* src, bool frag) {
    char path[256];
    uint32_t h = fnv(src);
    // 1) prebuilt bank in the VPK, 2) cache on ux0, 3) compile with vitashark and cache
    for (const char* dir : {"app0:shaders", kCacheDir}) {
        std::snprintf(path, sizeof path, "%s/%08x.gxp", dir, h);
        if (FILE* f = std::fopen(path, "rb")) {
            std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
            void* buf = malloc(n); std::fread(buf, 1, n, f); std::fclose(f);
            if (sceGxmProgramCheck((const SceGxmProgram*)buf) == 0) { std::printf("[gxm] shader %s\n", path); return (const SceGxmProgram*)buf; }
            free(buf);
        }
    }
    static bool shark_ok = [] {
        shark_install_log_cb([](const char* msg, shark_log_level lvl, int line) { std::printf("[shark] %d line %d: %s\n", (int)lvl, line, msg); });
        shark_set_warnings_level(SHARK_WARN_SILENT);
        int r = shark_init(nullptr);
        std::printf("[gxm] shark_init -> %d\n", r);
        return r >= 0;
    }();
    if (!shark_ok) { std::printf("[gxm] no shader cache and libshacccg unavailable\n"); return nullptr; }
    uint32_t size = (uint32_t)std::strlen(src);   // in: source length, out: program size
    SceGxmProgram* p = shark_compile_shader_extended(src, &size, frag ? SHARK_FRAGMENT_SHADER : SHARK_VERTEX_SHADER,
                                                     SHARK_OPT_DEFAULT, SHARK_ENABLE, SHARK_DISABLE, SHARK_ENABLE);
    if (!p) { std::printf("[gxm] shader compile failed\n"); return nullptr; }
    void* copy = malloc(size); std::memcpy(copy, p, size);
    shark_clear_output();
    sceIoMkdir(kCacheDir, 0777);
    std::snprintf(path, sizeof path, "%s/%08x.gxp", kCacheDir, h);
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(copy, 1, size, f); std::fclose(f); }
    std::printf("[gxm] compiled and cached %s (%u bytes)\n", path, size);
    return (const SceGxmProgram*)copy;
}

// Offline shader bank: with ux0:data/dolphinblue/shaderbank present, compile every distinct fragment variant
// once (cached as <hash>.gxp in the shader cache dir). Those files ship in the VPK under app0:shaders, so the
// game never runs the shader compiler.
static uint32_t variant_key(const ShaderKey& k);
static FVariant* fvariant(uint32_t v);
static void build_shader_bank() {
    std::set<uint32_t> keys;
    for (uint32_t bits = 0; bits < 512; ++bits) {
        ShaderKey k{};
        k.textured = bits & 1; k.offset = (bits >> 1) & 1; k.alpha_test = (bits >> 2) & 1; k.ignore_tex_alpha = (bits >> 3) & 1;
        k.use_alpha = (bits >> 4) & 1; k.shading = (bits >> 5) & 3; k.fog = (bits >> 7) & 3;
        keys.insert(variant_key(k));
    }
    std::printf("[gxm] shader bank: %u variants\n", (unsigned)keys.size());
    unsigned ok = 0;
    for (uint32_t v : keys) if (fvariant(v)->gxp) ++ok;
    std::printf("[gxm] shader bank done: %u/%u compiled or loaded\n", ok, (unsigned)keys.size());
}

bool backend_init(int, int) {
    SceGxmInitializeParams ip{};
    ip.displayQueueMaxPendingCount = BUFS - 1;
    ip.displayQueueCallback = display_cb;
    ip.displayQueueCallbackDataSize = sizeof(DisplayData);
    ip.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    int e;
    if ((e = sceGxmInitialize(&ip)) < 0) { std::printf("[gxm] sceGxmInitialize %08x\n", e); return false; }

    void* vdm = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
    void* vring = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
    void* fring = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
    unsigned int fusse_off;
    void* fusse = usse_alloc(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, true, &fusse_off);
    SceGxmContextParams cp{};
    cp.hostMem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE); cp.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    cp.vdmRingBufferMem = vdm; cp.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    cp.vertexRingBufferMem = vring; cp.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    cp.fragmentRingBufferMem = fring; cp.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferMem = fusse; cp.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    cp.fragmentUsseRingBufferOffset = fusse_off;
    if ((e = sceGxmCreateContext(&cp, &ctx)) < 0) { std::printf("[gxm] sceGxmCreateContext %08x\n", e); return false; }

    SceGxmRenderTargetParams rp{};
    rp.width = SCR_W; rp.height = SCR_H; rp.scenesPerFrame = 1; rp.multisampleMode = SCE_GXM_MULTISAMPLE_NONE; rp.driverMemBlock = -1;
    if ((e = sceGxmCreateRenderTarget(&rp, &rt)) < 0) { std::printf("[gxm] sceGxmCreateRenderTarget %08x\n", e); return false; }
    for (int i = 0; i < BUFS; ++i) {
        fb_mem[i] = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 4 * SCR_W * SCR_H, SCE_GXM_MEMORY_ATTRIB_RW);
        if (!fb_mem[i]) { std::printf("[gxm] framebuffer alloc failed\n"); return false; }
        std::memset(fb_mem[i], 0, 4 * SCR_W * SCR_H);
        sceGxmColorSurfaceInit(&surf[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8, SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                               SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, SCR_W, SCR_H, SCR_W, fb_mem[i]);
        sceGxmSyncObjectCreate(&sync_obj[i]);
    }
    uint32_t dw = align(SCR_W, SCE_GXM_TILE_SIZEX), dh = align(SCR_H, SCE_GXM_TILE_SIZEY);
    void* dmem = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * dw * dh, SCE_GXM_MEMORY_ATTRIB_RW);
    sceGxmDepthStencilSurfaceInit(&depth, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32, SCE_GXM_DEPTH_STENCIL_SURFACE_TILED, dw, dmem, nullptr);

    const unsigned PB = 64 * 1024, VU = 64 * 1024, FU = 64 * 1024;
    void* pbuf = gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, PB, SCE_GXM_MEMORY_ATTRIB_READ);
    unsigned int vu_off, fu_off;
    void* vu = usse_alloc(VU, false, &vu_off); void* fu = usse_alloc(FU, true, &fu_off);
    SceGxmShaderPatcherParams pp{};
    pp.hostAllocCallback = patcher_host_alloc; pp.hostFreeCallback = patcher_host_free;
    pp.bufferMem = pbuf; pp.bufferMemSize = PB;
    pp.vertexUsseMem = vu; pp.vertexUsseMemSize = VU; pp.vertexUsseOffset = vu_off;
    pp.fragmentUsseMem = fu; pp.fragmentUsseMemSize = FU; pp.fragmentUsseOffset = fu_off;
    if ((e = sceGxmShaderPatcherCreate(&pp, &patcher)) < 0) { std::printf("[gxm] patcher %08x\n", e); return false; }

    vs_gxp = load_or_compile(VS_SRC, false);
    if (!vs_gxp) { std::printf("[gxm] shaders unavailable\n"); return false; }
    sceGxmShaderPatcherRegisterProgram(patcher, vs_gxp, &vs_id);
    auto P = [](const SceGxmProgram* g, const char* n) { return sceGxmProgramFindParameterByName(g, n); };
    SceGxmVertexAttribute at[4]{};
    const char* names[4] = {"a_pos", "a_uv", "a_base", "a_offs"};
    const uint16_t offs[4] = {0, 12, 20, 24};
    const uint8_t fmts[4] = {SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_U8N, SCE_GXM_ATTRIBUTE_FORMAT_U8N};
    const uint8_t comps[4] = {3, 2, 4, 4};
    for (int i = 0; i < 4; ++i) {
        at[i].streamIndex = 0; at[i].offset = offs[i]; at[i].format = fmts[i]; at[i].componentCount = comps[i];
        at[i].regIndex = sceGxmProgramParameterGetResourceIndex(P(vs_gxp, names[i]));
    }
    SceGxmVertexStream st{}; st.stride = sizeof(Vertex); st.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if ((e = sceGxmShaderPatcherCreateVertexProgram(patcher, vs_id, at, 4, &st, 1, &vprog)) < 0) { std::printf("[gxm] vertex program %08x\n", e); return false; }
    p_scale = P(vs_gxp, "u_scale");

    indices = (uint16_t*)gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 65536 * 2, SCE_GXM_MEMORY_ATTRIB_READ);
    for (int i = 0; i < 65536; ++i) indices[i] = (uint16_t)i;
    vbuf = (uint8_t*)gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, VBUF_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
    ibuf = (uint16_t*)gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, IBUF_SIZE, SCE_GXM_MEMORY_ATTRIB_READ);
    tex_heap = (uint8_t*)gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, tex_heap_size, SCE_GXM_MEMORY_ATTRIB_READ);
    if (!tex_heap) tex_heap = (uint8_t*)gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, tex_heap_size, SCE_GXM_MEMORY_ATTRIB_READ);
    fog_mem = (uint32_t*)gpu_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4096, SCE_GXM_MEMORY_ATTRIB_READ);
    sceGxmTextureInitLinear(&fog_tex, fog_mem, SCE_GXM_TEXTURE_FORMAT_A8R8G8B8, 128, 1, 0);
    sceGxmTextureSetMinFilter(&fog_tex, SCE_GXM_TEXTURE_FILTER_POINT); sceGxmTextureSetMagFilter(&fog_tex, SCE_GXM_TEXTURE_FILTER_POINT);
    if (FILE* f = std::fopen("ux0:data/dolphinblue/shaderbank", "rb")) { std::fclose(f); build_shader_bank(); }
    if (FILE* f = std::fopen("ux0:data/dolphinblue/nocap", "rb")) { std::fclose(f); fps_cap = false; std::printf("[gxm] nocap: 30 fps cap off\n"); }
    std::printf("[gxm] init ok (texture heap %u MB)\n", (unsigned)(tex_heap_size >> 20));
    if (!(vbuf && tex_heap && indices)) std::printf("[gxm] buffer alloc failed vbuf %p tex %p idx %p\n", vbuf, tex_heap, indices);
    return vbuf && ibuf && tex_heap && indices;
}

// fmt: the PVR source format (0 ARGB1555, 1 RGB565, 2 ARGB4444): those are stored natively at 16 bits (GXM's
// layouts match the PVR's, and the 8-bit expansion is exact, so this is lossless); anything else is ARGB8888.
static bool tex_pressure;   // the heap ran out: the texture cache is flushed at the start of the next frame
bool backend_texture_pressure() { bool p = tex_pressure; tex_pressure = false; return p; }
void (*backend_flush_barrier)();   // platform: waits until no frame is pending for submission and the GPU is idle
void backend_texture_flush() {   // drop every texture (once nothing in flight can read them)
    if (backend_flush_barrier) backend_flush_barrier();
    TexLock lk;
    textures.resize(1); tex_free.clear(); tex_pending.clear(); tex_heap_used = 0;
}
bool backend_tex32() {   // ux0:data/dolphinblue/tex32: keep every texture at 32 bits (A/B switch for the 16-bit paths)
    static int tex32 = -1;
    if (tex32 < 0) { FILE* f = std::fopen("ux0:data/dolphinblue/tex32", "rb"); tex32 = f != nullptr; if (f) { std::fclose(f); std::printf("[gxm] tex32: 16-bit textures off\n"); } }
    return tex32;
}
uint32_t backend_upload_texture16(uint32_t id, int w, int h, const uint16_t* texels, int fmt) {
    static const SceGxmTextureFormat kFmt16[3] = {SCE_GXM_TEXTURE_FORMAT_U1U5U5U5_ARGB, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB, SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_ARGB};
    SceGxmTextureFormat gf = kFmt16[fmt];
    size_t n = (size_t)w * h * 2;
    TexLock lk;   // the GPU may be drawing the previous frame: never write into memory it may read (new memory, old retired)
    void* mem = tex_alloc(n);
    if (!mem) { if (!tex_pressure) std::printf("[gxm] texture heap full: flushing the texture cache next frame\n"); tex_pressure = true; return 0; }
    std::memcpy(mem, texels, n);
    Tex t{}; t.mem = mem; t.size = n; t.w = w; t.h = h; t.fmt = gf;
    sceGxmTextureInitLinear(&t.t, mem, gf, w, h, 0);
    if (id && id < textures.size()) { tex_retire(textures[id].size, textures[id].mem); textures[id] = t; return id; }
    textures.push_back(t);
    return (uint32_t)textures.size() - 1;
}
uint32_t backend_upload_texture(uint32_t id, int w, int h, const uint32_t* argb, int fmt) {
    static const SceGxmTextureFormat kFmt16[3] = {SCE_GXM_TEXTURE_FORMAT_U1U5U5U5_ARGB, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB, SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_ARGB};
    bool b16 = !backend_tex32() && fmt >= 0 && fmt <= 2;
    size_t npx = (size_t)w * h, n = npx * (b16 ? 2 : 4);
    SceGxmTextureFormat gf = b16 ? kFmt16[fmt] : SCE_GXM_TEXTURE_FORMAT_A8R8G8B8;
    auto fill = [&](void* dst) {
        if (!b16) { std::memcpy(dst, argb, n); return; }
        uint16_t* o = (uint16_t*)dst;
        for (size_t i = 0; i < npx; ++i) {
            uint32_t c = argb[i], a = c >> 24, r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            o[i] = fmt == 0 ? (uint16_t)((a ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3))
                 : fmt == 1 ? (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))
                            : (uint16_t)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
        }
    };
    TexLock lk;   // see backend_upload_texture16
    void* mem = tex_alloc(n);
    if (!mem) { if (!tex_pressure) std::printf("[gxm] texture heap full: flushing the texture cache next frame\n"); tex_pressure = true; return 0; }
    fill(mem);
    Tex t{}; t.mem = mem; t.size = n; t.w = w; t.h = h; t.fmt = gf;
    sceGxmTextureInitLinear(&t.t, mem, gf, w, h, 0);
    if (id && id < textures.size()) { tex_retire(textures[id].size, textures[id].mem); textures[id] = t; return id; }
    textures.push_back(t);
    return (uint32_t)textures.size() - 1;
}

// variant key from the batch state, normalised so states that render identically share one program
static uint32_t variant_key(const ShaderKey& k) {
    uint32_t tex = k.textured ? 1 : 0, fog = k.fog & 3;
    uint32_t offs = (tex || fog == 1) && k.offset ? 1 : 0;
    uint32_t shade = tex ? k.shading & 3 : 0, ign = tex && k.ignore_tex_alpha ? 1 : 0;
    return tex | shade << 1 | offs << 3 | ign << 4 | (k.use_alpha ? 1u : 0u) << 5 | fog << 6 | (k.alpha_test ? 1u : 0u) << 8;
}
static FVariant* fvariant(uint32_t v) {
    auto it = fvariants.find(v);
    if (it != fvariants.end()) return &it->second;
    char defs[160];
    std::snprintf(defs, sizeof defs, "#define TEX %u\n#define SHADE %u\n#define OFFS %u\n#define IGN_TA %u\n#define USE_ALPHA %u\n#define FOG %u\n#define ATEST %u\n",
                  v & 1, (v >> 1) & 3, (v >> 3) & 1, (v >> 4) & 1, (v >> 5) & 1, (v >> 6) & 3, (v >> 8) & 1);
    std::string src = std::string(defs) + FS_SRC;
    FVariant f{};
    f.gxp = load_or_compile(src.c_str(), true);
    if (f.gxp) {
        sceGxmShaderPatcherRegisterProgram(patcher, f.gxp, &f.id);
        auto P = [&](const char* n) { return sceGxmProgramFindParameterByName(f.gxp, n); };
        f.aref = P("u_alpha_ref"); f.fogd = P("u_fog_density"); f.fogram = P("u_fog_ram"); f.fogvert = P("u_fog_vert");
    }
    return &(fvariants[v] = f);
}
static SceGxmFragmentProgram* fprog(FVariant* fv, uint32_t v, bool blend, int src, int dst) {
    uint32_t key = v << 9 | (blend ? (1u << 8) | (src << 4) | dst : 0);
    auto it = fprogs.find(key);
    if (it != fprogs.end()) return it->second;
    std::printf("[gxm] first use: variant %03x blend %d src %d dst %d (frame %llu)\n", v, blend, src, dst, (unsigned long long)frames_presented_dbg);
    static const SceGxmBlendFactor sf[8] = {SCE_GXM_BLEND_FACTOR_ZERO, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_DST_COLOR,
        SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR, SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        SCE_GXM_BLEND_FACTOR_DST_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
    static const SceGxmBlendFactor df[8] = {SCE_GXM_BLEND_FACTOR_ZERO, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_SRC_COLOR,
        SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        SCE_GXM_BLEND_FACTOR_DST_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
    SceGxmBlendInfo bi{};
    bi.colorMask = SCE_GXM_COLOR_MASK_ALL;
    bi.colorFunc = SCE_GXM_BLEND_FUNC_ADD; bi.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    bi.colorSrc = sf[src & 7]; bi.colorDst = df[dst & 7]; bi.alphaSrc = sf[src & 7]; bi.alphaDst = df[dst & 7];
    SceGxmFragmentProgram* p = nullptr;
    sceGxmShaderPatcherCreateFragmentProgram(patcher, fv->id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                                             blend ? &bi : nullptr, vs_gxp, &p);
    fprogs[key] = p;
    return p;
}

static const SceGxmDepthFunc dfunc[8] = {SCE_GXM_DEPTH_FUNC_NEVER, SCE_GXM_DEPTH_FUNC_LESS, SCE_GXM_DEPTH_FUNC_EQUAL,
    SCE_GXM_DEPTH_FUNC_LESS_EQUAL, SCE_GXM_DEPTH_FUNC_GREATER, SCE_GXM_DEPTH_FUNC_NOT_EQUAL, SCE_GXM_DEPTH_FUNC_GREATER_EQUAL,
    SCE_GXM_DEPTH_FUNC_ALWAYS};

static void set_uniforms(float sx, float sy, float ax, float ay, const FVariant* fv, const DrawList& dl, float aref = -1) {
    void* vub; sceGxmReserveVertexDefaultUniformBuffer(ctx, &vub);
    float sc[4] = {sx, sy, ax, ay}; sceGxmSetUniformDataF(vub, p_scale, 0, 4, sc);
    if (!(fv->aref || fv->fogd || fv->fogram || fv->fogvert)) return;
    void* fub; sceGxmReserveFragmentDefaultUniformBuffer(ctx, &fub);
    auto rgb = [](uint32_t c, float* f) { f[0] = ((c >> 16) & 255) / 255.f; f[1] = ((c >> 8) & 255) / 255.f; f[2] = (c & 255) / 255.f; };
    if (fv->aref) { float a = aref >= 0 ? aref : dl.alpha_ref; sceGxmSetUniformDataF(fub, fv->aref, 0, 1, &a); }
    if (fv->fogd) sceGxmSetUniformDataF(fub, fv->fogd, 0, 1, &dl.fog_density);
    float f3[3];
    if (fv->fogram) { rgb(dl.fog_col_ram, f3); sceGxmSetUniformDataF(fub, fv->fogram, 0, 3, f3); }
    if (fv->fogvert) { rgb(dl.fog_col_vert, f3); sceGxmSetUniformDataF(fub, fv->fogvert, 0, 3, f3); }
}

// ---- host UI overlay (mission select) ------------------------------------------------------
struct OverlayQuad { float x0, y0, x1, y1; uint32_t col; };

static int font_idx(char c) {
    if (c == ' ') return 0;
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    switch (c) {
        case 'A': return 11; case 'B': return 12; case 'C': return 13; case 'D': return 14;
        case 'E': return 15; case 'I': return 16; case 'K': return 17; case 'L': return 18;
        case 'M': return 19; case 'N': return 20; case 'O': return 21; case 'S': return 22;
        case 'T': return 23; case 'X': return 24;
    }
    return 0;
}
static const uint8_t kFont[25][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, // 2
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, // A
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, // B
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, // C
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, // D
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, // E
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, // I
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, // K
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, // L
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, // M
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, // N
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, // O
    {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}, // S
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, // T
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, // X
};

// Build the mission-select menu as solid-colour quads in 640x480 game space (the pillarbox scaling
// applied by set_uniforms maps them onto the 960x544 screen). Returns the number of quads written.
static int build_menu_overlay(OverlayQuad* out, int maxq) {
    int n = 0;
    auto push = [&](OverlayQuad q) { if (n < maxq) out[n++] = q; };
    auto text = [&](const char* s, float x, float y, float sc, uint32_t col) {
        for (const char* p = s; *p; ++p) {
            const uint8_t* g = kFont[font_idx(*p)];
            for (int r = 0; r < 7; ++r) for (int c = 0; c < 5; ++c)
                if (g[r] & (1 << (4 - c))) push({x + c*sc, y + r*sc, x + (c+1)*sc, y + (r+1)*sc, col});
            x += 6 * sc;
        }
    };
    push({0, 0, 640, 480, 0xFF101018});   // opaque backdrop
    const char* title = "MISSION SELECT";
    float ts = 3.0f, tw = 6 * ts * (float)std::strlen(title);
    text(title, (640 - tw) / 2, 52, ts, 0xFF20E0E0);
    uint32_t count = ui_count; if (count > 16) count = 16; if (count < 1) count = 1;
    float top = 118, bottom = 414;
    float lh = (bottom - top) / count; if (lh > 44) lh = 44;
    float ls = lh / 8.0f; if (ls > 5.0f) ls = 5.0f;
    for (uint32_t i = 0; i < count; ++i) {
        float y = top + i * lh;
        char buf[8]; std::snprintf(buf, sizeof buf, "%u", (unsigned)ui_slot[i]);
        float w = 6 * ls * (float)std::strlen(buf);
        if (i == ui_cursor) {
            push({220, y + 2, 420, y + lh - 2, 0xFFE0C020});   // amber highlight bar
            text(buf, (640 - w) / 2, y + (lh - 7*ls) / 2, ls, 0xFF101018);
        } else {
            text(buf, (640 - w) / 2, y + (lh - 7*ls) / 2, ls, 0xFFB0B0B8);
        }
    }
    const char* hint = "X LOAD   O BACK";
    float hs = 2.0f, hw = 6 * hs * (float)std::strlen(hint);
    text(hint, (640 - hw) / 2, 436, hs, 0xFF90A0A0);
    return n;
}

void backend_render(const DrawList& dl, std::vector<uint32_t>&) {
    // vertices: full-screen black quad, the PVR background plane, the frame's triangles, then the host UI overlay
    static OverlayQuad oq[512];
    int nq = ui_menu ? build_menu_overlay(oq, 512) : 0;
    size_t total = dl.verts.size() + 12 + (size_t)nq * 6;
    if (total * sizeof(Vertex) > VBUF_SIZE) total = VBUF_SIZE / sizeof(Vertex);
    Vertex* vb = (Vertex*)vbuf;
    auto quad = [&](Vertex* q, float x0, float y0, float x1, float y1, float z, uint32_t col) {
        Vertex a{x0, y0, z, 0, 0, col, 0}, b{x1, y0, z, 0, 0, col, 0}, c{x1, y1, z, 0, 0, col, 0}, d{x0, y1, z, 0, 0, col, 0};
        q[0] = a; q[1] = b; q[2] = c; q[3] = a; q[4] = c; q[5] = d;
    };
    quad(vb, 0, 0, (float)dl.width, (float)dl.height, 1e-7f, 0xFF000000u);
    quad(vb + 6, 0, 0, (float)dl.width, (float)dl.height, dl.clear_depth > 0 ? dl.clear_depth : 1e-7f, dl.clear_color);
    size_t game_verts = dl.verts.size();
    if (game_verts > (total > 12 ? total - 12 : 0)) game_verts = total > 12 ? total - 12 : 0;
    std::memcpy(vb + 12, dl.verts.data(), game_verts * sizeof(Vertex));
    Vertex* ovb = vb + 12 + game_verts;
    int nqdrawn = 0;
    if (nq && 12 + game_verts + (size_t)nq * 6 <= total) {
        nqdrawn = nq;
        for (int q = 0; q < nq; ++q) quad(ovb + q * 6, oq[q].x0, oq[q].y0, oq[q].x1, oq[q].y1, 1e-7f, oq[q].col);
    }
    size_t itotal = dl.idx.size() < IBUF_SIZE / 2 ? dl.idx.size() : IBUF_SIZE / 2;
    std::memcpy(ibuf, dl.idx.data(), itotal * 2);
    for (int i = 0; i < 128; ++i) fog_mem[i] = 0xFF000000u | (uint32_t)dl.fog_hi[i] << 16 | (uint32_t)dl.fog_lo[i] << 8;

    render_stage = 10;
    sceGxmBeginScene(ctx, 0, rt, nullptr, nullptr, sync_obj[back], &surf[back], &depth);
    render_stage = 11;
    sceGxmSetVertexProgram(ctx, vprog);
    sceGxmSetCullMode(ctx, SCE_GXM_CULL_NONE);
    float sx = 2.0f / dl.width, sy = 2.0f / dl.height;
    // full screen by default; the file ux0:data/dolphinblue/aspect43 switches to the 4:3 pillarbox
    static const bool pillar = [] { SceIoStat st; return sceIoGetstat("ux0:data/dolphinblue/aspect43", &st) >= 0; }();
    float ax = pillar ? (SCR_H * 4.0f / 3.0f) / SCR_W : 1.0f, ay = 1.0f;
    ShaderKey clear_key{}; clear_key.use_alpha = true; clear_key.fog = 2;
    uint32_t cv = variant_key(clear_key); FVariant* cfv = fvariant(cv);
    sceGxmSetFragmentProgram(ctx, fprog(cfv, cv, false, 1, 0));
    sceGxmSetFrontDepthFunc(ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(ctx, SCE_GXM_DEPTH_WRITE_ENABLED);
    sceGxmSetFragmentTexture(ctx, 1, &fog_tex);
    set_uniforms(sx, sy, 1.0f, 1.0f, cfv, dl);
    sceGxmSetVertexStream(ctx, 0, vb);
    sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, 6);
    set_uniforms(sx, sy, ax, ay, cfv, dl);
    sceGxmSetVertexStream(ctx, 0, vb + 6);
    sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, 6);
    static const int order[3] = {L_OPAQUE, L_PUNCH, L_TRANS};
    for (int li : order) {
        for (const Batch& b : dl.lists[li]) {
            if (!b.count || b.first + b.count > itotal || 12 + b.vbase >= total) continue;
            ShaderKey k = b.key;
            { TexLock lk; if (k.textured && (!b.tex_id || b.tex_id >= textures.size())) k.textured = false; }
            uint32_t v = variant_key(k); FVariant* fv = fvariant(v);
            if (!fv->gxp) continue;
            sceGxmSetFragmentProgram(ctx, fprog(fv, v, li == L_TRANS, b.src_blend, b.dst_blend));
            sceGxmSetFrontDepthFunc(ctx, dfunc[b.depth_func & 7]);
            sceGxmSetFrontDepthWriteEnable(ctx, b.depth_write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED);
            if (k.textured) {
                {
                    SceGxmTexture t; { TexLock lk; t = textures[b.tex_id].t; }
                    auto am = [](bool clamp, bool flip) { return clamp ? SCE_GXM_TEXTURE_ADDR_CLAMP : flip ? SCE_GXM_TEXTURE_ADDR_MIRROR : SCE_GXM_TEXTURE_ADDR_REPEAT; };
                    sceGxmTextureSetUAddrMode(&t, am(b.clamp_u, b.flip_u));
                    sceGxmTextureSetVAddrMode(&t, am(b.clamp_v, b.flip_v));
                    SceGxmTextureFilter f = b.bilinear ? SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
                    sceGxmTextureSetMinFilter(&t, f); sceGxmTextureSetMagFilter(&t, f);
                    sceGxmSetFragmentTexture(ctx, 0, &t);
                }
            }
            set_uniforms(sx, sy, ax, ay, fv, dl, b.aref);
            sceGxmSetVertexStream(ctx, 0, vb + 12 + b.vbase);
            for (uint32_t done = 0; done < b.count;) {
                uint32_t n = b.count - done; if (n > 65532) n = 65532;
                sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, ibuf + b.first + done, n);
                done += n;
            }
        }
    }
    if (nqdrawn) {
        // host UI overlay: opaque solid-colour quads (menu backdrop, glyphs and highlight bar)
        sceGxmSetFragmentProgram(ctx, fprog(cfv, cv, false, 1, 0));
        sceGxmSetFrontDepthFunc(ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
        sceGxmSetFrontDepthWriteEnable(ctx, SCE_GXM_DEPTH_WRITE_ENABLED);
        set_uniforms(sx, sy, ax, ay, cfv, dl);
        for (int q = 0; q < nqdrawn; ++q) {
            sceGxmSetVertexStream(ctx, 0, ovb + q * 6);
            sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16, indices, 6);
        }
    }
    render_stage = 12;
    { TexLock lk; ++submit_serial; }   // textures retired from here on may be used by this frame
    sceGxmEndScene(ctx, nullptr, nullptr);
    render_stage = 13;
    sceGxmPadHeartbeat(&surf[back], sync_obj[back]);
    DisplayData d{fb_mem[back]};
    render_stage = 14;
    sceGxmDisplayQueueAddEntry(sync_obj[front], sync_obj[back], &d);
    render_stage = 15;
    front = back; back = (back + 1) % BUFS;
    gpu_busy = true;   // the GPU now runs while the guest produces the next frame
}

// Wait for the previous frame's GPU work. Called before the next frame converts textures or writes vertices,
// since both reuse memory the previous frame may still read.
void backend_wait_gpu() {
    if (!gpu_busy) return;
    uint64_t t = sceKernelGetProcessTimeWide();
    render_stage = 16;
    sceGxmFinish(ctx);
    render_stage = 17;
    {   // every submitted frame is done: memory retired up to now can be reused
        TexLock lk;
        size_t k = 0;
        for (auto& e : tex_pending) { if (e.first <= submit_serial) tex_free.insert(e.second); else tex_pending[k++] = e; }
        tex_pending.resize(k);
    }
    prof_gpu_us += sceKernelGetProcessTimeWide() - t;
    gpu_busy = false;
}

// test support: raw dump (960x544 RGBA) of the most recently presented frame
bool backend_dump_last(const char* path) {
    backend_wait_gpu();
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(fb_mem[front], 4, SCR_W * SCR_H, f);
    std::fclose(f);
    return true;
}

} // namespace render

namespace render {
// One-off hardware check of GXM's 16-bit texture layouts (ux0:data/dolphinblue/fmttest): each candidate format gets a
// 4x1 texture with known texels, drawn alpha-blended over white; the frame is dumped for analysis.
void backend_format_test() {
    struct Cand { SceGxmTextureFormat f; uint16_t t[4]; };
    // PVR-layout texels: 1555 {opaque black, transparent blue-lsb, opaque red, opaque blue}; 4444 likewise; 565 {red, green, blue, black}
    const Cand c[] = {
        {SCE_GXM_TEXTURE_FORMAT_U1U5U5U5_ARGB, {0x8000, 0x0001, 0xFC00, 0x801F}}, {SCE_GXM_TEXTURE_FORMAT_U1U5U5U5_ABGR, {0x8000, 0x0001, 0xFC00, 0x801F}},
        {SCE_GXM_TEXTURE_FORMAT_U5U5U5U1_RGBA, {0x8000, 0x0001, 0xFC00, 0x801F}}, {SCE_GXM_TEXTURE_FORMAT_U5U5U5U1_BGRA, {0x8000, 0x0001, 0xFC00, 0x801F}},
        {SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_ARGB, {0xF000, 0x000F, 0xFF00, 0xF00F}}, {SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_ABGR, {0xF000, 0x000F, 0xFF00, 0xF00F}},
        {SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_RGBA, {0xF000, 0x000F, 0xFF00, 0xF00F}}, {SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_BGRA, {0xF000, 0x000F, 0xFF00, 0xF00F}},
        {SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB, {0xF800, 0x07E0, 0x001F, 0x0000}}, {SCE_GXM_TEXTURE_FORMAT_U5U6U5_BGR, {0xF800, 0x07E0, 0x001F, 0x0000}},
    };
    DrawList dl; dl.width = 640; dl.height = 480; dl.clear_color = 0xFFFFFFFFu; dl.clear_depth = 0;
    for (int i = 0; i < 10; ++i) {
        void* mem = tex_alloc(4 * 2); std::memcpy(mem, c[i].t, 8);
        Tex t{}; t.mem = mem; t.size = 8; t.w = 4; t.h = 1; t.fmt = c[i].f;
        sceGxmTextureInitLinear(&t.t, mem, c[i].f, 4, 1, 0);
        textures.push_back(t);
        Batch b; b.key.textured = 1; b.key.use_alpha = 1; b.key.shading = 0; b.key.fog = 2; b.fog = 2;
        b.tex_id = (uint32_t)textures.size() - 1; b.src_blend = 4; b.dst_blend = 5; b.depth_func = 7; b.depth_write = false;
        b.first = (uint32_t)dl.idx.size(); b.vbase = (uint32_t)dl.verts.size(); b.count = 6;
        float x0 = 20 + (i % 5) * 120, y0 = 40 + (i / 5) * 200, x1 = x0 + 100, y1 = y0 + 100;
        Vertex v[4] = {{x0, y0, 1, 0, 0, 0xFFFFFFFFu, 0}, {x1, y0, 1, 1, 0, 0xFFFFFFFFu, 0}, {x1, y1, 1, 1, 1, 0xFFFFFFFFu, 0}, {x0, y1, 1, 0, 1, 0xFFFFFFFFu, 0}};
        for (auto& q : v) dl.verts.push_back(q);
        for (uint16_t k : {0, 1, 2, 0, 2, 3}) dl.idx.push_back(k);
        dl.lists[L_TRANS].push_back(b);
    }
    std::vector<uint32_t> dummy;
    for (int k = 0; k < 3; ++k) { backend_wait_gpu(); backend_render(dl, dummy); }
    backend_wait_gpu();
    backend_dump_last("ux0:data/dolphinblue/fmttest.rgba");
    std::printf("[gxm] format test written to fmttest.rgba\n");
}
}
