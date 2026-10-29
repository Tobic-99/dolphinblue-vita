// Backend-agnostic draw lists produced from PowerVR2 TA data.
// The producer (ta_decode.cpp) turns one frame's TA stream into DrawLists; a backend
// (gl_backend.cpp on the desktop, libgxm on the Vita in M4) consumes them. All PVR state is
// reduced to a small ShaderKey (selects one of a few fixed shader programs) plus blend/depth state.
#pragma once
#include <cstdint>
#include <vector>

namespace render {

struct Vertex {
    float x, y, invw;      // screen-space pixels and 1/w (the TA's z)
    float u, v;
    uint32_t base;         // ARGB8888
    uint32_t offs;         // ARGB8888 (offset/specular colour)
};

// Fixed-function state of the PVR2 TSP reduced to a shader key (a handful of programs).
struct ShaderKey {
    uint8_t textured : 1, offset : 1, alpha_test : 1, ignore_tex_alpha : 1, use_alpha : 1;
    uint8_t shading : 2;   // 0 decal, 1 modulate, 2 decal-alpha, 3 modulate-alpha
    uint8_t pad : 1;
    uint8_t fog = 2;       // 0 table, 1 vertex (offset alpha), 2 none, 3 table mode 2
    uint32_t bits() const { return textured | offset << 1 | alpha_test << 2 | ignore_tex_alpha << 3 | use_alpha << 4 | shading << 5 | (uint32_t)fog << 7; }
};

struct Batch {
    ShaderKey key{};
    uint32_t tex_id = 0;       // texture cache handle (0 = none)
    uint8_t depth_func = 7;    // PVR ISP compare mode: 0 never,1 less,2 equal,3 lequal,4 greater,5 notequal,6 gequal,7 always
    bool depth_write = true;
    uint8_t cull = 0;          // PVR culling mode (0 none, 1 small, 2 ccw, 3 cw) - not applied in M2
    uint8_t src_blend = 1, dst_blend = 0;   // PVR SRC/DST_ALPHA_INSTR codes
    bool clamp_u = false, clamp_v = false, flip_u = false, flip_v = false, bilinear = false;
    uint32_t first = 0, count = 0;          // triangle list: index range in DrawList::idx
    uint32_t vbase = 0;                      // the batch's first vertex; its indices are relative to it (16-bit)
    float aref = -1;                         // alpha-test reference for this batch (-1: the frame's PT_ALPHA_REF)
    float sort_z = 0;                        // translucent sort key (mean 1/w)
    uint8_t fog = 2;                         // TSP fog control: 0 table, 1 vertex (offset alpha), 2 none, 3 table mode 2
    uint32_t tsp = 0, isp = 0, tcw = 0, pcw = 0; // raw words (diagnostics)
};

enum ListType { L_OPAQUE = 0, L_OPAQUE_MOD = 1, L_TRANS = 2, L_TRANS_MOD = 3, L_PUNCH = 4, L_COUNT = 5 };

struct DrawList {
    int width = 640, height = 480;
    uint32_t clear_color = 0xFF000000;
    float clear_depth = 0;
    float alpha_ref = 0;                     // punch-through reference (0..1)
    bool presort = false;                    // translucent list already sorted by the game
    std::vector<Vertex> verts;               // each strip vertex once
    std::vector<uint16_t> idx;               // triangles, relative to their batch's vbase
    std::vector<Batch> lists[L_COUNT];
    uint32_t skipped_modvol = 0, skipped_params = 0, culled_tris = 0;
    uint32_t fog_col_vert = 0, fog_col_ram = 0;    // RGB
    float fog_density = 0;                         // FOG_DENSITY as a float (mantissa * 2^exp)
    uint8_t fog_hi[128] = {}, fog_lo[128] = {};    // FOG_TABLE entries: value at interval start / end
};

// vertex k (0..count-1) of a batch's triangle list
inline const Vertex& batch_vertex(const DrawList& dl, const Batch& b, uint32_t k) { return dl.verts[b.vbase + dl.idx[b.first + k]]; }

// --- texture conversion (pvr_tex.cpp) ---
struct TexInfo { uint32_t id; int w, h; };
uint32_t texture_get(uint32_t tcw, uint32_t tsp, int* w, int* h);   // converts on first use / after VRAM change
void texture_frame_begin();
struct TexStats { uint64_t conversions, hits, live; uint64_t convert_us; };   // convert_us: CPU time spent converting/uploading texels
TexStats texture_stats();
void texture_set_regs(const uint32_t* regs);   // PVR register snapshot used for palettes/stride

// --- producer (ta_decode.cpp) ---
void build_drawlist(const uint32_t* ta, size_t n, const uint32_t* pvr_regs, DrawList& out);
inline void build_drawlist(const std::vector<uint32_t>& ta, const uint32_t* pvr_regs, DrawList& out) { build_drawlist(ta.data(), ta.size(), pvr_regs, out); }

// --- backend (gl_backend.cpp) ---
bool backend_init(int w, int h);
uint32_t backend_upload_texture(uint32_t old_id, int w, int h, const uint32_t* rgba, int pvr_fmt);   // returns backend id
bool backend_tex32();
uint32_t backend_upload_texture16(uint32_t old_id, int w, int h, const uint16_t* texels, int gxm16_fmt);   // Vita: 0 ARGB1555, 1 RGB565, 2 ARGB4444
bool backend_texture_pressure();   // texture memory ran out since the last call
void backend_texture_flush();      // drop all textures (GPU idle)
void backend_render(const DrawList& dl, std::vector<uint32_t>& rgba_out);

bool write_png(const char* path, int w, int h, const uint32_t* rgba);

} // namespace render
