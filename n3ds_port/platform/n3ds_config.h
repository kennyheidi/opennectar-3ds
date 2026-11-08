// Runtime settings from sdmc:/3ds/nectar/nectar.ini (plain "key = value" lines, '#' or ';' start a comment).
// Pure C++ (stdio only) so the host test can exercise the parser. Missing file = defaults. Unknown keys are reported, not fatal.
#pragma once

// DEFAULTS are tuned for the New 3DS family (New 3DS, New 3DS XL, New 2DS XL: identical CPU/GPU/RAM, 400x240 top screen), so a
// missing or empty nectar.ini gives the intended experience. Everything below can still be overridden in nectar.ini.
struct N3dsConfig {
    int texOff       = 0;     // 1: never create textures (everything draws untextured) - isolates texture cost when benchmarking
    int texShift     = 0;     // 0..3: divide every texture's width and height by 2^shift
    int texMaxDim    = 256;   // 0 = no cap; else (rounded down to a power of two, 16..1024) the largest allowed width/height
    int texMinDim    = 16;    // a texture is never reduced below this (icons, font glyph sheets stay sharp)
    int gfxThread    = 1;     // 1: record GX calls on the game thread, decode/draw/present on a core-2 worker; 0: all on the game thread
    int texBox       = 1;     // 1: average the source texels when shrinking (nicer); 0: point-sample (about shift*2 times faster)
    int texForce16   = 1;     // 1: never use 32-bit RGBA8 (translucent textures become RGBA4)
    int texBudgetKb  = -1;    // -1 = compile-time default (N3DS_TEX_BUDGET_KB)
    int texDump      = -1;    // -1 = compile-time default
    int texLog       = -1;    // -1 = compile-time default
    int texLE16      = 0;     // 1: treat 16-bit texel/palette words as little-endian
    int fpsCap       = 30;    // presented frames per second: 60, 30, 20 or 15 (vblank aligned); 0 = no limit (present as fast as the GPU allows)
    int audioTest    = 0;     // 1: play a 3 s two-tone test through NDSP at startup (proves DSP + firmware + the audio sink work)
    int skipTitleCutscene = 1; // 1: do not load/play the title screen's looping opening.cin background (it runs at ~4 FPS); PRESS START shows at once
    int cstickRange  = 0;     // C-stick raw travel that counts as full deflection (GameCube: 74); 0 = learn it from the nub (see n3ds_cstick.h)
    int cstickDead   = 12;    // C-stick raw radius below which the stick reads 0 (the GameCube game itself ignores < 32 of 74)
    int skipTitle    = 1;     // 1: skip the title screen - the first time it would appear, go straight to the file select (also implies no opening.cin background)
    int audioStats   = 0;     // 1: the game's own audio diagnostics every 2 s in the log: per-bus peaks, loudest voices, and DROPPED sounds (no instrument / wave system / wave / free voice)
    int gfxProfile   = 0;     // 1: diagnostics on every 60th frame (per-batch [draw] census, cold/warm decode passes). Each such frame takes ~3 s on hardware - only for bug reports
    int fpsCounter   = 1;     // 1: show the presented frames per second in the top right corner of the top screen
    int gpuXform     = 1;     // r53: 1 = cached models are transformed AND lit by the PICA vertex shader (n3ds_port/AGENTS.md 5); 0 = every vertex on the CPU worker (r52 and older)
    int gpuCull      = 1;     // r53: face culling on the GPU path: 1 = GX front = clockwise (as the CPU path), 2 = the opposite winding, 0 = off (both sides drawn)
    int abTest       = 0;     // r53: A/B profiling (n3ds_port/AGENTS.md 6): 1 = alternate ab_key between ab_a and ab_b every ab_frames frames and log [ab] lines
    int abKey        = 0;     //      which setting: 0 gpu_xform, 1 lighting, 2 fast_xform, 3 mesh_cpu, 4 gpu_split, 5 lod_model_px, 6 auto_lod, 7 cull_faces
    int abA          = 0;     //      value in phase A
    int abB          = 1;     //      value in phase B
    int abFrames     = 180;   //      frames per phase (r54: 180, more A/B pairs per minute) (the first ab_warmup of each phase are not measured)
    int abWarmup     = 60;
    int meshCacheKb  = 6144;  // memory (linear heap) the GPU-transform model cache may use; when full, further models take the CPU path
    int meshLimit    = 0;     // diagnostics: > 0 = at most this many GPU-path model draws per frame (the rest are dropped, nothing else draws them)
    int meshNoDraw   = 0;     // diagnostics: 1 = the GPU mesh path sets up everything (state, buffers, uniforms) but never issues the draw call
    int meshNoLit    = 0;     // diagnostics: 1 = GPU-path models never use the lighting branch / normal matrices
    int meshGuardPct = 300;   // GPU-path models must lie within +-this percent of the viewport (x/y) to be drawn by the GPU; 100 = fully on screen, partly off-screen chunks take the CPU path
    int meshDefer    = 0;     // gpu_xform = 1: GPU model draws are queued and issued together (before the next blended / depth-less ring draw, or at the end of the frame) instead of one by one between ring draws: tests whether the ring <-> mesh state switches mid-frame are what corrupts the picture
    int meshReplay   = 0;     // 1: a cached model with some chunks needing the CPU path is drawn chunk by chunk (GPU parts + CPU replay); 0: it is drawn wholly by the CPU path
    int meshVerify   = 0;     // 1: re-hash a cached model's list + vertex arrays on EVERY use (slow; diagnostics: finds models the game changes after they were cached)
    int meshCpu      = 1;     // 1: with gpu_xform = 0, models are decoded once into a cache and drawn from it by the CPU (much faster than re-decoding every display list each frame); 0 = old behaviour
    int meshCpuCheck = 0;     // 1: the cached CPU draw is compared with the normal CPU path on a model's first uses and now and then; a difference switches that model (or all) back to the normal path
    int triMinArea   = 0;     // LOSSY speed-up: triangles covering less than this many 1/100 pixel^2 on screen are not drawn (0 = off; 100 = under 1 px^2). Distant detail only
    int lodPx        = 20;    // LOSSY speed-up (cached CPU draw only): a chunk of a model whose whole screen box is smaller than this many 1/10 pixel in both directions is not drawn (0 = off; 20 = under 2 px). Needs mesh_cpu_check = 0
    int lodSimplifyPx = 32;   // r10: a cached model smaller than this many px on screen is drawn from a simplified copy (5x5x5 vertex clustering per matrix); 0 = off
    int fastXform    = 0;     // r20: default 0 (log c5c4d917: classic was faster). r18: cached models: transform the vertices of each drawn chunk four at a time up front (1) or one by one on first use (0, the old path)
    int pikiLodExempt = 1;    // r17: 1 = Pikmin are never size-culled or simplified (the Pikmin draw is marked by the game); 0 = they follow the LOD settings like everything else
    int lodFarPx     = 100;   // r9: like lod_model_px, for pieces away from the centre of the screen (the captain stays near the centre): 100 = under 10 px; 0 = same as lod_model_px
    int lodModelPx   = 40;    // LOSSY 'render distance' (cached CPU draw): a whole model whose screen box is smaller than this many 1/10 pixel in both directions is not drawn (0 = off; 40 = under 4 px). Needs mesh_cpu_check = 0 (the check is skipped while it is on)
    int maxDtMs      = 66;    // game-speed catch-up ("VBI skip" equivalent): the game never advances more than 1/30 s of game time per frame, so below 30 FPS it runs in slow motion (10 FPS = 1/3 speed, the day clock too). N (34..200) = allow up to N ms of game time per frame, i.e. real speed down to 1000/N FPS; 0 = the original 33.3 ms cap. Bigger steps = coarser physics/AI
    int gpuSplit     = 6000;  // PERFORMANCE (CPU transform path): hand the GPU the frame's command list in pieces of about this many ring vertices instead of all at once at the end, so the GPU renders while the CPU still records (0 = off; try 6000)
    int gfxLite      = 1;     // 1: cheap always-on [lite] log line once a second: display-list time, vertex counts and the 6 slowest lists (where the frame time goes)
    int meshSafe     = 1;     // 1: GPU path only for single-matrix models, drawn as plain triangle lists (DrawArrays, fixed shader registers); 0: also indexed + skinned (garbage on hardware so far)
    int meshDebug    = 0;     // 1 (with gpu_xform = 1): draw every cached model on the GPU path in solid magenta FIRST, depth off, then normally on the CPU path on top. Visible magenta = GPU-path geometry in the wrong place
    int pak          = 1;     // 1: read packed asset files from sdmc:/3ds/nectar/nectar.pak when it exists (tools/make_pak.py); 0: loose files only
    int pakVerify    = 0;     // 1: compare every byte read from nectar.pak with the loose file; differences are logged ("[pak] VERIFY") and the loose data is used
    int allocProf    = 0;     // 1: "[allocprof]" timing of malloc/calloc/free (costs ~2 kernel calls per allocation: FPS drops, diagnostics only)
    int texDiag      = 0;     // time the first N texture creations phase by phase (and decode twice to tell contention from real cost)
    int gfxQueueKb   = 2048;  // size of the game-thread -> render-worker command queue (linear heap), 128..4096 KB. 256 KB made the game thread wait on the worker up to 39 ms/frame (field logs)
    int bottomMap    = 2;     // r19: bottom screen during gameplay: 0 = log console, 1 = the game's radar map once the Whimsical Radar is found, 2 = the radar map always
    int bottomCounter = 1;    // r24: 1 = the Pikmin counters are shown on the bottom screen (with the map) instead of the top screen
    int lighting     = 1;     // r47: 1 = GameCube lighting (ambient + the game's lights, per vertex), 0 = simple headlight (older builds)
    int cullFaces    = 1;     // r40: 1 = honour GXSetCullMode (front / back faces), 0 = draw both sides of every triangle (old behaviour), 2 = on with the opposite winding
    int stretch      = 0;     // r27: 1 = stretch the 4:3 picture over the whole 400x240 top screen (no black bars at the sides)
    int unlockAreas  = 0;     // r26: cheat - every area can be picked on the area-select map (saved into the file the next time the game saves)
    int mapZoom      = 2;     // r19: bottom-screen map zoom (the radar's own steps): 2 = whole area, 4, 8 = close
    int autoLod      = 3;     // adaptive render distance, 0..5 = highest level (0 = off). The game starts with exactly lod_px / lod_model_px; while frames miss the fps_cap target AND the render worker's display lists are the cost, both
                              // thresholds are multiplied by 1.25 / 1.6 / 2 / 2.5 / 3.2 (one level per ~0.5 s), and given back one level at a time after ~3 s of easy frames. Never reacts when the game thread is the bottleneck (menus)
    int preset       = 0;     // r52: 1 = Quality, 2 = Speed, 3 = Bare minimum: sets the detail / lighting / FPS settings below as a bundle when nectar.ini is read; 0 = use the individual values
};

// r52: in-game presets. A preset is a bundle of the settings the bottom-screen menu changes live (draw distance, auto detail, simpler far models,
// skip tiny shapes, Pikmin detail, lighting, FPS limit, when FPS drops). Everything else (gpu_split, map, screen, ...) is left alone.
enum { N3DS_PRESET_CUSTOM = 0, N3DS_PRESET_QUALITY = 1, N3DS_PRESET_SPEED = 2, N3DS_PRESET_MINIMUM = 3, N3DS_PRESET_COUNT = 4 };
void n3dsPresetApply(N3dsConfig& c, int preset);           // no-op for 0 / out of range
int  n3dsPresetMatch(const N3dsConfig& c);                 // the preset the current values are exactly, 0 = none (custom)
const char* n3dsPresetName(int preset);

// Parses ini text into cfg. Returns the number of lines that were not understood (for the log).
int n3dsConfigParse(const char* text, N3dsConfig& cfg, char* badKeys, int badKeysSize);
// Loads "nectar.ini" from the current directory once (the game chdir()s to sdmc:/3ds/nectar) and logs what it found.
const N3dsConfig& n3dsConfig();
// r17: bumped by the in-game debug menu after it changes a setting (lets code that cached a setting re-read it)
extern volatile int gN3dsCfgGen;
