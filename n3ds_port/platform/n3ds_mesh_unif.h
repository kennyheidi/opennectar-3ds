/* r54: the slim copy of a mesh draw's uniforms that the deferred-draw queue keeps (n3ds_gpu.cpp). Shared with the host mock
   (tools/host_test/mock_bridge.cpp), which fills the rest of the copy with garbage first: the PICA-interpreter check then proves
   meshIssue reads nothing the copy leaves out. */
#ifndef N3DS_MESH_UNIF_H
#define N3DS_MESH_UNIF_H
#include <stddef.h>
#include <string.h>
#include "n3ds_bridge.h"

/* Before r54 the queue copied all 2.2 KB per draw (ten matrix slots and the CPU-only clip[]); a one-matrix model now copies ~0.4 KB.
   Everything from tcu[] to the end of the struct is one contiguous block. */
static inline void n3ds_mesh_unif_copy(N3dsMeshUniforms* d, const N3dsMeshUniforms* u)
{
    const int ns = u->nSlots < 1 ? 1 : u->nSlots > N3DS_MESH_SLOTS ? N3DS_MESH_SLOTS : u->nSlots;
    d->nSlots = u->nSlots;
    if (u->clipOnly) memcpy(d->clip[0], u->clip[0], sizeof d->clip[0]);
    memcpy(d->proj, u->proj, sizeof d->proj);
    memcpy(d->pos, u->pos, sizeof(float) * 12 * (size_t)ns);
    if (u->lit || u->head) memcpy(d->nrm, u->nrm, sizeof(float) * 12 * (size_t)ns);
    memcpy(d->tcu, u->tcu, sizeof(N3dsMeshUniforms) - offsetof(N3dsMeshUniforms, tcu));
}
#endif
