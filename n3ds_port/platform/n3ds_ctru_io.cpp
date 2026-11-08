// libctru-side helpers (input, APT, frame present). No game headers here -- see n3ds_bridge.h.
#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include <exception>
#include <new>
#include <stdlib.h>
#include "n3ds_bridge.h"

extern "C" void n3ds_dbgmenu_touch(void);   /* n3ds_dbgmenu.cpp */
extern volatile int gN3dsEnvDbg;            /* n3ds_gfx.cpp (r39 test modes) */
extern "C" void n3ds_log(const char* fmt, ...);
extern "C" void n3ds_input_poll(N3dsInput* o)
{
    hidScanInput();
    n3ds_dbgmenu_touch();
    u32 h = hidKeysHeld();
    {   /* r40 test switch: hold L + R and press D-pad RIGHT to cycle the face-culling test modes (n3ds_gfx.cpp gN3dsEnvDbg) */
        const u32 d = hidKeysDown();
        if ((h & KEY_L) && (h & KEY_R) && (d & KEY_DRIGHT)) {
            gN3dsEnvDbg = (gN3dsEnvDbg + 1) % 7;
            static const char* const nm[7] = { "normal (face culling as nectar.ini says)", "face culling OFF", "face culling with the OPPOSITE winding", "sphere-mapped draws HIDDEN", "ONLY sphere-mapped draws shown", "BLACK untextured lit materials hidden", "ALL untextured lit materials hidden" };
            n3ds_log("[dbg] L+R+RIGHT: test mode %d = %s", gN3dsEnvDbg, nm[gN3dsEnvDbg]);
        }
    }
    unsigned b = 0;
    if (h & KEY_A) b |= N3DS_BTN_A;         if (h & KEY_B) b |= N3DS_BTN_B;
    if (h & KEY_X) b |= N3DS_BTN_X;         if (h & KEY_Y) b |= N3DS_BTN_Y;
    if (h & KEY_START) b |= N3DS_BTN_START; if (h & KEY_SELECT) b |= N3DS_BTN_SELECT;
    if (h & KEY_DUP) b |= N3DS_BTN_UP;      if (h & KEY_DDOWN) b |= N3DS_BTN_DOWN;
    if (h & KEY_DLEFT) b |= N3DS_BTN_LEFT;  if (h & KEY_DRIGHT) b |= N3DS_BTN_RIGHT;
    if (h & KEY_L) b |= N3DS_BTN_L;         if (h & KEY_R) b |= N3DS_BTN_R;
    if (h & KEY_ZL) b |= N3DS_BTN_ZL;       if (h & KEY_ZR) b |= N3DS_BTN_ZR;
    o->buttons = b;
    circlePosition cp = {0, 0}, cs = {0, 0};
    hidCircleRead(&cp);
    hidCstickRead(&cs);
    o->stickX = cp.dx;  o->stickY = cp.dy;
    o->cstickX = cs.dx; o->cstickY = cs.dy;
}

/* HOME-menu diagnostics: log every APT hook (suspend / restore / sleep / wake / exit) with the thread and tick so a log taken after
   "HOME and back = game does not advance" shows whether the restore hook ran and where the game stopped. Registered once, after citro3d's own hook. */
static aptHookCookie sAptCookie;
static bool sAptHooked = false;
static volatile unsigned sAptHooks = 0;
static void aptDiagHook(APT_HookType t, void*)
{
    static const char* const nm[] = { "ONSUSPEND", "ONRESTORE", "ONSLEEP", "ONWAKEUP", "ONEXIT" };
    ++sAptHooks;
    n3ds_log("[apt] %s (thread %u, tick %llu)", (unsigned)t < 5 ? nm[(unsigned)t] : "?", (unsigned)(uintptr_t)threadGetCurrent(), (unsigned long long)svcGetSystemTick());
}
// Wraps the terminate handler installed by n3ds_main: logs WHICH exception was in flight (bad_alloc = out of memory, ...), then chains.
static std::terminate_handler sPrevTerminate = NULL;
static void terminateWhy(void)
{
    try {
        std::exception_ptr ep = std::current_exception();
        if (ep) std::rethrow_exception(ep);
        n3ds_log("[n3ds] terminate: no active exception (noexcept violation / direct std::terminate)");
    } catch (const std::bad_alloc& e) {
        n3ds_log("[n3ds] terminate: std::bad_alloc (%s) -- out of memory", e.what());
    } catch (const std::exception& e) {
        n3ds_log("[n3ds] terminate: std::exception: %s", e.what());
    } catch (...) {
        n3ds_log("[n3ds] terminate: unknown exception type");
    }
    if (sPrevTerminate) sPrevTerminate();
    abort();
}

extern "C" int n3ds_alive(void)
{
    if (!sAptHooked) { sAptHooked = true; aptHook(&sAptCookie, aptDiagHook, NULL); sPrevTerminate = std::set_terminate(terminateWhy); }
    const unsigned h0 = sAptHooks;
    const unsigned long long t0 = svcGetSystemTick();
    const int r = aptMainLoop() ? 1 : 0;
    const unsigned long long dt = svcGetSystemTick() - t0;
    static int resumeLogs = 0;
    if (dt > SYSCLOCK_ARM11 / 5 || sAptHooks != h0 || resumeLogs > 0) {          /* slow aptMainLoop = the HOME menu / sleep happened */
        if (dt > SYSCLOCK_ARM11 / 5 || sAptHooks != h0) resumeLogs = 6;
        n3ds_log("[apt] aptMainLoop returned %d after %llu ms, hooks run during it: %u", r, dt * 1000ULL / SYSCLOCK_ARM11, sAptHooks - h0);
        --resumeLogs;
    }
    return r;
}

extern "C" void n3ds_present_heartbeat(unsigned frame)
{
    u16 w, h; /* top screen framebuffer: 240x400, column-major, BGR8 */
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    if (fb) {
        memset(fb, 0x18, (size_t)w * h * 3);
        unsigned col = (frame * 3) % 400;
        for (unsigned x = col; x < col + 8 && x < 400; ++x)
            for (unsigned y = 0; y < 240; ++y) {
                u8* px = fb + (x * 240 + y) * 3;
                px[0] = (u8)(frame * 5); px[1] = 0xC0; px[2] = 0x40;
            }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

extern "C" unsigned long long n3ds_ticks(void) { return svcGetSystemTick(); }
extern "C" unsigned n3ds_time_ms(void) { return (unsigned)osGetTime(); }
extern "C" unsigned n3ds_thread_id(void) { return (unsigned)(uintptr_t)threadGetCurrent(); }

/* OSYieldThread is used by the game as a poll-and-yield wait (log 1g: 73 M calls, the woken card thread never ran).
   On Horizon, sched_yield / svcSleepThread(0) only rotates same-priority threads that are already runnable and does not
   time-slice, so a woken worker can starve behind a polling thread. A real (short) sleep always hands the core over. */
static volatile unsigned sYieldCalls = 0;
static volatile unsigned sYieldMs10 = 0;       /* wall time spent inside n3ds_yield, in 0.1 ms units (all threads together) */
extern "C" void n3ds_yield(void)
{
    const u64 t0 = svcGetSystemTick();
    svcSleepThread(100000LL);                  /* 100 us; TUNE if polling latency matters */
    sYieldMs10 += (unsigned)((svcGetSystemTick() - t0) * 10000ULL / SYSCLOCK_ARM11);
    ++sYieldCalls;
}
extern "C" void n3ds_vfp_runfast(void)
{
#if defined(__arm__)
    unsigned v;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(v));
    v |= (1u << 24) | (1u << 25);   // FZ | DN
    v &= ~(0x1Fu << 8);             // exception trap enables off
    __asm__ volatile("vmsr fpscr, %0" : : "r"(v));
#endif
}
extern "C" void n3ds_yield_take(unsigned* calls, unsigned* ms10) { *calls = sYieldCalls; *ms10 = sYieldMs10; sYieldCalls = 0; sYieldMs10 = 0; }

/* ---- crash logger -----------------------------------------------------------------------------------------------------
   A Luma crash dump for this app always ends in svcBreak and says nothing about WHY. Every svcBreak in libctru/newlib is routed here
   (link flag -Wl,--wrap=svcBreak), so before the process dies we write the reason, the caller, the heap state and the code addresses
   found on the crashing thread's stack to the log (resolve them with: arm-none-eabi-addr2line -Cfpe nectar_3ds.elf <addrs>). */
extern "C" void __real_svcBreak(UserBreakType reason);
extern "C" void __wrap_svcBreak(UserBreakType reason)
{
    static volatile int inFatal = 0;
    if (!inFatal) {
        inFatal = 1;
        u32 sp;
        __asm__ volatile("mov %0, sp" : "=r"(sp));
        static const char* const names[] = {"PANIC", "ASSERT", "USER", "LOADRO", "?"};
        const unsigned ri = (unsigned)reason < 4 ? (unsigned)reason : 4;
        n3ds_log("[FATAL] svcBreak(%s) called from lr=%p on thread %p, sp=%08x", names[ri], __builtin_return_address(0), (void*)threadGetCurrent(), (unsigned)sp);
        n3ds_log_memory("at svcBreak");
        // code addresses on this thread's stack (bounded by the memory block that contains sp, at most 3 KB)
        MemInfo mi; PageInfo pi;
        u32 end = sp + 256;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, sp))) { end = mi.base_addr + mi.size; if (end > sp + 3072) end = sp + 3072; }
        char line[200]; int n = 0; line[0] = 0;
        int found = 0;
        for (u32 a = sp & ~3u; a + 4 <= end && found < 40; a += 4) {
            u32 v = *(volatile u32*)a;
            if (v >= 0x00100000 && v < 0x00500000 && (v & 3) == 0) {   // inside the app's code (0x100000 up), word aligned
                n += snprintf(line + n, sizeof line - n, " %06x", (unsigned)v);
                ++found;
                if (found % 8 == 0) { n3ds_log("[FATAL] stack code addrs:%s", line); n = 0; line[0] = 0; }
            }
        }
        if (n) n3ds_log("[FATAL] stack code addrs:%s", line);
        n3ds_log("[FATAL] end of crash report (the Luma dump follows)");
    }
    __real_svcBreak(reason);
}

// ---- [dvdprof]: time spent in fopen vs seek+fread of the DVD layer (see the DVD patch in CMakeLists.txt) --------------------------------------
extern "C" void n3ds_pak_stats(unsigned* hits, unsigned* misses);
extern "C" void n3ds_pak_verify_stats(unsigned* reads, unsigned* kb, unsigned* bad);
extern "C" void n3ds_dvdprof(int kind, unsigned long long t0, unsigned bytes)
{
    static unsigned long long tk[2] = {0, 0}, mx[2] = {0, 0}, bytesTot = 0, winStart = 0;
    static unsigned n[2] = {0, 0};
    const unsigned long long now = svcGetSystemTick(), d = now - t0;
    if (!winStart) winStart = t0;
    tk[kind] += d; ++n[kind]; if (d > mx[kind]) mx[kind] = d; bytesTot += bytes;
    if (n[0] + n[1] < 100) return;
    const unsigned long long ms = SYSCLOCK_ARM11 / 1000ULL;
    unsigned pakHits = 0, pakMisses = 0; n3ds_pak_stats(&pakHits, &pakMisses);
    { unsigned vr = 0, vk = 0, vb = 0; n3ds_pak_verify_stats(&vr, &vk, &vb); if (vr) n3ds_log("[pak] verify so far: %u reads, %u KB compared, %u mismatches", vr, vk, vb); }
    n3ds_log("[dvdprof] %u opens = %u ms (max %u ms) | %u reads = %u ms (max %u ms, %u KB) | window wall %u ms | nectar.pak since boot: %u from pak, %u loose",
             n[0], (unsigned)(tk[0] / ms), (unsigned)(mx[0] / ms), n[1], (unsigned)(tk[1] / ms), (unsigned)(mx[1] / ms),
             (unsigned)(bytesTot / 1024), (unsigned)((now - winStart) / ms), pakHits, pakMisses);
    tk[0] = tk[1] = mx[0] = mx[1] = 0; n[0] = n[1] = 0; bytesTot = 0; winStart = now;
}
