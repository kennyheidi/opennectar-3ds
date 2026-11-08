// The three pc_window_* calls that drive the frame loop; everything else in pc_window.h is a default
// no-op in n3ds_stubs_gen.cpp. Stage 1 of the citro3d backend: swap_buffers clears + presents the top screen (n3ds_gpu.cpp).
#include "n3ds_bridge.h"
#include "pc_window.h"
#include <string.h>
#include <stdlib.h>
#include "n3ds_config.h"
extern "C" void n3ds_sfx_report(void) __attribute__((weak));
extern "C" void n3ds_aprof_take(unsigned long long* t, unsigned* n);
extern "C" unsigned n3ds_audio_lock_frames(void);   /* defined in the CMake-patched audio_stubs.cpp (platform/n3ds_sfx_diag.inc) */

static unsigned sFrame = 0;
static unsigned sProfF0 = 0;

/* ---- game-thread timers (include/n3ds_prof.h): inclusive ms per frame, reported as [prof] lines ---- */
namespace {
struct ProfSlot { const char* group; const char* name; unsigned long long ticks; unsigned calls; };
struct ProfOpen { int slot; unsigned long long t0; };
ProfSlot sProf[96]; int sNProf = 0;
ProfOpen sProfStack[32]; int sProfDepth = 0;
}
extern "C" void n3ds_prof_begin(const char* group, const char* name)
{
    int idx = -1;
    if (sProfDepth < 32) {
        for (int i = 0; i < sNProf; ++i)
            if ((sProf[i].group == group || !strcmp(sProf[i].group, group)) && (sProf[i].name == name || !strcmp(sProf[i].name, name))) { idx = i; break; }
        if (idx < 0 && sNProf < 96) { idx = sNProf++; sProf[idx].group = group; sProf[idx].name = name; sProf[idx].ticks = 0; sProf[idx].calls = 0; }
        sProfStack[sProfDepth].slot = idx; sProfStack[sProfDepth].t0 = n3ds_ticks();
    }
    ++sProfDepth;
}
extern "C" void n3ds_prof_end(void)
{
    if (sProfDepth <= 0) return;
    --sProfDepth;
    if (sProfDepth >= 32) return;
    const ProfOpen& o = sProfStack[sProfDepth];
    if (o.slot >= 0) { sProf[o.slot].ticks += n3ds_ticks() - o.t0; ++sProf[o.slot].calls; }
}
static void n3dsProfReport(unsigned f0, unsigned f1)
{
    const unsigned n = f1 - f0;
    if (!n) return;
    n3ds_log("[prof] frames %u-%u, game thread, inclusive ms/frame (calls/frame); nested scopes are inside their parent:", f0 + 1, f1);
    bool used[96] = {false};
    for (int k = 0; k < 40; ++k) {
        int best = -1;
        for (int i = 0; i < sNProf; ++i) if (!used[i] && sProf[i].calls && (best < 0 || sProf[i].ticks > sProf[best].ticks)) best = i;
        if (best < 0) break;
        used[best] = true;
        const double ms = (double)sProf[best].ticks / N3DS_TICKS_PER_MS / n;
        if (ms < 0.05) break;
        n3ds_log("[prof]   %-7s %-26s %7.2f ms (%.1f)", sProf[best].group, sProf[best].name, ms, (double)sProf[best].calls / n);
    }
    for (int i = 0; i < sNProf; ++i) { sProf[i].ticks = 0; sProf[i].calls = 0; }
}
/* ---- A/B profiling (nectar.ini ab_test = 1; n3ds_port/AGENTS.md 6, tools/runner/ab_report.py) ------------------------------------------------
   r53: alternates one setting between ab_a (A) and ab_b (B) every ab_frames frames in the same play session, so both sides see the same
   scenes. The first ab_warmup frames of a phase are not measured (a switch can rebuild caches). The frame limiter is off during the test (with
   fps_cap a fast and a slow build both show 33.3 ms) and so is auto detail (it would react differently to A and B).
   r54, after the first capture (log 24) mixed title screen, menus and an area load into the phases:
     - the test waits for gameplay: it starts once 120 frames in a row drew game models (display lists);
     - a phase with a load hitch (a frame over 250 ms) or a frame without game models (menu, loading screen) is DISCARDED and run again on
       the same side after 60 calm gameplay frames ("[ab] phase N A discarded: ..."), so only clean gameplay phases reach the log
       (r56: a hitch must also be 3x the typical frame, see abHitch);
     - the order is A B B A A B B A ...: consecutive pairs (0,1) (2,3) ... each hold one A and one B, and a scene that keeps getting busier
       (or calmer) shows up on both sides equally instead of always favouring the side that came first.
   One "[ab]" line per phase: per-frame mean / median / p95 of the frame period, the game thread's work, its wait for the worker, the worker's
   busy time, and the display-list calls per frame (a scene check: both sides should show about the same number). */
extern "C" volatile unsigned gN3dsDlCallsFrame;     /* n3ds_gfx.cpp: display-list calls in the worker's last finished frame */
namespace {
struct AbState { bool on = false, waiting = false; int phase = -1, calm = 0, discards = 0; unsigned f0 = 0; int nS = 0; double dl = 0; double ema = 0;
                 float fr[1024], game[1024], wait[1024], busy[1024]; };
AbState sAb;
int* abField(N3dsConfig& c, int key)
{
    switch (key) {
    case 0: return &c.gpuXform; case 1: return &c.lighting; case 2: return &c.fastXform; case 3: return &c.meshCpu;
    case 4: return &c.gpuSplit; case 5: return &c.lodModelPx; case 6: return &c.autoLod; case 7: return &c.cullFaces;
    default: return nullptr;
    }
}
const char* abKeyName(int key)
{
    static const char* const n[] = {"gpu_xform", "lighting", "fast_xform", "mesh_cpu", "gpu_split", "lod_model_px", "auto_lod", "cull_faces"};
    return key >= 0 && key < 8 ? n[key] : "?";
}
/* r56: a load hitch is a frame over 250 ms AND over 3x the recent typical frame. r55 used 250 ms alone: in a heavy scene on the CPU side
   (log 26: ~130 ms frames, gpu_xform = 0) ordinary frames crossed it often enough that a discarded phase never got its 120 calm frames
   back, and the test sat on side A for 40 minutes. The typical frame is a slow average of the frames that were not hitches. */
bool abHitch(double frMs)
{
    const bool hitch = frMs > 250.0 && (sAb.ema <= 0.0 || frMs > 3.0 * sAb.ema);
    if (!hitch) sAb.ema = sAb.ema <= 0.0 ? frMs : 0.97 * sAb.ema + 0.03 * frMs;
    return hitch;
}
inline bool abIsB(int phase) { const int k = phase & 3; return k == 1 || k == 2; }   /* A B B A */
int cmpf(const void* a, const void* b) { const float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y ? 1 : 0; }
void abStats(float* v, int n, float& mean, float& p50, float& p95)
{
    double s = 0; for (int i = 0; i < n; ++i) s += v[i];
    mean = n ? (float)(s / n) : 0.0f;
    qsort(v, (size_t)n, sizeof(float), cmpf);
    p50 = n ? v[n / 2] : 0.0f; p95 = n ? v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1] : 0.0f;
}
void abFrame(unsigned f, double frMs, double waitMs, double busyMs)
{
    N3dsConfig& c = const_cast<N3dsConfig&>(n3dsConfig());
    if (!c.abTest) return;
    int* fld = abField(c, c.abKey);
    if (!fld) { if (!sAb.on) { sAb.on = true; n3ds_log("[ab] ab_key %d unknown: A/B test off", c.abKey); } return; }
    const int len = c.abFrames < 120 ? 120 : c.abFrames > 1000 ? 1000 : c.abFrames, warm = c.abWarmup < 0 ? 0 : c.abWarmup >= len - 30 ? len - 30 : c.abWarmup;
    const unsigned dl = gN3dsDlCallsFrame;
    if (!sAb.on) {
        sAb.on = true; sAb.waiting = true; sAb.calm = 0;
        n3ds_log("[ab] A/B test armed: %s A = %d, B = %d, %d frames per phase (first %d not measured), order A B B A; starts once 120 frames in a row draw game models",
                 abKeyName(c.abKey), c.abA, c.abB, len, warm);
        return;
    }
    if (sAb.waiting) {
        const bool hitch = abHitch(frMs);
        sAb.calm = (dl > 0 && !hitch) ? sAb.calm + 1 : 0;
        if (sAb.calm < (sAb.phase >= 0 ? 60 : 120)) return;
        sAb.waiting = false; sAb.f0 = f; sAb.nS = 0; sAb.dl = 0;
        if (sAb.phase >= 0) return;                         /* back from a discarded phase: the same phase again, same side */
        sAb.phase = 0;
        c.fpsCap = 0; if (c.abKey != 6) c.autoLod = 0;
        *fld = c.abA; ++gN3dsCfgGen;
        n3ds_log("[ab] A/B test on at frame %u: frame limiter and auto detail off for the test", f);
        return;
    }
    const unsigned in = f - sAb.f0;
    if (in >= (unsigned)warm) {
        if (abHitch(frMs) || dl == 0) {   /* a load hitch, a menu or a loading screen: this phase does not compare like with like */
            ++sAb.discards;
            n3ds_log("[ab] phase %d %c discarded at frame %u: %s (%.0f ms, %u display-list calls); running it again", sAb.phase, abIsB(sAb.phase) ? 'B' : 'A', f,
                     dl == 0 ? "no game models drawn" : "load hitch", frMs, dl);
            sAb.waiting = true; sAb.calm = 0; sAb.nS = 0; sAb.dl = 0;   /* resumes after 120 calm gameplay frames */
            return;
        }
        if (sAb.nS < 1024) {
            sAb.fr[sAb.nS] = (float)frMs; sAb.game[sAb.nS] = (float)(frMs - waitMs); sAb.wait[sAb.nS] = (float)waitMs; sAb.busy[sAb.nS] = (float)busyMs; ++sAb.nS;
            sAb.dl += dl;
        }
    }
    if (in + 1 >= (unsigned)len) {
        float fm, f50, f95, gm, g50, g95, wm, w50, w95, bm, b50, b95;
        const double dlm = sAb.nS ? sAb.dl / sAb.nS : 0.0;
        abStats(sAb.fr, sAb.nS, fm, f50, f95); abStats(sAb.game, sAb.nS, gm, g50, g95);
        abStats(sAb.wait, sAb.nS, wm, w50, w95); abStats(sAb.busy, sAb.nS, bm, b50, b95);
        n3ds_log("[ab] phase %d %c %s=%d frames %d | frame_ms mean %.2f p50 %.2f p95 %.2f | game_ms mean %.2f p50 %.2f p95 %.2f | wait_ms mean %.2f p50 %.2f p95 %.2f | worker_ms mean %.2f p50 %.2f p95 %.2f | dl_calls %.0f",
                 sAb.phase, abIsB(sAb.phase) ? 'B' : 'A', abKeyName(c.abKey), *fld, sAb.nS, fm, f50, f95, gm, g50, g95, wm, w50, w95, bm, b50, b95, dlm);
        ++sAb.phase; sAb.f0 = f + 1; sAb.nS = 0; sAb.dl = 0;
        *fld = abIsB(sAb.phase) ? c.abB : c.abA; ++gN3dsCfgGen;
    }
}
}

static bool sCloseLogged = false;
// 1j perf line: frame period, and how much of it was spent inside present() (the GPU/citro3d half, split further in [gpu-perf]).
static unsigned long long sPerfT0 = 0, sPresentTicks = 0;
static unsigned sPerfF0 = 0;
static unsigned sBusy0 = 0;                                 // last sampled worker-busy counter (32-bit ticks: wraps every 16 s, so it is sampled every frame)
static unsigned long long sBusyAcc = 0;                     // worker busy ticks since the last [perf-2core] line (64-bit)

bool pc_window_should_close(void)
{
    const bool close = !n3ds_alive();
    // 1i log ended with "game exited normally" after ~140 frames with nothing saying why. aptMainLoop() returning false
    // means the system asked the app to quit (HOME menu -> close software, power button, or the launcher exiting) --
    // it is NOT a crash. Log it once so the two cases can be told apart.
    if (close && !sCloseLogged) { sCloseLogged = true; n3ds_log("[window] aptMainLoop() returned false at frame %u: system requested exit (HOME/power/launcher), not a crash", sFrame); n3ds_gfxq_stop(); }
    return close;
}
void pc_window_shutdown(void) {}
void pc_window_swap_buffers(void)
{
    unsigned f = sFrame++;
    n3ds_frame_tick(f);
    if (f < 3 || f == 10 || f == 60 || f % 600 == 0) { n3ds_log("[frame %u presented]", f); if (f == 60) n3ds_log_memory("frame 60"); }
    const unsigned long long t0 = n3ds_ticks();
    unsigned long long waited = 0;
    const bool threaded = n3ds_gfxq_end_frame(f, &waited) != 0;
    if (!threaded) {                                        // single-thread mode: flush, present and end the frame right here
        n3ds_gfx_flush();                                   // commit vertices batched by the graphics backend
        if (!n3ds_gpu_present(f)) n3ds_present_heartbeat(f);   // citro3d clear+present; software heartbeat only if the GPU init failed
        n3ds_gfx_frame_end(f);                              // per-frame graphics counters ([gfx-perf]) + arms the next profiled frame
    }                                                       // two-thread mode: the core-2 worker flushes, presents and ends the frame
    const unsigned long long t1 = n3ds_ticks();
    sPresentTicks += t1 - t0;                               // threaded: time the game thread was blocked waiting for the worker
    unsigned busyFrame = 0;
    if (threaded) { const unsigned cur = n3ds_gfxq_busy_ticks(); busyFrame = (unsigned)(cur - sBusy0); sBusyAcc += busyFrame; sBusy0 = cur; }   // per frame: a 60-frame window can exceed the 16 s the 32-bit counter spans (log d313ed4b printed 10 ms for a worker that was busy ~280 ms)
    {   /* r53: A/B profiling, per frame */
        static unsigned long long sPrevT1 = 0;
        if (sPrevT1) abFrame(f, (double)(t1 - sPrevT1) / N3DS_TICKS_PER_MS, (double)(t1 - t0) / N3DS_TICKS_PER_MS, (double)busyFrame / N3DS_TICKS_PER_MS);
        sPrevT1 = t1;
    }
    if (f == 0) { sPerfT0 = t1; sPerfF0 = 0; sPresentTicks = 0; }
    else if (f % 60 == 0 && (f <= 600 || f % 600 == 0)) {
        const unsigned n = f - sPerfF0;
        const double total = (double)(t1 - sPerfT0) / N3DS_TICKS_PER_MS / n, pres = (double)sPresentTicks / N3DS_TICKS_PER_MS / n;
        n3ds_log("[perf] frames %u-%u: %.1f ms/frame (%.1f FPS) | present() %.1f ms | everything else (game logic + vertex assembly) %.1f ms",
                 sPerfF0 + 1, f, total, 1000.0 / total, pres, total - pres);
        if (threaded) {                                     // how the two cores share the frame (the slower one sets the frame rate)
            const unsigned long long busy = sBusyAcc; sBusyAcc = 0;
            n3ds_log("[perf-2core] game thread (core 0): %.1f ms/frame of work + %.1f ms waiting for the worker | worker (core 2): %.1f ms/frame busy incl. present/vsync",
                     total - pres, pres, (double)busy / N3DS_TICKS_PER_MS / n);
        }
        sPerfT0 = t1; sPerfF0 = f; sPresentTicks = 0;
    }
    if (f > 0 && f % 300 == 0) {
        n3dsProfReport(sProfF0, f);
        /* Where the game thread's wall time can hide outside the game's own code (per frame, last 300 frames). */
        unsigned yc, ym, lc, lm, cc, cm, cx, qb, qw, qm;
        n3ds_yield_take(&yc, &ym); n3ds_audio_diag_take(&lc, &lm, &cc, &cm, &cx); n3ds_gfxq_diag_take(&qb, &qw, &qm);
        const double n = (double)(f - sProfF0);
        n3ds_log("[prof2] frames %u-%u per frame: OSYieldThread %.1f calls = %.2f ms (all threads) | audio lock %.1f calls waited %.2f ms | mixer callback %.2f ms avg, %.2f ms max over %u chunks | gfx queue %.1f KB, %.2f full-queue stalls = %.2f ms",
                 sProfF0 + 1, f, yc / n, ym / 10.0 / n, lc / n, lm / 10.0 / n, cc ? cm / 10.0 / cc : 0.0, cx / 10.0, cc, qb / 1024.0 / n, qw / n, qm / 10.0 / n);
        { unsigned au, ad, aq; n3ds_audio_stats(&au, &ad, &aq);
          n3ds_log("[audio-stats] NDSP underruns so far %u, frames dropped (queue full) %u, blocks queued now %u", au, ad, aq);
          if (n3ds_sfx_report) n3ds_sfx_report(); }
        { /* r51: where pc_audio_tick()'s time goes (slots: see n3ds_sdl_audio.cpp / n3ds_audio_opt.cmake section 4) */
          unsigned long long t[16]; unsigned c[16];
          n3ds_aprof_take(t, c);
          auto ms = [&](int i) { return (double)t[i] / N3DS_TICKS_PER_MS / n; };
          n3ds_log("[audioprof] per frame: pc_audio_tick %.2f ms, of which waiting for the mixer lock %.2f ms (mixer lock pieces %u frames) | "
                   "sequencers (whole / interpreter / ticks per frame): BGM %.2f / %.2f / %.1f, boss %.2f / %.2f / %.1f, SE %.2f / %.2f / %.1f, events %.2f / %.2f / %.1f",
                   ms(14), ms(15), n3ds_audio_lock_frames(), ms(0), ms(4), c[4] / n, ms(1), ms(5), c[5] / n, ms(2), ms(6), c[6] / n, ms(3), ms(7), c[7] / n);
          n3ds_log("[audioprof] per frame: note starts %.1f = %.2f ms (%.3f ms each) | voice updates %.1f = %.2f ms | finished-voice scans %.1f = %.2f ms | BGM mix advance %.2f ms",
                   c[8] / n, ms(8), c[8] ? (double)t[8] / N3DS_TICKS_PER_MS / c[8] : 0.0, c[9] / n, ms(9), c[13] / n, ms(13), ms(12)); }   /* "[sfx]" line: sound-effect requests vs gates vs voices (spliced into audio_stubs.cpp by CMake; weak so a non-matching stub just skips it) */
        sProfF0 = f;
    }
}
