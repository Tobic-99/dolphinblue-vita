// SH-4 translator semantics tests: runs translated test programs on a minimal runtime.
#include "sh4ctx.h"
#include "test_funcs.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include "test_dispatch.inc"

namespace sh4 {
alignas(64) uint8_t ram[16u << 20];
uint32_t sq_mem[2][8];
[[noreturn]] static void die(const char* w, uint32_t a) { std::fprintf(stderr, "FATAL %s %08x\n", w, a); std::exit(2); }
uint32_t mmio_r8(uint32_t a) { die("mmio_r8", a); }
uint32_t mmio_r16(uint32_t a) { die("mmio_r16", a); }
uint32_t mmio_r32(uint32_t a) { die("mmio_r32", a); }
void mmio_w8(uint32_t a, uint32_t) { die("mmio_w8", a); }
void mmio_w16(uint32_t a, uint32_t) { die("mmio_w16", a); }
void mmio_w32(uint32_t a, uint32_t) { die("mmio_w32", a); }
void dispatch(Ctx* c, uint32_t t) {
    for (unsigned i = 0; i < testdisp::count; ++i) if (testdisp::table[i].a == t) { testdisp::table[i].f(c); return; }
    die("dispatch: unknown target", t);
}
void fatal_jump(Ctx*, uint32_t from, uint32_t t) { std::fprintf(stderr, "bad jump from %08x\n", from); die("fatal_jump", t); }
void pref_sq(Ctx*, uint32_t) {}
void trapa(Ctx*, uint32_t, uint32_t pc) { die("trapa", pc); }
void sleep(Ctx*, uint32_t pc) { die("sleep", pc); }
void rte(Ctx*) { die("rte", 0); }
void ldtlb(Ctx*) {}
void poll_irq(Ctx* c) { c->irq_pending = 0; }
void set_sr(Ctx* c, uint32_t v) {
    if ((v ^ c->sr) & SR_RB) for (int i = 0; i < 8; ++i) std::swap(c->r[i], c->rbank[i]);
    c->t = v & 1; c->sr = v & ~1u;
}
}

#define CHECK(expr, val) do { auto got_ = (unsigned long long)(expr); auto exp_ = (unsigned long long)(val); \
    if (got_ != exp_) { std::printf("    %s = 0x%llx, expected 0x%llx\n", #expr, got_, exp_); ++fails; } } while (0)

struct Case { const char* name; void (*fn)(sh4::Ctx*); void (*init)(sh4::Ctx*); void (*check)(sh4::Ctx*, int&); };
static const Case cases[] = {
#include "test_cases.inc"
};

int main() {
    uint8_t* const mem_base = sh4::ram; const size_t mem_size = 16u << 20;
    // load the assembled test image at its guest address (0x8C010000 -> RAM offset 0x10000)
    if (FILE* f = std::fopen(TEST_IMAGE, "rb")) { size_t n = std::fread(mem_base + 0x10000, 1, mem_size - 0x10000, f); std::fclose(f); (void)n; }
    else { std::fprintf(stderr, "cannot open %s\n", TEST_IMAGE); return 2; }
    int failed = 0, n = 0;
    for (const Case& k : cases) {
        sh4::Ctx c{};
        std::fill(mem_base + 0x100000, mem_base + 0x200000, 0);
        c.r[15] = 0x8C200000u; c.fpscr = 0x00040000u; c.sr = 0x700000F0u;
        k.init(&c);
        k.fn(&c);
        int fails = 0;
        k.check(&c, fails);
        ++n;
        std::printf("%-26s %s\n", k.name, fails ? "FAIL" : "ok");
        if (fails) ++failed;
    }
    std::printf("%d/%d passed\n", n - failed, n);
    return failed ? 1 : 0;
}
