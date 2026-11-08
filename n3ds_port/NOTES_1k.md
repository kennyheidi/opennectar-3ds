# Milestone 1k - what changed and what to look for in the next log

Built and unit-tested on the host (68 checks pass: `sh tools/host_test/run.sh`). NOT compiled for ARM here: n3ds_gpu.cpp and
n3ds_bench.cpp need devkitPro, so expect to fix a typo or two on the first real build.

## Changes
1. Textures: GC I4/I8/IA4/IA8/RGB565/RGB5A3/RGBA8/C4/C8/C14X2/CMPR -> PICA (RGB565 / L8 / LA8 / RGBA5551 / RGBA4 / RGBA8,
   smallest exact format), 8x8 Morton swizzle, pot padding. Decoded lazily on first draw, cached, LRU, 10 MB budget
   (-DN3DS_TEX_BUDGET_KB). Files: n3ds_tex.cpp, n3ds_texconv.cpp.
2. TEV: GX stages -> PICA combiner (6 stages max) in n3ds_tev.cpp. Falls back to tex*vertex colour when a program cannot be
   expressed (more than 6 stages, results kept in TEV registers). `approximations` counts inexact mappings.
3. CPU: lossless sub-pixel triangle reject (no pixel centre in bbox -> draws nothing), per-draw attribute list, reciprocal
   table for fixed-point fractions. Expect `sub-pixel` in [gfx-perf] to be large in the title scene (was 85% tiny tris).
4. Profiling: display-list split (parse / transform / assembly), pc_gfx_perf_scope_* timings ([scope] lines), [tex] stats,
   [bench] CPU probe at startup (tells you whether 804 MHz mode is really on).
5. Memory: N3DS_ARENA_MB default 24 -> 8. The malloc heap hit 87 MB in use and OSPanic'd on the next stage load. Revert
   with -DN3DS_ARENA_MB=24 if the game turns out to use the arena.

## Verify on hardware, in this order
- [bench]: int LCG 20M should be well under ~150 ms. If it is ~3x slower the speed-up mode is not active.
- Textures upside down -> rebuild with -DN3DS_TEX_FLIPV=1 (and expect to flip v in makeVtx instead).
- Colours wrong / garbage -> sdmc:/3ds/nectar/texdump/*.tga (first 24 textures, decoded from the GC data). If the TGAs look
  right but the screen does not, the bug is on the PICA side (byte order / format); if the TGAs are garbage, call
  n3dsconv::setLE16(true) (game may byte-swap texture data on load).
- "[tex] release_texture: 0 cache entries matched" on every call -> the argument is not the pixel pointer.
- "[tex] FAILED" lines: linear memory exhausted; lower N3DS_TEX_BUDGET_KB or N3DS_RING_VERTS.
- Approximations: PICA constant colour is one per stage; texgen/texture matrices, fog, lights, 2nd texture still missing.
