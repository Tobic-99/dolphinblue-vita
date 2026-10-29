// Draw producer: PowerVR2 TA parameter stream -> backend-agnostic DrawList.
// Own implementation from the PVR2/Holly TA documentation (parameter and vertex formats).
#include "draw.h"
#include "../host/hw.h"
#include <cstring>
#include <algorithm>
#include <cmath>

namespace render {

static inline float fbits(uint32_t w) { float f; std::memcpy(&f, &w, 4); return f; }
static inline uint32_t clamp8(float f) { if (!(f > 0)) return 0; if (f >= 1) return 255; return (uint32_t)(f * 255.0f + 0.5f); }
static inline uint32_t argb_f(float a, float r, float g, float b) { return clamp8(a) << 24 | clamp8(r) << 16 | clamp8(g) << 8 | clamp8(b); }
static inline uint32_t scale_rgb(uint32_t argb_face, float i) {   // intensity * face colour, alpha = face alpha
    float r = ((argb_face >> 16) & 255) / 255.f * i, g = ((argb_face >> 8) & 255) / 255.f * i, b = (argb_face & 255) / 255.f * i;
    return (argb_face & 0xFF000000u) | clamp8(r) << 16 | clamp8(g) << 8 | clamp8(b);
}
static inline uint32_t vram32(uint32_t addr) {   // 32-bit path read
    uint32_t v; std::memcpy(&v, hw::vram + hw::vram32_to_64(addr & 0x7FFFFC), 4); return v;
}

struct Decoder {
    DrawList* dl;
    int list = -1;
    uint32_t pcw_obj = 0, isp = 0, tsp = 0, tcw = 0;
    bool sprite = false, modvol = false;
    uint32_t face = 0xFFFFFFFF, face_off = 0;
    uint32_t sprite_base = 0, sprite_off = 0;
    uint32_t sv[2]; bool sok[2], sin_[2]; int sn = 0;   // the last two strip vertices (indices in dl->verts), finite, inside the guard band
    void strip_clear() { sn = 0; }
    Batch* cur = nullptr;

    void new_batch() {
        if (list < 0 || list >= L_COUNT || modvol) { cur = nullptr; return; }
        Batch b;
        // the TA takes texture/offset/gouraud/16-bit-UV from the PCW object control, not the ISP word
        bool textured = (pcw_obj >> 3) & 1;
        b.key.textured = textured;
        b.key.offset = (pcw_obj >> 2) & 1;
        b.key.alpha_test = list == L_PUNCH;
        b.key.ignore_tex_alpha = (tsp >> 19) & 1;
        b.key.use_alpha = (tsp >> 20) & 1;
        b.key.shading = (tsp >> 6) & 3;
        b.depth_func = isp >> 29;
        b.depth_write = !((isp >> 26) & 1);
        b.cull = (isp >> 27) & 3;
        b.src_blend = tsp >> 29; b.dst_blend = (tsp >> 26) & 7;
        b.clamp_u = (tsp >> 16) & 1; b.clamp_v = (tsp >> 15) & 1;
        b.flip_u = (tsp >> 18) & 1; b.flip_v = (tsp >> 17) & 1;
        b.bilinear = ((tsp >> 13) & 3) != 0;
        b.fog = (tsp >> 22) & 3;
        b.key.fog = b.fog;
        b.tsp = tsp; b.isp = isp; b.tcw = tcw; b.pcw = pcw_obj;
        if (textured) { int w, h; b.tex_id = texture_get(tcw, tsp, &w, &h); }
        // Translucent polygons with Z write: the PVR sorts translucent pixels, so fully transparent texels have no
        // effect there. Sorted per batch here, they would still write depth and cut a hole into whatever is drawn
        // behind them later (e.g. the water around the dolphin sprites): discard alpha 0 instead.
        if (list == L_TRANS && b.depth_write && textured && !b.key.ignore_tex_alpha) { b.key.alpha_test = 1; b.aref = 1.0f / 255.0f; }
        b.first = (uint32_t)dl->idx.size(); b.vbase = (uint32_t)dl->verts.size();
        dl->lists[list].push_back(b);
        cur = &dl->lists[list].back();
    }
    // 16-bit indices: before appending n vertices, continue in a copy of the batch if they would not fit; the
    // strip window's vertices are carried over (re-appended) so the strip continues seamlessly
    void room(uint32_t n) {
        if (dl->verts.size() + n - cur->vbase <= 65535) return;
        Batch b = *cur; b.first = (uint32_t)dl->idx.size(); b.vbase = (uint32_t)dl->verts.size(); b.count = 0;
        dl->lists[list].push_back(b); cur = &dl->lists[list].back();
        for (int i = 0; i < sn; ++i) { Vertex v = dl->verts[sv[i]]; dl->verts.push_back(v); sv[i] = (uint32_t)dl->verts.size() - 1; }
    }
    uint32_t add(const Vertex& v) { dl->verts.push_back(v); return (uint32_t)dl->verts.size() - 1; }
    void tri_idx(uint32_t a, uint32_t b, uint32_t c) {
        uint32_t base = cur->vbase;
        dl->idx.push_back((uint16_t)(a - base)); dl->idx.push_back((uint16_t)(b - base)); dl->idx.push_back((uint16_t)(c - base));
        cur->count += 3;
    }
    // Guard band: the Vita's tiler stalls for seconds on vertices millions of pixels off-screen, so triangles
    // reaching beyond 2048 px outside the screen are clipped here (screen space, attributes interpolated as
    // attr * 1/w and 1/w, i.e. exactly as the shader's perspective correction sees them) or dropped.
    static constexpr float kGuard = 2048.0f;
    static Vertex lerp_v(const Vertex& a, const Vertex& b, float t) {
        Vertex r;
        r.x = a.x + (b.x - a.x) * t; r.y = a.y + (b.y - a.y) * t;
        r.invw = a.invw + (b.invw - a.invw) * t;
        float ia = a.invw * (1 - t), ib = b.invw * t, q = r.invw != 0 ? 1.0f / r.invw : 0.0f;
        r.u = (a.u * ia + b.u * ib) * q; r.v = (a.v * ia + b.v * ib) * q;
        auto col = [&](uint32_t ca, uint32_t cb) {
            uint32_t o = 0;
            for (int sh = 0; sh < 32; sh += 8) {
                float f = (((ca >> sh) & 255) * ia + ((cb >> sh) & 255) * ib) * q;
                int iv = (int)(f + 0.5f); iv = iv < 0 ? 0 : iv > 255 ? 255 : iv;
                o |= (uint32_t)iv << sh;
            }
            return o;
        };
        r.base = col(a.base, b.base); r.offs = col(a.offs, b.offs);
        return r;
    }
    bool inside(const Vertex& v) const { return v.x >= -kGuard && v.x <= dl->width + kGuard && v.y >= -kGuard && v.y <= dl->height + kGuard; }
    void push_tri(uint32_t ia, uint32_t ib, uint32_t ic, bool all_inside) {   // vertices already in dl->verts
        if (__builtin_expect(all_inside, 1)) {
            if (__builtin_expect(ia < cur->vbase || ib < cur->vbase || ic < cur->vbase, 0)) {   // batch was split
                const Vertex a = dl->verts[ia], b = dl->verts[ib], c = dl->verts[ic];
                room(3); ia = add(a); ib = add(b); ic = add(c);
            }
            tri_idx(ia, ib, ic); return;
        }
        const float x0 = -kGuard, y0 = -kGuard, x1 = dl->width + kGuard, y1 = dl->height + kGuard;
        const Vertex a = dl->verts[ia], b = dl->verts[ib], c = dl->verts[ic];
        // Sutherland-Hodgman against the four guard-band edges, then a fan
        Vertex poly[2][9]; int n = 3, src = 0;
        poly[0][0] = a; poly[0][1] = b; poly[0][2] = c;
        for (int e = 0; e < 4 && n; ++e) {
            auto d = [&](const Vertex& v) { return e == 0 ? v.x - x0 : e == 1 ? x1 - v.x : e == 2 ? v.y - y0 : y1 - v.y; };
            int m = 0; const Vertex* in = poly[src]; Vertex* out = poly[src ^ 1];
            for (int i = 0; i < n; ++i) {
                const Vertex& p = in[i]; const Vertex& q = in[(i + 1) % n];
                float dp = d(p), dq = d(q);
                if (dp >= 0) out[m++] = p;
                if ((dp >= 0) != (dq >= 0)) out[m++] = lerp_v(p, q, dp / (dp - dq));
            }
            n = m; src ^= 1;
        }
        if (n < 3) { dl->culled_tris++; return; }
        room((uint32_t)n);
        uint32_t f = add(poly[src][0]), prev = add(poly[src][1]);
        for (int i = 1; i + 1 < n; ++i) { uint32_t nx = add(poly[src][i + 1]); tri_idx(f, prev, nx); prev = nx; }
    }
    static bool finite(const Vertex& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.invw); }
    void emit_quad(const Vertex& A, const Vertex& B, const Vertex& C, const Vertex& D) {   // sprites: (A,B,C), (A,C,D)
        if (!cur) return;
        bool fa = finite(A), fb = finite(B), fc = finite(C), fd = finite(D);
        room(4);
        uint32_t a = add(A), b = add(B), c = add(C), d = add(D);
        bool ia = inside(A), ib = inside(B), ic = inside(C), id = inside(D);
        if (fa && fb && fc) push_tri(a, b, c, ia && ib && ic); else dl->culled_tris++;
        if (fa && fc && fd) push_tri(a, c, d, ia && ic && id); else dl->culled_tris++;
    }
    void add_vertex(const Vertex& v, bool end_of_strip) {   // strip -> indexed triangle list; each vertex is stored once
        bool ok = finite(v);
        if (!cur) { if (end_of_strip) sn = 0; return; }
        room(1);
        uint32_t iv = add(v);
        bool in = ok && inside(v);
        if (sn >= 2) {
            if (ok && sok[0] && sok[1]) push_tri(sv[0], sv[1], iv, in && sin_[0] && sin_[1]);
            else dl->culled_tris++;
        }
        if (end_of_strip) { sn = 0; return; }
        if (sn < 2) { sv[sn] = iv; sok[sn] = ok; sin_[sn] = in; ++sn; }
        else { sv[0] = sv[1]; sok[0] = sok[1]; sin_[0] = sin_[1]; sv[1] = iv; sok[1] = ok; sin_[1] = in; }
    }

    void header(const uint32_t* w, uint32_t para) {
        uint32_t pcw = w[0];
        if (list < 0) list = (pcw >> 24) & 7;
        modvol = (list == L_OPAQUE_MOD || list == L_TRANS_MOD);
        if (modvol) { strip_clear(); cur = nullptr; dl->skipped_modvol++; return; }
        sprite = para == 5;
        pcw_obj = pcw & 0xFF;
        isp = w[1]; tsp = w[2]; tcw = w[3];
        if (sprite) { sprite_base = w[4]; sprite_off = w[5]; }
        else {
            uint32_t col = (pcw >> 4) & 3;
            if (col == 2) {   // intensity mode 1: face colours follow as floats (mode 2 = col 3 keeps the previous ones)
                bool vol = (pcw >> 6) & 1, off = (pcw >> 2) & 1;
                if (!vol && off) {        // polygon type 2 (64 bytes): face colour at words 8-11, face offset at 12-15
                    face = argb_f(fbits(w[8]), fbits(w[9]), fbits(w[10]), fbits(w[11]));
                    face_off = argb_f(fbits(w[12]), fbits(w[13]), fbits(w[14]), fbits(w[15]));
                } else if (vol) {         // polygon type 4 (64 bytes): face colour 0 at words 8-11
                    face = argb_f(fbits(w[8]), fbits(w[9]), fbits(w[10]), fbits(w[11]));
                    face_off = 0;
                } else {                  // polygon type 1 (32 bytes): face colour at words 4-7
                    face = argb_f(fbits(w[4]), fbits(w[5]), fbits(w[6]), fbits(w[7]));
                    face_off = 0;
                }
            }
        }
        strip_clear();
        new_batch();
    }

    void vertex(const uint32_t* w) {
        if (modvol) return;
        bool eos = (w[0] >> 28) & 1;
        if (sprite) {
            Vertex A{fbits(w[1]), fbits(w[2]), fbits(w[3])}, B{fbits(w[4]), fbits(w[5]), fbits(w[6])},
                   C{fbits(w[7]), fbits(w[8]), fbits(w[9])}, D{fbits(w[10]), fbits(w[11]), 0};
            D.invw = A.invw + C.invw - B.invw;
            auto uv = [](uint32_t x, float& u, float& v) { u = fbits(x & 0xFFFF0000u); v = fbits(x << 16); };
            uv(w[13], A.u, A.v); uv(w[14], B.u, B.v); uv(w[15], C.u, C.v);
            D.u = A.u + C.u - B.u; D.v = A.v + C.v - B.v;
            for (Vertex* p : {&A, &B, &C, &D}) { p->base = sprite_base; p->offs = sprite_off; }
            emit_quad(A, B, C, D);
            return;
        }
        Vertex v{fbits(w[1]), fbits(w[2]), fbits(w[3]), 0, 0, 0xFFFFFFFF, 0};
        uint32_t col = (pcw_obj >> 4) & 3; bool tex = (pcw_obj >> 3) & 1, vol = (pcw_obj >> 6) & 1, uv16 = pcw_obj & 1;
        auto uv16f = [&](uint32_t x) { v.u = fbits(x & 0xFFFF0000u); v.v = fbits(x << 16); };
        if (!vol) {
            if (!tex) {
                if (col == 0) v.base = w[6];
                else if (col == 1) v.base = argb_f(fbits(w[4]), fbits(w[5]), fbits(w[6]), fbits(w[7]));
                else v.base = scale_rgb(face, fbits(w[6]));
            } else {
                if (col == 0) { if (uv16) uv16f(w[4]); else { v.u = fbits(w[4]); v.v = fbits(w[5]); } v.base = w[6]; v.offs = w[7]; }
                else if (col == 1) {
                    if (uv16) uv16f(w[4]); else { v.u = fbits(w[4]); v.v = fbits(w[5]); }
                    v.base = argb_f(fbits(w[8]), fbits(w[9]), fbits(w[10]), fbits(w[11]));
                    v.offs = argb_f(fbits(w[12]), fbits(w[13]), fbits(w[14]), fbits(w[15]));
                } else {
                    if (uv16) uv16f(w[4]); else { v.u = fbits(w[4]); v.v = fbits(w[5]); }
                    v.base = scale_rgb(face, fbits(w[6])); v.offs = scale_rgb(face_off, fbits(w[7]));
                }
            }
        } else {   // two-volume formats: use volume 0
            if (!tex) v.base = (col == 0) ? w[4] : scale_rgb(face, fbits(w[4]));
            else {
                if (uv16) uv16f(w[4]); else { v.u = fbits(w[4]); v.v = fbits(w[5]); }
                if (col == 0) { v.base = w[6]; v.offs = w[7]; }
                else { v.base = scale_rgb(face, fbits(w[6])); v.offs = scale_rgb(face_off, fbits(w[7])); }
            }
        }
        add_vertex(v, eos);
    }
};

static int header_words(uint32_t pcw, int list) {
    uint32_t para = pcw >> 29;
    if (para == 5) return 8;
    if (list == L_OPAQUE_MOD || list == L_TRANS_MOD) return 8;
    uint32_t col = (pcw >> 4) & 3; bool vol = (pcw >> 6) & 1, off = (pcw >> 2) & 1;
    if (!vol) return (col == 2 && off) ? 16 : 8;
    return col == 2 ? 16 : 8;
}
static int vertex_words(uint32_t obj, bool sprite, int list) {
    if (sprite || list == L_OPAQUE_MOD || list == L_TRANS_MOD) return 16;
    uint32_t col = (obj >> 4) & 3; bool tex = (obj >> 3) & 1, vol = (obj >> 6) & 1;
    if (!vol) return (tex && col == 1) ? 16 : 8;
    return tex ? 16 : 8;
}

void build_drawlist(const uint32_t* ta, size_t n_words, const uint32_t* pvr, DrawList& dl) {
    {   // reset, keeping the vectors' capacity (no per-frame reallocation of ~50k vertices)
        std::vector<Vertex> v = std::move(dl.verts); v.clear();
        std::vector<uint16_t> ix = std::move(dl.idx); ix.clear();
        std::vector<Batch> l[L_COUNT]; for (int i = 0; i < L_COUNT; ++i) { l[i] = std::move(dl.lists[i]); l[i].clear(); }
        dl = DrawList();
        dl.verts = std::move(v); dl.idx = std::move(ix); for (int i = 0; i < L_COUNT; ++i) dl.lists[i] = std::move(l[i]);
    }
    texture_set_regs(pvr);
    auto R = [&](uint32_t off) { return pvr[off / 4]; };
    uint32_t xc = R(0x068), yc = R(0x06C);
    int w = (int)((xc >> 16) & 0x7FF) + 1, h = (int)((yc >> 16) & 0x3FF) + 1;
    if (w > 16 && w <= 1280) dl.width = w;
    if (h > 16 && h <= 1024) dl.height = h;
    dl.alpha_ref = (R(0x11C) & 0xFF) / 255.0f;
    dl.presort = R(0x098) & 1;
    dl.fog_col_ram = R(0x0B0) & 0xFFFFFF; dl.fog_col_vert = R(0x0B4) & 0xFFFFFF;
    { uint32_t fd = R(0x0B8); dl.fog_density = ((fd >> 8) & 0xFF) / 128.0f * std::ldexp(1.0f, (int8_t)(fd & 0xFF)); }
    for (int k = 0; k < 128; ++k) { uint32_t e = R(0x200 + 4 * k); dl.fog_hi[k] = (e >> 8) & 0xFF; dl.fog_lo[k] = e & 0xFF; }
    // background plane: ISP/TSP/TCW + 3 vertices in VRAM at PARAM_BASE + tag*4
    {
        uint32_t bgt = R(0x08C);
        uint32_t addr = (R(0x020) & 0xF00000) + ((bgt >> 3) & 0x1FFFFF) * 4;
        uint32_t isp = vram32(addr);
        bool tex = (isp >> 25) & 1;
        uint32_t col = vram32(addr + 12 + 12 + (tex ? 8 : 0));
        dl.clear_color = col | 0xFF000000u;
        dl.clear_depth = fbits(R(0x088));
    }
    Decoder d; d.dl = &dl;
    size_t i = 0, n = n_words;
    while (i + 8 <= n) {
        uint32_t pcw = ta[i], para = pcw >> 29;
        int words = 8;
        switch (para) {
        case 0: d.list = -1; d.strip_clear(); d.cur = nullptr; d.modvol = false; break;
        case 1: case 2: break;
        case 4: case 5: {
            int lst = d.list < 0 ? (int)((pcw >> 24) & 7) : d.list;
            words = header_words(pcw, lst);
            if (i + words > n) { i = n; continue; }
            d.header(&ta[i], para);
            break;
        }
        case 7:
            words = vertex_words(d.pcw_obj, d.sprite, d.list);
            if (i + words > n) { i = n; continue; }
            d.vertex(&ta[i]);
            break;
        default: dl.skipped_params++; break;
        }
        i += words;
    }
    for (auto& b : dl.lists[L_TRANS]) {
        double s = 0; for (uint32_t k = 0; k < b.count; ++k) s += batch_vertex(dl, b, k).invw;
        b.sort_z = b.count ? (float)(s / b.count) : 0;
    }
    if (!dl.presort)   // approximation of PVR auto-sort: per batch, far (small 1/w) first
        std::stable_sort(dl.lists[L_TRANS].begin(), dl.lists[L_TRANS].end(),
                         [](const Batch& a, const Batch& b) { return a.sort_z < b.sort_z; });
}

} // namespace render
