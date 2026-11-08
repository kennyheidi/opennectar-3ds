// One-shot CPU / memory probe (ctru side). Answers "is this really running at New 3DS speed, and where does bandwidth go?"
// Reference (New 3DS, 804 MHz): int loop ~ 250-400 Mops/s, dependent float chain ~ 40-100 Mops/s, heap memcpy ~ 100-200 MB/s.
// Old-3DS-class numbers (about 3x lower) would mean the speed-up mode is NOT active.
#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include "n3ds_bridge.h"

static double ms(u64 t0) { return (double)(svcGetSystemTick() - t0) / N3DS_TICKS_PER_MS; }

extern "C" void n3ds_bench(void)
{
    bool nn = false; APT_CheckNew3DS(&nn);
    n3ds_log("[bench] new3DS=%d", (int)nn);
    volatile unsigned sink = 0; u64 t = svcGetSystemTick();
    unsigned a = 1; for (unsigned i = 0; i < 20000000u; ++i) { a = a * 1664525u + 1013904223u; } sink = a;
    double t1 = ms(t); n3ds_log("[bench] int LCG 20M iters: %.0f ms = %.0f Miter/s", t1, 20000.0 / t1);
    t = svcGetSystemTick(); volatile float f = 1.0f; float x = 1.0001f;
    for (unsigned i = 0; i < 4000000u; ++i) { x = x * 1.0000001f + 0.0000001f; } f = x; (void)f;
    t1 = ms(t); n3ds_log("[bench] float dependent mul-add 4M: %.0f ms = %.1f Mop/s", t1, 4000.0 / t1);
    const size_t N = 2u << 20; char* p = (char*)malloc(N * 2);
    if (p) {
        memset(p, 1, N * 2); t = svcGetSystemTick();
        for (int i = 0; i < 16; ++i) memcpy(p + N, p, N);
        t1 = ms(t); n3ds_log("[bench] heap memcpy 2MB x16: %.0f ms = %.0f MB/s", t1, 32.0 * 1000.0 / t1); free(p);
    }
    void* l = linearAlloc(N * 2);
    if (l) {
        memset(l, 1, N * 2); t = svcGetSystemTick();
        for (int i = 0; i < 16; ++i) memcpy((char*)l + N, l, N);
        t1 = ms(t); n3ds_log("[bench] linear memcpy 2MB x16: %.0f ms = %.0f MB/s", t1, 32.0 * 1000.0 / t1); linearFree(l);
    }
    (void)sink;
}
