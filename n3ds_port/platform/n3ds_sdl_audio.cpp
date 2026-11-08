// SDL2 audio shim for the game's native mixer (pc_port/audio/pc_audio.cpp) on the 3DS.
// Implements exactly the SDL calls that file uses (see shim/SDL2/SDL.h) on top of the NDSP sink in n3ds_audio.cpp:
//   * SDL_OpenAudioDevice starts a mixer thread (New 3DS core 2, falls back to any core) that calls the game's audio
//     callback for ~1024-frame chunks whenever the NDSP ring has room and hands the result to n3ds_audio_write();
//   * SDL_LockAudioDevice/Unlock = a recursive lock the mixer thread also holds while it runs the callback;
//   * SDL_AudioStream = one-shot linear resampler (S16 stereo only) used by the game to bring STX streams to 32 kHz.
// nectar.ini keys read here (parsed locally so this file does not depend on n3ds_config): audio = 1|0 (default 1),
// audio_stx = 1|0 (default 1; was 0: STX music/cutscene streams decode to ~10 MB of PCM and stall the game for seconds).
// UNVERIFIED: not built with devkitPro, never run on hardware.
#include <3ds.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include "n3ds_bridge.h"

extern "C" Result APT_SetAppCpuTimeLimit(u32 percent) __attribute__((weak));   /* declared by <3ds/services/apt.h>; weak so an older libctru still links */

namespace {
struct Dev {
    SDL_AudioSpec spec;
    RecursiveLock lock;
    Thread thread;
    volatile bool run, paused;
    bool open;
    s16* buf;
    unsigned chunkFrames;
};
Dev sDev;
volatile unsigned sLockCalls = 0, sLockMs10 = 0, sCbCalls = 0, sCbMs10 = 0, sCbMax10 = 0;   /* diagnostics: game-thread waits on the mixer lock, mixer callback run time (0.1 ms units) */
volatile unsigned long long sLockTicks = 0;                 /* r51: the same waits in raw ticks, for [audioprof] */
volatile int sLockWaiters = 0;                               /* r51: threads inside SDL_LockAudioDevice right now (the mixer lets them in between two pieces) */
unsigned sLockFrames = 256;                                  /* r51: nectar.ini audio_lock_frames: the mixer holds the lock for this many frames at a time (0 = a whole chunk, old) */
char sErr[96] = "";

int iniInt(const char* key, int def)
{
    FILE* f = fopen("nectar.ini", "r");
    if (!f) return def;
    char line[160]; int val = def;
    while (fgets(line, sizeof line, f)) {
        char* p = line; while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';') continue;
        const size_t kl = strlen(key);
        if (strncmp(p, key, kl) != 0) continue;
        p += kl; while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') continue;
        ++p; while (*p == ' ' || *p == '\t') ++p;
        val = atoi(p);
    }
    fclose(f);
    return val;
}

/* Luma crash dump after HOME -> launch another app: core 2, pc = 0x20000010 = a call through a destroyed object. When the game thread
   finishes ("game exited normally") the game's objects are torn down while the main thread sits in its console loop, and this thread kept
   calling into the mixer. The game pumps pc_window_poll_events() every frame (n3ds_stubs_gen.cpp calls n3ds_audio_game_pulse() from it), so
   the mixer only runs the game callback while that heartbeat is recent. No libctru APT calls here: aptMainLoop/aptGetStatus belong to the
   main thread and their names differ between libctru versions (the previous revision of this file failed to build on that). */
volatile u64 sPulse = 0;                                      /* svcGetSystemTick() of the last game frame; 0 = no frame yet */
bool gameAlive()
{
    const u64 p = sPulse;
    return p == 0 || svcGetSystemTick() - p < SYSCLOCK_ARM11;      /* silent if the game has pumped no frame for 1 s */
}

void mixerMain(void*)
{
    /* R1 (n3ds_port/AGENTS.md): FPSCR is per-thread, so every thread that does float work sets flush-to-zero (FZ) + default-NaN (DN)
       at its entry, before any float work: decay can reach denormals, FZ|DN is the chosen numerical mode, and each thread's state
       must be established. Current libctru already does this in initThreadVars() for main and threadCreate() threads; the call is
       the defensive, version-independent guarantee (it also turns the VFP exception traps off). */
    n3ds_vfp_runfast();
    n3ds_log("[audio] mixer thread running on core %d", (int)svcGetProcessorID());
    while (sDev.run) {
        if (sDev.paused || !gameAlive() || n3ds_audio_queued_frames() > 2048) { svcSleepThread(2 * 1000 * 1000); continue; }
        const unsigned frames = sDev.chunkFrames;
        /* r51: the chunk is mixed in pieces of audio_lock_frames, with the lock released in between. The game thread's sound calls
           (note on/off, voice updates) used to wait for a WHOLE chunk (16-36 ms on the time-limited core 1; log: up to 11 ms of lock waits
           per frame); now at most one piece, and the mixer steps aside between pieces when someone is waiting. The callback is a stream
           (all its state is global), so pieces give exactly the same samples. */
        const unsigned piece = (sLockFrames == 0 || sLockFrames >= frames) ? frames : sLockFrames;
        u64 cbTicks = 0;
        bool stop = false;
        for (unsigned off = 0; off < frames; off += piece) {
            const unsigned n = frames - off < piece ? frames - off : piece;
            RecursiveLock_Lock(&sDev.lock);
            if (!sDev.run) { RecursiveLock_Unlock(&sDev.lock); stop = true; break; }
            const u64 cb0 = svcGetSystemTick();
            sDev.spec.callback(sDev.spec.userdata, (Uint8*)(sDev.buf + (size_t)off * 2), (int)(n * 4));
            cbTicks += svcGetSystemTick() - cb0;
            RecursiveLock_Unlock(&sDev.lock);
            for (int w = 0; w < 20 && sLockWaiters > 0 && off + n < frames; ++w) svcSleepThread(100 * 1000);   /* let a waiting game call in (<= 2 ms) */
        }
        if (stop) break;
        { const unsigned d = (unsigned)(cbTicks * 10000ULL / SYSCLOCK_ARM11); sCbMs10 += d; ++sCbCalls; if (d > sCbMax10) sCbMax10 = d; }
        unsigned done = 0, spins = 0;
        while (done < frames && sDev.run && spins < 500) {
            const unsigned took = n3ds_audio_write(sDev.buf + (size_t)done * 2, frames - done);
            done += took;
            if (done < frames) { svcSleepThread(2 * 1000 * 1000); ++spins; }
        }
    }
}

void stopMixerAtExit()
{
    if (!sDev.open) return;
    sDev.run = false;                                       /* the loop re-checks this under the lock before every callback */
    if (sDev.thread) {
        /* take the lock once: if the mixer is inside the callback right now we wait for it to finish */
        RecursiveLock_Lock(&sDev.lock); RecursiveLock_Unlock(&sDev.lock);
        if (R_SUCCEEDED(threadJoin(sDev.thread, 300ULL * 1000 * 1000))) { threadFree(sDev.thread); sDev.thread = nullptr; }
    }
}
} // namespace

extern "C" void n3ds_audio_diag_take(unsigned* lc, unsigned* lm, unsigned* cc, unsigned* cm, unsigned* cx)
{
    *lc = sLockCalls; *lm = sLockMs10; *cc = sCbCalls; *cm = sCbMs10; *cx = sCbMax10;
    sLockCalls = sLockMs10 = sCbCalls = sCbMs10 = sCbMax10 = 0;
}
extern "C" void n3ds_audio_game_pulse(void) { sPulse = svcGetSystemTick(); }

/* r51: [audioprof] timers. Slots 0-13 are set by the patched pc_audio.cpp (n3ds_audio_opt.cmake section 4), 14/15 by pc_window_poll_events
   (whole pc_audio_tick, and the mixer-lock waits inside it). Game thread only. */
static unsigned long long sAp[16]; static unsigned sApN[16];
extern "C" unsigned long long n3ds_aprof_now(void) { return svcGetSystemTick(); }
extern "C" void n3ds_aprof_add(int slot, unsigned long long t0) { if ((unsigned)slot < 16u) { sAp[slot] += svcGetSystemTick() - t0; ++sApN[slot]; } }
extern "C" void n3ds_aprof_add_raw(int slot, unsigned long long ticks) { if ((unsigned)slot < 16u) { sAp[slot] += ticks; ++sApN[slot]; } }
extern "C" unsigned long long n3ds_audio_lock_ticks(void) { return sLockTicks; }
extern "C" void n3ds_aprof_take(unsigned long long* t, unsigned* n) { for (int i = 0; i < 16; ++i) { t[i] = sAp[i]; n[i] = sApN[i]; sAp[i] = 0; sApN[i] = 0; } }
extern "C" unsigned n3ds_audio_lock_frames(void) { return sLockFrames; }
extern "C" int n3ds_audio_game_enabled(void) { static int v = -1; if (v < 0) v = iniInt("audio", 1) != 0; return v; }
// nectar.ini audio_rate: mixer output rate. 11025 (default, the cheapest), 16000, 22050 or 32000 (GameCube native). Lower = less mixer CPU on core 2, duller
// sound. Anything else snaps to the nearest of those.
extern "C" int n3ds_audio_rate(void)
{
    static int v = 0;
    if (!v) {
        const int r = iniInt("audio_rate", 11025);
        v = r >= 27000 ? 32000 : r >= 19000 ? 22050 : r >= 13500 ? 16000 : 11025;
    }
    return v;
}
extern "C" int n3ds_audio_fastmix(void) { static int v = -1; if (v < 0) v = iniInt("audio_fastmix", 1) != 0; return v; }   /* nectar.ini audio_fastmix: restructured mixer loop (default on) */
extern "C" int n3ds_piki_voice_map(void) { static int v = -1; if (v < 0) v = iniInt("piki_voice_map", 1) != 0; return v; }   /* nectar.ini piki_voice_map: Pikmin voice action N plays voice N (see n3ds_audio_stubs patch in CMakeLists.txt) */
extern "C" int n3ds_audio_aa(void) { static int v = -1; if (v < 0) v = iniInt("audio_aa", 1) != 0; return v; }   /* nectar.ini audio_aa: low-pass waves before they are played at a lower audio_rate (default on) */
extern "C" int n3ds_audio_stx_enabled(void)  { static int v = -1; if (v < 0) v = iniInt("audio_stx", 1) != 0; return v; }

extern "C" {

int SDL_InitSubSystem(Uint32) { return n3ds_audio_game_enabled() ? 0 : -1; }
void SDL_QuitSubSystem(Uint32) {}
const char* SDL_GetError(void) { return sErr; }

SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int)
{
    if (sDev.open) return 0;
    if (!desired || desired->format != AUDIO_S16SYS || desired->channels != 2 || !desired->callback) {
        snprintf(sErr, sizeof sErr, "unsupported audio spec"); return 0;
    }
    if (!n3ds_audio_open((unsigned)desired->freq)) { snprintf(sErr, sizeof sErr, "NDSP unavailable"); return 0; }
    memset(&sDev, 0, sizeof sDev);
    { const int lf = iniInt("audio_lock_frames", 256); sLockFrames = lf <= 0 ? 0u : lf < 64 ? 64u : (unsigned)lf; }
    sDev.spec = *desired;
    sDev.spec.size = (Uint32)desired->samples * 4;
    sDev.chunkFrames = desired->samples ? desired->samples : 1024;
    sDev.buf = (s16*)calloc(sDev.chunkFrames * 2, sizeof(s16));
    if (!sDev.buf) { snprintf(sErr, sizeof sErr, "out of memory"); return 0; }
    RecursiveLock_Init(&sDev.lock);
    sDev.paused = true; sDev.run = true; sDev.open = true;
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    const int mixPrio = prio > 0x19 ? prio - 1 : prio;       // just above the game thread
    // nectar.ini audio_core: 0 (default) = the game's core, 2 = the extra core shared with the render worker, 1 = the New 3DS's system core
    // (needs APT_SetAppCpuTimeLimit; falls back to 2 if the console refuses). Log cea14c68 (Forest of Hope, 6-8 FPS): the render worker on core 2
    // is the bottleneck, and the mixer, which runs at a HIGHER priority on that same core, took 18.4 ms of every 64 ms chunk = 29 % of it. Log
    // f60c4ca9: this console refuses core 1 (APT_SetAppCpuTimeLimit -> d8e05bf4), so the default is now core 0, where the game thread spends
    // 85-100 ms of every frame waiting for the worker and has the time to spare.
    // r34: default is now core 1 again. The refusal (d8e05bf4 = PM "value above the allowed maximum") came from asking for 70 %: the
    // exheader's MaxCpu 0x9E allows at most 30 % (0x80 flag + 0x1E). The mixer needs ~3-6 % of a core, so 30 % is plenty, and it takes
    // the mixer's 2-6 ms per frame off core 0, where the game thread runs. Falls back to core 0 if the console still refuses.
    int mixCore = iniInt("audio_core", 1);
    if (mixCore != 0 && mixCore != 2) mixCore = 1;
    sDev.thread = nullptr;
    if (mixCore == 1) {
        int pct = iniInt("audio_core1_pct", 30); if (pct < 5) pct = 5; if (pct > 80) pct = 80;
        Result lr = (Result)-1;
        const int tries[4] = { pct, 30, 25, 20 };
        for (int t = 0; t < 4 && APT_SetAppCpuTimeLimit; ++t) {   // weak symbol: a libctru without it just skips core 1
            if (t > 0 && tries[t] >= pct) continue;
            lr = APT_SetAppCpuTimeLimit((u32)tries[t]);
            n3ds_log("[audio] core 1: APT_SetAppCpuTimeLimit(%d) -> %08lx", tries[t], (unsigned long)lr);
            if (R_SUCCEEDED(lr)) break;
        }
        if (R_SUCCEEDED(lr)) sDev.thread = threadCreate(mixerMain, nullptr, 64 * 1024, mixPrio, 1, false);
        n3ds_log("[audio] core 1: mixer thread %s", sDev.thread ? "created on core 1" : "NOT created: using core 0");
        if (!sDev.thread) mixCore = 0;
    }
    if (!sDev.thread) sDev.thread = threadCreate(mixerMain, nullptr, 64 * 1024, mixPrio, mixCore == 0 ? 0 : 2, false);
    if (!sDev.thread) sDev.thread = threadCreate(mixerMain, nullptr, 64 * 1024, mixPrio, -2, false);
    if (!sDev.thread) { sDev.run = false; sDev.open = false; free(sDev.buf); snprintf(sErr, sizeof sErr, "threadCreate failed"); return 0; }
    if (obtained) *obtained = sDev.spec;
    { static bool reg = false; if (!reg) { reg = true; atexit(stopMixerAtExit); } }
    n3ds_log("[audio] mixer thread up: %d Hz, %u-frame chunks, prio %d, core %d", desired->freq, sDev.chunkFrames, (int)mixPrio, mixCore);
    return 1;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID, int pause_on) { sDev.paused = pause_on != 0; }
void SDL_LockAudioDevice(SDL_AudioDeviceID)
{
    if (!sDev.open) return;
    const u64 t0 = svcGetSystemTick();
    __atomic_add_fetch(&sLockWaiters, 1, __ATOMIC_ACQ_REL);
    RecursiveLock_Lock(&sDev.lock);
    __atomic_sub_fetch(&sLockWaiters, 1, __ATOMIC_ACQ_REL);
    const u64 w = svcGetSystemTick() - t0;
    sLockTicks += w;
    sLockMs10 += (unsigned)((unsigned)w / (unsigned)(SYSCLOCK_ARM11 / 10000)); ++sLockCalls;   /* 32-bit division: a wait is far below the 16 s a 32-bit tick count spans */
}
void SDL_UnlockAudioDevice(SDL_AudioDeviceID) { if (sDev.open) RecursiveLock_Unlock(&sDev.lock); }
void SDL_ClearQueuedAudio(SDL_AudioDeviceID) {}

void SDL_CloseAudioDevice(SDL_AudioDeviceID)
{
    if (!sDev.open) return;
    stopMixerAtExit();
    if (sDev.thread) return;                                /* still running (join timed out): leave the buffer alone */
    free(sDev.buf); sDev.buf = nullptr;
    sDev.open = false;
}

Uint64 SDL_GetPerformanceCounter(void) { return svcGetSystemTick(); }
Uint64 SDL_GetPerformanceFrequency(void) { return SYSCLOCK_ARM11; }

// ---- one-shot resampling stream (S16, 2 channels only) ----
struct SDL_AudioStream {
    int srcRate, dstRate;
    short* in; size_t inFrames, inCap;
    short* out; size_t outFrames;
    bool flushed;
};

SDL_AudioStream* SDL_NewAudioStream(SDL_AudioFormat sf, Uint8 sc, int sr, SDL_AudioFormat df, Uint8 dc, int dr)
{
    if (sf != AUDIO_S16SYS || df != AUDIO_S16SYS || sc != 2 || dc != 2 || sr <= 0 || dr <= 0) { snprintf(sErr, sizeof sErr, "unsupported stream"); return nullptr; }
    SDL_AudioStream* s = (SDL_AudioStream*)calloc(1, sizeof *s);
    if (s) { s->srcRate = sr; s->dstRate = dr; }
    return s;
}

int SDL_AudioStreamPut(SDL_AudioStream* s, const void* buf, int len)
{
    if (!s || !buf || len < 0 || s->flushed) return -1;
    const size_t frames = (size_t)len / 4;
    if (s->inFrames + frames > s->inCap) {
        const size_t cap = s->inFrames + frames;
        short* n = (short*)realloc(s->in, cap * 4);
        if (!n) return -1;
        s->in = n; s->inCap = cap;
    }
    memcpy(s->in + s->inFrames * 2, buf, frames * 4);
    s->inFrames += frames;
    return 0;
}

int SDL_AudioStreamFlush(SDL_AudioStream* s)
{
    if (!s) return -1;
    if (s->flushed) return 0;
    s->flushed = true;
    if (s->inFrames == 0) return 0;
    if (s->srcRate == s->dstRate) { s->out = s->in; s->outFrames = s->inFrames; s->in = nullptr; s->inFrames = s->inCap = 0; return 0; }
    const size_t outFrames = (size_t)((unsigned long long)s->inFrames * (unsigned)s->dstRate / (unsigned)s->srcRate);
    short* o = (short*)malloc(outFrames * 4 + 4);
    if (!o) return -1;
    // 16.16 fixed-point source position, linear interpolation
    const unsigned long long step = ((unsigned long long)s->srcRate << 16) / (unsigned)s->dstRate;
    unsigned long long pos = 0;
    for (size_t i = 0; i < outFrames; ++i, pos += step) {
        const size_t i0 = (size_t)(pos >> 16); const int fr = (int)(pos & 0xFFFF);
        const size_t i1 = i0 + 1 < s->inFrames ? i0 + 1 : i0;
        for (int c = 0; c < 2; ++c) {
            const int a = s->in[i0 * 2 + c], b = s->in[i1 * 2 + c];
            o[i * 2 + c] = (short)(a + (int)(((long long)(b - a) * fr) >> 16));   /* 64-bit: (b-a) * fr overflows int for swings > 32767 */
        }
    }
    free(s->in); s->in = nullptr; s->inFrames = s->inCap = 0;
    s->out = o; s->outFrames = outFrames;
    return 0;
}

int SDL_AudioStreamAvailable(SDL_AudioStream* s) { return s ? (int)(s->outFrames * 4) : 0; }

int SDL_AudioStreamGet(SDL_AudioStream* s, void* buf, int len)
{
    if (!s || !buf || len < 0) return -1;
    const int avail = (int)(s->outFrames * 4);
    const int n = len < avail ? len : avail;
    memcpy(buf, s->out, (size_t)n);
    return n;
}

void SDL_FreeAudioStream(SDL_AudioStream* s)
{
    if (!s) return;
    free(s->in); free(s->out); free(s);
}

} // extern "C"
