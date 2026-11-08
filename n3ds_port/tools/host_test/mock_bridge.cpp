// HOST TEST ONLY: the ctru-side half of n3ds_bridge.h, faked with a plain array "ring" so n3ds_gfx.cpp can run on a PC.
#include "n3ds_bridge.h"
#include <stdarg.h>
#include <stdio.h>
#include <chrono>
#include <vector>
#include <stdlib.h>
#include "mock_bridge.h"
#include "n3ds_mesh_unif.h"

MockRing gRing;
MockMesh gMesh;
static std::vector<N3dsVtx> sBuf;
static int sReserved = 0, sUsedV = 0;

void MockRing::reset(int cap) { capacity = cap; sBuf.assign(cap, N3dsVtx{}); sUsedV = 0; commits = 0; nVerts = 0; freeOverride = -1; }
const N3dsVtx* MockRing::verts() { return sBuf.data(); }
void MockRing::newFrame() { sUsedV = 0; nVerts = 0; commits = 0; }

extern "C" {
void n3ds_log(const char* fmt, ...) { if (!gRing.quiet) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); } }
unsigned long long n3ds_ticks(void) { return (unsigned long long)std::chrono::steady_clock::now().time_since_epoch().count() / 1000; }
int n3ds_gpu_tilt(float out[4]) { out[0] = 0; out[1] = 1; out[2] = -1; out[3] = 0; return 1; }   /* what the 1i hardware log printed */
int n3ds_gpu_target(int bottom) { (void)bottom; return 1; }   /* r19: bottom-screen map target */
void n3ds_gpu_set_clear(unsigned, unsigned, unsigned) {}
int n3ds_gpu_ring_free(void) { return gRing.freeOverride >= 0 ? gRing.freeOverride : gRing.capacity - sUsedV; }
N3dsVtx* n3ds_gpu_reserve(int n) { if (sUsedV + n > gRing.capacity) return nullptr; sReserved = sUsedV; return sBuf.data() + sUsedV; }
void n3ds_gpu_commit(int n, const N3dsDrawState* st) {
    sUsedV = sReserved + n; gRing.nVerts += n; ++gRing.commits; gRing.lastState = *st;
    if (gMesh.ready) { MockEvent e; e.type = 0; e.first = sReserved; e.count = n; e.st = *st; e.vtx = nullptr; e.nVtx = 0; e.idx = nullptr; gMesh.ev.push_back(e); }
}
int n3ds_gpu_mesh_ready(void) { return gMesh.ready ? 1 : 0; }
void n3ds_gpu_draw_mesh(const N3dsDrawState* st, const N3dsMeshVtx* vtx, unsigned nVtx, const unsigned short* idx, const unsigned* runs, int nRuns, const N3dsMeshUniforms* u)
{
    if (gMesh.bench) { static N3dsMeshUniforms sink; n3ds_mesh_unif_copy(&sink, u); (void)st; (void)vtx; (void)nVtx; (void)idx; (void)runs; (void)nRuns; return; }   /* r54 bench: what the real queue copies, nothing else */
    MockEvent e; e.type = 1; e.first = e.count = 0; e.st = *st; e.vtx = vtx; e.nVtx = nVtx; e.idx = idx; e.runs.assign(runs, runs + 2 * nRuns);
    /* r54: what the deferred queue keeps (n3ds_mesh_unif.h); the rest is poisoned, so a field meshIssue needs but the copy drops shows up as a failed check */
    memset(&e.u, 0x7F, sizeof e.u); n3ds_mesh_unif_copy(&e.u, u);
    gMesh.ev.push_back(e);
}
void* n3ds_linear_alloc(unsigned bytes) { ++gMesh.allocs; gMesh.linearBytes += bytes; return malloc(bytes); }
void n3ds_linear_free(void* p) { if (p) { ++gMesh.frees; free(p); } }
void n3ds_flush_dcache(const void*, unsigned) {}
unsigned n3ds_linear_free_kb(void) { return 60000; }
void n3ds_gpu_mesh_prof_take(unsigned long long t[4], unsigned c[3]) { for (int i = 0; i < 4; ++i) t[i] = 0; for (int i = 0; i < 3; ++i) c[i] = 0; }
}

// ---- 1k texture mocks ----
#include <stdlib.h>
static void* sMockTex[64]; static int sMockN = 0;
int n3ds_gpu_tex_create(int w, int h, int fmt, void** data, unsigned* bytes)
{
    if (sMockN >= 63) return 0;
    const unsigned b = (unsigned)w * h * (fmt == 0 ? 4 : fmt == 5 ? 1 : 2);
    sMockTex[++sMockN] = malloc(b); *data = sMockTex[sMockN]; *bytes = b; return sMockN;
}
void n3ds_gpu_tex_flush(int) {}
void n3ds_gpu_tex_destroy(int id) { if (id > 0 && id <= sMockN) { free(sMockTex[id]); sMockTex[id] = 0; } }
unsigned n3ds_gpu_tex_free_kb(void) { return 20000; }

// n3ds_config.cpp sets this (nectar.ini alloc_prof); the real one lives in platform/n3ds_alloc_trace.cpp (3DS only)
extern "C" { int n3ds_allocprof_on = 0; }
