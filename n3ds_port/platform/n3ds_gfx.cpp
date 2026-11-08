// Game-side graphics backend: the pc_gfx_* API the game links against, implemented on the citro3d ring in n3ds_gpu.cpp.
// What works: geometry (immediate mode + display lists), position matrices, projection, viewport/scissor, depth, blend,
// alpha test, vertex/material colour, a fake headlight when normals exist, and (1k) textures + a GX TEV -> PICA combiner
// mapping for up to 6 stages (n3ds_tex.cpp decodes GameCube textures, n3ds_tev.cpp maps the combiner).
// What does NOT yet: fog, real lights, texture matrices / projective texgen, multitexture, backface culling, lines/points.
// A textured draw whose texture cannot be created is tinted by a colour hashed from its data pointer (debug aid).
// No <3ds.h> here -- see n3ds_bridge.h.
//
// 1j additions: (1) CPU trivial-reject of triangles that lie entirely outside the near/far planes or the scissor rect
// (log 1i: the title scene flooded the vertex ring, ~97k verts/frame accepted and ~3.8k prims/frame dropped);
// (2) a UI reserve: when the ring is nearly full only depth-test-off (2D/UI) draws may still take space, so a busy 3D
// scene can no longer starve the logo/menus that are drawn after it; (3) per-frame counters -> [gfx-perf] line.
//
// The GX numeric values used below are the Dolphin SDK ones (GX_VA_POS = 9, GX_TRIANGLES = 0x90, ...); they are
// written as plain numbers so a header rename cannot break this file.
#include "n3ds_bridge.h"
#ifndef N3DS_UV_FLIPV
#define N3DS_UV_FLIPV 1   /* set 0 (-DN3DS_UV_FLIPV=0) if textures come out right-side-up WITHOUT this */
#endif
#include "gl/pc_gfx.h"
#include "n3ds_tex.h"
#include "n3ds_tev.h"
#include "n3ds_config.h"
#include "n3ds_autolod.h"
extern "C" { int n3ds_gfxq_running(void); void n3dstex_rp_load_tlut(GXTlutObj* o, u32 name); void n3dstex_rp_release_texture(void* p); }
extern "C" { volatile int gN3dsMatProbeGen = 0; }
volatile int gN3dsEnvDbg = 0;   /* r40: L+R+RIGHT test switch (n3ds_ctru_io.cpp): 0 face culling as nectar.ini says, 1 culling off, 2 culling with the opposite winding */   /* r36: bumped by newPikiGame.cpp on every area load: the [mat] list restarts */
extern "C" { volatile int gN3dsPickX = 0, gN3dsPickY = 0, gN3dsPickReq = 0; }   /* r44: hold L and tap the bottom screen = log every triangle that covers that top-screen pixel next frame */
/* r45: enemy tagging (tekibteki.cpp wraps each enemy's model draw in n3ds_gfx_tag(type + 1)). Wollywog / Yellow Wollywog / Puffy Blowhog
   triangles are measured every frame; every 90 frames the centre of what they drew is picked automatically ([pick] lines). */
static int sTag = 0; static unsigned sTagGpu = 0;
static inline bool tagWatched(int t) { return t == 1 || t == 34 || t == 17; }   /* TEKI_Frog (Yellow Wollywog) 0, TEKI_Frow (Wollywog) 33, TEKI_Mar (Puffy Blowhog) 16, +1 */
static const char* tagName(int t) { return t == 1 ? "Yellow Wollywog" : t == 34 ? "Wollywog" : t == 17 ? "Puffy Blowhog" : "enemy"; }
struct TagAcc { double sx, sy; unsigned n; float x0, y0, x1, y1; int tag; } sTagAcc = {0, 0, 0, 1e9f, 1e9f, -1e9f, -1e9f, 0};
static int sTagGen = -1, sTagPicks = 0, sTagStates = 0; static uint32_t sTagSeen[48];
/* r37: triangles written to the ring per GPU texture id, [0] opaque / [1] blended; reported every 600 frames for the textures the [envtex] probe watches */
static uint32_t sTexTris[1024][2];
static uint16_t sEnvWatch[16]; static int sEnvWatchN = 0;
static bool sEnvWatchFlag[1024];
static void envWatchAdd(uint16_t id) { sEnvWatchFlag[id & 1023] = true; for (int i = 0; i < sEnvWatchN; ++i) if (sEnvWatch[i] == id) return; if (sEnvWatchN < 16) sEnvWatch[sEnvWatchN++] = id; }
/* r38: the PICA combiner on the CPU (same rules as the hardware: every stage clamps to 0..1). prim = vertex colour, tex = unit-0 texel. */
static void picaCpu(const N3dsTevStage* st, int n, const float prim[4], const float tex[4], float out[4], unsigned bufRgb = 0, unsigned bufA = 0)
{
    float prev[4] = {0, 0, 0, 0}, buf[4] = {0, 0, 0, 0};
    for (int s = 0; s < n; ++s) {
        const N3dsTevStage& t = st[s];
        const float k[4] = {(t.color & 255) / 255.0f, ((t.color >> 8) & 255) / 255.0f, ((t.color >> 16) & 255) / 255.0f, (t.color >> 24) / 255.0f};
        auto src = [&](int i) -> const float* { return i == N3DS_TS_PRIMARY ? prim : i == N3DS_TS_CONST ? k : i == N3DS_TS_PREV ? prev : i == N3DS_TS_BUFFER ? buf : tex; };
        float a[3][3], al[3];
        for (int j = 0; j < 3; ++j) {
            const float* v = src(t.rgbSrc[j]);
            for (int i = 0; i < 3; ++i) { const int op = t.rgbOp[j]; a[j][i] = op == N3DS_TO_C ? v[i] : op == N3DS_TO_1MC ? 1 - v[i] : op == N3DS_TO_A ? v[3] : 1 - v[3]; }
            const float* w = src(t.aSrc[j]); al[j] = (t.aOp[j] == N3DS_TO_1MA || t.aOp[j] == N3DS_TO_1MC) ? 1 - w[3] : w[3];
        }
        auto fn = [](int f, float x, float y, float z) { switch (f) { case N3DS_TFN_REPLACE: return x; case N3DS_TFN_MODULATE: return x * y; case N3DS_TFN_ADD: return x + y;
            case N3DS_TFN_INTERP: return x * z + y * (1 - z); case N3DS_TFN_MULADD: return x * y + z; default: return x - y; } };
        float o[4];
        for (int i = 0; i < 3; ++i) { float x = fn(t.rgbFn, a[0][i], a[1][i], a[2][i]) * t.rgbScale; o[i] = x < 0 ? 0 : x > 1 ? 1 : x; }
        { float x = fn(t.aFn, al[0], al[1], al[2]) * t.aScale; o[3] = x < 0 ? 0 : x > 1 ? 1 : x; }
        /* r49: the buffer stage s + 1 sees takes stage s - 1's output (update bit s - 1): one stage of delay, as the hardware does */
        if (s >= 1 && s - 1 < 4 && (bufRgb >> (s - 1) & 1)) { buf[0] = prev[0]; buf[1] = prev[1]; buf[2] = prev[2]; }
        if (s >= 1 && s - 1 < 4 && (bufA >> (s - 1) & 1)) buf[3] = prev[3];
        for (int i = 0; i < 4; ++i) prev[i] = o[i];
    }
    for (int i = 0; i < 4; ++i) out[i] = prev[i];
}
static int sEnvDrawGen = -1, sEnvDrawN = 0; static uint32_t sEnvDrawSeen[48];
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Finite test WITHOUT the VFP. __builtin_isfinite compiles to "vcmpe.f32 |x|, FLT_MAX" on ARM: a SIGNALLING compare, which on a NaN
   raises an Invalid Operation exception the 3DS kernel cannot handle -> Luma crash dump type 3 on the render worker (dumps 30 and 31,
   both with gpu_xform = 1, faulting instruction vcmpe.f32 s13, s15). Integer bit test instead: exponent all ones = inf or NaN. */
static inline bool fin(float x) { uint32_t u; memcpy(&u, &x, 4); return (u & 0x7F800000u) != 0x7F800000u; }
/* finite and |x| < 2^40: what the GPU path accepts in a matrix / uniform (float24 tops out near 2^64, and huge values are garbage anyway) */
static inline bool sane(float x) { uint32_t u; memcpy(&u, &x, 4); return ((u >> 23) & 0xFFu) < 127u + 40u; }
#include <atomic>

#ifndef N3DS_GFX_DEBUG
#define N3DS_GFX_DEBUG 1   /* log the first few draws (clip coords, state) so the next hardware run can confirm orientation */
#endif

namespace {

enum { A_PNMTXIDX = 0, A_TEX0MTXIDX = 1, A_POS = 9, A_NRM = 10, A_CLR0 = 11, A_CLR1 = 12, A_TEX0 = 13, A_COUNT = 21 };
enum { P_QUADS = 0x80, P_TRIANGLES = 0x90, P_STRIP = 0x98, P_FAN = 0xA0 };
enum { CHUNK = 1536 };   /* vertices per ring reservation (multiple of 6: whole triangles and quads) */

struct AttrFmt { uint8_t cnt, type, frac; };
struct Vin {                       /* one decoded GX vertex */
    float x, y, z, nx, ny, nz, u, v;
    uint8_t r, g, b, a, mtx;
    bool hasN, hasC;
    float u1, v1;                  /* second texture-coordinate set (GX_VA_TEX1) */
    float u2, v2;                  /* r35: third set (GX_VA_TEX2): the Forest Navel floor's third texture used TEX0's coordinates */
    uint8_t tmi[8];                /* per-vertex texture-matrix ids (GX_VA_TEX0MTXIDX..); 0 = not sent (texgen's own matrix id applies) */
};

enum { OC_L = 1, OC_R = 2, OC_B = 4, OC_T = 8, OC_NEAR = 16, OC_FAR = 32, OC_BAD = 64 };
struct VCEntry;
struct PVtx { N3dsVtx v; float px, py; uint8_t oc; bool ok; uint8_t lazy, mtx; const uint8_t* raw; VCEntry* ce;
             int32_t fx, fy; uint8_t nc, sp; };   /* r22: fx/fy = px/py * 256 truncated (sp = 1 when they are in range), nc = needs clipping (finishGeom) */
/* lazy = 1: only the position part of v is valid (x,y,z,w, px,py, oc, ok). The attributes (uv, colour, lighting) are decoded from `raw` by
   resolveLazy() when a triangle that actually survives culling uses the vertex. ~70 % of a busy 3D scene is rejected by the outcode test, so most
   vertices never pay for their normal / colour / texcoord decode. */   /* px,py: position in viewport pixels (valid when ok = w > 0) */   /* assembled vertex + homogeneous outcode (see outcode()) */

struct Gfx {
    uint8_t desc[A_COUNT];                 /* 0 none, 1 direct, 2 index8, 3 index16 */
    AttrFmt fmt[8][A_COUNT];
    const uint8_t* arrBase[A_COUNT];
    uint8_t arrStride[A_COUNT];

    float proj[16]; unsigned projGen;
    float pos[64][12]; bool posSet[64]; unsigned posGen[64];
    float nrm[64][12]; bool nrmSet[64];
    float clip[64][16]; unsigned clipProjGen[64], clipPosGen[64]; bool clipValid[64];
    unsigned cur;

    int zCmp; bool zWrite, cWrite, aWrite;
    int blendMode, blendSrc, blendDst;
    int alphaCmp, alphaRef;
    int cull;
    short vp[4], sc[4];                    /* upright screen px */

    uint8_t matRGB[3], matA; bool matVtxRGB, matVtxA, lit;
    /* r47: GameCube lighting (GXSetChanCtrl / GXSetChanAmbColor / GXLoadLightObjImm), evaluated per vertex like Dolphin.
       ch[0] = COLOR0 (rgb), ch[1] = ALPHA0. Lights are in view space (the game transforms them with the camera). */
    struct ChanCtl { bool en, ambVtx, matVtx; uint8_t mask, diff, attn; } ch[2];
    float amb[4];                                   /* ambient register of channel 0: r, g, b (COLOR0), a (ALPHA0), 0..1 */
    struct Light { float pos[3], dir[3], a[3], k[3], col[4]; } light[8];
    /* 1k: material = texture refs (snapshotted at GXLoadTexObj) + the TEV program; resolved lazily into the fields below */
    N3dsTexRef texRef[8]; N3dsTevIn tev; bool matDirty;
    uint16_t curTexId; float uvS, uvT; uint8_t curWrapS, curWrapT; bool texFailed; const void* failedData;
    /* texture coordinate generation (GXSetTexCoordGen) + texture matrices (GXLoadTexMtxImm): rows 0 and 1 of the 2x4 matrix, indexed by GX matrix id & 127 */
    float tm[128][8]; bool tm60Ident;
    uint8_t tgSrc[8], tgType[8], tgMtx[8]; uint8_t stageCoord[16];
    /* the PICA texture units the current program uses (resolveMaterial): which GX map each reads and how its coordinates are generated */
    struct UnitGen { uint8_t src, mtx, coord; bool plain; float su, sv; uint16_t tex; uint8_t wrapS, wrapT; } ug[3];
    int nUnits;
    N3dsTevOut tevOut; float tint[3]; bool tintOn;
    unsigned frameNo;

    /* current primitive */
    int prim; bool active;
    Vin iv; bool imm, haveV, immC;
    PVtx q[4]; int qn;                     /* assembler scratch */
    PVtx s0, s1; int sc_n;
    float cb[4]; bool cullOn; float hvw, hvh; int faceMode; float faceSign;   /* r40: face culling for the current primitive: 0 none, 1 drop front, 2 drop back, 3 drop all */   /* half viewport size in px */
                  /* scissor in viewport-NDC: x0, x1, y0(bottom), y1(top) */
};
Gfx G;

/* Ring reservations: CHUNK vertices at a time. While fewer than UI_RESERVE vertices are free, only depth-test-off draws
   (2D / UI / fades) may reserve, so the HUD and menus drawn after a heavy 3D scene still get through. */
enum { UI_RESERVE = 8 * CHUNK };

struct Stats {
    unsigned prims, ringDrops, uiDrops, emptyScissor, inVerts, tris, culled, bad, dlCalls, subpx, vcHits, lazyDone, clipped, bigLogged;
    unsigned faceCulled[2];   /* r40: triangles dropped by GXSetCullMode: [0] back, [1] front / all */
    unsigned lodSkipped;   /* tri_min_area: triangles dropped for covering less than the configured area (opt-in, lossy) */
    unsigned segTests, segCulled, segVertsSkipped, dlBuilt;   /* display-list segment culling (see DLInfo) */
    unsigned long long emitTicks, parseTicks, xformTicks;   /* display-list split: cold full pass / warm parse-only / warm parse+transform */
    double fillPx; unsigned tiny, big, nearClip;   /* profiled frames only: estimated covered pixels, size classes */
} S;
/* Draw census (profiled frames only): one [draw] line per primitive batch, so a missing object can be found by where it should be
   (bbox is the upright NDC box of what was actually emitted; verts/tris/culled/subpx/clip are the batch's own deltas). */
struct Census { bool open; int idx, prim; unsigned tex; bool texFail; int tevN, approx, bm, bs, bd, zc, cull, ac, ar; bool zw, lit; short vp[4];
                unsigned in0, tris0, culled0, sub0, clip0; float bx0, by0, bx1, by1, u0, v0, u1, v1; uint8_t rgba[4]; bool haveRgba; };
Census sCen; int sCenIdx = 0, sCenPrinted = 0;
struct CenGroup { unsigned tex; int tev; int bm; unsigned draws, verts, culled, subpx; };   /* batches that emitted nothing, grouped by material */
CenGroup sCenGrp[48]; int sCenNGrp = 0;
int sDbgSub = 0;          /* diagnostic: immediate-mode (2D UI) triangles logged as sub-pixel on this profiled frame */
bool sProfile = false;   /* time the display-list decoder on this frame (svcGetSystemTick per call is not free) */

N3dsVtx* outP = nullptr; int outN = 0; N3dsDrawState outSt;
float tilt[4]; bool tiltOk = false;
int dbgDraws = 0, dbgTris = 0;
bool dlWarned = false;

constexpr bool kPillar =
#ifdef N3DS_STRETCH
    false;
#else
    true;
#endif

/* ---- GX 640x480 EFB px -> upright top-screen px --------------------------------------------------------------- */
int sOnBottom = 0;                                  /* r19: drawing the bottom-screen map (320x240, no pillarbox) */
int sStretch = 0;                                   /* r27: nectar.ini stretch / the settings menu: 4:3 picture stretched over all 400 px of the top screen */
float sRawVp[4] = {0, 0, 640, 480}, sRawSc[4] = {0, 0, 640, 480};   /* last GX viewport / scissor, re-mapped when the screen changes */
void gxRectToScreen(float x, float y, float w, float h, short out[4])
{
    const bool pil = kPillar && !sOnBottom && !sStretch;
    const float sx = pil ? 0.5f : (sOnBottom ? 0.5f : 400.0f / 640.0f), ox = pil ? 40.0f : 0.0f, sy = 0.5f;
    const float lo = pil ? 40.0f : 0.0f, hi = pil ? 360.0f : (sOnBottom ? 320.0f : 400.0f);
    float x0 = ox + x * sx, x1 = ox + (x + w) * sx, y0 = y * sy, y1 = (y + h) * sy;
    if (x0 < lo) x0 = lo; if (x1 > hi) x1 = hi; if (x1 < x0) x1 = x0;
    if (y0 < 0) y0 = 0;   if (y1 > 240) y1 = 240; if (y1 < y0) y1 = y0;
    out[0] = (short)floorf(x0); out[1] = (short)floorf(y0); out[2] = (short)ceilf(x1); out[3] = (short)ceilf(y1);
}

N3dsDrawState currentState()
{
    N3dsDrawState s; memset(&s, 0, sizeof s);
    s.depthCmp = (uint8_t)G.zCmp;
    s.writeMask = (uint8_t)((G.cWrite ? 1 : 0) | (G.aWrite ? 2 : 0) | (G.zWrite ? 4 : 0));
    s.blendMode = (uint8_t)G.blendMode; s.blendSrc = (uint8_t)G.blendSrc; s.blendDst = (uint8_t)G.blendDst;
    s.alphaCmp = (uint8_t)G.alphaCmp; s.alphaRef = (uint8_t)G.alphaRef; s.cull = (uint8_t)G.cull;
    s.vpX0 = G.vp[0]; s.vpY0 = G.vp[1]; s.vpX1 = G.vp[2]; s.vpY1 = G.vp[3];
    s.scX0 = G.sc[0]; s.scY0 = G.sc[1]; s.scX1 = G.sc[2]; s.scY1 = G.sc[3];
    s.texId = G.curTexId; s.texWrapS = G.curWrapS; s.texWrapT = G.curWrapT;
    if (G.nUnits > 1) { s.tex1Id = G.ug[1].tex; s.tex1WrapS = G.ug[1].wrapS; s.tex1WrapT = G.ug[1].wrapT; }
    if (G.nUnits > 2) { s.tex2Id = G.ug[2].tex; s.tex2WrapS = G.ug[2].wrapS; s.tex2WrapT = G.ug[2].wrapT; }
    s.nTev = (uint8_t)G.tevOut.n;
    for (int i = 0; i < G.tevOut.n && i < 6; ++i) s.tev[i] = G.tevOut.st[i];
    s.tevBufRgb = G.tevOut.bufRgb; s.tevBufA = G.tevOut.bufA;
    /* r46 (from ikevik/sms-3ds): a perspective surface depth-TESTED (less / lequal) but not depth-WRITTEN lies on what is under it (decals,
       water, shadows). The PICA's float24 depth makes such pairs fight in distant bands; it is drawn a hair toward the camera. */
    s.depthBias = (uint8_t)((G.zCmp == 1 || G.zCmp == 3) && !G.zWrite && G.proj[15] == 0.0f ? 1 : 0);
    return s;
}

/* ---- ring output --------------------------------------------------------------------------------------------- */
/* gfx_lite worker timeline (see liteReport): wall ticks spent in the GPU submit (applyState + C3D_DrawArrays), in resolveMaterial (texture lookup, TEV program
   build) and in the model-cache lookup (signature checks, verify hash, rebuilds), summed between two [lite2] lines. Worker thread only. */
unsigned long long sTmCommit = 0, sTmMat = 0, sTmLookup = 0; unsigned sTmCommitN = 0, sTmMatN = 0, sTmLookupN = 0;
unsigned long long sTmSetup = 0, sTmLoop = 0; unsigned sTmBodyN = 0, sTmBodyTris = 0, sTmBodyVerts = 0;   /* r16: cached-draw split (setup before the triangle loop / the loop) */
unsigned long long sTmCls[4] = {0, 0, 0, 0};   /* worker wall ticks per kind of command: 0 display lists, 1 immediate mode (begin..end), 2 state calls, 3 frame end + present */
void flushOut()
{
    if (outP && outN > 0) {
        if (n3dsConfig().gfxLite) { const unsigned long long t0 = n3ds_ticks(); n3ds_gpu_commit(outN, &outSt); sTmCommit += n3ds_ticks() - t0; ++sTmCommitN; }
        else n3ds_gpu_commit(outN, &outSt);
    }
    outP = nullptr; outN = 0;
}
bool reserveOut()
{
    outN = 0;
    if (outSt.depthCmp != 7 && n3ds_gpu_ring_free() < UI_RESERVE) { outP = nullptr; ++S.uiDrops; return false; }   /* 3D draw, ring nearly full */
    outP = n3ds_gpu_reserve(CHUNK);
    if (!outP) ++S.ringDrops;
    return outP != nullptr;
}

/* Profiled frames only: rough screen coverage of one emitted triangle (NDC area x viewport half-extents; the tilt is a
   rotation so area is unchanged). NDC is clamped to +-1.5 so one huge triangle cannot dominate the estimate. Overdraw is
   counted, which is exactly what the GPU pays for. Triangles with a vertex at w <= 0 (crossing the near plane) are only
   counted, not measured. */
void profileTri(const N3dsVtx& a, const N3dsVtx& b, const N3dsVtx& c)
{
    if (a.w <= 1e-6f || b.w <= 1e-6f || c.w <= 1e-6f) { ++S.nearClip; return; }
    auto cl = [](float v) { return v < -1.5f ? -1.5f : v > 1.5f ? 1.5f : v; };
    const float ax = cl(a.x / a.w), ay = cl(a.y / a.w), bx = cl(b.x / b.w), by = cl(b.y / b.w), cx = cl(c.x / c.w), cy = cl(c.y / c.w);
    const float ndcArea = 0.5f * fabsf((bx - ax) * (cy - ay) - (cx - ax) * (by - ay));
    const float px = ndcArea * 0.25f * (float)(G.vp[2] - G.vp[0]) * (float)(G.vp[3] - G.vp[1]);   /* NDC spans 2x2 */
    S.fillPx += px;
    if (px < 2.0f) ++S.tiny;
    else if (px > 20000.0f) {
        ++S.big;
        if (S.bigLogged < 8) {   /* what is the giant flat polygon of the cutscenes? log the material and corners of the first few */
            ++S.bigLogged;
            n3ds_log("[gfx-big] ~%.0f px tri | tex %u (%s) tev %d stages, tint %d | blend %d/%d/%d z %d/%s cull %d alpha %d>%d | rgba %u,%u,%u,%u / %u,%u,%u,%u / %u,%u,%u,%u | "
                     "ndc (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f) w %.1f %.1f %.1f | mtx %u lit %d prim %02x vp %d,%d-%d,%d",
                     px, (unsigned)G.curTexId, G.texFailed ? "FAILED" : "ok", G.tevOut.n, (int)G.tintOn, G.blendMode, G.blendSrc, G.blendDst, G.zCmp, G.zWrite ? "w" : "-", G.cull,
                     G.alphaCmp, G.alphaRef, a.r, a.g, a.b, a.a, b.r, b.g, b.b, b.a, c.r, c.g, c.b, c.a, ax, ay, bx, by, cx, cy, a.w, b.w, c.w,
                     (unsigned)G.cur, (int)G.lit, G.prim, G.vp[0], G.vp[1], G.vp[2], G.vp[3]);
        }
    }
}

void resolveLazy(PVtx& v);   /* decodes a lazy vertex's attributes (defined with the display-list decoder) */

/* r38 probe (Wollywog black although its texture, coordinates and TEV program check out): for each distinct draw state that samples a
   sphere-mapped texture, once per area, the first vertex as it goes to the GPU and the pixel the combiner should give for it (CPU-evaluated
   with the GC texel at its coordinate), with blend / depth / alpha state. If that pixel is bright, whatever makes it black happens on the GPU side. */
static void envDrawProbe(const N3dsVtx& a)
{
    if (sEnvDrawGen != gN3dsMatProbeGen) { sEnvDrawGen = gN3dsMatProbeGen; sEnvDrawN = 0; }
    if (sEnvDrawN >= 48) return;
    uint32_t h = 2166136261u; const uint8_t* bp = (const uint8_t*)&outSt; for (unsigned k = 0; k < sizeof outSt; ++k) if (k < offsetof(N3dsDrawState, vpX0) || k >= offsetof(N3dsDrawState, texId)) h = (h ^ bp[k]) * 16777619u;
    for (int i = 0; i < sEnvDrawN; ++i) if (sEnvDrawSeen[i] == h) return;
    sEnvDrawSeen[sEnvDrawN++] = h;
    const N3dsTexRef& tr = G.texRef[G.tev.unitMap[0] & 7];
    const float su = G.ug[0].su > 0 ? G.ug[0].su : 1.0f, sv = G.ug[0].sv > 0 ? G.ug[0].sv : 1.0f;
    const uint32_t tx = n3dstexSampleGc(tr, a.u / su, a.v / sv);
    const float prim[4] = {a.r / 255.0f, a.g / 255.0f, a.b / 255.0f, a.a / 255.0f};
    const float tex[4] = {(tx & 255) / 255.0f, ((tx >> 8) & 255) / 255.0f, ((tx >> 16) & 255) / 255.0f, (tx >> 24) / 255.0f};
    float px[4]; picaCpu(outSt.tev, outSt.nTev, prim, tex, px, outSt.tevBufRgb, outSt.tevBufA);
    char st[200]; int o = 0; st[0] = 0;
    for (int i = 0; i < outSt.nTev && i < 6; ++i) { const N3dsTevStage& t = outSt.tev[i];
        o += snprintf(st + o, sizeof st - o, " %d:%d%d%d/%d%d%d f%d x%d|%d%d%d f%d x%d k%08x", i, t.rgbSrc[0], t.rgbSrc[1], t.rgbSrc[2], t.rgbOp[0], t.rgbOp[1], t.rgbOp[2], t.rgbFn, t.rgbScale,
                      t.aSrc[0], t.aSrc[1], t.aSrc[2], t.aFn, t.aScale, t.color); if (o >= (int)sizeof st) break; }
    n3ds_log("[envdraw] tex %u: vertex rgba %u,%u,%u,%u uv %.3f,%.3f -> GC texel %u,%u,%u,%u -> combiner gives %.0f,%.0f,%.0f,%.0f | blend %u %u/%u alpha %u>%u z %u mask %u cull %u | z/w %.4f w %.3f | mat %u,%u,%u,%u lit %d |%s",
             (unsigned)outSt.texId, a.r, a.g, a.b, a.a, a.u, a.v, (unsigned)(tx & 255), (unsigned)((tx >> 8) & 255), (unsigned)((tx >> 16) & 255), (unsigned)(tx >> 24),
             px[0] * 255, px[1] * 255, px[2] * 255, px[3] * 255, outSt.blendMode, outSt.blendSrc, outSt.blendDst, outSt.alphaCmp, outSt.alphaRef, outSt.depthCmp, outSt.writeMask, outSt.cull,
             a.w != 0 ? a.z / a.w : 0.0f, a.w, G.matRGB[0], G.matRGB[1], G.matRGB[2], G.matA, (int)G.lit, st);
}

/* r41 probe: which draw paints BLACK? Once per primitive (first triangle), the combiner is evaluated on the CPU for its first vertex; every
   distinct draw state whose pixel comes out (near) black and is written to the colour buffer is logged once per area, with its material. */
static bool sDarkCheck = false; static int sDarkGen = -1, sDarkN = 0; static uint32_t sDarkSeen[64];
static void darkProbe(const N3dsVtx& a)
{
    if (sDarkGen != gN3dsMatProbeGen) { sDarkGen = gN3dsMatProbeGen; sDarkN = 0; }
    if (sDarkN >= 64 || !(outSt.writeMask & 1)) return;
    float tex[4] = {1, 1, 1, 1};
    if (G.nUnits > 0) {
        const N3dsTexRef& tr = G.texRef[G.tev.unitMap[0] & 7];
        const float su = G.ug[0].su > 0 ? G.ug[0].su : 1.0f, sv = G.ug[0].sv > 0 ? G.ug[0].sv : 1.0f;
        const uint32_t tx = n3dstexSampleGc(tr, a.u / su, a.v / sv);
        tex[0] = (tx & 255) / 255.0f; tex[1] = ((tx >> 8) & 255) / 255.0f; tex[2] = ((tx >> 16) & 255) / 255.0f; tex[3] = (tx >> 24) / 255.0f;
    }
    const float prim[4] = {a.r / 255.0f, a.g / 255.0f, a.b / 255.0f, a.a / 255.0f};
    float px[4]; picaCpu(outSt.tev, outSt.nTev, prim, tex, px, outSt.tevBufRgb, outSt.tevBufA);
    if (px[0] > 0.1f || px[1] > 0.1f || px[2] > 0.1f) return;
    if (outSt.blendMode == 1 && outSt.blendSrc == 4 && px[3] < 0.3f) return;   /* faint alpha-blended: not what paints a solid black shape */
    uint32_t h = 2166136261u; const uint8_t* bp = (const uint8_t*)&outSt; for (unsigned k = 0; k < sizeof outSt; ++k) if (k < offsetof(N3dsDrawState, vpX0) || k >= offsetof(N3dsDrawState, texId)) h = (h ^ bp[k]) * 16777619u;
    h = (h ^ (uint32_t)(G.matRGB[0] | G.matRGB[1] << 8 | G.matRGB[2] << 16)) * 16777619u;
    for (int i = 0; i < sDarkN; ++i) if (sDarkSeen[i] == h) return;
    sDarkSeen[sDarkN++] = h;
    char st[160]; int o = 0; st[0] = 0;
    for (int i = 0; i < G.tev.nStages && i < 4; ++i) { const N3dsTevRaw& r = G.tev.st[i];
        o += snprintf(st + o, sizeof st - o, " s%d[ch%d C%d,%d,%d,%d A%d,%d,%d,%d]", i, r.chan == 255 ? -1 : r.chan, r.ci[0], r.ci[1], r.ci[2], r.ci[3], r.ai[0], r.ai[1], r.ai[2], r.ai[3]); if (o >= (int)sizeof st) break; }
    const N3dsTexRef* t0 = G.nUnits > 0 ? &G.texRef[G.tev.unitMap[0] & 7] : nullptr;
    n3ds_log("[dark] draw paints %.0f,%.0f,%.0f a%.0f | vertex %u,%u,%u,%u z/w %.4f w %.1f | tex %u %ux%u f%u %p src %d | mat %u,%u,%u,%u lit %d regs %08x %08x %08x | blend %u %u/%u alpha %u>%u z %u mask %u cull %d |%s",
             px[0] * 255, px[1] * 255, px[2] * 255, px[3] * 255, a.r, a.g, a.b, a.a, a.w != 0 ? a.z / a.w : 0.0f, a.w,
             (unsigned)outSt.texId, t0 ? t0->w : 0, t0 ? t0->h : 0, t0 ? t0->fmt : 0, t0 ? t0->data : nullptr, G.nUnits > 0 ? G.ug[0].src : -1,
             G.matRGB[0], G.matRGB[1], G.matRGB[2], G.matA, (int)G.lit, (unsigned)G.tev.reg[1], (unsigned)G.tev.reg[2], (unsigned)G.tev.reg[3],
             outSt.blendMode, outSt.blendSrc, outSt.blendDst, outSt.alphaCmp, outSt.alphaRef, outSt.depthCmp, outSt.writeMask, G.cull, st);
}

/* Write one finished triangle into the ring. Returns false when no ring space could be reserved (the primitive is then dropped). */
bool putTri(const N3dsVtx& a, const N3dsVtx& b, const N3dsVtx& c)
{
    /* Crash dump 18 (data abort, FAR 0, worker thread): flushOut() sets outP = nullptr, and the frame-end flush (Q_FRAME) can run while
       G.active is still true from the previous primitive, so a triangle then wrote through a null outP. Reserve lazily whenever there is
       no valid reservation; the draw state is re-read because outSt belongs to the primitive that was flushed. */
    if (!outP) {
        outSt = currentState();
        if (!reserveOut()) { G.active = false; return false; }
    } else if (outN + 3 > CHUNK) {
        flushOut();
        if (!reserveOut()) { G.active = false; return false; }
    }
    if (sProfile) {
        profileTri(a, b, c);
        const N3dsVtx* const tri3[3] = {&a, &b, &c};
        if (sCen.open) for (int ti = 0; ti < 3; ++ti) if (tri3[ti]->w > 1e-6f) {
            const N3dsVtx* q = tri3[ti];
            const float ox = q->x / q->w, oy = q->y / q->w, ux = tilt[0] * ox + tilt[2] * oy, uy = tilt[1] * ox + tilt[3] * oy;   /* inverse of the 90-degree tilt */
            if (q->u < sCen.u0) sCen.u0 = q->u; if (q->u > sCen.u1) sCen.u1 = q->u; if (q->v < sCen.v0) sCen.v0 = q->v; if (q->v > sCen.v1) sCen.v1 = q->v;
            if (!sCen.haveRgba) { sCen.haveRgba = true; sCen.rgba[0] = q->r; sCen.rgba[1] = q->g; sCen.rgba[2] = q->b; sCen.rgba[3] = q->a; }
            if (ux < sCen.bx0) sCen.bx0 = ux; if (ux > sCen.bx1) sCen.bx1 = ux; if (uy < sCen.by0) sCen.by0 = uy; if (uy > sCen.by1) sCen.by1 = uy;
        }
    }
    __builtin_prefetch(&outP[outN + 9]);      /* PLD: the ring is written sequentially and every line is a cache miss (main memory moves ~390 MB/s on this CPU) */
    outP[outN++] = a; outP[outN++] = b; outP[outN++] = c;
    ++S.tris; ++sTexTris[outSt.texId & 1023][outSt.blendMode != 0];
    if (sEnvWatchFlag[outSt.texId & 1023]) envDrawProbe(a);
    if (sDarkCheck) { sDarkCheck = false; darkProbe(a); }
    return true;
}

/* ---- clipping ------------------------------------------------------------------------------------------------------
   The PICA rasteriser only clips against the near plane and has a limited screen-space coordinate range, so a triangle that crosses the
   camera plane or reaches far outside the viewport (cutscene cameras sit inside huge sky/ground meshes: log 1n showed 70 near-crossing and
   85 "big" triangles per frame, drawn as one giant flat polygon) is clipped here, in homogeneous clip space, before it reaches the GPU.
   Planes: near (z + w >= 0, the GX/PICA convention used throughout) and a guard band of +-GUARD viewports in x and y. Clip-space linear
   interpolation of every attribute is exactly perspective-correct. */
static inline float guardV() { static float g = -1.0f; if (g < 0.0f) g = (float)n3dsConfig().meshGuardPct * 0.01f; return g; }   /* read once: needsClip() calls this ~15 times per triangle */   /* nectar.ini mesh_guard (percent of a viewport), default 300 */
enum { CLIP_MAX = 12 };
inline float clipDist(const N3dsVtx& v, int plane)
{
    switch (plane) {
    case 0: return v.z + v.w + 1e-6f * fabsf(v.w);   /* tiny tolerance: flat UI quads sit exactly on the near plane */
    case 1: return guardV() * v.w - v.x;
    case 2: return guardV() * v.w + v.x;
    case 3: return guardV() * v.w - v.y;
    default: return guardV() * v.w + v.y;
    }
}
inline bool needsClip(const N3dsVtx& v)
{
    return v.z + v.w + 1e-6f * fabsf(v.w) < 0.0f || v.x > guardV() * v.w || v.x < -guardV() * v.w || v.y > guardV() * v.w || v.y < -guardV() * v.w;
}
inline uint8_t lerpByte(uint8_t a, uint8_t b, float t) { const float f = (float)a + ((float)b - (float)a) * t + 0.5f; return f <= 0.0f ? 0 : f >= 255.0f ? 255 : (uint8_t)f; }
inline N3dsVtx lerpVtx(const N3dsVtx& a, const N3dsVtx& b, float t)
{
    N3dsVtx o;
    o.x = a.x + (b.x - a.x) * t; o.y = a.y + (b.y - a.y) * t; o.z = a.z + (b.z - a.z) * t; o.w = a.w + (b.w - a.w) * t;
    o.u = a.u + (b.u - a.u) * t; o.v = a.v + (b.v - a.v) * t;
    o.u1 = a.u1 + (b.u1 - a.u1) * t; o.v1 = a.v1 + (b.v1 - a.v1) * t; o.u2 = a.u2 + (b.u2 - a.u2) * t; o.v2 = a.v2 + (b.v2 - a.v2) * t;
    o.r = lerpByte(a.r, b.r, t); o.g = lerpByte(a.g, b.g, t); o.b = lerpByte(a.b, b.b, t); o.a = lerpByte(a.a, b.a, t);
    return o;
}

void clipAndEmit(const N3dsVtx& a, const N3dsVtx& b, const N3dsVtx& c)
{
    ++S.clipped;
    N3dsVtx bufA[CLIP_MAX], bufB[CLIP_MAX];
    N3dsVtx* in = bufA; N3dsVtx* out = bufB; int n = 3;
    in[0] = a; in[1] = b; in[2] = c;
    for (int pl = 0; pl < 5 && n >= 3; ++pl) {
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const N3dsVtx& p = in[i]; const N3dsVtx& q = in[i + 1 == n ? 0 : i + 1];
            const float dp = clipDist(p, pl), dq = clipDist(q, pl);
            if (dp >= 0.0f) { if (m < CLIP_MAX) out[m++] = p; }
            if ((dp >= 0.0f) != (dq >= 0.0f)) { if (m < CLIP_MAX) out[m++] = lerpVtx(p, q, dp / (dp - dq)); }
        }
        N3dsVtx* t = in; in = out; out = t; n = m;
    }
    for (int i = 1; i + 1 < n; ++i) if (!putTri(in[0], in[i], in[i + 1])) return;   /* fan keeps the winding of the source triangle */
}

static inline float minArea() { static float a = -1.0f; static int g = -1; if (g != gN3dsCfgGen) { g = gN3dsCfgGen; a = (float)n3dsConfig().triMinArea * 0.01f; } return a; }   /* r19: re-read after the in-game menu changes it */   /* nectar.ini tri_min_area (1/100 pixel), default 0 = off */
/* floorf / ceilf without the libm call (the ARM11 VFP has no round-to-integer instruction): exact for |x| < 2^22, libm otherwise (and for NaN). */
static inline float fastFloor(float x) { if (!(fabsf(x) < 4194304.0f)) return floorf(x); const int i = (int)x; return (float)(i - (x < (float)i)); }
static inline float fastCeil(float x)  { if (!(fabsf(x) < 4194304.0f)) return ceilf(x);  const int i = (int)x; return (float)(i + (x > (float)i)); }
static bool sChkOn = false; static unsigned sChkT = 0; static uint32_t sChkH = 2166136261u;
enum { CAP_MAX = 4096 }; static float sCap[CAP_MAX][4]; static unsigned sCapN = 0; static bool sCapOn = false;   /* mesh_debug = 13: clip-space vertices the CPU path emitted */
/* r20: emitTri split in two so the phase profiler ([lite4]) can time the reject tests and the output separately: triReject() = every test that
   drops a triangle before its attributes are needed (true = dropped), emitKept() = attribute decode + clip + ring write of a surviving one. */
static inline bool triReject(const PVtx& pa, const PVtx& pb, const PVtx& pc)
{
    if ((pa.oc | pb.oc | pc.oc) & OC_BAD) { ++S.bad; return true; }        /* NaN / inf: never hand these to the GPU */
    if (G.faceMode) {                                                      /* r40: GXSetCullMode (see primBegin) */
        if (G.faceMode == 3) { ++S.faceCulled[1]; return true; }
        const float x0 = pa.v.x, y0 = pa.v.y, w0 = pa.v.w, x1 = pb.v.x, y1 = pb.v.y, w1 = pb.v.w, x2 = pc.v.x, y2 = pc.v.y, w2 = pc.v.w;
        const float d = ((x0 * w2 - x2 * w0) * y1 + (x2 * y0 - x0 * y2) * w1 + (y2 * w0 - y0 * w2) * x1) * G.faceSign;
        if (G.faceMode == 2 ? d > 0.0f : d <= 0.0f) { ++S.faceCulled[G.faceMode == 2 ? 0 : 1]; return true; }
    }
    if (pa.oc & pb.oc & pc.oc) { ++S.culled; return true; }                /* entirely outside one clip plane */
    if (pa.ok & pb.ok & pc.ok) {
        /* Lossless: the rasteriser only lights pixels whose CENTRE is inside the triangle. If the padded bounding box holds no pixel centre in x
           or in y, the triangle draws nothing. Log 1j: ~85 % of the title scene's triangles were < 2 px.
           r22: integer test on the 1/256 px fixed-point copies (no float compares / conversions): the true position lies within +-1 unit of the
           truncated value, the box is widened by that and by 6/256 px (> the old 0.02 px pad), so it only ever rejects LESS than the float test did. */
        bool sub = false;
        if (pa.sp & pb.sp & pc.sp) {
            int32_t mnx = pa.fx < pb.fx ? pa.fx : pb.fx, mxx = pa.fx < pb.fx ? pb.fx : pa.fx;
            int32_t mny = pa.fy < pb.fy ? pa.fy : pb.fy, mxy = pa.fy < pb.fy ? pb.fy : pa.fy;
            mnx = pc.fx < mnx ? pc.fx : mnx; mxx = pc.fx > mxx ? pc.fx : mxx; mny = pc.fy < mny ? pc.fy : mny; mxy = pc.fy > mxy ? pc.fy : mxy;
            /* a centre (256k + 128) exists in [lo, hi] <=> ceil((lo - 128) / 256) <= floor((hi - 128) / 256) */
            const int32_t lx = mnx - 7 - 128, hx = mxx + 7 - 128, ly = mny - 7 - 128, hy = mxy + 7 - 128;
            sub = (-((-lx) >> 8)) > (hx >> 8) || (-((-ly) >> 8)) > (hy >> 8);
        }
        if (sub) {
            ++S.subpx;
            if (sProfile && G.imm && sDbgSub < 4) {      /* why is a 2D quad "too small"? log its projected pixels and the state behind them */
                ++sDbgSub;
                const float* P = G.proj; const float* M = G.pos[G.cur & 63];
                n3ds_log("[gfx-dbg] imm tri rejected as sub-pixel: prim=%d cur=%u | px (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f) | w %.4f %.4f %.4f | hvw/hvh %.1f/%.1f | "
                         "vp %d,%d-%d,%d sc %d,%d-%d,%d | proj diag %.5g %.5g %.5g %.5g trans %.5g %.5g %.5g | pos[cur] set=%d diag %.5g %.5g %.5g trans %.5g %.5g",
                         G.prim, G.cur, pa.px, pa.py, pb.px, pb.py, pc.px, pc.py, pa.v.w, pb.v.w, pc.v.w, G.hvw, G.hvh,
                         G.vp[0], G.vp[1], G.vp[2], G.vp[3], G.sc[0], G.sc[1], G.sc[2], G.sc[3],
                         P[0], P[5], P[10], P[15], P[3], P[7], P[11], (int)G.posSet[G.cur & 63], M[0], M[5], M[10], M[3], M[7]);
            }
            return true;
        }
        if (minArea() > 0.0f) {   /* tri_min_area (opt-in, lossy): distant geometry made of triangles smaller than a pixel or two costs the CPU and the GPU a lot and adds almost nothing */
            const float cr = (pb.px - pa.px) * (pc.py - pa.py) - (pc.px - pa.px) * (pb.py - pa.py);
            if (fabsf(cr) * 0.5f < minArea()) { ++S.lodSkipped; return true; }
        }
    }
    return false;
}
static bool sPickOn = false; static unsigned sPickN = 0;
static void pickProbe(const PVtx& pa, const PVtx& pb, const PVtx& pc)
{
    if (sOnBottom || !(pa.ok && pb.ok && pc.ok)) return;
    const float X = (float)gN3dsPickX + 0.5f - (float)G.vp[0], Y = (float)gN3dsPickY + 0.5f - (float)G.vp[1];
    const float x0 = pa.px, y0 = pa.py, x1 = pb.px, y1 = pb.py, x2 = pc.px, y2 = pc.py;
    const float d = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (fabsf(d) < 1e-6f) return;
    const float l1 = ((X - x0) * (y2 - y0) - (x2 - x0) * (Y - y0)) / d, l2 = ((x1 - x0) * (Y - y0) - (X - x0) * (y1 - y0)) / d, l0 = 1.0f - l1 - l2;
    if (l0 < 0 || l1 < 0 || l2 < 0) return;
    ++sPickN;
    if (sPickN > 80) return;
    const N3dsVtx &a = pa.v, &b = pb.v, &c = pc.v;
    const float prim[4] = {(l0 * a.r + l1 * b.r + l2 * c.r) / 255.0f, (l0 * a.g + l1 * b.g + l2 * c.g) / 255.0f, (l0 * a.b + l1 * b.b + l2 * c.b) / 255.0f, (l0 * a.a + l1 * b.a + l2 * c.a) / 255.0f};
    const float u = l0 * a.u + l1 * b.u + l2 * c.u, v = l0 * a.v + l1 * b.v + l2 * c.v;
    const float zw = l0 * (a.z / a.w) + l1 * (b.z / b.w) + l2 * (c.z / c.w);
    float tex[4] = {1, 1, 1, 1}; uint32_t tx = 0xFFFFFFFFu;
    const N3dsTexRef* t0 = G.nUnits > 0 ? &G.texRef[G.tev.unitMap[0] & 7] : nullptr;
    if (t0) {
        const float su = G.ug[0].su > 0 ? G.ug[0].su : 1.0f, sv = G.ug[0].sv > 0 ? G.ug[0].sv : 1.0f;
        tx = n3dstexSampleGc(*t0, u / su, v / sv);
        tex[0] = (tx & 255) / 255.0f; tex[1] = ((tx >> 8) & 255) / 255.0f; tex[2] = ((tx >> 16) & 255) / 255.0f; tex[3] = (tx >> 24) / 255.0f;
    }
    const N3dsDrawState st = currentState();
    float px[4]; picaCpu(st.tev, st.nTev, prim, tex, px, st.tevBufRgb, st.tevBufA);
    char sb[160]; int o = 0; sb[0] = 0;
    for (int i = 0; i < G.tev.nStages && i < 4; ++i) { const N3dsTevRaw& r = G.tev.st[i];
        o += snprintf(sb + o, sizeof sb - o, " s%d[ch%d C%d,%d,%d,%d]", i, r.chan == 255 ? -1 : r.chan, r.ci[0], r.ci[1], r.ci[2], r.ci[3]); if (o >= (int)sizeof sb) break; }
    n3ds_log("[pick] #%u prim %u tag %d: colour %.0f,%.0f,%.0f a%.0f | z/w %.5f | vertex %.0f,%.0f,%.0f,%.0f uv %.3f,%.3f texel %08x | tex %u %ux%u f%u %p src %d | mat %u,%u,%u,%u lit %d regs %08x %08x %08x | blend %u %u/%u alpha %u>%u z %u mask %u cull %d/%d |%s",
             sPickN, S.prims, sTag, px[0] * 255, px[1] * 255, px[2] * 255, px[3] * 255, zw, prim[0] * 255, prim[1] * 255, prim[2] * 255, prim[3] * 255, u, v, (unsigned)tx,
             (unsigned)st.texId, t0 ? t0->w : 0, t0 ? t0->h : 0, t0 ? t0->fmt : 0, t0 ? t0->data : nullptr, G.nUnits > 0 ? G.ug[0].src : -1,
             G.matRGB[0], G.matRGB[1], G.matRGB[2], G.matA, (int)G.lit, (unsigned)G.tev.reg[1], (unsigned)G.tev.reg[2], (unsigned)G.tev.reg[3],
             st.blendMode, st.blendSrc, st.blendDst, st.alphaCmp, st.alphaRef, st.depthCmp, st.writeMask, G.cull, G.faceMode, sb);
}
static void tagProbe(const PVtx& pa, const PVtx& pb, const PVtx& pc)
{
    const float ox = (float)G.vp[0], oy = (float)G.vp[1];
    const float xs[3] = {pa.px + ox, pb.px + ox, pc.px + ox}, ys[3] = {pa.py + oy, pb.py + oy, pc.py + oy};
    for (int k = 0; k < 3; ++k) {
        sTagAcc.sx += xs[k]; sTagAcc.sy += ys[k];
        if (xs[k] < sTagAcc.x0) sTagAcc.x0 = xs[k]; if (xs[k] > sTagAcc.x1) sTagAcc.x1 = xs[k];
        if (ys[k] < sTagAcc.y0) sTagAcc.y0 = ys[k]; if (ys[k] > sTagAcc.y1) sTagAcc.y1 = ys[k];
    }
    ++sTagAcc.n; sTagAcc.tag = sTag;
    /* every distinct draw state of the watched enemy once per area: what it draws (computed colour at the first vertex) */
    if (sTagGen != gN3dsMatProbeGen) { sTagGen = gN3dsMatProbeGen; sTagStates = 0; sTagPicks = 0; }
    if (sTagStates >= 48) return;
    const N3dsDrawState st = currentState();
    uint32_t h = 2166136261u; const uint8_t* bp = (const uint8_t*)&st; for (unsigned k = 0; k < sizeof st; ++k) if (k < offsetof(N3dsDrawState, vpX0) || k >= offsetof(N3dsDrawState, texId)) h = (h ^ bp[k]) * 16777619u;
    h = (h ^ (uint32_t)(G.matRGB[0] | G.matRGB[1] << 8 | G.matRGB[2] << 16 | (uint32_t)sTag << 24)) * 16777619u;
    for (int i = 0; i < sTagStates; ++i) if (sTagSeen[i] == h) return;
    sTagSeen[sTagStates++] = h;
    const N3dsVtx& a = pa.v;
    float tex[4] = {1, 1, 1, 1}; uint32_t tx = 0xFFFFFFFFu;
    const N3dsTexRef* t0 = G.nUnits > 0 ? &G.texRef[G.tev.unitMap[0] & 7] : nullptr;
    if (t0) { const float su = G.ug[0].su > 0 ? G.ug[0].su : 1.0f, sv = G.ug[0].sv > 0 ? G.ug[0].sv : 1.0f; tx = n3dstexSampleGc(*t0, a.u / su, a.v / sv);
              tex[0] = (tx & 255) / 255.0f; tex[1] = ((tx >> 8) & 255) / 255.0f; tex[2] = ((tx >> 16) & 255) / 255.0f; tex[3] = (tx >> 24) / 255.0f; }
    const float prim[4] = {a.r / 255.0f, a.g / 255.0f, a.b / 255.0f, a.a / 255.0f};
    float px[4]; picaCpu(st.tev, st.nTev, prim, tex, px, st.tevBufRgb, st.tevBufA);
    char sb[200]; int o = 0; sb[0] = 0;
    for (int i = 0; i < G.tev.nStages && i < 4; ++i) { const N3dsTevRaw& r = G.tev.st[i];
        o += snprintf(sb + o, sizeof sb - o, " s%d[ch%d C%d,%d,%d,%d A%d,%d,%d,%d o%d/%d]", i, r.chan == 255 ? -1 : r.chan, r.ci[0], r.ci[1], r.ci[2], r.ci[3], r.ai[0], r.ai[1], r.ai[2], r.ai[3], r.cout, r.aout); if (o >= (int)sizeof sb) break; }
    n3ds_log("[tagdraw] %s: colour %.0f,%.0f,%.0f a%.0f | vertex %u,%u,%u,%u z/w %.4f | tex %u %ux%u f%u src %d texel %08x | mat %u,%u,%u,%u lit %d regs %08x %08x %08x | blend %u %u/%u alpha %u>%u z %u mask %u cull %d/%d | pica %d stages |%s",
             tagName(sTag), px[0] * 255, px[1] * 255, px[2] * 255, px[3] * 255, a.r, a.g, a.b, a.a, a.w != 0 ? a.z / a.w : 0.0f,
             (unsigned)st.texId, t0 ? t0->w : 0, t0 ? t0->h : 0, t0 ? t0->fmt : 0, G.nUnits > 0 ? G.ug[0].src : -1, (unsigned)tx,
             G.matRGB[0], G.matRGB[1], G.matRGB[2], G.matA, (int)G.lit, (unsigned)G.tev.reg[1], (unsigned)G.tev.reg[2], (unsigned)G.tev.reg[3],
             st.blendMode, st.blendSrc, st.blendDst, st.alphaCmp, st.alphaRef, st.depthCmp, st.writeMask, G.cull, G.faceMode, st.nTev, sb);
}
static void emitKept(const PVtx& pa, const PVtx& pb, const PVtx& pc)
{
    /* The triangle survived every reject test: only now decode the attributes of vertices that were assembled position-only. The three
       arguments are always the assembler's own copies (G.q / G.s0 / G.s1) or the vertex-cache entry, never const data. */
    if (pa.lazy) resolveLazy(const_cast<PVtx&>(pa));
    if (pb.lazy) resolveLazy(const_cast<PVtx&>(pb));
    if (pc.lazy) resolveLazy(const_cast<PVtx&>(pc));
    if (sPickOn) pickProbe(pa, pb, pc);
    if (sTag && tagWatched(sTag) && !sOnBottom && pa.ok && pb.ok && pc.ok) tagProbe(pa, pb, pc);
    const N3dsVtx &a = pa.v, &b = pb.v, &c = pc.v;
    if (sChkOn) {                                     /* mesh_debug = 13 / mesh_cpu_check: count and fingerprint the surviving triangles instead of emitting them */
        /* A triangle with two vertices at the same clip position draws nothing; the cached mesh drops those at build time (same raw vertex used
           twice), so they are left out of the fingerprint on both sides. */
        if ((a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w) || (b.x == c.x && b.y == c.y && b.z == c.z && b.w == c.w) || (a.x == c.x && a.y == c.y && a.z == c.z && a.w == c.w)) return;
        ++sChkT;
        if (sCapOn && sCapN + 3 <= CAP_MAX) { const N3dsVtx* q3[3] = {&a, &b, &c}; for (int i = 0; i < 3; ++i) { float* o = sCap[sCapN++]; o[0] = q3[i]->x; o[1] = q3[i]->y; o[2] = q3[i]->z; o[3] = q3[i]->w; } }
        const float f[12] = {a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, c.x, c.y, c.z, c.w};
        uint32_t w; for (int i = 0; i < 12; ++i) { memcpy(&w, &f[i], 4); sChkH = (sChkH ^ w) * 16777619u; }
        const uint8_t col[12] = {a.r, a.g, a.b, a.a, b.r, b.g, b.b, b.a, c.r, c.g, c.b, c.a};
        for (int i = 0; i < 12; ++i) sChkH = (sChkH ^ col[i]) * 16777619u;
        const float uv[12] = {a.u, a.v, a.u1, a.v1, b.u, b.v, b.u1, b.v1, c.u, c.v, c.u1, c.v1};
        for (int i = 0; i < 12; ++i) { memcpy(&w, &uv[i], 4); sChkH = (sChkH ^ w) * 16777619u; }
        return;
    }
    if (pa.nc | pb.nc | pc.nc) { clipAndEmit(a, b, c); return; }   /* r22: flag computed once per vertex by finishGeom */
    if (!putTri(a, b, c)) return;
#if N3DS_GFX_DEBUG
    if (dbgTris < 4) { ++dbgTris; n3ds_log("[gfx] tri v0=(%.2f %.2f %.2f w%.2f) v1=(%.2f %.2f) v2=(%.2f %.2f) rgba=%u,%u,%u,%u",
        a.x, a.y, a.z, a.w, b.x, b.y, c.x, c.y, a.r, a.g, a.b, a.a); }
#endif
}
void emitTri(const PVtx& pa, const PVtx& pb, const PVtx& pc)
{
    if (!G.active) return;
    if (triReject(pa, pb, pc)) return;
    emitKept(pa, pb, pc);
}

/* Scissor rect -> NDC bounds of the current viewport, for the trivial-reject test. What can be visible on the PICA is
   what lies inside the scissor rect (the viewport is only a mapping), so the test uses the scissor, padded by 2 px
   for the rounding differences between gxRectToScreen and the buffer-space rect in n3ds_gpu.cpp. */
void computeCullBounds()
{
    const float vw = (float)(G.vp[2] - G.vp[0]), vh = (float)(G.vp[3] - G.vp[1]);
    G.hvw = 0.5f * vw; G.hvh = 0.5f * vh;
    G.cullOn = vw >= 2.0f && vh >= 2.0f;
    if (!G.cullOn) return;
    const float cx = 0.5f * (G.vp[0] + G.vp[2]), cy = 0.5f * (G.vp[1] + G.vp[3]), pad = 2.0f;
    G.cb[0] = ((float)G.sc[0] - pad - cx) / (vw * 0.5f);
    G.cb[1] = ((float)G.sc[2] + pad - cx) / (vw * 0.5f);
    G.cb[2] = (cy - ((float)G.sc[3] + pad)) / (vh * 0.5f);
    G.cb[3] = (cy - ((float)G.sc[1] - pad)) / (vh * 0.5f);
}

/* Homogeneous outcode of a clip-space point (pre-tilt, upright x/y). Every bit is a linear half-space test, so
   "all three vertices of a triangle share a bit" => the whole triangle is outside that plane => nothing of it can be
   visible (holds for any sign of w: visible points have w > 0 and satisfy every plane inequality). z range is the GX/PICA
   one, -w <= z <= 0. Strict comparisons: a vertex exactly on a plane is never rejected. */
/* r22: no float compares (ARM11 VFP: every compare is a pipeline-draining FMSTAT plus a branch; log f2ad7288 [lite4] measured ~600 cycles per
   vertex transform and ~400 per reject test). a < b is evaluated as "a - b is negative and not zero" on the integer bits - exactly the same
   result for finite values (a - b is +0 only when a == b; a negative result never rounds to +0). */
static inline uint32_t negBit(float f) { uint32_t u; memcpy(&u, &f, 4); return (u >> 31) & (uint32_t)((u << 1) != 0); }
inline uint8_t outcode(float cx, float cy, float z, float w)
{
    if (!(fin(cx) && fin(cy) && fin(z) && fin(w))) return OC_BAD;
    uint32_t oc = negBit(z + w) * OC_NEAR | negBit(-z) * OC_FAR;
    if (G.cullOn) {
        oc |= negBit(cx - G.cb[0] * w) * OC_L | negBit(G.cb[1] * w - cx) * OC_R | negBit(cy - G.cb[2] * w) * OC_B | negBit(G.cb[3] * w - cy) * OC_T;
    }
    return (uint8_t)oc;
}
/* r22: position tail shared by every transform (makeGeom, xfOne, xfFour, xfFourG): outcode, ok, pixel position, its fixed-point copy for the
   integer sub-pixel test, and the needs-clipping flag (once per vertex instead of three times per triangle, without float compares). */
static inline void finishGeom(PVtx& pv, float cx, float cy)
{
    const N3dsVtx& o = pv.v;
    pv.oc = outcode(cx, cy, o.z, o.w);
    pv.ok = negBit(1e-6f - o.w) && (pv.oc & OC_BAD) == 0;   /* w > 1e-6 */
    pv.sp = 0;
    if (pv.ok) {
        const float inv = 1.0f / o.w; pv.px = (cx * inv + 1.0f) * G.hvw; pv.py = (1.0f - cy * inv) * G.hvh;
        const float ax = fabsf(pv.px), ay = fabsf(pv.py);
        if (negBit(ax - 16000.0f) & negBit(ay - 16000.0f)) { pv.fx = (int32_t)(pv.px * 256.0f); pv.fy = (int32_t)(pv.py * 256.0f); pv.sp = 1; }
    } else { pv.px = pv.py = 0; }
    const float g = guardV(), gw = g * o.w;
    pv.nc = (uint8_t)(negBit(o.z + o.w + 1e-6f * fabsf(o.w)) | negBit(gw - o.x) | negBit(gw + o.x) | negBit(gw - o.y) | negBit(gw + o.y));
}

/* Turn the current texture refs + TEV program into what the draw state needs: the texture id/wraps/UV scale and PICA combiner
   stages. Only redone when something material-related changed (or on a new frame, so textures in use keep their LRU stamp). */
void resolveMaterialBody();
void resolveMaterial()
{
    if (!n3dsConfig().gfxLite) { resolveMaterialBody(); return; }
    const unsigned long long t0 = n3ds_ticks();
    resolveMaterialBody();
    sTmMat += n3ds_ticks() - t0; ++sTmMatN;
}
void resolveMaterialBody()
{
    G.matDirty = false;
    /* Which GX texture maps does the program really read, and through which texture coordinate? Each distinct map gets a PICA texture unit
       (up to 3, in order of first use). Stages that only feed a TEV register are dropped by the builder (lenient mode), so they do not count. */
    int mapOf[3] = {-1, -1, -1}, coordOf[3] = {0, 0, 0}, nu = 0;
    uint8_t whyNone[8] = {0, 0, 0, 0, 0, 0, 0, 0};   /* diagnostics: 1 no valid texture object, 2 more than 3 maps, 3 texture creation failed */
    G.curTexId = 0; G.uvS = G.uvT = 1.0f; G.curWrapS = G.curWrapT = 0; G.texFailed = false;
    for (int i = 0; i < G.tev.nStages && i < 16; ++i) {
        const N3dsTevRaw& st = G.tev.st[i];
        if (st.cout != 0 && st.aout != 0) continue;
        const int m = st.map;
        if (m >= 8) continue;
        bool reads = false;
        for (int k = 0; k < 4; ++k) if (st.ci[k] == 8 || st.ci[k] == 9 || st.ai[k] == 4) reads = true;
        if (!reads) continue;
        if (!G.texRef[m].valid) { whyNone[m] = 1; continue; }
        bool have = false; for (int u = 0; u < nu; ++u) if (mapOf[u] == m) have = true;
        if (!have && nu >= 3) whyNone[m] = 2;
        if (have || nu >= 3) continue;
        mapOf[nu] = m; coordOf[nu] = st.map < 8 ? G.stageCoord[i] : 0; ++nu;
    }
    G.nUnits = 0; memset(G.ug, 0, sizeof G.ug);
    G.tev.nUnits = 0; G.tev.unitMap[0] = G.tev.unitMap[1] = G.tev.unitMap[2] = -1;
    for (int u = 0; u < nu; ++u) {
        const int m = mapOf[u];
        const N3dsTexBound b = n3dstexResolve(G.texRef[m], G.frameNo);
        if (!b.ok) { G.texFailed = true; G.failedData = G.texRef[m].data; whyNone[m] = 3; continue; }
        const int k = G.nUnits++;
        const int coord = coordOf[u] < 8 ? coordOf[u] : 0;
        Gfx::UnitGen& g = G.ug[k];
        g.src = G.tgSrc[coord]; g.mtx = G.tgMtx[coord]; g.coord = (uint8_t)coord;
        g.plain = g.src >= 4 && g.mtx == 60;               /* vertex texcoord through the identity matrix (HUD, menus, most plain models) */
        g.su = b.su; g.sv = b.sv; g.tex = b.id; g.wrapS = G.texRef[m].wrapS; g.wrapT = G.texRef[m].wrapT;
        G.tev.unitMap[k] = m;
    }
    G.tev.nUnits = G.nUnits;
    {   /* telemetry (always on, first 32 distinct): a combiner stage samples a texture that will not be bound. The PICA program then reads that
           texture as opaque WHITE, which is what makes effects/particles show up as solid white quads. Logged with the reason. */
        static uint32_t seenT[32]; static int nSeenT = 0;
        for (int m = 0; m < 8; ++m) if (whyNone[m]) {
            const N3dsTexRef& r = G.texRef[m];
            const uint32_t h = ((uint32_t)(uintptr_t)r.data * 2654435761u) ^ (uint32_t)whyNone[m] ^ ((uint32_t)r.fmt << 24);
            bool dup = false; for (int i = 0; i < nSeenT; ++i) if (seenT[i] == h) dup = true;
            if (dup || nSeenT >= 32) continue;
            seenT[nSeenT++] = h;
            n3ds_log("[tex-missing] frame %u: a TEV stage samples map %d but no texture is bound (%s) | data %p %ux%u gc-fmt %u palette %p | %d stages, blend %d/%d/%d, alpha test %d>%d",
                     G.frameNo, m, whyNone[m] == 1 ? "GXLoadTexObj gave no usable texture object (not created by GXInitTexObj, or a CI texture whose palette was not loaded yet)"
                                 : whyNone[m] == 2 ? "more than 3 textures in one material" : "texture creation failed (see the [tex] FAILED line)",
                     r.data, (unsigned)r.w, (unsigned)r.h, (unsigned)r.fmt, r.pal, G.tev.nStages, G.blendMode, G.blendSrc, G.blendDst, G.alphaCmp, G.alphaRef);
        }
    }
    if (G.nUnits > 0) { G.curTexId = G.ug[0].tex; G.uvS = G.ug[0].su; G.uvT = G.ug[0].sv; G.curWrapS = G.ug[0].wrapS; G.curWrapT = G.ug[0].wrapT; }
    int pm = G.tev.unitMap[0];
    G.tev.primaryMap = pm;
    G.tev.lenient = true;
    const bool tevOk = n3dsTevBuild(G.tev, G.tevOut);
    if (!tevOk) { n3dsTevFallback(pm >= 0, G.tevOut); G.nUnits = pm >= 0 ? 1 : 0; }   /* fallback samples unit 0 only */
    {   /* telemetry: log each distinct TEV program that could not be mapped exactly (first 24), with the raw GX stage fields */
        static uint32_t seen[24]; static int nSeen = 0;
        if (!tevOk || G.tevOut.approximations > 0) {
            uint32_t h = 2166136261u;
            for (int i = 0; i < G.tev.nStages && i < 16; ++i) { const uint8_t* b = (const uint8_t*)&G.tev.st[i]; for (unsigned k = 0; k < sizeof(N3dsTevRaw); ++k) h = (h ^ b[k]) * 16777619u; }
            bool dup = false; for (int i = 0; i < nSeen; ++i) if (seen[i] == h) dup = true;
            if (!dup && nSeen < 24) {
                seen[nSeen++] = h;
                n3ds_log("[tev] program not exact (build %s, approx %d, %d stages, primaryMap %d, regs %08x %08x %08x %08x):", tevOk ? "ok" : "REFUSED", G.tevOut.approximations,
                         G.tev.nStages, G.tev.primaryMap, (unsigned)G.tev.reg[0], (unsigned)G.tev.reg[1], (unsigned)G.tev.reg[2], (unsigned)G.tev.reg[3]);
                for (int i = 0; i < G.tev.nStages && i < 6; ++i) { const N3dsTevRaw& r = G.tev.st[i];
                    n3ds_log("[tev]   s%d C(%d,%d,%d,%d) A(%d,%d,%d,%d) cop%d/b%d/s%d/o%d aop%d/b%d/s%d/o%d map%d", i, r.ci[0], r.ci[1], r.ci[2], r.ci[3], r.ai[0], r.ai[1], r.ai[2], r.ai[3],
                             r.cop, r.cbias, r.cscale, r.cout, r.aop, r.abias, r.ascale, r.aout, r.map); }
            }
        }
    }
    {   /* r36 probe: EVERY distinct material once (program + registers + textures + blend), first 160, so a wrong-looking object (black Wollywog,
           black sky) can be found by its colours / texture addresses: [mat] lines. */
        static uint32_t seenM[160]; static int nSeenM = 0; static int resetSeen = 0;
        if (resetSeen != gN3dsMatProbeGen) { resetSeen = gN3dsMatProbeGen; nSeenM = 0; n3ds_log("[mat] --- new area: material list restarts ---"); }
        if (nSeenM < 160) {
            uint32_t h = 2166136261u;
            for (int i = 0; i < G.tev.nStages && i < 16; ++i) { const uint8_t* b = (const uint8_t*)&G.tev.st[i]; for (unsigned k = 0; k < sizeof(N3dsTevRaw); ++k) h = (h ^ b[k]) * 16777619u; }
            for (int i = 0; i < 4; ++i) h = (h ^ G.tev.reg[i] ^ (G.tev.kcolor[i] << 1)) * 16777619u;
            /* @vfp-exempt R2 — screen false positive: integer casts of a pointer and of integer material fields into a hash, no float involved. measured n/a. Verified 2026-11-01. */
            for (int u = 0; u < G.nUnits && u < 3; ++u) h = (h ^ (uint32_t)(uintptr_t)G.texRef[G.tev.unitMap[u] & 7].data ^ ((uint32_t)G.ug[u].src << 24)) * 16777619u;
            h = (h ^ (uint32_t)(G.blendMode | G.blendSrc << 4 | G.blendDst << 8 | G.alphaCmp << 12 | (int)G.lit << 16 | G.matRGB[0] << 20)) * 16777619u;
            bool dup = false; for (int i = 0; i < nSeenM; ++i) if (seenM[i] == h) dup = true;
            if (!dup) {
                seenM[nSeenM++] = h;
                char st[300]; int so = 0; st[0] = 0;
                for (int i = 0; i < G.tev.nStages && i < 6; ++i) { const N3dsTevRaw& r = G.tev.st[i];
                    so += snprintf(st + so, sizeof st - so, " s%d[ch%d C%d,%d,%d,%d A%d,%d,%d,%d m%d k%d/%d]", i, r.chan == 255 ? -1 : r.chan, r.ci[0], r.ci[1], r.ci[2], r.ci[3], r.ai[0], r.ai[1], r.ai[2], r.ai[3], r.map == 255 ? -1 : r.map, r.ksel, r.kasel); }
                char ub[260]; int uo = 0; ub[0] = 0;
                for (int u = 0; u < G.nUnits && u < 3; ++u) { const N3dsTexRef& t = G.texRef[G.tev.unitMap[u] & 7];
                    uo += snprintf(ub + uo, sizeof ub - uo, " u%d[%ux%u f%u %p src%d mtx%d]", u, t.w, t.h, t.fmt, t.data, G.ug[u].src, G.ug[u].mtx); }
                n3ds_log("[mat] #%d%s | regs %08x %08x %08x k %08x %08x | mat %d,%d,%d,%d vtx %d%d lit %d | out %d st ap %d | blend %d/%d/%d a%d>%d z%d/%d%s", nSeenM, st,
                         (unsigned)G.tev.reg[1], (unsigned)G.tev.reg[2], (unsigned)G.tev.reg[3], (unsigned)G.tev.kcolor[0], (unsigned)G.tev.kcolor[1],
                         G.matRGB[0], G.matRGB[1], G.matRGB[2], G.matA, (int)G.matVtxRGB, (int)G.matVtxA, (int)G.lit, G.tevOut.n, G.tevOut.approximations,
                         G.blendMode, G.blendSrc, G.blendDst, G.alphaCmp, G.alphaRef, G.zCmp, (int)G.zWrite, ub);
            }
        }
    }
    {   /* r37 probe (black Wollywog / Puffy Blowhog: every material of theirs multiplies by a sphere-mapped I8 texture): each distinct texture a
           unit samples through a NORMAL / POSITION generated coordinate, once per area, with its data statistics and the GPU copy's average. */
        static const void* seenE[24]; static int nSeenE = 0, genE = -1;
        if (genE != gN3dsMatProbeGen) { genE = gN3dsMatProbeGen; nSeenE = 0; sEnvWatchN = 0; memset(sEnvWatchFlag, 0, sizeof sEnvWatchFlag); }
        for (int u = 0; u < G.nUnits && u < 3; ++u) {
            if (G.ug[u].src >= 4) continue;
            const N3dsTexRef& t = G.texRef[G.tev.unitMap[u] & 7];
            bool dup = false; for (int i = 0; i < nSeenE; ++i) if (seenE[i] == t.data) dup = true;
            if (dup || nSeenE >= 24) continue;
            seenE[nSeenE++] = t.data;
            envWatchAdd(G.ug[u].tex);
            char d[400]; n3dstexDescribe(t, d, sizeof d);
            /* @vfp-exempt R2 — screen false positive: (unsigned) of integer texture ids in a probe log line (runs for the first few materials of an area). measured n/a. Verified 2026-11-01. */
            n3ds_log("[envtex] gpu tex %u (unit %d, coord from %s, matrix %d) | %s", (unsigned)G.ug[u].tex, u, G.ug[u].src == 1 ? "normal" : G.ug[u].src == 0 ? "position" : "other", G.ug[u].mtx, d);
        }
    }
    {   /* r32 probe: every distinct program that reads channel 1 (COLOR1 / COLOR1A1) rasterised colour, with each stage's channel */
        static uint32_t seen1[32]; static int nSeen1 = 0;
        bool uses1 = false;
        for (int i = 0; i < G.tev.nStages && i < 16; ++i) { const N3dsTevRaw& r = G.tev.st[i];
            if ((r.chan == 1 || r.chan == 3 || r.chan == 5) && (r.ci[0] == 10 || r.ci[1] == 10 || r.ci[2] == 10 || r.ci[3] == 10 || r.ci[0] == 11 || r.ci[1] == 11 || r.ci[2] == 11 || r.ci[3] == 11)) uses1 = true; }
        if (uses1 && nSeen1 < 32) {
            uint32_t h = 2166136261u;
            for (int i = 0; i < G.tev.nStages && i < 16; ++i) { const uint8_t* b = (const uint8_t*)&G.tev.st[i]; for (unsigned k = 0; k < sizeof(N3dsTevRaw); ++k) h = (h ^ b[k]) * 16777619u; }
            bool dup = false; for (int i = 0; i < nSeen1; ++i) if (seen1[i] == h) dup = true;
            if (!dup) {
                seen1[nSeen1++] = h;
                n3ds_log("[chan1] program reads channel 1: %d stages, mat %d,%d,%d,%d lit %d | regs %08x %08x %08x %08x", G.tev.nStages, G.matRGB[0], G.matRGB[1], G.matRGB[2], G.matA, (int)G.lit,
                         (unsigned)G.tev.reg[0], (unsigned)G.tev.reg[1], (unsigned)G.tev.reg[2], (unsigned)G.tev.reg[3]);
                for (int i = 0; i < G.tev.nStages && i < 6; ++i) { const N3dsTevRaw& r = G.tev.st[i];
                    n3ds_log("[chan1]   s%d chan %d C(%d,%d,%d,%d) A(%d,%d,%d,%d) map%d", i, r.chan, r.ci[0], r.ci[1], r.ci[2], r.ci[3], r.ai[0], r.ai[1], r.ai[2], r.ai[3], r.map); }
                /* r33: everything else that decides the colour (Wollywog still black in r32 although its program evaluates fine on the host) */
                char ub[240]; int uo = 0; ub[0] = 0;
                /* @vfp-exempt R2 — screen false positive: integer fields printed by a probe log line. measured n/a. Verified 2026-11-01. */
                for (int u = 0; u < G.nUnits && u < 3; ++u) uo += snprintf(ub + uo, sizeof ub - uo, " u%d:gpu tex %u map %d coord %d src %d mtx %d%s", u, (unsigned)G.ug[u].tex, G.tev.unitMap[u], G.ug[u].coord, G.ug[u].src, G.ug[u].mtx, G.ug[u].plain ? " plain" : "");
                const int pm = G.tev.primaryMap;
                const N3dsTexRef* tr = (pm >= 0 && pm < 8) ? &G.texRef[pm] : nullptr;
                n3ds_log("[chan1]   units %d%s | tex0 %s %ux%u gc-fmt %u data %p pal %p (%u) | tev out %d stages approx %d | vtxRGB %d vtxA %d tint %d texFailed %d | blend %d/%d/%d alpha %d>%d z %d/%d cull %d",
                         G.nUnits, ub, tr && tr->valid ? "ok" : "none", tr ? tr->w : 0, tr ? tr->h : 0, tr ? tr->fmt : 0, tr ? tr->data : nullptr, tr ? tr->pal : nullptr, tr ? tr->palN : 0,
                         G.tevOut.n, G.tevOut.approximations, (int)G.matVtxRGB, (int)G.matVtxA, (int)G.tintOn, (int)G.texFailed, G.blendMode, G.blendSrc, G.blendDst, G.alphaCmp, G.alphaRef, G.zCmp, (int)G.zWrite, G.cull);
            }
        }
    }
    {   /* r14 probe (Pikmin look pink): dump the full material state, once per distinct program, when the material or a TEV register holds a Pikmin colour
           (red 255,30,0 / blue 0,50,255 / yellow 255,210,0). Pikmin are a grey texture painted by that colour. */
        auto isPiki = [](unsigned r, unsigned g, unsigned b) { return (r == 255 && g == 30 && b == 0) || (r == 0 && g == 50 && b == 255) || (r == 255 && g == 210 && b == 0); };
        bool hit = isPiki(G.matRGB[0], G.matRGB[1], G.matRGB[2]);
        for (int i = 0; i < 4 && !hit; ++i) { const uint32_t c = G.tev.reg[i]; hit = isPiki(c & 255, (c >> 8) & 255, (c >> 16) & 255); }
        static uint32_t seenP[8]; static int nSeenP = 0;
        if (hit && nSeenP < 8) {
            uint32_t h = 2166136261u ^ (uint32_t)G.lit ^ (uint32_t)G.matVtxRGB << 1;
            for (int i = 0; i < G.tev.nStages && i < 16; ++i) { const uint8_t* b = (const uint8_t*)&G.tev.st[i]; for (unsigned k = 0; k < sizeof(N3dsTevRaw); ++k) h = (h ^ b[k]) * 16777619u; }
            bool dup = false; for (int i = 0; i < nSeenP; ++i) if (seenP[i] == h) dup = true;
            if (!dup) {
                seenP[nSeenP++] = h;
                n3ds_log("[piki-mat] build %s approx %d | %d stages | mat %u,%u,%u,%u (from vertex %d) lit %d | regs %08x %08x %08x %08x | k %08x %08x %08x %08x | units %d | blend %d %d %d",
                         tevOk ? "ok" : "REFUSED", G.tevOut.approximations, G.tev.nStages, G.matRGB[0], G.matRGB[1], G.matRGB[2], G.matA, (int)G.matVtxRGB, (int)G.lit,
                         (unsigned)G.tev.reg[0], (unsigned)G.tev.reg[1], (unsigned)G.tev.reg[2], (unsigned)G.tev.reg[3],
                         (unsigned)G.tev.kcolor[0], (unsigned)G.tev.kcolor[1], (unsigned)G.tev.kcolor[2], (unsigned)G.tev.kcolor[3], G.nUnits, G.blendMode, G.blendSrc, G.blendDst);
                for (int i = 0; i < G.tev.nStages && i < 8; ++i) { const N3dsTevRaw& r = G.tev.st[i];
                    n3ds_log("[piki-mat]   s%d C(%d,%d,%d,%d) A(%d,%d,%d,%d) cop%d/b%d/s%d/o%d aop%d/b%d/s%d/o%d map%d ksel %d", i, r.ci[0], r.ci[1], r.ci[2], r.ci[3], r.ai[0], r.ai[1], r.ai[2], r.ai[3],
                             r.cop, r.cbias, r.cscale, r.cout, r.aop, r.abias, r.ascale, r.aout, r.map, r.ksel); }
                /* @vfp-exempt R2 — screen false positive: integer texture sizes printed by the [piki-mat] probe. measured n/a. Verified 2026-11-01. */
                for (int m = 0; m < 3; ++m) { const N3dsTexRef& t = G.texRef[m]; if (t.valid) n3ds_log("[piki-mat]   tex map%d %ux%u gx-format %u", m, (unsigned)t.w, (unsigned)t.h, (unsigned)t.fmt); }
            }
        }
    }
    if (G.nUnits > 1 || (G.nUnits == 1 && !G.ug[0].plain)) {   /* telemetry: multi-texture / generated-coordinate materials (first 40 distinct) */
        static uint32_t seenU[40]; static int nSeenU = 0;
        uint32_t h = 2166136261u ^ (uint32_t)G.nUnits;
        /* @vfp-exempt R2 — screen false positive: integer unit fields folded into a material hash. measured n/a. Verified 2026-11-01. */
        for (int u = 0; u < G.nUnits; ++u) h = (h ^ (G.ug[u].src | G.ug[u].mtx << 8 | (uint32_t)G.ug[u].tex << 16)) * 16777619u;
        h = (h ^ (uint32_t)(G.tevOut.n | G.tevOut.approximations << 8)) * 16777619u;
        bool dup = false; for (int i = 0; i < nSeenU; ++i) if (seenU[i] == h) dup = true;
        if (!dup && nSeenU < 40) {
            seenU[nSeenU++] = h;
            char buf[200]; int o = 0;
            /* @vfp-exempt R2 — screen false positive: integer unit fields printed by a probe log line. measured n/a. Verified 2026-11-01. */
            for (int u = 0; u < G.nUnits; ++u) o += snprintf(buf + o, sizeof buf - o, " | u%d tex %u coord %d src %d mtx %d%s", u, (unsigned)G.ug[u].tex, G.ug[u].coord, G.ug[u].src, G.ug[u].mtx, G.ug[u].plain ? " plain" : "");
            n3ds_log("[unit] %d texture unit(s)%s | %d PICA stages, %d approximations", G.nUnits, buf, G.tevOut.n, G.tevOut.approximations);
        }
    }
    G.tintOn = G.texFailed;
    if (G.tintOn) {
        const uint32_t h = (uint32_t)(((uintptr_t)G.failedData) >> 3) * 2654435761u;
        G.tint[0] = (110 + (h & 127)) / 255.0f; G.tint[1] = (110 + ((h >> 8) & 127)) / 255.0f; G.tint[2] = (110 + ((h >> 16) & 127)) / 255.0f;
    }
}

void censusClose()
{
    if (!sCen.open) return;
    sCen.open = false;
    const unsigned verts = S.inVerts - sCen.in0, tris = S.tris - sCen.tris0;
    if (verts == 0) return;
    if (tris == 0) {   /* nothing emitted: only count it, grouped by texture/tev/blend (a missing object shows up as a group of all-culled / sub-pixel batches) */
        int g = -1;
        for (int k = 0; k < sCenNGrp; ++k) if (sCenGrp[k].tex == sCen.tex && sCenGrp[k].tev == sCen.tevN && sCenGrp[k].bm == sCen.bm) { g = k; break; }
        if (g < 0 && sCenNGrp < 48) { g = sCenNGrp++; sCenGrp[g] = CenGroup{sCen.tex, sCen.tevN, sCen.bm, 0, 0, 0, 0}; }
        if (g >= 0) { ++sCenGrp[g].draws; sCenGrp[g].verts += verts; sCenGrp[g].culled += S.culled - sCen.culled0; sCenGrp[g].subpx += S.subpx - sCen.sub0; }
        return;
    }
    if (sCenPrinted >= 250) return;
    ++sCenPrinted;
    n3ds_log("[draw] #%d p%02x tex %u%s tev %d%s bl %d/%d/%d z %d%c cu %d a %d>%d lit %d | v %u t %u cul %u sub %u clip %u | bb (%.2f,%.2f)-(%.2f,%.2f) uv (%.2f,%.2f)-(%.2f,%.2f) rgba %u,%u,%u,%u",
             sCen.idx, sCen.prim, sCen.tex, sCen.texFail ? "FAIL" : "", sCen.tevN, sCen.approx ? "~" : "", sCen.bm, sCen.bs, sCen.bd, sCen.zc, sCen.zw ? 'w' : '-',
             sCen.cull, sCen.ac, sCen.ar, (int)sCen.lit, verts, tris, S.culled - sCen.culled0, S.subpx - sCen.sub0, S.clipped - sCen.clip0,
             sCen.bx0, sCen.by0, sCen.bx1, sCen.by1, sCen.u0, sCen.v0, sCen.u1, sCen.v1, sCen.rgba[0], sCen.rgba[1], sCen.rgba[2], sCen.rgba[3]);
}
void censusFlushGroups()
{
    for (int k = 0; k < sCenNGrp; ++k)
        n3ds_log("[draw0] tex %u tev %d bl %d: %u batches emitted nothing (verts %u, culled tris %u, sub-pixel tris %u)", sCenGrp[k].tex, sCenGrp[k].tev, sCenGrp[k].bm,
                 sCenGrp[k].draws, sCenGrp[k].verts, sCenGrp[k].culled, sCenGrp[k].subpx);
    sCenNGrp = 0; sCenPrinted = 0;
}
void censusOpen(int prim)
{
    sCen.open = true; sCen.idx = sCenIdx++; sCen.prim = prim; sCen.tex = G.curTexId; sCen.texFail = G.texFailed; sCen.tevN = G.tevOut.n; sCen.approx = G.tevOut.approximations;
    sCen.bm = G.blendMode; sCen.bs = G.blendSrc; sCen.bd = G.blendDst; sCen.zc = G.zCmp; sCen.zw = G.zWrite; sCen.cull = G.cull; sCen.ac = G.alphaCmp; sCen.ar = G.alphaRef;
    sCen.lit = G.lit; for (int i = 0; i < 4; ++i) sCen.vp[i] = G.vp[i];
    sCen.in0 = S.inVerts; sCen.tris0 = S.tris; sCen.culled0 = S.culled; sCen.sub0 = S.subpx; sCen.clip0 = S.clipped;
    sCen.bx0 = sCen.by0 = sCen.u0 = sCen.v0 = 1e9f; sCen.bx1 = sCen.by1 = sCen.u1 = sCen.v1 = -1e9f; sCen.haveRgba = false;
}

void primBegin(int prim)
{
    if (sProfile) censusClose();
    G.active = false;
    if (!tiltOk) tiltOk = n3ds_gpu_tilt(tilt) != 0;
    if (!tiltOk) return;
    if (prim != P_QUADS && prim != P_TRIANGLES && prim != P_STRIP && prim != P_FAN) return;   /* lines/points: not yet */
    if (G.sc[2] <= G.sc[0] || G.sc[3] <= G.sc[1]) { ++S.emptyScissor; return; }              /* nothing can be visible */
    if (G.matDirty) resolveMaterial();
    if (gN3dsEnvDbg == 3 || gN3dsEnvDbg == 4) {   /* r41 test modes: 3 = hide every draw that samples a normal-mapped (sphere-mapped) texture, 4 = show ONLY those */
        bool env = false; for (int u = 0; u < G.nUnits; ++u) if (G.ug[u].src < 4) env = true;
        if (gN3dsEnvDbg == 3 ? env : !env) return;
    }
    if (gN3dsEnvDbg == 5 && G.nUnits == 0 && G.lit && G.matRGB[0] == 0 && G.matRGB[1] == 0 && G.matRGB[2] == 0) return;   /* r43: hide black untextured lit materials */
    if (gN3dsEnvDbg == 6 && G.nUnits == 0 && G.lit) return;                                                           /* r43: hide every untextured lit material */
    N3dsDrawState st = currentState();
    if (outP && outN > 0 && memcmp(&st, &outSt, sizeof st) != 0) flushOut();
    outSt = st;                                  /* reserveOut() reads outSt.depthCmp (UI reserve) */
    if (!outP && !reserveOut()) return;
    computeCullBounds();
    {   /* r40: GXSetCullMode was ignored, so both sides of every triangle were drawn. Models with a black "inside" shell meant to be seen only
           from behind (Wollywog, Puffy Blowhog) were covered by it. Culling follows GX: a triangle is front-facing when it is clockwise on
           screen. Dolphin's software rasteriser (Clipper IsBackface): d = det(x,y,w) of the clip-space vertices; GX_CULL_BACK (hardware value 1 after
           the SDK's front/back swap) drops d > 0, GX_CULL_FRONT drops d <= 0. tilt[] is the LCD rotation applied to x/y. */
        int cfg = n3dsConfig().cullFaces; const int dbg = gN3dsEnvDbg;
        if (dbg == 1) cfg = 0; else if (dbg == 2) cfg = cfg == 2 ? 1 : 2;   /* modes 3 / 4 keep nectar.ini culling */
        G.faceMode = cfg ? (G.cull & 3) : 0;
        const float td = tilt[0] * tilt[3] - tilt[1] * tilt[2];
        G.faceSign = (td < 0 ? -1.0f : 1.0f) * (cfg == 2 ? -1.0f : 1.0f);
        if (G.cull == 1) {   /* which materials ask for FRONT culling (an inverted shell is the usual reason) */
            static int nFront = 0; static int gen = -1; if (gen != gN3dsMatProbeGen) { gen = gN3dsMatProbeGen; nFront = 0; }
            if (nFront < 12) { ++nFront; n3ds_log("[cull] front-face culling requested: tex %u, %d tev stages, mat %u,%u,%u,%u lit %d, regs %08x %08x, blend %d %d/%d z %d/%d (applied: %s)",
                (unsigned)G.curTexId, G.tev.nStages, G.matRGB[0], G.matRGB[1], G.matRGB[2], G.matA, (int)G.lit, (unsigned)G.tev.reg[1], (unsigned)G.tev.reg[2],
                G.blendMode, G.blendSrc, G.blendDst, G.zCmp, (int)G.zWrite, G.faceMode ? "yes" : "no"); }
        }
    }
    ++S.prims;
    sDarkCheck = !G.imm || G.lit;   /* r41: 3D draws only */
    G.prim = prim; G.qn = 0; G.sc_n = 0; G.active = true;
    if (sProfile) censusOpen(prim);
#if N3DS_GFX_DEBUG
    if (dbgDraws < 6) { ++dbgDraws; n3ds_log("[gfx] draw#%d prim=%02x cur=%u zc=%d zw=%d blend=%d/%d/%d vp=%d,%d-%d,%d", dbgDraws, prim, G.cur,
        G.zCmp, G.zWrite, G.blendMode, G.blendSrc, G.blendDst, G.vp[0], G.vp[1], G.vp[2], G.vp[3]); }
#endif
}

/* ---- matrices ----------------------------------------------------------------------------------------------- */
const float* clipFor(unsigned id)
{
    id &= 63;
    if (!G.clipValid[id] || G.clipProjGen[id] != G.projGen || G.clipPosGen[id] != G.posGen[id]) {
        float M[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        if (G.posSet[id]) memcpy(M, G.pos[id], 12 * sizeof(float));
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
            G.clip[id][r * 4 + c] = G.proj[r * 4 + 0] * M[0 * 4 + c] + G.proj[r * 4 + 1] * M[1 * 4 + c]
                                  + G.proj[r * 4 + 2] * M[2 * 4 + c] + G.proj[r * 4 + 3] * M[3 * 4 + c];
        G.clipValid[id] = true; G.clipProjGen[id] = G.projGen; G.clipPosGen[id] = G.posGen[id];
    }
    return G.clip[id];
}

#if defined(__arm__)
/* r22: float -> int conversion (saturating on ARM) + USAT instead of two float compares: identical for every non-NaN input */
uint8_t clamp255(float f) { int32_t i = (int32_t)(f + 0.5f); __asm__("usat %0, #8, %1" : "=r"(i) : "r"(i)); return (uint8_t)i; }
#else
uint8_t clamp255(float f) { return f <= 0 ? 0 : f >= 255 ? 255 : (uint8_t)(f + 0.5f); }
#endif

/* geometry half of makeVtx: clip position, outcode, viewport px. Needs only v.x/y/z and v.mtx. */
void makeGeom(const Vin& v, PVtx& pv)
{
    ++S.inVerts;
    const float* C = clipFor(v.mtx);
    float cx = C[0] * v.x + C[1] * v.y + C[2] * v.z + C[3];
    float cy = C[4] * v.x + C[5] * v.y + C[6] * v.z + C[7];
    N3dsVtx& o = pv.v;
    o.x = tilt[0] * cx + tilt[1] * cy;
    o.y = tilt[2] * cx + tilt[3] * cy;
    o.z = C[8] * v.x + C[9] * v.y + C[10] * v.z + C[11];
    o.w = C[12] * v.x + C[13] * v.y + C[14] * v.z + C[15];
    finishGeom(pv, cx, cy);
    pv.lazy = 0; pv.mtx = v.mtx; pv.raw = nullptr; pv.ce = nullptr;
}

/* attribute half: texcoord, material/vertex colour, headlight, debug tint. Needs everything in v except the position. */
/* r47: GameCube lighting for one vertex, as Dolphin evaluates it (VideoCommon LightingShaderGen): colour = material x clamp(ambient +
   sum over the channel's lights of light colour x attenuation x diffuse term). Positions and normals in view space; the game loads its
   lights already in view space. Without a normal the diffuse term is 0 (ambient only), as on the GameCube. r,g,b,a: the material colour
   (register or vertex colour) in 0..255, multiplied in place. */
static int sLightMode = 1;   /* nectar.ini lighting: 1 GameCube, 0 simple (re-read each frame) */
/* The straightforward version (r47). Kept as the reference the fast one below is checked against (tools/host_test: lighting check). */
__attribute__((unused)) static void gxLightVertexRef(const Vin& v, float& r, float& g, float& b, float& a)
{
    const unsigned id = v.mtx & 63;
    const float* P = G.pos[id];
    const float px = P[0] * v.x + P[1] * v.y + P[2] * v.z + P[3], py = P[4] * v.x + P[5] * v.y + P[6] * v.z + P[7], pz = P[8] * v.x + P[9] * v.y + P[10] * v.z + P[11];
    float nx = 0, ny = 0, nz = 0;
    if (v.hasN) {
        const float* R = G.nrmSet[id] ? G.nrm[id] : P;
        nx = R[0] * v.nx + R[1] * v.ny + R[2] * v.nz; ny = R[4] * v.nx + R[5] * v.ny + R[6] * v.nz; nz = R[8] * v.nx + R[9] * v.ny + R[10] * v.nz;
        const float l2 = nx * nx + ny * ny + nz * nz;
        if (l2 > 1e-12f) { const float inv = 1.0f / sqrtf(l2); nx *= inv; ny *= inv; nz *= inv; } else nx = ny = nz = 0;
    }
    float acc[4];
    for (int k = 0; k < 2; ++k) {
        const Gfx::ChanCtl& c = G.ch[k];
        if (!c.en) continue;
        if (k == 0) { if (c.ambVtx && v.hasC) { acc[0] = v.r / 255.0f; acc[1] = v.g / 255.0f; acc[2] = v.b / 255.0f; } else { acc[0] = G.amb[0]; acc[1] = G.amb[1]; acc[2] = G.amb[2]; } }
        else acc[3] = (c.ambVtx && v.hasC) ? v.a / 255.0f : G.amb[3];
        for (int i = 0; i < 8; ++i) {
            if (!(c.mask & (1u << i))) continue;
            const Gfx::Light& L = G.light[i];
            float lx = L.pos[0] - px, ly = L.pos[1] - py, lz = L.pos[2] - pz;
            const float d2 = lx * lx + ly * ly + lz * lz;
            const float d = sqrtf(d2);
            if (d > 1e-12f) { const float inv = 1.0f / d; lx *= inv; ly *= inv; lz *= inv; }
            const float nl = lx * nx + ly * ny + lz * nz;
            float attn = 1.0f;
            if (c.attn == 1) {          /* GX_AF_SPOT: angle (cosine) and distance attenuation */
                const float cs = fmaxf(0.0f, lx * L.dir[0] + ly * L.dir[1] + lz * L.dir[2]);
                const float num = fmaxf(0.0f, L.a[0] + L.a[1] * cs + L.a[2] * cs * cs), den = L.k[0] + L.k[1] * d + L.k[2] * d2;
                attn = den > 1e-9f ? num / den : 0.0f;
            } else if (c.attn == 0) {   /* GX_AF_SPEC: dir holds the half-angle vector */
                const float hs = nl >= 0.0f ? fmaxf(0.0f, nx * L.dir[0] + ny * L.dir[1] + nz * L.dir[2]) : 0.0f;
                const float num = fmaxf(0.0f, L.a[0] + L.a[1] * hs + L.a[2] * hs * hs), den = L.k[0] + L.k[1] * hs + L.k[2] * hs * hs;
                attn = den > 1e-9f ? num / den : 0.0f;
            }
            const float diff = c.diff == 0 ? 1.0f : c.diff == 1 ? nl : fmaxf(0.0f, nl);
            const float f = attn * diff;
            if (k == 0) { acc[0] += f * L.col[0]; acc[1] += f * L.col[1]; acc[2] += f * L.col[2]; }
            else acc[3] += f * L.col[3];
        }
    }
    auto cl = [](float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; };
    if (G.ch[0].en) { r *= cl(acc[0]); g *= cl(acc[1]); b *= cl(acc[2]); }
    if (G.ch[1].en) a *= cl(acc[3]);
}

/* r50: the same lighting, about 2x cheaper per vertex (r47 made the per-vertex colour work go from 0.9 to 2.8 us).
   - Everything that only depends on the lights and the channel setup is worked out once, when it changes (litPrepare): which lights each
     channel uses, 1 / k0 when the distance attenuation is constant, the angle numerator when it is constant.
   - Per vertex, a light's direction / distance / N.L are computed once even when both channels use the light.
   - Position and normal are transformed only when some light needs them; channels without lights cost almost nothing.
   - No fmaxf (a library call on this VFP), and one square root + one division per normalisation.
   Results equal the reference up to float rounding (host test: lighting fast path). */
namespace {
struct LitPre { float kInv, numC; bool kConst, aConst; };   /* per light: constant parts of the attenuation */
LitPre sLitPre[8];
struct LitChan { uint8_t n, diff, attn; uint8_t idx[8]; };
LitChan sLitCh[2];
uint32_t sLitGen = 1, sLitBuilt = 0;                         /* sLitGen++ whenever a light or a channel control changes */
bool sLitNeedPos = false, sLitNeedN = false;
inline float max0(float x) { return x > 0.0f ? x : 0.0f; }
inline float cl01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

void litPrepare()
{
    sLitBuilt = sLitGen;
    for (int i = 0; i < 8; ++i) {
        const Gfx::Light& L = G.light[i]; LitPre& P = sLitPre[i];
        P.aConst = L.a[1] == 0.0f && L.a[2] == 0.0f;
        P.numC = max0(L.a[0]);
        P.kConst = L.k[1] == 0.0f && L.k[2] == 0.0f;
        P.kInv = L.k[0] > 1e-9f ? 1.0f / L.k[0] : 0.0f;
    }
    sLitNeedPos = sLitNeedN = false;
    for (int k = 0; k < 2; ++k) {
        const Gfx::ChanCtl& c = G.ch[k]; LitChan& C = sLitCh[k];
        C.n = 0; C.diff = c.diff; C.attn = c.attn;
        if (!c.en) continue;
        for (int i = 0; i < 8; ++i) if (c.mask & (1u << i)) C.idx[C.n++] = (uint8_t)i;
        if (!C.n) continue;
        const bool geo = c.diff != 0 || c.attn == 0 || c.attn == 1;   /* needs the direction to the light */
        if (geo) sLitNeedPos = true;
        if (c.diff != 0 || c.attn == 0) sLitNeedN = true;
    }
}
} // namespace

static void gxLightVertex(const Vin& v, float& r, float& g, float& b, float& a)
{
    if (sLitBuilt != sLitGen) litPrepare();
    float px = 0, py = 0, pz = 0, nx = 0, ny = 0, nz = 0;
    const unsigned id = v.mtx & 63;
    if (sLitNeedPos) {
        const float* P = G.pos[id];
        px = P[0] * v.x + P[1] * v.y + P[2] * v.z + P[3]; py = P[4] * v.x + P[5] * v.y + P[6] * v.z + P[7]; pz = P[8] * v.x + P[9] * v.y + P[10] * v.z + P[11];
        if (sLitNeedN && v.hasN) {
            const float* R = G.nrmSet[id] ? G.nrm[id] : P;
            nx = R[0] * v.nx + R[1] * v.ny + R[2] * v.nz; ny = R[4] * v.nx + R[5] * v.ny + R[6] * v.nz; nz = R[8] * v.nx + R[9] * v.ny + R[10] * v.nz;
            const float l2 = nx * nx + ny * ny + nz * nz;
            if (l2 > 1e-12f) { const float inv = 1.0f / sqrtf(l2); nx *= inv; ny *= inv; nz *= inv; } else nx = ny = nz = 0;
        }
    }
    float lg[8][5];          /* per light, this vertex: direction x y z (normalised), distance, N.L */
    unsigned done = 0;
    float acc[4];
    for (int k = 0; k < 2; ++k) {
        const Gfx::ChanCtl& c = G.ch[k];
        if (!c.en) continue;
        if (k == 0) { if (c.ambVtx && v.hasC) { acc[0] = v.r * (1.0f / 255.0f); acc[1] = v.g * (1.0f / 255.0f); acc[2] = v.b * (1.0f / 255.0f); } else { acc[0] = G.amb[0]; acc[1] = G.amb[1]; acc[2] = G.amb[2]; } }
        else acc[3] = (c.ambVtx && v.hasC) ? v.a * (1.0f / 255.0f) : G.amb[3];
        const LitChan& C = sLitCh[k];
        const bool geo = C.diff != 0 || C.attn <= 1;
        for (unsigned j = 0; j < C.n; ++j) {
            const unsigned i = C.idx[j];
            const Gfx::Light& L = G.light[i];
            float f;
            if (!geo) f = 1.0f;
            else {
                float* q = lg[i];
                if (!(done & (1u << i))) {
                    done |= 1u << i;
                    float lx = L.pos[0] - px, ly = L.pos[1] - py, lz = L.pos[2] - pz;
                    const float d2 = lx * lx + ly * ly + lz * lz;
                    float d;
                    if (d2 > 1e-24f) { const float inv = 1.0f / sqrtf(d2); lx *= inv; ly *= inv; lz *= inv; d = d2 * inv; }
                    else d = sqrtf(d2);
                    q[0] = lx; q[1] = ly; q[2] = lz; q[3] = d; q[4] = lx * nx + ly * ny + lz * nz;
                }
                const float nl = q[4];
                const LitPre& Pr = sLitPre[i];
                float attn = 1.0f;
                if (C.attn == 1) {          /* GX_AF_SPOT */
                    float num;
                    if (Pr.aConst) num = Pr.numC;
                    else { const float cs = max0(q[0] * L.dir[0] + q[1] * L.dir[1] + q[2] * L.dir[2]); num = max0(L.a[0] + L.a[1] * cs + L.a[2] * cs * cs); }
                    if (Pr.kConst) attn = num * Pr.kInv;
                    else { const float d = q[3], den = L.k[0] + L.k[1] * d + L.k[2] * d * d; attn = den > 1e-9f ? num / den : 0.0f; }
                } else if (C.attn == 0) {   /* GX_AF_SPEC: dir holds the half-angle vector */
                    const float hs = nl >= 0.0f ? max0(nx * L.dir[0] + ny * L.dir[1] + nz * L.dir[2]) : 0.0f;
                    const float num = Pr.aConst ? Pr.numC : max0(L.a[0] + L.a[1] * hs + L.a[2] * hs * hs);
                    if (Pr.kConst) attn = num * Pr.kInv;
                    else { const float den = L.k[0] + L.k[1] * hs + L.k[2] * hs * hs; attn = den > 1e-9f ? num / den : 0.0f; }
                }
                f = attn * (C.diff == 0 ? 1.0f : C.diff == 1 ? nl : max0(nl));
            }
            if (k == 0) { acc[0] += f * L.col[0]; acc[1] += f * L.col[1]; acc[2] += f * L.col[2]; }
            else acc[3] += f * L.col[3];
        }
    }
    if (G.ch[0].en) { r *= cl01(acc[0]); g *= cl01(acc[1]); b *= cl01(acc[2]); }
    if (G.ch[1].en) a *= cl01(acc[3]);
}

void makeAttr(const Vin& v, PVtx& pv)
{
    N3dsVtx& o = pv.v;
    if (G.nUnits == 1 && G.ug[0].plain && G.tm60Ident) {   /* the common case: one texture, plain vertex texcoord */
        o.u = (G.ug[0].src == 5 ? v.u1 : v.u) * G.uvS;
#if N3DS_UV_FLIPV
        o.v = 1.0f - (G.ug[0].src == 5 ? v.v1 : v.v) * G.uvT;   /* PICA samples v=0 at the LAST stored row; GX v=0 is the first row (HUD digits/text came out upside down) */
#else
        o.v = (G.ug[0].src == 5 ? v.v1 : v.v) * G.uvT;
#endif
    } else if (G.nUnits == 0) {   /* untextured: the coordinates are never sampled; kept identical to the textured path's raw value */
        o.u = v.u;
#if N3DS_UV_FLIPV
        o.v = 1.0f - v.v;
#else
        o.v = v.v;
#endif
    }
    else for (int k = 0; k < G.nUnits; ++k) {
        const Gfx::UnitGen& g = G.ug[k];
        float a, b, c = 1.0f;
        switch (g.src) {                                   /* GXTexGenSrc: 0 POS, 1 NRM, 4.. TEXn (only TEX0 / TEX1 are decoded) */
        case 0: a = v.x; b = v.y; c = v.z; break;
        case 1: a = v.nx; b = v.ny; c = v.nz; break;
        case 5: a = v.u1; b = v.v1; break;
        case 6: a = v.u2; b = v.v2; break;   /* r35: GX_TG_TEX2 */
        default: a = v.u; b = v.v; break;
        }
        const unsigned id = (g.coord < 8 && v.tmi[g.coord]) ? (unsigned)v.tmi[g.coord] : (unsigned)g.mtx;   /* a vertex that carries its own matrix id wins */
        const float* M = G.tm[id & 127];
        const float tu = (M[0] * a + M[1] * b + M[2] * c + M[3]) * g.su;
#if N3DS_UV_FLIPV
        const float tv = 1.0f - (M[4] * a + M[5] * b + M[6] * c + M[7]) * g.sv;
#else
        const float tv = (M[4] * a + M[5] * b + M[6] * c + M[7]) * g.sv;
#endif
        if (g.src == 1) {   /* r35 probe (Wollywog black): the first environment-mapped (normal-sourced) coordinates, with the matrix that made them */
            static uint16_t sEnvTex[24]; static uint8_t sEnvCnt[24]; static int sEnvN = 0, sEnvGen = -1;   /* 4 samples for each of the first 24 textures (per area since r37) */
            if (sEnvGen != gN3dsMatProbeGen) { sEnvGen = gN3dsMatProbeGen; sEnvN = 0; }
            int ei = 0; while (ei < sEnvN && sEnvTex[ei] != g.tex) ++ei;
            if (ei == sEnvN && sEnvN < 24) { sEnvTex[sEnvN] = g.tex; sEnvCnt[sEnvN] = 0; ++sEnvN; }
            if (ei < sEnvN && sEnvCnt[ei] < 4) { ++sEnvCnt[ei];
                const uint32_t tx = n3dstexSampleGc(G.texRef[G.tev.unitMap[k] & 7], tu / (g.su > 0.0f ? g.su : 1.0f), tv / (g.sv > 0.0f ? g.sv : 1.0f));
                n3ds_log("[envmap] unit %d tex %u | matrix id %u (%s) | normal %.3f %.3f %.3f | M %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f | scale %.3f %.3f -> uv %.3f %.3f -> GC texel %u,%u,%u,%u",
                         k, (unsigned)g.tex, id, (g.coord < 8 && v.tmi[g.coord]) ? "per-vertex" : "material", a, b, c, M[0], M[1], M[2], M[3], M[4], M[5], M[6], M[7], g.su, g.sv, tu, tv,
                         (unsigned)(tx & 255), (unsigned)((tx >> 8) & 255), (unsigned)((tx >> 16) & 255), (unsigned)(tx >> 24)); }
        }
        if (k == 0) { o.u = tu; o.v = tv; } else if (k == 1) { o.u1 = tu; o.v1 = tv; } else { o.u2 = tu; o.v2 = tv; }
    }
    if (G.nUnits < 2) { o.u1 = o.v1 = 0.0f; }   /* unused sets stay deterministic (bit-identical output paths, no NaN garbage in the ring) */
    if (G.nUnits < 3) { o.u2 = o.v2 = 0.0f; }
    float r = G.matVtxRGB && v.hasC ? v.r : G.matRGB[0];
    float g = G.matVtxRGB && v.hasC ? v.g : G.matRGB[1];
    float b = G.matVtxRGB && v.hasC ? v.b : G.matRGB[2];
    float a = G.matVtxA   && v.hasC ? v.a : G.matA;
    if (sLightMode && (G.ch[0].en || G.ch[1].en)) gxLightVertex(v, r, g, b, a);
    else if (G.lit && v.hasN) {   /* simple lighting (nectar.ini lighting = 0): a fake headlight in view space */
        const unsigned id = v.mtx & 63;
        const float* R = G.nrmSet[id] ? G.nrm[id] : G.pos[id];
        float ex = R[0] * v.nx + R[1] * v.ny + R[2] * v.nz, ey = R[4] * v.nx + R[5] * v.ny + R[6] * v.nz,
              ez = R[8] * v.nx + R[9] * v.ny + R[10] * v.nz;
        float len = sqrtf(ex * ex + ey * ey + ez * ez);
        if (len > 1e-6f) {
            float d = fabsf((ex * 0.30f + ey * 0.50f + ez * 0.81f) / len);
            float k = 0.40f + 0.60f * d;
            r *= k; g *= k; b *= k;
        }
    }
    if (G.tintOn) { r *= G.tint[0]; g *= G.tint[1]; b *= G.tint[2]; }
    o.r = clamp255(r); o.g = clamp255(g); o.b = clamp255(b); o.a = clamp255(a);
}

PVtx makeVtx(const Vin& v)
{
    PVtx pv; makeGeom(v, pv); makeAttr(v, pv);
    return pv;
}

/* ---- primitive assembly (GX prim -> triangle list) ------------------------------------------------------------ */
void asmPush(const PVtx& v)
{
    switch (G.prim) {
    case P_TRIANGLES:
        G.q[G.qn++] = v;
        if (G.qn == 3) { emitTri(G.q[0], G.q[1], G.q[2]); G.qn = 0; }
        break;
    case P_QUADS:
        G.q[G.qn++] = v;
        if (G.qn == 4) { emitTri(G.q[0], G.q[1], G.q[2]); emitTri(G.q[0], G.q[2], G.q[3]); G.qn = 0; }
        break;
    case P_STRIP:
        if (G.sc_n < 2) { (G.sc_n == 0 ? G.s0 : G.s1) = v; }
        else { if (G.sc_n & 1) emitTri(G.s1, G.s0, v); else emitTri(G.s0, G.s1, v); G.s0 = G.s1; G.s1 = v; }
        ++G.sc_n;
        break;
    case P_FAN:
        if (G.sc_n < 2) { (G.sc_n == 0 ? G.s0 : G.s1) = v; }   /* s0 = hub, s1 = previous rim vertex */
        else { emitTri(G.s0, G.s1, v); G.s1 = v; }
        ++G.sc_n;
        break;
    }
}

/* ---- vertex-stream decoding (display lists) -------------------------------------------------------------------- */
inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline uint32_t be16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }

int compSize(int type) { return type <= 1 ? 1 : type <= 3 ? 2 : 4; }
int colorSize(int type) { static const int s[6] = {2, 3, 4, 2, 3, 4}; return type >= 0 && type < 6 ? s[type] : 4; }

int directSize(int attr, const AttrFmt& f)
{
    if (attr < A_POS) return 1;
    if (attr == A_POS) return (f.cnt == 0 ? 2 : 3) * compSize(f.type);
    if (attr == A_NRM) return (f.cnt == 1 ? 9 : 3) * compSize(f.type);
    if (attr == A_CLR0 || attr == A_CLR1) return colorSize(f.type);
    return (f.cnt == 0 ? 1 : 2) * compSize(f.type);
}

float sInvFrac[32];   /* 1 / (1 << frac): a multiply instead of a VFP divide per component (filled in pc_gfx_init) */

float readComp(const uint8_t*& p, int type, int frac, bool be)
{
    float v;
    switch (type) {
    case 0: v = (float)p[0]; p += 1; break;
    case 1: v = (float)(int8_t)p[0]; p += 1; break;
    case 2: { uint16_t t; if (be) t = (uint16_t)be16(p); else memcpy(&t, p, 2); v = (float)t; p += 2; break; }
    case 3: { uint16_t t; if (be) t = (uint16_t)be16(p); else memcpy(&t, p, 2); v = (float)(int16_t)t; p += 2; break; }
    default: { uint32_t t; if (be) t = be32(p); else memcpy(&t, p, 4); memcpy(&v, &t, 4); p += 4; return v; }
    }
    return frac ? v * sInvFrac[frac & 31] : v;
}

void readColor(const uint8_t*& p, int type, bool be, Vin& v)
{
    v.a = 255;
    switch (type) {
    case 0: { uint32_t t = be ? be16(p) : (uint32_t)(p[0] | p[1] << 8); p += 2;
              v.r = (uint8_t)(((t >> 11) & 31) * 255 / 31); v.g = (uint8_t)(((t >> 5) & 63) * 255 / 63); v.b = (uint8_t)((t & 31) * 255 / 31); break; }
    case 1: v.r = p[0]; v.g = p[1]; v.b = p[2]; p += 3; break;
    case 2: v.r = p[0]; v.g = p[1]; v.b = p[2]; p += 4; break;
    case 3: { uint32_t t = be ? be16(p) : (uint32_t)(p[0] | p[1] << 8); p += 2;
              v.r = (uint8_t)(((t >> 12) & 15) * 17); v.g = (uint8_t)(((t >> 8) & 15) * 17); v.b = (uint8_t)(((t >> 4) & 15) * 17); v.a = (uint8_t)((t & 15) * 17); break; }
    case 4: { uint32_t t = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; p += 3;
              v.r = (uint8_t)(((t >> 18) & 63) * 255 / 63); v.g = (uint8_t)(((t >> 12) & 63) * 255 / 63); v.b = (uint8_t)(((t >> 6) & 63) * 255 / 63); v.a = (uint8_t)((t & 63) * 255 / 63); break; }
    default: v.r = p[0]; v.g = p[1]; v.b = p[2]; v.a = p[3]; p += 4; break;
    }
    v.hasC = true;
}

/* Decode one attribute value at p (direct data in a display list is big-endian; indexed array data is native). */
void decodeAttr(int attr, const AttrFmt& f, const uint8_t*& p, bool be, Vin& v)
{
    if (attr < A_POS) { if (attr == A_PNMTXIDX) v.mtx = p[0]; else v.tmi[attr - A_TEX0MTXIDX] = p[0]; ++p; return; }
    if (attr == A_POS) {
        v.x = readComp(p, f.type, f.frac, be); v.y = readComp(p, f.type, f.frac, be);
        v.z = f.cnt == 0 ? 0.0f : readComp(p, f.type, f.frac, be);
    } else if (attr == A_NRM) {
        v.nx = readComp(p, f.type, f.frac, be); v.ny = readComp(p, f.type, f.frac, be); v.nz = readComp(p, f.type, f.frac, be);
        if (f.cnt == 1) for (int i = 0; i < 6; ++i) readComp(p, f.type, f.frac, be);   /* NBT: skip binormal/tangent */
        v.hasN = true;
    } else if (attr == A_CLR0) {
        readColor(p, f.type, be, v);
    } else if (attr == A_CLR1) {
        Vin dummy; readColor(p, f.type, be, dummy);
    } else {
        float s = readComp(p, f.type, f.frac, be), t = f.cnt == 0 ? 0.0f : readComp(p, f.type, f.frac, be);
        if (attr == A_TEX0) { v.u = s; v.v = t; }
        else if (attr == A_TEX0 + 1) { v.u1 = s; v.v1 = t; }
        else if (attr == A_TEX0 + 2) { v.u2 = s; v.v2 = t; }
    }
}

/* mode 0: real. mode 1 (profile only): decode attributes, nothing else. mode 2 (profile only): decode + transform/light, no
   assembly or emission. Modes 1/2 never touch the ring or the assembler state. */
volatile float sSink;

/* Post-transform vertex cache for display lists. A GX list names vertices by index tuples; strips and quads repeat the same tuple
   many times. Everything makeVtx() computes depends only on the raw index bytes, the current matrix index (G.cur) and draw state
   that cannot change inside a list, so a hit is exact. The epoch is bumped at the start of every list and whenever G.cur changes. */
struct VCEntry { uint32_t epoch; uint8_t raw[16]; PVtx pv; };   /* (declared earlier as an incomplete type for PVtx::ce) */
VCEntry sVC[128];
struct DLCtx { uint8_t la[A_COUNT]; int nla, fm; } sDL;   /* attribute list of the draw being decoded (read by resolveLazy) */
uint32_t sVCEpoch = 1;
inline uint32_t vcHash(const uint8_t* p, uint32_t n) { uint32_t h = 2166136261u; for (uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u; return h ^ (h >> 11); }

/* ---- display-list segment culling ---------------------------------------------------------------------------------
   Log af7acc26: the course map is ~10 000 draw commands per frame (every triangle is its own 3-vertex GX_TRIANGLEFAN inside a few big
   lists), and 16 000 of the ~18 000 triangles are entirely off screen. Each one still paid for a command parse, a draw-state build, three
   vertex transforms and an outcode test (~9 us each = most of the 90 ms draw time). The game data (checked on stage1/forest.mod) is spatially
   coherent: 16 consecutive commands span 1-5 % of the list's footprint. So the first time a list is seen it is scanned once and cut into
   segments of consecutive draw commands with an object-space bounding box. On every later call the 8 box corners go through the same
   transform + outcode as a vertex; if they all share one outside bit, every vertex of every triangle in the segment shares it too, so the
   per-triangle test would have rejected them all and the segment is skipped without being parsed. The result is identical to testing each
   triangle. Lists with matrix-index changes, XF/BP commands, non-finite positions or varying per-vertex matrices are never culled.
   Positions are read from the vertex arrays when the list is first seen (static model data); the bounds are rebuilt every DLC_REBUILD uses
   of a list in case the game rewrote the array. */
#ifndef N3DS_DL_CULL
#define N3DS_DL_CULL 1
#endif
enum { DLC_SLOTS = 1024, DLC_MIN_CMDS = 24, DLC_SEG_CMDS = 24, DLC_SEG_VERTS = 120, DLC_MAX_SEGS = 3000, DLC_REBUILD = 900 };
struct DLSeg { uint32_t start, end, verts; float mn[3], mx[3]; };
struct DLInfo {
    const uint8_t* ptr; uint32_t n, sig, edge;
    uint8_t state;                 /* 0 empty, 1 ready (segments valid), 2 not cullable */
    int mtx;                       /* PNMTXIDX shared by every vertex, or -1 = no per-vertex matrix index (G.cur applies) */
    uint32_t nseg; DLSeg* seg;
    uint32_t uses, tested, culledSeg, skipUntil;
};
DLInfo sDLC[DLC_SLOTS];
uint32_t sDLCSegs = 0;
bool sDLCOn = true;   /* test hook: n3ds_gfx_debug_dlcull() */

inline uint32_t dlcSig()
{
    uint32_t h = 2166136261u;
    for (int a = 0; a < A_COUNT; ++a) h = (h ^ G.desc[a]) * 16777619u;
    for (int f = 0; f < 8; ++f) {
        const AttrFmt& t = G.fmt[f][A_POS]; h = (h ^ t.cnt) * 16777619u; h = (h ^ t.type) * 16777619u; h = (h ^ t.frac) * 16777619u;
    }
    h = (h ^ (uint32_t)(uintptr_t)G.arrBase[A_POS]) * 16777619u; h = (h ^ G.arrStride[A_POS]) * 16777619u;
    return h;
}
inline uint32_t dlcEdge(const uint8_t* p, uint32_t n)
{
    /* a cheap content fingerprint: 16 words spread over the list + the first position-array entry. Guards against a freed list's address being
       reused by a different one (stage change) with the same length and vertex-array address. */
    uint32_t h = 2166136261u, w;
    for (int i = 0; i < 16; ++i) { memcpy(&w, p + (size_t)((n - 4) / 15) * i, 4); h = (h ^ w) * 16777619u; }
    if (G.arrBase[A_POS]) { memcpy(&w, G.arrBase[A_POS], 4); h = (h ^ w) * 16777619u; memcpy(&w, G.arrBase[A_POS] + 4, 4); h = (h ^ w) * 16777619u; }
    return h;
}
void dlcFree(DLInfo& d) { if (d.seg) { free(d.seg); sDLCSegs -= d.nseg; } d.seg = nullptr; d.nseg = 0; d.state = 0; }

/* Scan one list. Leaves d.state = 1 (segments built) or 2 (not cullable). */
void dlcBuild(DLInfo& d, const uint8_t* list, uint32_t n)
{
    dlcFree(d);
    d.ptr = list; d.n = n; d.sig = dlcSig(); d.edge = dlcEdge(list, n); d.state = 2; d.mtx = -1; d.uses = 0; d.tested = d.culledSeg = d.skipUntil = 0;
    ++S.dlBuilt;
    if (G.desc[A_POS] == 0 || (G.desc[A_POS] != 1 && !G.arrBase[A_POS])) return;
    const uint8_t* p = list; const uint8_t* const end = list + n;
    static DLSeg tmp[DLC_MAX_SEGS]; uint32_t nseg = 0, totalCmds = 0;
    bool open = false; DLSeg cur; uint32_t curCmds = 0;
    int mtx = -2;
    memset(&cur, 0, sizeof cur);
    while (p < end) {
        const uint8_t* opPos = p; const uint8_t op = *p++;
        if (op == 0x00) continue;
        if (!(op & 0x80)) return;                                  /* matrix-index / XF / BP / call: leave this list alone */
        const int fm = op & 7; if (end - p < 2) return;
        const uint32_t count = be16(p); p += 2;
        uint32_t vsize = 0;
        for (int a = 0; a < A_COUNT; ++a) vsize += G.desc[a] == 1 ? (uint32_t)directSize(a, G.fmt[fm][a]) : G.desc[a] == 2 ? 1u : G.desc[a] == 3 ? 2u : 0u;
        if (vsize == 0 || (uint32_t)(end - p) < count * vsize) return;
        if (!open) { open = true; cur.start = (uint32_t)(opPos - list); cur.verts = 0; for (int k = 0; k < 3; ++k) { cur.mn[k] = 1e30f; cur.mx[k] = -1e30f; } curCmds = 0; }
        for (uint32_t i = 0; i < count; ++i, p += vsize) {
            const uint8_t* vp = p; Vin v; memset(&v, 0, sizeof v);
            for (int a = 0; a <= A_POS; ++a) {
                const int dsc = G.desc[a]; if (!dsc) continue;
                if (dsc == 1) { decodeAttr(a, G.fmt[fm][a], vp, true, v); continue; }
                const uint32_t idx = dsc == 2 ? vp[0] : be16(vp); vp += dsc == 2 ? 1 : 2;
                if (!G.arrBase[a]) { if (a == A_POS) return; continue; }
                const uint8_t* src = G.arrBase[a] + (size_t)idx * G.arrStride[a];
                decodeAttr(a, G.fmt[fm][a], src, false, v);
            }
            if (!(fin(v.x) && fin(v.y) && fin(v.z))) return;
            if (G.desc[A_PNMTXIDX]) { if (mtx == -2) mtx = v.mtx; else if (mtx != (int)v.mtx) return; }
            if (v.x < cur.mn[0]) cur.mn[0] = v.x;
            if (v.x > cur.mx[0]) cur.mx[0] = v.x;
            if (v.y < cur.mn[1]) cur.mn[1] = v.y;
            if (v.y > cur.mx[1]) cur.mx[1] = v.y;
            if (v.z < cur.mn[2]) cur.mn[2] = v.z;
            if (v.z > cur.mx[2]) cur.mx[2] = v.z;
        }
        cur.verts += count; ++curCmds; ++totalCmds;
        if (curCmds >= DLC_SEG_CMDS || cur.verts >= DLC_SEG_VERTS) {
            cur.end = (uint32_t)(p - list); if (nseg >= DLC_MAX_SEGS) return; tmp[nseg++] = cur; open = false;
        }
    }
    if (open) { cur.end = (uint32_t)(p - list); if (nseg >= DLC_MAX_SEGS) return; tmp[nseg++] = cur; }
    if (totalCmds < DLC_MIN_CMDS || nseg < 2 || sDLCSegs + nseg > DLC_MAX_SEGS) return;
    d.seg = (DLSeg*)malloc(nseg * sizeof(DLSeg));
    if (!d.seg) return;
    memcpy(d.seg, tmp, nseg * sizeof(DLSeg)); d.nseg = nseg; sDLCSegs += nseg;
    d.mtx = mtx == -2 ? -1 : mtx; d.state = 1;
}

/* Find (or build) the cull info of a list; nullptr = process it normally. */
DLInfo* dlcLookup(const uint8_t* list, uint32_t n)
{
    if (!sDLCOn || n < DLC_MIN_CMDS * 3) return nullptr;
    DLInfo& d = sDLC[(((uintptr_t)list >> 5) ^ ((uintptr_t)list >> 15)) & (DLC_SLOTS - 1)];
    const uint32_t sig = dlcSig();
    if (d.ptr != list || d.n != n || d.sig != sig || d.edge != dlcEdge(list, n) || d.uses >= DLC_REBUILD) dlcBuild(d, list, n);
    ++d.uses;
    if (d.state != 1 || d.skipUntil > d.uses) return nullptr;
    return &d;
}

/* true = nothing in the segment can produce a pixel: either every vertex is outside one clip plane, or (all corners in front of the camera) the
   box projects onto a pixel rectangle that holds no pixel centre, which is exactly emitTri's sub-pixel rejection applied to the box instead of
   to a triangle inside it (a triangle's bounding box lies within the projected box). Same planes and arithmetic as makeGeom + emitTri. */
inline bool boxInvisible(const float* bmn, const float* bmx, const float* C, float minPx = 0.0f)
{
    /* phase 1: the 8 corners' clip coordinates and outcodes (a box wholly outside one plane - the common case - is decided here, without a single divide) */
    uint8_t acc = 0xFF; bool allFront = true;
    float cxs[8], cys[8], ows[8];
    for (int c = 0; c < 8; ++c) {
        const float x = (c & 1) ? bmx[0] : bmn[0], y = (c & 2) ? bmx[1] : bmn[1], z = (c & 4) ? bmx[2] : bmn[2];
        const float cx = C[0] * x + C[1] * y + C[2] * z + C[3], cy = C[4] * x + C[5] * y + C[6] * z + C[7];
        const float oz = C[8] * x + C[9] * y + C[10] * z + C[11], ow = C[12] * x + C[13] * y + C[14] * z + C[15];
        const uint8_t oc = outcode(cx, cy, oz, ow);
        if (oc & OC_BAD) return false;
        acc &= oc;
        cxs[c] = cx; cys[c] = cy; ows[c] = ow;
        if (!(ow > 1e-6f)) allFront = false;
    }
    if (acc) return true;
    if (!allFront) return false;
    /* phase 2: every corner is in front of the camera - does the projected rectangle hold a pixel centre? */
    float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
    for (int c = 0; c < 8; ++c) {
        const float inv = 1.0f / ows[c], px = (cxs[c] * inv + 1.0f) * G.hvw, py = (1.0f - cys[c] * inv) * G.hvh;
        if (px < mnx) mnx = px;
        if (px > mxx) mxx = px;
        if (py < mny) mny = py;
        if (py > mxy) mxy = py;
    }
    if (minPx > 0.0f && mxx - mnx < minPx && mxy - mny < minPx) return true;       /* lod_px (lossy): the whole box is a few pixels at most */
    return fastFloor(mxx + 0.02f - 0.5f) < fastCeil(mnx - 0.02f - 0.5f) || fastFloor(mxy + 0.02f - 0.5f) < fastCeil(mny - 0.02f - 0.5f);
}
inline bool dlcSegInvisible(const DLSeg& s, const float* C) { return boxInvisible(s.mn, s.mx, C); }

void doDisplayList(const uint8_t* p, uint32_t n, int mode = 0, bool sub = false)
{
    const uint8_t* const base = p;
    DLInfo* dlc = nullptr; uint32_t dlcSi = 0; const float* dlcC = nullptr;
#if N3DS_DL_CULL
    if (mode == 0 && !sub) { dlc = dlcLookup(p, n); if (dlc) { computeCullBounds(); dlcC = clipFor((dlc->mtx >= 0 ? (unsigned)dlc->mtx : (unsigned)G.cur) & 63); } }
#endif
    const uint8_t* end = p + n;
    ++sVCEpoch;
    int lastFm = -1;
    while (p < end) {
        if (dlc) {                                                              /* segment culling (see DLInfo) */
            const uint32_t off = (uint32_t)(p - base);
            while (dlcSi < dlc->nseg && dlc->seg[dlcSi].start < off) ++dlcSi;
            if (dlcSi < dlc->nseg && dlc->seg[dlcSi].start == off) {
                const DLSeg& sg = dlc->seg[dlcSi++];
                ++S.segTests; ++dlc->tested;
                if (dlcSegInvisible(sg, dlcC)) {
                    ++S.segCulled; ++dlc->culledSeg; S.segVertsSkipped += sg.verts;
                    p = base + sg.end; continue;
                }
                if (dlc->tested >= 96 && dlc->culledSeg * 16 < dlc->tested) {    /* this list never culls (incoherent order?): stop paying for the tests for a while */
                    dlc->skipUntil = dlc->uses + 240; dlc->tested = dlc->culledSeg = 0; dlc = nullptr;
                }
            }
        }
        uint8_t op = *p++;
        if (op == 0x00 || op == 0x48) continue;                                  /* NOP, invalidate vertex cache */
        if (op == 0x08) { if (end - p < 5) break; if (p[0] == 0x30) { const uint32_t m = be32(p + 1) & 0x3F; if (m != (uint32_t)G.cur) { G.cur = (int)m; ++sVCEpoch; } } p += 5; continue; }   /* CP reg: MATINDEX_A */
        if (op == 0x10) { if (end - p < 4) break; uint32_t h = be32(p); p += 4 + 4 * (((h >> 16) & 0xFFFF) + 1); continue; }   /* XF load */
        if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) { p += 4; continue; }   /* indexed XF load */
        if (op == 0x40) { p += 8; continue; }                                     /* call DL: not followed */
        if (op == 0x61) { p += 4; continue; }                                     /* BP reg write */
        if (!(op & 0x80)) { if (!dlWarned) { dlWarned = true; n3ds_log("[gfx] display list: unknown opcode %02x, list abandoned", op); } return; }

        const int prim = op & 0xF8, fm = op & 7;
        if (fm != lastFm) { lastFm = fm; ++sVCEpoch; }          /* same index bytes decode differently under another vertex format */
        if (end - p < 2) break;
        const uint32_t count = be16(p); p += 2;
        uint32_t vsize = 0; uint8_t la[A_COUNT]; int nla = 0;   /* active attributes of this draw, in GX order (only these are visited per vertex) */
        for (int a = 0; a < A_COUNT; ++a) {
            if (G.desc[a]) la[nla++] = (uint8_t)a;
            vsize += G.desc[a] == 1 ? (uint32_t)directSize(a, G.fmt[fm][a]) : G.desc[a] == 2 ? 1u : G.desc[a] == 3 ? 2u : 0u;
        }
        if (vsize == 0 || (uint32_t)(end - p) < count * vsize) { if (!dlWarned) { dlWarned = true; n3ds_log("[gfx] display list: truncated draw (need %u, have %d)", count * vsize, (int)(end - p)); } return; }

        if (mode == 0) { primBegin(prim); if (!G.active) { p += count * vsize; continue; } }
        /* The key is the raw vertex bytes, so DIRECT attributes (e.g. the 1-byte matrix index GX_VA_PNMTXIDX that skinned models carry)
           are exactly as valid as indexed ones. 1l required every attribute to be indexed and so never hit the cache on such lists. */
        const bool cacheable = mode != 1 && vsize <= 16;
        const bool lazy = mode == 0;                 /* 1n: assemble position-only; attributes are decoded by emitTri for surviving triangles */
        if (lazy) { sDL.nla = nla; sDL.fm = fm; memcpy(sDL.la, la, (size_t)nla); }
        for (uint32_t i = 0; i < count; ++i) {
            VCEntry* ce = nullptr;
            if (cacheable) {
                ce = &sVC[vcHash(p, vsize) & 127];
                if (ce->epoch == sVCEpoch && memcmp(ce->raw, p, vsize) == 0) {
                    p += vsize; ++S.inVerts; ++S.vcHits;
                    if (mode == 0) asmPush(ce->pv); else sSink = ce->pv.v.x;
                    continue;
                }
            }
            const uint8_t* vstart = p;
            Vin v;
            if (lazy) {
                /* stage A: matrix index + position only (the attribute list is sorted, POS is the first attribute that matters here) */
                v.x = v.y = v.z = 0.0f; v.mtx = (uint8_t)G.cur;
                for (int k = 0; k < nla; ++k) {
                    const int a = la[k], d = G.desc[a];
                    if (a > A_POS) break;
                    if (d == 1) { decodeAttr(a, G.fmt[fm][a], p, true, v); continue; }
                    uint32_t idx = d == 2 ? p[0] : be16(p); p += d == 2 ? 1 : 2;
                    if (!G.arrBase[a]) continue;
                    const uint8_t* src = G.arrBase[a] + (size_t)idx * G.arrStride[a];
                    decodeAttr(a, G.fmt[fm][a], src, false, v);
                }
                p = vstart + vsize;
                PVtx pv; makeGeom(v, pv);
                pv.lazy = 1; pv.raw = vstart;
                if (ce) { ce->epoch = sVCEpoch; memcpy(ce->raw, vstart, vsize); pv.ce = ce; ce->pv = pv; asmPush(ce->pv); }
                else asmPush(pv);
                continue;
            }
            memset(&v, 0, sizeof v); v.r = v.g = v.b = v.a = 255; v.mtx = (uint8_t)G.cur;
            for (int k = 0; k < nla; ++k) {
                const int a = la[k], d = G.desc[a];
                if (d == 1) { decodeAttr(a, G.fmt[fm][a], p, true, v); continue; }
                uint32_t idx = d == 2 ? p[0] : be16(p); p += d == 2 ? 1 : 2;
                if (!G.arrBase[a]) continue;
                const uint8_t* src = G.arrBase[a] + (size_t)idx * G.arrStride[a];
                decodeAttr(a, G.fmt[fm][a], src, false, v);
            }
            if (mode == 1) continue;
            const PVtx pv = makeVtx(v);
            if (ce) { ce->epoch = sVCEpoch; memcpy(ce->raw, vstart, vsize); ce->pv = pv; }
            sSink = pv.v.x;
        }
    }
}

/* Decode the attributes of a position-only vertex from its raw display-list bytes. v.mtx is the value stage A saw (G.cur can have moved on
   by the time a later triangle resolves a cached vertex), so it is taken from the PVtx; the position itself is not needed again. */
bool sCmHasN = false, sCmHasC = false;   /* the cached mesh being drawn by the CPU (meshCpuBody) has normals / vertex colours */
void resolveLazy(PVtx& pv)
{
    if (pv.lazy == 2) {   /* vertex of a cached mesh: pv.raw points at its N3dsMeshVtx; the Vin below is exactly what the display-list decoder would have produced */
        const N3dsMeshVtx& mv = *reinterpret_cast<const N3dsMeshVtx*>(pv.raw);
        Vin v; memset(&v, 0, sizeof v);
        v.x = mv.x; v.y = mv.y; v.z = mv.z; v.nx = mv.nx; v.ny = mv.ny; v.nz = mv.nz;
        v.u = mv.u0; v.v = mv.v0; v.u1 = mv.u1; v.v1 = mv.v1; v.u2 = mv.u2; v.v2 = mv.v2;
        v.r = mv.r; v.g = mv.g; v.b = mv.b; v.a = mv.a;
        v.hasN = sCmHasN; v.hasC = sCmHasC; v.mtx = pv.mtx;
        { uint32_t tm; memcpy(&tm, &mv.nw, 4); v.tmi[0] = (uint8_t)tm; v.tmi[1] = (uint8_t)(tm >> 8); v.tmi[2] = (uint8_t)(tm >> 16); v.tmi[3] = (uint8_t)(tm >> 24); }   /* per-vertex texture-matrix ids (0 = none) */
        makeAttr(v, pv);
        pv.lazy = 0; ++S.lazyDone;
        return;
    }
    Vin v; memset(&v, 0, sizeof v); v.r = v.g = v.b = v.a = 255;
    const int fm = sDL.fm;
    const uint8_t* p = pv.raw;
    for (int k = 0; k < sDL.nla; ++k) {
        const int a = sDL.la[k], d = G.desc[a];
        if (d == 1) { decodeAttr(a, G.fmt[fm][a], p, true, v); continue; }
        uint32_t idx = d == 2 ? p[0] : be16(p); p += d == 2 ? 1 : 2;
        if (!G.arrBase[a]) continue;
        const uint8_t* src = G.arrBase[a] + (size_t)idx * G.arrStride[a];
        decodeAttr(a, G.fmt[fm][a], src, false, v);
    }
    v.mtx = pv.mtx;
    makeAttr(v, pv);
    pv.lazy = 0; ++S.lazyDone;
    if (pv.ce && pv.ce->epoch == sVCEpoch && pv.ce->pv.raw == pv.raw && &pv.ce->pv != &pv) pv.ce->pv = pv;   /* later cache hits skip the decode */
}

/* ================= GPU-transform mesh cache (1p) ===================================================================================
   The CPU path above decodes, transforms, culls and assembles every display-list vertex on every frame (log 8c8560e6: ~55 ms of the
   worker's frame, the bottleneck). Model display lists are static data, so the first time a list is drawn it is decoded ONCE into a
   model-space indexed triangle mesh in linear memory (vertices de-duplicated by their raw index tuple, the same GX primitive assembly
   as asmPush), and from then on the call costs a signature check, a few bounding-box tests and a uniform upload: the PICA vertex shader
   does the transform, texgen, colour and headlight (passthrough.v.pica, xform branch).
   Exactness: a chunk (24 commands / 120 vertices, like the segment culler) goes to the GPU only when its bounding box, through the
   current matrix, lies in front of the near plane, inside the +-guardV() band and not beyond the far plane: then the CPU path would neither
   clip nor alter any triangle of it, and the triangles it would have rejected (outside the scissor, sub-pixel) produce no pixel on the
   GPU either. Boxes that are entirely invisible are skipped (same test as dlcSegInvisible); any other chunk is drawn by the unchanged
   CPU path, in order. Lists with matrix loads / CP / BP / calls inside, per-vertex texture-matrix indices, position/normal texgen, more
   than 2 texture units or more than N3DS_MESH_SLOTS matrices, or > 65000 distinct vertices always take the CPU path.
   Invalidation: DCFlushRange/DCStoreRange over any byte the mesh was built from (queued in order with the draws), a heap reset
   (pc_gfx_invalidate_resident_meshes), a changed vertex descriptor/format/array, a changed fingerprint of the list, and a full content
   re-hash every MC_VERIFY uses as a safety net. Freed meshes are released 3 frames later (the GPU may still read them). */
enum { MC_SLOTS_LOG2 = 10, MC_SLOTS = 1 << MC_SLOTS_LOG2, MC_PROBE = 16, MC_MAXV = 65000, MC_VERIFY = 240, MC_MAXRNG = 8, MC_CHUNK_CMDS = 24, MC_CHUNK_VERTS = 120, MC_CPU_CHUNK_CMDS = 8, MC_CPU_CHUNK_VERTS = 36, MC_RETRY = 1200, MC_DEAD = 2048 };
struct MRange { uintptr_t lo, hi; };
struct MChunk { uint32_t start, end, i0, i1; float mn[3], mx[3]; };
struct MGroup { uint32_t c0, c1; float mn[3], mx[3]; };
enum { MC_GROUP = 6 };
struct Mesh {
    const uint8_t* ptr; uint32_t n, sig, edge;
    uint8_t state;                                   /* 0 empty, 1 ready, 2 not eligible (CPU path) */
    uint8_t nslot; bool usesCur, hasN, hasC;
    uint8_t slotId[N3DS_MESH_SLOTS];
    float smn[N3DS_MESH_SLOTS][3], smx[N3DS_MESH_SLOTS][3];
    N3dsMeshVtx* vtx; uint16_t* idx; uint32_t nv, ni, bytes;
    MChunk* ch; uint32_t nch;
    MRange rng[MC_MAXRNG]; int nrng; uint32_t hash, shash;   /* hash = full content hash of the source ranges; shash = sampled one (cheap periodic check) */
    uint32_t uses, builtAt;
    MGroup* grp; uint32_t ngrp;                      /* CPU mode: bounding boxes over MC_GROUP consecutive chunks (hierarchical culling) */
    uint32_t lastBuildUse, volUntil; uint8_t churn;  /* rebuild-thrash guard: a list whose data keeps changing is left to the normal path for a while */
    uint8_t memFails;                                /* consecutive builds that failed for lack of memory (retry sooner than for a permanent refusal, with back-off) */
    uint8_t cpuBad, checked, recov;                  /* the cached CPU draw of this list differed from the normal path: never use it */
    bool cpu;                                        /* built for the CPU draw (indexed, any number of matrices) */
    bool gpuFlushed;                                 /* r53: vertex / index data written back for the GPU */
    uint16_t* lidx; uint32_t* lrng; uint32_t lni; uint8_t lodState; float dbgExt; uint32_t dbgTris, dbgChunks, dbgLod;   /* r10: simplified index list for small on-screen sizes (malloc; CPU draw only) */
};
Mesh* sMC = nullptr;        /* MC_SLOTS entries (~0.5 MB), allocated on first use from the LINEAR heap: the malloc heap has only 0.3-1.8 MB to spare */
bool sMCTried = false;
bool mcEnsure()
{
    if (sMC) return true;
    if (sMCTried) return false;
    sMCTried = true;
    sMC = (Mesh*)n3ds_linear_alloc((unsigned)(sizeof(Mesh) * MC_SLOTS));
    if (!sMC) { n3ds_log("[mesh] no linear memory for the %u-entry model table: models stay on the normal path", (unsigned)MC_SLOTS); return false; }
    memset((void*)sMC, 0, sizeof(Mesh) * MC_SLOTS);
    n3ds_log("[mesh] model table: %u entries x %u bytes in linear memory", (unsigned)MC_SLOTS, (unsigned)sizeof(Mesh));
    return true;
}
struct DeadMesh { void* vtx; void* idx; void* ch; unsigned frame; };
DeadMesh sDead[MC_DEAD]; unsigned sDeadHead = 0, sDeadN = 0;
uint32_t sMCBytes = 0;
uintptr_t sMCLo = ~(uintptr_t)0, sMCHi = 0;          /* union of every cached range (fast reject for DCFlushRange) */
struct MStats { unsigned calls, gpuChunks, cpuChunks, invisChunks, built, buildFail, inval, verifyFail, tris, wholeCpu, badUnif, volatileLists, nonOpaque, wholeGpu; unsigned long long buildTicks; } MS, MSsum;
struct MTot { unsigned built, buildFail, volatileLists, inval, verifyFail; } MStot;   /* since start (reported by the [lite] line) */
const char* sMCLastReason = "";
unsigned sRb[6];   /* why lists were (re)built since the last [mesh] line: first use, evicted-by-collision, source changed, verify, retry, thrash timeout */
struct FailR { const char* why; unsigned n; } sFailR[14]; int sNFailR = 0;     /* why lists stay on the normal path (histogram, logged with the [mesh] line) */
void noteFail(const char* why)
{
    sMCLastReason = why;
    for (int i = 0; i < sNFailR; ++i) if (sFailR[i].why == why || strcmp(sFailR[i].why, why) == 0) { ++sFailR[i].n; return; }
    if (sNFailR < 14) { sFailR[sNFailR].why = why; sFailR[sNFailR].n = 1; ++sNFailR; }
}

/* Model-vertex arena: ONE linear-memory block allocated the first time a model is cached (size = mesh_cache_kb, at most 8 MB), handed out in
   256-byte pages with a first-fit scan. Per-model linearAlloc/linearFree calls made the linear heap fragment (a level-2 load ran out of linear memory
   once the model cache had been churning) and gave each model a different random place in it; a fixed arena removes both.
   r37: pages were 4 KB. Most models are small (a 1-3 KB vertex block + a few hundred bytes of indices, two allocations each), so rounding every
   allocation up to 4 KB filled the 6 MB arena with ~1.7 MB of real data (log__15: "arena 0 KB free of 6144 ... cached 1678 KB") and 168 models
   per area fell back to the slow decode-every-frame path. */
enum { AP_SHIFT = 8, AP_SIZE = 1 << AP_SHIFT };
uint8_t* sArena = nullptr; uint32_t sArenaPages = 0; bool sArenaTried = false;
uint16_t* sArenaRun = nullptr;                      /* run length (pages) at the first page of an allocation, 0xFFFF inside one, 0 = free page */
void* arenaAlloc(uint32_t bytes)
{
    if (!sArenaTried) {
        sArenaTried = true;
        int kb = n3dsConfig().meshCacheKb; if (kb > 8192) kb = 8192; if (kb < 256) kb = 256;
        sArenaPages = (uint32_t)kb * (1024u / AP_SIZE);
        sArena = (uint8_t*)n3ds_linear_alloc((size_t)sArenaPages * AP_SIZE);
        sArenaRun = (uint16_t*)calloc(sArenaPages, sizeof(uint16_t));
        if (!sArena || !sArenaRun) { n3ds_linear_free(sArena); free(sArenaRun); sArena = nullptr; sArenaRun = nullptr; sArenaPages = 0; }
        else n3ds_log("[mesh] model arena: %u KB of linear memory (%u-byte pages)", (unsigned)(sArenaPages * AP_SIZE / 1024u), (unsigned)AP_SIZE);
    }
    if (!sArena || !bytes) return nullptr;
    const uint32_t need = (bytes + AP_SIZE - 1u) >> AP_SHIFT;
    if (need >= 0xFFFFu || need > sArenaPages) return nullptr;
    auto take = [](uint32_t i, uint32_t n) -> void* { for (uint32_t q = 1; q < n; ++q) sArenaRun[i + q] = 0xFFFF; sArenaRun[i] = (uint16_t)n; return sArena + ((size_t)i << AP_SHIFT); };
    if (need >= (96u * 1024u >> AP_SHIFT)) {        /* big models (>= 96 KB) are placed from the TOP end, small ones from the bottom: the two kinds stop fragmenting each other */
        uint32_t end = sArenaPages;                 /* candidate block = [end - need, end) */
        while (end >= need) {
            uint32_t k = need;
            while (k > 0 && sArenaRun[end - need + k - 1] == 0) --k;
            if (k == 0) return take(end - need, need);
            end = end - need + k - 1;                 /* page (end - need + k - 1) is in use: the block must end at or before it */
        }
        return nullptr;
    }
    uint32_t i = 0;
    while (i + need <= sArenaPages) {
        uint32_t j = 0;
        while (j < need && sArenaRun[i + j] == 0) ++j;
        if (j == need) return take(i, need);
        const uint16_t r = sArenaRun[i + j];        /* the first used page after a free stretch is an allocation's first page: skip the whole allocation */
        i = i + j + (r != 0 && r != 0xFFFF ? r : 1u);
    }
    return nullptr;
}
void arenaFree(void* p)
{
    if (!p || !sArena) return;
    const uint8_t* q = (const uint8_t*)p;
    if (q < sArena || q >= sArena + ((size_t)sArenaPages << AP_SHIFT)) return;
    const uint32_t i = (uint32_t)((size_t)(q - sArena) >> AP_SHIFT);
    const uint32_t n = sArenaRun[i];
    if (n == 0 || n == 0xFFFF) return;
    for (uint32_t k = 0; k < n; ++k) sArenaRun[i + k] = 0;
}

static uint32_t sLodBytes = 0;
static void meshLodFree(Mesh& m)
{
    if (m.lidx || m.lrng) sLodBytes -= m.lni * 2u + m.nch * 8u;
    free(m.lidx); free(m.lrng); m.lidx = nullptr; m.lrng = nullptr; m.lni = 0; m.lodState = 0;
}
void meshRelease(Mesh& m)
{
    if (m.vtx || m.idx || m.ch) {
        if (sDeadN == MC_DEAD) {                     /* graveyard full (never expected): free the oldest now */
            DeadMesh& d = sDead[sDeadHead]; arenaFree(d.vtx); arenaFree(d.idx); free(d.ch);
            sDeadHead = (sDeadHead + 1) % MC_DEAD; --sDeadN;
        }
        DeadMesh& d = sDead[(sDeadHead + sDeadN) % MC_DEAD]; ++sDeadN;
        d.vtx = m.vtx; d.idx = m.idx; d.ch = m.ch; d.frame = G.frameNo;
        sMCBytes -= m.bytes;
    }
    free(m.grp); m.grp = nullptr; m.ngrp = 0;
    meshLodFree(m);
    m.vtx = nullptr; m.idx = nullptr; m.ch = nullptr; m.nch = 0; m.nv = m.ni = m.bytes = 0; m.nrng = 0;
}
void meshKill(Mesh& m) { meshRelease(m); m.state = 0; m.ptr = nullptr; m.n = 0; m.churn = 0; m.volUntil = 0; m.cpuBad = 0; m.recov = 0; m.checked = 0; m.lastBuildUse = 0; m.cpu = false; m.memFails = 0; }

void meshReap(unsigned frame)                        /* frame end: free what the GPU can no longer be reading */
{
    while (sDeadN && sDead[sDeadHead].frame + 3 <= frame) {
        DeadMesh& d = sDead[sDeadHead]; arenaFree(d.vtx); arenaFree(d.idx); free(d.ch);
        sDeadHead = (sDeadHead + 1) % MC_DEAD; --sDeadN;
    }
}

inline uint32_t meshSig()
{
    uint32_t h = 2166136261u;
    for (int a = 0; a < A_COUNT; ++a) {
        const int d = G.desc[a];
        h = (h ^ (uint32_t)d) * 16777619u;
        if (!d) continue;
        for (int f = 0; f < 8; ++f) { const AttrFmt& t = G.fmt[f][a]; h = (h ^ (uint32_t)(t.cnt | t.type << 8 | t.frac << 16)) * 16777619u; }
        if (d >= 2) { h = (h ^ (uint32_t)(uintptr_t)G.arrBase[a]) * 16777619u; h = (h ^ G.arrStride[a]) * 16777619u; }
    }
    return h;
}
uint32_t meshHashRanges(const MRange* r, int nr)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < nr; ++i) {
        const uint8_t* p = (const uint8_t*)r[i].lo; const uint8_t* e = (const uint8_t*)r[i].hi;
        uint32_t w;
        for (; p + 4 <= e; p += 4) { memcpy(&w, p, 4); h = (h ^ w) * 16777619u; }
        for (; p < e; ++p) h = (h ^ *p) * 16777619u;
    }
    return h;
}

/* Cheap periodic check: up to 96 words spread over each range (the full hash of a big shared vertex array cost 15-20 ms per model, once per
   MC_VERIFY uses of every model). It is a safety net behind the DCFlushRange invalidation, not the primary mechanism. */
uint32_t meshHashSample(const MRange* r, int nr)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < nr; ++i) {
        const uint8_t* p = (const uint8_t*)r[i].lo; const size_t len = r[i].hi > r[i].lo ? (size_t)(r[i].hi - r[i].lo) : 0;
        h = (h ^ (uint32_t)len) * 16777619u;
        if (len < 4) continue;
        size_t step = (len / 96) & ~(size_t)3; if (step < 4) step = 4;
        uint32_t w;
        for (size_t o = 0; o + 4 <= len; o += step) { memcpy(&w, p + o, 4); h = (h ^ w) * 16777619u; }
        memcpy(&w, p + ((len - 4) & ~(size_t)3), 4); h = (h ^ w) * 16777619u;
    }
    return h;
}

/* Decode the list into a mesh. On any reason to stay on the CPU path, leaves state 2. */
void meshBuild(Mesh& m, const uint8_t* list, uint32_t n)
{
    const unsigned long long t0 = n3ds_ticks();
    meshRelease(m);
    m.ptr = list; m.n = n; m.sig = meshSig(); m.edge = dlcEdge(list, n); m.state = 2; m.uses = 0; m.builtAt = 0;
    /* r53: with mesh_cpu = 1 (default) every model is built in the CPU-draw format, and the GPU path draws from it too: a list the shader cannot
       take (environment map, too many lights, ...) or a piece crossing the near plane then gets the CACHED CPU draw, not a full re-decode. */
    const bool cpuMode = n3dsConfig().meshCpu || !n3dsConfig().gpuXform;
    m.cpu = cpuMode; m.gpuFlushed = false;
    m.nslot = 0; m.usesCur = G.desc[A_PNMTXIDX] == 0;
    /* hasN / hasC exactly as the display-list decoder sets Vin::hasN / hasC: direct data, or an index with an array behind it */
    m.hasN = G.desc[A_NRM] == 1 || (G.desc[A_NRM] >= 2 && G.arrBase[A_NRM]);
    m.hasC = G.desc[A_CLR0] == 1 || (G.desc[A_CLR0] >= 2 && G.arrBase[A_CLR0]);
    m.lastBuildUse = m.uses; m.checked = 0;
    ++MS.built;
    #define MC_FAIL(why) do { noteFail(why); ++MS.buildFail; goto fail; } while (0)
    {
    if (!G.desc[A_POS] || (G.desc[A_POS] >= 2 && !G.arrBase[A_POS])) MC_FAIL("no position");
    /* Per-vertex texture-matrix ids (GX_VA_TEX0MTXIDX..): log cea14c68 had 55-72 such lists per frame on the slow normal path (~25 ms of worker time). The
       CPU cache stores ids of units 0-3 in the 4 spare bytes of each mesh vertex (nw, unused by the CPU draw) and makeAttr reads them back exactly as the
       list decoder would have; the GPU path has no use for them and still refuses such lists. */
    for (int a = A_TEX0MTXIDX; a < A_POS; ++a) if (G.desc[a]) { if (!cpuMode) MC_FAIL("per-vertex texture matrix"); if (a >= A_TEX0MTXIDX + 4) MC_FAIL("per-vertex texture matrix of unit > 3"); }
    /* pass 1: validate the command stream, count vertex entries */
    const uint8_t* const end = list + n;
    uint32_t entries = 0, cmds = 0;
    for (const uint8_t* p = list; p < end;) {
        const uint8_t op = *p++;
        if (op == 0x00 || op == 0x48) continue;
        if (!(op & 0x80)) MC_FAIL("matrix/XF/BP/call command inside the list");
        const int fm = op & 7;
        if (end - p < 2) MC_FAIL("truncated");
        const uint32_t count = be16(p); p += 2;
        uint32_t vsize = 0;
        for (int a = 0; a < A_COUNT; ++a) vsize += G.desc[a] == 1 ? (uint32_t)directSize(a, G.fmt[fm][a]) : G.desc[a] == 2 ? 1u : G.desc[a] == 3 ? 2u : 0u;
        if (vsize == 0 || vsize > 32 || (uint32_t)(end - p) < count * vsize) MC_FAIL("vertex size / truncated draw");
        p += count * vsize; entries += count; ++cmds;
    }
    if (entries < 3) MC_FAIL("no triangles");
    /* pass 2: decode, de-duplicate, assemble */
    uint32_t cap = 64; while (cap < entries * 2) cap <<= 1;
    N3dsMeshVtx* tv = (N3dsMeshVtx*)malloc((size_t)entries * sizeof(N3dsMeshVtx));
    const uint8_t** traw = (const uint8_t**)malloc((size_t)entries * sizeof(void*));
    uint8_t* tfm = (uint8_t*)malloc(entries);
    uint32_t* hs = (uint32_t*)calloc(cap, sizeof(uint32_t));
    uint16_t* ti = (uint16_t*)malloc((size_t)entries * 3 * sizeof(uint16_t));
    uint32_t* li = (uint32_t*)malloc((size_t)entries * sizeof(uint32_t));
    MChunk* tch = (MChunk*)malloc(((size_t)cmds + 1) * sizeof(MChunk));
    bool ok = tv && traw && tfm && hs && ti && li && tch;
    uint32_t nv = 0, ni = 0, nch = 0;
    MRange arr[A_COUNT]; for (int a = 0; a < A_COUNT; ++a) { arr[a].lo = ~(uintptr_t)0; arr[a].hi = 0; }
    const char* why = ok ? nullptr : "out of memory (temp)";
    for (int s = 0; s < N3DS_MESH_SLOTS; ++s) for (int k = 0; k < 3; ++k) { m.smn[s][k] = 1e30f; m.smx[s][k] = -1e30f; }
    MChunk cur; bool open = false; uint32_t curCmds = 0, curVerts = 0;
    const uint32_t chunkCmds = cpuMode ? MC_CPU_CHUNK_CMDS : MC_CHUNK_CMDS, chunkVerts = cpuMode ? MC_CPU_CHUNK_VERTS : MC_CHUNK_VERTS;   /* CPU mode: finer boxes (testing a box is far cheaper than drawing its triangles) */
    for (const uint8_t* p = list; ok && p < end;) {
        const uint8_t* opPos = p; const uint8_t op = *p++;
        if (op == 0x00 || op == 0x48) continue;
        const int prim = op & 0xF8, fm = op & 7;
        const uint32_t count = be16(p); p += 2;
        uint32_t vsize = 0; uint8_t la[A_COUNT]; int nla = 0;
        for (int a = 0; a < A_COUNT; ++a) { if (G.desc[a]) la[nla++] = (uint8_t)a; vsize += G.desc[a] == 1 ? (uint32_t)directSize(a, G.fmt[fm][a]) : G.desc[a] == 2 ? 1u : G.desc[a] == 3 ? 2u : 0u; }
        const bool tri = prim == P_QUADS || prim == P_TRIANGLES || prim == P_STRIP || prim == P_FAN;
        if (!tri) { p += count * vsize; continue; }          /* lines / points: the CPU path draws nothing for them either */
        if (!open) { open = true; cur.start = (uint32_t)(opPos - list); cur.i0 = ni; for (int k = 0; k < 3; ++k) { cur.mn[k] = 1e30f; cur.mx[k] = -1e30f; } curCmds = curVerts = 0; }
        for (uint32_t i = 0; i < count; ++i, p += vsize) {
            /* de-duplicate by (format, raw bytes) */
            uint32_t hh = vcHash(p, vsize) ^ (uint32_t)fm * 0x9E3779B1u;
            uint32_t slot = hh & (cap - 1), found = 0;
            while (hs[slot]) {
                const uint32_t c = hs[slot] - 1;
                if (tfm[c] == fm && memcmp(traw[c], p, vsize) == 0) { found = c + 1; break; }
                slot = (slot + 1) & (cap - 1);
            }
            uint32_t vi;
            if (found) vi = found - 1;
            else {
                Vin v; memset(&v, 0, sizeof v); v.r = v.g = v.b = v.a = 255; v.mtx = (uint8_t)G.cur;
                const uint8_t* q = p;
                for (int k = 0; k < nla; ++k) {
                    const int a = la[k], d = G.desc[a];
                    if (d == 1) { decodeAttr(a, G.fmt[fm][a], q, true, v); continue; }
                    const uint32_t idx = d == 2 ? q[0] : be16(q); q += d == 2 ? 1 : 2;
                    if (!G.arrBase[a]) continue;
                    const uint8_t* src = G.arrBase[a] + (size_t)idx * G.arrStride[a];
                    const uint8_t* s0 = src;
                    decodeAttr(a, G.fmt[fm][a], src, false, v);
                    if ((uintptr_t)s0 < arr[a].lo) arr[a].lo = (uintptr_t)s0;
                    if ((uintptr_t)src > arr[a].hi) arr[a].hi = (uintptr_t)src;
                }
                if (!(fin(v.x) && fin(v.y) && fin(v.z))) { why = "non-finite position"; ok = false; break; }
                int sl = 0;
                if (!m.usesCur) {
                    const uint8_t id = (uint8_t)(v.mtx & 63);
                    for (sl = 0; sl < m.nslot && m.slotId[sl] != id; ++sl) {}
                    if (sl == m.nslot) { if (m.nslot >= N3DS_MESH_SLOTS) { why = "too many matrices"; ok = false; break; } m.slotId[m.nslot++] = id; }
                }
                if (nv >= MC_MAXV) { why = "too many vertices"; ok = false; break; }
                vi = nv++;
                N3dsMeshVtx& o = tv[vi];
                o.x = v.x; o.y = v.y; o.z = v.z; o.r = v.r; o.g = v.g; o.b = v.b; o.a = v.a;
                o.u0 = v.u; o.v0 = v.v; o.u1 = v.u1; o.v1 = v.v1; o.u2 = v.u2; o.v2 = v.v2;
                o.nx = o.ny = o.nz = o.nw = 0.0f;
                if (v.hasN && fin(v.nx) && fin(v.ny) && fin(v.nz)) { o.nx = v.nx; o.ny = v.ny; o.nz = v.nz; }
                if (cpuMode) { const uint32_t tm = (uint32_t)v.tmi[0] | (uint32_t)v.tmi[1] << 8 | (uint32_t)v.tmi[2] << 16 | (uint32_t)v.tmi[3] << 24; memcpy(&o.nw, &tm, 4); }   /* per-vertex texture-matrix ids of units 0-3 */
                o.slot = (float)sl;
                for (int k = 0; k < 3; ++k) { const float c = k == 0 ? v.x : k == 1 ? v.y : v.z; if (c < m.smn[sl][k]) m.smn[sl][k] = c; if (c > m.smx[sl][k]) m.smx[sl][k] = c; }
                traw[vi] = p; tfm[vi] = (uint8_t)fm; hs[slot] = vi + 1;
            }
            li[i] = vi;
            const N3dsMeshVtx& o = tv[vi];
            if (o.x < cur.mn[0]) cur.mn[0] = o.x; if (o.x > cur.mx[0]) cur.mx[0] = o.x;
            if (o.y < cur.mn[1]) cur.mn[1] = o.y; if (o.y > cur.mx[1]) cur.mx[1] = o.y;
            if (o.z < cur.mn[2]) cur.mn[2] = o.z; if (o.z > cur.mx[2]) cur.mx[2] = o.z;
        }
        if (!ok) break;
        /* GX primitive -> triangle list, same order and winding as asmPush; degenerate triangles are dropped (they draw nothing) */
        auto put = [&](uint32_t a, uint32_t b, uint32_t c) { if (a == b || b == c || a == c) return; ti[ni++] = (uint16_t)a; ti[ni++] = (uint16_t)b; ti[ni++] = (uint16_t)c; };
        if (prim == P_TRIANGLES) { for (uint32_t i = 0; i + 2 < count; i += 3) put(li[i], li[i + 1], li[i + 2]); }
        else if (prim == P_QUADS) { for (uint32_t i = 0; i + 3 < count; i += 4) { put(li[i], li[i + 1], li[i + 2]); put(li[i], li[i + 2], li[i + 3]); } }
        else if (prim == P_STRIP) { for (uint32_t i = 2; i < count; ++i) { if (i & 1) put(li[i - 1], li[i - 2], li[i]); else put(li[i - 2], li[i - 1], li[i]); } }
        else { for (uint32_t i = 2; i < count; ++i) put(li[0], li[i - 1], li[i]); }
        curVerts += count; ++curCmds;
        if (curCmds >= chunkCmds || curVerts >= chunkVerts) { cur.end = (uint32_t)(p - list); cur.i1 = ni; tch[nch++] = cur; open = false; }
    }
    if (ok && open) { cur.end = (uint32_t)(end - list); cur.i1 = ni; tch[nch++] = cur; }
    if (ok && ni < 3) { ok = false; why = "no triangles"; }
    if (ok && m.usesCur) m.nslot = 1;
    /* mesh_safe = 1 (default): only single-matrix meshes, stored as a plain triangle list (vertex k = index k) and drawn with DrawArrays like
       the CPU ring, with the shader's fixed-register branch. That avoids the two things only the GPU path used on hardware: indexed drawing
       and mova / relative uniform addressing - one of them produced garbage geometry on the console while the PICA interpreter was happy. */
    const bool safe = !cpuMode && n3dsConfig().meshSafe != 0;
    if (ok && safe && m.nslot > 1) { ok = false; why = "several matrices (mesh_safe = 1)"; }
    if (ok) {
        const uint32_t nvOut = safe ? ni : nv;
        const uint32_t vb = nvOut * (uint32_t)sizeof(N3dsMeshVtx), ib = safe ? 0u : ((ni * 2 + 15) & ~15u), total = vb + ib;
        const int budgetKb = n3dsConfig().meshCacheKb;
        if (sMCBytes + total > (uint32_t)(budgetKb < 0 ? 0 : budgetKb) * 1024u) { ok = false; why = "mesh_cache_kb budget full"; }
        else if (!sArenaTried && n3ds_linear_free_kb() < 24576u) { ok = false; why = "linear memory low (kept for textures)"; static bool sWarned = false; if (!sWarned) { sWarned = true; n3ds_log("[mesh] MODEL CACHE REFUSED: only %u KB of linear memory free, %u KB needed before the first model is cached. Without the cache every model is decoded again each frame (field log 4a5c3ceb: 7 FPS in Forest of Hope). Build the CIA with ./build_3ds.sh --mode 178 (80 MB linear heap)", n3ds_linear_free_kb(), 24576u); } }
        else {
            m.vtx = (N3dsMeshVtx*)arenaAlloc(vb); m.idx = safe ? nullptr : (uint16_t*)arenaAlloc(ib);
            m.ch = (MChunk*)malloc(nch * sizeof(MChunk));
            const uint32_t ngrp = cpuMode ? (nch + MC_GROUP - 1) / MC_GROUP : 0;
            m.grp = ngrp ? (MGroup*)malloc(ngrp * sizeof(MGroup)) : nullptr;
            if (!m.vtx || (!safe && !m.idx) || !m.ch || (ngrp && !m.grp)) {
                why = !m.vtx ? "arena full/fragmented (vertices)" : (!safe && !m.idx) ? "arena full/fragmented (indices)" : !m.ch ? "heap full (chunk table)" : "heap full (group table)";
                { static unsigned nA = 0; if (nA < 6) { ++nA; unsigned fr = 0, run = 0, best = 0; for (uint32_t k = 0; k < sArenaPages; ++k) { if (sArenaRun[k] == 0) { ++fr; ++run; if (run > best) best = run; } else run = 0; }
                    n3ds_log("[mesh] %s: wanted %u KB vertices + %u KB indices; arena %u KB free of %u, largest free block %u KB; cached %u KB", why, vb / 1024, ib / 1024, fr * AP_SIZE / 1024u, sArenaPages * AP_SIZE / 1024u, best * AP_SIZE / 1024u, sMCBytes / 1024); } }
                arenaFree(m.vtx); arenaFree(m.idx); free(m.ch); free(m.grp); m.vtx = nullptr; m.idx = nullptr; m.ch = nullptr; m.grp = nullptr; ok = false; }
            else {
                if (safe) { for (uint32_t k = 0; k < ni; ++k) m.vtx[k] = tv[ti[k]]; }      /* de-indexed: run (first, count) = vertex range */
                else { memcpy(m.vtx, tv, vb); memcpy(m.idx, ti, ni * 2); }
                memcpy(m.ch, tch, nch * sizeof(MChunk));
                m.ngrp = ngrp;
                for (uint32_t g = 0; g < ngrp; ++g) {
                    MGroup& gr = m.grp[g]; gr.c0 = g * MC_GROUP; gr.c1 = gr.c0 + MC_GROUP < nch ? gr.c0 + MC_GROUP : nch;
                    for (int k = 0; k < 3; ++k) { gr.mn[k] = 1e30f; gr.mx[k] = -1e30f; }
                    for (uint32_t c = gr.c0; c < gr.c1; ++c) for (int k = 0; k < 3; ++k) { if (tch[c].mn[k] < gr.mn[k]) gr.mn[k] = tch[c].mn[k]; if (tch[c].mx[k] > gr.mx[k]) gr.mx[k] = tch[c].mx[k]; }
                }
                m.gpuFlushed = n3dsConfig().gpuXform != 0;
                if (m.gpuFlushed) { n3ds_flush_dcache(m.vtx, vb); if (m.idx) n3ds_flush_dcache(m.idx, ib); }   /* the GPU reads it (r53: CPU-format meshes too) */
                m.nv = nvOut; m.ni = ni; m.nch = nch; m.bytes = total; sMCBytes += total;
                m.nrng = 0;
                m.rng[m.nrng].lo = (uintptr_t)list; m.rng[m.nrng].hi = (uintptr_t)end; ++m.nrng;
                for (int a = 0; a < A_COUNT && m.nrng < MC_MAXRNG; ++a) if (arr[a].hi > arr[a].lo) m.rng[m.nrng++] = arr[a];
                for (int i = 0; i < m.nrng; ++i) { if (m.rng[i].lo < sMCLo) sMCLo = m.rng[i].lo; if (m.rng[i].hi > sMCHi) sMCHi = m.rng[i].hi; }
                m.hash = meshHashRanges(m.rng, m.nrng); m.shash = meshHashSample(m.rng, m.nrng);
                m.state = 1;
            }
        }
    }
    free(tv); free(traw); free(tfm); free(hs); free(ti); free(li); free(tch);
    if (!ok) {
        noteFail(why ? why : "?"); ++MS.buildFail;
        /* Lack of memory is usually temporary (a level change frees a lot, the graveyard releases pages 3 frames later): try again after 20 uses,
           then 40, 80 ... up to the normal 1200. Log f60c4ca9: 82 big models failed once while a level loaded and then stayed on the slow path
           (~31 ms per frame) for the next 1200 uses = minutes. */
        if (why && (strstr(why, "arena") || strstr(why, "heap") || strstr(why, "out of memory") || strstr(why, "budget") || strstr(why, "linear memory"))) {
            const uint32_t delay = m.memFails < 6 ? (20u << m.memFails) : (uint32_t)MC_RETRY;
            if (m.memFails < 250) ++m.memFails;
            m.builtAt = 0u - (uint32_t)(MC_RETRY - (delay < (uint32_t)MC_RETRY ? delay : (uint32_t)MC_RETRY));
        }
    } else if (m.state == 1) m.memFails = 0;
    }
fail:
    #undef MC_FAIL
    MS.buildTicks += n3ds_ticks() - t0;
    {   /* the first builds are logged one by one: if anything ever hangs here, the log shows the list it was working on */
        static unsigned nLogged = 0;
        if (nLogged < 16) { ++nLogged;
            n3ds_log("[mesh] build #%u: list %p, %u bytes -> %s (%u verts, %u tris, %u chunks, %u matrices) in %.2f ms", nLogged, (const void*)list, n,
                     m.state == 1 ? "GPU" : sMCLastReason, m.nv, m.ni / 3, m.nch, (unsigned)m.nslot, (double)(n3ds_ticks() - t0) / N3DS_TICKS_PER_MS); }
    }
}

/* A list whose data keeps changing (the game rewrites its vertex array every frame: CPU-skinned or animated models) must not be rebuilt on
   every use (2-19 ms each): after 3 builds in quick succession it is left to the normal path for MC_VOLATILE uses, then tried once more. */
enum { MC_VOLATILE = 3600 };
void meshRebuildGuarded(Mesh& m, const uint8_t* list, uint32_t n)
{
    if (m.uses < 120) { if (m.churn < 250) ++m.churn; } else m.churn = 0;
    if (m.churn >= 3) {
        const uint8_t ch = m.churn;
        meshRelease(m);
        m.state = 2; m.sig = meshSig(); m.edge = dlcEdge(list, n); m.uses = 0; m.builtAt = 0; m.volUntil = MC_VOLATILE; m.churn = ch; m.cpu = n3dsConfig().meshCpu || !n3dsConfig().gpuXform;
        ++MS.volatileLists;
        return;
    }
    meshBuild(m, list, n);
}

Mesh* meshLookup(const uint8_t* list, uint32_t n)
{
    if (!mcEnsure()) return nullptr;
    const uint32_t h = (uint32_t)((((uintptr_t)list >> 4) ^ ((uintptr_t)list >> 13)) * 2654435761u) >> (32 - MC_SLOTS_LOG2);
    Mesh* victim = nullptr;
    for (int k = 0; k < MC_PROBE; ++k) {
        Mesh& m = sMC[(h + k) & (MC_SLOTS - 1)];
        if (m.ptr == list && m.n == n) {
            ++m.uses;
            if (m.state == 2 && m.volUntil) {                                  /* thrashing list: normal path until the timeout */
                if (m.uses < m.volUntil) return &m;
                m.volUntil = 0; m.churn = 2; ++sRb[5];
                meshBuild(m, list, n);
                return &m;
            }
            if (m.cpu != (n3dsConfig().meshCpu || !n3dsConfig().gpuXform)) { ++sRb[2]; meshBuild(m, list, n); return &m; }   /* built in the other format (a test / the A/B switch changed it at run time) */
            if (n3dsConfig().gpuXform && !m.gpuFlushed && m.state == 1) {   /* r53: built while gpu_xform was 0 (A/B switch): the GPU needs it written back */
                n3ds_flush_dcache(m.vtx, m.nv * (uint32_t)sizeof(N3dsMeshVtx)); if (m.idx) n3ds_flush_dcache(m.idx, ((m.ni * 2 + 15) & ~15u));
                m.gpuFlushed = true;
            }
            if (m.sig != meshSig() || m.edge != dlcEdge(list, n)) { ++sRb[2]; meshRebuildGuarded(m, list, n); }
            else if (m.state == 2 && m.uses - m.builtAt > MC_RETRY) { ++sRb[4]; meshBuild(m, list, n); }
            else if (m.state == 1 && (n3dsConfig().meshVerify || m.uses % MC_VERIFY == 0)
                     && (n3dsConfig().meshVerify ? meshHashRanges(m.rng, m.nrng) != m.hash : meshHashSample(m.rng, m.nrng) != m.shash)) {
                static int nVf = 0;
                if (nVf < 12) { ++nVf; n3ds_log("[mesh] VERIFY mismatch #%d: list %p, %u bytes, use %u, %d range(s): the game changed this model's data since it was cached", nVf, (const void*)list, n, (unsigned)m.uses, m.nrng); }
                ++MS.verifyFail; ++sRb[3]; meshRebuildGuarded(m, list, n); }
            return &m;
        }
        if (!victim || m.state == 0 || (victim->state != 0 && m.uses < victim->uses)) victim = &m;
    }
    for (int k = 0; k < MC_PROBE; ++k) { Mesh& m = sMC[(h + k) & (MC_SLOTS - 1)]; if (m.uses > 1) m.uses >>= 1; }   /* ageing, so a dead entry can be replaced */
    ++sRb[victim->state != 0 ? 1 : 0];
    meshKill(*victim);
    meshBuild(*victim, list, n);
    victim->uses = 1;
    return victim;
}

/* Visibility of a set of model-space boxes under one clip matrix each (corners of all of them form one point set).
   0 = nothing can be visible, 1 = GPU-safe (no clipping needed, not beyond the far plane), 2 = needs the CPU path. */
/* r54b: interval version of meshClassify, run first. A box's clip-space coordinates are bounded per linear form R (a row of the clip matrix, or a
   plane built from rows) by R.centre +- sum |R_j| half_j: that bound holds for EVERY point of the box, so for its 8 corners too. Hence
     1 = every point is in front of the camera, inside the near/far planes and the (99.9 % of the) guard band: exactly what meshClassify's 1 needs
         (it may still have answered 0 for a box between pixel centres: the GPU then draws nothing there, the same picture);
     0 = every point is outside one culling plane: meshClassify's 0 too;
    -1 = undecided: the exact 8-corner test decides.
   ~60 multiplies, no divide, ~15 compares per box instead of 8 corner transforms, 8 divides and ~80 compares (log 25: ~22 us per GPU-path call
   in culling, mostly skinned Pikmin with 8 matrix boxes each). A margin of 1e-5 of the terms' magnitude keeps float rounding on the safe side;
   a non-finite matrix or box makes every test false = undecided (the exact test then says 2). */
int meshClassifyFast(const float* const* C, const float (*mn)[3], const float (*mx)[3], int nb)
{
    const float g = guardV() * 0.999f;
    unsigned outAll = 0xFFu; bool safe = true;
    for (int b = 0; b < nb; ++b) {
        const float* M = C[b];
        const float c[3] = { 0.5f * (mn[b][0] + mx[b][0]), 0.5f * (mn[b][1] + mx[b][1]), 0.5f * (mn[b][2] + mx[b][2]) };
        const float h[3] = { 0.5f * (mx[b][0] - mn[b][0]), 0.5f * (mx[b][1] - mn[b][1]), 0.5f * (mx[b][2] - mn[b][2]) };
        auto iv = [&](const float* R, float& lo, float& hi, float& m) {
            const float v = R[0] * c[0] + R[1] * c[1] + R[2] * c[2] + R[3];
            const float r = fabsf(R[0]) * h[0] + fabsf(R[1]) * h[1] + fabsf(R[2]) * h[2];
            m = 1e-5f * (fabsf(R[0] * c[0]) + fabsf(R[1] * c[1]) + fabsf(R[2] * c[2]) + fabsf(R[3]) + r) + 1e-30f;
            lo = v - r; hi = v + r;
        };
        float X[4], Y[4], Z[4], W[4], P[4];
        for (int j = 0; j < 4; ++j) { X[j] = M[j]; Y[j] = M[4 + j]; Z[j] = M[8 + j]; W[j] = M[12 + j]; }
        float lo, hi, m;
        unsigned out = 0;
        /* w > 1e-6 for every point (allFront) */
        iv(W, lo, hi, m); if (!(lo > 1e-6f + m)) safe = false;
        /* near: z + w >= 0 */
        for (int j = 0; j < 4; ++j) P[j] = Z[j] + W[j];
        iv(P, lo, hi, m); if (!(lo >= m)) safe = false; if (hi < -m) out |= OC_NEAR;
        /* far: -z >= 0 */
        for (int j = 0; j < 4; ++j) P[j] = -Z[j];
        iv(P, lo, hi, m); if (!(lo >= m)) safe = false; if (hi < -m) out |= OC_FAR;
        if (G.cullOn) {   /* the culling planes (a box wholly outside one is invisible) */
            for (int j = 0; j < 4; ++j) P[j] = X[j] - G.cb[0] * W[j]; iv(P, lo, hi, m); if (hi < -m) out |= OC_L;
            for (int j = 0; j < 4; ++j) P[j] = G.cb[1] * W[j] - X[j]; iv(P, lo, hi, m); if (hi < -m) out |= OC_R;
            for (int j = 0; j < 4; ++j) P[j] = Y[j] - G.cb[2] * W[j]; iv(P, lo, hi, m); if (hi < -m) out |= OC_B;
            for (int j = 0; j < 4; ++j) P[j] = G.cb[3] * W[j] - Y[j]; iv(P, lo, hi, m); if (hi < -m) out |= OC_T;
        }
        outAll &= out;
        if (safe) {        /* the guard band, in the tilted (screen) axes as meshClassify: |tx| <= g w, |ty| <= g w */
            float T[4];
            for (int j = 0; j < 4; ++j) T[j] = tilt[0] * X[j] + tilt[1] * Y[j];
            for (int j = 0; j < 4; ++j) P[j] = g * W[j] - T[j]; iv(P, lo, hi, m); if (!(lo >= m)) safe = false;
            for (int j = 0; j < 4; ++j) P[j] = g * W[j] + T[j]; iv(P, lo, hi, m); if (!(lo >= m)) safe = false;
            for (int j = 0; j < 4; ++j) T[j] = tilt[2] * X[j] + tilt[3] * Y[j];
            for (int j = 0; j < 4; ++j) P[j] = g * W[j] - T[j]; iv(P, lo, hi, m); if (!(lo >= m)) safe = false;
            for (int j = 0; j < 4; ++j) P[j] = g * W[j] + T[j]; iv(P, lo, hi, m); if (!(lo >= m)) safe = false;
        }
        if (!safe && !outAll) return -1;
    }
    if (outAll && nb > 0) return 0;
    return safe ? 1 : -1;
}

/* r54: minPx > 0 folds the lod_px test of boxInvisible() into the same 8 corners (was a second full box transform per chunk) */
int meshClassify(const float* const* C, const float (*mn)[3], const float (*mx)[3], int nb, float minPx = 0.0f)
{
    uint8_t acc = 0xFF; bool allFront = true, cpu = false;
    float rx0 = 1e30f, rx1 = -1e30f, ry0 = 1e30f, ry1 = -1e30f;
    for (int b = 0; b < nb; ++b) {
        const float* M = C[b];
        for (int c = 0; c < 8; ++c) {
            const float x = (c & 1) ? mx[b][0] : mn[b][0], y = (c & 2) ? mx[b][1] : mn[b][1], z = (c & 4) ? mx[b][2] : mn[b][2];
            const float cx = M[0] * x + M[1] * y + M[2] * z + M[3], cy = M[4] * x + M[5] * y + M[6] * z + M[7];
            const float oz = M[8] * x + M[9] * y + M[10] * z + M[11], ow = M[12] * x + M[13] * y + M[14] * z + M[15];
            const uint8_t oc = outcode(cx, cy, oz, ow);
            if (oc & OC_BAD) return 2;
            acc &= oc;
            if (oc & OC_FAR) cpu = true;
            if (ow > 1e-6f) {
                const float tx = tilt[0] * cx + tilt[1] * cy, ty = tilt[2] * cx + tilt[3] * cy;
                if (oz + ow + 1e-6f * fabsf(ow) < 0.0f || tx > guardV() * ow || tx < -guardV() * ow || ty > guardV() * ow || ty < -guardV() * ow) cpu = true;
                const float inv = 1.0f / ow, px = (cx * inv + 1.0f) * G.hvw, py = (1.0f - cy * inv) * G.hvh;
                if (px < rx0) rx0 = px; if (px > rx1) rx1 = px; if (py < ry0) ry0 = py; if (py > ry1) ry1 = py;
            } else { allFront = false; cpu = true; }
        }
    }
    if (acc) return 0;
    if (allFront && minPx > 0.0f && rx1 - rx0 < minPx && ry1 - ry0 < minPx) return 0;   /* lod_px (lossy), exactly as boxInvisible */
    if (allFront && (floorf(rx1 + 0.02f - 0.5f) < ceilf(rx0 - 0.02f - 0.5f) || floorf(ry1 + 0.02f - 0.5f) < ceilf(ry0 - 0.02f - 0.5f))) return 0;
    return cpu ? 2 : 1;
}

N3dsMeshUniforms sMU;
bool meshUniformsSane(const N3dsMeshUniforms& u)
{
    for (int k = 0; k < 4; ++k) if (!sane(tilt[k])) return false;
    for (int i = 0; i < 16; ++i) if (!sane(u.proj[i])) return false;
    for (int sl = 0; sl < u.nSlots && sl < N3DS_MESH_SLOTS; ++sl) {   /* r54: clip[] is not checked (debug only now); F and P are what is drawn */
        for (int i = 0; i < 12; ++i) if (!sane(u.pos[sl][i])) return false;
        if (u.lit || u.head) for (int i = 0; i < 12; ++i) if (!sane(u.nrm[sl][i])) return false;
    }
    if (u.lit) {
        for (int i = 0; i < 4; ++i) if (!sane(u.amb[i])) return false;
        for (int k = 0; k < u.nLights; ++k) for (int i = 0; i < 4; ++i)
            if (!sane(u.lp[k][i]) || !sane(u.lc[k][i]) || !sane(u.lf[k][i]) || !sane(u.la[k][i]) || !sane(u.lk[k][i]) || !sane(u.ld[k][i])) return false;
    }
    for (int r = 0; r < 8; ++r) for (int i = 0; i < 4; ++i) if (!sane(u.tcu[r][i])) return false;
    for (int i = 0; i < 4; ++i) if (!sane(u.cw[i]) || !sane(u.mc[i])) return false;
    return true;
}
/* r53: why a cached list is NOT drawn by the GPU transform path (it then takes the cached CPU draw). Logged with the [mesh] line. */
enum GpuNo { GN_TEXGEN, GN_UNITS, GN_TMIDX, GN_LIGHTS, GN_AMBVTX, GN_SHARED, GN_UNIF, GN_NOTREADY, GN_SKINNED_CLIP, GN_COUNT };
static const char* const kGpuNoName[GN_COUNT] = { "texture coordinates from position/normal (environment map)", "more than 2 texture units",
    "per-vertex texture matrices", "more than 2 lights", "ambient from the vertex colour", "a light shared by both channels with different functions",
    "non-finite matrix / light", "mesh not built / not GPU-ready", "skinned model crossing the near plane / guard band" };
static unsigned sGpuNo[GN_COUNT], sGpuNoTot[GN_COUNT];
/* r54 [lite5]: where the GPU transform path's CPU time goes. r53's A/B capture (log 24): ~70 us of worker time per GPU-path call, even for a
   9-vertex model, which ate most of what moving the vertex work to the PICA saved. Measured on every 4th frame only (gfx_lite = 1). */
struct GpuProf { unsigned long long tot, pre, state, cpu, queue, refused, mat, lookup, unif, mtx, cls, lod, flush; unsigned calls, nRefused, cpuChunks, cpuTris, frames; };
static GpuProf sGP;
static bool sGPOn = false;
static unsigned sWholeGpuTot = 0;          /* r54: whole models sent to the GPU without per-piece tests, since start (host test) */
static unsigned sDlFrameCalls = 0;
extern "C" { volatile unsigned gN3dsDlCallsFrame = 0; }   /* r54: display-list calls in the last finished frame (A/B test: in gameplay or not) */
extern "C" { volatile int gN3dsGpuProf = 0; }   /* read by n3ds_gpu.cpp meshIssue: time the citro3d side on the same frames */

/* r53: the GameCube lighting of the current material as shader uniforms (passthrough.v.pica). False = the shader cannot express it exactly
   (the list then takes the CPU path, which runs gxLightVertex). Mirrors gxLightVertex: rgb from channel 0 (COLOR0), alpha from channel 1
   (ALPHA0); a channel's lights add LC (its colour in that channel only) x attenuation x diffuse; a channel without lighting has AMB 1, LC 0. */
static bool meshLighting(const Mesh& m, N3dsMeshUniforms& u, int* why)
{
    u.lit = 0; u.head = 0; u.nLights = 0;
    if (!sLightMode) { u.head = (G.lit && m.hasN) ? 1 : 0; return true; }      /* nectar.ini lighting = 0: the old headlight */
    const Gfx::ChanCtl& c0 = G.ch[0]; const Gfx::ChanCtl& c1 = G.ch[1];
    if (!c0.en && !c1.en) return true;                                           /* unlit: material colour only */
    if ((c0.en && c0.ambVtx && m.hasC) || (c1.en && c1.ambVtx && m.hasC)) { *why = GN_AMBVTX; return false; }
    const unsigned mask = (c0.en ? c0.mask : 0u) | (c1.en ? c1.mask : 0u);
    int n = 0;
    for (int i = 0; i < 8; ++i) {
        if (!(mask & (1u << i))) continue;
        if (n == 2) { *why = GN_LIGHTS; return false; }
        const bool in0 = c0.en && (c0.mask & (1u << i)), in1 = c1.en && (c1.mask & (1u << i));
        if (in0 && in1 && (c0.diff != c1.diff || c0.attn != c1.attn)) { *why = GN_SHARED; return false; }
        const Gfx::ChanCtl& c = in0 ? c0 : c1;
        const Gfx::Light& L = G.light[i];
        float* lp = u.lp[n]; lp[0] = L.pos[0]; lp[1] = L.pos[1]; lp[2] = L.pos[2]; lp[3] = 0.0f;
        float* ld = u.ld[n]; ld[0] = L.dir[0]; ld[1] = L.dir[1]; ld[2] = L.dir[2]; ld[3] = 0.0f;
        float* la = u.la[n]; la[0] = L.a[0]; la[1] = L.a[1]; la[2] = L.a[2]; la[3] = 0.0f;
        float* lk = u.lk[n]; lk[0] = L.k[0]; lk[1] = L.k[1]; lk[2] = L.k[2]; lk[3] = 0.0f;
        float* lc = u.lc[n]; lc[0] = in0 ? L.col[0] : 0.0f; lc[1] = in0 ? L.col[1] : 0.0f; lc[2] = in0 ? L.col[2] : 0.0f; lc[3] = in1 ? L.col[3] : 0.0f;
        float* lf = u.lf[n];   /* diffuse = max(N.L, x) * y + z: none (0 0 1), sign (-2 1 0: N.L is in [-1, 1]), clamp (0 1 0) */
        if (c.diff == 0) { lf[0] = 0.0f; lf[1] = 0.0f; lf[2] = 1.0f; }
        else if (c.diff == 1) { lf[0] = -2.0f; lf[1] = 1.0f; lf[2] = 0.0f; }
        else { lf[0] = 0.0f; lf[1] = 1.0f; lf[2] = 0.0f; }
        lf[3] = 0.0f;
        u.spot[n] = c.attn == 1; u.spec[n] = c.attn == 0;
        ++n;
    }
    u.nLights = n;
    u.amb[0] = c0.en ? G.amb[0] : 1.0f; u.amb[1] = c0.en ? G.amb[1] : 1.0f; u.amb[2] = c0.en ? G.amb[2] : 1.0f; u.amb[3] = c1.en ? G.amb[3] : 1.0f;
    u.lit = 1;
    return true;
}

/* Returns false (and *why) when the material's lighting cannot be expressed by the shader. */
bool meshUniforms(const Mesh& m, int* why = nullptr)
{
    N3dsMeshUniforms& u = sMU;
    int dummy = 0; if (!why) why = &dummy;
    const bool wantClip = n3dsConfig().meshDebug != 0;
    u.nSlots = m.nslot; u.clipOnly = 0;
    for (int s = 0; s < m.nslot; ++s) {
        const unsigned id = (m.usesCur ? (unsigned)G.cur : (unsigned)m.slotId[s]) & 63;
        if (wantClip) {   /* r54: the shader never reads clip[] (it gets F and P); only the debug probes / checks do */
            const float* C = clipFor(id);
            float* o = u.clip[s];
            for (int c = 0; c < 4; ++c) { o[c] = tilt[0] * C[c] + tilt[1] * C[4 + c]; o[4 + c] = tilt[2] * C[c] + tilt[3] * C[4 + c]; o[8 + c] = C[8 + c]; o[12 + c] = C[12 + c]; }
        }
        /* r53: the shader computes clip = F . (P . v): P is the game's position matrix (identity when none was loaded, as clipFor) */
        if (G.posSet[id]) memcpy(u.pos[s], G.pos[id], 12 * sizeof(float));
        else { static const float I[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; memcpy(u.pos[s], I, sizeof I); }
        const float* R = G.nrmSet[id] ? G.nrm[id] : G.pos[id];
        memcpy(u.nrm[s], R, 12 * sizeof(float));
    }
    {   /* F: the projection with the LCD tilt folded into rows 0 and 1 (the same rows clipFor + the tilt above give) */
        const float* Pj = G.proj; float* f = u.proj;
        for (int c = 0; c < 4; ++c) { f[c] = tilt[0] * Pj[c] + tilt[1] * Pj[4 + c]; f[4 + c] = tilt[2] * Pj[c] + tilt[3] * Pj[4 + c]; f[8 + c] = Pj[8 + c]; f[12 + c] = Pj[12 + c]; }
    }
    const bool lightOk = meshLighting(m, u, why);
    memset(u.tcu, 0, sizeof u.tcu);
    for (int k = 0; k < G.nUnits && k < 2; ++k) {
        const Gfx::UnitGen& g = G.ug[k];
        const float* M = G.tm[g.mtx & 127];
        const int sel = g.src == 5 ? 1 : 0;
        float* ru = u.tcu[4 * k + sel]; float* rv = u.tcu[4 * k + 2 + sel];
        ru[0] = M[0] * g.su; ru[1] = M[1] * g.su; ru[2] = 0.0f; ru[3] = (M[2] + M[3]) * g.su;
#if N3DS_UV_FLIPV
        rv[0] = -M[4] * g.sv; rv[1] = -M[5] * g.sv; rv[2] = 0.0f; rv[3] = 1.0f - (M[6] + M[7]) * g.sv;
#else
        rv[0] = M[4] * g.sv; rv[1] = M[5] * g.sv; rv[2] = 0.0f; rv[3] = (M[6] + M[7]) * g.sv;
#endif
    }
    const float t0 = G.tintOn ? G.tint[0] : 1.0f, t1 = G.tintOn ? G.tint[1] : 1.0f, t2 = G.tintOn ? G.tint[2] : 1.0f;
    const bool vr = G.matVtxRGB && m.hasC, va = G.matVtxA && m.hasC;
    const float k = 1.0f / 255.0f;
    u.cw[0] = vr ? t0 * k : 0.0f; u.cw[1] = vr ? t1 * k : 0.0f; u.cw[2] = vr ? t2 * k : 0.0f; u.cw[3] = va ? k : 0.0f;
    u.mc[0] = vr ? 0.0f : G.matRGB[0] * k * t0; u.mc[1] = vr ? 0.0f : G.matRGB[1] * k * t1; u.mc[2] = vr ? 0.0f : G.matRGB[2] * k * t2; u.mc[3] = va ? 0.0f : G.matA * k;
    return lightOk;
}

/* ---- invalidation mailbox (1p fix) -------------------------------------------------------------------------------------------
   DCFlushRange / DCStoreRange are called from ANY thread: the game thread, the loader thread that draws the loading screen while the
   main thread loads (System::startLoading -> loadFunc), and the audio code. The first 1p build pushed them into the GX command queue,
   which has exactly ONE producer: two threads pushing at once corrupted it and the game hung ("stuck at frame 233" = the first stage
   load). Now any thread posts the range into this lock-free multi-producer mailbox (a ticket + a sequence stamp per slot, no locks, so
   no priority inversion between threads on one core), and the render worker drains it before it looks up a mesh. If the mailbox
   overflows, or the game resets its heap, everything is dropped instead (always safe). Taking an invalidation a little early is
   harmless: the CPU path reads vertex memory at replay time as well. */
enum { INV_N = 512 };
struct InvSlot { std::atomic<uint32_t> seq; std::atomic<uintptr_t> lo, hi; };   /* lo/hi atomic: a slot can be rewritten by a producer one lap ahead (overflow -> drop-all anyway) */
InvSlot sInv[INV_N];
std::atomic<uint32_t> sInvHead(0), sInvTailPub(0), sInvAllGen(0);
uint32_t sInvTail = 0, sInvAllSeen = 0;          /* consumer (render worker) only */

void invPost(uintptr_t lo, uintptr_t hi)
{
    const uint32_t t = sInvHead.fetch_add(1, std::memory_order_acq_rel);
    if (t - sInvTailPub.load(std::memory_order_acquire) >= (uint32_t)INV_N - 8) { sInvAllGen.fetch_add(1, std::memory_order_release); }
    InvSlot& sl = sInv[t % INV_N];
    sl.lo.store(lo, std::memory_order_relaxed); sl.hi.store(hi, std::memory_order_relaxed);
    sl.seq.store(t + 1, std::memory_order_release);
}
void invPostAll() { sInvAllGen.fetch_add(1, std::memory_order_release); }

void meshInvalRange(uintptr_t lo, uintptr_t hi);
void meshInvalAll();
void invDrain()
{
    const uint32_t g = sInvAllGen.load(std::memory_order_acquire);
    if (g != sInvAllSeen) {
        sInvAllSeen = g;
        sInvTail = sInvHead.load(std::memory_order_acquire);      /* everything posted so far is covered by dropping all meshes */
        sInvTailPub.store(sInvTail, std::memory_order_release);
        meshInvalAll();
        return;
    }
    const uint32_t head = sInvHead.load(std::memory_order_acquire);
    while (sInvTail != head) {
        InvSlot& sl = sInv[sInvTail % INV_N];
        if (sl.seq.load(std::memory_order_acquire) != sInvTail + 1) break;   /* its writer has not finished yet: next time */
        const uintptr_t lo = sl.lo.load(std::memory_order_relaxed), hi = sl.hi.load(std::memory_order_relaxed);
        if (sl.seq.load(std::memory_order_acquire) != sInvTail + 1) break;   /* rewritten meanwhile: the overflow already requested drop-all */
        ++sInvTail;
        meshInvalRange(lo, hi);
    }
    sInvTailPub.store(sInvTail, std::memory_order_release);
}


/* ================= CPU draw of a cached mesh (gpu_xform = 0, mesh_cpu = 1) ==========================================================
   Log d313ed4b: with the normal CPU path the render worker spends ~250 ms per frame in the Forest of Hope (57 000 display-list vertices,
   15 000 draw commands, ~4 us per vertex): every frame it re-parses the list byte by byte, fetches each attribute from four different
   vertex arrays (cache misses), hashes/compares raw vertex bytes for its vertex cache and runs a full state build per draw command.
   Model display lists are static, so the first time a list is seen it is decoded once into the cached mesh above (de-duplicated vertices
   in one contiguous block, triangle indices in the same order and winding as asmPush, bounding boxes per 8-command chunk and per group
   of 6 chunks). Every later frame:
     - one draw-state setup per list instead of one per draw command,
     - boxes of groups / chunks that cannot produce a pixel are skipped without touching their vertices (the same test dlcSegInvisible
       applies, on finer boxes),
     - the surviving triangles go through the SAME makeGeom / emitTri / makeAttr code as the normal path (so the output is bit-identical,
       including clipping, sub-pixel rejection, lighting and texture-coordinate generation), but each vertex is transformed once per
       list from the contiguous cache entry, and its attributes only when a triangle that survives culling uses it.
   Safety: mesh_cpu_check compares the fingerprint (positions, colours, texcoords of every surviving triangle, in order) of the cached
   draw with the normal path on a model's first uses and every 97th use; a difference stops that model (3 differences: every model)
   from using the cached draw. */
struct CTEnt { uint32_t epoch, vi; PVtx pv; };
CTEnt sCT[256];
uint32_t sCTEpoch = 0;
struct CpuStats { unsigned lists, groupsCulled, chunksCulled, chunksDrawn, tris, checks, bad, recoveries; } CS;
bool sCpuOff = false; unsigned sCpuBadTotal = 0, sCpuChecksOk = 0, sCpuRecTotal = 0;

static inline PVtx* ctGet(const Mesh& m, uint32_t vi, const PVtx* av1, const PVtx* av2, const uint8_t* mos, const float* const* Cs)
{
    const uint32_t s0 = vi & 255;
    CTEnt* e0 = &sCT[s0]; CTEnt* e1 = &sCT[(s0 + 1) & 255]; CTEnt* e2 = &sCT[(s0 + 2) & 255];
    if (e0->epoch == sCTEpoch && e0->vi == vi) return &e0->pv;
    if (e1->epoch == sCTEpoch && e1->vi == vi) return &e1->pv;
    if (e2->epoch == sCTEpoch && e2->vi == vi) return &e2->pv;
    CTEnt* e = e0;                                   /* miss: take a slot that does not hold one of the triangle's other two vertices */
    if (&e->pv == av1 || &e->pv == av2) { e = e1; if (&e->pv == av1 || &e->pv == av2) e = e2; }
    const N3dsMeshVtx& mv = m.vtx[vi];
    __builtin_prefetch(&mv + 2); __builtin_prefetch(reinterpret_cast<const uint8_t*>(&mv + 2) + 32);   /* vertices are numbered in order of first use: the next ones are needed soon */
    /* r34: the transform inline with the model's per-slot matrices (same arithmetic as makeGeom, without the per-vertex Vin copy and clipFor lookup) */
    {
        ++S.inVerts;
        const float* C = Cs[(int)mv.slot];
        const float x = mv.x, y = mv.y, z = mv.z;
        const float cx = C[0] * x + C[1] * y + C[2] * z + C[3];
        const float cy = C[4] * x + C[5] * y + C[6] * z + C[7];
        PVtx& pv = e->pv; N3dsVtx& o = pv.v;
        o.x = tilt[0] * cx + tilt[1] * cy;
        o.y = tilt[2] * cx + tilt[3] * cy;
        o.z = C[8] * x + C[9] * y + C[10] * z + C[11];
        o.w = C[12] * x + C[13] * y + C[14] * z + C[15];
        finishGeom(pv, cx, cy);
        pv.mtx = mos[(int)mv.slot]; pv.ce = nullptr;
    }
    e->pv.lazy = 2; e->pv.raw = reinterpret_cast<const uint8_t*>(&mv);
    e->epoch = sCTEpoch; e->vi = vi;
    return &e->pv;
}

/* Whole-model size on screen (lod_model_px, LOSSY "render distance"): the model's boxes of every matrix projected to pixels. Log 3072d785: the worker needs
   ~30 ms with ~3000 triangles in view and ~90 ms with ~11000, ~2.9 us per triangle that goes through the cache, and most of the 250 lists per frame
   are small things (Pikmin, pellets, plants, objects) far from the camera. A model smaller than the threshold in both directions is skipped as a whole. */
struct SizeHist { unsigned models[7], tris[7]; unsigned culled, culledTris; } SH;
inline bool modelExtent(const Mesh& m, const float* const* Cs, int nslot, float* ext, float* ctrX = nullptr, float* ctrY = nullptr)
{
    float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f; bool any = false;
    for (int s = 0; s < nslot && s < N3DS_MESH_SLOTS; ++s) {
        const float* bmn = m.smn[s]; const float* bmx = m.smx[s];
        if (!(bmn[0] <= bmx[0])) continue;                                   /* no vertex uses this matrix */
        const float* C = Cs[s];
        for (int c = 0; c < 8; ++c) {
            const float x = (c & 1) ? bmx[0] : bmn[0], y = (c & 2) ? bmx[1] : bmn[1], z = (c & 4) ? bmx[2] : bmn[2];
            const float cx = C[0] * x + C[1] * y + C[2] * z + C[3], cy = C[4] * x + C[5] * y + C[6] * z + C[7], ow = C[12] * x + C[13] * y + C[14] * z + C[15];
            if (!(fin(cx) && fin(cy) && fin(ow)) || !(ow > 1e-6f)) return false;   /* behind / at the camera: size unknown, never culled */
            const float inv = 1.0f / ow, px = (cx * inv + 1.0f) * G.hvw, py = (1.0f - cy * inv) * G.hvh;
            if (px < mnx) mnx = px; if (px > mxx) mxx = px; if (py < mny) mny = py; if (py > mxy) mxy = py;
            any = true;
        }
    }
    if (!any) return false;
    const float w = mxx - mnx, h = mxy - mny;
    *ext = w > h ? w : h;
    if (ctrX) *ctrX = 0.5f * (mnx + mxx);
    if (ctrY) *ctrY = 0.5f * (mny + mxy);
    return true;
}

/* Effective render-distance thresholds (1/10 px) of the cached CPU draw. nectar.ini lod_px / lod_model_px are level 0; auto_lod (n3ds_autolod.h) multiplies
   them while the worker cannot make the fps_cap target. Read and written only by the thread that draws (the render worker, or the game thread when
   gfx_worker = 0). If the ini has the thresholds off (0) the controller still has something to scale: it starts from 80 / 30 at level 1. */
static N3dsAutoLod sAuto; static bool sAutoInit = false; static int sAutoGen = -1;
static void lodInit()
{
    const N3dsConfig& cf = n3dsConfig();
    sAuto.configure(cf.fpsCap > 0 ? cf.autoLod : 0, cf.fpsCap > 0 ? 1000.0 / cf.fpsCap : 1000.0 / 30);   /* no cap = no target to miss */
    if (cf.autoLod <= 0 || cf.fpsCap <= 0) sAuto.level = 0;
    sAutoInit = true; sAutoGen = gN3dsCfgGen;
}
/* base is the ini value in 1/10 px; the result is in px. Level 0 = exactly the ini. Above it auto_lod scales the value, but never past capPx (a cap the ini value itself can exceed):
   a bigger render distance made Olimar's body parts vanish from the default zoomed-out camera (each display list is judged on its own size, and his parts are 10-16 px there).
   An ini value of 0 (off) is replaced by offDefaultPx at level > 0 so there is something to scale. */
static inline float lodEff(int base10, float offDefaultPx, float capPx)
{
    float b = base10 > 0 ? base10 * 0.1f : 0.0f;
    if (sAuto.level > 0) {
        const float ini = b;
        if (b <= 0.0f) b = offDefaultPx;
        b *= N3dsAutoLod::mult(sAuto.level);
        const float cap = capPx > ini ? capPx : ini;
        if (b > cap) b = cap;
    }
    return b;
}
static inline float lodModelNow() { return lodEff(n3dsConfig().lodModelPx, 4.0f, 6.0f); }   /* whole-list size cull: auto_lod never goes above 6 px */
static inline float lodFarNow()   { const N3dsConfig& c = n3dsConfig(); const float near = lodModelNow(); if (c.lodFarPx <= 0) return near; float f = c.lodFarPx * 0.1f * (sAuto.level > 0 ? N3dsAutoLod::mult(sAuto.level) : 1.0f); if (f > 24.0f && c.lodFarPx * 0.1f <= 24.0f) f = 24.0f; return f > near ? f : near; }   /* outside the centre of the screen */
/* r9: the camera keeps the captain near the middle of the screen. Pieces whose centre is inside this ellipse (where the captain and the Pikmin around him are) use the
   gentle lod_model_px; everything else on screen (far crowds, plants, pebbles toward the edges and the horizon) uses lod_far_px. */
static inline bool lodNearCentre(float cx, float cy)
{
    const float nx = (cx - G.hvw) / (G.hvw > 1.0f ? G.hvw : 1.0f), ny = (cy - G.hvh * 1.1f) / (G.hvh > 1.0f ? G.hvh : 1.0f);
    return nx * nx * (1.0f / (0.45f * 0.45f)) + ny * ny * (1.0f / (0.6f * 0.6f)) < 1.0f;
}
static inline float lodChunkNow() { return lodEff(n3dsConfig().lodPx, 2.0f, 4.0f); }        /* piece size cull inside a list: never above 4 px */

/* r10 (log 9ab84c6d: one Pikmin body list, drawn 42 times a frame, was 22.9 of the worker's ~60 ms of display lists): a simplified copy of a cached model for when it
   is small on screen. Vertex clustering per matrix slot: each slot's box is cut into a 5x5x5 grid, every vertex is replaced by the first vertex of its cell (same slot,
   so skinned joints never tear), triangles that collapse are dropped. Built once, the first time the model is drawn small; per chunk, so materials stay as they were.
   nectar.ini lod_simplify_px: models smaller than this on screen (px) use the copy; 0 = off. */
enum { LOD_GRID = 5, LOD_MIN_TRIS = 24, LOD_MAX_IDX = 6000, LOD_BUDGET = 512 * 1024 };
static bool meshLodBuild(Mesh& m)
{
    m.lodState = 2;
    if (!m.idx || !m.vtx || !m.ch || m.ni < LOD_MIN_TRIS * 3 || m.ni > LOD_MAX_IDX || m.nv > 65535) return false;
    const uint32_t need = m.ni * 2u + m.nch * 8u;
    if (sLodBytes + need > LOD_BUDGET) return false;
    uint16_t* li = (uint16_t*)malloc(m.ni * 2u); uint32_t* lr = (uint32_t*)malloc(m.nch * 8u);
    uint16_t* rep = (uint16_t*)malloc(m.nv * 2u);
    struct Cell { uint32_t key; uint16_t v; };
    const uint32_t hsz = 1024; Cell* tab = (Cell*)malloc(hsz * sizeof(Cell));
    if (!li || !lr || !rep || !tab) { free(li); free(lr); free(rep); free(tab); return false; }
    for (uint32_t i = 0; i < hsz; ++i) tab[i].key = 0xFFFFFFFFu;
    const int grid = m.nslot > 1 ? 3 : LOD_GRID;   /* skinned: each matrix holds only a few vertices, so a coarser grid per matrix */
    for (uint32_t v = 0; v < m.nv; ++v) {
        const N3dsMeshVtx& mv = m.vtx[v];
        const int s = (int)mv.slot; const int si = s < 0 ? 0 : s >= N3DS_MESH_SLOTS ? N3DS_MESH_SLOTS - 1 : s;
        int q[3];
        for (int k = 0; k < 3; ++k) {
            const float lo = m.smn[si][k], hi = m.smx[si][k], c = (&mv.x)[k];
            const float span = hi - lo;
            int g = span > 1e-6f ? (int)((c - lo) / span * grid) : 0;
            q[k] = g < 0 ? 0 : g >= grid ? grid - 1 : g;
        }
        const uint32_t key = (uint32_t)si * 1000u + (uint32_t)(q[0] * 100 + q[1] * 10 + q[2]);
        uint32_t h = (key * 2654435761u) >> 22;   /* 10 bits */
        while (tab[h].key != 0xFFFFFFFFu && tab[h].key != key) h = (h + 1) & (hsz - 1);
        if (tab[h].key == 0xFFFFFFFFu) { tab[h].key = key; tab[h].v = (uint16_t)v; }
        rep[v] = tab[h].v;
    }
    uint32_t n = 0;
    for (uint32_t c = 0; c < m.nch; ++c) {
        const MChunk& ch = m.ch[c];
        lr[c * 2] = n;
        for (uint32_t t = ch.i0; t + 2 < ch.i1 + 0u && t + 2 < m.ni; t += 3) {
            const uint16_t a = rep[m.idx[t]], b = rep[m.idx[t + 1]], d = rep[m.idx[t + 2]];
            if (a == b || b == d || a == d) continue;
            li[n++] = a; li[n++] = b; li[n++] = d;
        }
        lr[c * 2 + 1] = n;
    }
    free(rep); free(tab);
    if (n * 10u > m.ni * 9u) { free(li); free(lr); return false; }      /* saves under 10 %: not worth it */
    m.lidx = li; m.lrng = lr; m.lni = n; m.lodState = 1; sLodBytes += m.ni * 2u + m.nch * 8u;
    return true;
}
static unsigned sLodSavedTris = 0;
/* r13: the game marks the Pikmin draw (GameCoreSection::draw around pikiMgr->refresh): no size cull and no simplified copies inside it. Worker-side state. */
static int sLodExempt = 0;
void rp_lod_exempt(int on) { sLodExempt = (on && n3dsConfig().pikiLodExempt) ? 1 : 0; }
/* r18: batched vertex transform for the cached CPU draw ([lite3], log 73988e22: the triangle loop is 3.2-3.5 us per triangle, ~one vertex transform each,
   and the ARM11's VFP needs ~13 cycles per DEPENDENT multiply-add (boot [bench] line) while it can start an independent one every cycle). The classic path
   transforms each vertex on first use through the 3-way vertex cache, one at a time, between branches. Here every vertex a drawn chunk needs is transformed
   up front, four at a time, with the exact same arithmetic as makeGeom (bit-identical results, host-tested), into a per-model scratch table; the triangle
   loop then just indexes it. Models with more than XF_MAX vertices keep the classic path. nectar.ini fast_xform / the in-game menu switch it. */
enum { XF_MAX = 4096 };
static PVtx* sXf = nullptr; static uint32_t* sXfStamp = nullptr; static uint32_t sXfEpoch = 0; static bool sXfTried = false;
static bool xfEnsure()
{
    if (sXf) return true;
    if (sXfTried) return false;
    sXfTried = true;
    sXf = (PVtx*)n3ds_linear_alloc((unsigned)(sizeof(PVtx) * XF_MAX));
    sXfStamp = (uint32_t*)n3ds_linear_alloc((unsigned)(sizeof(uint32_t) * XF_MAX));
    if (!sXf || !sXfStamp) { sXf = nullptr; n3ds_log("[meshcpu] no linear memory for the batched transform table: classic path"); return false; }
    memset(sXfStamp, 0, sizeof(uint32_t) * XF_MAX);
    return true;
}
static inline void xfOne(const Mesh& m, uint32_t vi, const float* const* Cs, const uint8_t* mos)
{
    const N3dsMeshVtx& mv = m.vtx[vi];
    const float* C = Cs[(int)mv.slot];
    PVtx& pv = sXf[vi];
    const float x = mv.x, y = mv.y, z = mv.z;
    const float cx = C[0] * x + C[1] * y + C[2] * z + C[3];
    const float cy = C[4] * x + C[5] * y + C[6] * z + C[7];
    N3dsVtx& o = pv.v;
    o.x = tilt[0] * cx + tilt[1] * cy;
    o.y = tilt[2] * cx + tilt[3] * cy;
    o.z = C[8] * x + C[9] * y + C[10] * z + C[11];
    o.w = C[12] * x + C[13] * y + C[14] * z + C[15];
    finishGeom(pv, cx, cy);
    pv.lazy = 2; pv.mtx = mos[(int)mv.slot]; pv.raw = reinterpret_cast<const uint8_t*>(&mv); pv.ce = nullptr;
    sXfStamp[vi] = sXfEpoch;
}
/* four vertices with their position maths interleaved (independent chains for the VFP pipeline), then the per-vertex tail */
static inline void xfFour(const Mesh& m, uint32_t v0, const float* const* Cs, const uint8_t* mos)
{
    const N3dsMeshVtx* mv[4] = { &m.vtx[v0], &m.vtx[v0 + 1], &m.vtx[v0 + 2], &m.vtx[v0 + 3] };
    const float* C[4] = { Cs[(int)mv[0]->slot], Cs[(int)mv[1]->slot], Cs[(int)mv[2]->slot], Cs[(int)mv[3]->slot] };
    float cx[4], cy[4], cz[4], cw[4];
    for (int k = 0; k < 4; ++k) cx[k] = C[k][0] * mv[k]->x + C[k][1] * mv[k]->y + C[k][2] * mv[k]->z + C[k][3];
    for (int k = 0; k < 4; ++k) cy[k] = C[k][4] * mv[k]->x + C[k][5] * mv[k]->y + C[k][6] * mv[k]->z + C[k][7];
    for (int k = 0; k < 4; ++k) cz[k] = C[k][8] * mv[k]->x + C[k][9] * mv[k]->y + C[k][10] * mv[k]->z + C[k][11];
    for (int k = 0; k < 4; ++k) cw[k] = C[k][12] * mv[k]->x + C[k][13] * mv[k]->y + C[k][14] * mv[k]->z + C[k][15];
    for (int k = 0; k < 4; ++k) {
        PVtx& pv = sXf[v0 + k]; N3dsVtx& o = pv.v;
        o.x = tilt[0] * cx[k] + tilt[1] * cy[k];
        o.y = tilt[2] * cx[k] + tilt[3] * cy[k];
        o.z = cz[k]; o.w = cw[k];
    }
    for (int k = 0; k < 4; ++k) {
        PVtx& pv = sXf[v0 + k]; const N3dsVtx& o = pv.v;
        (void)o; finishGeom(pv, cx[k], cy[k]);
        pv.lazy = 2; pv.mtx = mos[(int)mv[k]->slot]; pv.raw = reinterpret_cast<const uint8_t*>(mv[k]); pv.ce = nullptr;
        sXfStamp[v0 + k] = sXfEpoch;
    }
}
/* r20: four ARBITRARY vertices (the gather list of a chunk), same maths as xfFour */
static inline void xfFourG(const Mesh& m, const uint32_t* vi, const float* const* Cs, const uint8_t* mos)
{
    const N3dsMeshVtx* mv[4] = { &m.vtx[vi[0]], &m.vtx[vi[1]], &m.vtx[vi[2]], &m.vtx[vi[3]] };
    const float* C[4] = { Cs[(int)mv[0]->slot], Cs[(int)mv[1]->slot], Cs[(int)mv[2]->slot], Cs[(int)mv[3]->slot] };
    float cx[4], cy[4], cz[4], cw[4];
    for (int k = 0; k < 4; ++k) cx[k] = C[k][0] * mv[k]->x + C[k][1] * mv[k]->y + C[k][2] * mv[k]->z + C[k][3];
    for (int k = 0; k < 4; ++k) cy[k] = C[k][4] * mv[k]->x + C[k][5] * mv[k]->y + C[k][6] * mv[k]->z + C[k][7];
    for (int k = 0; k < 4; ++k) cz[k] = C[k][8] * mv[k]->x + C[k][9] * mv[k]->y + C[k][10] * mv[k]->z + C[k][11];
    for (int k = 0; k < 4; ++k) cw[k] = C[k][12] * mv[k]->x + C[k][13] * mv[k]->y + C[k][14] * mv[k]->z + C[k][15];
    for (int k = 0; k < 4; ++k) {
        PVtx& pv = sXf[vi[k]]; N3dsVtx& o = pv.v;
        o.x = tilt[0] * cx[k] + tilt[1] * cy[k];
        o.y = tilt[2] * cx[k] + tilt[3] * cy[k];
        o.z = cz[k]; o.w = cw[k];
    }
    for (int k = 0; k < 4; ++k) {
        PVtx& pv = sXf[vi[k]]; const N3dsVtx& o = pv.v;
        (void)o; finishGeom(pv, cx[k], cy[k]);
        pv.lazy = 2; pv.mtx = mos[(int)mv[k]->slot]; pv.raw = reinterpret_cast<const uint8_t*>(mv[k]); pv.ce = nullptr;
    }
}
/* r20 (log c5c4d917: r18's xfRange transformed EVERY vertex between the lowest and highest index of a chunk - ~3x the vertices the chunk
   uses in the Pikmin field, 10500 transforms for 3400 triangles, 4.9 us/tri against 2.8 classic): only the vertices the chunk's triangles
   reference are transformed, four at a time from a gather list. */
static void xfGather(const Mesh& m, const uint16_t* ix, uint32_t t0, uint32_t t1, const float* const* Cs, const uint8_t* mos)
{
    uint32_t pend[64]; unsigned np = 0;
    for (uint32_t t = t0; t < t1; ++t) {
        const uint32_t vi = ix[t];
        if (sXfStamp[vi] == sXfEpoch) continue;
        sXfStamp[vi] = sXfEpoch;
        pend[np++] = vi;
        if (np == 64) { for (unsigned k = 0; k < 64; k += 4) xfFourG(m, pend + k, Cs, mos); S.inVerts += 64; np = 0; }
    }
    unsigned k = 0;
    for (; k + 4 <= np; k += 4) xfFourG(m, pend + k, Cs, mos);
    for (; k < np; ++k) xfOne(m, pend[k], Cs, mos);
    S.inVerts += np;
}
/* r20 phase profiler: on one frame per [lite] window the cached draw runs its chunks in four separate passes, each timed:
   transform (positions of the referenced vertices) / reject tests / attribute decode of the survivors / clip + ring write. */
bool sPhaseOn = false;
unsigned long long sPh[4] = {0, 0, 0, 0}, sPhTickCost = 0; unsigned sPhVerts = 0, sPhTris = 0, sPhKept = 0, sPhDecoded = 0, sPhChunks = 0, sPhFrames = 0;
/* transform every not-yet-done vertex of [lo, hi] */
static void xfRange(const Mesh& m, uint32_t lo, uint32_t hi, const float* const* Cs, const uint8_t* mos)
{
    uint32_t v = lo;
    while (v <= hi) {
        if (v + 3 <= hi && sXfStamp[v] != sXfEpoch && sXfStamp[v + 1] != sXfEpoch && sXfStamp[v + 2] != sXfEpoch && sXfStamp[v + 3] != sXfEpoch) { xfFour(m, v, Cs, mos); S.inVerts += 4; v += 4; }
        else { if (sXfStamp[v] != sXfEpoch) { xfOne(m, v, Cs, mos); ++S.inVerts; } ++v; }
    }
}
void cpuDrawBody(const Mesh& m)
{
    struct BodyTimer { unsigned long long t0, tl = 0; bool on; unsigned v0;
        BodyTimer() : on(n3dsConfig().gfxLite != 0) { t0 = on ? n3ds_ticks() : 0; v0 = S.inVerts; }
        ~BodyTimer() { if (!on) return; const unsigned long long t = n3ds_ticks(); ++sTmBodyN; sTmBodyVerts += S.inVerts - v0;
                       if (tl) { sTmSetup += tl - t0; sTmLoop += t - tl; } else sTmSetup += t - t0; } } bt;
    if (!sAutoInit || sAutoGen != gN3dsCfgGen) lodInit();
    computeCullBounds();
    uint8_t mos[N3DS_MESH_SLOTS]; const float* Cs[N3DS_MESH_SLOTS];
    for (int s = 0; s < m.nslot && s < N3DS_MESH_SLOTS; ++s) { mos[s] = m.usesCur ? (uint8_t)G.cur : m.slotId[s]; Cs[s] = clipFor(mos[s] & 63u); }
    const bool single = m.nslot <= 1;
    if (!single && meshClassify(Cs, m.smn, m.smx, m.nslot) == 0) return;       /* skinned: one decision for the whole list */
    const float* C0 = Cs[0];
    if (single && boxInvisible(m.smn[0], m.smx[0], C0)) return;                 /* the whole model is off screen / sub-pixel */
    bool useLod = false;
    if (!sLodExempt) {
        const float lodM = lodModelNow();
        if (lodM > 0.0f || n3dsConfig().gfxLite || n3dsConfig().lodSimplifyPx > 0) {
            float ext = 0.0f;
            const unsigned tr = m.ni / 3;
            float ecx = 0.0f, ecy = 0.0f;
            if (modelExtent(m, Cs, m.nslot, &ext, &ecx, &ecy)) {
                const int b = ext < 4.0f ? 0 : ext < 8.0f ? 1 : ext < 16.0f ? 2 : ext < 32.0f ? 3 : ext < 64.0f ? 4 : 5;
                ++SH.models[b]; SH.tris[b] += tr;
                const_cast<Mesh&>(m).dbgExt = ext;
                const float lim = (lodM > 0.0f && !lodNearCentre(ecx, ecy)) ? lodFarNow() : lodM;
                if (lim > 0.0f && ext < lim) { ++SH.culled; SH.culledTris += tr; return; }
                const int simp = n3dsConfig().lodSimplifyPx;
                if (simp > 0 && ext < (float)simp) {
                    Mesh& mm = const_cast<Mesh&>(m);
                    if (mm.lodState == 0) meshLodBuild(mm);
                    useLod = mm.lodState == 1;
                }
            } else { ++SH.models[6]; SH.tris[6] += tr; }
        }
    }
    const float lodPx = sLodExempt ? 0.0f : lodChunkNow();
    { Mesh& md = const_cast<Mesh&>(m); md.dbgTris = 0; md.dbgChunks = 0; if (useLod) ++md.dbgLod; }
    ++sCTEpoch; sCmHasN = m.hasN; sCmHasC = m.hasC;
    ++CS.lists;
    if (bt.on) bt.tl = n3ds_ticks();
    const bool fitsXf = m.nv <= XF_MAX && xfEnsure();
    const bool phase = sPhaseOn && fitsXf;
    const bool useXf = (phase || n3dsConfig().fastXform) && fitsXf;
    if (useXf && ++sXfEpoch == 0) { memset(sXfStamp, 0, sizeof(uint32_t) * XF_MAX); sXfEpoch = 1; }
    bool begun = false;
    const uint16_t* const idx = m.idx;
    for (uint32_t g = 0; g < m.ngrp; ++g) {
        const MGroup& gr = m.grp[g];
        if (single && boxInvisible(gr.mn, gr.mx, C0, lodPx)) { CS.groupsCulled += gr.c1 - gr.c0; continue; }
        for (uint32_t c = gr.c0; c < gr.c1; ++c) {
            const MChunk& ch = m.ch[c];
            if (ch.i1 <= ch.i0) continue;
            if (single && boxInvisible(ch.mn, ch.mx, C0, lodPx)) { ++CS.chunksCulled; continue; }
            if (!begun) { primBegin(P_TRIANGLES); if (!G.active) return; begun = true; }
            ++CS.chunksDrawn;
            uint32_t t0 = ch.i0, t1 = ch.i1; const uint16_t* ix = idx;
            if (useLod) { t0 = m.lrng[c * 2]; t1 = m.lrng[c * 2 + 1]; ix = m.lidx; sLodSavedTris += ((ch.i1 - ch.i0) - (t1 - t0)) / 3; }
            CS.tris += (t1 - t0) / 3; sTmBodyTris += (t1 - t0) / 3;
            { Mesh& md = const_cast<Mesh&>(m); md.dbgTris += (t1 - t0) / 3; ++md.dbgChunks; }
            if (phase && t1 - t0 <= 3 * 256) {
                const unsigned long long p0 = n3ds_ticks();
                const unsigned v0 = S.inVerts;
                xfGather(m, ix, t0, t1, Cs, mos);
                const unsigned long long p1 = n3ds_ticks();
                bool keep[256]; unsigned nk = 0;
                for (uint32_t t = t0, k = 0; t < t1; t += 3, ++k) { keep[k] = !triReject(sXf[ix[t]], sXf[ix[t + 1]], sXf[ix[t + 2]]); nk += keep[k]; }
                const unsigned long long p2 = n3ds_ticks();
                unsigned dec = 0;
                for (uint32_t t = t0, k = 0; t < t1; t += 3, ++k) if (keep[k]) for (int j = 0; j < 3; ++j) { PVtx& v = sXf[ix[t + j]]; if (v.lazy) { resolveLazy(v); ++dec; } }
                const unsigned long long p3 = n3ds_ticks();
                for (uint32_t t = t0, k = 0; t < t1; t += 3, ++k) { if (!G.active) break; if (keep[k]) emitKept(sXf[ix[t]], sXf[ix[t + 1]], sXf[ix[t + 2]]); }
                const unsigned long long p4 = n3ds_ticks();
                sPh[0] += p1 - p0; sPh[1] += p2 - p1; sPh[2] += p3 - p2; sPh[3] += p4 - p3;
                sPhVerts += S.inVerts - v0; sPhTris += (t1 - t0) / 3; sPhKept += nk; sPhDecoded += dec; ++sPhChunks;
                continue;
            }
            if (useXf) {
                if (t1 > t0) xfGather(m, ix, t0, t1, Cs, mos);
                for (uint32_t t = t0; t < t1; t += 3) {
                    if (!G.active) break;
                    emitTri(sXf[ix[t]], sXf[ix[t + 1]], sXf[ix[t + 2]]);
                }
                continue;
            }
            for (uint32_t t = t0; t < t1; t += 3) {
                const uint16_t* idx = ix;
                __builtin_prefetch(&idx[t + 60]);
                PVtx* a = ctGet(m, idx[t], nullptr, nullptr, mos, Cs);
                PVtx* b = ctGet(m, idx[t + 1], a, nullptr, mos, Cs);
                PVtx* d = ctGet(m, idx[t + 2], a, b, mos, Cs);
                emitTri(*a, *b, *d);
            }
        }
    }
}

/* r53: GXSetCullMode for a GPU-path draw, as the PICA's C3D_CullFace value: 0 none, 1 GPU_CULL_FRONT_CCW, 2 GPU_CULL_BACK_CCW, 3 = cull
   everything (draw nothing). Same decision as primBegin makes for the CPU path: GX front faces are clockwise on screen; the CPU drops a
   triangle when d = det(x, y, w) of its (tilted) clip-space vertices, times faceSign, is > 0 (GX_CULL_BACK) or <= 0 (GX_CULL_FRONT).
   d > 0 is counter-clockwise in the PICA's y-up clip space, so "drop d > 0" = GPU_CULL_FRONT_CCW (the mapping sms-3ds uses on hardware).
   nectar.ini gpu_cull: 1 = this mapping (default), 2 = the opposite winding (if a console disagrees: insides of models show), 0 = off. */
static int meshPicaCull()
{
    int cfg = n3dsConfig().cullFaces; const int dbg = gN3dsEnvDbg;
    if (dbg == 1) cfg = 0; else if (dbg == 2) cfg = cfg == 2 ? 1 : 2;
    const int mode = cfg ? (G.cull & 3) : 0;
    if (!mode) return 0;
    if (mode == 3) return 3;
    const int gc = n3dsConfig().gpuCull;
    if (gc == 0) return 0;
    const float td = tilt[0] * tilt[3] - tilt[1] * tilt[2];
    bool pos = (td < 0) == (cfg == 2);           /* faceSign > 0 */
    if (gc == 2) pos = !pos;
    if (mode == 2) return pos ? 1 : 2;          /* GX_CULL_BACK: drop d * faceSign > 0 */
    return pos ? 2 : 1;                         /* GX_CULL_FRONT: drop d * faceSign <= 0 */
}

/* Returns true when the call was fully handled here (drawn, partly by the CPU path, or invisible). */
bool meshTryDraw(const uint8_t* list, uint32_t n)
{
    struct GpTimer {
        bool on; bool drawn = false; unsigned long long t0, tPre = 0, tState = 0;
        GpTimer() : on(sGPOn) { t0 = on ? n3ds_ticks() : 0; }
        void pre() { if (on) { tPre = tState = n3ds_ticks(); drawn = true; } }
        void state() { if (on) tState = n3ds_ticks(); }
        ~GpTimer() {
            if (!on) return;
            const unsigned long long t = n3ds_ticks();
            if (!drawn) { sGP.refused += t - t0; ++sGP.nRefused; return; }
            sGP.tot += t - t0; sGP.pre += tPre - t0; sGP.state += tState - tPre; ++sGP.calls;
        }
    } gt;
    invDrain();
    if (!n3dsConfig().gpuXform || n3dsConfig().meshNoDraw || n < 8 || !n3ds_gpu_mesh_ready()) return false;
    if (!tiltOk) tiltOk = n3ds_gpu_tilt(tilt) != 0;
    if (!tiltOk) return false;
    if (G.matDirty) { if (gt.on) { const unsigned long long a = n3ds_ticks(); resolveMaterial(); sGP.mat += n3ds_ticks() - a; } else resolveMaterial(); }
    /* r53: what the shader cannot do goes to the cached CPU draw (meshCpuTry, next in rp_call_display_list), counted by reason */
    if (G.nUnits > 2) { ++sGpuNo[GN_UNITS]; return false; }
    for (int k = 0; k < G.nUnits; ++k) if (G.ug[k].src < 2) { ++sGpuNo[GN_TEXGEN]; return false; }   /* position / normal texgen (environment maps) */
    for (int a = A_TEX0MTXIDX; a < A_POS; ++a) if (G.desc[a]) { ++sGpuNo[GN_TMIDX]; return false; }
    const unsigned long long tL = gt.on ? n3ds_ticks() : 0;
    Mesh* m = meshLookup(list, n);
    if (gt.on) sGP.lookup += n3ds_ticks() - tL;
    if (!m || m->state != 1 || !m->vtx || m->cpuBad || m->nslot < 1 || m->nslot > N3DS_MESH_SLOTS) { ++sGpuNo[GN_NOTREADY]; return false; }
    if (n3dsConfig().meshDebug == 8) return false;          /* bisect: models are looked up / built, nothing is ever drawn by the mesh path */
    /* Never hand the GPU a non-finite or absurd matrix/uniform: the CPU path culls such triangles one by one, the PICA would rasterize
       garbage (the magenta stripe with mesh_debug = 1; possibly far outside the framebuffer). Such a draw takes the CPU path instead. */
    const unsigned long long tU = gt.on ? n3ds_ticks() : 0;
    { int why = GN_NOTREADY; if (!meshUniforms(*m, &why)) { ++sGpuNo[why]; return false; } }
    if (!meshUniformsSane(sMU)) { ++MS.badUnif; ++sGpuNo[GN_UNIF]; return false; }
    if (gt.on) sGP.unif += n3ds_ticks() - tU;
    if (n3dsConfig().meshDefer) {
        /* mesh_defer: GPU models are issued after (or later than) the ring draws made since, so only opaque, depth-tested, depth-writing models can be
           deferred without changing the picture; blended / depth-less ones take the CPU path in their place */
        const N3dsDrawState s0 = currentState();
        if (s0.blendMode != 0 || (s0.writeMask & 5) != 5 || s0.depthCmp == 7 || s0.depthCmp == 0) { ++MS.nonOpaque; return false; }
    }
    if (n3dsConfig().meshDebug == 9) return false;          /* bisect: as 8, plus uniforms computed; still nothing drawn */
    if (n3dsConfig().meshDebug == 13) {
        /* Self-check of the chunk replay on the console: the surviving triangles of the whole list (normal CPU path) are counted and
           fingerprinted without drawing them, then the same for the list replayed chunk by chunk. A difference = replay is not equivalent. */
        static int nChecked = 0, nBad = 0, nOk = 0;
        if (m->nslot == 1 && G.sc[2] > G.sc[0] && G.sc[3] > G.sc[1] && (m->uses < 3 || m->uses % 97 == 0) && nBad < 30) {
            if (G.matDirty) resolveMaterial();
            computeCullBounds();
            sChkOn = true;
            sChkT = 0; sChkH = 2166136261u; doDisplayList(list, n, 0, false);
            const unsigned t1 = sChkT; const uint32_t h1 = sChkH;
            G.active = false;
            sChkT = 0; sChkH = 2166136261u;
            static int nDataBad = 0; static unsigned nDataVerts = 0, nDataMiss = 0, nDataChunks = 0;
            const float* cm = sMU.clip[0];
            for (uint32_t c = 0; c < m->nch; ++c) {
                sCapN = 0; sCapOn = true;
                doDisplayList(list + m->ch[c].start, m->ch[c].end - m->ch[c].start, 0, true);
                sCapOn = false;
                /* the cached vertices of this chunk, projected on the CPU with the SAME clip rows the GPU would get */
                const uint32_t i0 = m->ch[c].i0, i1 = m->ch[c].i1;
                unsigned miss = 0, firstMiss = 0xFFFFFFFFu; float bestD = 0.0f;
                for (unsigned j = 0; j < sCapN; ++j) {
                    const float ew = sCap[j][3]; if (!(ew > 1e-6f)) continue;
                    const float ex = sCap[j][0] / ew, ey = sCap[j][1] / ew, ez = sCap[j][2] / ew;
                    float best = 1e30f; bool hit = false;
                    for (uint32_t k = i0; k < i1 && !hit; ++k) {
                        const N3dsMeshVtx& v = m->vtx[k];
                        const float w = cm[12]*v.x + cm[13]*v.y + cm[14]*v.z + cm[15];
                        const float X = (cm[0]*v.x + cm[1]*v.y + cm[2]*v.z + cm[3]) / w, Y = (cm[4]*v.x + cm[5]*v.y + cm[6]*v.z + cm[7]) / w, Z = (cm[8]*v.x + cm[9]*v.y + cm[10]*v.z + cm[11]) / w;
                        const float d = fabsf(X - ex) + fabsf(Y - ey) + fabsf(Z - ez);
                        if (d < best) best = d;
                        if (d <= 0.004f + 0.002f * (fabsf(ex) + fabsf(ey))) hit = true;
                    }
                    if (!hit) { if (!miss) { firstMiss = j; bestD = best; } ++miss; }
                }
                nDataVerts += sCapN; nDataMiss += miss; ++nDataChunks;
                if (miss && nDataBad < 25) {
                    ++nDataBad;
                    n3ds_log("[mesh] DATA MISMATCH #%d: list %p chunk %u/%u (cmds @%u..%u, verts %u..%u) | CPU path emitted %u vertices, %u are not among the cached chunk's | first: ndc (%.4f %.4f %.5f) w %.4g, nearest cached distance %.4f",
                             nDataBad, (const void*)list, c, m->nch, m->ch[c].start, m->ch[c].end, i0, i1, sCapN, miss,
                             sCap[firstMiss][0] / sCap[firstMiss][3], sCap[firstMiss][1] / sCap[firstMiss][3], sCap[firstMiss][2] / sCap[firstMiss][3], sCap[firstMiss][3], bestD);
                }
            }
            if ((nChecked % 25) == 0) n3ds_log("[mesh] data check so far: %u chunks, %u CPU-path vertices, %u not in the cache (%.2f%%)", nDataChunks, nDataVerts, nDataMiss, nDataVerts ? 100.0 * nDataMiss / nDataVerts : 0.0);
            const unsigned t2 = sChkT; const uint32_t h2 = sChkH;
            sChkOn = false; G.active = false;
            ++nChecked;
            if (t1 != t2 || h1 != h2) {
                ++nBad;
                n3ds_log("[mesh] REPLAY MISMATCH #%d: list %p %u bytes, %u chunks, %u verts, cur %d | whole list: %u tris hash %08x | chunk replay: %u tris hash %08x", nBad, (const void*)list, n, m->nch, m->nv, (int)G.cur, t1, h1, t2, h2);
            } else if (nOk < 6) { ++nOk; n3ds_log("[mesh] replay ok: list %p %u bytes, %u chunks: %u tris hash %08x", (const void*)list, n, m->nch, t1, h1); }
            if ((nChecked % 50) == 0) n3ds_log("[mesh] replay self-check so far: %d lists checked, %d mismatched", nChecked, nBad);
        }
        return false;
    }
    if (n3dsConfig().meshDebug == 6 || n3dsConfig().meshDebug == 7) {
        /* Single-model test (mesh_debug = 6): only ONE cached model (the first single-matrix one with >= 100 vertices - the same kind of model the
           mesh_debug = 5 fit probe drew cleanly) takes the real GPU path, with the game's real matrices and real state; every other list is drawn
           by the CPU path. If this model shows up right in the scene, the GPU path and the matrices are fine for it; if it is garbage, the real
           matrices / real draw state are what breaks (the vertex data is the one already proven good). */
        static const uint8_t* oneList = nullptr; static uint32_t oneN = 0;
        if (!oneList && m && m->state == 1 && m->nslot == 1 && m->nv >= 100 && m->vtx) {
            oneList = list; oneN = n;
            const float* c = sMU.clip[0];
            n3ds_log("[mesh] single-model test: list %p, %u verts | bbox x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f | clip rows [%.4g %.4g %.4g %.4g] [%.4g %.4g %.4g %.4g] [%.4g %.4g %.4g %.4g] [%.4g %.4g %.4g %.4g]",
                     (const void*)list, m->nv, m->smn[0][0], m->smx[0][0], m->smn[0][1], m->smx[0][1], m->smn[0][2], m->smx[0][2],
                     c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
        }
        if (list != oneList || n != oneN) return false;
    }
    if (n3dsConfig().meshDebug == 4 && G.sc[2] > G.sc[0] && G.sc[3] > G.sc[1]) {
        /* Probe (mesh_debug = 4): a hand-made 9-vertex mesh in the exact mesh vertex format, identity-like matrix (tilt only), flat magenta,
           drawn through the real GPU mesh path on top of the normal CPU picture. Clean right-hand square + left-hand triangle = vertex fetch,
           shader and uniform upload work, so the fault is in the cached model data. Garbage = fetch / shader / upload is broken. */
        static N3dsMeshVtx* pv = nullptr; static bool tried = false;
        if (!tried) {
            tried = true;
            pv = (N3dsMeshVtx*)n3ds_linear_alloc(9 * sizeof(N3dsMeshVtx));
            if (pv) {
                static const float P[9][2] = {{0.1f,-0.8f},{0.9f,-0.8f},{0.9f,0.8f},{0.1f,-0.8f},{0.9f,0.8f},{0.1f,0.8f},{-0.9f,-0.8f},{-0.1f,-0.8f},{-0.5f,0.8f}};
                memset(pv, 0, 9 * sizeof(N3dsMeshVtx));
                for (int i = 0; i < 9; ++i) { pv[i].x = P[i][0]; pv[i].y = P[i][1]; pv[i].z = 0.0f; pv[i].slot = 0.0f; pv[i].r = pv[i].g = pv[i].b = pv[i].a = 255; pv[i].nz = 1.0f; }
                n3ds_flush_dcache(pv, 9 * sizeof(N3dsMeshVtx));
                n3ds_log("[mesh] probe built: 9 vertices, stride %u", (unsigned)sizeof(N3dsMeshVtx));
            }
        }
        if (pv) {
            N3dsDrawState st = currentState();
            st.depthCmp = 7; st.writeMask = 1; st.blendMode = 0; st.alphaCmp = 7; st.cull = 0;
            st.texId = 0; st.tex1Id = 0; st.tex2Id = 0; st.nTev = 1;
            memset(&st.tev[0], 0, sizeof(st.tev[0]));
            for (int k = 0; k < 3; ++k) { st.tev[0].rgbSrc[k] = N3DS_TS_CONST; st.tev[0].rgbOp[k] = N3DS_TO_C; st.tev[0].aSrc[k] = N3DS_TS_CONST; st.tev[0].aOp[k] = N3DS_TO_A; }
            st.tev[0].rgbFn = N3DS_TFN_REPLACE; st.tev[0].aFn = N3DS_TFN_REPLACE; st.tev[0].rgbScale = 1; st.tev[0].aScale = 1;
            st.tev[0].color = 0xFFFF00FFu;
            N3dsMeshUniforms u; memset(&u, 0, sizeof u);
            u.nSlots = 1;
            u.clip[0][0] = tilt[0]; u.clip[0][1] = tilt[1]; u.clip[0][4] = tilt[2]; u.clip[0][5] = tilt[3];
            u.clip[0][11] = -0.5f; u.clip[0][15] = 1.0f; u.clipOnly = 1;
            u.mc[0] = u.mc[1] = u.mc[2] = u.mc[3] = 1.0f;
            static unsigned pr[2] = {0, 9};
            flushOut();
            n3ds_gpu_draw_mesh(&st, pv, 9, nullptr, pr, 1, &u);
        }
        return false;
    }
    if (n3dsConfig().meshDebug == 5 && G.sc[2] > G.sc[0] && G.sc[3] > G.sc[1]) {
        /* Fit probe (mesh_debug = 5): the first reasonably big single-matrix cached model is drawn through the GPU mesh path with a matrix that
           fits its own bounding box (x/y, side view) into the middle of the screen, flat magenta, on top of the normal CPU picture. The model
           silhouette = cached DATA and fetch are right (then the game's matrices/uniforms are the problem); spikes/garbage = the data is. */
        static const uint8_t* fitList = nullptr; static uint32_t fitN = 0;
        if (!fitList && m && m->state == 1 && m->nslot == 1 && m->nv >= 100 && m->vtx) {
            fitList = list; fitN = n;
            n3ds_log("[mesh] fit probe: list %p, %u verts, bbox x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f; first vertices (%.2f %.2f %.2f %.0f) (%.2f %.2f %.2f %.0f) (%.2f %.2f %.2f %.0f)",
                     (const void*)list, m->nv, m->smn[0][0], m->smx[0][0], m->smn[0][1], m->smx[0][1], m->smn[0][2], m->smx[0][2],
                     m->vtx[0].x, m->vtx[0].y, m->vtx[0].z, m->vtx[0].slot, m->vtx[1].x, m->vtx[1].y, m->vtx[1].z, m->vtx[1].slot,
                     m->vtx[2].x, m->vtx[2].y, m->vtx[2].z, m->vtx[2].slot);
        }
        Mesh* f = fitList ? meshLookup(fitList, fitN) : nullptr;
        if (f && f->state == 1 && f->nslot == 1 && f->vtx) {
            N3dsDrawState st = currentState();
            st.depthCmp = 7; st.writeMask = 1; st.blendMode = 0; st.alphaCmp = 7; st.cull = 0;
            st.texId = 0; st.tex1Id = 0; st.tex2Id = 0; st.nTev = 1;
            memset(&st.tev[0], 0, sizeof(st.tev[0]));
            for (int k = 0; k < 3; ++k) { st.tev[0].rgbSrc[k] = N3DS_TS_CONST; st.tev[0].rgbOp[k] = N3DS_TO_C; st.tev[0].aSrc[k] = N3DS_TS_CONST; st.tev[0].aOp[k] = N3DS_TO_A; }
            st.tev[0].rgbFn = N3DS_TFN_REPLACE; st.tev[0].aFn = N3DS_TFN_REPLACE; st.tev[0].rgbScale = 1; st.tev[0].aScale = 1;
            st.tev[0].color = 0xFFFF00FFu;
            const float cx = 0.5f * (f->smn[0][0] + f->smx[0][0]), cy = 0.5f * (f->smn[0][1] + f->smx[0][1]);
            float ext = f->smx[0][0] - f->smn[0][0]; const float ey = f->smx[0][1] - f->smn[0][1]; if (ey > ext) ext = ey;
            if (ext > 1e-6f) {
                const float sc = 1.6f / ext;
                N3dsMeshUniforms u; memset(&u, 0, sizeof u);
                u.nSlots = 1;
                u.clip[0][0] = tilt[0] * sc; u.clip[0][1] = tilt[1] * sc; u.clip[0][3] = -sc * (tilt[0] * cx + tilt[1] * cy);
                u.clip[0][4] = tilt[2] * sc; u.clip[0][5] = tilt[3] * sc; u.clip[0][7] = -sc * (tilt[2] * cx + tilt[3] * cy);
                u.clip[0][11] = -0.5f; u.clip[0][15] = 1.0f; u.clipOnly = 1;
                u.mc[0] = u.mc[1] = u.mc[2] = u.mc[3] = 1.0f;
                static unsigned fr[2]; fr[0] = 0; fr[1] = f->ni;
                flushOut();
                n3ds_gpu_draw_mesh(&st, f->vtx, f->nv, f->idx, fr, 1, &u); if (sTag) ++sTagGpu;
            }
        }
        return false;
    }
    if (n3dsConfig().meshDebug == 1) {
        /* Diagnostic (nectar.ini mesh_debug = 1): the whole mesh through the GPU path as flat magenta, no depth test, no depth write,
           no culling; then "not handled" so the CPU path draws the same list normally ON TOP. Where the GPU path puts the geometry where
           the CPU path does, the magenta is completely covered; any magenta left on screen is GPU-path geometry in the wrong place. */
        N3dsDrawState st = currentState();
        st.depthCmp = 7; st.writeMask = 1; st.blendMode = 0; st.alphaCmp = 7; st.cull = 0;
        st.texId = 0; st.tex1Id = 0; st.tex2Id = 0; st.nTev = 1;
        memset(&st.tev[0], 0, sizeof(st.tev[0]));
        for (int k = 0; k < 3; ++k) { st.tev[0].rgbSrc[k] = N3DS_TS_CONST; st.tev[0].rgbOp[k] = N3DS_TO_C; st.tev[0].aSrc[k] = N3DS_TS_CONST; st.tev[0].aOp[k] = N3DS_TO_A; }
        st.tev[0].rgbFn = N3DS_TFN_REPLACE; st.tev[0].aFn = N3DS_TFN_REPLACE; st.tev[0].rgbScale = 1; st.tev[0].aScale = 1;
        st.tev[0].color = 0xFFFF00FFu;                       /* 0xAABBGGRR: magenta */
        if (G.sc[2] > G.sc[0] && G.sc[3] > G.sc[1]) {
            static unsigned dbgRuns[2];
            dbgRuns[0] = 0; dbgRuns[1] = m->ni;
            flushOut();
            meshUniforms(*m);
            if (m->nslot == 1 && m->nv > 0) {
                static int nDump = 0;
                if (nDump < 8 && m->nv >= 30) {
                    ++nDump;
                    const float* c = sMU.clip[0]; const N3dsMeshVtx& v0 = m->vtx[0];
                    const float cx = c[0] * v0.x + c[1] * v0.y + c[2] * v0.z + c[3], cy = c[4] * v0.x + c[5] * v0.y + c[6] * v0.z + c[7];
                    const float cz = c[8] * v0.x + c[9] * v0.y + c[10] * v0.z + c[11], cw = c[12] * v0.x + c[13] * v0.y + c[14] * v0.z + c[15];
                    n3ds_log("[mesh] dbg draw %d: id %u cur %d verts %u | clip rows (tilt applied) [%.4g %.4g %.4g %.4g] [%.4g %.4g %.4g %.4g] [%.4g %.4g %.4g %.4g] [%.4g %.4g %.4g %.4g] | v0 (%.3f %.3f %.3f) -> clip (%.4g %.4g %.4g %.4g) = ndc (%.3f %.3f %.3f)",
                             nDump, (unsigned)m->slotId[0], (int)m->usesCur, m->nv, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15],
                             v0.x, v0.y, v0.z, cx, cy, cz, cw, cx / cw, cy / cw, cz / cw);
                }
            }
            n3ds_gpu_draw_mesh(&st, m->vtx, m->nv, m->idx, dbgRuns, 1, &sMU); if (sTag) ++sTagGpu;
            ++MS.calls; ++MS.gpuChunks;
        }
        return false;
    }
    ++MS.calls;
    gt.pre();
    if (sProfile) censusClose();
    G.active = false;
    if (G.sc[2] <= G.sc[0] || G.sc[3] <= G.sc[1]) { ++S.emptyScissor; return true; }
    if (!sAutoInit || sAutoGen != gN3dsCfgGen) lodInit();
    computeCullBounds();
    N3dsDrawState st = currentState();
    {   /* r53: GXSetCullMode on the GPU path = the PICA's culling (the CPU path tests every triangle in triReject). Same decision as primBegin. */
        const int pc = meshPicaCull();
        if (pc == 3) return true;                                /* GX_CULL_ALL: nothing is drawn */
        st.cull = (uint8_t)pc;
    }
    gt.state();
    if (n3dsConfig().meshDebug == 7) {       /* single model, REAL runs and matrices, but flat magenta: no textures / combiner / blending / depth test */
        st.depthCmp = 7; st.writeMask = 1; st.blendMode = 0; st.alphaCmp = 7; st.cull = 0;
        st.texId = 0; st.tex1Id = 0; st.tex2Id = 0; st.nTev = 1;
        memset(&st.tev[0], 0, sizeof(st.tev[0]));
        for (int k = 0; k < 3; ++k) { st.tev[0].rgbSrc[k] = N3DS_TS_CONST; st.tev[0].rgbOp[k] = N3DS_TO_C; st.tev[0].aSrc[k] = N3DS_TS_CONST; st.tev[0].aOp[k] = N3DS_TO_A; }
        st.tev[0].rgbFn = N3DS_TFN_REPLACE; st.tev[0].aFn = N3DS_TFN_REPLACE; st.tev[0].rgbScale = 1; st.tev[0].aScale = 1;
        st.tev[0].color = 0xFFFF00FFu;
    }
    static unsigned runs[2 * 512]; int nr = 0;
    auto submit = [&]() {
        if (!nr) return;
        if (n3dsConfig().meshDebug == 10) { nr = 0; return; }       /* bisect: classification + CPU replay as normal, GPU parts simply not drawn */
        if (gt.on) { const unsigned long long f0 = n3ds_ticks(); flushOut(); sGP.flush += n3ds_ticks() - f0; }   /* r54: sMU was filled once at the top of meshTryDraw */
        else flushOut();
        if (n3dsConfig().meshDebug >= 6) {
            static int nLogR = 0;
            if (nLogR < 12) {
                ++nLogR;
                n3ds_log("[mesh] single-model draw %d: %d run(s), mesh %u verts, cull %d depth %d blend %d tev %d units %d%d%d, lit %d:", nLogR, nr, m->nv, (int)st.cull, (int)st.depthCmp, (int)st.blendMode, (int)st.nTev,
                         (int)st.texId, (int)st.tex1Id, (int)st.tex2Id, (int)sMU.lit);
                for (int i = 0; i < nr && i < 8; ++i) {
                    const unsigned f0 = runs[2 * i], cn = runs[2 * i + 1];
                    const N3dsMeshVtx& a = m->vtx[f0]; const N3dsMeshVtx& b = m->vtx[f0 + cn - 1];
                    const float* c = sMU.clip[0];
                    auto proj = [&](const N3dsMeshVtx& v, float* o) {
                        const float w = c[12]*v.x + c[13]*v.y + c[14]*v.z + c[15];
                        o[0] = (c[0]*v.x + c[1]*v.y + c[2]*v.z + c[3]) / w; o[1] = (c[4]*v.x + c[5]*v.y + c[6]*v.z + c[7]) / w;
                        o[2] = (c[8]*v.x + c[9]*v.y + c[10]*v.z + c[11]) / w; o[3] = w;
                    };
                    float pa[4], pb[4]; proj(a, pa); proj(b, pb);
                    n3ds_log("[mesh]    run %d: first %u count %u | first vtx (%.2f %.2f %.2f) ndc (%.3f %.3f %.5f) w %.4g | last vtx ndc (%.3f %.3f %.5f) w %.4g",
                             i, f0, cn, a.x, a.y, a.z, pa[0], pa[1], pa[2], pa[3], pb[0], pb[1], pb[2], pb[3]);
                }
            }
        }
        if (n3dsConfig().meshDebug >= 6) {
            /* invariant check on what the GPU is about to be given: every vertex of every run, projected on the CPU with the SAME uniforms */
            static int nViol = 0; static unsigned nChk = 0;
            const float* c = sMU.clip[0];
            float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f, mnz = 1e30f, mxz = -1e30f, mnw = 1e30f, mxw = -1e30f; unsigned bad = 0, firstBad = 0;
            for (int i = 0; i < nr; ++i)
                for (unsigned k = runs[2 * i]; k < runs[2 * i] + runs[2 * i + 1]; ++k) {
                    const N3dsMeshVtx& v = m->vtx[k];
                    const float w = c[12]*v.x + c[13]*v.y + c[14]*v.z + c[15];
                    const float X = (c[0]*v.x + c[1]*v.y + c[2]*v.z + c[3]) / w, Y = (c[4]*v.x + c[5]*v.y + c[6]*v.z + c[7]) / w, Z = (c[8]*v.x + c[9]*v.y + c[10]*v.z + c[11]) / w;
                    if (X < mnx) mnx = X; if (X > mxx) mxx = X; if (Y < mny) mny = Y; if (Y > mxy) mxy = Y;
                    if (Z < mnz) mnz = Z; if (Z > mxz) mxz = Z; if (w < mnw) mnw = w; if (w > mxw) mxw = w;
                    if (!(w > 1e-4f) || !(fabsf(X) <= guardV() + 0.01f) || !(fabsf(Y) <= guardV() + 0.01f) || !(Z <= 0.0005f) || !(Z >= -1.0005f)) { if (!bad) firstBad = k; ++bad; }
                }
            ++nChk;
            if ((bad && nViol < 40) || (nChk % 20) == 1) {
                if (bad) ++nViol;
                n3ds_log("[mesh] check #%u: %d run(s) | ndc x %.3f..%.3f y %.3f..%.3f z %.5f..%.5f w %.4g..%.4g | %u vertices OUTSIDE guard/near/far%s", nChk, nr, mnx, mxx, mny, mxy, mnz, mxz, mnw, mxw, bad, bad ? " <<<" : "");
                if (bad) { const N3dsMeshVtx& v = m->vtx[firstBad]; n3ds_log("[mesh]    first bad vertex %u: obj (%.2f %.2f %.2f)", firstBad, v.x, v.y, v.z); }
            }
        }
        if (gt.on) { const unsigned long long q0 = n3ds_ticks(); n3ds_gpu_draw_mesh(&st, m->vtx, m->nv, m->idx, runs, nr, &sMU); sGP.queue += n3ds_ticks() - q0; }
        else n3ds_gpu_draw_mesh(&st, m->vtx, m->nv, m->idx, runs, nr, &sMU);
        nr = 0;
    };
    auto addRun = [&](uint32_t i0, uint32_t i1) {
        if (i1 <= i0) return;
        MS.tris += (i1 - i0) / 3;
        if (nr && runs[2 * nr - 2] + runs[2 * nr - 1] == i0) { runs[2 * nr - 1] += i1 - i0; return; }
        if (nr == 512) submit();
        runs[2 * nr] = i0; runs[2 * nr + 1] = i1 - i0; ++nr;
    };
    unsigned long long tk = gt.on ? n3ds_ticks() : 0;   /* r56 [lite5]: the culling bucket split into matrices / box tests / lod / ring flush */
    auto lap = [&](unsigned long long& acc) { if (gt.on) { const unsigned long long t = n3ds_ticks(); acc += t - tk; tk = t; } };
    uint8_t mos[N3DS_MESH_SLOTS]; const float* Cs[N3DS_MESH_SLOTS];
    for (int s = 0; s < m->nslot; ++s) { mos[s] = m->usesCur ? (uint8_t)G.cur : m->slotId[s]; Cs[s] = clipFor(mos[s] & 63u); }
    lap(sGP.mtx);
    const bool single = m->nslot <= 1;
    if (!single) {                                            /* skinned: one decision for the whole list, over every matrix's box */
        int cls = meshClassifyFast(Cs, m->smn, m->smx, m->nslot);     /* r54b: interval test first, the 8-corner test only when undecided */
        if (cls < 0) cls = meshClassify(Cs, m->smn, m->smx, m->nslot);
        if (cls == 0) { ++MS.invisChunks; lap(sGP.cls); return true; }
        if (cls == 2) { ++MS.wholeCpu; ++sGpuNo[GN_SKINNED_CLIP]; lap(sGP.cls); return false; }   /* the cached CPU draw clips it */
    }
    int wholeFast = single ? meshClassifyFast(Cs, m->smn, m->smx, 1) : -1;   /* r54b */
    if (single) {
        if (wholeFast == 0) { ++MS.invisChunks; lap(sGP.cls); return true; }
        if (wholeFast < 0 && boxInvisible(m->smn[0], m->smx[0], Cs[0])) { ++MS.invisChunks; lap(sGP.cls); return true; }
    }
    lap(sGP.cls);
    /* r53: the same draw-distance rules as the cached CPU draw (cpuDrawBody): a whole model smaller than lod_model_px / lod_far_px is not drawn,
       a piece smaller than lod_px is skipped (Pikmin exempt). The simplified far-model index lists are CPU-only (malloc'd, not GPU-visible). */
    if (!sLodExempt) {
        const float lodM = lodModelNow();
        if (lodM > 0.0f) {
            float ext = 0.0f, ecx = 0.0f, ecy = 0.0f;
            if (modelExtent(*m, Cs, m->nslot, &ext, &ecx, &ecy)) {
                const float lim = !lodNearCentre(ecx, ecy) ? lodFarNow() : lodM;
                if (lim > 0.0f && ext < lim) { ++SH.culled; SH.culledTris += m->ni / 3; lap(sGP.lod); return true; }
            }
        }
    }
    lap(sGP.lod);
    if (!single) { addRun(0, m->ni); ++MS.gpuChunks; submit(); return true; }
    const float lodPx = sLodExempt ? 0.0f : lodChunkNow();
    bool cpuBegun = false;
    auto cpuChunkBody = [&](const MChunk& ch) {
        if (!m->cpu || !m->idx) { doDisplayList(list + ch.start, ch.end - ch.start, 0, true); return; }
        if (!cpuBegun) { ++sCTEpoch; sCmHasN = m->hasN; sCmHasC = m->hasC; primBegin(P_TRIANGLES); cpuBegun = true; }
        else if (!G.active) primBegin(P_TRIANGLES);
        if (!G.active) return;
        for (uint32_t t = ch.i0; t < ch.i1; t += 3) {
            PVtx* a = ctGet(*m, m->idx[t], nullptr, nullptr, mos, Cs);
            PVtx* b = ctGet(*m, m->idx[t + 1], a, nullptr, mos, Cs);
            PVtx* d = ctGet(*m, m->idx[t + 2], a, b, mos, Cs);
            emitTri(*a, *b, *d);
            if (!G.active) break;
        }
        CS.tris += (ch.i1 - ch.i0) / 3;
    };
    auto cpuChunk = [&](const MChunk& ch) {                  /* a piece the PICA cannot clip: drawn by the cached CPU draw, in draw order */
        submit();
        if (!gt.on) { cpuChunkBody(ch); return; }
        const unsigned long long c0 = n3ds_ticks();
        cpuChunkBody(ch);
        sGP.cpu += n3ds_ticks() - c0; ++sGP.cpuChunks; sGP.cpuTris += (ch.i1 - ch.i0) / 3;
    };
    auto oneChunk = [&](const MChunk& ch, uint32_t c) {
        if (ch.i1 <= ch.i0) return;
        int cls = n3dsConfig().meshDebug == 12 ? 2 : meshClassifyFast(Cs, &ch.mn, &ch.mx, 1);   /* 12: bisect, every chunk drawn by the CPU */
        if (cls < 0) cls = meshClassify(Cs, &ch.mn, &ch.mx, 1, lodPx);
        (void)c;
        if (cls == 0) { ++MS.invisChunks; return; }
        if (cls == 1) { ++MS.gpuChunks; addRun(ch.i0, ch.i1); return; }
        ++MS.cpuChunks;
        if (!n3dsConfig().meshReplay && !m->cpu) return;      /* GPU-format mesh without chunk replay: handled below (whole list to the CPU) */
        cpuChunk(ch);
    };
    if (!m->cpu && !n3dsConfig().meshReplay) {
        /* a GPU-format mesh (mesh_cpu = 0) with chunk replay off: a list with ANY chunk that needs the CPU path is drawn by the normal path */
        for (uint32_t c = 0; c < m->nch; ++c) if (meshClassify(Cs, &m->ch[c].mn, &m->ch[c].mx, 1) == 2) { ++MS.wholeCpu; return false; }
    }
    /* r54: a box the PICA can draw without any clipping (in front of the camera, inside the guard band, not beyond the far plane) holds only such
       chunks: they all go to the GPU as one run, without a test each. What the per-chunk tests would have skipped draws no pixel on the GPU
       either (wholly outside the view: clipped; between pixel centres: not rasterised), except lod_px's lossy skips, which only ever saved CPU
       time. r53 tested every chunk of every model (2 box transforms each): ~70 us of worker time per GPU-path call on the console (log 24). */
    const bool dbgAllCpu = n3dsConfig().meshDebug == 12;
    auto allGpu = [&](uint32_t c0, uint32_t c1) { for (uint32_t c = c0; c < c1; ++c) { const MChunk& ch = m->ch[c]; if (ch.i1 > ch.i0) { ++MS.gpuChunks; addRun(ch.i0, ch.i1); } } };
    if (!dbgAllCpu) {
        const int cls = wholeFast >= 0 ? wholeFast : meshClassify(Cs, m->smn, m->smx, 1);
        if (cls == 0) { ++MS.invisChunks; return true; }
        if (cls == 1) { ++MS.wholeGpu; ++sWholeGpuTot; allGpu(0, m->nch); submit(); return true; }
    }
    if (m->grp && m->ngrp) {                                  /* CPU-format mesh: hierarchical boxes over MC_GROUP chunks */
        for (uint32_t g = 0; g < m->ngrp; ++g) {
            const MGroup& gr = m->grp[g];
            int gc = meshClassifyFast(Cs, &gr.mn, &gr.mx, 1);
            if (gc < 0) gc = meshClassify(Cs, &gr.mn, &gr.mx, 1, lodPx);
            if (gc == 0) { MS.invisChunks += gr.c1 - gr.c0; continue; }
            if (gc == 1 && !dbgAllCpu) { allGpu(gr.c0, gr.c1); continue; }
            for (uint32_t c = gr.c0; c < gr.c1; ++c) oneChunk(m->ch[c], c);
        }
    } else {
        for (uint32_t c = 0; c < m->nch; ++c) oneChunk(m->ch[c], c);
    }
    submit();
    return true;
}

/* true = handled (drawn or invisible); false = the normal path must draw this list. */
bool meshCpuTry(const uint8_t* list, uint32_t n)
{
    const N3dsConfig& cf = n3dsConfig();
    if (!cf.meshCpu || sCpuOff || n < 8) return false;   /* r53: also with gpu_xform = 1, for the lists the GPU path hands back */
    invDrain();
    if (!tiltOk) tiltOk = n3ds_gpu_tilt(tilt) != 0;
    if (!tiltOk) return false;
    if (G.sc[2] <= G.sc[0] || G.sc[3] <= G.sc[1]) return false;
    if (G.matDirty) resolveMaterial();
    Mesh* m;
    if (cf.gfxLite) { const unsigned long long t0 = n3ds_ticks(); m = meshLookup(list, n); sTmLookup += n3ds_ticks() - t0; ++sTmLookupN; }
    else m = meshLookup(list, n);
    if (!m || m->state != 1 || !m->cpu || m->cpuBad || !m->idx || !m->grp || m->nslot < 1) return false;
    if (cf.meshCpuCheck && cf.lodPx <= 0 && cf.lodModelPx <= 0 && cf.lodSimplifyPx <= 0 && sAuto.level == 0 && (m->checked < 1 || m->uses % 601 == 0)) {     /* (lod_px drops chunks the normal path draws: the comparison would always differ) */
        computeCullBounds();
        sChkOn = true;
        sChkT = 0; sChkH = 2166136261u; doDisplayList(list, n, 0, false);
        const unsigned t1 = sChkT; const uint32_t h1 = sChkH;
        G.active = false;
        sChkT = 0; sChkH = 2166136261u; cpuDrawBody(*m);
        const unsigned t2 = sChkT; const uint32_t h2 = sChkH;
        sChkOn = false; G.active = false;
        ++CS.checks;
        if (t1 != t2 || h1 != h2) {
            if (m->recov == 0) {   /* first difference: the cache may simply be stale (data changed without a flush) - rebuild it and check again on the next use */
                m->recov = 1; ++CS.recoveries; ++sCpuRecTotal;
                static unsigned nRec = 0;
                if (++nRec <= 12) n3ds_log("[meshcpu] cached draw differs from the normal path (list %p, %u bytes, %u verts, use %u: normal %u tris %08x, cached %u tris %08x): rebuilding the cache entry", (const void*)list, n, m->nv, (unsigned)m->uses, t1, h1, t2, h2);
                meshBuild(*m, list, n);
                return false;
            }
            ++CS.bad; ++sCpuBadTotal; m->cpuBad = 1;
            if (sCpuBadTotal <= 12)
                n3ds_log("[meshcpu] CHECK MISMATCH #%u: list %p, %u bytes, %u verts, %u chunks, %u matrices, use %u | normal path: %u tris hash %08x | cached draw: %u tris hash %08x (second time in a row) -> this model uses the normal path from now on",
                         sCpuBadTotal, (const void*)list, n, m->nv, m->nch, (unsigned)m->nslot, (unsigned)m->uses, t1, h1, t2, h2);
            if (sCpuBadTotal >= 3 && !sCpuOff) { sCpuOff = true; n3ds_log("[meshcpu] 3 differences: the cached CPU draw is switched off for every model (normal path)"); }
            return false;
        }
        ++sCpuChecksOk; if (m->checked < 250) ++m->checked; m->recov = 0;
        if (sCpuChecksOk <= 4) n3ds_log("[meshcpu] check ok #%u: list %p, %u bytes, %u verts, %u tris, %u chunks, hash %08x", sCpuChecksOk, (const void*)list, n, m->nv, t1, m->nch, h1);
    }
    cpuDrawBody(*m);
    return true;
}

/* DCFlushRange / DCStoreRange (queued, so they apply in order with the draws) and heap resets */
void meshInvalRange(uintptr_t lo, uintptr_t hi)
{
    if (!sMC || hi <= sMCLo || lo >= sMCHi) return;
    for (int i = 0; i < MC_SLOTS; ++i) {
        Mesh& m = sMC[i];
        if (m.state != 1) continue;
        for (int r = 0; r < m.nrng; ++r) if (m.rng[r].lo < hi && lo < m.rng[r].hi) { meshKill(m); ++MS.inval; break; }
    }
}
void meshInvalAll()
{
    if (sMC) for (int i = 0; i < MC_SLOTS; ++i) if (sMC[i].state) { meshKill(sMC[i]); ++MS.inval; }
    sMCLo = ~(uintptr_t)0; sMCHi = 0;
}

void meshFrameEnd(unsigned frame)
{
    invDrain();
    meshReap(frame + 1);
    MSsum.calls += MS.calls; MSsum.gpuChunks += MS.gpuChunks; MSsum.cpuChunks += MS.cpuChunks; MSsum.invisChunks += MS.invisChunks;
    MSsum.built += MS.built; MSsum.buildFail += MS.buildFail; MSsum.inval += MS.inval; MSsum.verifyFail += MS.verifyFail; MSsum.badUnif += MS.badUnif; MSsum.nonOpaque += MS.nonOpaque; MSsum.tris += MS.tris;
    MSsum.wholeCpu += MS.wholeCpu; MSsum.buildTicks += MS.buildTicks; MSsum.volatileLists += MS.volatileLists;
    MStot.built += MS.built; MStot.buildFail += MS.buildFail; MStot.volatileLists += MS.volatileLists; MStot.inval += MS.inval; MStot.verifyFail += MS.verifyFail;
    if (frame == 10 || (frame > 0 && frame % 300 == 0)) {
        unsigned live = 0; if (sMC) for (int i = 0; i < MC_SLOTS; ++i) live += sMC[i].state == 1;
        n3ds_log("[mesh] frame %u: this frame %u lists on the GPU path (%u tris; %u whole models without per-piece tests), chunks gpu/cpu/invisible %u/%u/%u, skinned lists sent to CPU %u | "
                 "since last line: built %u (%u stayed on CPU, last reason: %s) in %.1f ms, invalidated %u, verify mismatches %u, non-finite matrices sent to CPU %u, blended/depth-less models kept on CPU (mesh_defer) %u | cache %u meshes, %u KB",
                 frame, MS.calls, MS.tris, MS.wholeGpu, MS.gpuChunks, MS.cpuChunks, MS.invisChunks, MS.wholeCpu, MSsum.built, MSsum.buildFail, sMCLastReason,
                 (double)MSsum.buildTicks / N3DS_TICKS_PER_MS, MSsum.inval, MSsum.verifyFail, MSsum.badUnif, MSsum.nonOpaque, live, sMCBytes / 1024);
        memset(&MSsum, 0, sizeof MSsum);
        n3ds_log("[mesh] builds by cause since last line: first use %u, evicted by a colliding list %u, list source changed %u, periodic verify %u, retry after refusal %u, thrash timeout %u",
                 sRb[0], sRb[1], sRb[2], sRb[3], sRb[4], sRb[5]);
        memset(sRb, 0, sizeof sRb);
        if (n3dsConfig().gpuXform) {   /* r53 (AGENTS.md 5): every list the GPU transform path handed back to the CPU, by reason */
            char b[600]; int o = 0; unsigned tot = 0;
            for (int i = 0; i < GN_COUNT; ++i) { sGpuNoTot[i] += sGpuNo[i]; tot += sGpuNo[i]; }
            for (int i = 0; i < GN_COUNT && o < 540; ++i) if (sGpuNo[i]) o += snprintf(b + o, sizeof b - o, "%s%s x%u", o ? "; " : "", kGpuNoName[i], sGpuNo[i]);
            n3ds_log("[mesh] GPU transform path: %u list draws handed to the CPU since the last line%s%s", tot, tot ? ": " : "", tot ? b : "");
            memset(sGpuNo, 0, sizeof sGpuNo);
        }
        if (sNFailR) {
            char b[400]; int o = 0;
            for (int i = 0; i < sNFailR && o < 340; ++i) o += snprintf(b + o, sizeof b - o, "%s%s x%u", i ? "; " : "", sFailR[i].why, sFailR[i].n);
            n3ds_log("[mesh] lists that stayed on the normal path since start (reason x builds): %s", b);
        }
    }
    memset(&MS, 0, sizeof MS);
}

inline uint32_t packColor(GXColor c) { return (uint32_t)c.r | (uint32_t)c.g << 8 | (uint32_t)c.b << 16 | (uint32_t)c.a << 24; }
inline uint32_t clampS10(int v) { return v < 0 ? 0u : v > 255 ? 255u : (uint32_t)v; }

/* ---- profiling scopes (rp_perf_scope_begin/end: the game's own instrumentation points; timed on profiled frames only) ---- */
struct Scope { const char* name; unsigned long long ticks; unsigned calls; };
Scope sScopes[48]; int sNScopes = 0;
struct OpenScope { int idx; unsigned long long t0; };
OpenScope sOpen[24]; int sDepth = 0;

void immCommit()
{
    if (!G.haveV) return;
    G.haveV = false;
    if (G.active) { G.iv.hasC = G.immC; asmPush(makeVtx(G.iv)); }
}

} // namespace

extern "C" void n3ds_gfx_flush(void) { flushOut(); G.active = false; }   /* a frame boundary ends any primitive in flight */

/* Called by pc_window_swap_buffers after the frame's vertices were committed. Logs the counters of the frame that just
   ended when it was a profiled one (every 60th up to frame 600, then every 600th) and on frame 10. */
static void liteReport(unsigned frame);
extern "C" void rp_bottom(int on);
extern "C" int n3ds_gfx_autolod_level(void) { return sAuto.level; }
extern "C" void n3ds_gfx_frame_end(unsigned frame)
{
    sLodExempt = 0;
    if (sOnBottom) rp_bottom(0);
    {   /* r27: picture stretch, switched live between frames: re-map the viewport / scissor in force */
        const int st = n3dsConfig().stretch ? 1 : 0;
        if (st != sStretch) {
            sStretch = st;
            gxRectToScreen(sRawVp[0], sRawVp[1], sRawVp[2], sRawVp[3], G.vp);
            gxRectToScreen(sRawSc[0], sRawSc[1], sRawSc[2], sRawSc[3], G.sc);
        }
    }
    sPhaseOn = n3dsConfig().gfxLite && (frame + 1) % 60 == 30;   /* r20: the next frame is the [lite4] phase-profiled one */
    if (sPhaseOn && !sPhTickCost) { const unsigned long long a = n3ds_ticks(); for (int i = 0; i < 15; ++i) (void)n3ds_ticks(); sPhTickCost = (n3ds_ticks() - a) / 16; }
    if (sProfile) { censusClose(); censusFlushGroups(); }
    sCenIdx = 0;
    if (sProfile || frame == 10)
        n3ds_log("[gfx-perf] frame %u: prims %u | dropped: ring-full %u, ui-reserve %u, empty-scissor %u | in-verts %u -> tris out %u, "
                 "culled %u, sub-pixel %u, non-finite %u | display lists %u, vertex-cache hits %u of %u, attribute decodes (lazy) %u, clipped %u | list segments: %u tested, %u skipped (%u verts), %u lists scanned", frame, S.prims, S.ringDrops, S.uiDrops, S.emptyScissor, S.inVerts, S.tris,
                 S.culled, S.subpx, S.bad, S.dlCalls, S.vcHits, S.inVerts, S.lazyDone, S.clipped, S.segTests, S.segCulled, S.segVertsSkipped, S.dlBuilt);
    if (sProfile)
        n3ds_log("[gfx-perf] frame %u: display-list decode + assembly = %.1f ms (this frame only, includes svcGetSystemTick overhead)",
                 frame, (double)S.emitTicks / N3DS_TICKS_PER_MS);
    if (sProfile && S.dlCalls)
        n3ds_log("[gfx-perf] frame %u: display-list split (%u lists): full cold pass %.1f ms | then warm re-runs: parse-only %.1f ms, parse+transform %.1f ms  => "
                 "transform ~%.1f ms, assembly+emit+state ~%.1f ms (cold/warm cache differences make this approximate)", frame, S.dlCalls,
                 (double)S.emitTicks / N3DS_TICKS_PER_MS, (double)S.parseTicks / N3DS_TICKS_PER_MS, (double)S.xformTicks / N3DS_TICKS_PER_MS,
                 (double)(S.xformTicks - S.parseTicks) / N3DS_TICKS_PER_MS, (double)S.emitTicks / N3DS_TICKS_PER_MS - (double)S.xformTicks / N3DS_TICKS_PER_MS);
    if (sProfile) {
        /* scopes, biggest first (inclusive times; nested scopes overlap) */
        for (int pass = 0; pass < 12; ++pass) {
            int best = -1;
            for (int i = 0; i < sNScopes; ++i) if (sScopes[i].calls && (best < 0 || sScopes[i].ticks > sScopes[best].ticks)) best = i;
            if (best < 0) break;
            n3ds_log("[scope] frame %u: %-28s %7.2f ms  (%u calls)", frame, sScopes[best].name, (double)sScopes[best].ticks / N3DS_TICKS_PER_MS, sScopes[best].calls);
            sScopes[best].calls = 0;
        }
        n3dstexLogStats(frame);
    }
    for (int i = 0; i < sNScopes; ++i) { sScopes[i].ticks = 0; sScopes[i].calls = 0; }
    sDepth = 0;
    n3dstexFrame(frame);
    if (sPickOn) { sPickOn = false; n3ds_log("[pick] end of frame %u: %u triangles covered pixel (%d,%d) (first 80 listed, in drawing order)", frame, sPickN, gN3dsPickX, gN3dsPickY); }
    if (sTagAcc.n > 0) {
        if (frame % 90 == 0 && !sPickOn && !gN3dsPickReq && sTagPicks < 6) {
            ++sTagPicks;
            int cx = (int)(sTagAcc.sx / (3.0 * sTagAcc.n)), cy = (int)(sTagAcc.sy / (3.0 * sTagAcc.n));
            if (cx < 0) cx = 0; if (cx > 399) cx = 399; if (cy < 0) cy = 0; if (cy > 239) cy = 239;
            n3ds_log("[tag] frame %u: %s drew %u triangles, screen box (%.0f,%.0f)-(%.0f,%.0f), centre (%d,%d) -> picking that pixel | GPU-path draws while tagged: %u",
                     frame, tagName(sTagAcc.tag), sTagAcc.n, sTagAcc.x0, sTagAcc.y0, sTagAcc.x1, sTagAcc.y1, cx, cy, sTagGpu);
            gN3dsPickX = cx; gN3dsPickY = cy; gN3dsPickReq = 1;
        }
        sTagAcc = TagAcc{0, 0, 0, 1e9f, 1e9f, -1e9f, -1e9f, 0};
    }
    if (gN3dsPickReq) { gN3dsPickReq = 0; sPickOn = true; sPickN = 0; n3ds_log("[pick] top-screen pixel (%d,%d): every triangle that covers it in the next frame:", gN3dsPickX, gN3dsPickY); }
    sLightMode = n3dsConfig().lighting;
    meshFrameEnd(frame);
    liteReport(frame);
    { static unsigned long fb = 0, ff = 0; fb += S.faceCulled[0]; ff += S.faceCulled[1];
      if (frame > 0 && frame % 600 == 0) { n3ds_log("[cull] last 600 frames: %lu back-facing and %lu front-facing triangles dropped (cull_faces %d, test switch %d)", fb, ff, n3dsConfig().cullFaces, (int)gN3dsEnvDbg); fb = ff = 0; } }
    if (frame > 0 && frame % 600 == 0) {   /* r37: are the sphere-mapped models drawn at all? (triangles that reached the ring, per texture) */
        if (sEnvWatchN > 0) {
            char b[400]; int o = 0; b[0] = 0;
            for (int i = 0; i < sEnvWatchN && o < (int)sizeof b - 40; ++i)
                o += snprintf(b + o, sizeof b - o, " | tex %u: %u opaque, %u blended", (unsigned)sEnvWatch[i], (unsigned)sTexTris[sEnvWatch[i] & 1023][0], (unsigned)sTexTris[sEnvWatch[i] & 1023][1]);
            n3ds_log("[envtex] triangles drawn in the last 600 frames%s", b);
        }
        memset(sTexTris, 0, sizeof sTexTris);
    }
    memset(&CS, 0, sizeof CS); memset(&SH, 0, sizeof SH); sLodSavedTris = 0;
    G.frameNo = frame + 1; G.matDirty = true;
    if (sProfile)
        n3ds_log("[gfx-perf] frame %u: estimated fill %.2f Mpx (%.1f screens of 320x240; PICA fill is roughly 1 px/clock at 268 MHz, so > ~3 Mpx/frame is GPU-bound) | "
                 "tris: tiny(<2px) %u, big(>20k px) %u, crossing near plane %u of %u", frame, S.fillPx / 1e6, S.fillPx / 76800.0, S.tiny, S.big, S.nearClip, S.tris);
    memset(&S, 0, sizeof S);
    const unsigned nf = frame + 1;
    sProfile = n3dsConfig().gfxProfile && nf % 60 == 0 && (nf <= 600 || nf % 240 == 0);   /* 1n: every 240th after 600 (was 600th) so the [draw] census has more chances to catch a scene */
    sDbgSub = 0;
}

extern "C" {

/* HUD / menu virtual size. The 3DS shows one fixed 4:3 picture (640x480 GX space, pillarboxed), so these are constants. The generated stubs
   returned 0: menus and HUD then built viewports of width 0 ("vp 40,0-40,240" in the log), every primitive was rejected as sub-pixel, and the
   top screen stayed black for the whole run. */
int pc_gfx_get_hud_virtual_width(void) { return 640; }
int pc_gfx_get_hud_virtual_height(void) { return 480; }
int pc_gfx_menu_virt_width(void) { return 640; }

void pc_gfx_init(void)
{
    if (n3ds_gfxq_running()) return;           /* the worker owns G once it runs */
    n3ds_log("[gfx] n3ds_gfx.cpp built %s %s (emitTri null-outP fix present)", __DATE__, __TIME__);   /* the banner time comes from n3ds_main.cpp and is NOT refreshed when only other files change */
    memset(&G, 0, sizeof G);
    ++sLitGen;
    static const float I[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    memcpy(G.proj, I, sizeof I); G.projGen = 1;
    for (int i = 0; i < 64; ++i) G.posGen[i] = 1;
    G.zCmp = 3; G.zWrite = true; G.cWrite = G.aWrite = true;
    G.alphaCmp = 7; G.matRGB[0] = G.matRGB[1] = G.matRGB[2] = G.matA = 255; G.matVtxRGB = G.matVtxA = true;
    G.blendSrc = 1; G.blendDst = 0;
    for (int i = 1; i < 32; ++i) sInvFrac[i] = 1.0f / (float)(1u << i);
    sInvFrac[0] = 1.0f;
    for (int i = 0; i < 16; ++i) G.tev.st[i].map = 0xFF;
    G.tev.nStages = 1;                       /* GX default = PASSCLR: out = rasterised colour */
    { uint8_t c[4] = {15, 15, 15, 10}, a[4] = {7, 7, 7, 5}; memcpy(G.tev.st[0].ci, c, 4); memcpy(G.tev.st[0].ai, a, 4); }
    G.tev.st[0].cscale = G.tev.st[0].ascale = 0;
    G.tev.primaryMap = -1; G.tev.lenient = true; G.matDirty = true;
    for (int i = 0; i < 128; ++i) { G.tm[i][0] = 1; G.tm[i][1] = 0; G.tm[i][2] = 0; G.tm[i][3] = 0; G.tm[i][4] = 0; G.tm[i][5] = 1; G.tm[i][6] = 0; G.tm[i][7] = 0; }
    G.tm60Ident = true;
    for (int i = 0; i < 8; ++i) { G.tgSrc[i] = (uint8_t)(4 + i); G.tgType[i] = 1; G.tgMtx[i] = 60; }   /* GXInit: coord i = 2x4 matrix of TEXi through GX_IDENTITY */
    for (int i = 0; i < 16; ++i) G.stageCoord[i] = (uint8_t)(i & 7);
    gxRectToScreen(0, 0, 640, 480, G.vp); gxRectToScreen(0, 0, 640, 480, G.sc);
    n3ds_gfxq_start(n3dsConfig().gfxThread);   /* last: from here on G belongs to the worker */
}

/* ---- transform / viewport ---- */
void rp_set_projection(const Mtx44 m, GXProjectionType) { for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) G.proj[r * 4 + c] = m[r][c]; ++G.projGen; }
void rp_set_viewport(f32 x, f32 y, f32 w, f32 h, f32, f32) { sRawVp[0] = x; sRawVp[1] = y; sRawVp[2] = w; sRawVp[3] = h; gxRectToScreen(x, y, w, h, G.vp); }
void rp_set_scissor(u32 x, u32 y, u32 w, u32 h) { sRawSc[0] = (float)x; sRawSc[1] = (float)y; sRawSc[2] = (float)w; sRawSc[3] = (float)h; gxRectToScreen((float)x, (float)y, (float)w, (float)h, G.sc); }
/* r19: the game's bottom-screen map. Everything recorded between n3ds_gfx_bottom(1) and (0) is drawn on the bottom screen, its 640x480 GX
   viewport mapped onto 320x240. The viewport / scissor in force are re-mapped for the new screen. */
void rp_bottom(int on)
{
    on = on ? 1 : 0;
    if (on == sOnBottom) return;
    flushOut(); G.active = false;
    if (!n3ds_gpu_target(on) && on) return;          /* no bottom target: stay on the top screen (the game also checks gN3dsBottomOk) */
    sOnBottom = on;
    gxRectToScreen(sRawVp[0], sRawVp[1], sRawVp[2], sRawVp[3], G.vp);
    gxRectToScreen(sRawSc[0], sRawSc[1], sRawSc[2], sRawSc[3], G.sc);
}
void rp_load_pos_mtx(const Mtx m, u32 id) { id &= 63; for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) G.pos[id][r * 4 + c] = m[r][c]; G.posSet[id] = true; ++G.posGen[id]; }
void rp_load_nrm_mtx(const Mtx m, u32 id) { id &= 63; for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) G.nrm[id][r * 4 + c] = m[r][c]; G.nrmSet[id] = true; }
void rp_set_current_mtx(u32 id) { G.cur = id & 63; }

/* ---- pixel state ---- */
void rp_set_z_mode(GXBool en, GXCompare func, GXBool upd) { G.zCmp = en ? (int)func : 7; G.zWrite = upd != 0; }
void rp_set_blend_mode(GXBlendMode type, GXBlendFactor s, GXBlendFactor d, GXLogicOp) {
    int t = (int)type; G.blendMode = t == 1 ? 1 : t == 3 ? 2 : 0; G.blendSrc = (int)s; G.blendDst = (int)d; }
void rp_set_cull_mode(GXCullMode m) { G.cull = (int)m; }
void rp_set_color_update(GXBool b) { G.cWrite = b != 0; }
void rp_set_alpha_update(GXBool b) { G.aWrite = b != 0; }
/* GX has two alpha compares joined by AND/OR/XOR; PICA has one. A compare that can never fail (ALWAYS, <=255, >=0) is dropped, so
   e.g. GEQUAL 0x80 AND LEQUAL 255 (cut-out sprites/text) keeps its real test. Both real: take the first (same-op pairs are equal anyway). */
static bool alphaTrivial(int c, int r) { return c == 7 || (c == 3 && r >= 255) || (c == 6 && r <= 0); }
void rp_set_alpha_compare(GXCompare c0, u8 r0, GXAlphaOp, GXCompare c1, u8 r1) {
    if (alphaTrivial((int)c1, r1)) { G.alphaCmp = (int)c0; G.alphaRef = r0; }
    else if (alphaTrivial((int)c0, r0)) { G.alphaCmp = (int)c1; G.alphaRef = r1; }
    else { G.alphaCmp = (int)c0; G.alphaRef = r0; } }
void rp_set_copy_clear(GXColor c, u32) { n3ds_gpu_set_clear(c.r, c.g, c.b); }

/* ---- colour channels / TEV bookkeeping ---- */
void rp_set_chan_ctrl(GXChannelID chan, GXBool enable, GXColorSrc ambSrc, GXColorSrc matSrc, u32 lightMask, GXDiffuseFn diffFn, GXAttnFn attnFn) {
    int c = (int)chan; bool vtx = (int)matSrc == 1;
    if (c == 0 || c == 4) { G.matVtxRGB = vtx; G.lit = enable && lightMask != 0; }
    if (c == 2 || c == 4) G.matVtxA = vtx;
    for (int k = 0; k < 2; ++k) if (c == (k ? 2 : 0) || c == 4) {
        Gfx::ChanCtl& t = G.ch[k]; t.en = enable != 0; t.ambVtx = (int)ambSrc == 1; t.matVtx = vtx; t.mask = (uint8_t)lightMask; t.diff = (uint8_t)(int)diffFn; t.attn = (uint8_t)(int)attnFn; }
    ++sLitGen; }

/* r47: ambient register and light objects (worker side; names without rp_ on purpose: tools/host_test/run_mesh.sh renames rp_* to pc_gfx_*) */
void chanAmbSet(int c, GXColor col) {
    if (c == 0 || c == 4) { G.amb[0] = col.r / 255.0f; G.amb[1] = col.g / 255.0f; G.amb[2] = col.b / 255.0f; }
    if (c == 2 || c == 4) G.amb[3] = col.a / 255.0f; }
void lightLoad(const float* v, uint32_t mask) {    /* v: pos 3, dir 3, a 3, k 3, colour rgba 4 (0..1) */
    for (int i = 0; i < 8; ++i) if (mask & (1u << i)) memcpy(&G.light[i], v, sizeof(Gfx::Light));
    ++sLitGen;
    static int gen = -1, n = 0;                     /* the first lights of each area, for checking against the GameCube */
    if (gen != gN3dsMatProbeGen) { gen = gN3dsMatProbeGen; n = 0; }
    if (n < 10) { ++n; n3ds_log("[light] mask %02x: colour %.0f,%.0f,%.0f,%.0f pos %.1f %.1f %.1f dir %.2f %.2f %.2f angle %.3g %.3g %.3g dist %.3g %.3g %.3g | ambient %.0f,%.0f,%.0f,%.0f",
        (unsigned)mask, v[12] * 255, v[13] * 255, v[14] * 255, v[15] * 255, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
        G.amb[0] * 255, G.amb[1] * 255, G.amb[2] * 255, G.amb[3] * 255); } }

void rp_set_chan_mat_color(GXChannelID chan, GXColor col) {
    int c = (int)chan;
    if (c == 0 || c == 4) { G.matRGB[0] = col.r; G.matRGB[1] = col.g; G.matRGB[2] = col.b; }
    if (c == 2 || c == 4) G.matA = col.a; }
void rp_set_tev_order(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID chan) {
    const int i = (int)stage; if (i < 0 || i >= 16) return;
    G.tev.st[i].chan = (uint8_t)(int)chan;
    G.tev.st[i].map = (uint8_t)(int)map; G.stageCoord[i] = (uint8_t)(int)coord; G.matDirty = true; }
/* GXSetTexCoordGen: coordinate `coord` = (matrix id `mtx`) x (source: vertex texcoord / normal / position). Evaluated per vertex in makeAttr. */
void rp_set_tex_coord_gen(GXTexCoordID coord, GXTexGenType type, GXTexGenSrc src, u32 mtx) {
    const int c = (int)coord; if (c < 0 || c >= 8) return;
    G.tgType[c] = (uint8_t)(int)type; G.tgSrc[c] = (uint8_t)(int)src; G.tgMtx[c] = (uint8_t)(mtx & 127); G.matDirty = true; }
/* GXLoadTexMtxImm: rows 0 and 1 are all a 2x4 texgen uses (the game never loads a projective third row that matters). Read live by makeAttr. */
void rp_load_tex_mtx(const Mtx m, u32 id) {
    id &= 127; for (int r = 0; r < 2; ++r) for (int c = 0; c < 4; ++c) G.tm[id][r * 4 + c] = m[r][c];
    if (id == 60) { const float* t = G.tm[60]; G.tm60Ident = t[0] == 1 && t[1] == 0 && t[2] == 0 && t[3] == 0 && t[4] == 0 && t[5] == 1 && t[6] == 0 && t[7] == 0; } }
void rp_load_tex_obj(GXTexObj* obj, GXTexMapID id) {
    N3dsTexRef r; if (!n3dstexCapture(obj, &r)) r.valid = false;
    G.texRef[(int)id & 7] = r; G.matDirty = true; }

/* ---- TEV program (Dolphin SDK numbering, see n3ds_tev.h) ---- */
void rp_set_num_tev_stages(u8 n) { G.tev.nStages = n < 1 ? 1 : n > 16 ? 16 : n; G.matDirty = true; }
void rp_set_tev_color_in(GXTevStageID s, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    const int i = (int)s; if (i < 0 || i >= 16) return;
    G.tev.st[i].ci[0] = (uint8_t)(int)a; G.tev.st[i].ci[1] = (uint8_t)(int)b; G.tev.st[i].ci[2] = (uint8_t)(int)c; G.tev.st[i].ci[3] = (uint8_t)(int)d; G.matDirty = true; }
void rp_set_tev_alpha_in(GXTevStageID s, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    const int i = (int)s; if (i < 0 || i >= 16) return;
    G.tev.st[i].ai[0] = (uint8_t)(int)a; G.tev.st[i].ai[1] = (uint8_t)(int)b; G.tev.st[i].ai[2] = (uint8_t)(int)c; G.tev.st[i].ai[3] = (uint8_t)(int)d; G.matDirty = true; }
void rp_set_tev_color_op(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale sc, GXBool, GXTevRegID out) {
    const int i = (int)s; if (i < 0 || i >= 16) return;
    G.tev.st[i].cop = (uint8_t)(int)op; G.tev.st[i].cbias = (uint8_t)(int)bias; G.tev.st[i].cscale = (uint8_t)(int)sc; G.tev.st[i].cout = (uint8_t)(int)out; G.matDirty = true; }
void rp_set_tev_alpha_op(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale sc, GXBool, GXTevRegID out) {
    const int i = (int)s; if (i < 0 || i >= 16) return;
    G.tev.st[i].aop = (uint8_t)(int)op; G.tev.st[i].abias = (uint8_t)(int)bias; G.tev.st[i].ascale = (uint8_t)(int)sc; G.tev.st[i].aout = (uint8_t)(int)out; G.matDirty = true; }
void rp_set_tev_color(GXTevRegID r, GXColor c) { const int i = (int)r; if (i >= 0 && i < 4) { G.tev.reg[i] = packColor(c); G.matDirty = true; } }
void rp_set_tev_color_s10(GXTevRegID r, GXColorS10 c) {
    const int i = (int)r; if (i < 0 || i >= 4) return;
    G.tev.reg[i] = clampS10(c.r) | clampS10(c.g) << 8 | clampS10(c.b) << 16 | clampS10(c.a) << 24; G.matDirty = true; }
void rp_set_tev_kcolor(GXTevKColorID id, GXColor c) { const int i = (int)id; if (i >= 0 && i < 4) { G.tev.kcolor[i] = packColor(c); G.matDirty = true; } }
void rp_set_tev_kcolor_sel(GXTevStageID s, GXTevKColorSel k) { const int i = (int)s; if (i >= 0 && i < 16) { G.tev.st[i].ksel = (uint8_t)(int)k; G.matDirty = true; } }
void rp_set_tev_kalpha_sel(GXTevStageID s, GXTevKAlphaSel k) { const int i = (int)s; if (i >= 0 && i < 16) { G.tev.st[i].kasel = (uint8_t)(int)k; G.matDirty = true; } }

/* ---- the game's own perf scopes (inclusive, profiled frames only) ---- */
void rp_perf_scope_begin(const char* name) {
    if (!sProfile || sDepth >= 24) { if (sDepth < 24) sOpen[sDepth++].idx = -1; return; }
    int idx = -1;
    for (int i = 0; i < sNScopes; ++i) if (sScopes[i].name == name || (sScopes[i].name && name && strcmp(sScopes[i].name, name) == 0)) { idx = i; break; }
    if (idx < 0 && sNScopes < 48) { idx = sNScopes++; sScopes[idx].name = name; sScopes[idx].ticks = 0; sScopes[idx].calls = 0; }
    sOpen[sDepth].idx = idx; sOpen[sDepth].t0 = n3ds_ticks(); ++sDepth; }
void rp_perf_scope_end(void) {
    if (sDepth <= 0) return;
    const OpenScope o = sOpen[--sDepth];
    if (o.idx >= 0 && sProfile) { sScopes[o.idx].ticks += n3ds_ticks() - o.t0; ++sScopes[o.idx].calls; } }

/* ---- vertex setup ---- */
void rp_clear_vtx_desc(void) { memset(G.desc, 0, sizeof G.desc); }
void rp_set_vtx_desc(GXAttr a, GXAttrType t) { int i = (int)a; if (i >= 0 && i < A_COUNT) G.desc[i] = (uint8_t)(int)t; }
void rp_set_vtx_attr_fmt(GXVtxFmt f, GXAttr a, GXCompCnt cnt, GXCompType type, u8 frac) {
    int fi = (int)f & 7, i = (int)a; if (i >= 0 && i < A_COUNT) { G.fmt[fi][i].cnt = (uint8_t)(int)cnt; G.fmt[fi][i].type = (uint8_t)(int)type; G.fmt[fi][i].frac = frac; } }
void rp_set_array(GXAttr a, void* base, u8 stride) { int i = (int)a; if (i >= 0 && i < A_COUNT) { G.arrBase[i] = (const uint8_t*)base; G.arrStride[i] = stride; } }

/* ---- immediate mode ---- */
void rp_begin(GXPrimitive type, GXVtxFmt, u16) { primBegin((int)type); G.imm = true; G.haveV = false; G.immC = false; G.iv.r = G.iv.g = G.iv.b = G.iv.a = 255; }
void rp_position(f32 x, f32 y, f32 z) { if (!G.imm) return; immCommit(); G.iv.x = x; G.iv.y = y; G.iv.z = z; G.iv.mtx = (uint8_t)G.cur; G.haveV = true; }
void rp_color(u8 r, u8 g, u8 b, u8 a) { G.iv.r = r; G.iv.g = g; G.iv.b = b; G.iv.a = a; G.immC = true; }
void rp_texcoord(f32 u, f32 v) { G.iv.u = u; G.iv.v = v; }
void rp_end(void) { immCommit(); G.imm = false; }

/* ---- display lists ---- */
/* ---- gfx_lite: cheap always-on measurement of where the display-list time goes ---------------------------------------------------- */
extern "C++" {
struct LiteTop { const void* p; uint32_t n, calls; unsigned long long ticks; };
static LiteTop sLite[64]; static unsigned long long sLiteAll = 0, sLiteCachedT = 0; static unsigned sLiteCalls = 0, sLiteCachedN = 0;
static unsigned long long sLiteBusyAcc = 0; static unsigned sLiteBusyLast = 0, sLiteBusyFrames = 0;   /* worker busy time per frame, summed between two [lite] lines (64-bit: the 32-bit tick counter wraps every 16 s) */
static unsigned long long sLiteCachedTot = 0;
extern "C" { volatile float gN3dsUsPerTri = 0.0f, gN3dsDlMs = 0.0f; }   /* r18: shown live in the performance menu */   /* r16: cached display-list time over the [lite2] window */
static void liteNote(const void* list, uint32_t n, unsigned long long dt, bool cached)
{
    sLiteAll += dt; ++sLiteCalls; if (cached) { sLiteCachedT += dt; ++sLiteCachedN; sLiteCachedTot += dt; }
    unsigned h = (unsigned)(((uintptr_t)list >> 3) * 2654435761u) & 63;
    for (int k = 0; k < 8; ++k, h = (h + 1) & 63) {
        LiteTop& e = sLite[h];
        if (!e.p) { e.p = list; e.n = n; e.calls = 0; e.ticks = 0; }
        if (e.p == list && e.n == n) { ++e.calls; e.ticks += dt; return; }
    }
}
static void liteReport(unsigned frame)
{
    gN3dsDlCallsFrame = sDlFrameCalls; sDlFrameCalls = 0;
    if (sGPOn) ++sGP.frames;
    sGPOn = n3dsConfig().gfxLite && n3dsConfig().gpuXform && (frame & 3) == 3;   /* r54: [lite5] timers on the next frame (one in four) */
    gN3dsGpuProf = sGPOn ? 1 : 0;
    {   /* auto_lod: one decision input per presented frame (period since the previous one, display-list time of this one) */
        if (!sAutoInit || sAutoGen != gN3dsCfgGen) lodInit();
        static unsigned long long sLastT = 0;
        const unsigned long long now = n3ds_ticks();
        const double period = sLastT ? (double)(now - sLastT) / N3DS_TICKS_PER_MS : 0.0;
        sLastT = now;
        if (sAuto.frame(period, (double)sLiteAll / N3DS_TICKS_PER_MS, (double)sLiteCachedT / N3DS_TICKS_PER_MS)) {
            n3ds_log("[autolod] frame %u: level %d of %d -> lod_model_px %.1f px, lod_px %.1f px (frame period %.1f ms, display lists %.1f ms; next step down after %d quiet windows)",
                     frame, sAuto.level, sAuto.maxLevel, lodModelNow(), lodChunkNow(), period, (double)sLiteAll / N3DS_TICKS_PER_MS, sAuto.calmNeeded());
        }
    }
    { const unsigned cur = n3ds_gfxq_busy_ticks(); sLiteBusyAcc += (unsigned)(cur - sLiteBusyLast); sLiteBusyLast = cur; ++sLiteBusyFrames; }
    if (n3dsConfig().gfxLite && frame % 600 == 300) n3dstexLogStats(frame);   /* texture pool census every ~20 s */
    if (n3dsConfig().gfxLite && frame % 60 == 30) {
        n3ds_log("[lite] frame %u: render worker busy %.1f ms/frame (average of the last %u frames) | this frame: %u display-list calls = %.1f ms total (%u drawn from the model cache = %.1f ms, %u by the normal path = %.1f ms) | in-verts %u (vertex-cache hits %u, lazy decodes %u) -> tris out %u, culled %u, sub-pixel %u, clipped %u, dropped as tiny (tri_min_area) %u | segments %u tested, %u skipped (%u verts) | prims %u",
                 frame, sLiteBusyFrames ? (double)sLiteBusyAcc / N3DS_TICKS_PER_MS / sLiteBusyFrames : 0.0, sLiteBusyFrames, sLiteCalls, (double)sLiteAll / N3DS_TICKS_PER_MS, sLiteCachedN, (double)sLiteCachedT / N3DS_TICKS_PER_MS,
                 sLiteCalls - sLiteCachedN, (double)(sLiteAll - sLiteCachedT) / N3DS_TICKS_PER_MS, S.inVerts, S.vcHits, S.lazyDone, S.tris, S.culled, S.subpx, S.clipped, S.lodSkipped, S.segTests, S.segCulled, S.segVertsSkipped, S.prims);
        if (n3dsConfig().meshCpu && !n3dsConfig().gpuXform)
            n3ds_log("[lite]    cached CPU draw: %s | %u lists, %u chunks drawn, %u chunks + %u in culled groups skipped, %u tris through the cache | checks this frame %u (%u ok so far, %u rebuilt after a difference, %u confirmed differences) | mesh cache: %u built (%u stayed on the normal path), %u volatile lists, %u invalidated, %u verify mismatches",
                     sCpuOff ? "SWITCHED OFF" : "on", CS.lists, CS.chunksDrawn, CS.chunksCulled, CS.groupsCulled, CS.tris, CS.checks, sCpuChecksOk, sCpuRecTotal, sCpuBadTotal, MStot.built, MStot.buildFail, MStot.volatileLists, MStot.inval, MStot.verifyFail);
        if (n3dsConfig().meshCpu && !n3dsConfig().gpuXform)
            n3ds_log("[lite]    model sizes on screen, models/triangles (largest side): <4px %u/%u, <8px %u/%u, <16px %u/%u, <32px %u/%u, <64px %u/%u, bigger %u/%u, undecided %u/%u | lod_model_px %.1f (auto level %d): skipped %u models (%u tris) | lod_simplify_px %d: %u tris saved this frame, %u KB of simplified copies",
                     SH.models[0], SH.tris[0], SH.models[1], SH.tris[1], SH.models[2], SH.tris[2], SH.models[3], SH.tris[3], SH.models[4], SH.tris[4], SH.models[5], SH.tris[5], SH.models[6], SH.tris[6],
                     lodModelNow(), sAuto.level, SH.culled, SH.culledTris, n3dsConfig().lodSimplifyPx, sLodSavedTris, sLodBytes / 1024u);
        if (sLiteBusyFrames) {   /* where the worker's wall time goes, per frame (NEWEST-68): command classes add up to the busy time; the rest are parts of the display-list time */
            const double nf = (double)sLiteBusyFrames * N3DS_TICKS_PER_MS;
            const double cls0 = sTmCls[0] / nf, cls1 = sTmCls[1] / nf, cls2 = sTmCls[2] / nf, cls3 = sTmCls[3] / nf;
            n3ds_log("[lite2] worker, ms per frame over the last %u frames: display lists %.1f | immediate mode %.1f | state calls %.1f | frame end + present %.1f | sum %.1f (busy %.1f) || inside the display lists: GPU submit %.1f (%.0f draws/frame), material + texture resolve %.1f (%.0f/frame), model-cache lookup + verify + rebuild %.1f (%.0f/frame)",
                     sLiteBusyFrames, cls0, cls1, cls2, cls3, cls0 + cls1 + cls2 + cls3, (double)sLiteBusyAcc / nf,
                     sTmCommit / nf, (double)sTmCommitN / sLiteBusyFrames, sTmMat / nf, (double)sTmMatN / sLiteBusyFrames, sTmLookup / nf, (double)sTmLookupN / sLiteBusyFrames);
            if (sTmBodyN && sTmBodyTris) { gN3dsUsPerTri = (float)((double)sTmLoop / N3DS_TICKS_PER_MS * 1000.0 / sTmBodyTris); gN3dsDlMs = (float)(sLiteCachedTot / nf); }
            if (sTmBodyN) n3ds_log("[lite3] cached draws per frame: %.0f calls | setup (matrices, culling, size) %.1f ms = %.1f us/call | triangle loop %.1f ms for %.0f tris, %.0f vertex transforms = %.2f us/tri | rest of a display-list call (lookup, material, submit) %.1f ms",
                     (double)sTmBodyN / sLiteBusyFrames, sTmSetup / nf, (double)sTmSetup / N3DS_TICKS_PER_MS * 1000.0 / sTmBodyN, sTmLoop / nf, (double)sTmBodyTris / sLiteBusyFrames, (double)sTmBodyVerts / sLiteBusyFrames,
                     sTmBodyTris ? (double)sTmLoop / N3DS_TICKS_PER_MS * 1000.0 / sTmBodyTris : 0.0, (double)sLiteCachedTot / nf - sTmSetup / nf - sTmLoop / nf);
        }
        if (sGP.frames && (sGP.calls || sGP.nRefused)) {   /* r54 [lite5]: the GPU transform path's own CPU cost */
            unsigned long long it[4] = {0, 0, 0, 0}; unsigned ic[3] = {0, 0, 0};
            n3ds_gpu_mesh_prof_take(it, ic);
            const double fr = (double)sGP.frames, us = N3DS_TICKS_PER_MS / 1000.0, c = sGP.calls ? (double)sGP.calls : 1.0;
            const double cull = (double)(sGP.tot - sGP.pre - sGP.state - sGP.cpu - sGP.queue);
            const double tc = (double)(sGP.calls + sGP.nRefused ? sGP.calls + sGP.nRefused : 1);   /* material / lookup / uniforms run for refused calls too */
            n3ds_log("[lite5]   culling+classify split, us per call: matrices %.1f, model box tests %.1f, lod %.1f, ring flush before a draw %.1f, pieces/groups %.1f",
                     sGP.mtx / us / c, sGP.cls / us / c, sGP.lod / us / c, sGP.flush / us / c, (cull - (double)(sGP.mtx + sGP.cls + sGP.lod + sGP.flush)) / us / c);
            n3ds_log("[lite5] GPU transform path over %u profiled frames, per frame: %.0f calls = %.1f ms (%.1f us/call: gate+lookup+uniforms %.1f [material %.1f, model lookup %.1f, uniforms+checks %.1f per call reaching them], draw state %.1f, culling+classify %.1f, queue %.1f, chunks on the CPU %.1f) | %.1f CPU chunks (%.0f tris) | %.0f calls handed back = %.1f ms | issue (citro3d) %.1f ms for %.0f draws: state %.1f, uniforms %.1f (%.1f registers/draw), draw calls + splits %.1f",
                     sGP.frames, sGP.calls / fr, sGP.tot / us / 1000.0 / fr, sGP.tot / us / c, sGP.pre / us / c, sGP.mat / us / tc, sGP.lookup / us / tc, sGP.unif / us / c, sGP.state / us / c, cull / us / c, sGP.queue / us / c, sGP.cpu / us / c,
                     sGP.cpuChunks / fr, sGP.cpuTris / fr, sGP.nRefused / fr, sGP.refused / us / 1000.0 / fr,
                     it[0] / us / 1000.0 / fr, ic[0] / fr, it[1] / us / 1000.0 / fr, it[2] / us / 1000.0 / fr, ic[0] ? (double)ic[1] / ic[0] : 0.0, it[3] / us / 1000.0 / fr);
            memset(&sGP, 0, sizeof sGP);
        }
        if (sPhChunks) {   /* r20 [lite4]: the phase-profiled frame (n3ds_ticks() cost already taken off each phase) */
            const double tk = (double)N3DS_TICKS_PER_MS / 1000.0, oh = (double)sPhTickCost * sPhChunks;   /* ticks per us; one tick read per phase per chunk */
            double ph[4]; for (int i = 0; i < 4; ++i) { ph[i] = ((double)sPh[i] - oh) / tk; if (ph[i] < 0) ph[i] = 0; }
            n3ds_log("[lite4] phase-profiled frame %u: %u chunks, %u tris (%u kept) | transform %.1f ms for %u verts = %.2f us/vert | reject tests %.1f ms = %.2f us/tri | attribute decode %.1f ms for %u verts = %.2f us/vert | clip + ring write %.1f ms = %.2f us/kept tri | tick read %.2f us",
                     frame, sPhChunks, sPhTris, sPhKept, ph[0] / 1000.0, sPhVerts, sPhVerts ? ph[0] / sPhVerts : 0.0, ph[1] / 1000.0, sPhTris ? ph[1] / sPhTris : 0.0,
                     ph[2] / 1000.0, sPhDecoded, sPhDecoded ? ph[2] / sPhDecoded : 0.0, ph[3] / 1000.0, sPhKept ? ph[3] / sPhKept : 0.0, (double)sPhTickCost / tk);
            memset(sPh, 0, sizeof sPh); sPhVerts = sPhTris = sPhKept = sPhDecoded = sPhChunks = 0;
        }
        bool used[64] = {};
        for (int pass = 0; pass < 6; ++pass) {
            int best = -1;
            for (int i = 0; i < 64; ++i) if (sLite[i].p && !used[i] && (best < 0 || sLite[i].ticks > sLite[best].ticks)) best = i;
            if (best < 0) break;
            used[best] = true;
            const LiteTop& e = sLite[best];
            const Mesh* mi = nullptr;
            if (sMC) { const uint32_t h = (uint32_t)((((uintptr_t)e.p >> 4) ^ ((uintptr_t)e.p >> 13)) * 2654435761u) >> (32 - MC_SLOTS_LOG2);
                for (int k = 0; k < MC_PROBE; ++k) { const Mesh& m = sMC[(h + k) & (MC_SLOTS - 1)]; if (m.ptr == e.p && m.n == e.n) { mi = &m; break; } } }
            if (mi) n3ds_log("[lite]    #%d list %p %u bytes: %u call(s), %.2f ms (%.2f ms per call) | model: %u verts, %u tris, %u matrices, %u chunks, normals %d, colours %d | last draw: %.0f px, %u tris in %u chunks | simplified copy: state %u, %u tris, used %u times",
                             pass + 1, e.p, e.n, e.calls, (double)e.ticks / N3DS_TICKS_PER_MS, e.calls ? (double)e.ticks / N3DS_TICKS_PER_MS / e.calls : 0.0,
                             mi->nv, mi->ni / 3, mi->nslot, mi->nch, (int)mi->hasN, (int)mi->hasC, mi->dbgExt, mi->dbgTris, mi->dbgChunks, mi->lodState, mi->lni / 3, mi->dbgLod);
            else
            n3ds_log("[lite]    #%d list %p %u bytes: %u call(s), %.2f ms (%.2f ms per call)", pass + 1, e.p, e.n, e.calls, (double)e.ticks / N3DS_TICKS_PER_MS, e.calls ? (double)e.ticks / N3DS_TICKS_PER_MS / e.calls : 0.0);
        }
    }
    if (n3dsConfig().gfxLite && frame % 60 == 30) { sLiteBusyAcc = 0; sLiteBusyFrames = 0; memset(sTmCls, 0, sizeof sTmCls); sTmCommit = sTmMat = sTmLookup = 0; sTmCommitN = sTmMatN = sTmLookupN = 0; sTmSetup = sTmLoop = 0; sTmBodyN = sTmBodyTris = sTmBodyVerts = 0; sLiteCachedTot = 0; }
    memset(sLite, 0, sizeof sLite); sLiteAll = 0; sLiteCalls = 0; sLiteCachedT = 0; sLiteCachedN = 0;
}
}   /* extern "C++" */

void rp_call_display_list(const void* list, u32 nbytes)
{
    if (!list) return;
    ++S.dlCalls; ++sDlFrameCalls;
    if (n3dsConfig().gfxLite || n3dsConfig().autoLod) {      /* always-on, cheap: time per list (auto_lod needs the total), top lists logged once a second (gfx_lite) */
        const unsigned long long t0 = n3ds_ticks();
        const bool cached = meshTryDraw((const uint8_t*)list, nbytes) || meshCpuTry((const uint8_t*)list, nbytes);
        if (!cached) doDisplayList((const uint8_t*)list, nbytes);
        liteNote(list, nbytes, n3ds_ticks() - t0, cached);
        return;
    }
    if (meshTryDraw((const uint8_t*)list, nbytes)) return;   /* 1p: cached mesh, transformed by the GPU */
    if (meshCpuTry((const uint8_t*)list, nbytes)) return;    /* cached mesh, drawn by the CPU from the cache (gpu_xform = 0) */
    if (!sProfile) { doDisplayList((const uint8_t*)list, nbytes); return; }
    const unsigned long long t0 = n3ds_ticks();
    doDisplayList((const uint8_t*)list, nbytes);
    S.emitTicks += n3ds_ticks() - t0;
    /* Profiled frames only: re-run the same list without emitting, to split the cost (parse-only, then parse + transform). */
    const unsigned savedIn = S.inVerts, savedHits = S.vcHits;
    const unsigned long long t1 = n3ds_ticks();
    doDisplayList((const uint8_t*)list, nbytes, 1);
    const unsigned long long t2 = n3ds_ticks();
    doDisplayList((const uint8_t*)list, nbytes, 2);
    const unsigned long long t3 = n3ds_ticks();
    S.parseTicks += t2 - t1; S.xformTicks += t3 - t2;
    S.inVerts = savedIn; S.vcHits = savedHits;
}
/* Title screen "logo only" (title_logo_only.patch): TitleSection::draw sets this around CinematicPlayer::refresh, and the player then draws only
   the actor whose model is cinemas/titles/logo.mod instead of the whole opening.cin scene (ground, leaves, sky: 71 000 vertices, ~3 FPS). */
static volatile int sCineLogoOnly = 0;
extern "C" void n3ds_cine_logo_only(int on) { sCineLogoOnly = on; }
extern "C" int n3ds_cine_logo_only_get(void) { return sCineLogoOnly; }

/* test hooks (tools/host_test) */
extern "C" void n3ds_gfx_debug_dlcull(int on) { sDLCOn = on != 0; }
extern "C" unsigned n3ds_gfx_debug_seg_culled(void) { return S.segCulled; }
/* how many display-list vertices were served from the vertex cache this frame */
extern "C" unsigned n3ds_gfx_debug_vcache_hits(void) { return S.vcHits; }


/* ================= two-thread split: the game thread RECORDS pc_gfx_* calls, a core-2 worker REPLAYS them =================
   Generated from the definitions above (each recorded call was renamed pc_gfx_X -> rp_X; the pc_gfx_X wrappers below
   serialize their arguments into a single-producer / single-consumer byte ring in linear memory). Matrices and texture
   objects are copied; display lists, vertex arrays and texture data are referenced by pointer (see NOTES_gfx_thread.md).
   Everything touching G, the vertex ring, citro3d and the texture cache runs on the worker only. */
}  // extern "C" (re-opened below)

namespace {
enum QOp : uint32_t { Q_WRAP = 0, Q_FRAME = 1, Q_load_tlut, Q_release_texture, Q_set_projection, Q_set_viewport, Q_set_scissor, Q_load_pos_mtx, Q_load_nrm_mtx, Q_load_tex_mtx, Q_set_tex_coord_gen, Q_set_current_mtx, Q_set_z_mode, Q_set_blend_mode, Q_set_cull_mode, Q_set_color_update, Q_set_alpha_update, Q_set_alpha_compare, Q_set_copy_clear, Q_set_chan_ctrl, Q_set_chan_mat_color, Q_set_tev_order, Q_load_tex_obj, Q_set_num_tev_stages, Q_set_tev_color_in, Q_set_tev_alpha_in, Q_set_tev_color_op, Q_set_tev_alpha_op, Q_set_tev_color, Q_set_tev_color_s10, Q_set_tev_kcolor, Q_set_tev_kcolor_sel, Q_set_tev_kalpha_sel, Q_perf_scope_begin, Q_perf_scope_end, Q_clear_vtx_desc, Q_set_vtx_desc, Q_set_vtx_attr_fmt, Q_set_array, Q_begin, Q_position, Q_color, Q_texcoord, Q_end, Q_call_display_list, Q_lod_exempt, Q_bottom, Q_tag, Q_load_light, Q_chan_amb };
struct QHdr { uint32_t op, size; };
struct QFrame { unsigned frame; };
struct QS_load_tlut { GXTlutObj a0; u32 a1; };
struct QS_release_texture { void* a0; };
struct QS_set_projection { float a0[4][4]; GXProjectionType a1; };
struct QS_set_viewport { f32 a0; f32 a1; f32 a2; f32 a3; f32 a4; f32 a5; };
struct QS_set_scissor { u32 a0; u32 a1; u32 a2; u32 a3; };
struct QS_load_pos_mtx { float a0[3][4]; u32 a1; };
struct QS_load_tex_mtx { float a0[3][4]; u32 a1; };
struct QS_set_tex_coord_gen { GXTexCoordID a0; GXTexGenType a1; GXTexGenSrc a2; u32 a3; };
struct QS_load_nrm_mtx { float a0[3][4]; u32 a1; };
struct QS_set_current_mtx { u32 a0; };
struct QS_set_z_mode { GXBool a0; GXCompare a1; GXBool a2; };
struct QS_set_blend_mode { GXBlendMode a0; GXBlendFactor a1; GXBlendFactor a2; GXLogicOp a3; };
struct QS_set_cull_mode { GXCullMode a0; };
struct QS_set_color_update { GXBool a0; };
struct QS_set_alpha_update { GXBool a0; };
struct QS_set_alpha_compare { GXCompare a0; u8 a1; GXAlphaOp a2; GXCompare a3; u8 a4; };
struct QS_set_copy_clear { GXColor a0; u32 a1; };
struct QS_set_chan_ctrl { GXChannelID a0; GXBool a1; GXColorSrc a2; GXColorSrc a3; u32 a4; GXDiffuseFn a5; GXAttnFn a6; };
struct QS_set_chan_mat_color { GXChannelID a0; GXColor a1; };
struct QS_set_tev_order { GXTevStageID a0; GXTexCoordID a1; GXTexMapID a2; GXChannelID a3; };
struct QS_load_tex_obj { GXTexObj a0; GXTexMapID a1; };
struct QS_set_num_tev_stages { u8 a0; };
struct QS_set_tev_color_in { GXTevStageID a0; GXTevColorArg a1; GXTevColorArg a2; GXTevColorArg a3; GXTevColorArg a4; };
struct QS_set_tev_alpha_in { GXTevStageID a0; GXTevAlphaArg a1; GXTevAlphaArg a2; GXTevAlphaArg a3; GXTevAlphaArg a4; };
struct QS_set_tev_color_op { GXTevStageID a0; GXTevOp a1; GXTevBias a2; GXTevScale a3; GXBool a4; GXTevRegID a5; };
struct QS_set_tev_alpha_op { GXTevStageID a0; GXTevOp a1; GXTevBias a2; GXTevScale a3; GXBool a4; GXTevRegID a5; };
struct QS_set_tev_color { GXTevRegID a0; GXColor a1; };
struct QS_set_tev_color_s10 { GXTevRegID a0; GXColorS10 a1; };
struct QS_set_tev_kcolor { GXTevKColorID a0; GXColor a1; };
struct QS_set_tev_kcolor_sel { GXTevStageID a0; GXTevKColorSel a1; };
struct QS_set_tev_kalpha_sel { GXTevStageID a0; GXTevKAlphaSel a1; };
struct QS_perf_scope_begin { const char* a0; };
struct QS_lod_exempt { int a0; };
struct QS_bottom { int a0; };
struct QS_tag { int a0; };
struct QS_load_light { float v[16]; u32 mask; };
struct QS_chan_amb { int c; GXColor col; };
struct QS_perf_scope_end { int none; };
struct QS_clear_vtx_desc { int none; };
struct QS_set_vtx_desc { GXAttr a0; GXAttrType a1; };
struct QS_set_vtx_attr_fmt { GXVtxFmt a0; GXAttr a1; GXCompCnt a2; GXCompType a3; u8 a4; };
struct QS_set_array { GXAttr a0; void* a1; u8 a2; };
struct QS_begin { GXPrimitive a0; GXVtxFmt a1; u16 a2; };
struct QS_position { f32 a0; f32 a1; f32 a2; };
struct QS_color { u8 a0; u8 a1; u8 a2; u8 a3; };
struct QS_texcoord { f32 a0; f32 a1; };
struct QS_end { int none; };
struct QS_call_display_list { const void* a0; u32 a1; };
bool sQOn = false;
uint8_t* sQBuf = nullptr;
uint32_t sQCap = 0, sQWrite = 0, sQPending = 0;                  /* producer-private: bytes written / not yet published */
std::atomic<uint32_t> sQProduced(0), sQConsumed(0);               /* monotonic byte counters, position = counter % capacity */
std::atomic<unsigned> sQFramesDone(0);
unsigned sQFramesPushed = 0;
unsigned sQDiagWaits = 0, sQDiagLast = 0; unsigned long long sQDiagTicks = 0;   /* diagnostics: producer stalls on a full queue */
inline bool qOn() { return sQOn; }
void qPublish() { sQProduced.store(sQWrite, std::memory_order_release); sQPending = 0; }
void qPublishIf(bool force) { if (force || sQPending >= 4096) qPublish(); }
void* qPush(uint32_t op, uint32_t size)
{
    const uint32_t total = 8 + ((size + 7) & ~7u);
    for (;;) {
        const uint32_t used = sQWrite - sQConsumed.load(std::memory_order_acquire);
        const uint32_t pos = (sQWrite & (sQCap - 1)), pad = pos + total > sQCap ? sQCap - pos : 0;
        if (used + pad + total <= sQCap) {
            if (pad) { QHdr* w = (QHdr*)(sQBuf + pos); w->op = Q_WRAP; w->size = pad - 8; sQWrite += pad; sQPending += pad; }
            QHdr* h = (QHdr*)(sQBuf + (sQWrite & (sQCap - 1))); h->op = op; h->size = total - 8;
            sQWrite += total; sQPending += total;
            return h + 1;
        }
        qPublish();                                                /* ring full: let the worker run, then retry */
        const unsigned long long w0 = n3ds_ticks();
        n3ds_sleep_ns(60000);
        sQDiagTicks += n3ds_ticks() - w0; ++sQDiagWaits;   /* (NEWEST-68: was truncated to 0.1 ms units per 60 us wait = always 0) */
    }
}
void qReplay(uint32_t op, void* p)
{
    switch (op) {
    case Q_FRAME: {
        const unsigned f = ((QFrame*)p)->frame;
        n3ds_gfx_flush();
        if (!n3ds_gpu_present(f)) n3ds_present_heartbeat(f);
        n3ds_gfx_frame_end(f);
        sQFramesDone.store(f + 1, std::memory_order_release);
        break; }
    case Q_set_projection: { QS_set_projection* q = (QS_set_projection*)p; (void)q; rp_set_projection(q->a0, q->a1); break; }
    case Q_set_viewport: { QS_set_viewport* q = (QS_set_viewport*)p; (void)q; rp_set_viewport(q->a0, q->a1, q->a2, q->a3, q->a4, q->a5); break; }
    case Q_set_scissor: { QS_set_scissor* q = (QS_set_scissor*)p; (void)q; rp_set_scissor(q->a0, q->a1, q->a2, q->a3); break; }
    case Q_load_tex_mtx: { QS_load_tex_mtx* q = (QS_load_tex_mtx*)p; (void)q; rp_load_tex_mtx(q->a0, q->a1); break; }
    case Q_set_tex_coord_gen: { QS_set_tex_coord_gen* q = (QS_set_tex_coord_gen*)p; (void)q; rp_set_tex_coord_gen(q->a0, q->a1, q->a2, q->a3); break; }
    case Q_load_pos_mtx: { QS_load_pos_mtx* q = (QS_load_pos_mtx*)p; (void)q; rp_load_pos_mtx(q->a0, q->a1); break; }
    case Q_load_nrm_mtx: { QS_load_nrm_mtx* q = (QS_load_nrm_mtx*)p; (void)q; rp_load_nrm_mtx(q->a0, q->a1); break; }
    case Q_set_current_mtx: { QS_set_current_mtx* q = (QS_set_current_mtx*)p; (void)q; rp_set_current_mtx(q->a0); break; }
    case Q_set_z_mode: { QS_set_z_mode* q = (QS_set_z_mode*)p; (void)q; rp_set_z_mode(q->a0, q->a1, q->a2); break; }
    case Q_set_blend_mode: { QS_set_blend_mode* q = (QS_set_blend_mode*)p; (void)q; rp_set_blend_mode(q->a0, q->a1, q->a2, q->a3); break; }
    case Q_set_cull_mode: { QS_set_cull_mode* q = (QS_set_cull_mode*)p; (void)q; rp_set_cull_mode(q->a0); break; }
    case Q_set_color_update: { QS_set_color_update* q = (QS_set_color_update*)p; (void)q; rp_set_color_update(q->a0); break; }
    case Q_set_alpha_update: { QS_set_alpha_update* q = (QS_set_alpha_update*)p; (void)q; rp_set_alpha_update(q->a0); break; }
    case Q_set_alpha_compare: { QS_set_alpha_compare* q = (QS_set_alpha_compare*)p; (void)q; rp_set_alpha_compare(q->a0, q->a1, q->a2, q->a3, q->a4); break; }
    case Q_set_copy_clear: { QS_set_copy_clear* q = (QS_set_copy_clear*)p; (void)q; rp_set_copy_clear(q->a0, q->a1); break; }
    case Q_set_chan_ctrl: { QS_set_chan_ctrl* q = (QS_set_chan_ctrl*)p; (void)q; rp_set_chan_ctrl(q->a0, q->a1, q->a2, q->a3, q->a4, q->a5, q->a6); break; }
    case Q_set_chan_mat_color: { QS_set_chan_mat_color* q = (QS_set_chan_mat_color*)p; (void)q; rp_set_chan_mat_color(q->a0, q->a1); break; }
    case Q_set_tev_order: { QS_set_tev_order* q = (QS_set_tev_order*)p; (void)q; rp_set_tev_order(q->a0, q->a1, q->a2, q->a3); break; }
    case Q_load_tex_obj: { QS_load_tex_obj* q = (QS_load_tex_obj*)p; (void)q; rp_load_tex_obj(&q->a0, q->a1); break; }
    case Q_set_num_tev_stages: { QS_set_num_tev_stages* q = (QS_set_num_tev_stages*)p; (void)q; rp_set_num_tev_stages(q->a0); break; }
    case Q_set_tev_color_in: { QS_set_tev_color_in* q = (QS_set_tev_color_in*)p; (void)q; rp_set_tev_color_in(q->a0, q->a1, q->a2, q->a3, q->a4); break; }
    case Q_set_tev_alpha_in: { QS_set_tev_alpha_in* q = (QS_set_tev_alpha_in*)p; (void)q; rp_set_tev_alpha_in(q->a0, q->a1, q->a2, q->a3, q->a4); break; }
    case Q_set_tev_color_op: { QS_set_tev_color_op* q = (QS_set_tev_color_op*)p; (void)q; rp_set_tev_color_op(q->a0, q->a1, q->a2, q->a3, q->a4, q->a5); break; }
    case Q_set_tev_alpha_op: { QS_set_tev_alpha_op* q = (QS_set_tev_alpha_op*)p; (void)q; rp_set_tev_alpha_op(q->a0, q->a1, q->a2, q->a3, q->a4, q->a5); break; }
    case Q_set_tev_color: { QS_set_tev_color* q = (QS_set_tev_color*)p; (void)q; rp_set_tev_color(q->a0, q->a1); break; }
    case Q_set_tev_color_s10: { QS_set_tev_color_s10* q = (QS_set_tev_color_s10*)p; (void)q; rp_set_tev_color_s10(q->a0, q->a1); break; }
    case Q_set_tev_kcolor: { QS_set_tev_kcolor* q = (QS_set_tev_kcolor*)p; (void)q; rp_set_tev_kcolor(q->a0, q->a1); break; }
    case Q_set_tev_kcolor_sel: { QS_set_tev_kcolor_sel* q = (QS_set_tev_kcolor_sel*)p; (void)q; rp_set_tev_kcolor_sel(q->a0, q->a1); break; }
    case Q_set_tev_kalpha_sel: { QS_set_tev_kalpha_sel* q = (QS_set_tev_kalpha_sel*)p; (void)q; rp_set_tev_kalpha_sel(q->a0, q->a1); break; }
    case Q_perf_scope_begin: { QS_perf_scope_begin* q = (QS_perf_scope_begin*)p; (void)q; rp_perf_scope_begin(q->a0); break; }
    case Q_perf_scope_end: { QS_perf_scope_end* q = (QS_perf_scope_end*)p; (void)q; rp_perf_scope_end(); break; }
    case Q_clear_vtx_desc: { QS_clear_vtx_desc* q = (QS_clear_vtx_desc*)p; (void)q; rp_clear_vtx_desc(); break; }
    case Q_set_vtx_desc: { QS_set_vtx_desc* q = (QS_set_vtx_desc*)p; (void)q; rp_set_vtx_desc(q->a0, q->a1); break; }
    case Q_set_vtx_attr_fmt: { QS_set_vtx_attr_fmt* q = (QS_set_vtx_attr_fmt*)p; (void)q; rp_set_vtx_attr_fmt(q->a0, q->a1, q->a2, q->a3, q->a4); break; }
    case Q_set_array: { QS_set_array* q = (QS_set_array*)p; (void)q; rp_set_array(q->a0, q->a1, q->a2); break; }
    case Q_begin: { QS_begin* q = (QS_begin*)p; (void)q; rp_begin(q->a0, q->a1, q->a2); break; }
    case Q_position: { QS_position* q = (QS_position*)p; (void)q; rp_position(q->a0, q->a1, q->a2); break; }
    case Q_color: { QS_color* q = (QS_color*)p; (void)q; rp_color(q->a0, q->a1, q->a2, q->a3); break; }
    case Q_texcoord: { QS_texcoord* q = (QS_texcoord*)p; (void)q; rp_texcoord(q->a0, q->a1); break; }
    case Q_end: { QS_end* q = (QS_end*)p; (void)q; rp_end(); break; }
    case Q_lod_exempt: { rp_lod_exempt(((QS_lod_exempt*)p)->a0); break; }
    case Q_bottom: { rp_bottom(((QS_bottom*)p)->a0); break; }
    case Q_tag: { sTag = ((QS_tag*)p)->a0; break; }
    case Q_load_light: { QS_load_light* q = (QS_load_light*)p; lightLoad(q->v, q->mask); break; }
    case Q_chan_amb: { QS_chan_amb* q = (QS_chan_amb*)p; chanAmbSet(q->c, q->col); break; }
    case Q_call_display_list: { QS_call_display_list* q = (QS_call_display_list*)p; (void)q; rp_call_display_list(q->a0, q->a1); break; }
    case Q_load_tlut: { QS_load_tlut* q = (QS_load_tlut*)p; n3dstex_rp_load_tlut(&q->a0, q->a1); break; }
    case Q_release_texture: { QS_release_texture* q = (QS_release_texture*)p; n3dstex_rp_release_texture(q->a0); break; }
    default: break;
    }
}
}  // namespace

extern "C" {

/* Worker entry: run every command that has been published. Returns how many ran (0 = idle, the caller sleeps). */
static inline int opClass(uint32_t op) { return op == Q_call_display_list ? 0 : (op >= Q_begin && op <= Q_end) ? 1 : op == Q_FRAME ? 3 : 2; }
static volatile uint32_t sQBusy = 0;                                  /* worker ticks spent replaying (single writer; the reader uses wrap-safe differences) */
unsigned n3ds_gfxq_busy_ticks(void) { return sQBusy; }
extern "C" void n3ds_gfxq_diag_take(unsigned* bytes, unsigned* waits, unsigned* ms10)
{
    *bytes = sQWrite - sQDiagLast; sQDiagLast = sQWrite; *waits = sQDiagWaits; *ms10 = (unsigned)(sQDiagTicks * 10000ULL / 268111856ULL); sQDiagWaits = 0; sQDiagTicks = 0;
}

int n3ds_gfxq_drain(void)
{
    int n = 0;
    unsigned long long tLast = 0;                                     /* start of the busy interval not yet added to sQBusy */
    const bool tm = n3dsConfig().gfxLite;
    int curCls = -1; unsigned long long tCls = 0;                     /* gfx_lite timeline: the command class being replayed and when it started (one clock read per CLASS CHANGE, not per command) */
    for (;;) {
        const uint32_t prod = sQProduced.load(std::memory_order_acquire), cons = sQConsumed.load(std::memory_order_relaxed);
        if (prod == cons) { if (n) sQBusy = sQBusy + (uint32_t)(n3ds_ticks() - tLast); if (tm && curCls >= 0) sTmCls[curCls] += n3ds_ticks() - tCls; return n; }
        if (!n) tLast = n3ds_ticks();
        QHdr* h = (QHdr*)(sQBuf + (cons & (sQCap - 1)));
        const uint32_t total = 8 + h->size;
        const uint32_t op = h->op;
        if (tm && op != Q_WRAP) { const int c = opClass(op); if (c != curCls) { const unsigned long long t = n3ds_ticks(); if (curCls >= 0) sTmCls[curCls] += t - tCls; tCls = t; curCls = c; } }
        if (op != Q_WRAP) { qReplay(op, h + 1); ++n; }
        sQConsumed.store(cons + total, std::memory_order_release);
        /* account at every frame boundary and every 64 commands, so a drain that runs for a whole frame is still counted */
        if (op == Q_FRAME || (n & 63) == 0) { const unsigned long long t = n3ds_ticks(); sQBusy = sQBusy + (uint32_t)(t - tLast); tLast = t; }
    }
}

int n3ds_gfxq_running(void) { return sQOn ? 1 : 0; }

/* Call once after pc_gfx_init. Disabled / no worker (old 3DS) / no memory -> everything stays on the game thread. */
void n3ds_gfxq_start(int enable)
{
    if (sQOn || sQBuf) return;
    if (!enable) { n3ds_log("[gfxq] gfx_worker=0: rendering on the game thread"); return; }
    { uint32_t cap = 128u * 1024u; const uint32_t want = (uint32_t)n3dsConfig().gfxQueueKb * 1024u;   /* power of two: the read/write counters are masked with %, and wrap at 2^32 */
      while (cap * 2 <= want && cap < 4096u * 1024u) cap *= 2; sQCap = cap; }
    /* r53 R11 (AGENTS.md): sQCap is a power of two (built just above), so the queue wraps with "& (sQCap - 1)": "% sQCap" by a runtime
       value was a __aeabi_uidivmod call on EVERY recorded GX command (game thread) and every replayed one (worker). */
    sQBuf = (uint8_t*)n3ds_linear_alloc(sQCap);
    if (!sQBuf) { n3ds_log("[gfxq] no linear memory for the command queue: staying single-threaded"); return; }
    if (!n3ds_worker_start()) { n3ds_log("[gfxq] worker thread unavailable: staying single-threaded"); return; }
    sQOn = true;
    n3ds_log("[gfxq] two-thread rendering on: %u KB queue, worker replays GX commands and presents", sQCap / 1024);
}

/* Shutdown: let the worker finish everything already recorded (including the in-flight present), stop it, and hand the renderer
   back to the calling thread. Without this the worker kept polling the queue on core 2 while the app unmapped its memory and
   crashed (Luma dumps: data abort in n3ds_gfxq_drain). Safe to call twice; the queue buffer is deliberately not freed. */
void n3ds_gfxq_stop(void)
{
    if (!sQOn) return;
    qPublish();
    const unsigned long long t0 = n3ds_ticks();
    while (sQConsumed.load(std::memory_order_acquire) != sQWrite && n3ds_ticks() - t0 < 2ull * 268111856ull) n3ds_sleep_ns(100000);   /* <= ~2 s */
    n3ds_worker_stop();                                                /* join: after this the worker no longer touches G or the queue */
    sQOn = false;
    n3ds_log("[gfxq] worker stopped (%u frames queued, %u presented)", sQFramesPushed, sQFramesDone.load());
}

/* Replaces flush + present + frame_end in pc_window_swap_buffers. Returns 0 when not threaded (the caller does it inline).
   One frame may be in flight: blocks until the worker has finished the previous one. *waited = ticks spent blocked. */
int n3ds_gfxq_end_frame(unsigned frame, unsigned long long* waited)
{
    if (!sQOn) return 0;
    QFrame* q = (QFrame*)qPush(Q_FRAME, sizeof(QFrame)); q->frame = frame;
    qPublish();
    ++sQFramesPushed;
    const unsigned long long t0 = n3ds_ticks();
    while (sQFramesDone.load(std::memory_order_acquire) + 1 < sQFramesPushed) n3ds_sleep_ns(50000);
    if (waited) *waited = n3ds_ticks() - t0;
    return 1;
}

/* ---- recorded API ---- */
void pc_gfx_load_tlut(GXTlutObj* o, u32 name)
{
    if (!qOn()) { n3dstex_rp_load_tlut(o, name); return; }
    QS_load_tlut* q = (QS_load_tlut*)qPush(Q_load_tlut, sizeof(QS_load_tlut)); memcpy(&q->a0, o, sizeof q->a0); q->a1 = name; qPublishIf(false);
}
/* the worker may still hold a queued draw that samples this texture: the release is replayed in order, after those draws */
void pc_gfx_release_texture(void* p)
{
    if (!qOn()) { n3dstex_rp_release_texture(p); return; }
    QS_release_texture* q = (QS_release_texture*)qPush(Q_release_texture, sizeof(QS_release_texture)); q->a0 = p; qPublishIf(false);
}
void pc_gfx_set_projection(const Mtx44 a0, GXProjectionType a1) { if (!qOn()) { rp_set_projection(a0, a1); return; } QS_set_projection* q = (QS_set_projection*)qPush(Q_set_projection, sizeof(QS_set_projection)); memcpy(q->a0, a0, sizeof q->a0); q->a1 = a1; qPublishIf(false); }
void pc_gfx_set_viewport(f32 a0, f32 a1, f32 a2, f32 a3, f32 a4, f32 a5) { if (!qOn()) { rp_set_viewport(a0, a1, a2, a3, a4, a5); return; } QS_set_viewport* q = (QS_set_viewport*)qPush(Q_set_viewport, sizeof(QS_set_viewport)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4; q->a5 = a5; qPublishIf(false); }
void pc_gfx_set_scissor(u32 a0, u32 a1, u32 a2, u32 a3) { if (!qOn()) { rp_set_scissor(a0, a1, a2, a3); return; } QS_set_scissor* q = (QS_set_scissor*)qPush(Q_set_scissor, sizeof(QS_set_scissor)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; qPublishIf(false); }
void pc_gfx_load_tex_mtx(const Mtx a0, u32 a1) { if (!qOn()) { rp_load_tex_mtx(a0, a1); return; } QS_load_tex_mtx* q = (QS_load_tex_mtx*)qPush(Q_load_tex_mtx, sizeof(QS_load_tex_mtx)); memcpy(q->a0, a0, sizeof q->a0); q->a1 = a1; qPublishIf(false); }
void pc_gfx_set_tex_coord_gen(GXTexCoordID a0, GXTexGenType a1, GXTexGenSrc a2, u32 a3) { if (!qOn()) { rp_set_tex_coord_gen(a0, a1, a2, a3); return; } QS_set_tex_coord_gen* q = (QS_set_tex_coord_gen*)qPush(Q_set_tex_coord_gen, sizeof(QS_set_tex_coord_gen)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; qPublishIf(false); }
void pc_gfx_load_pos_mtx(const Mtx a0, u32 a1) { if (!qOn()) { rp_load_pos_mtx(a0, a1); return; } QS_load_pos_mtx* q = (QS_load_pos_mtx*)qPush(Q_load_pos_mtx, sizeof(QS_load_pos_mtx)); memcpy(q->a0, a0, sizeof q->a0); q->a1 = a1; qPublishIf(false); }
void pc_gfx_load_nrm_mtx(const Mtx a0, u32 a1) { if (!qOn()) { rp_load_nrm_mtx(a0, a1); return; } QS_load_nrm_mtx* q = (QS_load_nrm_mtx*)qPush(Q_load_nrm_mtx, sizeof(QS_load_nrm_mtx)); memcpy(q->a0, a0, sizeof q->a0); q->a1 = a1; qPublishIf(false); }
void pc_gfx_set_current_mtx(u32 a0) { if (!qOn()) { rp_set_current_mtx(a0); return; } QS_set_current_mtx* q = (QS_set_current_mtx*)qPush(Q_set_current_mtx, sizeof(QS_set_current_mtx)); q->a0 = a0; qPublishIf(false); }
void pc_gfx_set_z_mode(GXBool a0, GXCompare a1, GXBool a2) { if (!qOn()) { rp_set_z_mode(a0, a1, a2); return; } QS_set_z_mode* q = (QS_set_z_mode*)qPush(Q_set_z_mode, sizeof(QS_set_z_mode)); q->a0 = a0; q->a1 = a1; q->a2 = a2; qPublishIf(false); }
void pc_gfx_set_blend_mode(GXBlendMode a0, GXBlendFactor a1, GXBlendFactor a2, GXLogicOp a3) { if (!qOn()) { rp_set_blend_mode(a0, a1, a2, a3); return; } QS_set_blend_mode* q = (QS_set_blend_mode*)qPush(Q_set_blend_mode, sizeof(QS_set_blend_mode)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; qPublishIf(false); }
void pc_gfx_set_cull_mode(GXCullMode a0) { if (!qOn()) { rp_set_cull_mode(a0); return; } QS_set_cull_mode* q = (QS_set_cull_mode*)qPush(Q_set_cull_mode, sizeof(QS_set_cull_mode)); q->a0 = a0; qPublishIf(false); }
void pc_gfx_set_color_update(GXBool a0) { if (!qOn()) { rp_set_color_update(a0); return; } QS_set_color_update* q = (QS_set_color_update*)qPush(Q_set_color_update, sizeof(QS_set_color_update)); q->a0 = a0; qPublishIf(false); }
void pc_gfx_set_alpha_update(GXBool a0) { if (!qOn()) { rp_set_alpha_update(a0); return; } QS_set_alpha_update* q = (QS_set_alpha_update*)qPush(Q_set_alpha_update, sizeof(QS_set_alpha_update)); q->a0 = a0; qPublishIf(false); }
void pc_gfx_set_alpha_compare(GXCompare a0, u8 a1, GXAlphaOp a2, GXCompare a3, u8 a4) { if (!qOn()) { rp_set_alpha_compare(a0, a1, a2, a3, a4); return; } QS_set_alpha_compare* q = (QS_set_alpha_compare*)qPush(Q_set_alpha_compare, sizeof(QS_set_alpha_compare)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4; qPublishIf(false); }
// 0.9.3 additions to pc_gfx.h. Eternal Night glow (ambient boost) and the netplay two-pass UI reset have no 3DS use: no-ops.
void pc_gfx_get_alpha_compare(GXCompare* c0, u8* r0, GXAlphaOp* op, GXCompare* c1, u8* r1) { if (c0) *c0 = (GXCompare)7; if (r0) *r0 = 0; if (op) *op = (GXAlphaOp)0; if (c1) *c1 = (GXCompare)7; if (r1) *r1 = 0; }
void pc_gfx_set_amb_boost(GXColor) {}
void pc_gfx_clear_amb_boost(void) {}
void pc_gfx_reset_ui_state(void) {}
void pc_gfx_set_copy_clear(GXColor a0, u32 a1) { if (!qOn()) { rp_set_copy_clear(a0, a1); return; } QS_set_copy_clear* q = (QS_set_copy_clear*)qPush(Q_set_copy_clear, sizeof(QS_set_copy_clear)); q->a0 = a0; q->a1 = a1; qPublishIf(false); }
void pc_gfx_set_chan_ctrl(GXChannelID a0, GXBool a1, GXColorSrc a2, GXColorSrc a3, u32 a4, GXDiffuseFn a5, GXAttnFn a6) { if (!qOn()) { rp_set_chan_ctrl(a0, a1, a2, a3, a4, a5, a6); return; } QS_set_chan_ctrl* q = (QS_set_chan_ctrl*)qPush(Q_set_chan_ctrl, sizeof(QS_set_chan_ctrl)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4; q->a5 = a5; q->a6 = a6; qPublishIf(false); }
void pc_gfx_set_chan_mat_color(GXChannelID a0, GXColor a1) { if (!qOn()) { rp_set_chan_mat_color(a0, a1); return; } QS_set_chan_mat_color* q = (QS_set_chan_mat_color*)qPush(Q_set_chan_mat_color, sizeof(QS_set_chan_mat_color)); q->a0 = a0; q->a1 = a1; qPublishIf(false); }
void pc_gfx_set_tev_order(GXTevStageID a0, GXTexCoordID a1, GXTexMapID a2, GXChannelID a3) { if (!qOn()) { rp_set_tev_order(a0, a1, a2, a3); return; } QS_set_tev_order* q = (QS_set_tev_order*)qPush(Q_set_tev_order, sizeof(QS_set_tev_order)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; qPublishIf(false); }
void pc_gfx_load_tex_obj(GXTexObj* a0, GXTexMapID a1) { if (!qOn()) { rp_load_tex_obj(a0, a1); return; } QS_load_tex_obj* q = (QS_load_tex_obj*)qPush(Q_load_tex_obj, sizeof(QS_load_tex_obj)); memcpy(&q->a0, a0, sizeof q->a0); q->a1 = a1; qPublishIf(false); }
void pc_gfx_set_num_tev_stages(u8 a0) { if (!qOn()) { rp_set_num_tev_stages(a0); return; } QS_set_num_tev_stages* q = (QS_set_num_tev_stages*)qPush(Q_set_num_tev_stages, sizeof(QS_set_num_tev_stages)); q->a0 = a0; qPublishIf(false); }
void pc_gfx_set_tev_color_in(GXTevStageID a0, GXTevColorArg a1, GXTevColorArg a2, GXTevColorArg a3, GXTevColorArg a4) { if (!qOn()) { rp_set_tev_color_in(a0, a1, a2, a3, a4); return; } QS_set_tev_color_in* q = (QS_set_tev_color_in*)qPush(Q_set_tev_color_in, sizeof(QS_set_tev_color_in)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4; qPublishIf(false); }
void pc_gfx_set_tev_alpha_in(GXTevStageID a0, GXTevAlphaArg a1, GXTevAlphaArg a2, GXTevAlphaArg a3, GXTevAlphaArg a4) { if (!qOn()) { rp_set_tev_alpha_in(a0, a1, a2, a3, a4); return; } QS_set_tev_alpha_in* q = (QS_set_tev_alpha_in*)qPush(Q_set_tev_alpha_in, sizeof(QS_set_tev_alpha_in)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4; qPublishIf(false); }
void pc_gfx_set_tev_color_op(GXTevStageID a0, GXTevOp a1, GXTevBias a2, GXTevScale a3, GXBool a4, GXTevRegID a5) { if (!qOn()) { rp_set_tev_color_op(a0, a1, a2, a3, a4, a5); return; } QS_set_tev_color_op* q = (QS_set_tev_color_op*)qPush(Q_set_tev_color_op, sizeof(QS_set_tev_color_op)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4; q->a5 = a5; qPublishIf(false); }
void pc_gfx_set_tev_alpha_op(GXTevStageID a0, GXTevOp a1, GXTevBias a2, GXTevScale a3, GXBool a4, GXTevRegID a5) { if (!qOn()) { rp_set_tev_alpha_op(a0, a1, a2, a3, a4, a5); return; } QS_set_tev_alpha_op* q = (QS_set_tev_alpha_op*)qPush(Q_set_tev_alpha_op, sizeof(QS_set_tev_alpha_op)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4; q->a5 = a5; qPublishIf(false); }
void pc_gfx_set_tev_color(GXTevRegID a0, GXColor a1) { if (!qOn()) { rp_set_tev_color(a0, a1); return; } QS_set_tev_color* q = (QS_set_tev_color*)qPush(Q_set_tev_color, sizeof(QS_set_tev_color)); q->a0 = a0; q->a1 = a1; qPublishIf(false); }
void pc_gfx_set_tev_color_s10(GXTevRegID a0, GXColorS10 a1) { if (!qOn()) { rp_set_tev_color_s10(a0, a1); return; } QS_set_tev_color_s10* q = (QS_set_tev_color_s10*)qPush(Q_set_tev_color_s10, sizeof(QS_set_tev_color_s10)); q->a0 = a0; q->a1 = a1; qPublishIf(false); }
void pc_gfx_set_tev_kcolor(GXTevKColorID a0, GXColor a1) { if (!qOn()) { rp_set_tev_kcolor(a0, a1); return; } QS_set_tev_kcolor* q = (QS_set_tev_kcolor*)qPush(Q_set_tev_kcolor, sizeof(QS_set_tev_kcolor)); q->a0 = a0; q->a1 = a1; qPublishIf(false); }
void pc_gfx_set_tev_kcolor_sel(GXTevStageID a0, GXTevKColorSel a1) { if (!qOn()) { rp_set_tev_kcolor_sel(a0, a1); return; } QS_set_tev_kcolor_sel* q = (QS_set_tev_kcolor_sel*)qPush(Q_set_tev_kcolor_sel, sizeof(QS_set_tev_kcolor_sel)); q->a0 = a0; q->a1 = a1; qPublishIf(false); }
void pc_gfx_set_tev_kalpha_sel(GXTevStageID a0, GXTevKAlphaSel a1) { if (!qOn()) { rp_set_tev_kalpha_sel(a0, a1); return; } QS_set_tev_kalpha_sel* q = (QS_set_tev_kalpha_sel*)qPush(Q_set_tev_kalpha_sel, sizeof(QS_set_tev_kalpha_sel)); q->a0 = a0; q->a1 = a1; qPublishIf(false); }
/* r47: GX light objects. The game initialises a GXLightObj (64 bytes, opaque to it) with GXInitLight*, then loads it with GXLoadLightObjImm:
   the record lives inside the object, and loading copies it into the queue. Same conventions as the Dolphin SDK (GXInitLightDir stores the
   NEGATED direction; GXInitSpecularDir stores a far-away position and the half-angle vector). */
struct LightRec { float pos[3], dir[3], a[3], k[3]; uint8_t col[4]; };
static_assert(sizeof(LightRec) <= 64, "GXLightObj (64 bytes) too small for the light record");
static LightRec* lrec(void* o) { return reinterpret_cast<LightRec*>(o); }
void pc_gfx_init_light_pos(void* o, f32 x, f32 y, f32 z) { LightRec* l = lrec(o); l->pos[0] = x; l->pos[1] = y; l->pos[2] = z; }
void pc_gfx_init_light_dir(void* o, f32 x, f32 y, f32 z) { LightRec* l = lrec(o); l->dir[0] = -x; l->dir[1] = -y; l->dir[2] = -z; }
void pc_gfx_init_specular_dir(void* o, f32 nx, f32 ny, f32 nz) {
    LightRec* l = lrec(o);
    float hx = -nx, hy = -ny, hz = -nz + 1.0f; const float m2 = hx * hx + hy * hy + hz * hz;
    if (m2 > 0.0f) { const float inv = 1.0f / sqrtf(m2); hx *= inv; hy *= inv; hz *= inv; }
    l->dir[0] = hx; l->dir[1] = hy; l->dir[2] = hz;
    l->pos[0] = nx * -1048576.0f; l->pos[1] = ny * -1048576.0f; l->pos[2] = nz * -1048576.0f; }
void pc_gfx_init_light_color(void* o, GXColor c) { LightRec* l = lrec(o); l->col[0] = c.r; l->col[1] = c.g; l->col[2] = c.b; l->col[3] = c.a; }
void pc_gfx_init_light_attn(void* o, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) { LightRec* l = lrec(o); l->a[0] = a0; l->a[1] = a1; l->a[2] = a2; l->k[0] = k0; l->k[1] = k1; l->k[2] = k2; }
void pc_gfx_init_light_attn_a(void* o, f32 a0, f32 a1, f32 a2) { LightRec* l = lrec(o); l->a[0] = a0; l->a[1] = a1; l->a[2] = a2; }
void pc_gfx_init_light_attn_k(void* o, f32 k0, f32 k1, f32 k2) { LightRec* l = lrec(o); l->k[0] = k0; l->k[1] = k1; l->k[2] = k2; }
void pc_gfx_load_light(void* o, u32 mask) {
    const LightRec* l = lrec(o); float v[16];
    for (int i = 0; i < 3; ++i) { v[i] = l->pos[i]; v[3 + i] = l->dir[i]; v[6 + i] = l->a[i]; v[9 + i] = l->k[i]; }
    for (int i = 0; i < 4; ++i) v[12 + i] = l->col[i] / 255.0f;
    if (!qOn()) { lightLoad(v, mask); return; }
    QS_load_light* q = (QS_load_light*)qPush(Q_load_light, sizeof(QS_load_light)); memcpy(q->v, v, sizeof v); q->mask = mask; qPublishIf(false); }
void pc_gfx_set_chan_amb_color(GXChannelID c, GXColor col) {
    if (!qOn()) { chanAmbSet((int)c, col); return; }
    QS_chan_amb* q = (QS_chan_amb*)qPush(Q_chan_amb, sizeof(QS_chan_amb)); q->c = (int)c; q->col = col; qPublishIf(false); }
void n3ds_gfx_tag(int t) { if (!qOn()) { sTag = t; return; } QS_tag* q = (QS_tag*)qPush(Q_tag, sizeof(QS_tag)); q->a0 = t; qPublishIf(false); }
void n3ds_gfx_bottom(int on) { if (!qOn()) { rp_bottom(on); return; } QS_bottom* q = (QS_bottom*)qPush(Q_bottom, sizeof(QS_bottom)); q->a0 = on; qPublishIf(false); }
void n3ds_gfx_lod_exempt(int on) { if (!qOn()) { rp_lod_exempt(on); return; } QS_lod_exempt* q = (QS_lod_exempt*)qPush(Q_lod_exempt, sizeof(QS_lod_exempt)); q->a0 = on; qPublishIf(false); }
void pc_gfx_perf_scope_begin(const char* a0) { if (!qOn()) { rp_perf_scope_begin(a0); return; } QS_perf_scope_begin* q = (QS_perf_scope_begin*)qPush(Q_perf_scope_begin, sizeof(QS_perf_scope_begin)); q->a0 = a0; qPublishIf(false); }
void pc_gfx_perf_scope_end(void) { if (!qOn()) { rp_perf_scope_end(); return; } qPush(Q_perf_scope_end, sizeof(QS_perf_scope_end)); qPublishIf(false); }
void pc_gfx_clear_vtx_desc(void) { if (!qOn()) { rp_clear_vtx_desc(); return; } qPush(Q_clear_vtx_desc, sizeof(QS_clear_vtx_desc)); qPublishIf(false); }
void pc_gfx_set_vtx_desc(GXAttr a0, GXAttrType a1) { if (!qOn()) { rp_set_vtx_desc(a0, a1); return; } QS_set_vtx_desc* q = (QS_set_vtx_desc*)qPush(Q_set_vtx_desc, sizeof(QS_set_vtx_desc)); q->a0 = a0; q->a1 = a1; qPublishIf(false); }
void pc_gfx_set_vtx_attr_fmt(GXVtxFmt a0, GXAttr a1, GXCompCnt a2, GXCompType a3, u8 a4) { if (!qOn()) { rp_set_vtx_attr_fmt(a0, a1, a2, a3, a4); return; } QS_set_vtx_attr_fmt* q = (QS_set_vtx_attr_fmt*)qPush(Q_set_vtx_attr_fmt, sizeof(QS_set_vtx_attr_fmt)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4; qPublishIf(false); }
void pc_gfx_set_array(GXAttr a0, void* a1, u8 a2) { if (!qOn()) { rp_set_array(a0, a1, a2); return; } QS_set_array* q = (QS_set_array*)qPush(Q_set_array, sizeof(QS_set_array)); q->a0 = a0; q->a1 = a1; q->a2 = a2; qPublishIf(false); }
void pc_gfx_begin(GXPrimitive a0, GXVtxFmt a1, u16 a2) { if (!qOn()) { rp_begin(a0, a1, a2); return; } QS_begin* q = (QS_begin*)qPush(Q_begin, sizeof(QS_begin)); q->a0 = a0; q->a1 = a1; q->a2 = a2; qPublishIf(false); }
void pc_gfx_position(f32 a0, f32 a1, f32 a2) { if (!qOn()) { rp_position(a0, a1, a2); return; } QS_position* q = (QS_position*)qPush(Q_position, sizeof(QS_position)); q->a0 = a0; q->a1 = a1; q->a2 = a2; qPublishIf(false); }
void pc_gfx_color(u8 a0, u8 a1, u8 a2, u8 a3) { if (!qOn()) { rp_color(a0, a1, a2, a3); return; } QS_color* q = (QS_color*)qPush(Q_color, sizeof(QS_color)); q->a0 = a0; q->a1 = a1; q->a2 = a2; q->a3 = a3; qPublishIf(false); }
void pc_gfx_texcoord(f32 a0, f32 a1) { if (!qOn()) { rp_texcoord(a0, a1); return; } QS_texcoord* q = (QS_texcoord*)qPush(Q_texcoord, sizeof(QS_texcoord)); q->a0 = a0; q->a1 = a1; qPublishIf(false); }
void pc_gfx_end(void) { if (!qOn()) { rp_end(); return; } qPush(Q_end, sizeof(QS_end)); qPublishIf(true); }
/* 1p: DCFlushRange / DCStoreRange (os stubs) and heap resets drop GPU-transform meshes built from that memory. Callable from ANY thread:
   they only post into the invalidation mailbox (never into the single-producer GX queue). */
void pc_gfx_invalidate_cpu_range(const void* addr, size_t bytes)
{
    if (!addr || !bytes) return;
    invPost((uintptr_t)addr, (uintptr_t)addr + bytes);
}
void pc_gfx_invalidate_resident_meshes(void) { invPostAll(); }
void pc_gfx_call_display_list(const void* a0, u32 a1) { if (!qOn()) { rp_call_display_list(a0, a1); return; } QS_call_display_list* q = (QS_call_display_list*)qPush(Q_call_display_list, sizeof(QS_call_display_list)); q->a0 = a0; q->a1 = a1; qPublishIf(true); }

} // extern "C"
