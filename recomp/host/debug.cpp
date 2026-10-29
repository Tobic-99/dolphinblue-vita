// Debug support for the SH4_DEBUG build: shadow call stack, FPU entry-mode checks, write watchpoints.
#include "hw.h"
#ifdef SH4_DEBUG
#include <vector>
namespace sh4 {
uint32_t watch_lo = 0, watch_hi = 0;
static std::vector<uint32_t> stack;
static uint64_t fp_mismatch;
void dbg_enter(Ctx* c, uint32_t addr, uint32_t fpmask, uint32_t fpval) {
    stack.push_back(addr);
    c->cur_func = addr;
    if ((c->fpscr & fpmask) != fpval && fp_mismatch++ < 20)
        std::printf("[dbg] FPU mode mismatch entering %08x: fpscr=%08x expected (mask %08x) %08x\n", addr, c->fpscr, fpmask, fpval);
}
void dbg_leave(Ctx* c) { stack.pop_back(); c->cur_func = stack.empty() ? 0 : stack.back(); }
void dbg_watch(uint32_t a, uint32_t v, int size) {
    std::printf("[watch] wr%d %08x <- %08x  vbl=%u stack:", size * 8, a, v, hw::vblank_count);
    for (size_t i = stack.size() > 8 ? stack.size() - 8 : 0; i < stack.size(); ++i) std::printf(" %08x", stack[i]);
    std::printf("\n");
}
void dbg_print_stack() {
    std::printf("call stack:");
    for (size_t i = stack.size() > 24 ? stack.size() - 24 : 0; i < stack.size(); ++i) std::printf(" %08x", stack[i]);
    std::printf("\n");
}
}
#else
namespace sh4 { void dbg_print_stack() {} }
#endif
