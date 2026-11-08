#include "n3ds_config.h"
#include "n3ds_bridge.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool setKey(N3dsConfig& c, const char* k, int v)
{
    struct E { const char* name; int* p; };
    const E table[] = {
        {"tex_off", &c.texOff}, {"tex_shift", &c.texShift}, {"tex_max_dim", &c.texMaxDim}, {"tex_min_dim", &c.texMinDim},
        {"gfx_worker", &c.gfxThread}, {"tex_box", &c.texBox}, {"tex_force16", &c.texForce16}, {"tex_budget_kb", &c.texBudgetKb}, {"tex_dump", &c.texDump},
        {"tex_log", &c.texLog}, {"tex_le16", &c.texLE16}, {"tex_diag", &c.texDiag}, {"fps_cap", &c.fpsCap}, {"audio_test", &c.audioTest}, {"skip_title_cutscene", &c.skipTitleCutscene},
        {"skip_title", &c.skipTitle}, {"gfx_profile", &c.gfxProfile}, {"audio_stats", &c.audioStats}, {"gpu_xform", &c.gpuXform}, {"fps_counter", &c.fpsCounter}, {"alloc_prof", &c.allocProf}, {"pak", &c.pak}, {"pak_verify", &c.pakVerify}, {"mesh_debug", &c.meshDebug}, {"mesh_safe", &c.meshSafe}, {"gfx_lite", &c.gfxLite}, {"mesh_verify", &c.meshVerify}, {"mesh_replay", &c.meshReplay}, {"mesh_defer", &c.meshDefer}, {"mesh_cpu", &c.meshCpu}, {"tri_min_area", &c.triMinArea}, {"lod_px", &c.lodPx}, {"lod_model_px", &c.lodModelPx}, {"lod_far_px", &c.lodFarPx}, {"piki_lod_exempt", &c.pikiLodExempt}, {"fast_xform", &c.fastXform}, {"lod_simplify_px", &c.lodSimplifyPx}, {"max_dt_ms", &c.maxDtMs}, {"gpu_split", &c.gpuSplit}, {"mesh_cpu_check", &c.meshCpuCheck}, {"gfx_queue_kb", &c.gfxQueueKb}, {"auto_lod", &c.autoLod}, {"bottom_map", &c.bottomMap}, {"map_zoom", &c.mapZoom}, {"bottom_counter", &c.bottomCounter}, {"unlock_all_areas", &c.unlockAreas}, {"stretch", &c.stretch}, {"cull_faces", &c.cullFaces}, {"lighting", &c.lighting}, {"mesh_guard", &c.meshGuardPct}, {"mesh_limit", &c.meshLimit}, {"mesh_nolit", &c.meshNoLit}, {"mesh_nodraw", &c.meshNoDraw}, {"mesh_cache_kb", &c.meshCacheKb}, {"cstick_range", &c.cstickRange}, {"cstick_dead", &c.cstickDead}, {"preset", &c.preset}, {"gpu_cull", &c.gpuCull}, {"ab_test", &c.abTest}, {"ab_key", &c.abKey}, {"ab_a", &c.abA}, {"ab_b", &c.abB}, {"ab_frames", &c.abFrames}, {"ab_warmup", &c.abWarmup},
    };
    for (const E& e : table) if (strcmp(e.name, k) == 0) { *e.p = v; return true; }
    static const char* const readElsewhere[] = { "audio", "audio_stx", "audio_rate", "audio_core", "audio_core1_pct", "audio_aa", "audio_fastmix", "piki_voice_map", "audio_lock_frames" };   /* n3ds_sdl_audio.cpp reads these itself */
    for (const char* name : readElsewhere) if (strcmp(name, k) == 0) return true;
    return false;
}

int n3dsConfigParse(const char* text, N3dsConfig& cfg, char* bad, int badSize)
{
    int nBad = 0;
    if (bad && badSize > 0) bad[0] = 0;
    const char* p = text;
    while (*p) {
        const char* eol = strchr(p, '\n'); const size_t full = eol ? (size_t)(eol - p) : strlen(p);
        char line[256]; size_t len = full; if (len >= sizeof line) len = sizeof line - 1;   /* only the copy is cut; a long comment no longer spills into a 'next line' */
        memcpy(line, p, len); line[len] = 0;
        p += eol ? full + 1 : full;
        for (char* c = line; *c; ++c) if (*c == '#' || *c == ';' || *c == '\r') { *c = 0; break; }
        char* a = line; while (*a && isspace((unsigned char)*a)) ++a;
        if (!*a) continue;
        char* eq = strchr(a, '=');
        bool ok = false;
        if (eq) {
            *eq = 0; char* e = eq - 1; while (e >= a && isspace((unsigned char)*e)) *e-- = 0;
            char* v = eq + 1; while (*v && isspace((unsigned char)*v)) ++v;
            char* endp = nullptr; const long n = strtol(v, &endp, 0);
            if (endp != v) ok = setKey(cfg, a, (int)n);
        }
        if (!ok) { ++nBad; if (bad && (int)strlen(bad) + (int)strlen(a) + 2 < badSize) { strcat(bad, a); strcat(bad, " "); } }
    }
    // sanity
    if (cfg.texShift < 0) cfg.texShift = 0;
    if (cfg.texShift > 3) cfg.texShift = 3;
    if (cfg.texMinDim < 8) cfg.texMinDim = 8;
    if (cfg.texMinDim > 1024) cfg.texMinDim = 1024;
    if (cfg.texMaxDim < 0) cfg.texMaxDim = 0;
    if (cfg.fpsCap < 0) cfg.fpsCap = 0;
    if (cfg.fpsCap > 60) cfg.fpsCap = 60;
    if (cfg.gfxQueueKb < 128) cfg.gfxQueueKb = 128;
    if (cfg.gfxQueueKb > 4096) cfg.gfxQueueKb = 4096;
    if (cfg.autoLod < 0) cfg.autoLod = 0;
    if (cfg.autoLod > 5) cfg.autoLod = 5;
    if (cfg.preset < 0 || cfg.preset >= N3DS_PRESET_COUNT) cfg.preset = 0;
    n3dsPresetApply(cfg, cfg.preset);   /* r52: a preset wins over the individual keys it covers (the menu saves preset = 0 when they no longer match one) */
    return nBad;
}

/* r52: Quality = the most detail the game can show (full draw distance, GameCube lighting), auto detail only steps in when it gets slow.
   Speed = medium distance, simpler far models, simple lighting: the steadiest 30 FPS. Bare minimum = everything turned down, 20 FPS limit,
   game speed kept down to 10 FPS. Values are steps of the menu's own options, so every row shows a name, not "Custom". */
namespace {
struct PresetVals { const char* name; int model, far, piece, autoLod, simplify, triMin, pikiFull, lighting, fpsCap, maxDt; };
const PresetVals kPresets[N3DS_PRESET_COUNT] = {
    { "Custom",       0,  0,   0,  0, 0,  0,   0, 0, 0,  0   },
    { "Quality",      0,  0,   0,  2, 0,  0,   1, 1, 30, 66  },
    { "Speed",        60, 160, 30, 3, 32, 25,  1, 0, 30, 66  },
    { "Bare minimum", 80, 240, 40, 5, 48, 100, 0, 0, 20, 100 },
};
}
void n3dsPresetApply(N3dsConfig& c, int p)
{
    if (p <= 0 || p >= N3DS_PRESET_COUNT) return;
    const PresetVals& v = kPresets[p];
    c.lodModelPx = v.model; c.lodFarPx = v.far; c.lodPx = v.piece; c.autoLod = v.autoLod; c.lodSimplifyPx = v.simplify; c.triMinArea = v.triMin;
    c.pikiLodExempt = v.pikiFull; c.lighting = v.lighting; c.fpsCap = v.fpsCap; c.maxDtMs = v.maxDt;
}
int n3dsPresetMatch(const N3dsConfig& c)
{
    for (int p = 1; p < N3DS_PRESET_COUNT; ++p) {
        const PresetVals& v = kPresets[p];
        if (c.lodModelPx == v.model && c.lodFarPx == v.far && c.lodPx == v.piece && c.autoLod == v.autoLod && c.lodSimplifyPx == v.simplify && c.triMinArea == v.triMin
            && c.pikiLodExempt == v.pikiFull && c.lighting == v.lighting && c.fpsCap == v.fpsCap && c.maxDtMs == v.maxDt) return p;
    }
    return 0;
}
const char* n3dsPresetName(int p) { return (p >= 0 && p < N3DS_PRESET_COUNT) ? kPresets[p].name : "Custom"; }

extern "C" int n3ds_allocprof_on;   // n3ds_alloc_trace.cpp
volatile int gN3dsCfgGen = 0;
const N3dsConfig& n3dsConfig()
{
    static N3dsConfig cfg; static bool loaded = false;
    if (loaded) return cfg;
    loaded = true;
    FILE* f = fopen("nectar.ini", "rb");
    if (!f) { n3ds_log("[cfg] no nectar.ini in the working directory: built-in New 3DS defaults (tex_max_dim=%d tex_force16=%d fps_cap=%d gfx_worker=%d)", cfg.texMaxDim, cfg.texForce16, cfg.fpsCap, cfg.gfxThread); return cfg; }
    /* The whole file. This used to be a 4096-byte buffer: nectar.ini has long comments and grew past 4 KB, so every key after byte 4095 (lod_px,
       lod_model_px, max_dt_ms, gpu_split, audio_stats, mesh_*, ...) was silently never read and stayed at its default (log 966f0929). */
    static char buf[65536]; size_t n = 0, got;
    while (n < sizeof buf - 1 && (got = fread(buf + n, 1, sizeof buf - 1 - n, f)) > 0) n += got;
    const bool truncated = n >= sizeof buf - 1; buf[n] = 0; fclose(f);
    char bad[256]; const int nBad = n3dsConfigParse(buf, cfg, bad, sizeof bad);
    n3ds_log("[cfg] nectar.ini: tex_off=%d tex_shift=%d tex_max_dim=%d tex_min_dim=%d tex_box=%d tex_force16=%d tex_budget_kb=%d tex_le16=%d tex_diag=%d fps_cap=%d audio_test=%d gfx_worker=%d skip_title=%d gfx_profile=%d cstick_range=%d cstick_dead=%d audio_stats=%d gpu_xform=%d mesh_cache_kb=%d%s%s",
             cfg.texOff, cfg.texShift, cfg.texMaxDim, cfg.texMinDim, cfg.texBox, cfg.texForce16, cfg.texBudgetKb, cfg.texLE16, cfg.texDiag, cfg.fpsCap, cfg.audioTest, cfg.gfxThread, cfg.skipTitle, cfg.gfxProfile, cfg.cstickRange, cfg.cstickDead, cfg.audioStats, cfg.gpuXform, cfg.meshCacheKb,
             nBad ? " | IGNORED: " : "", nBad ? bad : "");
    if (cfg.preset) n3ds_log("[cfg] preset = %d (%s): detail, lighting and FPS settings come from the preset", cfg.preset, n3dsPresetName(cfg.preset));
    n3ds_log("[cfg] nectar.ini: %u bytes read%s | lod_px=%d lod_model_px=%d tri_min_area=%d gpu_split=%d max_dt_ms=%d mesh_cpu=%d mesh_cpu_check=%d gfx_lite=%d mesh_defer=%d mesh_replay=%d alloc_prof=%d pak_verify=%d",
             (unsigned)n, truncated ? " (TRUNCATED: file bigger than 64 KB)" : "", cfg.lodPx, cfg.lodModelPx, cfg.triMinArea, cfg.gpuSplit, cfg.maxDtMs, cfg.meshCpu, cfg.meshCpuCheck, cfg.gfxLite, cfg.meshDefer, cfg.meshReplay, cfg.allocProf, cfg.pakVerify);
    n3ds_allocprof_on = cfg.allocProf;
    if (cfg.maxDtMs) n3ds_log("[cfg] max_dt_ms = %d: the game may advance up to %d ms of game time per frame (original cap 33.3 ms)", cfg.maxDtMs, cfg.maxDtMs < 34 ? 34 : (cfg.maxDtMs > 200 ? 200 : cfg.maxDtMs));
    if (cfg.audioStats) setenv("PIKMIN_AUDIO_STATS", "1", 1);   // read (once) by pc_audio.cpp when the first sound is requested
    if (cfg.audioStats >= 2) {   // audio_stats = 2: ALSO log EVERY event-sound request of the enemy (1), water (5) and Pikmin-voice (6) event types with its outcome (sent / dropped and why)
        setenv("PIKMIN_AUDIO_TRACE_ALL", "1", 1);
        setenv("PIKMIN_AUDIO_TRACE_TYPE", "1,5,6", 1);
    }
    return cfg;
}

// Read by src/plugPikiColin/titles.cpp (3DS builds only).
extern "C" int n3ds_skip_title_cutscene(void)
{
    static int logged = 0;
    const int v = (n3dsConfig().skipTitleCutscene || n3dsConfig().skipTitle) ? 1 : 0;
    if (!logged) { logged = 1; n3ds_log("[cfg] skip_title_cutscene = %d", v); }
    return v;
}

// Read by src/plugPikiColin/titles.cpp (3DS builds only): 1 = leave the title section for the file select the first time it runs.
extern "C" int n3ds_skip_title_to_select(void)
{
    static int logged = 0;
    const int v = n3dsConfig().skipTitle ? 1 : 0;
    if (!logged) { logged = 1; n3ds_log("[cfg] skip_title = %d (title screen %s)", v, v ? "skipped: straight to the file select" : "shown"); }
    return v;
}

// Read by n3ds_pak.cpp (nectar.pak on/off).
extern "C" int n3ds_pak_enabled(void) { return n3dsConfig().pak; }
extern "C" int n3ds_pak_verify(void) { return n3dsConfig().pakVerify; }

// Read by the build-time copy of src/sysDolphin/system.cpp (System::updateSysClock, see n3ds_port/CMakeLists.txt): the largest game-time step per frame.
// 0 = the original 1/30 s. Called once per frame.
extern "C" float n3ds_max_dt(void)
{
    static float v = -1.0f; static int g = -1;
    if (v < 0.0f || g != gN3dsCfgGen) {   // r19: the in-game menu can change max_dt_ms
        g = gN3dsCfgGen;
        const int ms = n3dsConfig().maxDtMs;
        v = ms <= 0 ? (1.0f / 30.0f) : (ms < 34 ? 34 : (ms > 200 ? 200 : ms)) / 1000.0f;
    }
    return v;
}
