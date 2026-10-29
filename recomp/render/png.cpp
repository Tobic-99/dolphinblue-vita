// Minimal PNG writer (RGBA8, zlib deflate) for desktop verification output.
#include "draw.h"
#include <zlib.h>
#include <cstdio>
#include <vector>

namespace render {
static void be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> c; be32(c, (uint32_t)data.size());
    c.insert(c.end(), type, type + 4); c.insert(c.end(), data.begin(), data.end());
    uint32_t crc = crc32(0, c.data() + 4, (uInt)(c.size() - 4));
    be32(c, crc);
    std::fwrite(c.data(), 1, c.size(), f);
}
bool write_png(const char* path, int w, int h, const uint32_t* rgba) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr; be32(ihdr, w); be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw; raw.reserve((size_t)(w * 4 + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint8_t* row = (const uint8_t*)(rgba + (size_t)y * w);
        for (int x = 0; x < w; ++x) { raw.push_back(row[4 * x]); raw.push_back(row[4 * x + 1]); raw.push_back(row[4 * x + 2]); raw.push_back(255); }
    }
    uLongf clen = compressBound((uLong)raw.size());
    std::vector<uint8_t> comp(clen);
    compress2(comp.data(), &clen, raw.data(), (uLong)raw.size(), 6);
    comp.resize(clen);
    chunk(f, "IDAT", comp);
    chunk(f, "IEND", {});
    std::fclose(f);
    return true;
}
}
