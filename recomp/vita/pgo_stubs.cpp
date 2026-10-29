extern "C" {
/* DB_PGO=gen only. The vitasdk libgcov is built without its value profilers (those objects are empty), so the
   instrumented code's value/indirect-call/time profiling hooks are no-ops here. The edge (arc) counters, which
   drive -fprofile-use's block layout and inlining, are written normally by __gcov_dump. */
typedef long long gcov_type;
struct indirect_call_tuple { void* callee; gcov_type* counters; };
struct indirect_call_tuple __gcov_indirect_call;
gcov_type __gcov_time_profiler_counter;
void __gcov_indirect_call_profiler_v4(gcov_type value, void* cur_func) { (void)value; (void)cur_func; }
void __gcov_merge_time_profile(gcov_type* counters, unsigned n) { (void)counters; (void)n; }
void __gcov_interval_profiler(gcov_type* c, gcov_type v, int start, unsigned steps) { (void)c; (void)v; (void)start; (void)steps; }
void __gcov_pow2_profiler(gcov_type* c, gcov_type v) { (void)c; (void)v; }
void __gcov_topn_values_profiler(gcov_type* c, gcov_type v) { (void)c; (void)v; }
void __gcov_average_profiler(gcov_type* c, gcov_type v) { (void)c; (void)v; }
void __gcov_ior_profiler(gcov_type* c, gcov_type v) { (void)c; (void)v; }

/* vitasdk's libgcov is also built without file I/O, so __gcov_dump writes nothing. Instead: record every module's
   gcov_info as its constructor registers it, and serialise them with __gcov_info_to_gcda (GCC's freestanding
   interface) through plain stdio. */
}
#include <cstdio>
#include <cstdlib>
struct gcov_info;
extern "C" void __gcov_info_to_gcda(const gcov_info*, void (*)(const char*, void*), void (*)(const void*, unsigned, void*),
                                    void* (*)(unsigned, void*), void*);
static const gcov_info* g_infos[2048]; static int g_ninfo;
extern "C" void __gcov_init(const gcov_info* i) { if (g_ninfo < 2048) g_infos[g_ninfo++] = i; }
extern "C" void db_pgo_dump() {
    int ok = 0;
    for (int k = 0; k < g_ninfo; ++k) {
        FILE* f = nullptr;
        __gcov_info_to_gcda(g_infos[k],
            [](const char* name, void* arg) { *(FILE**)arg = std::fopen(name, "wb"); if (!*(FILE**)arg) std::printf("[pgo] cannot create %s\n", name); },
            [](const void* d, unsigned n, void* arg) { if (*(FILE**)arg) std::fwrite(d, 1, n, *(FILE**)arg); },
            [](unsigned n, void*) -> void* { return std::malloc(n); }, &f);
        if (f) { std::fclose(f); ok++; }
    }
    std::printf("[pgo] %d of %d modules written\n", ok, g_ninfo);
}
// vita-elf-create needs room after segment 0 for its import stubs; this build's layout ended just short of a page
__attribute__((used)) static const unsigned char db_seg0_pad[3072] = {1};
