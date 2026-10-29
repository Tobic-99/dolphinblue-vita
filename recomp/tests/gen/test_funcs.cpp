#include "sh4ctx.h"
#include "test_funcs.h"

__attribute__((weak)) void f_8c010000(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c010000u, 0x00380000u, 0x00000000u);
L_8c010000:;
    ::sh4::addc(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c010008(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c010008u, 0x00380000u, 0x00000000u);
L_8c010008:;
    ::sh4::addc(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c010010(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c010010u, 0x00380000u, 0x00000000u);
L_8c010010:;
    ::sh4::subc(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c010018(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c010018u, 0x00380000u, 0x00000000u);
L_8c010018:;
    ::sh4::negc(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c010020(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c010020u, 0x00380000u, 0x00000000u);
L_8c010020:;
    ::sh4::addv(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c010028(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c010028u, 0x00380000u, 0x00000000u);
L_8c010028:;
    ::sh4::subv(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c010030(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R0 = c->r[0], R1 = c->r[1];
    uint32_t S_t = c->t;
    SH4_ENTRY_CHECK(c, 0x8c010030u, 0x00380000u, 0x00000000u);
L_8c010030:;
    R0 = R0 << 16;
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div0u(c);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; S_t = c->t; }
    { uint32_t t_ = R1 >> 31; R1 = (R1 << 1) | S_t; S_t = t_; }
    R1 = R1 & 0xFFFFu;
    { c->r[0] = R0; c->r[1] = R1; c->t = S_t; return; }
    c->r[0] = R0; c->r[1] = R1; c->t = S_t;
}

__attribute__((weak)) void f_8c01005c(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R0 = c->r[0], R1 = c->r[1], R2 = c->r[2], R3 = c->r[3];
    uint32_t S_t = c->t;
    SH4_ENTRY_CHECK(c, 0x8c01005cu, 0x00380000u, 0x00000000u);
L_8c01005c:;
    R0 = R0 << 16;
    R1 = ::sh4::sx16(R1);
    R2 ^= R2;
    R3 = R1;
    { uint32_t t_ = R3 >> 31; R3 = (R3 << 1) | S_t; S_t = t_; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::subc(c, 1, 2);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div0s(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::div1(c, 1, 0);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    R1 = ::sh4::sx16(R1);
    { uint32_t t_ = R1 >> 31; R1 = (R1 << 1) | S_t; S_t = t_; }
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; }
    ::sh4::addc(c, 1, 2);
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; R3 = c->r[3]; S_t = c->t; }
    R1 = ::sh4::sx16(R1);
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t; return; }
    c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->t = S_t;
}

__attribute__((weak)) void f_8c010098(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c010098u, 0x00380000u, 0x00000000u);
L_8c010098:;
    ::sh4::shad(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c0100a0(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c0100a0u, 0x00380000u, 0x00000000u);
L_8c0100a0:;
    ::sh4::shad(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c0100a8(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c0100a8u, 0x00380000u, 0x00000000u);
L_8c0100a8:;
    ::sh4::shld(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c0100b0(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c0100b0u, 0x00380000u, 0x00000000u);
L_8c0100b0:;
    ::sh4::shad(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c0100b8(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1], R2 = c->r[2];
    uint32_t S_t = c->t;
    SH4_ENTRY_CHECK(c, 0x8c0100b8u, 0x00380000u, 0x00000000u);
L_8c0100b8:;
    { uint32_t t_ = R1 & 1; R1 = (R1 >> 1) | (S_t << 31); S_t = t_; }
    { uint32_t t_ = R2 >> 31; R2 = (R2 << 1) | S_t; S_t = t_; }
    S_t = 0;
    { c->r[1] = R1; c->r[2] = R2; c->t = S_t; return; }
    c->r[1] = R1; c->r[2] = R2; c->t = S_t;
}

__attribute__((weak)) void f_8c0100c4(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1], R2 = c->r[2];
    uint32_t S_t = c->t;
    SH4_ENTRY_CHECK(c, 0x8c0100c4u, 0x00380000u, 0x00000000u);
L_8c0100c4:;
    S_t = ::sh4::cmpstr(R1, R2);
    { c->t = S_t; return; }
    c->t = S_t;
}

__attribute__((weak)) void f_8c0100cc(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1], R2 = c->r[2];
    uint32_t S_t = c->t;
    SH4_ENTRY_CHECK(c, 0x8c0100ccu, 0x00380000u, 0x00000000u);
L_8c0100cc:;
    S_t = ::sh4::cmpstr(R1, R2);
    { c->t = S_t; return; }
    c->t = S_t;
}

__attribute__((weak)) void f_8c0100d4(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1], R2 = c->r[2];
    uint32_t S_t = c->t;
    SH4_ENTRY_CHECK(c, 0x8c0100d4u, 0x00380000u, 0x00000000u);
L_8c0100d4:;
    R2 += 0x00000001u;
    R1 -= 1; S_t = R1 == 0;
    if (!S_t) { if (--c->slice <= 0 || c->irq_pending) { c->r[1] = R1; c->r[2] = R2; c->t = S_t; ::sh4::poll_irq(c); R1 = c->r[1]; R2 = c->r[2]; S_t = c->t; c->cur_func = 0x8c0100d4U; } goto L_8c0100d4; }
    { c->r[1] = R1; c->r[2] = R2; c->t = S_t; return; }
    c->r[1] = R1; c->r[2] = R2; c->t = S_t;
}

__attribute__((weak)) void f_8c0100e0(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R0 = c->r[0], R1 = c->r[1], R3 = c->r[3], R4 = c->r[4];
    uint32_t S_t = c->t;
    SH4_ENTRY_CHECK(c, 0x8c0100e0u, 0x00380000u, 0x00000000u);
L_8c0100e0:;
    S_t = R0 == 0x00000005u;
    S_t = R1 == R1;
    { uint32_t cond_ = S_t;
    R3 = 0x00000007u;
    if (cond_) { goto L_8c0100ea; } }
    R4 = 0x00000001u;
L_8c0100ea:;
    { c->r[3] = R3; c->r[4] = R4; c->t = S_t; return; }
    c->r[3] = R3; c->r[4] = R4; c->t = S_t;
}

__attribute__((weak)) void f_8c0100f0(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R3 = c->r[3], R4 = c->r[4];
    uint32_t S_t = c->t;
    SH4_ENTRY_CHECK(c, 0x8c0100f0u, 0x00380000u, 0x00000000u);
L_8c0100f0:;
    S_t = 1;
    { uint32_t cond_ = !S_t;
    R3 = 0x00000007u;
    if (cond_) { goto L_8c0100f8; } }
    R4 = 0x00000001u;
L_8c0100f8:;
    { c->r[3] = R3; c->r[4] = R4; c->t = S_t; return; }
    c->r[3] = R3; c->r[4] = R4; c->t = S_t;
}

__attribute__((weak)) void f_8c0100fc(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R0 = c->r[0], R15 = c->r[15];
    uint32_t S_pr = c->pr;
    SH4_ENTRY_CHECK(c, 0x8c0100fcu, 0x00380000u, 0x00000000u);
L_8c0100fc:;
    R15 -= 4; ::sh4::wr32(R15, S_pr);
    {
    S_pr = 0x8c010102u;
    { c->r[0] = R0; c->r[15] = R15; c->pr = S_pr; }
    f_8c01010a(c); }
    { R0 = c->r[0]; R15 = c->r[15]; S_pr = c->pr; c->cur_func = 0x8c0100fcU; }
    if (--c->slice <= 0 || c->irq_pending) { c->r[0] = R0; c->r[15] = R15; c->pr = S_pr; ::sh4::poll_irq(c); R0 = c->r[0]; R15 = c->r[15]; S_pr = c->pr; c->cur_func = 0x8c0100fcU; }
    R0 += 0x00000001u;
    { uint32_t v_ = ::sh4::rd32(R15); R15 += 4; S_pr = v_; }
    { c->r[0] = R0; c->r[15] = R15; c->pr = S_pr; return; }
    c->r[0] = R0; c->r[15] = R15; c->pr = S_pr;
}

__attribute__((weak)) void f_8c01010a(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R0 = c->r[0];
    SH4_ENTRY_CHECK(c, 0x8c01010au, 0x00380000u, 0x00000000u);
L_8c01010a:;
    R0 = 0x0000002au;
    { c->r[0] = R0; return; }
    c->r[0] = R0;
}

__attribute__((weak)) void f_8c010110(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1], R15 = c->r[15];
    uint32_t S_pr = c->pr;
    SH4_ENTRY_CHECK(c, 0x8c010110u, 0x00380000u, 0x00000000u);
L_8c010110:;
    R15 -= 4; ::sh4::wr32(R15, S_pr);
    R1 = 0x8c010124u; /* lit @8c010120 */
    {
    S_pr = 0x8c010118u;
    { c->r[1] = R1; c->r[15] = R15; c->pr = S_pr; }
    f_8c010124(c); }
    { R1 = c->r[1]; R15 = c->r[15]; S_pr = c->pr; c->cur_func = 0x8c010110U; }
    if (--c->slice <= 0 || c->irq_pending) { c->r[1] = R1; c->r[15] = R15; c->pr = S_pr; ::sh4::poll_irq(c); R1 = c->r[1]; R15 = c->r[15]; S_pr = c->pr; c->cur_func = 0x8c010110U; }
    { uint32_t v_ = ::sh4::rd32(R15); R15 += 4; S_pr = v_; }
    { c->r[1] = R1; c->r[15] = R15; c->pr = S_pr; return; }
    c->r[1] = R1; c->r[15] = R15; c->pr = S_pr;
}

__attribute__((weak)) void f_8c010124(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R5 = c->r[5];
    uint32_t S_pr = c->pr;
    SH4_ENTRY_CHECK(c, 0x8c010124u, 0x00380000u, 0x00000000u);
L_8c010124:;
    R5 = S_pr;
    { c->r[5] = R5; return; }
    c->r[5] = R5;
}

__attribute__((weak)) void f_8c010128(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1];
    SH4_ENTRY_CHECK(c, 0x8c010128u, 0x00380000u, 0x00000000u);
L_8c010128:;
    { uint32_t v_ = R1, a_ = R1 - 4; ::sh4::wr32(a_, v_); R1 = a_; }
    { c->r[1] = R1; return; }
    c->r[1] = R1;
}

__attribute__((weak)) void f_8c010130(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1];
    SH4_ENTRY_CHECK(c, 0x8c010130u, 0x00380000u, 0x00000000u);
L_8c010130:;
    R1 = ::sh4::rd32(R1);
    { c->r[1] = R1; return; }
    c->r[1] = R1;
}

__attribute__((weak)) void f_8c010138(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1], R2 = c->r[2];
    SH4_ENTRY_CHECK(c, 0x8c010138u, 0x00380000u, 0x00000000u);
L_8c010138:;
    R2 = ::sh4::sx16(::sh4::rd16(R1));
    { c->r[2] = R2; return; }
    c->r[2] = R2;
}

__attribute__((weak)) void f_8c010140(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1], R2 = c->r[2], R3 = c->r[3], R4 = c->r[4];
    SH4_ENTRY_CHECK(c, 0x8c010140u, 0x00380000u, 0x00000000u);
L_8c010140:;
    R2 = (R1 & 0xFFFF0000u) | ((R1 & 0xFFu) << 8) | ((R1 >> 8) & 0xFFu);
    R3 = (R1 << 16) | (R1 >> 16);
    R4 = (R4 >> 16) | (R1 << 16);
    { c->r[2] = R2; c->r[3] = R3; c->r[4] = R4; return; }
    c->r[2] = R2; c->r[3] = R3; c->r[4] = R4;
}

__attribute__((weak)) void f_8c01014c(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c01014cu, 0x00380000u, 0x00000000u);
L_8c01014c:;
    ::sh4::dmuls(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c010154(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c010154u, 0x00380000u, 0x00000000u);
L_8c010154:;
    ::sh4::dmulu(c, 1, 2);
    return;
}

__attribute__((weak)) void f_8c01015c(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1], R2 = c->r[2], R3 = c->r[3];
    uint32_t S_macl = c->macl;
    SH4_ENTRY_CHECK(c, 0x8c01015cu, 0x00380000u, 0x00000000u);
L_8c01015c:;
    S_macl = (uint32_t)((int32_t)(int16_t)R1 * (int32_t)(int16_t)R2);
    R3 = S_macl;
    S_macl = (uint32_t)(uint16_t)R1 * (uint32_t)(uint16_t)R2;
    { c->r[3] = R3; c->macl = S_macl; return; }
    c->r[3] = R3; c->macl = S_macl;
}

__attribute__((weak)) void f_8c010168(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c010168u, 0x00380000u, 0x00000000u);
L_8c010168:;
    ::sh4::tas(c, 1);
    return;
}

__attribute__((weak)) void f_8c010170(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1], R2 = c->r[2], R3 = c->r[3], R4 = c->r[4], R5 = c->r[5], R6 = c->r[6], R7 = c->r[7];
    uint32_t S_t = c->t;
    SH4_ENTRY_CHECK(c, 0x8c010170u, 0x00380000u, 0x00000000u);
L_8c010170:;
    R3 = ::sh4::sx8(R1);
    R4 = R2 & 0xFFFFu;
    R5 = 0x00000000u;
    R5 += 0xffffffffu;
    S_t = R5 & 1; R5 >>= 1;
    R6 = S_t;
    R7 = 0u - R2;
    { c->r[3] = R3; c->r[4] = R4; c->r[5] = R5; c->r[6] = R6; c->r[7] = R7; c->t = S_t; return; }
    c->r[3] = R3; c->r[4] = R4; c->r[5] = R5; c->r[6] = R6; c->r[7] = R7; c->t = S_t;
}

__attribute__((weak)) void f_8c010184(sh4::Ctx* c) {   // weak: host code may override with a native port
    union { uint32_t u; float f; } F0_0; F0_0.u = c->fr.u[0][0];
    union { uint32_t u; float f; } F0_1; F0_1.u = c->fr.u[0][1];
    uint32_t S_fpul = c->fpul;
    SH4_ENTRY_CHECK(c, 0x8c010184u, 0x00380000u, 0x00000000u);
L_8c010184:;
    F0_0.u = 0x3F800000u;
    F0_1.f = (float)(int32_t)S_fpul;
    F0_1.f = F0_1.f + F0_0.f;
    S_fpul = ::sh4::ftrc_s(F0_1.f);
    { c->fr.u[0][0] = F0_0.u; c->fr.u[0][1] = F0_1.u; c->fpul = S_fpul; return; }
    c->fr.u[0][0] = F0_0.u; c->fr.u[0][1] = F0_1.u; c->fpul = S_fpul;
}

__attribute__((weak)) void f_8c010190(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1], R2 = c->r[2], R3 = c->r[3];
    union { uint32_t u; float f; } F0_1; F0_1.u = c->fr.u[0][1];
    union { uint32_t u; float f; } F0_2; F0_2.u = c->fr.u[0][2];
    union { uint32_t u; float f; } F0_3; F0_3.u = c->fr.u[0][3];
    uint32_t S_fpul = c->fpul;
    SH4_ENTRY_CHECK(c, 0x8c010190u, 0x00380000u, 0x00000000u);
L_8c010190:;
    S_fpul = ::sh4::ftrc_s(F0_1.f);
    R1 = S_fpul;
    S_fpul = ::sh4::ftrc_s(F0_2.f);
    R2 = S_fpul;
    S_fpul = ::sh4::ftrc_s(F0_3.f);
    R3 = S_fpul;
    { c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->fpul = S_fpul; return; }
    c->r[1] = R1; c->r[2] = R2; c->r[3] = R3; c->fpul = S_fpul;
}

__attribute__((weak)) void f_8c0101a0(sh4::Ctx* c) {   // weak: host code may override with a native port
    union { uint32_t u; float f; } F0_0; F0_0.u = c->fr.u[0][0];
    union { uint32_t u; float f; } F0_1; F0_1.u = c->fr.u[0][1];
    union { uint32_t u; float f; } F0_2; F0_2.u = c->fr.u[0][2];
    SH4_ENTRY_CHECK(c, 0x8c0101a0u, 0x00380000u, 0x00000000u);
L_8c0101a0:;
    F0_2.f = ::sh4::fmac(F0_0.f, F0_1.f, F0_2.f);
    { c->fr.u[0][2] = F0_2.u; return; }
    c->fr.u[0][2] = F0_2.u;
}

__attribute__((weak)) void f_8c0101a8(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1];
    union { uint32_t u; float f; } F0_2; F0_2.u = c->fr.u[0][2];
    union { uint32_t u; float f; } F0_3; F0_3.u = c->fr.u[0][3];
    union { uint32_t u; float f; } F1_0; F1_0.u = c->fr.u[1][0];
    union { uint32_t u; float f; } F1_1; F1_1.u = c->fr.u[1][1];
    SH4_ENTRY_CHECK(c, 0x8c0101a8u, 0x00380000u, 0x00000000u);
L_8c0101a8:;
    c->fpscr ^= ::sh4::FPSCR_SZ;
    { uint32_t a_ = R1; F0_2.u = ::sh4::rd32(a_); F0_3.u = ::sh4::rd32(a_ + 4); } R1 += 8;
    F1_0.u = F0_2.u; F1_1.u = F0_3.u;
    c->fpscr ^= ::sh4::FPSCR_SZ;
    { c->r[1] = R1; c->fr.u[0][2] = F0_2.u; c->fr.u[0][3] = F0_3.u; c->fr.u[1][0] = F1_0.u; c->fr.u[1][1] = F1_1.u; return; }
    c->r[1] = R1; c->fr.u[0][2] = F0_2.u; c->fr.u[0][3] = F0_3.u; c->fr.u[1][0] = F1_0.u; c->fr.u[1][1] = F1_1.u;
}

__attribute__((weak)) void f_8c0101b4(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1];
    SH4_ENTRY_CHECK(c, 0x8c0101b4u, 0x00380000u, 0x00000000u);
L_8c0101b4:;
    R1 = 0x000c0000u; /* lit @8c0101c8 */
    c->fpscr = R1;
    { c->r[1] = R1; }
    ::sh4::setdr(c, 0, 0, (double)(int32_t)c->fpul);
    { R1 = c->r[1]; }
    { c->r[1] = R1; }
    ::sh4::setdr(c, 0, 0, ::sh4::getdr(c, 0, 0) + ::sh4::getdr(c, 0, 0));
    { R1 = c->r[1]; }
    { c->r[1] = R1; }
    c->fpul = ::sh4::ftrc_d(::sh4::getdr(c, 0, 0));
    { R1 = c->r[1]; }
    R1 = 0x00040000u; /* lit @8c0101cc */
    c->fpscr = R1;
    { c->r[1] = R1; return; }
    c->r[1] = R1;
}

__attribute__((weak)) void f_8c0101d0(sh4::Ctx* c) {   // weak: host code may override with a native port
    union { uint32_t u; float f; } F0_4; F0_4.u = c->fr.u[0][4];
    union { uint32_t u; float f; } F0_5; F0_5.u = c->fr.u[0][5];
    union { uint32_t u; float f; } F0_6; F0_6.u = c->fr.u[0][6];
    union { uint32_t u; float f; } F0_7; F0_7.u = c->fr.u[0][7];
    union { uint32_t u; float f; } F1_0; F1_0.u = c->fr.u[1][0];
    union { uint32_t u; float f; } F1_1; F1_1.u = c->fr.u[1][1];
    union { uint32_t u; float f; } F1_2; F1_2.u = c->fr.u[1][2];
    union { uint32_t u; float f; } F1_3; F1_3.u = c->fr.u[1][3];
    union { uint32_t u; float f; } F1_4; F1_4.u = c->fr.u[1][4];
    union { uint32_t u; float f; } F1_5; F1_5.u = c->fr.u[1][5];
    union { uint32_t u; float f; } F1_6; F1_6.u = c->fr.u[1][6];
    union { uint32_t u; float f; } F1_7; F1_7.u = c->fr.u[1][7];
    union { uint32_t u; float f; } F1_8; F1_8.u = c->fr.u[1][8];
    union { uint32_t u; float f; } F1_9; F1_9.u = c->fr.u[1][9];
    union { uint32_t u; float f; } F1_10; F1_10.u = c->fr.u[1][10];
    union { uint32_t u; float f; } F1_11; F1_11.u = c->fr.u[1][11];
    union { uint32_t u; float f; } F1_12; F1_12.u = c->fr.u[1][12];
    union { uint32_t u; float f; } F1_13; F1_13.u = c->fr.u[1][13];
    union { uint32_t u; float f; } F1_14; F1_14.u = c->fr.u[1][14];
    union { uint32_t u; float f; } F1_15; F1_15.u = c->fr.u[1][15];
    SH4_ENTRY_CHECK(c, 0x8c0101d0u, 0x00380000u, 0x00000000u);
L_8c0101d0:;
    { double t0_ = (double)F1_0.f * F0_4.f + (double)F1_4.f * F0_5.f + (double)F1_8.f * F0_6.f + (double)F1_12.f * F0_7.f, t1_ = (double)F1_1.f * F0_4.f + (double)F1_5.f * F0_5.f + (double)F1_9.f * F0_6.f + (double)F1_13.f * F0_7.f, t2_ = (double)F1_2.f * F0_4.f + (double)F1_6.f * F0_5.f + (double)F1_10.f * F0_6.f + (double)F1_14.f * F0_7.f, t3_ = (double)F1_3.f * F0_4.f + (double)F1_7.f * F0_5.f + (double)F1_11.f * F0_6.f + (double)F1_15.f * F0_7.f; F0_4.f = (float)t0_; F0_5.f = (float)t1_; F0_6.f = (float)t2_; F0_7.f = (float)t3_; }
    { c->fr.u[0][4] = F0_4.u; c->fr.u[0][5] = F0_5.u; c->fr.u[0][6] = F0_6.u; c->fr.u[0][7] = F0_7.u; return; }
    c->fr.u[0][4] = F0_4.u; c->fr.u[0][5] = F0_5.u; c->fr.u[0][6] = F0_6.u; c->fr.u[0][7] = F0_7.u;
}

__attribute__((weak)) void f_8c0101d8(sh4::Ctx* c) {   // weak: host code may override with a native port
    union { uint32_t u; float f; } F0_0; F0_0.u = c->fr.u[0][0];
    union { uint32_t u; float f; } F0_1; F0_1.u = c->fr.u[0][1];
    union { uint32_t u; float f; } F0_2; F0_2.u = c->fr.u[0][2];
    union { uint32_t u; float f; } F0_3; F0_3.u = c->fr.u[0][3];
    union { uint32_t u; float f; } F1_0; F1_0.u = c->fr.u[1][0];
    union { uint32_t u; float f; } F1_1; F1_1.u = c->fr.u[1][1];
    union { uint32_t u; float f; } F1_2; F1_2.u = c->fr.u[1][2];
    union { uint32_t u; float f; } F1_3; F1_3.u = c->fr.u[1][3];
    union { uint32_t u; float f; } F1_4; F1_4.u = c->fr.u[1][4];
    union { uint32_t u; float f; } F1_5; F1_5.u = c->fr.u[1][5];
    union { uint32_t u; float f; } F1_6; F1_6.u = c->fr.u[1][6];
    union { uint32_t u; float f; } F1_7; F1_7.u = c->fr.u[1][7];
    union { uint32_t u; float f; } F1_8; F1_8.u = c->fr.u[1][8];
    union { uint32_t u; float f; } F1_9; F1_9.u = c->fr.u[1][9];
    union { uint32_t u; float f; } F1_10; F1_10.u = c->fr.u[1][10];
    union { uint32_t u; float f; } F1_11; F1_11.u = c->fr.u[1][11];
    union { uint32_t u; float f; } F1_12; F1_12.u = c->fr.u[1][12];
    union { uint32_t u; float f; } F1_13; F1_13.u = c->fr.u[1][13];
    union { uint32_t u; float f; } F1_14; F1_14.u = c->fr.u[1][14];
    union { uint32_t u; float f; } F1_15; F1_15.u = c->fr.u[1][15];
    SH4_ENTRY_CHECK(c, 0x8c0101d8u, 0x00380000u, 0x00000000u);
L_8c0101d8:;
    { double t0_ = (double)F1_0.f * F0_0.f + (double)F1_4.f * F0_1.f + (double)F1_8.f * F0_2.f + (double)F1_12.f * F0_3.f, t1_ = (double)F1_1.f * F0_0.f + (double)F1_5.f * F0_1.f + (double)F1_9.f * F0_2.f + (double)F1_13.f * F0_3.f, t2_ = (double)F1_2.f * F0_0.f + (double)F1_6.f * F0_1.f + (double)F1_10.f * F0_2.f + (double)F1_14.f * F0_3.f, t3_ = (double)F1_3.f * F0_0.f + (double)F1_7.f * F0_1.f + (double)F1_11.f * F0_2.f + (double)F1_15.f * F0_3.f; F0_0.f = (float)t0_; F0_1.f = (float)t1_; F0_2.f = (float)t2_; F0_3.f = (float)t3_; }
    { c->fr.u[0][0] = F0_0.u; c->fr.u[0][1] = F0_1.u; c->fr.u[0][2] = F0_2.u; c->fr.u[0][3] = F0_3.u; return; }
    c->fr.u[0][0] = F0_0.u; c->fr.u[0][1] = F0_1.u; c->fr.u[0][2] = F0_2.u; c->fr.u[0][3] = F0_3.u;
}

__attribute__((weak)) void f_8c0101e0(sh4::Ctx* c) {   // weak: host code may override with a native port
    union { uint32_t u; float f; } F0_0; F0_0.u = c->fr.u[0][0];
    union { uint32_t u; float f; } F0_1; F0_1.u = c->fr.u[0][1];
    union { uint32_t u; float f; } F0_2; F0_2.u = c->fr.u[0][2];
    union { uint32_t u; float f; } F0_3; F0_3.u = c->fr.u[0][3];
    union { uint32_t u; float f; } F0_4; F0_4.u = c->fr.u[0][4];
    union { uint32_t u; float f; } F0_5; F0_5.u = c->fr.u[0][5];
    union { uint32_t u; float f; } F0_6; F0_6.u = c->fr.u[0][6];
    union { uint32_t u; float f; } F0_7; F0_7.u = c->fr.u[0][7];
    SH4_ENTRY_CHECK(c, 0x8c0101e0u, 0x00380000u, 0x00000000u);
L_8c0101e0:;
    { double r_ = (double)F0_4.f * F0_0.f + (double)F0_5.f * F0_1.f + (double)F0_6.f * F0_2.f + (double)F0_7.f * F0_3.f; F0_7.f = (float)r_; }
    { c->fr.u[0][7] = F0_7.u; return; }
    c->fr.u[0][7] = F0_7.u;
}

__attribute__((weak)) void f_8c0101e8(sh4::Ctx* c) {   // weak: host code may override with a native port
    SH4_ENTRY_CHECK(c, 0x8c0101e8u, 0x00380000u, 0x00000000u);
L_8c0101e8:;
    ::sh4::fsca(c, 0, 2);
    return;
}

__attribute__((weak)) void f_8c0101f0(sh4::Ctx* c) {   // weak: host code may override with a native port
    union { uint32_t u; float f; } F0_1; F0_1.u = c->fr.u[0][1];
    union { uint32_t u; float f; } F0_2; F0_2.u = c->fr.u[0][2];
    uint32_t S_t = c->t;
    SH4_ENTRY_CHECK(c, 0x8c0101f0u, 0x00380000u, 0x00000000u);
L_8c0101f0:;
    F0_1.u ^= 0x80000000u;
    F0_2.u &= 0x7FFFFFFFu;
    S_t = F0_2.f > F0_1.f;
    { c->fr.u[0][1] = F0_1.u; c->fr.u[0][2] = F0_2.u; c->t = S_t; return; }
    c->fr.u[0][1] = F0_1.u; c->fr.u[0][2] = F0_2.u; c->t = S_t;
}

__attribute__((weak)) void f_8c0101fc(sh4::Ctx* c) {   // weak: host code may override with a native port
    union { uint32_t u; float f; } F0_4; F0_4.u = c->fr.u[0][4];
    union { uint32_t u; float f; } F0_5; F0_5.u = c->fr.u[0][5];
    union { uint32_t u; float f; } F1_3; F1_3.u = c->fr.u[1][3];
    union { uint32_t u; float f; } F1_4; F1_4.u = c->fr.u[1][4];
    union { uint32_t u; float f; } F1_5; F1_5.u = c->fr.u[1][5];
    SH4_ENTRY_CHECK(c, 0x8c0101fcu, 0x00380000u, 0x00000000u);
L_8c0101fc:;
    c->fpscr ^= ::sh4::FPSCR_FR;
    F1_4.u = F1_3.u;
    c->fpscr ^= ::sh4::FPSCR_FR;
    F0_5.u = F0_4.u;
    c->fpscr ^= ::sh4::FPSCR_FR;
    F1_5.u = F1_4.u;
    c->fpscr ^= ::sh4::FPSCR_FR;
    { c->fr.u[0][5] = F0_5.u; c->fr.u[1][4] = F1_4.u; c->fr.u[1][5] = F1_5.u; return; }
    c->fr.u[0][5] = F0_5.u; c->fr.u[1][4] = F1_4.u; c->fr.u[1][5] = F1_5.u;
}

__attribute__((weak)) void f_8c010210(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R0 = c->r[0], R1 = c->r[1], R2 = c->r[2];
    uint32_t S_t = c->t;
    SH4_ENTRY_CHECK(c, 0x8c010210u, 0x00380000u, 0x00000000u);
L_8c010210:;
    R1 = 0x00000003u;
    S_t = R0 >= R1;
    if (S_t) { goto L_8c01023c; }
    S_t = R0 >> 31; R0 <<= 1;
    R1 = R0;
    R0 = 0x8c010224u;
    R0 = ::sh4::sx16(::sh4::rd16(R1 + R0));
    { uint32_t tgt_ = 0x8c010222u + R0;
    switch (tgt_) {
    case 0x8c01022au: goto L_8c01022a;
    case 0x8c010230u: goto L_8c010230;
    case 0x8c010236u: goto L_8c010236;
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->t = S_t; }
    default: ::sh4::fatal_jump(c, 0x8c01021eu, tgt_); } }
    { R0 = c->r[0]; R1 = c->r[1]; R2 = c->r[2]; S_t = c->t; }
L_8c01022a:;
    R2 = 0x0000000au;
    goto L_8c01023e;
L_8c010230:;
    R2 = 0x00000014u;
    goto L_8c01023e;
L_8c010236:;
    R2 = 0x0000001eu;
    goto L_8c01023e;
L_8c01023c:;
    R2 = 0x00000063u;
L_8c01023e:;
    { c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->t = S_t; return; }
    c->r[0] = R0; c->r[1] = R1; c->r[2] = R2; c->t = S_t;
}

__attribute__((weak)) void f_8c010244(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R1 = c->r[1], R3 = c->r[3], R15 = c->r[15];
    uint32_t S_pr = c->pr;
    SH4_ENTRY_CHECK(c, 0x8c010244u, 0x00380000u, 0x00000000u);
L_8c010244:;
    R15 -= 4; ::sh4::wr32(R15, S_pr);
    R1 = 0x8c100060u; /* lit @8c010254 */
    R3 = ::sh4::rd32(R1);
    { uint32_t tgt_ = R3;
    S_pr = 0x8c01024eu;
    { c->r[1] = R1; c->r[3] = R3; c->r[15] = R15; c->pr = S_pr; }
    ::sh4::dispatch(c, tgt_); }
    { R1 = c->r[1]; R3 = c->r[3]; R15 = c->r[15]; S_pr = c->pr; c->cur_func = 0x8c010244U; }
    if (--c->slice <= 0 || c->irq_pending) { c->r[1] = R1; c->r[3] = R3; c->r[15] = R15; c->pr = S_pr; ::sh4::poll_irq(c); R1 = c->r[1]; R3 = c->r[3]; R15 = c->r[15]; S_pr = c->pr; c->cur_func = 0x8c010244U; }
    { uint32_t v_ = ::sh4::rd32(R15); R15 += 4; S_pr = v_; }
    { c->r[1] = R1; c->r[3] = R3; c->r[15] = R15; c->pr = S_pr; return; }
    c->r[1] = R1; c->r[3] = R3; c->r[15] = R15; c->pr = S_pr;
}

__attribute__((weak)) void f_8c010258(sh4::Ctx* c) {   // weak: host code may override with a native port
    uint32_t R0 = c->r[0];
    SH4_ENTRY_CHECK(c, 0x8c010258u, 0x00380000u, 0x00000000u);
L_8c010258:;
    R0 = 0x0000004du;
    { c->r[0] = R0; return; }
    c->r[0] = R0;
}

