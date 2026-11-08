// ctru side of the two-thread renderer (queue + replay live in n3ds_gfx.cpp): the worker thread, linear memory and sleep.
// The worker replays GX commands, assembles vertices, drives citro3d and presents; the game thread only records them.
#include <3ds.h>
#include <stdlib.h>
#include "n3ds_bridge.h"

static volatile bool sStop = false;
static Thread sWorker;

static void workerMain(void*)
{
    /* R1 (n3ds_port/AGENTS.md): FPSCR is per-thread, so every thread that does float work sets flush-to-zero (FZ) + default-NaN (DN)
       at its entry, before any float work: decay can reach denormals, FZ|DN is the chosen numerical mode, and each thread's state
       must be established. Current libctru already does this in initThreadVars() for main and threadCreate() threads; the call is
       the defensive, version-independent guarantee (it also turns the VFP exception traps off). */
    n3ds_vfp_runfast();
    n3ds_log("[gfxq] worker running on core %d", (int)svcGetProcessorID());
    while (!sStop) {
        if (!n3ds_gfxq_drain()) svcSleepThread(100000);      // idle: poll every 0.1 ms (core 2 has nothing else to do)
    }
}

extern "C" void* n3ds_linear_alloc(unsigned bytes) { return linearAlloc(bytes); }
extern "C" void n3ds_linear_free(void* p) { if (p) linearFree(p); }
extern "C" void n3ds_flush_dcache(const void* p, unsigned bytes) { GSPGPU_FlushDataCache(p, bytes); }
extern "C" unsigned n3ds_linear_free_kb(void) { return (unsigned)(linearSpaceFree() / 1024); }
extern "C" void n3ds_sleep_ns(long long ns) { svcSleepThread(ns); }

extern "C" void n3ds_worker_stop(void);
extern "C" int n3ds_worker_start(void)
{
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    if (!isNew) return 0;                                    // core 2 only exists on New 3DS; the old model stays single-threaded
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    sStop = false;
    sWorker = threadCreate(workerMain, nullptr, 96 * 1024, prio, 2, false);
    if (sWorker) atexit(n3ds_worker_stop);                   // safety net: the worker must be gone before the app tears down linear memory / its stack
    return sWorker ? 1 : 0;
}

// Ask the worker to leave its loop and wait for it. Idempotent. The 1i-era exit crashes (Luma dumps 28/29) were the worker still
// polling the queue on core 2 after the process had started unmapping memory.
extern "C" void n3ds_worker_stop(void)
{
    if (!sWorker) return;
    sStop = true;
    threadJoin(sWorker, 2000000000ULL);                      // up to 2 s; the loop only needs to finish its current command
    threadFree(sWorker);
    sWorker = nullptr;
}
