// citro3d side of the graphics backend (libctru/citro3d only -- NEVER include game headers here, see n3ds_bridge.h).
//
// Frame model: the game draws between two pc_window_swap_buffers calls, so a citro3d frame is kept open all the time:
//   present(): flush ring -> C3D_FrameEnd -> flip ring -> C3D_FrameBegin(SYNCDRAW) -> clear -> DrawOn -> set fixed state.
// Vertices are produced on the CPU (n3ds_gfx.cpp) in PICA clip space and written straight into a linear-memory ring
// (two rings, one per frame in flight), so the vertex shader is a passthrough with no uniforms.
// citro3d is not thread-safe: every C3D_* call here happens on the game thread.
#include <3ds.h>
#include <citro3d.h>
#include <stddef.h>
#include <string.h>
#include "n3ds_bridge.h"
#include "n3ds_mesh_unif.h"
#include "n3ds_config.h"

#ifdef N3DS_HAVE_SHADER
extern "C" { extern const unsigned char passthrough_shbin[]; extern const unsigned int passthrough_shbin_size; }
#endif

/* GPU command buffer, from the linear heap. Log "crash_dump_00000016": the render worker died in GPUCMD_Add -> svcBreak(PANIC) when the
   buffer filled up (C3D_DrawArrays -> C3Di_UpdateContext -> GPUCMD_Add). The default is only 256 KB, which a busy cutscene frame
   overflows. libctru PANICS on overflow, so n3ds_gpu_commit also drops draws once the buffer is nearly full instead of crashing. */
#ifndef N3DS_CMDBUF_KB
#define N3DS_CMDBUF_KB 2048
#endif
#ifndef N3DS_CMDBUF_MIN_WORDS
#define N3DS_CMDBUF_MIN_WORDS 8192   /* also drop when fewer than this many 32-bit words are left (one draw plus its state updates is far below it) */
#endif
#ifndef N3DS_CMDBUF_DROP_AT
#define N3DS_CMDBUF_DROP_AT 0.93f   /* fraction of the buffer used above which draws are dropped (one draw adds well under 5%) */
#endif

#ifndef N3DS_RING_VERTS
#define N3DS_RING_VERTS 131072   /* 1j: was 98304 (log 1i: the title scene filled it every frame). 131072 * 44 B = 5.5 MB per ring */
#endif
#define RING_VERTS N3DS_RING_VERTS   /* 2 rings from the linear heap; textures (stage 3) will need the rest, so do not grow this blindly */

static C3D_RenderTarget* sTop = NULL;
// r19: bottom-screen target (the radar map). 320x240 upright, created 240 wide x 320 tall like the top one; its display transfer writes RGB565
// straight into the bottom framebuffer the log console uses (single-buffered), and only on frames that actually drew on it (citro3d skips unused targets).
static C3D_RenderTarget* sBottom = NULL;
static bool sOnBottom = false, sBottomCleared = false;
static unsigned sBotFrames = 0, sBotDraws = 0, sBotFramesTot = 0;   // r21: frames / draws that went to the bottom screen (log line every 300 frames)
static int sScrW = 400;           // width of the upright screen being drawn on (rectToBuffer / scissor clamp)
extern "C" volatile int gN3dsBottomOk;
volatile int gN3dsBottomOk = 0;   // 1 once the bottom target exists (the game only records map draws then)
static bool sTried = false, sOk = false, sInFrame = false, sShader = false;
static u32 sClear = 0x101828FF; // 0xRRGGBBAA
static float sTilt[4] = {0, 1, -1, 0};

static N3dsVtx* sRing[2] = {NULL, NULL};
static C3D_BufInfo sBuf[2];
static C3D_AttrInfo sAttr;
// GPU-transform mesh path (1p): its own attribute layout; the shader is the same program (bool uniform 'xform' picks the branch)
static C3D_AttrInfo sAttrMesh;
static C3D_BufInfo sBufMesh;
static bool sMeshOk = false, sMeshMode = false;
static int sLocProj = -1, sLocPos = -1, sLocNrm = -1, sLocTcu = -1, sLocCw = -1, sLocMc = -1, sLocXform = -1, sLocLit = -1, sLocMulti = -1;
/* r53: GameCube lighting uniforms + bools of the mesh branch (passthrough.v.pica) */
static int sLocAmb = -1, sLocLP = -1, sLocLC = -1, sLocLF = -1, sLocLA = -1, sLocLK = -1, sLocLD = -1;
static int sLocHead = -1, sLocL[2] = {-1, -1}, sLocA[2] = {-1, -1}, sLocS[2] = {-1, -1};
static signed char sBoolShadow[16];      /* last value sent per bool register (-1 unknown), so an unchanged bool is not re-sent */
static int sCullShadow = -1;             /* r53: last C3D_CullFace sent (mesh draws set it, ring draws are culled by the CPU and need NONE) */
static float sUShadow[96][4];                 // last value written to each float uniform register (NaN = unknown)
static bool sLitShadow = false, sLitKnown = false;
static unsigned sMeshDraws = 0, sMeshRuns = 0, sMeshIdx = 0, sMeshDropped = 0;
static shaderProgram_s sProg;
static DVLB_s* sDvlb = NULL;
static int sRingIdx = 0, sUsed = 0, sResStart = 0;
static int sFlushedTo = 0, sSplits = 0, sLastSplits = 0;      // gpu_split: ring vertices already flushed to memory the GPU reads, command-list pieces handed over this frame
extern "C" void C3D_FrameSplit(u8 flags) __attribute__((weak));   // older citro3d: no splitting, the option is simply ignored
static unsigned sDraws = 0, sVerts = 0, sDropped = 0, sFrameNo = 0;
/* How full the GPU command buffer is RIGHT NOW. C3D_GetCmdBufUsage() only updates when a frame ends, so it read 0% for the whole frame and
   the guard never fired (crash_dump_00000019: GPUCMD_Add -> svcBreak again, with a 1 MB buffer). libctru's own write offset is exact. */
static float cmdFill(u32* wordsLeft)
{
    u32* addr = NULL; u32 size = 0, off = 0;
    GPUCMD_GetBuffer(&addr, &size, &off);
    if (wordsLeft) *wordsLeft = size > off ? size - off : 0;
    return size ? (float)off / (float)size : 0.0f;
}
static unsigned sCmdDrops = 0, sCmdDropsFrame = 0;   /* draws skipped because the GPU command buffer was nearly full */
static float sCmdPeak = 0.0f;                        /* highest command-buffer fill seen (0..1) */
static N3dsDrawState sApplied;
static bool sHaveApplied = false, sScissorEmpty = false;
// 1j perf accounting (ticks of svcGetSystemTick, summed over the frames since the last [gpu-perf] line)
static unsigned long long sTFlush = 0, sTEnd = 0, sTBegin = 0, sTPresent = 0;
static unsigned sPerfFrames = 0;

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define BOTTOM_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

extern "C" void n3ds_gpu_set_clear(unsigned r, unsigned g, unsigned b)
{
    sClear = ((r & 255u) << 24) | ((g & 255u) << 16) | ((b & 255u) << 8) | 0xFFu;
}

extern "C" int n3ds_gpu_tilt(float out[4])
{
    if (!sOk) return 0;
    for (int i = 0; i < 4; ++i) out[i] = sTilt[i];
    return 1;
}

static bool gpuInit(void)
{
    if (!C3D_Init((size_t)N3DS_CMDBUF_KB * 1024)) { n3ds_log("[gpu] C3D_Init failed (linear heap?)"); return false; }
    // Top screen is 400x240 but the LCD is rotated: the target is created 240 wide x 400 tall (as in the citro3d samples).
    sTop = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!sTop) { n3ds_log("[gpu] C3D_RenderTargetCreate failed (VRAM?)"); C3D_Fini(); return false; }
    C3D_RenderTargetSetOutput(sTop, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    sBottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (sBottom) {
        C3D_RenderTargetSetOutput(sBottom, GFX_BOTTOM, GFX_LEFT, BOTTOM_TRANSFER_FLAGS);
        gN3dsBottomOk = 1;
    }
    n3ds_log("[gpu] bottom-screen target (map) %s", sBottom ? "ready 320x240 -> RGB565" : "NOT created (VRAM?) - bottom screen stays the log");

    // The LCD rotation, taken from citro3d itself: an ortho over [-1,1] carries only the x/y tilt (unit coefficients).
    C3D_Mtx t;
    Mtx_OrthoTilt(&t, -1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, true);
    sTilt[0] = t.r[0].x; sTilt[1] = t.r[0].y; sTilt[2] = t.r[1].x; sTilt[3] = t.r[1].y;
    n3ds_log("[gpu] tilt X'=%.2fx%+.2fy Y'=%.2fx%+.2fy", sTilt[0], sTilt[1], sTilt[2], sTilt[3]);

#ifdef N3DS_HAVE_SHADER
    sDvlb = DVLB_ParseFile((u32*)(void*)passthrough_shbin, passthrough_shbin_size);
    if (sDvlb) {
        shaderProgramInit(&sProg);
        shaderProgramSetVsh(&sProg, &sDvlb->DVLE[0]);
        sShader = true;
    } else n3ds_log("[gpu] DVLB_ParseFile failed: shader not usable, clear only");
#else
    n3ds_log("[gpu] built without picasso shader: clear only");
#endif
    for (int i = 0; i < 2; ++i) {
        sRing[i] = (N3dsVtx*)linearAlloc((size_t)RING_VERTS * sizeof(N3dsVtx));
        if (!sRing[i]) { n3ds_log("[gpu] linearAlloc ring %d failed (%lu KB linear free)", i, (unsigned long)(linearSpaceFree() / 1024)); sShader = false; break; }
        BufInfo_Init(&sBuf[i]);
        BufInfo_Add(&sBuf[i], sRing[i], sizeof(N3dsVtx), 5, 0x43210);
    }
    AttrInfo_Init(&sAttr);
    AttrInfo_AddLoader(&sAttr, 0, GPU_FLOAT, 4);          // clip position
    AttrInfo_AddLoader(&sAttr, 1, GPU_UNSIGNED_BYTE, 4);  // colour
    AttrInfo_AddLoader(&sAttr, 2, GPU_FLOAT, 2);          // texcoord, texture unit 0
    AttrInfo_AddLoader(&sAttr, 3, GPU_FLOAT, 2);          // texcoord, texture unit 1 (multi-texture combiners)
    AttrInfo_AddLoader(&sAttr, 4, GPU_FLOAT, 2);          // texcoord, texture unit 2
    // mesh layout (see N3dsMeshVtx): (x,y,z,slot) float4, colour ubyte4, texcoord sets 0 and 1 float2, normal float4 - the formats the ring uses
    AttrInfo_Init(&sAttrMesh);
    AttrInfo_AddLoader(&sAttrMesh, 0, GPU_FLOAT, 4);          // x, y, z, matrix slot
    AttrInfo_AddLoader(&sAttrMesh, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&sAttrMesh, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&sAttrMesh, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&sAttrMesh, 4, GPU_FLOAT, 4);          // normal (nx, ny, nz, 0)
    memset(sUShadow, 0xFF, sizeof sUShadow);
    memset(sBoolShadow, -1, sizeof sBoolShadow);
    if (sShader) {
        shaderInstance_s* vi = sProg.vertexShader;
        auto L = [&](const char* n) { return shaderInstanceGetUniformLocation(vi, n); };
        sLocProj = L("F"); sLocPos = L("P"); sLocNrm = L("N"); sLocTcu = L("tcu"); sLocCw = L("cw"); sLocMc = L("mc");
        sLocAmb = L("AMB"); sLocLP = L("LP"); sLocLC = L("LC"); sLocLF = L("LF"); sLocLA = L("LA"); sLocLK = L("LK"); sLocLD = L("LD");
        sLocXform = L("xform"); sLocLit = L("lit"); sLocHead = L("head"); sLocMulti = L("multi");
        sLocL[0] = L("l0"); sLocL[1] = L("l1"); sLocA[0] = L("a0"); sLocA[1] = L("a1"); sLocS[0] = L("s0"); sLocS[1] = L("s1");
        const int fl[] = { sLocProj, sLocPos, sLocNrm, sLocTcu, sLocCw, sLocMc, sLocAmb, sLocLP, sLocLC, sLocLF, sLocLA, sLocLK, sLocLD };
        const int bl[] = { sLocXform, sLocLit, sLocHead, sLocMulti, sLocL[0], sLocL[1], sLocA[0], sLocA[1], sLocS[0], sLocS[1] };
        sMeshOk = true;
        for (int v : fl) if (v < 0) sMeshOk = false;
        for (int v : bl) if (v < 0) sMeshOk = false;
        if (sMeshOk && (sLocProj + 4 > 96 || sLocPos + 30 > 96 || sLocNrm + 30 > 96 || sLocTcu + 8 > 96 || sLocLD + 2 > 96)) sMeshOk = false;
        n3ds_log("[gpu] mesh path %s | uniform registers: F %d P %d N %d tcu %d cw %d mc %d AMB %d LP %d LC %d LF %d LA %d LK %d LD %d | bools: xform %d lit %d head %d multi %d",
                 sMeshOk ? "ready (GPU transform + GameCube lighting, r53)" : "UNAVAILABLE (shader uniforms not found)",
                 sLocProj, sLocPos, sLocNrm, sLocTcu, sLocCw, sLocMc, sLocAmb, sLocLP, sLocLC, sLocLF, sLocLA, sLocLK, sLocLD, sLocXform, sLocLit, sLocHead, sLocMulti);
    }
    n3ds_log("[gpu] n3ds_gpu.cpp built %s %s | cmdbuf %d KB, live-fill guard at %d%% / %d words left", __DATE__, __TIME__, N3DS_CMDBUF_KB, (int)(N3DS_CMDBUF_DROP_AT * 100), N3DS_CMDBUF_MIN_WORDS);
    n3ds_log("[gpu] citro3d up: top target 400x240 RGBA8 + D24S8 | shader %s | rings 2x%d verts | linear free %lu KB",
             sShader ? "ok" : "OFF", RING_VERTS, (unsigned long)(linearSpaceFree() / 1024));
    return true;
}

// ---- screen rect (upright 400x240, top-left origin) -> render-target pixels, through the tilt ------------------------
static void rectToBuffer(short x0, short y0, short x1, short y1, int* bx0, int* by0, int* bx1, int* by1)
{
    float xs[2] = {(float)x0, (float)x1}, ys[2] = {(float)y0, (float)y1};
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) {
        float n = 2.0f * xs[i] / (float)sScrW - 1.0f, m = 1.0f - 2.0f * ys[j] / 240.0f;   // upright NDC (r19: 400 top / 320 bottom)
        float X = sTilt[0] * n + sTilt[1] * m, Y = sTilt[2] * n + sTilt[3] * m;      // PICA NDC
        float bx = (X + 1.0f) * 0.5f * 240.0f, by = (Y + 1.0f) * 0.5f * (float)sScrW; // buffer pixels
        if (bx < minX) minX = bx; if (bx > maxX) maxX = bx;
        if (by < minY) minY = by; if (by > maxY) maxY = by;
    }
    *bx0 = (int)(minX + 0.5f); *by0 = (int)(minY + 0.5f); *bx1 = (int)(maxX + 0.5f); *by1 = (int)(maxY + 0.5f);
}

static GPU_TESTFUNC mapDepth(int gx)   // depth is inverted on the PICA (near = larger), so LESS <-> GREATER
{
    switch (gx) {
        case 0: return GPU_NEVER;  case 1: return GPU_GREATER; case 2: return GPU_EQUAL;   case 3: return GPU_GEQUAL;
        case 4: return GPU_LESS;   case 5: return GPU_NOTEQUAL; case 6: return GPU_LEQUAL; default: return GPU_ALWAYS;
    }
}
static GPU_TESTFUNC mapAlpha(int gx)
{
    switch (gx) {
        case 0: return GPU_NEVER;  case 1: return GPU_LESS;    case 2: return GPU_EQUAL;   case 3: return GPU_LEQUAL;
        case 4: return GPU_GREATER; case 5: return GPU_NOTEQUAL; case 6: return GPU_GEQUAL; default: return GPU_ALWAYS;
    }
}
static GPU_BLENDFACTOR mapBF(int f, bool isSrc)   // GX value 2/3 means "the other colour": DST for a source factor, SRC for a dest factor
{
    switch (f) {
        case 0: return GPU_ZERO;
        case 1: return GPU_ONE;
        case 2: return isSrc ? GPU_DST_COLOR : GPU_SRC_COLOR;
        case 3: return isSrc ? GPU_ONE_MINUS_DST_COLOR : GPU_ONE_MINUS_SRC_COLOR;
        case 4: return GPU_SRC_ALPHA;
        case 5: return GPU_ONE_MINUS_SRC_ALPHA;
        case 6: return GPU_DST_ALPHA;
        default: return GPU_ONE_MINUS_DST_ALPHA;
    }
}

// ---- textures (1k) ---------------------------------------------------------------------------------------------------
struct TexSlot { C3D_Tex tex; bool used; };
static TexSlot sTex[1024];   // id = index + 1

static GPU_TEXCOLOR mapTexFmt(int f)
{
    switch (f) {
        case N3DS_TF_RGBA8: return GPU_RGBA8;     case N3DS_TF_RGB565: return GPU_RGB565; case N3DS_TF_RGBA5551: return GPU_RGBA5551;
        case N3DS_TF_RGBA4: return GPU_RGBA4;     case N3DS_TF_LA8: return GPU_LA8;       default: return GPU_L8;
    }
}
static unsigned texBytes(int w, int h, int f) { return (unsigned)w * h * (f == N3DS_TF_RGBA8 ? 4 : f == N3DS_TF_L8 ? 1 : 2); }

extern "C" int n3ds_gpu_tex_create(int w, int h, int fmt, void** data, unsigned* bytes)
{
    if (!sOk || w < 8 || h < 8 || w > 1024 || h > 1024) return 0;
    int i = 0;
    for (; i < 1024; ++i) if (!sTex[i].used) break;
    if (i == 1024) return 0;
    if (!C3D_TexInit(&sTex[i].tex, (u16)w, (u16)h, mapTexFmt(fmt))) return 0;        // linearAlloc inside; fails when linear memory is gone
    C3D_TexSetFilter(&sTex[i].tex, GPU_LINEAR, GPU_LINEAR);
    sTex[i].used = true;
    *data = sTex[i].tex.data; *bytes = texBytes(w, h, fmt);
    return i + 1;
}
extern "C" void n3ds_gpu_tex_flush(int id) { if (id > 0 && id <= 1024 && sTex[id - 1].used) C3D_TexFlush(&sTex[id - 1].tex); }
extern "C" void n3ds_gpu_tex_destroy(int id)
{
    if (id <= 0 || id > 1024 || !sTex[id - 1].used) return;
    C3D_TexDelete(&sTex[id - 1].tex);
    sTex[id - 1].used = false;
}
extern "C" unsigned n3ds_gpu_tex_free_kb(void) { return (unsigned)(linearSpaceFree() / 1024); }

static GPU_TEXTURE_WRAP_PARAM mapWrap(int w) { return w == 1 ? GPU_REPEAT : w == 2 ? GPU_MIRRORED_REPEAT : GPU_CLAMP_TO_EDGE; }
static GPU_TEVSRC mapSrc(int s) { switch (s) { case N3DS_TS_BUFFER: return GPU_PREVIOUS_BUFFER; case N3DS_TS_TEX0: return GPU_TEXTURE0; case N3DS_TS_TEX1: return GPU_TEXTURE1; case N3DS_TS_TEX2: return GPU_TEXTURE2; case N3DS_TS_CONST: return GPU_CONSTANT; case N3DS_TS_PREV: return GPU_PREVIOUS; default: return GPU_PRIMARY_COLOR; } }
static GPU_TEVOP_RGB mapOpRgb(int o) { switch (o) { case N3DS_TO_1MC: return GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR; case N3DS_TO_A: return GPU_TEVOP_RGB_SRC_ALPHA; case N3DS_TO_1MA: return GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA; default: return GPU_TEVOP_RGB_SRC_COLOR; } }
static GPU_TEVOP_A mapOpA(int o) { return (o == N3DS_TO_1MA || o == N3DS_TO_1MC) ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_SRC_ALPHA; }
static GPU_COMBINEFUNC mapFn(int f)
{
    switch (f) {
        case N3DS_TFN_MODULATE: return GPU_MODULATE;  case N3DS_TFN_ADD: return GPU_ADD; case N3DS_TFN_INTERP: return GPU_INTERPOLATE;
        case N3DS_TFN_MULADD: return GPU_MULTIPLY_ADD; case N3DS_TFN_SUB: return GPU_SUBTRACT; default: return GPU_REPLACE;
    }
}
static GPU_TEVSCALE mapScale(int s) { return s == 4 ? GPU_TEVSCALE_4 : s == 2 ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1; }

static void programTev(int stage, const N3dsTevStage* t)
{
    C3D_TexEnv* env = C3D_GetTexEnv(stage);
    C3D_TexEnvInit(env);
    if (!t) return;                                    // Init = REPLACE(previous): a pass-through stage
    C3D_TexEnvSrc(env, C3D_RGB, mapSrc(t->rgbSrc[0]), mapSrc(t->rgbSrc[1]), mapSrc(t->rgbSrc[2]));
    C3D_TexEnvSrc(env, C3D_Alpha, mapSrc(t->aSrc[0]), mapSrc(t->aSrc[1]), mapSrc(t->aSrc[2]));
    C3D_TexEnvOpRgb(env, mapOpRgb(t->rgbOp[0]), mapOpRgb(t->rgbOp[1]), mapOpRgb(t->rgbOp[2]));
    C3D_TexEnvOpAlpha(env, mapOpA(t->aOp[0]), mapOpA(t->aOp[1]), mapOpA(t->aOp[2]));
    C3D_TexEnvFunc(env, C3D_RGB, mapFn(t->rgbFn));
    C3D_TexEnvFunc(env, C3D_Alpha, mapFn(t->aFn));
    C3D_TexEnvScale(env, C3D_RGB, mapScale(t->rgbScale));
    C3D_TexEnvScale(env, C3D_Alpha, mapScale(t->aScale));
    C3D_TexEnvColor(env, t->color);                    // 0xAABBGGRR == the PICA constant-colour register layout
}

static void applyState(const N3dsDrawState* s)
{
    const N3dsDrawState& a = sApplied;
    const bool f = !sHaveApplied;
    if (f || s->depthCmp != a.depthCmp || s->writeMask != a.writeMask) {
        u8 wm = 0;
        if (s->writeMask & 1) wm |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
        if (s->writeMask & 2) wm |= GPU_WRITE_ALPHA;
        if (s->writeMask & 4) wm |= GPU_WRITE_DEPTH;
        C3D_DepthTest(true, mapDepth(s->depthCmp), (GPU_WRITEMASK)wm);   // "compare off" arrives as ALWAYS so z writes still work
    }
    if (f || s->depthBias != a.depthBias)                 // r46: depth = -z/w (+ 1/16384 = nearer for depth-tested, unwritten perspective surfaces)
        C3D_DepthMap(true, -1.0f, s->depthBias ? 1.0f / 16384.0f : 0.0f);
    if (f || s->blendMode != a.blendMode || s->blendSrc != a.blendSrc || s->blendDst != a.blendDst) {
        if (s->blendMode == 1) {
            GPU_BLENDFACTOR sf = mapBF(s->blendSrc, true), df = mapBF(s->blendDst, false);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, sf, df, sf, df);
        } else if (s->blendMode == 2) {
            C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
        } else {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        }
    }
    if (f || s->alphaCmp != a.alphaCmp || s->alphaRef != a.alphaRef)
        C3D_AlphaTest(s->alphaCmp != 7, mapAlpha(s->alphaCmp), s->alphaRef);
    // 1j FIX: citro3d's C3D_SetViewport also sets the scissor to the viewport rect. Applying the viewport alone therefore
    // silently replaced the game's scissor, so the scissor is re-applied whenever the viewport was.
    const bool vpChanged = f || memcmp(&s->vpX0, &a.vpX0, 4 * sizeof(short)) != 0;
    if (vpChanged) {
        int x0, y0, x1, y1; rectToBuffer(s->vpX0, s->vpY0, s->vpX1, s->vpY1, &x0, &y0, &x1, &y1);
        if (x1 > x0 && y1 > y0) C3D_SetViewport(x0, y0, x1 - x0, y1 - y0);
    }
    if (vpChanged || memcmp(&s->scX0, &a.scX0, 4 * sizeof(short)) != 0) {
        int x0, y0, x1, y1; rectToBuffer(s->scX0, s->scY0, s->scX1, s->scY1, &x0, &y0, &x1, &y1);
        // ALWAYS inside the 240x400 render target: the scissor is the only thing that keeps the rasterizer's writes inside our buffer. The
        // game's rect can reach past the screen (rectToBuffer does not clamp), and a garbage triangle under an oversized scissor can write
        // past the framebuffer into other VRAM - suspected cause of the HOME menu's garbled text after gpu_xform = 1 sessions.
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > 240) x1 = 240; if (y1 > sScrW) y1 = sScrW;
        // 1j FIX: an empty rect must not reach citro3d (it programs right-1 / bottom-1, which wraps to 0xFFFF for 0 and
        // would turn an "empty" scissor into "everything"). Draws under an empty scissor are skipped instead.
        sScissorEmpty = !(x1 > x0 && y1 > y0);
        if (!sScissorEmpty) C3D_SetScissor(GPU_SCISSOR_NORMAL, x0, y0, x1, y1);
    }
    // 1k: combiner stages (only the ones that differ; stages the new program does not use are reset to pass-through)
    const int nOld = f ? 6 : a.nTev, nNew = s->nTev;
    if (f || nNew != a.nTev || memcmp(s->tev, a.tev, sizeof(N3dsTevStage) * (nNew > nOld ? nNew : nOld)) != 0) {
        const int top = nNew > nOld ? nNew : nOld;
        for (int i = 0; i < top && i < 6; ++i) {
            const bool same = !f && i < nOld && i < nNew && memcmp(&s->tev[i], &a.tev[i], sizeof(N3dsTevStage)) == 0;
            if (!same) programTev(i, i < nNew ? &s->tev[i] : NULL);
        }
        if (nNew == 0) { programTev(0, NULL); }
    }
    if (f || s->tevBufRgb != a.tevBufRgb || s->tevBufA != a.tevBufA) {   // r48: TEV register outputs live in the combiner buffer
        C3D_TexEnvBufUpdate(C3D_RGB, s->tevBufRgb & 0xF);
        C3D_TexEnvBufUpdate(C3D_Alpha, s->tevBufA & 0xF);
    }
    // EVERY texture unit always has a valid texture bound (ac-3ds port, gx_bind.c: "once the vertex shader outputs texcoords, the real GPU hangs
    // on a unit never given one"; emulators do not hang). A unit with no real texture is parked on a tiny 8x8 texture the combiner does not read.
    // NEVER C3D_TexBind(unit > 0, NULL): citro3d reads tex->flags for units above 0 and faults on NULL (Luma crash dump 26).
    static C3D_Tex sDummyTex; static int sDummyState = 0;   // 0 untried, 1 ok, -1 failed
    if (sDummyState == 0) {
        if (C3D_TexInit(&sDummyTex, 8, 8, GPU_RGBA8)) { memset(sDummyTex.data, 0xFF, 8 * 8 * 4); C3D_TexFlush(&sDummyTex); sDummyState = 1; }
        else { sDummyState = -1; n3ds_log("[gpu] could not allocate the parking texture for unused texture units"); }
    }
    auto bindUnit = [&](int unit, unsigned id, unsigned char wrapS, unsigned char wrapT) {
        if (id > 0 && id <= 1024 && sTex[id - 1].used) {
            C3D_Tex* t = &sTex[id - 1].tex;
            C3D_TexSetWrap(t, mapWrap(wrapS), mapWrap(wrapT));
            C3D_TexBind(unit, t);
            return;
        }
        if (sDummyState == 1) C3D_TexBind(unit, &sDummyTex);
    };
    if (f || s->texId != a.texId || s->texWrapS != a.texWrapS || s->texWrapT != a.texWrapT) bindUnit(0, s->texId, s->texWrapS, s->texWrapT);
    if (f || s->tex1Id != a.tex1Id || s->tex1WrapS != a.tex1WrapS || s->tex1WrapT != a.tex1WrapT) bindUnit(1, s->tex1Id, s->tex1WrapS, s->tex1WrapT);
    if (f || s->tex2Id != a.tex2Id || s->tex2WrapS != a.tex2WrapS || s->tex2WrapT != a.tex2WrapT) bindUnit(2, s->tex2Id, s->tex2WrapS, s->tex2WrapT);
    sApplied = *s; sHaveApplied = true;
}

extern "C" int n3ds_gpu_ring_free(void)
{
    return (sInFrame && sShader) ? (RING_VERTS - sUsed) : 0;
}

extern "C" N3dsVtx* n3ds_gpu_reserve(int maxVerts)
{
    if (!sInFrame || !sShader) return NULL;
    if (sUsed + maxVerts > RING_VERTS) { ++sDropped; return NULL; }
    sResStart = sUsed;
    return sRing[sRingIdx] + sUsed;
}

/* gpu_split: the GPU only starts on the frame's command list at C3D_FrameEnd, so its 10-15 ms of rendering came AFTER the CPU's 80-90 ms (log 0e0cdb02:
   'FrameBegin wait' 10.8 ms). Handing over the list in pieces lets it render while the CPU keeps recording. The ring vertices a piece reads must be
   in memory first, so exactly those are flushed before each split.
   r54b: the GPU transform path draws from model buffers (flushed when built), not the ring, so with gpu_xform = 1 the ring rarely reached gpu_split
   vertices and the frame went to the GPU in ONE piece at the end ("0 command-list pieces"; log 25: 'FrameBegin wait' 20 ms of a 100 ms frame with
   40k triangles of lit models). Mesh indices now count too: a piece is handed over every 4 x gpu_split indices (24000 = 8000 triangles). */
static unsigned sMeshIdxSplit = 0;            /* mesh indices issued since the last split */
static void maybeSplit(void)
{
    const int every = n3dsConfig().gpuSplit;
    if (every <= 0 || !C3D_FrameSplit) return;
    if (sUsed - sFlushedTo < every && sMeshIdxSplit < (unsigned)every * 4u) return;
    if (sUsed > sFlushedTo) GSPGPU_FlushDataCache(sRing[sRingIdx] + sFlushedTo, (u32)(sUsed - sFlushedTo) * sizeof(N3dsVtx));
    sFlushedTo = sUsed; sMeshIdxSplit = 0;
    C3D_FrameSplit(0);
    ++sSplits;
}

static void leaveMeshMode(void)
{
    // back to the ring layout: attribute loaders, the ring buffer, and the shader branch that forwards CPU-transformed vertices
    C3D_SetAttrInfo(&sAttr);
    C3D_SetBufInfo(&sBuf[sRingIdx]);
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, sLocXform, false);
    if (sCullShadow != 0) { C3D_CullFace(GPU_CULL_NONE); sCullShadow = 0; }   /* r53: ring triangles are already culled by the CPU */
    sMeshMode = false;
}

static unsigned sUnifWrites = 0;          /* r54 [lite5]: registers actually written (profiled frames) */
static inline void setU(int reg, const float* v)
{
    /* r54: bitwise compare (4 integer compares, no VFP compare + FPSCR transfer per component). The shadow starts as NaN (never equal as
       floats); as bits it could only match a NaN uniform, and meshUniformsSane never lets one through. */
    float* sh = sUShadow[reg];
    u32 a[4], b[4]; memcpy(a, sh, sizeof a); memcpy(b, v, sizeof b);
    if (((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3])) == 0) return;
    memcpy(sh, v, 4 * sizeof(float));
    C3D_FVUnifSet(GPU_VERTEX_SHADER, reg, v[0], v[1], v[2], v[3]);
    ++sUnifWrites;
}

extern "C" int n3ds_gpu_mesh_ready(void) { return (sInFrame && sShader && sMeshOk) ? 1 : 0; }

/* mesh_defer = 1: GPU model draws are not issued where the game makes them but collected and issued in one batch. Reason: every earlier
   hardware test that drew ONE tiny real model in the middle of a frame corrupted the picture and the HOME menu, while the probes (drawn
   once, at the very end of the frame) were clean. The one thing only the mid-frame draws do is switching ring -> mesh -> ring state (attribute
   layout, vertex buffer, shader branch) between ordinary draws; a batch switches once per flush. A flush happens before any ring draw whose result
   depends on draw order against opaque geometry (blended, depth test off, no depth/colour write) and at the end of the frame. */
struct DeferredMesh {
    N3dsDrawState st; const N3dsMeshVtx* vtx; const unsigned short* idx; unsigned nVtx; unsigned runOff; int nRuns;
    N3dsMeshUniforms u;                  /* r53: the whole block (transform + lighting) */
};
enum { DEF_MAX = 256, DEF_RUNS = 6144 };
static DeferredMesh* sDef = NULL; static unsigned* sDefRuns = NULL;
static int sDefN = 0, sDefRunN = 0; static bool sDefTried = false;
static unsigned sDefFlushes = 0, sDefDraws = 0, sDefFull = 0;     // per [gpu] log interval

static void meshIssue(const N3dsDrawState* st, const N3dsMeshVtx* vtx, unsigned nVtx, const unsigned short* idx,
                      const unsigned* runs, int nRuns, const N3dsMeshUniforms* u);
extern "C" volatile int gN3dsGpuProf;      /* n3ds_gfx.cpp: 1 on the frames the [lite5] timers run */
static u64 sProfT[4]; static unsigned sProfC[3];

static void flushDeferred(void)
{
    if (!sDefN) return;
    for (int i = 0; i < sDefN; ++i) {
        const DeferredMesh& d = sDef[i];
        meshIssue(&d.st, d.vtx, d.nVtx, d.idx, sDefRuns + d.runOff, d.nRuns, &d.u);
    }
    sDefN = 0; sDefRunN = 0; ++sDefFlushes;
}

extern "C" void n3ds_gpu_draw_mesh(const N3dsDrawState* st, const N3dsMeshVtx* vtx, unsigned nVtx, const unsigned short* idx,
                                   const unsigned* runs, int nRuns, const N3dsMeshUniforms* u)
{
    if (!n3dsConfig().meshDefer || !sInFrame || !sShader || !sMeshOk || nRuns <= 0 || !vtx) { meshIssue(st, vtx, nVtx, idx, runs, nRuns, u); return; }
    if (!sDefTried) {
        sDefTried = true;
        sDef = (DeferredMesh*)linearAlloc(sizeof(DeferredMesh) * DEF_MAX);
        sDefRuns = (unsigned*)linearAlloc(sizeof(unsigned) * DEF_RUNS);
        if (!sDef || !sDefRuns) n3ds_log("[gpu] mesh_defer: could not allocate the draw queue - mesh draws are issued immediately");
        else n3ds_log("[gpu] mesh_defer: queue of %d draws / %d runs (%u KB linear)", (int)DEF_MAX, (int)DEF_RUNS, (unsigned)((sizeof(DeferredMesh) * DEF_MAX + sizeof(unsigned) * DEF_RUNS) / 1024));
    }
    if (!sDef || !sDefRuns) { meshIssue(st, vtx, nVtx, idx, runs, nRuns, u); return; }
    if (sDefN >= DEF_MAX || sDefRunN + 2 * nRuns > DEF_RUNS) { ++sDefFull; flushDeferred(); }
    if (2 * nRuns > DEF_RUNS) { meshIssue(st, vtx, nVtx, idx, runs, nRuns, u); return; }
    DeferredMesh& d = sDef[sDefN++];
    d.st = *st; d.vtx = vtx; d.idx = idx; d.nVtx = nVtx; d.nRuns = nRuns; d.runOff = (unsigned)sDefRunN;
    memcpy(sDefRuns + sDefRunN, runs, sizeof(unsigned) * 2 * nRuns); sDefRunN += 2 * nRuns;
    n3ds_mesh_unif_copy(&d.u, u);   /* r54: only what meshIssue reads (n3ds_mesh_unif.h) */
    ++sDefDraws;
}

static void meshIssue(const N3dsDrawState* st, const N3dsMeshVtx* vtx, unsigned nVtx, const unsigned short* idx,
                      const unsigned* runs, int nRuns, const N3dsMeshUniforms* u)
{
    (void)nVtx;
    if (!sInFrame || !sShader || !sMeshOk || nRuns <= 0 || !vtx) return;     // idx == NULL: non-indexed triangle list (mesh_safe)
    u32 wordsLeft = 0;
    const float fill = cmdFill(&wordsLeft);
    if (fill > sCmdPeak) sCmdPeak = fill;
    if (fill > N3DS_CMDBUF_DROP_AT || wordsLeft < N3DS_CMDBUF_MIN_WORDS + 1024) { ++sCmdDrops; ++sCmdDropsFrame; ++sMeshDropped; return; }
    {   /* the PICA addresses vertex and index buffers as 28-bit offsets from 0x18000000; anything beyond would be fetched from the wrong
           place (garbage indices / positions can hang the GPU). Never seen, but checked because a GPU hang takes the whole console down. */
        const u32 base = 0x18000000u, pv = osConvertVirtToPhys(vtx), pi = idx ? osConvertVirtToPhys(idx) : pv;
        if (pv < base || pi < base || pv - base >= 0x0FF00000u || pi - base >= 0x0FF00000u) {
            static int nLog = 0;
            if (nLog < 4) { ++nLog; n3ds_log("[gpu] mesh draw SKIPPED: buffers outside the PICA's reach (vertices at phys %08lx, indices at %08lx)", (unsigned long)pv, (unsigned long)pi); }
            ++sMeshDropped; return;
        }
    }
    if (n3dsConfig().meshLimit > 0 && (int)sMeshDraws >= n3dsConfig().meshLimit) return;     // diagnostics (mesh_limit)
    const bool prof = gN3dsGpuProf != 0;
    const u64 p0 = prof ? svcGetSystemTick() : 0;
    applyState(st);
    if (sScissorEmpty) return;
    const u64 p1 = prof ? svcGetSystemTick() : 0;
    const unsigned w0 = sUnifWrites;
    if (!sMeshMode) { C3D_SetAttrInfo(&sAttrMesh); C3D_BoolUnifSet(GPU_VERTEX_SHADER, sLocXform, true); sMeshMode = true; }
    BufInfo_Init(&sBufMesh);
    /* r53: 5 loaders cover 52 of the 60-byte vertex (u2/v2 are CPU-path only). A trailing padding entry (0xD = 8 bytes) makes the
       permutation cover the whole stride: sms-3ds found that the New 3DS GPU LOCKS UP on a draw whose loaders cover less than the stride
       ("49 of its 60-byte vertices"), which Azahar and our PICA interpreter never show. */
    static_assert(sizeof(N3dsMeshVtx) == 60, "mesh vertex layout changed: update the padding entry");
    BufInfo_Add(&sBufMesh, vtx, sizeof(N3dsMeshVtx), 6, 0xD43210);
    C3D_SetBufInfo(&sBufMesh);
    const int ns = u->nSlots < 1 ? 1 : u->nSlots > N3DS_MESH_SLOTS ? N3DS_MESH_SLOTS : u->nSlots;
    N3dsMeshUniforms nlU;
    if (n3dsConfig().meshNoLit && (u->lit || u->head)) { nlU = *u; nlU.lit = 0; nlU.head = 0; u = &nlU; }   // diagnostics (mesh_nolit)
    N3dsMeshUniforms dbgU;
    if (n3dsConfig().meshDebug == 3 && ns == 1) {
        /* Bisect (mesh_debug = 3): ignore the game's matrices and put model x/y at a small fixed scale around the screen centre (tilt
           applied), z fixed. Every model then shows up as a small, recognisable silhouette stacked at the centre if vertex FETCH and the
           shader are fine; garbage again = the fetch/attribute side is broken, not the matrices. */
        dbgU = *u;
        memset(dbgU.clip[0], 0, sizeof dbgU.clip[0]);
        const float s = 0.004f;
        dbgU.clip[0][0] = sTilt[0] * s; dbgU.clip[0][1] = sTilt[1] * s;
        dbgU.clip[0][4] = sTilt[2] * s; dbgU.clip[0][5] = sTilt[3] * s;
        dbgU.clip[0][11] = -0.5f; dbgU.clip[0][15] = 1.0f;
        dbgU.clipOnly = 1;
        u = &dbgU;
    }
    /* r53: transform + lighting uniforms (passthrough.v.pica). clipOnly (debug probes): F = clip[0], P = identity, no lighting. */
    static const float kIdRows[3][4] = { {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0} };
    const bool clipOnly = u->clipOnly != 0;
    const float* const Fm = clipOnly ? u->clip[0] : u->proj;
    for (int r = 0; r < 4; ++r) setU(sLocProj + r, Fm + r * 4);
    const float* const P0 = clipOnly ? kIdRows[0] : u->pos[0];       /* kIdRows: 3 contiguous rows of 4; pos[]: 12 floats per slot */
    const int pStride = clipOnly ? 0 : 12;
    for (int sl = 0; sl < ns; ++sl)
        for (int r = 0; r < 3; ++r) setU(sLocPos + sl * 3 + r, P0 + sl * pStride + r * 4);
    const bool lit = !clipOnly && u->lit, head = !clipOnly && !u->lit && u->head;
    if (lit || head)
        for (int sl = 0; sl < ns; ++sl)
            for (int r = 0; r < 3; ++r) { const float row[4] = {u->nrm[sl][r * 4], u->nrm[sl][r * 4 + 1], u->nrm[sl][r * 4 + 2], 0.0f}; setU(sLocNrm + sl * 3 + r, row); }
    for (int i = 0; i < 8; ++i) setU(sLocTcu + i, u->tcu[i]);
    setU(sLocCw, u->cw); setU(sLocMc, u->mc);
    if (lit) {
        setU(sLocAmb, u->amb);
        for (int k = 0; k < u->nLights && k < 2; ++k) {
            setU(sLocLP + k, u->lp[k]); setU(sLocLC + k, u->lc[k]); setU(sLocLF + k, u->lf[k]);
            setU(sLocLA + k, u->la[k]); setU(sLocLK + k, u->lk[k]); setU(sLocLD + k, u->ld[k]);
        }
    }
    auto setB = [](int loc, bool v) {   /* bool uniforms: sent only when they change */
        if (loc < 0) return;
        signed char& sh = sBoolShadow[(loc - 0x68) & 15];
        if (sh == (signed char)v) return;
        sh = (signed char)v; C3D_BoolUnifSet(GPU_VERTEX_SHADER, loc, v);
    };
    setB(sLocLit, lit); setB(sLocHead, head);
    for (int k = 0; k < 2; ++k) {
        const bool on = lit && k < u->nLights;
        setB(sLocL[k], on); setB(sLocA[k], on && u->spot[k]); setB(sLocS[k], on && u->spec[k]);
    }
    setB(sLocMulti, ns > 1);   // one matrix: fixed registers, no mova / relative addressing
    /* r53: face culling by the PICA for mesh draws (st->cull: 0 none, 1 GPU_CULL_FRONT_CCW, 2 GPU_CULL_BACK_CCW, mapped from GXSetCullMode
       by n3ds_gfx.cpp meshPicaCull). Ring draws are culled by the CPU and reset it to NONE (leaveMeshMode). */
    const int cull = (st->cull == 1 || st->cull == 2) ? st->cull : 0;
    if (cull != sCullShadow) { C3D_CullFace((GPU_CULLMODE)cull); sCullShadow = cull; }
    ++sMeshDraws;
    const u64 p2 = prof ? svcGetSystemTick() : 0;
    for (int i = 0; i < nRuns; ++i) {
        const unsigned first = runs[2 * i], count = runs[2 * i + 1];
        if (count < 3) continue;
        if (n3dsConfig().meshNoDraw) { ++sMeshRuns; continue; }     // diagnostics (mesh_nodraw)
        if (idx) C3D_DrawElements(GPU_TRIANGLES, (int)count, C3D_UNSIGNED_SHORT, idx + first);
        else     C3D_DrawArrays(GPU_TRIANGLES, (int)first, (int)count);
        ++sMeshRuns; sMeshIdx += count; sMeshIdxSplit += count;
        if (prof) ++sProfC[2];
    }
    maybeSplit();
    if (prof) {
        const u64 p3 = svcGetSystemTick();
        sProfT[0] += p3 - p0; sProfT[1] += p1 - p0; sProfT[2] += p2 - p1; sProfT[3] += p3 - p2;
        ++sProfC[0]; sProfC[1] += sUnifWrites - w0;
    }
}

extern "C" void n3ds_gpu_mesh_prof_take(unsigned long long t[4], unsigned c[3])
{
    for (int i = 0; i < 4; ++i) { t[i] = sProfT[i]; sProfT[i] = 0; }
    for (int i = 0; i < 3; ++i) { c[i] = sProfC[i]; sProfC[i] = 0; }
}

extern "C" void n3ds_gpu_commit(int n, const N3dsDrawState* st)
{
    if (!sInFrame || !sShader || n <= 0) return;
    u32 wordsLeft = 0;
    const float fill = cmdFill(&wordsLeft);
    if (fill > sCmdPeak) sCmdPeak = fill;
    if (fill > N3DS_CMDBUF_DROP_AT || wordsLeft < N3DS_CMDBUF_MIN_WORDS) { ++sCmdDrops; ++sCmdDropsFrame; return; }   // libctru would svcBreak on overflow
    if (sDefN && (st->blendMode != 0 || (st->writeMask & 5) != 5 || st->depthCmp == 7 || st->depthCmp == 0)) flushDeferred();   // order matters against this draw
    applyState(st);
    if (sScissorEmpty) return;                    // nothing visible; the reserved vertices stay free
    if (sMeshMode) leaveMeshMode();
    C3D_DrawArrays(GPU_TRIANGLES, sResStart, n);
    if (sOnBottom) ++sBotDraws;
    sUsed = sResStart + n;
    ++sDraws; sVerts += (unsigned)n;
    maybeSplit();
}

// r19: switch the screen the following draws go to (0 top, 1 bottom). Worker thread, inside a frame.
extern "C" int n3ds_gpu_target(int bottom)
{
    if (!sInFrame || !sShader) return 0;
    if (bottom && !sBottom) return 0;
    if ((bottom != 0) == sOnBottom) return 1;
    flushDeferred();                               // queued GPU-path models belong to the screen they were drawn for
    if (bottom) {
        if (!sBottomCleared) { C3D_RenderTargetClear(sBottom, C3D_CLEAR_ALL, 0x10204000u | 0xFF, 0); sBottomCleared = true; ++sBotFrames; if (!sBotFramesTot++) n3ds_log("[gpu] first frame drawn on the bottom screen (frame %u)", sFrameNo); }   // dark blue: a map frame with no map in it still shows
        C3D_FrameDrawOn(sBottom);
    } else C3D_FrameDrawOn(sTop);
    sOnBottom = bottom != 0; sScrW = sOnBottom ? 320 : 400;
    sHaveApplied = false; sScissorEmpty = false;   // viewport / scissor are in the new screen's pixels: program everything again
    return 1;
}

static void beginFrame(void)
{
    n3ds_note("worker: in C3D_FrameBegin");
    /* No C3D_FRAME_SYNCDRAW: it adds a wait for the next vblank on top of the wait for the previous frame's GPU work, and log f60c4ca9 showed a
       15-20 ms 'frame end + present' per frame on a ~110 ms frame. The pacing to the display is done by paceToVblank() (fps_cap) before
       C3D_FrameEnd, and C3D_FrameBegin(0) still blocks until the previous frame's command list and transfer are finished. */
    C3D_FrameBegin(0);
    n3ds_note("worker: FrameBegin done");
    C3D_RenderTargetClear(sTop, C3D_CLEAR_ALL, sClear, 0);   // depth 0 = far under the (-1, 0) depth map below
    C3D_FrameDrawOn(sTop);
    if (sShader) {
        C3D_BindProgram(&sProg);
        C3D_SetAttrInfo(&sAttr);
        C3D_SetBufInfo(&sBuf[sRingIdx]);
        sMeshMode = false; sLitKnown = false;
        if (sMeshOk) C3D_BoolUnifSet(GPU_VERTEX_SHADER, sLocXform, false);
        C3D_DepthMap(true, -1.0f, 0.0f);
        C3D_CullFace(GPU_CULL_NONE); sCullShadow = 0;
        memset(sBoolShadow, -1, sizeof sBoolShadow);   /* r53: re-send every mesh bool at the first mesh draw of the frame */
        // texture combiner stages are programmed per draw in applyState() (sHaveApplied=false below forces the first program)
    }
    sDefN = 0; sDefRunN = 0;
    sOnBottom = false; sBottomCleared = false; sScrW = 400;
    sInFrame = true; sHaveApplied = false; sScissorEmpty = false; sUsed = 0; sDraws = 0; sVerts = 0; sFlushedTo = 0; sSplits = 0; sMeshIdxSplit = 0;
}

// ---- frame limiter (fps_cap in nectar.ini, default 30) ----------------------------------------------------------------------
// The top screen refreshes at ~59.83 Hz and a frame queued with C3D_FrameEnd is shown at the next vblank, so the presented rate is
// 60 / n when successive submissions are n vblanks apart. The limiter counts REAL vblanks (gspWaitForVBlank) instead of sleeping a
// fixed time, so it cannot drift against the display (a 33.3 ms sleep against a 33.4 ms cadence beats to 30 -> 29 -> 30 fps with
// a judder every few seconds). A frame that takes longer than the target is never delayed further: the limiter only ever waits.
static const u64 VB_TICKS = 4481136;        // SYSCLOCK_ARM11 (268111856) / 59.8261 Hz
static u64 sLastVb = 0;                     // timestamp of the vblank the previous submission was aligned to (0 = none yet)
static u64 sTPace = 0;                      // ticks spent waiting in the limiter since the last [gpu-perf] line

static void paceToVblank(void)
{
    const int cap = n3dsConfig().fpsCap;
    if (cap <= 0) return;
    int vbs = (int)(60.0f / (float)cap + 0.5f);
    if (vbs < 1) vbs = 1;
    if (vbs > 4) vbs = 4;
    const u64 t0 = svcGetSystemTick();
    if (!sLastVb) { gspWaitForVBlank(); sLastVb = svcGetSystemTick(); sTPace += sLastVb - t0; return; }
    // vblanks that have gone by since the last aligned one (the slack absorbs wake-up latency; ~2 ms)
    int k = (int)((t0 - sLastVb + VB_TICKS / 8) / VB_TICKS);
    if (k >= vbs) { sLastVb = t0 - ((t0 - sLastVb) % VB_TICKS); return; }   // already late: submit now, re-align to the latest vblank
    while (k < vbs) { gspWaitForVBlank(); ++k; }
    const u64 t1 = svcGetSystemTick();
    sLastVb = t1; sTPace += t1 - t0;
}

// ---- FPS counter (nectar.ini fps_counter = 1): presented frames per second, 7-segment digits in the top right corner ----------
// Drawn through the normal ring path just before the frame is closed, so it sits on top of everything. Coordinates are upright
// top-screen pixels (400x240, origin top-left); the tilt is applied the same way the game's vertices get it.
static unsigned sFpsShown = 0;                 // value on screen (rounded, updated twice a second)
static unsigned sFpsFrames = 0;
static u64 sFpsT0 = 0;

static void fpsQuad(N3dsVtx*& o, float x0, float y0, float x1, float y1, u8 r, u8 g, u8 b)
{
    const float t0 = sTilt[0], t1 = sTilt[1], t2 = sTilt[2], t3 = sTilt[3];
    const float px[4] = {x0, x1, x1, x0}, py[4] = {y0, y0, y1, y1};
    static const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; ++k) {
        const float cx = px[order[k]] / 200.0f - 1.0f, cy = 1.0f - py[order[k]] / 120.0f;
        N3dsVtx& v = *o++;
        memset(&v, 0, sizeof v);
        v.x = t0 * cx + t1 * cy; v.y = t2 * cx + t3 * cy; v.z = -0.5f; v.w = 1.0f;
        v.r = r; v.g = g; v.b = b; v.a = 255;
    }
}

static void drawFpsCounter(void)
{
    if (!n3dsConfig().fpsCounter || !sShader || !sInFrame) return;
    const u64 now = svcGetSystemTick();
    ++sFpsFrames;
    if (!sFpsT0) sFpsT0 = now;
    if (now - sFpsT0 >= SYSCLOCK_ARM11 / 2) {
        sFpsShown = (unsigned)((double)sFpsFrames * SYSCLOCK_ARM11 / (double)(now - sFpsT0) + 0.5);
        sFpsFrames = 0; sFpsT0 = now;
    }
    // segments a b c d e f g (bit 0..6) for 0-9
    static const unsigned char seg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    char dig[3]; int nd = 0;
    unsigned v = sFpsShown > 999 ? 999 : sFpsShown;
    if (v >= 100) dig[nd++] = (char)(v / 100 % 10);
    if (v >= 10) dig[nd++] = (char)(v / 10 % 10);
    dig[nd++] = (char)(v % 10);
    const float W = 7, H = 13, T = 2, GAP = 3, PAD = 2;
    const float totalW = nd * W + (nd - 1) * GAP;
    const float x0 = 400.0f - PAD - totalW, y0 = PAD + 1;
    N3dsVtx* o = n3ds_gpu_reserve(6 + 6 * 7 * 3);
    if (!o) return;
    N3dsVtx* const start = o;
    fpsQuad(o, x0 - PAD, 0, 400, y0 + H + PAD, 0, 0, 0);                         // black backing box
    for (int d = 0; d < nd; ++d) {
        const float dx = x0 + d * (W + GAP), m = H / 2;
        const unsigned char s = seg[(int)dig[d]];
        if (s & 0x01) fpsQuad(o, dx,         y0,         dx + W,     y0 + T,     255, 255, 255);   // a
        if (s & 0x02) fpsQuad(o, dx + W - T, y0,         dx + W,     y0 + m + 1, 255, 255, 255);   // b
        if (s & 0x04) fpsQuad(o, dx + W - T, y0 + m,     dx + W,     y0 + H,     255, 255, 255);   // c
        if (s & 0x08) fpsQuad(o, dx,         y0 + H - T, dx + W,     y0 + H,     255, 255, 255);   // d
        if (s & 0x10) fpsQuad(o, dx,         y0 + m,     dx + T,     y0 + H,     255, 255, 255);   // e
        if (s & 0x20) fpsQuad(o, dx,         y0,         dx + T,     y0 + m + 1, 255, 255, 255);   // f
        if (s & 0x40) fpsQuad(o, dx,         y0 + m - 1, dx + W,     y0 + m + 1, 255, 255, 255);   // g
    }
    N3dsDrawState st; memset(&st, 0, sizeof st);
    st.depthCmp = 7; st.writeMask = 3;                                             // always pass, colour + alpha only (no depth write)
    st.blendMode = 0; st.alphaCmp = 7; st.cull = 0;
    st.vpX0 = 0; st.vpY0 = 0; st.vpX1 = 400; st.vpY1 = 240;
    st.scX0 = 0; st.scY0 = 0; st.scX1 = 400; st.scY1 = 240;
    n3ds_gpu_commit((int)(o - start), &st);
}

// Leave the GPU in a neutral state at the end of EVERY frame. GPU registers are not reset when the HOME menu (or another process) takes
// the GPU over, so whatever our last draw left there is what the HOME menu's renderer starts from. After a gpu_xform = 1 session the
// HOME menu drew doubled/garbled text until a reboot; the candidates are what only we change: the shader's bool uniforms (b0 xform,
// b1 lit - citro3d only uploads uniforms at the next draw, so the GPU keeps the last draw's values) and texture units 1/2 left enabled.
// Bools go back to 0 and units 1/2 are disabled here; the next frame re-binds every unit (sHaveApplied = false forces applyState).
static void neutralGpuState(void)
{
    if (!sShader) return;
    if (sMeshMode) leaveMeshMode();
    if (sMeshOk) {
        const int all[] = { sLocXform, sLocLit, sLocHead, sLocMulti, sLocL[0], sLocL[1], sLocA[0], sLocA[1], sLocS[0], sLocS[1] };
        for (int b : all) if (b >= 0) C3D_BoolUnifSet(GPU_VERTEX_SHADER, b, false);   /* r53: every mesh bool back to 0 */
        memset(sBoolShadow, 0, sizeof sBoolShadow);
        sLitShadow = false; sLitKnown = true;
        C3D_CullFace(GPU_CULL_NONE); sCullShadow = 0;
    }
    C3D_UpdateUniforms(GPU_VERTEX_SHADER);                          // emits the bool write now, not at the next draw
    GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x1, BIT(0));      // byte 0: unit 0 on, units 1/2 (and proctex) off
}

static void endFrame(void)
{
    if (sOnBottom) n3ds_gpu_target(0);            // the FPS counter and the next frame start on the top screen
    if (sFrameNo % 300 == 0) { n3ds_log("[gpu] bottom screen: %u of the last 300 frames drew on it, %u draws (target %s)", sBotFrames, sBotDraws, sBottom ? "ok" : "MISSING"); sBotFrames = sBotDraws = 0; }
    flushDeferred();
    drawFpsCounter();
    neutralGpuState();
    const u64 t0 = svcGetSystemTick();
    if (sShader && sUsed > sFlushedTo) GSPGPU_FlushDataCache(sRing[sRingIdx] + sFlushedTo, (u32)(sUsed - sFlushedTo) * sizeof(N3dsVtx));
    const u64 t1 = svcGetSystemTick();
    if (sFrameNo == 60 || sFrameNo == 300 || sFrameNo % 600 == 0 || (sDropped && sFrameNo % 60 == 0) || sCmdDropsFrame)
        n3ds_log("[gpu] frame %u: %u draws, %u verts (ring %d%% of %d), ring-full drops so far %u | cmdbuf %d KB: peak %d%%, now %d%%, %u draws dropped this frame (%u total)",
                 sFrameNo, sDraws, sVerts, (int)((long long)sUsed * 100 / RING_VERTS), RING_VERTS, sDropped,
                 N3DS_CMDBUF_KB, (int)(sCmdPeak * 100.0f), (int)(cmdFill(NULL) * 100.0f), sCmdDropsFrame, sCmdDrops);
    if (sMeshDraws && (sFrameNo == 60 || sFrameNo == 300 || sFrameNo % 600 == 0))
        n3ds_log("[gpu] frame %u: mesh path %u draws in %u runs, %u indices (%u triangles) | mesh draws dropped for a full command buffer so far %u", sFrameNo, sMeshDraws, sMeshRuns, sMeshIdx, sMeshIdx / 3, sMeshDropped);
    if (n3dsConfig().meshDefer && (sFrameNo == 60 || sFrameNo == 300 || sFrameNo % 600 == 0))
        n3ds_log("[gpu] frame %u: mesh_defer: %u draws queued, %u flushes (queue full %u) since the last line", sFrameNo, sDefDraws, sDefFlushes, sDefFull);
    if (sFrameNo == 60 || sFrameNo == 300 || sFrameNo % 600 == 0) sDefDraws = sDefFlushes = sDefFull = 0;
    sMeshDraws = sMeshRuns = sMeshIdx = 0;
    sCmdDropsFrame = 0;
    if (sFrameNo % 60 == 0) sCmdPeak = 0.0f;
    n3ds_note("worker: in paceToVblank");
    paceToVblank();                               // after the cache flush: the wait is pure idle time, nothing is lost by submitting late
    const u64 t2 = svcGetSystemTick();
    n3ds_note("worker: in C3D_FrameEnd");
    C3D_FrameEnd(0);
    n3ds_note("worker: FrameEnd done");
    sTFlush += t1 - t0; sTEnd += svcGetSystemTick() - t2;
    sInFrame = false; sLastSplits = sSplits;
    sRingIdx ^= 1;
}

extern "C" int n3ds_gpu_present(unsigned frame)
{
    sFrameNo = frame;
    if (!sTried) { sTried = true; sOk = gpuInit(); if (sOk) beginFrame(); return sOk ? 1 : 0; }
    if (!sOk) return 0;
    const u64 t0 = svcGetSystemTick();
    endFrame();
    const u64 t1 = svcGetSystemTick();
    beginFrame();
    const u64 t2 = svcGetSystemTick();
    sTBegin += t2 - t1; sTPresent += t2 - t0; ++sPerfFrames;
    // Where does a frame go? begin = C3D_FrameBegin(0): blocks until the GPU finished the PREVIOUS frame, so a large 'begin' with a small game-side time means the GPU (geometry / fill) is the bottleneck; a small
    // 'begin' means the CPU side (game logic + vertex assembly) is.
    if (frame > 0 && frame % 60 == 0 && (frame <= 600 || frame % 600 == 0) && sPerfFrames) {
        const double d = (double)sPerfFrames * N3DS_TICKS_PER_MS;
        n3ds_log("[gpu-perf] frame %u, avg of last %u frames, ms/frame: present() %.1f = cache flush %.1f + FrameEnd %.1f + FrameBegin(wait GPU/vsync)+clear %.1f | of which frame-limiter wait (fps_cap %d) %.1f | gpu_split %d (%s): %d command-list pieces in the frame just closed",
                 frame, sPerfFrames, sTPresent / d, sTFlush / d, sTEnd / d, sTBegin / d, n3dsConfig().fpsCap, sTPace / d,
                 n3dsConfig().gpuSplit, C3D_FrameSplit ? "available" : "NOT in this citro3d", sLastSplits);
        sTFlush = sTEnd = sTBegin = sTPresent = sTPace = 0; sPerfFrames = 0;
    }
    return 1;
}
