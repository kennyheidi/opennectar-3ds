// r19: 14 options (16 px touch bands), the bottom-screen map (bottom_map / map_zoom) and live tri_min_area / gpu_split / max_dt_ms.
// r17: in-game performance menu on the bottom screen (tap to open). Lets the render-distance / LOD / frame-cap settings be changed while playing
// and saved to nectar.ini. The bottom screen otherwise shows the log console; while the menu is open log lines are not printed there.
//
// Threads: the GAME thread scans input (n3ds_input_poll) and hands touch-down taps over through n3ds_dbgmenu_touch(). A small low-priority
// MENU thread draws the text (its stdout is the bottom-screen console; the game thread's stdout is log.txt) and applies the taps. Settings
// are plain ints in the N3dsConfig that the game thread and the render worker read live; gN3dsCfgGen tells the worker to re-read auto_lod.
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "n3ds_config.h"
#include "n3ds_bridge.h"

extern "C" volatile unsigned n3ds_frame_count;
extern "C" volatile int gN3dsPickX, gN3dsPickY, gN3dsPickReq;   /* n3ds_gfx.cpp (r44 pixel pick) */
extern "C" int n3ds_gfx_autolod_level(void);
extern "C" volatile float gN3dsUsPerTri, gN3dsDlMs;

volatile int n3ds_dbgmenu_open = 0;          // read by n3ds_log: no console printing while the menu is shown

namespace {
volatile int sTapPending = 0, sTapX = 0, sTapY = 0;
volatile int sStop = 0;
Thread sThread = nullptr;
char sStatus[48] = "";

N3dsConfig& cfg() { return const_cast<N3dsConfig&>(n3dsConfig()); }

// ---- options -------------------------------------------------------------------------------------------------------------------
// Render distance presets: lod_model_px / lod_far_px / lod_px (auto_lod has its own row since r19)
struct Preset { const char* name; int model, far, piece; };
const Preset kDist[] = {
    { "Max (draw all)", 0,   0,   0  },
    { "High",           40,  100, 20 },     // the r9+ defaults
    { "Medium",         60,  160, 30 },
    { "Low",            80,  240, 40 },
};
const int kAuto[]  = { 0, 2, 3, 5 };
const int kSimp[]  = { 0, 20, 32, 48 };
const int kTri[]   = { 0, 25, 50, 100, 400 };   // r23: + 0.25 px^2
const int kSplit[] = { 0, 3000, 6000, 12000 };
const int kCap[]   = { 30, 20, 0 };
const int kSpeed[] = { 0, 66, 100 };
const int kZoom[]  = { 2, 4, 8 };

int distIndex()
{
    const N3dsConfig& c = n3dsConfig();
    for (int i = 0; i < 4; ++i) if (kDist[i].model == c.lodModelPx && kDist[i].far == c.lodFarPx && kDist[i].piece == c.lodPx) return i;
    return -1;   // custom (from nectar.ini)
}
template <size_t N> int idxOf(const int (&a)[N], int v) { for (size_t i = 0; i < N; ++i) if (a[i] == v) return (int)i; return -1; }
template <size_t N> int next(const int (&a)[N], int v) { return a[(size_t)(idxOf(a, v) + 1) % N]; }

enum { ROW_DIST = 0, ROW_AUTO, ROW_SIMP, ROW_TRI, ROW_PIKI, ROW_LIGHT, ROW_SPLIT, ROW_CAP, ROW_SPEED, ROW_FPS, ROW_MAP, ROW_ZOOM, ROW_UNLOCK, ROW_SAVE, ROW_COUNT };   // r26: Save and Close share the last band (left / right half)
const int ROW_CLOSE = -2;
const int kRowTop = 3, kRowH = 2;            // console rows (8 px each): every option is a 16 px tall touch band, 14 of them fill rows 3..30

void apply(int row)
{
    N3dsConfig& c = cfg();
    switch (row) {
    case ROW_DIST:  { const int i = (distIndex() + 1) % 4; c.lodModelPx = kDist[i].model; c.lodFarPx = kDist[i].far; c.lodPx = kDist[i].piece; break; }
    case ROW_AUTO:  c.autoLod = next(kAuto, c.autoLod); break;
    case ROW_SIMP:  c.lodSimplifyPx = next(kSimp, c.lodSimplifyPx); break;
    case ROW_TRI:   c.triMinArea = next(kTri, c.triMinArea); break;
    case ROW_PIKI:  c.pikiLodExempt = !c.pikiLodExempt; break;
    case ROW_LIGHT: c.lighting = !c.lighting; break;   // r47 (replaces "Vertex mode": fast_xform stays in nectar.ini)
    case ROW_SPLIT: c.gpuSplit = next(kSplit, c.gpuSplit); break;
    case ROW_CAP:   c.fpsCap = next(kCap, c.fpsCap); break;
    case ROW_SPEED: c.maxDtMs = next(kSpeed, c.maxDtMs); break;
    case ROW_FPS: {   // r27 "Top screen": 4:3 + FPS -> 4:3 -> Stretched + FPS -> Stretched -> ...
        if (c.fpsCounter) c.fpsCounter = 0;
        else { c.fpsCounter = 1; c.stretch = !c.stretch; }
        break;
    }
    case ROW_MAP: {   // r25: Minimap + Pikmin count -> Big map -> Minimap once the radar is found (+ count) -> Text log -> ...
        if (c.bottomMap == 2 && c.bottomCounter) c.bottomCounter = 0;
        else if (c.bottomMap == 2) { c.bottomMap = 1; c.bottomCounter = 1; }
        else if (c.bottomMap == 1) c.bottomMap = 0;
        else { c.bottomMap = 2; c.bottomCounter = 1; }
        break;
    }
    case ROW_ZOOM:  c.mapZoom = next(kZoom, c.mapZoom); break;
    case ROW_UNLOCK: c.unlockAreas = !c.unlockAreas; break;
    default: break;
    }
    c.preset = n3dsPresetMatch(c);   // r52: still exactly a preset? (else "custom": saved as preset = 0 so the individual values are used)
    ++gN3dsCfgGen;
}

// r52: the preset band (top two console rows): Quality / Speed / Bare minimum, one third of the screen each
void applyPreset(int p)
{
    N3dsConfig& c = cfg();
    n3dsPresetApply(c, p);
    c.preset = n3dsPresetMatch(c);
    ++gN3dsCfgGen;
}

// Rewrites the managed keys in nectar.ini (keeps every other line and its comments), appends the ones that are missing.
bool saveIni()
{
    const N3dsConfig& c = n3dsConfig();
    struct KV { const char* k; int v; bool done; } kv[] = {
        { "lod_model_px", c.lodModelPx, false }, { "lod_far_px", c.lodFarPx, false }, { "lod_px", c.lodPx, false }, { "auto_lod", c.autoLod, false },
        { "lod_simplify_px", c.lodSimplifyPx, false }, { "piki_lod_exempt", c.pikiLodExempt, false }, { "fps_cap", c.fpsCap, false }, { "fps_counter", c.fpsCounter, false }, { "fast_xform", c.fastXform, false },
        { "tri_min_area", c.triMinArea, false }, { "gpu_split", c.gpuSplit, false }, { "max_dt_ms", c.maxDtMs, false }, { "bottom_map", c.bottomMap, false }, { "map_zoom", c.mapZoom, false }, { "bottom_counter", c.bottomCounter, false }, { "unlock_all_areas", c.unlockAreas, false }, { "stretch", c.stretch, false }, { "lighting", c.lighting, false },
        { "preset", n3dsPresetMatch(c), false },   // r52: nectar.ini applies a preset over the keys above, so it must say 0 once they are custom
    };
    char* text = nullptr; long len = 0;
    if (FILE* f = fopen("nectar.ini", "rb")) {
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
        text = (char*)malloc(len + 1);
        if (text) { len = (long)fread(text, 1, len, f); text[len] = 0; }
        fclose(f);
    }
    FILE* o = fopen("nectar.ini.tmp", "wb");
    if (!o) { free(text); return false; }
    const char* p = text;
    while (p && *p) {
        const char* e = strchr(p, '\n'); const size_t n = e ? (size_t)(e - p + 1) : strlen(p);
        const char* q = p; while (q < p + n && (*q == ' ' || *q == '\t')) ++q;
        bool replaced = false;
        for (KV& x : kv) {
            const size_t kl = strlen(x.k);
            if ((size_t)(p + n - q) > kl && strncmp(q, x.k, kl) == 0 && (q[kl] == ' ' || q[kl] == '\t' || q[kl] == '=')) {
                const char* cm = (const char*)memchr(q, '#', p + n - q);           // keep the line's comment
                fprintf(o, "%-14s= %-6d", x.k, x.v);
                if (cm) fwrite(cm, 1, p + n - cm, o); else fputc('\n', o);
                x.done = true; replaced = true; break;
            }
        }
        if (!replaced) fwrite(p, 1, n, o);
        p += n;
    }
    for (KV& x : kv) if (!x.done) fprintf(o, "%-14s= %-6d # saved from the in-game menu\n", x.k, x.v);
    fclose(o); free(text);
    remove("nectar.ini");
    return rename("nectar.ini.tmp", "nectar.ini") == 0;
}

void draw(int fps)
{
    const N3dsConfig& c = n3dsConfig();
    const int di = distIndex();
    /* r52: rows 1-2 = the preset band (tap a third of it), row 2 also carries the FPS line */
    const int pm = n3dsPresetMatch(c);
    printf("\x1b[1;1H");
    for (int i = 0; i < 3; ++i) {
        const char* nmP = n3dsPresetName(i + 1);
        const int w = 13, l = (int)strlen(nmP), lp = (w - l) / 2;
        printf("%s%*s%s%*s\x1b[0m", pm == i + 1 ? "\x1b[30;46m" : "\x1b[36;1m", lp, "", nmP, w - l - lp, "");
    }
    printf("\x1b[2;1H\x1b[37;1m FPS %2d\x1b[0m  auto detail level %d  %-9s", fps, n3ds_gfx_autolod_level(), pm ? "" : "(custom)");
    struct { const char* label; char val[24]; } r[ROW_COUNT];
    snprintf(r[ROW_DIST].val, 24, "%s", di >= 0 ? kDist[di].name : "Custom (ini file)");
    static const char* const kAutoName[] = { "Off", "Light", "Normal", "Strong" };
    static const char* const kSimpName[] = { "Off", "Light", "Normal", "Strong" };
    static const char* const kTriName[]  = { "Off", "Smallest", "Small", "Medium", "Large" };
    static const char* const kSplitName[] = { "Off", "Small", "Medium", "Large" };
    static const char* const kZoomName[] = { "Far", "Medium", "Close" };
    auto nm = [](const char* const* names, int idx) { return idx >= 0 ? names[idx] : "Custom"; };
    r[ROW_DIST].label  = "Draw distance";
    r[ROW_AUTO].label  = "Auto detail";      snprintf(r[ROW_AUTO].val, 24, "%s", nm(kAutoName, idxOf(kAuto, c.autoLod)));
    r[ROW_SIMP].label  = "Simpler far models"; snprintf(r[ROW_SIMP].val, 24, "%s", nm(kSimpName, idxOf(kSimp, c.lodSimplifyPx)));
    r[ROW_TRI].label   = "Skip tiny shapes"; snprintf(r[ROW_TRI].val, 24, "%s", nm(kTriName, idxOf(kTri, c.triMinArea)));
    r[ROW_PIKI].label  = "Pikmin detail";    snprintf(r[ROW_PIKI].val, 24, "%s", c.pikiLodExempt ? "Always full" : "Like the rest");
    r[ROW_LIGHT].label = "Lighting";         snprintf(r[ROW_LIGHT].val, 24, "%s", c.lighting ? "GameCube" : "Simple");
    r[ROW_SPLIT].label = "GPU head start";   snprintf(r[ROW_SPLIT].val, 24, "%s", nm(kSplitName, idxOf(kSplit, c.gpuSplit)));
    r[ROW_CAP].label   = "FPS limit";        if (c.fpsCap) snprintf(r[ROW_CAP].val, 24, "%d", c.fpsCap); else snprintf(r[ROW_CAP].val, 24, "None");
    r[ROW_SPEED].label = "When FPS drops";   snprintf(r[ROW_SPEED].val, 24, "%s", c.maxDtMs <= 0 ? "Slow motion" : c.maxDtMs >= 100 ? "Full speed (to 10)" : "Full speed (to 15)");
    r[ROW_FPS].label   = "Top screen";       snprintf(r[ROW_FPS].val, 24, "%s%s", c.stretch ? "Stretched" : "4:3", c.fpsCounter ? " + FPS" : "");
    r[ROW_MAP].label   = "Bottom screen";    snprintf(r[ROW_MAP].val, 24, "%s", c.bottomMap == 0 ? "Text log" : c.bottomMap == 1 ? "Minimap (w/ radar)" : c.bottomCounter ? "Minimap + Pikmin" : "Big map");
    r[ROW_ZOOM].label  = "Map zoom";         snprintf(r[ROW_ZOOM].val, 24, "%s", nm(kZoomName, idxOf(kZoom, c.mapZoom)));
    r[ROW_UNLOCK].label = "Unlock all areas"; snprintf(r[ROW_UNLOCK].val, 24, "%s", c.unlockAreas ? "On (cheat)" : "Off");
    r[ROW_SAVE].label  = "Save settings";    snprintf(r[ROW_SAVE].val, 24, "%-9s  [Close]", sStatus[0] ? sStatus : "");
    for (int i = 0; i < ROW_COUNT; ++i) {
        const int y = kRowTop + i * kRowH;
        const char* bg = (i & 1) ? "\x1b[44m" : "\x1b[40m";               // alternate band colours: each band is one touch target
        printf("\x1b[%d;1H%s %-18s \x1b[33;1m%-19s\x1b[0m", y, bg, r[i].label, r[i].val);
        printf("\x1b[%d;1H%s%39s\x1b[0m", y + 1, bg, "");   // 39: a 40th character on row 30 would scroll the console
    }
}

volatile int sMapShown = 0;          // the game drew the map on a recent frame (the console is covered: nothing is printed)
volatile unsigned sMapFrame = 0;

void menuThread(void*)
{
    /* R1 (n3ds_port/AGENTS.md): FPSCR is per-thread, so every thread that does float work sets flush-to-zero (FZ) + default-NaN (DN)
       at its entry, before any float work: decay can reach denormals, FZ|DN is the chosen numerical mode, and each thread's state
       must be established. Current libctru already does this in initThreadVars() for main and threadCreate() threads; the call is
       the defensive, version-independent guarantee (it also turns the VFP exception traps off). */
    n3ds_vfp_runfast();
    bool shown = false; unsigned f0 = n3ds_frame_count; u64 t0 = svcGetSystemTick(); int fps = 0; u64 lastDraw = 0;
    while (!sStop) {
        svcSleepThread(50 * 1000000LL);
        const u64 now = svcGetSystemTick();
        if (now - t0 >= SYSCLOCK_ARM11) { const unsigned f = n3ds_frame_count; fps = (int)((f - f0) * (double)SYSCLOCK_ARM11 / (double)(now - t0) + 0.5); f0 = f; t0 = now; }
        if (sMapShown && n3ds_frame_count - sMapFrame > 15) {          // map gone (menu, area change, setting): give the console back
            sMapShown = 0;
            if (!n3ds_dbgmenu_open) printf("\x1b[0m\x1b[2J\x1b[1;1H(tap the bottom screen for settings)\n");
        }
        if (sTapPending) {
            const int x = sTapX, y = sTapY; sTapPending = 0;
            if (!n3ds_dbgmenu_open) {               // any tap opens the menu
                n3ds_dbgmenu_open = 1; sStatus[0] = 0;
                svcSleepThread(120 * 1000000LL);    // frames already queued may still put the map on the bottom screen
                printf("\x1b[0m\x1b[2J"); shown = true; lastDraw = 0;
            } else {
                const int line = y / 8 + 1;
                if (line < kRowTop) {                                            // r52: preset band
                    const int p = x < 107 ? N3DS_PRESET_QUALITY : x < 213 ? N3DS_PRESET_SPEED : N3DS_PRESET_MINIMUM;
                    applyPreset(p); sStatus[0] = 0;
                    const N3dsConfig& c = n3dsConfig();
                    n3ds_log("[menu] preset %s: lod_model_px %d lod_far_px %d lod_px %d auto_lod %d lod_simplify_px %d tri_min_area %d piki_lod_exempt %d lighting %d fps_cap %d max_dt_ms %d",
                             n3dsPresetName(p), c.lodModelPx, c.lodFarPx, c.lodPx, c.autoLod, c.lodSimplifyPx, c.triMinArea, c.pikiLodExempt, c.lighting, c.fpsCap, c.maxDtMs);
                    lastDraw = 0;
                    continue;
                }
                int row = (line - kRowTop) / kRowH;                              // console row (1-based) -> option band
                if (row == ROW_SAVE && x >= 240) row = ROW_CLOSE;                // [Close] sits on the right of the Save band
                if (line >= kRowTop && (row == ROW_CLOSE || (row >= 0 && row < ROW_COUNT))) {
                    if (row == ROW_CLOSE) { n3ds_dbgmenu_open = 0; printf("\x1b[0m\x1b[2J\x1b[1;1H(tap the bottom screen for settings)\n"); shown = false; continue; }
                    if (row == ROW_SAVE) { snprintf(sStatus, sizeof sStatus, "%s", saveIni() ? "Saved!" : "Could not save"); n3ds_log("[menu] settings saved to nectar.ini (%s)", sStatus); }
                    else { apply(row); sStatus[0] = 0;
                           const N3dsConfig& c = n3dsConfig();
                           n3ds_log("[menu] lod_model_px %d lod_far_px %d lod_px %d auto_lod %d lod_simplify_px %d tri_min_area %d piki_lod_exempt %d fast_xform %d gpu_split %d fps_cap %d max_dt_ms %d fps_counter %d bottom_map %d bottom_counter %d map_zoom %d",
                                    c.lodModelPx, c.lodFarPx, c.lodPx, c.autoLod, c.lodSimplifyPx, c.triMinArea, c.pikiLodExempt, c.fastXform, c.gpuSplit, c.fpsCap, c.maxDtMs, c.fpsCounter, c.bottomMap, c.bottomCounter, c.mapZoom);
                           if (row == ROW_UNLOCK) n3ds_log("[menu] unlock_all_areas %d", c.unlockAreas); }
                    lastDraw = 0;
                }
            }
        }
        if (shown && n3ds_dbgmenu_open && (lastDraw == 0 || now - lastDraw >= SYSCLOCK_ARM11 / 2)) { draw(fps); lastDraw = now ? now : 1; }
    }
}
}  // namespace

// ---- bottom-screen map (newPikiGame.cpp draws the game's radar there) -------------------------------------------------------------
extern "C" int n3ds_bottom_map_mode(void) { return n3ds_dbgmenu_open ? 0 : n3dsConfig().bottomMap; }
extern "C" int n3ds_bottom_map_zoom(void) { const int z = n3dsConfig().mapZoom; return z < 1 ? 1 : z > 16 ? 16 : z; }
extern "C" int n3ds_unlock_all_areas(void) { return n3dsConfig().unlockAreas; }   // r26: playerState.cpp (pc_cheat_unlock_zones)
extern "C" int n3ds_bottom_counter(void) { return n3dsConfig().bottomCounter; }
extern "C" void n3ds_bottom_map_drawn(void) { sMapFrame = n3ds_frame_count; sMapShown = 1; }
extern "C" int n3ds_bottom_map_shown(void) { return sMapShown; }   // n3ds_log: the console is covered by the map

// Game thread, right after hidScanInput.
extern "C" void n3ds_dbgmenu_touch(void)
{
    if (!(hidKeysDown() & KEY_TOUCH)) return;
    touchPosition t; hidTouchRead(&t);
    if (hidKeysHeld() & KEY_L) {   /* r44: L + tap = pick the top-screen pixel under the tap (the bottom screen is laid over the 4:3 picture) */
        const int st = n3dsConfig().stretch;
        gN3dsPickX = st ? t.px * 400 / 320 : 40 + t.px; gN3dsPickY = t.py; gN3dsPickReq = 1;
        n3ds_log("[pick] L + tap at bottom (%d,%d) -> top-screen pixel (%d,%d)", t.px, t.py, gN3dsPickX, gN3dsPickY);
        return;
    }
    sTapX = t.px; sTapY = t.py; sTapPending = 1;
}

extern "C" void n3ds_dbgmenu_start(void)
{
    if (sThread) return;
    s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    sThread = threadCreate(menuThread, nullptr, 16 * 1024, prio + 4 > 0x3F ? 0x3F : prio + 4, -2, true);   // below the main thread: only runs when the game waits
    n3ds_log("[menu] performance menu ready: tap the bottom screen to open it%s", sThread ? "" : " (THREAD FAILED)");
}

// host test hook (tools/host_test): apply one menu row / save the ini
extern "C" int n3ds_dbgmenu_test(int row) { if (row < 0) return saveIni() ? 1 : 0; if (row >= 100) { applyPreset(row - 100); return n3dsPresetMatch(n3dsConfig()); } apply(row); return 1; }
