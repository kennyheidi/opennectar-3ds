// Diagnostic tracing for the "frame counter froze" class of bugs. Linked with -Wl,--wrap=<fn> for the OS thread /
// message-queue / DVD / retrace calls. Each wrapper records an event in a lock-free ring buffer (all threads);
// the watchdog dumps the last N events when frames stop advancing, so the log shows which thread was blocked on
// what. Costs a few instructions per call; remove the --wrap flags in CMakeLists.txt to disable.
#include <stdarg.h>
#include <stddef.h>
#include "n3ds_bridge.h"
#include "Dolphin/os.h"
#include "Dolphin/dvd.h"

struct Ev { unsigned ms, tid; const char* what; unsigned long a, b; };
static Ev sRing[256];
static volatile unsigned sHead = 0;
static volatile unsigned sIdleLoops = 0;
/* Too hot for the ring (a spin loop would flood it): plain counters. Compare two dumps (t=10 s, t=70 s) to see whether
   a call is still being made during a stall, and whether OSGetTime is still advancing. */
static volatile unsigned sLockCalls = 0, sUnlockCalls = 0, sGetTimeCalls = 0;
static volatile unsigned sYieldCalls = 0, sSignalCalls = 0, sWakeupCalls = 0;   /* 1g: yield moved out of the ring (a spin loop flooded it) */
static volatile unsigned long sLastSignalCond = 0;
static volatile long long sLastGetTime = 0;

static void rec(const char* what, unsigned long a = 0, unsigned long b = 0)
{
    unsigned i = __sync_fetch_and_add(&sHead, 1) & 255;
    Ev e = { n3ds_time_ms(), n3ds_thread_id(), what, a, b };
    sRing[i] = e;
}

extern "C" void n3ds_trace_dump(int lastN)
{
    /* 1g: copy the ring first. Logging to the SD card is slow, and a running thread keeps overwriting the ring while
       we print (the 1f dump showed impossible negative ages for that reason). */
    static Ev snap[256];
    unsigned head = sHead;
    for (int k = 0; k < 256; ++k) snap[k] = sRing[k];
    unsigned n = head < 256 ? head : 256;
    if ((unsigned)lastN > n) lastN = (int)n;
    unsigned now = n3ds_time_ms();
    n3ds_log("[trace] last %d of %u events (oldest first; age = ms before now; T = thread id):", lastN, head);
    for (int k = lastN; k >= 1; --k) {
        const Ev& e = snap[(head - k) & 255];
        n3ds_log("[trace]  age %6dms T%08x %-22s %08lx %08lx", (int)(now - e.ms), e.tid, e.what, e.a, e.b);
    }
    /* Per-thread summary: the newest event of each thread id seen in the ring. A thread whose newest event is
       "... WAIT" / "OSJoinThread >>" / "OSWaitCond >>" is blocked there. */
    unsigned seen[12]; int ns = 0;
    n3ds_log("[trace] per-thread last event:");
    for (unsigned k = 1; k <= n && ns < 12; ++k) {
        const Ev& e = snap[(head - k) & 255];
        bool dup = false;
        for (int j = 0; j < ns; ++j) if (seen[j] == e.tid) { dup = true; break; }
        if (dup) continue;
        seen[ns++] = e.tid;
        n3ds_log("[trace]  T%08x last: %-22s %08lx %08lx  (%dms ago)", e.tid, e.what, e.a, e.b, (int)(now - e.ms));
    }
    n3ds_log("[trace] counters: OSLockMutex=%u OSUnlockMutex=%u OSGetTime=%u (last value %lld)",
             sLockCalls, sUnlockCalls, sGetTimeCalls, (long long)sLastGetTime);
    n3ds_log("[trace] counters: OSYieldThread=%u OSSignalCond=%u (last cond %08lx) OSWakeupThread=%u",
             sYieldCalls, sSignalCalls, sLastSignalCond, sWakeupCalls);
}

/* Called once per System::run loop iteration (Jac_Gsync is the first call in that loop; see n3ds_stubs_gen). */
extern "C" void n3ds_diag_loop(void)
{
    unsigned n = ++sIdleLoops;
    rec("loop", n);
    if (n <= 3 || n % 2000 == 0) n3ds_log("[loop %u] System::run iteration", n);
}

extern "C" {
BOOL __real_OSCreateThread(OSThread*, OSThreadStartFunction, void*, void*, u32, OSPriority, u16);
s32  __real_OSResumeThread(OSThread*);
BOOL __real_OSJoinThread(OSThread*, void**);
BOOL __real_OSSendMessage(OSMessageQueue*, OSMessage, s32);
BOOL __real_OSReceiveMessage(OSMessageQueue*, OSMessage*, s32);
void __real_OSWaitCond(OSCond*, OSMutex*);
s32  __real_DVDReadPrio(DVDFileInfo*, void*, s32, s32, s32);
void __real_VIWaitForRetrace(void);
/* Added in 1f. Weak + void* params: these are only ever forwarded, and a function upstream does not define
   must not break the link (the wrapper is then simply never called). */
void      __real_OSSignalCond(void*)   __attribute__((weak));
void      __real_OSLockMutex(void*)    __attribute__((weak));
void      __real_OSUnlockMutex(void*)  __attribute__((weak));
void      __real_OSYieldThread(void)   __attribute__((weak));
void      __real_OSSleepThread(void*)  __attribute__((weak));
void      __real_OSWakeupThread(void*) __attribute__((weak));
long long __real_OSGetTime(void)       __attribute__((weak));

BOOL __wrap_OSCreateThread(OSThread* t, OSThreadStartFunction f, void* p, void* stk, u32 sz, OSPriority pr, u16 at)
{
    rec("OSCreateThread", (unsigned long)f, sz);
    n3ds_log("[os] OSCreateThread func=%p stack=%u prio=%d attr=%u (game-requested; real stack = libctru pthread default)",
             (void*)f, (unsigned)sz, (int)pr, (unsigned)at);
    return __real_OSCreateThread(t, f, p, stk, sz, pr, at);
}
s32 __wrap_OSResumeThread(OSThread* t)
{
    rec("OSResumeThread", (unsigned long)t);
    return __real_OSResumeThread(t);
}
BOOL __wrap_OSJoinThread(OSThread* t, void** v)
{
    rec("OSJoinThread >>", (unsigned long)t);
    BOOL r = __real_OSJoinThread(t, v);
    rec("OSJoinThread <<", (unsigned long)t);
    return r;
}
BOOL __wrap_OSSendMessage(OSMessageQueue* q, OSMessage m, s32 flags)
{
    rec("OSSendMessage", (unsigned long)q, (unsigned long)m);
    return __real_OSSendMessage(q, m, flags);
}
BOOL __wrap_OSReceiveMessage(OSMessageQueue* q, OSMessage* m, s32 flags)
{
    bool willWait = flags == OS_MESSAGE_BLOCK && q->usedCount == 0;
    rec(willWait ? "OSReceiveMessage WAIT" : "OSReceiveMessage", (unsigned long)q);
    BOOL r = __real_OSReceiveMessage(q, m, flags);
    if (willWait) rec("OSReceiveMessage GOT", (unsigned long)q, m ? (unsigned long)*m : 0);
    return r;
}
void __wrap_OSWaitCond(OSCond* c, OSMutex* m)
{
    rec("OSWaitCond >>", (unsigned long)c);
    __real_OSWaitCond(c, m);
    rec("OSWaitCond <<", (unsigned long)c);
}
s32 __wrap_DVDReadPrio(DVDFileInfo* fi, void* addr, s32 len, s32 off, s32 prio)
{
    rec("DVDReadPrio", (unsigned long)len, (unsigned long)off);
    return __real_DVDReadPrio(fi, addr, len, off, prio);
}
void __wrap_VIWaitForRetrace(void)
{
    rec("VIWaitForRetrace");
    __real_VIWaitForRetrace();
}

void __wrap_OSSignalCond(void* c)
{
    __sync_fetch_and_add(&sSignalCalls, 1);
    sLastSignalCond = (unsigned long)c;
    rec("OSSignalCond", (unsigned long)c);
    if (__real_OSSignalCond) __real_OSSignalCond(c);
}
void __wrap_OSLockMutex(void* m)
{
    __sync_fetch_and_add(&sLockCalls, 1);
    if (__real_OSLockMutex) __real_OSLockMutex(m);
}
void __wrap_OSUnlockMutex(void* m)
{
    __sync_fetch_and_add(&sUnlockCalls, 1);
    if (__real_OSUnlockMutex) __real_OSUnlockMutex(m);
}
void __wrap_OSYieldThread(void)
{
    __sync_fetch_and_add(&sYieldCalls, 1);
    n3ds_yield(); /* not __real_OSYieldThread: a plain yield starves the woken card thread on Horizon (see n3ds_ctru_io.cpp) */
}
void __wrap_OSSleepThread(void* q)
{
    rec("OSSleepThread >>", (unsigned long)q);
    if (__real_OSSleepThread) __real_OSSleepThread(q);
    rec("OSSleepThread <<", (unsigned long)q);
}
void __wrap_OSWakeupThread(void* q)
{
    __sync_fetch_and_add(&sWakeupCalls, 1);
    rec("OSWakeupThread", (unsigned long)q);
    if (__real_OSWakeupThread) __real_OSWakeupThread(q);
}
long long __wrap_OSGetTime(void)
{
    long long t = __real_OSGetTime ? __real_OSGetTime() : 0;
    __sync_fetch_and_add(&sGetTimeCalls, 1);
    sLastGetTime = t;
    return t;
}
} // extern "C"

/* ---- Optional call trace (CMake -DN3DS_TRACE_CALLS=ON compiles the game with -finstrument-functions) -------------
   Each thread that runs instrumented game code gets a SHADOW STACK: enter pushes the function address, exit pops.
   On a stall the watchdog dumps every thread's stack, i.e. the full call chain (System::run -> ... -> spinner), and
   tools/resolve_calls.sh turns the addresses into function names. (1f: replaces the 1e "last N events" ring, which
   in a tight poll loop is just one function entering and exiting over and over.)
   Reading it: frame #0 is the outermost call. For a thread spinning on a leaf, the leaf itself is on the stack only
   while it is between enter and exit, so the innermost frame you see is the loop's caller if the dump caught it
   between calls. Deliberately not instrumented itself. Racy by design (read while the thread runs); good enough. */
#define N3DS_SHADOW_THREADS 16
#define N3DS_SHADOW_DEPTH   48
struct Shadow { volatile unsigned tid; volatile int depth; volatile unsigned fn[N3DS_SHADOW_DEPTH]; };
static Shadow sShadow[N3DS_SHADOW_THREADS];
static volatile unsigned sCallCount = 0;

__attribute__((no_instrument_function)) static Shadow* shadowFor(unsigned tid)
{
    if (tid == 0) tid = 1; /* 0 marks a free slot */
    for (int i = 0; i < N3DS_SHADOW_THREADS; ++i) {
        unsigned cur = sShadow[i].tid;
        if (cur == tid) return &sShadow[i];
        if (cur == 0 && __sync_bool_compare_and_swap(&sShadow[i].tid, 0u, tid)) return &sShadow[i];
        if (sShadow[i].tid == tid) return &sShadow[i]; /* lost the race to the same thread id (cannot happen, but cheap) */
    }
    return &sShadow[N3DS_SHADOW_THREADS - 1]; /* out of slots (threads come and go, ids are never freed): share the last */
}

extern "C" {
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void* fn, void*)
{
    Shadow* s = shadowFor(n3ds_thread_id());
    int d = s->depth;
    if (d >= 0 && d < N3DS_SHADOW_DEPTH) s->fn[d] = (unsigned)(unsigned long)fn & ~1u;
    s->depth = d + 1;
    ++sCallCount;
}
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void*, void*)
{
    Shadow* s = shadowFor(n3ds_thread_id());
    if (s->depth > 0) s->depth = s->depth - 1;
}
}

/* lastN is unused now (kept so n3ds_bridge.h and the watchdog call stay unchanged). */
extern "C" void n3ds_calls_dump(int)
{
    if (sCallCount == 0) { n3ds_log("[stack] (call trace not enabled: rebuild with -DN3DS_TRACE_CALLS=ON)"); return; }
    n3ds_log("[stack] begin: %u function entries so far", sCallCount);
    for (int i = 0; i < N3DS_SHADOW_THREADS; ++i) {
        unsigned tid = sShadow[i].tid;
        if (!tid) continue;
        int depth = sShadow[i].depth;
        int n = depth < N3DS_SHADOW_DEPTH ? depth : N3DS_SHADOW_DEPTH;
        if (n < 0) n = 0;
        n3ds_log("[stack] T%08x depth=%d", tid, depth);
        for (int d = 0; d < n; ++d) n3ds_log("[stack]   #%d %08x", d, sShadow[i].fn[d]);
    }
    n3ds_log("[stack] end");
}
