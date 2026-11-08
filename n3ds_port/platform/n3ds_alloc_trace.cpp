// Link with -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free.
// 1) Logs every block >= 2 MB and EVERY failed block of any size, with the caller's return address. Resolve on the PC:
//      arm-none-eabi-addr2line -Cfe nectar_3ds.elf 0x001xxxxx      (3dsx loads at the ELF's link address)
// 2) LINEAR-HEAP FALLBACK: when the normal malloc heap is exhausted (it is nearly always full on the 3DS: ~0.5-1 MB free
//    during play) the request is served from the linear heap instead, which has tens of MB free in the 178 MB mode.
//    Such blocks carry a 32-byte header and are recognised by address (linear heap lives at >= 0x10000000, the malloc heap
//    below it), so free()/realloc() route them back correctly. The game's audio mixer relies on this for its sample cache.
#include <3ds.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "n3ds_bridge.h"

extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);
void __real_free(void*);
void* __real_memalign(size_t, size_t);

static const uintptr_t kLinearBase = 0x10000000u;
static const size_t kHdr = 32;
static const uint32_t kMagic = 0x4C4E4B31u;      // "LNK1"
static LightLock sLinLock;
static bool sLinInit = false;
static unsigned sLinCount = 0;
static size_t sLinBytes = 0, sLinPeak = 0;

static inline bool isLinear(const void* p) { return (uintptr_t)p >= kLinearBase; }

static void* linearFallback(size_t n)
{
    if (!sLinInit) { LightLock_Init(&sLinLock); sLinInit = true; }
    LightLock_Lock(&sLinLock);
    unsigned char* b = (unsigned char*)linearMemAlign(n + kHdr, 32);
    void* out = nullptr;
    if (b) {
        ((uint32_t*)b)[0] = kMagic; ((uint32_t*)b)[1] = (uint32_t)n;
        out = b + kHdr;
        ++sLinCount; sLinBytes += n; if (sLinBytes > sLinPeak) sLinPeak = sLinBytes;
    }
    LightLock_Unlock(&sLinLock);
    return out;
}

static void linearRelease(void* p)
{
    unsigned char* b = (unsigned char*)p - kHdr;
    if (((uint32_t*)b)[0] != kMagic) return;     // not ours: leak rather than corrupt
    const size_t n = ((uint32_t*)b)[1];
    ((uint32_t*)b)[0] = 0;
    LightLock_Lock(&sLinLock);
    sLinBytes -= n;
    linearFree(b);
    LightLock_Unlock(&sLinLock);
}

static volatile unsigned sResets = 0, sWalks = 0;   // diagnostics: failing walks of the free list, fast-fail resets
static bool sInTrace = false;
static void trace(const char* what, size_t bytes, void* result, void* caller, bool viaLinear)
{
    if (sInTrace) return;                          // printf may malloc: don't recurse
    if (result && !viaLinear && bytes < (2u << 20)) return;      // quiet unless big, failed or redirected
    sInTrace = true;
    /* mallinfo() walks the WHOLE newlib free list. It used to run before every logged-or-not linear fallback: with ~1.4 MB of free heap in tiny
       fragments that is ~10 ms per call, and a screen-building burst (end-of-day results / save screens: thousands of small blocks) made the game thread
       spend ~100 allocations per second = no frame for a minute (log 1ece79ae: frame counter stuck at 23848 while the UI .blo files kept loading). */
    struct mallinfo mi; memset(&mi, 0, sizeof mi);
    if (!viaLinear) mi = mallinfo();
    if (viaLinear) {
        static unsigned logged = 0;
        if (logged < 12 || (sLinCount & 255) == 0) { ++logged;
            n3ds_log("[alloc] %s %lu bytes served from the LINEAR heap (malloc heap full) | caller %p | linear-fallback blocks %u, %lu KB (peak %lu KB) | failing heap walks %u, fast-fail resets %u so far",
                     what, (unsigned long)bytes, caller, sLinCount, (unsigned long)(sLinBytes / 1024), (unsigned long)(sLinPeak / 1024), sWalks, sResets); }
    } else {
        n3ds_log("[alloc] %s %lu bytes (%.1f MB) %s | caller %p | heap in-use %d KB, free %d KB",
                 what, (unsigned long)bytes, bytes / 1048576.0, result ? "ok" : "FAILED", caller,
                 mi.uordblks / 1024, mi.fordblks / 1024);
    }
    sInTrace = false;
}

// Fast-fail: when the malloc heap is full, newlib walks its whole (fragmented) free list on EVERY failing request before we fall back to the
// linear heap. A level load makes thousands of such requests (the log showed ~55 s of loading). So remember the smallest size that failed and send
// anything at least that big straight to the linear heap until a block that large is freed (or every 256 frees, since neighbours may have merged).
static volatile size_t sFailMin = (size_t)-1;
static volatile unsigned sFreeCount = 0;

// Diagnostic: how much wall time is spent INSIDE malloc/calloc/free (a fragmented newlib heap makes every call walk a long free list). Logged every
// 8192 calls as "[allocprof]". Compare "inside malloc/free" with the wall time to see whether allocation is what makes level loads slow.
int n3ds_allocprof_on = 0;   // set from nectar.ini alloc_prof (n3ds_config.cpp)
static uint64_t sPStart = 0, sPTicks = 0, sPMax = 0;
static unsigned sPN = 0;
static bool sInProf = false;
static inline void profDone(uint64_t t0)
{
    if (!t0) return;
    const uint64_t now = svcGetSystemTick(), d = now - t0;
    sPTicks += d; if (d > sPMax) sPMax = d;
    if (++sPN < 8192 || sInProf) return;
    sInProf = true;
    if (sPStart) {
        const unsigned wall = (unsigned)((now - sPStart) * 1000ULL / SYSCLOCK_ARM11);
        n3ds_log("[allocprof] %u alloc calls in %u ms wall: %u ms inside malloc/calloc/free (avg %u us, max %u ms)",
                 sPN, wall, (unsigned)(sPTicks * 1000ULL / SYSCLOCK_ARM11), (unsigned)(sPTicks * 1000000ULL / SYSCLOCK_ARM11 / sPN), (unsigned)(sPMax * 1000ULL / SYSCLOCK_ARM11));
    }
    sPStart = now; sPTicks = 0; sPMax = 0; sPN = 0;
    sInProf = false;
}

void* __wrap_malloc(size_t n)
{
    const uint64_t t0 = n3ds_allocprof_on ? svcGetSystemTick() : 0;
    void* p = (n >= sFailMin) ? nullptr : __real_malloc(n);
    bool lin = false;
    if (!p && n) { if (n < sFailMin) { sFailMin = n; ++sWalks; } p = linearFallback(n); lin = p != nullptr; }
    trace("malloc", n, p, __builtin_return_address(0), lin);
    profDone(t0);
    return p;
}
void* __wrap_calloc(size_t n, size_t sz)
{
    const uint64_t t0 = n3ds_allocprof_on ? svcGetSystemTick() : 0;
    void* p = (n && sz && n * sz >= sFailMin) ? nullptr : __real_calloc(n, sz);
    bool lin = false;
    if (!p && n && sz) { if (n * sz < sFailMin) { sFailMin = n * sz; ++sWalks; } p = linearFallback(n * sz); if (p) { memset(p, 0, n * sz); lin = true; } }
    trace("calloc", n * sz, p, __builtin_return_address(0), lin);
    profDone(t0);
    return p;
}
void* __wrap_realloc(void* old, size_t n)
{
    if (old && isLinear(old)) {                    // a fallback block: move it (size is in the header)
        const size_t oldN = ((uint32_t*)((unsigned char*)old - kHdr))[1];
        if (n <= oldN) return old;
        void* p = __wrap_malloc(n);
        if (p) { memcpy(p, old, oldN); linearRelease(old); }
        return p;
    }
    void* p = __real_realloc(old, n);
    if (!p && n) {                                 // heap full: move the block into linear memory
        void* q = linearFallback(n);
        if (q) {
            if (old) { const size_t oldN = malloc_usable_size(old); memcpy(q, old, oldN < n ? oldN : n); __real_free(old); }
            trace("realloc", n, q, __builtin_return_address(0), true);
            return q;
        }
    }
    return p;
}
// memalign() is what libctru's threadCreate() uses for a thread's stack+TLS block. With the malloc heap full it returned NULL, pthread_create failed
// ("Not enough space") and the game died / hung when it asked for a thread (log d25f9b2b, frame 705). Small blocks fall back to the linear heap
// like malloc does (our 32-byte header keeps the result 32-byte aligned); big ones (e.g. the GC arena) keep the old NULL so callers can shrink.
void* __wrap_memalign(size_t align, size_t n)
{
    void* p = __real_memalign(align, n);
    if (!p && n && n <= (1u << 20) && align <= kHdr) {
        p = linearFallback(n);
        trace("memalign", n, p, __builtin_return_address(0), p != nullptr);
    }
    return p;
}
void __wrap_free(void* p)
{
    if (!p) return;
    const uint64_t t0 = n3ds_allocprof_on ? svcGetSystemTick() : 0;
    if (isLinear(p)) { linearRelease(p); return; }
    if (sFailMin != (size_t)-1) {
        /* Forget "requests of this size fail" only when a big hole opened (>= 16 KB and big enough for the failing size) or every 4096th free. It used
           to reset on ANY free of a block >= the failing size; a UI burst frees small blocks constantly, so nearly every allocation after that paid a full
           failing walk of the fragmented free list (~10 ms) again. */
        const size_t us = malloc_usable_size(p);
        if ((us >= sFailMin && us >= 16384) || (++sFreeCount & 4095) == 0) { sFailMin = (size_t)-1; ++sResets; }
    }
    __real_free(p);
    profDone(t0);
}
}
