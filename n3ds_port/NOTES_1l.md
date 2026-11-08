# Milestone 1l (on top of 1k)

## What the 1k log showed
- CPU is fine: [bench] int 161 Miter/s, heap memcpy 388 MB/s (804 MHz mode is on). CIA already runs in 124 MB mode
  (app region 126976 KB, homebrew=0); memory was never the limit (heap 72 of 87 MB, linear 20 MB free).
- The ~21 s startup stall was MY debug TGA dump: 1k dumped the first 24 textures with one fwrite() per pixel to the SD card, inside
  the timed region. Only dumped textures (<= 512 px) were slow (35-120 us/px); the 1024x1024 one, too large to dump, took 286 ms.
  Fixed: dump off by default (tex_dump=N in nectar.ini), one buffered write, timed separately.
- Steady state is 4 FPS because of geometry, not textures: logo.mod + opening.mod = 71k vertices/frame, 606 display lists,
  ~3 us per vertex (212 ms), 65-85% of the triangles sub-pixel. 1j had the same 172 ms at frame 60.

## Changes
1. Memory mode: -DN3DS_SYSMODE_EXT=124MB (default) | 178MB. RSF is now a template (platform/nectar.rsf.in -> build/nectar.rsf).
   178MB auto-sets N3DS_LINEAR_HEAP_MB=80 (the malloc heap window is 96 MB) and a 40 MB texture budget. UNTESTED on hardware.
2. nectar.ini (see nectar.ini.example): tex_shift / tex_max_dim / tex_min_dim / tex_box / tex_force16 / tex_budget_kb / tex_off /
   tex_dump / tex_diag. Shrinking happens while decoding, so it also cuts decode time (tex_box=0 decodes 4^shift times fewer texels).
3. tools/tune_textures.py (stdlib only): analyze <log>, fit <log> --budget-kb N, preset low|medium|high|potato, preview <texdump> <out>.
4. Display lists: per-list post-transform vertex cache (exact; keyed on raw index bytes + matrix + vertex format). The next
   [gfx-perf] line prints "vertex-cache hits X of Y": that number decides whether more geometry work is needed.
5. [tex-diag]: phase timing + a second warm decode for the first 8 textures.

## Next log: what to look at
- frame-60 "display-list decode" vs 200 ms before, and vertex-cache hit rate.
- [tex-diag] lines: should show convert in tens of ms, no multi-second outliers.
- If still ~4 FPS on logo.mod/opening.mod: try tex_off=1 to rule textures out, then the real fix is per-object culling / LOD
  (most of those triangles are sub-pixel on a 400x240 screen).

# 1m: 30 fps lock
- fps_cap in nectar.ini (default 30; 60/20/15/0 also valid). Implemented in n3ds_gpu.cpp paceToVblank(): counts real vblanks
  (gspWaitForVBlank) before C3D_FrameEnd, so successive submissions are exactly 2 vblanks apart at 30. It only ever waits; a frame that
  already took longer is submitted at once. [gpu-perf] prints the limiter wait; note present() in the [perf] line now includes it.
- It cannot raise a slow frame rate: while frames take > 33 ms (currently ~240 ms) the cap does nothing. It only matters once under it.
- Unverified: whether the game's own scheduler advances logic per presented frame or by wall clock. If game speed looks halved at 30,
  set fps_cap = 60 and tell me; if it looks right, nothing else is needed.
