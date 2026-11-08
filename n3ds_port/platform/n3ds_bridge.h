// Plain-C boundary between the two halves of the platform layer:
//   ctru side  (n3ds_main.cpp, n3ds_ctru_io.cpp)  includes <3ds.h> and NEVER game headers
//   game side  (n3ds_pad/window/game.cpp)         includes game headers and NEVER <3ds.h>
// Keeping them apart avoids typedef/macro collisions between libctru and the decompiled game.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
enum {
  N3DS_BTN_A = 1 << 0, N3DS_BTN_B = 1 << 1, N3DS_BTN_X = 1 << 2, N3DS_BTN_Y = 1 << 3,
  N3DS_BTN_START = 1 << 4, N3DS_BTN_SELECT = 1 << 5,
  N3DS_BTN_UP = 1 << 6, N3DS_BTN_DOWN = 1 << 7, N3DS_BTN_LEFT = 1 << 8, N3DS_BTN_RIGHT = 1 << 9,
  N3DS_BTN_L = 1 << 10, N3DS_BTN_R = 1 << 11, N3DS_BTN_ZL = 1 << 12, N3DS_BTN_ZR = 1 << 13,
};
typedef struct {
  unsigned buttons;      // N3DS_BTN_* held
  int stickX, stickY;    // circle pad, raw (about +-156)
  int cstickX, cstickY;  // C-stick (New 3DS), raw; 0 on Old 3DS
} N3dsInput;

void n3ds_log(const char* fmt, ...);            // bottom screen + sdmc:/3ds/nectar/log.txt
void n3ds_log_memory(const char* where);        // free app/linear memory + malloc in-use
void n3ds_input_poll(N3dsInput* out);           // hidScanInput + read
int  n3ds_alive(void);                          // aptMainLoop(): 0 when the system wants us to quit
void n3ds_present_heartbeat(unsigned frame);    // temporary: proves the frame loop runs (until citro3d)
void n3ds_frame_tick(unsigned frame);           // window layer -> watchdog: presented-frame counter
void n3ds_note(const char* what);               // remember "last thing that happened" for the watchdog line
unsigned n3ds_time_ms(void);                    // ctru side: monotonic-ish ms (osGetTime) -- deliberately NOT std::chrono
unsigned n3ds_thread_id(void);                  // ctru side: threadGetCurrent() as an id
void n3ds_calls_dump(int lastN);                // game side: last N function enter/exit events (needs -DN3DS_TRACE_CALLS=ON)
void n3ds_trace_dump(int lastN);                // game side: print the last N recorded events (watchdog calls it on a stall)
void n3ds_vfp_runfast(void);                    // set FPSCR flush-to-zero + default-NaN for the calling thread (no VFP exceptions on denormals/NaN)
void n3ds_yield_take(unsigned* calls, unsigned* ms10); // yields since the last call: count, wall time in 0.1 ms
void n3ds_audio_diag_take(unsigned* lockCalls, unsigned* lockMs10, unsigned* cbCalls, unsigned* cbMs10, unsigned* cbMaxMs10); // audio lock waits + mixer callback time since the last call
void n3ds_gfxq_diag_take(unsigned* bytes, unsigned* fullWaits, unsigned* fullMs10); // game->worker queue traffic since the last call
void n3ds_yield(void);                          // ctru side: REAL yield for OSYieldThread (short svcSleepThread), see n3ds_ctru_io.cpp
int  n3ds_gpu_present(unsigned frame);          // ctru side (citro3d): draw + present one top-screen frame; 0 = GPU unavailable
void n3ds_gpu_set_clear(unsigned r, unsigned g, unsigned b); // ctru side: clear colour for the next frame (0-255 each)
unsigned long long n3ds_ticks(void);            // ctru side: svcGetSystemTick(); N3DS_TICKS_PER_MS ticks = 1 ms (1j perf lines)
#define N3DS_TICKS_PER_MS 268111.856            // SYSCLOCK_ARM11 / 1000 (system tick rate is the same on Old and New 3DS)
void n3ds_gfx_frame_end(unsigned frame);        // game side (n3ds_gfx.cpp): per-frame stats -> [gfx-perf] log line, resets counters

/* ---- graphics (stage 2) -------------------------------------------------------------------------------------------
   Game side (n3ds_gfx.cpp) emits triangle lists straight into a GPU-visible ring owned by the ctru side (n3ds_gpu.cpp):
     v = n3ds_gpu_reserve(n); fill v[0..k); n3ds_gpu_commit(k, &state);
   Positions are PICA clip space (already tilted for the rotated LCD, see n3ds_gpu_tilt). Compare/blend numbers are the
   GX SDK values (GX_NEVER=0 .. GX_ALWAYS=7, blend factors GX_BL_ZERO=0 .. GX_BL_INVDSTALPHA=7); the ctru side maps them. */
typedef struct { float x, y, z, w; unsigned char r, g, b, a; float u, v; float u1, v1, u2, v2; } N3dsVtx;   /* 44 bytes: the PICA attribute layout (u1..v2 = texture units 1 and 2, only meaningful for multi-texture draws) */
/* ---- textures + texture combiner (1k) ------------------------------------------------------------------------------
   The game side decodes GameCube textures into PICA layout (8x8 Morton tiles) straight into memory owned by the ctru side.
   Numeric values here are this port's own; n3ds_gpu.cpp maps them to citro3d enums with explicit switches. */
enum { N3DS_TF_RGBA8 = 0, N3DS_TF_RGB565 = 1, N3DS_TF_RGBA5551 = 2, N3DS_TF_RGBA4 = 3, N3DS_TF_LA8 = 4, N3DS_TF_L8 = 5 };
enum { N3DS_TS_PRIMARY = 0, N3DS_TS_TEX0 = 1, N3DS_TS_CONST = 2, N3DS_TS_PREV = 3, N3DS_TS_TEX1 = 4, N3DS_TS_TEX2 = 5, N3DS_TS_BUFFER = 6 };   /* combiner source (r48: BUFFER = the PICA combiner buffer) */
enum { N3DS_TO_C = 0, N3DS_TO_1MC = 1, N3DS_TO_A = 2, N3DS_TO_1MA = 3 };                       /* operand (alpha stages use A/1MA only) */
enum { N3DS_TFN_REPLACE = 0, N3DS_TFN_MODULATE = 1, N3DS_TFN_ADD = 2, N3DS_TFN_INTERP = 3, N3DS_TFN_MULADD = 4, N3DS_TFN_SUB = 5 };
typedef struct {
  unsigned char rgbSrc[3], rgbOp[3], rgbFn, rgbScale;   /* scale: 1, 2 or 4 */
  unsigned char aSrc[3], aOp[3], aFn, aScale;
  unsigned int color;                                   /* stage constant, 0xAABBGGRR */
} N3dsTevStage;

typedef struct {
  unsigned char depthCmp;               /* GX compare; 7 = always */
  unsigned char writeMask;              /* bit0 RGB, bit1 A, bit2 Z */
  unsigned char blendMode;              /* 0 none, 1 blend, 2 subtract (dst - src) */
  unsigned char blendSrc, blendDst;     /* GX_BL_* */
  unsigned char alphaCmp, alphaRef;     /* GX compare; 7 = alpha test off */
  unsigned char cull;                   /* 0 none, 1 front, 2 back (ignored in stage 2: nothing is culled) */
  short vpX0, vpY0, vpX1, vpY1;         /* viewport rect in upright top-screen pixels (400x240, origin top-left) */
  short scX0, scY0, scX1, scY1;         /* scissor rect, same space */
  unsigned short texId;                 /* 1k: texture from n3ds_gpu_tex_create(); 0 = untextured */
  unsigned char texWrapS, texWrapT;     /* GX wrap: 0 clamp, 1 repeat, 2 mirror */
  unsigned char nTev, depthBias;        /* number of valid tev[] stages; 0 = plain vertex colour | r46: 1 = pulled toward the camera (decal) */
  unsigned short tex1Id, tex2Id;        /* texture units 1 and 2 (multi-texture combiners); 0 = unused */
  unsigned char tex1WrapS, tex1WrapT, tex2WrapS, tex2WrapT;
  unsigned char tevBufRgb, tevBufA, _pad2[2];   /* r48: combiner buffer update masks (bit i: after stage i (0..3) the buffer takes its rgb / alpha) */
  N3dsTevStage tev[6];                  /* PICA texture-combiner stages (see n3ds_tev.cpp) */
} N3dsDrawState;   /* always memset to 0 before filling: the struct is compared with memcmp */
N3dsVtx* n3ds_gpu_reserve(int maxVerts);                      /* NULL: not in a frame, or ring full (draw dropped) */
void     n3ds_gpu_commit(int nVerts, const N3dsDrawState* st);/* draws the first nVerts of the last reservation */
int      n3ds_gpu_ring_free(void);                            /* vertices still free in this frame's ring (0 if not in a frame) */
/* ---- GPU-transform mesh path (1p) ----------------------------------------------------------------------------------
   Cached model-space display lists are drawn indexed with the vertex shader doing the transform. A mesh vertex is 36 bytes:
   float x,y,z,slot | ubyte r,g,b,a | float u0,v0 | float u1,v1 | float nx,ny,nz,0  (slot = which per-draw matrix, 0..N3DS_MESH_SLOTS-1).
   52 bytes, only attribute formats the ring path already uses on hardware (float4 / ubyte4 / float2). */
#define N3DS_MESH_SLOTS 10
typedef struct { float x, y, z, slot; unsigned char r, g, b, a; float u0, v0, u1, v1; float nx, ny, nz, nw; float u2, v2; } N3dsMeshVtx;   /* 60 bytes (r35: + texture-coordinate set 2, CPU path only) */
typedef struct {
  int   nSlots;
  float clip[N3DS_MESH_SLOTS][16];      /* row-major 4x4 per slot: projection * model, LCD tilt folded into rows 0 and 1. CPU side only since r53
                                           (culling / checks / host tests), except with clipOnly (debug probes): then it is what is drawn */
  float proj[16];                       /* r53: F, row-major 4x4 view -> clip (projection with the LCD tilt folded into rows 0 and 1) */
  float pos[N3DS_MESH_SLOTS][12];       /* r53: P, row-major 3x4 model-view (the game's position matrix) per slot */
  float nrm[N3DS_MESH_SLOTS][12];       /* N, row-major 3x4 normal matrix per slot (lit / head only) */
  float tcu[8][4];                      /* texture-coordinate generation rows, see passthrough.v.pica */
  float cw[4], mc[4];                   /* colour: vertex weight (incl. 1/255, tint) and material part */
  int   lit;                            /* r53: GameCube lighting in the shader (amb / lights below) */
  int   head;                           /* the fake headlight (nectar.ini lighting = 0) */
  int   clipOnly;                       /* debug probes: draw with clip[0] as the whole transform (no lighting) */
  float amb[4];                         /* ambient: rgb of COLOR0, a of ALPHA0; 1 for a channel without lighting */
  int   nLights;                        /* 0..2 */
  float lp[2][4], lc[2][4], lf[2][4], la[2][4], lk[2][4], ld[2][4];   /* view-space position, colour per channel, diffuse fn, angle / distance attenuation, direction */
  int   spot[2], spec[2];               /* attenuation function: spot, specular (neither = none) */
} N3dsMeshUniforms;
int      n3ds_gpu_mesh_ready(void);                            /* 1 when the unified shader is up and a frame is open */
void     n3ds_gpu_draw_mesh(const N3dsDrawState* st, const N3dsMeshVtx* vtx, unsigned nVtx, const unsigned short* idx,
                            const unsigned* runs, int nRuns, const N3dsMeshUniforms* u);   /* runs = (firstIndex, indexCount) pairs; vtx/idx linear memory */
void     n3ds_gpu_mesh_prof_take(unsigned long long t[4], unsigned c[3]);   /* r54 [lite5]: ticks issuing mesh draws (total, state, uniforms, draw calls) and
                                                                               counts (draws, uniform registers written, runs) on profiled frames; resets them */
void     n3ds_linear_free(void* p);                            /* ctru side: free linear memory from n3ds_linear_alloc */
void     n3ds_flush_dcache(const void* p, unsigned bytes);     /* ctru side: make CPU writes visible to the GPU */
unsigned n3ds_linear_free_kb(void);
int      n3ds_gpu_target(int bottom);                         /* r19: following draws go to the top (0) or bottom (1) screen; 0 = not possible */
int      n3ds_gpu_tilt(float out[4]);                         /* X' = t0*x + t1*y, Y' = t2*x + t3*y; 0 while GPU not up */
void     n3ds_gfx_flush(void);                                /* game side: commit pending vertices (call before present) */
/* Textures. create: allocates a w x h texture (both powers of two, 8..1024) in GPU-visible memory and returns its id (> 0) with
   *data pointing at the memory to fill (PICA-swizzled); 0 = failed (out of linear memory / table full). flush: call after
   filling. destroy: only when the GPU can no longer be reading it (the game side waits 3 frames). */
int      n3ds_gpu_tex_create(int w, int h, int fmt, void** data, unsigned* bytes);
void     n3ds_gpu_tex_flush(int id);
void     n3ds_gpu_tex_destroy(int id);
unsigned n3ds_gpu_tex_free_kb(void);                          /* linear heap still free, KB */
void     n3ds_bench(void);                                    // ctru side: one-shot CPU / memory speed probe -> [bench] log lines
void n3ds_game_entry(void);                     // game side: Initialise -> NodeMgr -> run

/* ---- audio output (1n) -------------------------------------------------------------------------------------------
   ctru side (n3ds_audio.cpp): NDSP stereo PCM16 sink. Whatever mixes the game's sound (Open Nectar's JAudio software DSP)
   hands interleaved L,R samples to n3ds_audio_write(); it never blocks and returns how many FRAMES it accepted. */
int      n3ds_audio_open(unsigned rate);                      /* 1 = ready (idempotent); 0 = NDSP/DSP firmware unavailable */
unsigned n3ds_audio_write(const short* interleavedLR, unsigned frames);
unsigned n3ds_audio_queued_frames(void);                      /* frames the DSP still has (for a mixer deciding how much to make) */
void     n3ds_audio_stats(unsigned* underruns, unsigned* droppedFrames, unsigned* blocksQueued);
void     n3ds_audio_close(void);
void     n3ds_audio_selftest(unsigned ms);                    /* two-tone test through the same path; nectar.ini audio_test = 1 */

/* two-thread rendering (n3ds_gfx.cpp = queue, n3ds_worker.cpp = ctru side thread) */
void*    n3ds_linear_alloc(unsigned bytes);                   /* ctru side: linear (GPU-visible) memory; NULL on failure */
void     n3ds_sleep_ns(long long ns);                         /* ctru side: svcSleepThread */
void     n3ds_worker_stop(void);                              /* ctru side: stop + join the render worker (idempotent; also runs atexit) */
int      n3ds_worker_start(void);                             /* ctru side: spawn the render worker (core 2); 0 = unavailable */
int      n3ds_gfxq_drain(void);                               /* game side, called by the worker: run published commands, returns count */
void     n3ds_gfxq_start(int enable);                         /* game side: call once after pc_gfx_init */
int      n3ds_gfxq_end_frame(unsigned frame, unsigned long long* waitedTicks); /* game side: 1 = queued (worker presents), 0 = do it inline */
void     n3ds_gfxq_stop(void);                                /* game side: drain the queue, stop the worker, go back to inline rendering (idempotent) */
unsigned n3ds_gfxq_busy_ticks(void);                  // two-thread mode: ticks the core-2 worker has spent replaying so far (wraps; use differences)
#ifdef __cplusplus
}
#endif
