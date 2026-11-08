// New 3DS entry point. Mirrors upstream pc_port/pc_main.cpp (Initialise -> NodeMgr -> gsys->run) but
// without SDL/GL. The game runs on its own thread so we control its stack size; the main thread only
// waits. Everything printed to stdout appears on the bottom screen; stderr + n3ds_log go to
// sdmc:/3ds/nectar/log.txt (flushed every line, so a crash still leaves the last message).
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <exception>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include "n3ds_bridge.h"
#include "n3ds_config.h"

#define N3DS_HOME       "sdmc:/3ds/nectar"
#define N3DS_ASSETS     N3DS_HOME "/assets" /* the game reads assets/<path> relative to N3DS_HOME (= cwd) */
/* Memory split: default = libctru's own (124 MB mode: malloc heap ~88 MB, linear ~32 MB).
   DO NOT shrink linear carelessly. The process heap lives in a fixed 96 MB virtual window
   (OS_HEAP_AREA 0x08000000-0x0E000000); if heap = remaining - linear exceeds it, svcControlMemory fails inside
   libctru's startup and it calls svcBreak(PANIC) before main (Luma "crash_dump", process 3dsx_app). That is
   exactly what a 12 MB linear override did here (heap 0x06BFC000 = 108 MB). Rule for an override:
       linear >= (app region - code) - 96 MB      (~28 MB in 124 MB mode)
   Set -DN3DS_LINEAR_HEAP_MB=<n> only if you have checked that; 0 keeps libctru's default. */
#ifndef N3DS_LINEAR_HEAP_MB
#define N3DS_LINEAR_HEAP_MB 0
#endif
#if N3DS_LINEAR_HEAP_MB > 0
extern "C" { u32 __ctru_linear_heap_size = (u32)N3DS_LINEAR_HEAP_MB << 20; }
#endif
extern "C" { extern u32 __ctru_heap_size, __ctru_linear_heap_size; } /* filled in by libctru before main */

/* Below this the app region is not the New 3DS 124 MB mode (64 MB mode reports ~64 MB; 124 MB mode ~124 MB). */
#ifndef N3DS_MIN_APP_REGION_MB
#define N3DS_MIN_APP_REGION_MB 100
#endif

#define GAME_STACK_SIZE (1024 * 1024) /* TUNE: decomp stack use is unknown; check the log for stack faults */

static FILE* sLog = NULL;
static FILE* sGameLog = NULL;   /* r37: the game thread's stdout/stderr: a SEPARATE FILE on log.txt (both opened for append). Sharing one FILE between the
                                   game's unlocked printf and the worker's n3ds_log corrupted lines (log__14/15: "ffff[PC Port] Textures..." with the rest lost). */
volatile unsigned n3ds_frame_count = 0;   /* bumped by pc_window_swap_buffers (once per presented frame) */
static char sLastNote[160] = "(nothing yet)";
extern "C" void n3ds_note(const char* s) { strncpy(sLastNote, s, sizeof(sLastNote) - 1); sLastNote[sizeof(sLastNote) - 1] = 0; }
extern "C" void n3ds_frame_tick(unsigned f) { n3ds_frame_count = f; }
static bool sCleanShutdown = false; /* set right before gfxExit: no logging/printf after the console is gone */

extern volatile int n3ds_dbgmenu_open;   /* n3ds_dbgmenu.cpp */
extern "C" int n3ds_bottom_map_shown(void);   /* n3ds_dbgmenu.cpp: the game's map covers the console */
extern "C" void n3ds_dbgmenu_start(void);
static LightLock sLogLock = 1;   /* 1 = unlocked: the render worker and the game thread both log */
extern "C" void n3ds_log(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LightLock_Lock(&sLogLock);
    if (stdout != sLog && stdout != sGameLog && !n3ds_dbgmenu_open && !n3ds_bottom_map_shown()) printf("%s\n", buf); /* game thread: stdout IS sLog, would double every line; the performance menu owns the console while open */
    if (sLog) { fprintf(sLog, "%s\n", buf); fflush(sLog); }
    LightLock_Unlock(&sLogLock);
}

extern "C" void n3ds_log_memory(const char* where)
{
    struct mallinfo mi = mallinfo();
    /* app-region free is ~0 by design: libctru's malloc heap claims the whole region up front, so the number that
       matters is the malloc heap itself: total obtained from the OS (arena) and how much of it is still free. */
    n3ds_log("[mem %s] heap total %d KB | heap free %d KB | in-use %d KB | linear free %lu KB | app-region free %lu KB",
             where, mi.arena / 1024, mi.fordblks / 1024, mi.uordblks / 1024,
             (unsigned long)(linearSpaceFree() / 1024),
             (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024));
}

/* Make sure sdmc:/3ds/nectar/assets exists and is not empty. Returns the number of entries found (0 = missing/empty). */
static int checkAssets(void)
{
    mkdir(N3DS_ASSETS, 0777); /* harmless if it already exists; gives the user a folder to drop files into */
    DIR* d = opendir(N3DS_ASSETS);
    if (!d) return 0;
    int n = 0;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        ++n;
    }
    closedir(d);
    return n;
}

/* The game leaving the process without returning (exit(), abort(), std::terminate, OSPanic-style halts) shows up
   as "just went back to the launcher" with nothing in the log. These hooks make that visible. */
static void onAtExit(void) { if (!sCleanShutdown) n3ds_log("[n3ds] process is exiting (exit()/return from main)"); }
static void onAbort(int) { n3ds_log("[n3ds] abort() / SIGABRT called by the game or runtime"); }
static void onTerminate(void)
{
    n3ds_log("[n3ds] std::terminate (uncaught C++ exception, or exception thrown through a noexcept function)");
    abort();
}

/* Watchdog: every 5 s report frame count and free heap. If "frame" stops advancing the game is stuck or loading;
   the last stdout line the game printed (see the log just above) says where. Runs at higher priority than the game
   but sleeps almost always. */
static void watchdogThread(void*)
{
    n3ds_vfp_runfast();   /* R1: FZ|DN on every thread (see gameThread) */
    unsigned last = (unsigned)-1, t = 0;
    for (;;) {
        svcSleepThread(5000000000LL);
        t += 5;
        struct mallinfo mi = mallinfo();
        unsigned f = n3ds_frame_count;
        n3ds_log("[watchdog t=%us] frame=%u %s | heap free %d KB | last note: %s", t, f,
                 f == last ? "(NOT ADVANCING)" : "", mi.fordblks / 1024, sLastNote);
        /* On a stall show what every thread was doing: first after 10 s, then every 60 s. */
        if (f == last && (t == 10 || t % 60 == 0)) { n3ds_trace_dump(40); n3ds_calls_dump(120); }
        last = f;
    }
}

static void gameThread(void*)
{
    /* R1 (n3ds_port/AGENTS.md): FPSCR is per-thread, so every thread that does float work sets flush-to-zero (FZ) + default-NaN (DN)
       at its entry, before any float work: decay can reach denormals, FZ|DN is the chosen numerical mode, and each thread's state
       must be established. Current libctru already does this in initThreadVars() for main and threadCreate() threads; the call is
       the defensive, version-independent guarantee (it also turns the VFP exception traps off). */
    n3ds_vfp_runfast();
    /* stdout/stderr are per-thread in newlib: redirect them HERE so the game's printf (upstream logs every
       DVDOpen and OSReport this way) and OSPanic land in log.txt instead of scrolling off the bottom screen. */
    if (sGameLog) { stdout = sGameLog; stderr = sGameLog; }
    else if (sLog) { stdout = sLog; stderr = sLog; }
    n3ds_log("[n3ds] game thread started");
    n3ds_log_memory("start");
    n3ds_bench();
    if (n3dsConfig().audioTest) n3ds_audio_selftest(3000);
    n3ds_game_entry();
    n3ds_log("[n3ds] game exited normally");
}

int main(int, char**)
{
    /* R1 (n3ds_port/AGENTS.md): FPSCR is per-thread, so every thread that does float work sets flush-to-zero (FZ) + default-NaN (DN)
       at its entry, before any float work: decay can reach denormals, FZ|DN is the chosen numerical mode, and each thread's state
       must be established. Current libctru already does this in initThreadVars() for main and threadCreate() threads; the call is
       the defensive, version-independent guarantee (it also turns the VFP exception traps off). */
    n3ds_vfp_runfast();
    osSetSpeedupEnable(true); /* 804 MHz + L2 cache (New 3DS only; this port requires a New 3DS) */
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);

    mkdir(N3DS_HOME, 0777);
    if (chdir(N3DS_HOME) != 0) printf("WARNING: cannot chdir to %s\n", N3DS_HOME);
    sLog = fopen(N3DS_HOME "/log.txt", "w");
    if (sLog) {   /* truncate, then reopen twice in append mode: every write lands at the current end, whichever FILE it comes from */
        fclose(sLog);
        sLog = fopen(N3DS_HOME "/log.txt", "a");
        sGameLog = sLog ? fopen(N3DS_HOME "/log.txt", "a") : NULL;
        if (sGameLog) setvbuf(sGameLog, NULL, _IONBF, 0);
    }
    if (sLog) {
        setvbuf(sLog, NULL, _IONBF, 0); /* unbuffered: a panic message printed just before exit()/abort() must not be lost */
        stderr = sLog;                  /* game's fprintf(stderr, ...) alloc stats and panics land in the log too */
    }
    atexit(onAtExit);
    signal(SIGABRT, onAbort);
    std::set_terminate(onTerminate);

    /* card_stubs.cpp derives the save folder from this (install dir + "save"). */
    setenv("NECTAR_EXECUTABLE_PATH", N3DS_HOME "/nectar.3dsx", 1);
    /* "sdmc:/..." has no leading '/', so std::filesystem treats it as relative and fs::absolute() prepends the cwd
       (log showed sdmc:/3ds/nectar/sdmc:/3ds/nectar/save). A cwd-relative path resolves correctly instead. */
    setenv("NECTAR_SAVE_DIR", "save", 1);

    n3ds_log("Open Nectar 3DS r56 (Open Nectar 0.9.3 base; reworked gfx/texture path; title ID 0xEC402 when N3DS_TEST_BUILD) build %s %s", __DATE__, __TIME__);
    n3ds_log("assets folder: %s", N3DS_ASSETS);
    int assetCount = checkAssets();
    n3ds_log("assets folder has %d entries", assetCount);
    /* THE number that decides whether this game can run: 64 MB mode ~ 65536 KB, New 3DS 124 MB mode ~ 126976 KB.
       A .3dsx launched from a launcher inherits the launcher's mode; SystemModeExt in nectar.rsf only applies to an installed .cia. */
    n3ds_log("app region total %lu KB (64MB mode ~65536, 124MB mode ~126976) | homebrew=%d",
             (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024), (int)envIsHomebrew());
    n3ds_log("malloc heap %lu KB (window limit 98304) | linear heap %lu KB",
             (unsigned long)(__ctru_heap_size / 1024), (unsigned long)(__ctru_linear_heap_size / 1024));
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    n3ds_log("console: %s 3DS", isNew ? "New" : "Old");
    /* NEW 3DS ONLY. The game needs the 124 MB extended memory mode and the 804 MHz clock; an Old 3DS has
       neither (64 MB mode, 268 MHz, no L2), so refuse cleanly instead of dying in an allocation later. */
    if (!isNew) {
        n3ds_log("ERROR: this port requires a New 3DS / New 2DS XL (needs 124 MB memory mode and 804 MHz).");
        n3ds_log("It cannot run on an Old 3DS / 2DS.");
    } else if (osGetMemRegionSize(MEMREGION_APPLICATION) < ((u32)N3DS_MIN_APP_REGION_MB << 20)) {
        /* Not fatal (lets you experiment), but a crash from here on is almost certainly memory. */
        n3ds_log("WARNING: app region is %lu KB, expected ~126976 KB (124 MB mode).",
                 (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024));
        n3ds_log("Install the .cia (SystemModeExt 124MB in nectar.rsf); a .3dsx inherits its launcher's mode.");
    }
    const bool canRun = isNew && assetCount > 0;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread t = NULL;
    /* Fixed just above the lowest user priority (0x18 is the highest allowed): it sleeps 5 s at a time, and this keeps
       prio-1 free for game threads that get mapped to (game prio - 1) once OSCreateThread honours priorities. */
    Thread wd = threadCreate(watchdogThread, NULL, 16 * 1024, 0x19, -2, true);
    (void)wd;
    if (!isNew) {
        /* message already shown above */
    } else if (assetCount == 0) {
        n3ds_log("ERROR: %s is missing or empty.", N3DS_ASSETS);
        n3ds_log("Copy the extracted game files there (so files sit at");
        n3ds_log("%s/<path>), then start again.", N3DS_ASSETS);
    } else {
        n3ds_dbgmenu_start();   /* r17: bottom-screen performance menu (tap to open) */
        t = threadCreate(gameThread, NULL, GAME_STACK_SIZE, prio, -2, false);
    }
    if (!canRun) {
        /* nothing to run; fall through to the exit prompt below */
    } else if (!t) {
        n3ds_log("FATAL: could not create game thread (out of memory?)");
    } else {
        threadJoin(t, U64_MAX);
        threadFree(t);
    }

    n3ds_log("done. press START to exit");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    sCleanShutdown = true;
    if (sLog) { FILE* f = sLog; sLog = NULL; fclose(f); }
    /* sGameLog stays open: the game thread may still be printing into it while the app closes */
    gfxExit();
    return 0;
}
