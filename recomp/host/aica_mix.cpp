// AICA sound generator: the 64 PCM/ADPCM channels the ARM7 driver programs, mixed to 44.1 kHz stereo.
// Own implementation from the AICA register documentation (channel layout, PCM formats, Yamaha ADPCM, pitch,
// loop control, envelope generator with the hardware rate times, TL/DISDL/DIPAN levels, master volume).
// Effects: per-channel sends (ISEL/IMXL) feed the DSP (aica_dsp.cpp). Not modelled yet: LFOs, the filter.
#include "hw.h"
#include <cmath>
#include <cstring>

namespace hw {
extern uint16_t* const aica_regs_ptr;
uint16_t* aica_regs();   // aica.cpp: 16-bit registers at a 4-byte stride
uint8_t* aica_aram();
void aica_dsp_input(int sel, int32_t v);             // aica_dsp.cpp
void aica_dsp_step();
void aica_dsp_output(float* l, float* r, const float* step3_gain);

namespace {
inline uint16_t CR(int ch, int n) { return aica_regs_ptr[(ch * 0x80 + n * 4) / 4]; }   // channel register n (byte offset n*4)

// envelope: attack/decay times (ms) from full scale to -96 dB per effective rate 0..63 (hardware rate tables)
const float kAttackMs[64] = {1e9f, 1e9f, 8100, 6900, 6000, 4800, 4000, 3400, 3000, 2400, 2000, 1700, 1500, 1200, 1000, 860, 760, 600, 500,
    430, 380, 300, 250, 220, 190, 150, 130, 110, 95, 76, 63, 55, 47, 38, 31, 27, 24, 19, 15, 13, 12, 9.4f, 7.9f, 6.8f, 6.0f, 4.7f, 3.8f,
    3.4f, 3.0f, 2.4f, 2.0f, 1.8f, 1.6f, 1.3f, 1.1f, 0.93f, 0.85f, 0.65f, 0.53f, 0.44f, 0.40f, 0.35f, 0, 0};
const float kDecayMs[64] = {1e9f, 1e9f, 118200, 101300, 88600, 70900, 59100, 50700, 44300, 35500, 29600, 25300, 22200, 17700, 14800,
    12700, 11100, 8900, 7400, 6300, 5500, 4400, 3700, 3200, 2800, 2200, 1800, 1600, 1400, 1100, 920, 790, 690, 550, 460, 390, 340, 270,
    230, 200, 170, 140, 110, 98, 85, 68, 57, 49, 43, 34, 28, 25, 22, 18, 14, 12, 11, 8.5f, 7.1f, 6.1f, 5.4f, 4.3f, 3.6f, 3.1f};
const int kAdpcmDiff[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
const int kAdpcmScale[8] = {0x0E6, 0x0E6, 0x0E6, 0x0E6, 0x133, 0x199, 0x200, 0x266};

enum Stage : uint8_t { ATTACK, DECAY1, DECAY2, RELEASE, OFF };   // one byte: Channel is saved raw, and the shipped states use a 48-byte Channel
struct Channel {
    bool on = false;
    Stage stage = OFF;
    float att = 1023;           // envelope attenuation, 0 (max) .. 1023 (silent), 0.09375 dB per step
    uint64_t pos = 0;           // sample position, 32.32 fixed point, relative to SA
    uint32_t prev_int = 0;      // integer position already decoded (ADPCM)
    int adpcm_val = 0, adpcm_step = 0x7F;
    int adpcm_loop_val = 0, adpcm_loop_step = 0x7F; bool adpcm_loop_saved = false;
    int16_t cur = 0;            // current decoded sample
    bool lp = false;            // loop end passed (monitor flag)
};
static_assert(sizeof(Channel) == 48, "Channel is part of the save-state format");
Channel ch[64];
// per-channel parameters derived from the channel registers; recomputed only after the channel is written
struct Params {
    float rate[4];              // attenuation step per sample: attack, decay1, decay2, release
    float dl;                   // decay level (attenuation)
    float tl_g, dis_g, pan_l, pan_r, send_g;
    int isel; bool voff, send, loop; uint32_t fmt, lsa, lea;
    uint64_t step;
};
Params prm[64];
bool prm_dirty[64];

float level_db(float db) { return std::pow(10.0f, db / 20.0f); }
float lin_att[1025];            // envelope attenuation step -> gain
float tl_gain[256];             // TL: 0.375 dB steps
float step3_gain[16];           // DISDL / pan / master volume: 3 dB steps (index = attenuation steps)
bool tables_ready = [] {
    for (int i = 0; i <= 1024; ++i) lin_att[i] = i >= 1023 ? 0.0f : level_db(-0.09375f * i);
    for (int i = 0; i < 256; ++i) tl_gain[i] = level_db(-0.375f * i);
    for (int i = 0; i < 16; ++i) step3_gain[i] = level_db(-3.0f * i);
    return true;
}();

int eff_rate(int ch_, int r) {
    if (r == 0) return 0;
    int krs = (CR(ch_, 5) >> 10) & 15;
    int oct = (CR(ch_, 6) >> 11) & 15; if (oct & 8) oct -= 16;
    int e = 2 * r + (krs != 15 ? krs + oct : 0);
    return e < 0 ? 0 : e > 63 ? 63 : e;
}
float rate_step(float ms) { return ms <= 0 ? 1024.0f : ms >= 1e8f ? 0.0f : 1023.0f / (ms * 44.1f); }   // attenuation per sample

void update_params(int c) {
    Params& p = prm[c];
    p.rate[0] = rate_step(kAttackMs[eff_rate(c, CR(c, 4) & 31)]);
    p.rate[1] = rate_step(kDecayMs[eff_rate(c, (CR(c, 4) >> 6) & 31)]);
    p.rate[2] = rate_step(kDecayMs[eff_rate(c, (CR(c, 4) >> 11) & 31)]);
    p.rate[3] = rate_step(kDecayMs[eff_rate(c, CR(c, 5) & 31)]);
    p.dl = (float)(((CR(c, 5) >> 5) & 31) << 5);
    uint16_t r9 = CR(c, 9), r10 = CR(c, 10), r8 = CR(c, 8);
    int disdl = (r9 >> 8) & 15, dipan = r9 & 31, tl = r10 >> 8;
    p.voff = (r10 >> 6) & 1;
    p.tl_g = tl_gain[tl];
    p.dis_g = disdl ? step3_gain[15 - disdl] : 0.0f;
    int pa = dipan & 15;
    float pg = pa == 15 ? 0.0f : step3_gain[pa];
    p.pan_l = (dipan & 16) ? pg : 1.0f; p.pan_r = (dipan & 16) ? 1.0f : pg;
    int imxl = (r8 >> 4) & 15;
    p.send = imxl != 0; p.isel = r8 & 15; p.send_g = imxl ? step3_gain[15 - imxl] : 0.0f;
    uint16_t r6 = CR(c, 6);
    int oct = (r6 >> 11) & 15; if (oct & 8) oct -= 16;
    uint64_t step = ((uint64_t)(1024 + (r6 & 0x3FF)) << 22);   // (1024+FNS)/1024 in 32.32
    p.step = oct >= 0 ? step << oct : step >> -oct;
    p.lsa = CR(c, 2); p.lea = CR(c, 3);
    p.loop = (CR(c, 0) >> 9) & 1; p.fmt = (CR(c, 0) >> 7) & 3;
    prm_dirty[c] = false;
}

int16_t read_sample(int c, uint32_t idx) {
    uint32_t sa = ((CR(c, 0) & 0x7F) << 16) | CR(c, 1);
    uint32_t fmt = (CR(c, 0) >> 7) & 3;
    const uint8_t* a = aica_aram();
    if (fmt == 0) { int16_t v; std::memcpy(&v, a + ((sa + idx * 2) & 0x1FFFFE), 2); return v; }
    if (fmt == 1) return (int16_t)((int8_t)a[(sa + idx) & 0x1FFFFF] << 8);
    // ADPCM: decode forward from the last decoded position (nibbles, low nibble first)
    Channel& k = ch[c];
    while (k.prev_int < idx) {
        uint32_t n = ++k.prev_int;
        uint8_t b = a[(sa + (n >> 1)) & 0x1FFFFF];
        int nib = (n & 1) ? b >> 4 : b & 15;
        k.adpcm_val += (k.adpcm_step * kAdpcmDiff[nib]) / 8;
        k.adpcm_val = k.adpcm_val > 32767 ? 32767 : k.adpcm_val < -32768 ? -32768 : k.adpcm_val;
        k.adpcm_step = (k.adpcm_step * kAdpcmScale[nib & 7]) >> 8;
        k.adpcm_step = k.adpcm_step < 0x7F ? 0x7F : k.adpcm_step > 0x6000 ? 0x6000 : k.adpcm_step;
        if (!k.adpcm_loop_saved && n == CR(c, 2)) { k.adpcm_loop_val = k.adpcm_val; k.adpcm_loop_step = k.adpcm_step; k.adpcm_loop_saved = true; }
    }
    return (int16_t)k.adpcm_val;
}

void key_on(int c) {
    prm_dirty[c] = true;
    Channel& k = ch[c];
    k = Channel();
    k.on = true; k.stage = ATTACK; k.att = 1023;
    k.prev_int = 0;
    uint32_t sa = ((CR(c, 0) & 0x7F) << 16) | CR(c, 1);
    if (((CR(c, 0) >> 7) & 3) >= 2) {   // ADPCM: decode sample 0 (low nibble of the first byte)
        uint8_t b = aica_aram()[sa & 0x1FFFFF];
        int nib = b & 15;
        k.adpcm_val = (0x7F * kAdpcmDiff[nib]) / 8;
        k.adpcm_step = (0x7F * kAdpcmScale[nib & 7]) >> 8; if (k.adpcm_step < 0x7F) k.adpcm_step = 0x7F;
        if (CR(c, 2) == 0) { k.adpcm_loop_val = k.adpcm_val; k.adpcm_loop_step = k.adpcm_step; k.adpcm_loop_saved = true; }
    }
    k.cur = read_sample(c, 0);
    if (eff_rate(c, CR(c, 4) & 31) >= 62) { k.att = 0; k.stage = DECAY1; }   // instant attack
}
void key_off(int c) { if (ch[c].on && ch[c].stage != OFF) ch[c].stage = RELEASE; }
} // namespace

void aica_mix_reg_written(uint32_t off) { if (off < 0x2000) prm_dirty[off >> 7] = true; }   // channel register write

// a write with KYONEX set applies every channel's KYONB
void aica_mix_kyonex() {
    for (int c = 0; c < 64; ++c) {
        bool kb = (CR(c, 0) >> 14) & 1;
        if (kb && (!ch[c].on || ch[c].stage == RELEASE || ch[c].stage == OFF)) key_on(c);
        else if (!kb && ch[c].on) key_off(c);
    }
}

// one output sample (left, right), 16-bit range
void aica_mix_sample(int32_t* out_l, int32_t* out_r) {
    float l = 0, r = 0;
    for (int c = 0; c < 64; ++c) {
        Channel& k = ch[c];
        if (!k.on) continue;
        if (prm_dirty[c]) update_params(c);
        const Params& p = prm[c];
        // envelope
        switch (k.stage) {
        case ATTACK: k.att -= p.rate[0]; if (k.att <= 0) { k.att = 0; k.stage = DECAY1; } break;
        case DECAY1: k.att += p.rate[1]; if (k.att >= p.dl) { k.att = p.dl; k.stage = DECAY2; } break;
        case DECAY2: k.att += p.rate[2]; break;
        case RELEASE: k.att += p.rate[3]; break;
        case OFF: break;
        }
        if (k.att >= 1023) { k.att = 1023; if (k.stage == RELEASE || k.stage == DECAY2) { k.on = false; k.stage = OFF; continue; } }
        // level: envelope x total level x direct send level, panned
        float ge = (p.voff ? 1.0f : lin_att[(int)k.att]) * p.tl_g;
        float g = ge * p.dis_g;
        if (p.send) aica_dsp_input(p.isel, (int32_t)(k.cur * ge * p.send_g) << 4);   // 20-bit send
        float s = k.cur;
        l += s * (g * p.pan_l); r += s * (g * p.pan_r);
        // advance: step = (1024 + FNS) * 2^OCT / 1024 samples per output sample
        k.pos += p.step;
        uint32_t idx = (uint32_t)(k.pos >> 32);
        if (idx >= p.lea) {
            k.lp = true;
            if (p.loop) {   // LPCTL: loop back to LSA
                uint32_t len = p.lea > p.lsa ? p.lea - p.lsa : 1;
                idx = p.lsa + (idx - p.lea) % len;
                k.pos = ((uint64_t)idx << 32) | (k.pos & 0xFFFFFFFFu);
                if (p.fmt >= 2) { k.prev_int = p.lsa; k.adpcm_val = k.adpcm_loop_val; k.adpcm_step = k.adpcm_loop_step; }
            } else { k.on = false; k.stage = OFF; continue; }
        }
        k.cur = read_sample(c, idx);
    }
    aica_dsp_step();
    aica_dsp_output(&l, &r, step3_gain);
    uint16_t mv = aica_regs_ptr[0x2800 / 4] & 15;
    float m = mv ? step3_gain[15 - mv] : 0.0f;
    l *= m; r *= m;
    *out_l = (int32_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
    *out_r = (int32_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
}

// monitor registers for the channel selected by MSLC (0x280C bits 13:8): 0x2810 = LP (bit 15) | EG level,
// 0x2814 = current sample position (CA); LP clears on read
uint32_t aica_mix_monitor(uint32_t off) {
    int c = (aica_regs_ptr[0x280C / 4] >> 8) & 63;
    Channel& k = ch[c];
    if (off == 0x2810) { uint32_t v = (k.lp ? 0x8000u : 0) | ((uint32_t)k.att & 0x1FFF); k.lp = false; return v; }
    return (uint32_t)(k.pos >> 32) & 0xFFFF;
}
void aica_mix_state(SaveIO& s) {
    s.v(ch); s.v(prm);
    if (s.load) for (auto& d : prm_dirty) d = true;
}
} // namespace hw
