// Game-side texture registry for the citro3d backend (1k). Never includes <3ds.h> (see n3ds_bridge.h).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "gl/pc_gfx.h"

struct N3dsTexRef {                        // snapshot taken by GXLoadTexObj (the GXTexObj itself may be a stack temporary)
    const void* data; const void* pal;
    uint32_t hash, palHash;
    uint16_t w, h, palN;
    uint8_t fmt, wrapS, wrapT, palFmt;
    bool valid;
};
struct N3dsTexBound { uint16_t id; float su, sv; bool ok; };   // id: n3ds_gpu_tex_create() id; su/sv: image extent inside the pot texture

bool n3dstexCapture(const GXTexObj* obj, N3dsTexRef* out);          // false: not one of ours (never initialised through pc_gfx_init_tex_obj*)
N3dsTexBound n3dstexResolve(const N3dsTexRef& r, unsigned frame);   // decodes + uploads on first use; ok=false -> draw untextured
void n3dstexFrame(unsigned frame);                                  // once per presented frame: frees textures the GPU is done with
void n3dstexLogStats(unsigned frame);                               // one [tex] summary line
void n3dstexDescribe(const N3dsTexRef& r, char* out, size_t n);       // r37 diagnostics: GC data stats, changed-since-init, cache entry + GPU copy average
uint32_t n3dstexSampleGc(const N3dsTexRef& r, float u, float v);    // the GC texel (0xAABBGGRR) a coordinate lands on, nearest, with the wrap modes
