#pragma once
#include "n3ds_bridge.h"
struct MockRing {
    int capacity = 0, commits = 0, nVerts = 0, freeOverride = -1; bool quiet = false; N3dsDrawState lastState;
    void reset(int cap); void newFrame(); const N3dsVtx* verts();
};
extern MockRing gRing;
// 1p: GPU-transform mesh path. Off unless a test sets ready; every ring commit and mesh draw is logged in order.
#include <vector>
struct MockEvent { int type; int first, count; N3dsDrawState st; const N3dsMeshVtx* vtx; unsigned nVtx; const unsigned short* idx; std::vector<unsigned> runs; N3dsMeshUniforms u; };
struct MockMesh { bool ready = false, bench = false; std::vector<MockEvent> ev; size_t linearBytes = 0; int allocs = 0, frees = 0; };
extern MockMesh gMesh;
