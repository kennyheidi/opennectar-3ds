# 3DS graphics diagnosis: log.txt from the 1i build (New 3DS, 124 MB mode)

## Short version
- **It did not crash.** No abort, no `std::terminate`, no fault. The game thread returned normally
  (`[n3ds] game exited normally`) shortly after the t=25 s watchdog line (frame 128). The only exit path in the platform code is
  `pc_window_should_close() == !aptMainLoop()`, i.e. the system asked the app to quit (HOME -> close software, power
  button, launcher). If you did not do that, tell me: it would mean the game itself left `System::run`.
- **The graphics are wrong in three separate ways**, one confirmed by the log, one strongly suspected from the code,
  one unknown until the new timers report:
  1. The vertex ring overflows every frame and later draws are thrown away (**confirmed by the log**).
  2. Vertex colours are almost certainly saturating to white (**code review, ~90 % confident, not visible in the log**).
  3. The frame rate fell from ~30 FPS (1h, no-op graphics) to ~6.5 FPS. **The log cannot say whether the CPU or the GPU is
     to blame**; 1j adds the timers that can.

## Evidence
| Log line | What it says |
|---|---|
| `[gpu] frame 60: 64 draws, 97593 verts (ring 99%), ring-full drops so far 219621` | 64 draws x 1536-vertex chunks = 98,304 = the whole ring. It is full after the first ~1/3 of the frame's primitives. |
| `... frame 120: ... drops so far 451252` | (451252-219621)/60 = **~3,860 primitives dropped every frame, steadily**. Whatever is drawn after the ring fills never reaches the GPU; in a typical frame that is the end of the draw order, i.e. the UI/logo. |
| `[gfx] tri v0=(823.05 -108.11 -0.08 w6384.78) v1=(760.72 -179.42) v2=(774.39 -126.06)` | Strip winding is right (I re-derived tris 2-4 from the logged vertices). w = 6384 with z = -0.08 means these vertices sit right on the far plane; NDC extent ~0.01-0.02 = **roughly 1-3 pixel triangles**: dense, far geometry. Nothing rejects it, so the ring fills with it. |
| `rgba=76,106,93,255` | Colour is handed to the GPU as raw 0..255 ubyte (see cause 2). |
| `FPS: 6.5 ... TEV programs: 0, Textures: 0 live` | Constant 6.4-6.7 FPS from frame 10 on; watchdog frame counts 29/62/95/128 agree. Textures/TEV are still stubbed, as designed. |
| `[watchdog t=25s] frame=128` then `game exited normally` | Clean exit, see above. |
| `DVDOpen(".../logoMotion/logo.dck") -> FAILED` | A game asset is missing from `assets/` (the title-logo motion data). Not a graphics crash, but the logo animation will be wrong until it is extracted. |
| binary junk between `logo.bin ... size = 108` and `System: Opened file` | Something wrote ~100 non-text bytes into log.txt. Not a graphics fault; suspect the game printing a non-terminated buffer or two writers interleaving on the unbuffered log. Worth a look, not a priority. |

## Causes and fixes (all in this package)
1. **Ring overflow (confirmed).** Nothing culled geometry and a full ring silently dropped the tail of the frame.
   - `n3ds_gfx.cpp`: triangles wholly outside one clip plane (near, far, or the scissor rect) are rejected before they take
     ring space; NaN/inf vertices are dropped. The test is an exact half-space test in homogeneous coordinates, so it is
     safe for any w.
   - UI reserve: once fewer than 8 chunks are free only depth-test-off draws (2D/UI) may take space, so the HUD/logo
     survive a heavy 3D scene.
   - Ring 98,304 -> 131,072 vertices (`-DN3DS_RING_VERTS=`).
2. **Colours saturate (strong suspicion).** The PICA200 converts `GPU_UNSIGNED_BYTE` attributes to float **without
   normalising** (0..255), and the combiner clamps at 1.0, so any non-zero channel becomes full. Expect white blobs on the
   dark-blue clear colour. `passthrough.v.pica` now multiplies by 1/255.
   *How to falsify:* if the picture is nearly black after this build, my assumption is wrong; set the constant in the
   shader to `1.0`.
3. **Two smaller bugs found on the way.** `C3D_SetViewport` also resets citro3d's scissor to the viewport, so a viewport
   change silently replaced the game's scissor; it is now re-applied. An empty scissor would have been programmed as
   right-1 = 0xFFFF (i.e. "everything"); empty scissors now skip the draw.
4. **Frame rate (unknown).** New timers, logged every 60 frames up to 600, then every 600:
   - `[perf]` ms/frame, and how much of it is inside `present()`.
   - `[gpu-perf]` `present()` split into cache flush / `C3D_FrameEnd` / `C3D_FrameBegin` (which blocks until the GPU
     finished the previous frame).
   - `[gfx-perf]` prims, drops by cause, input vertices -> triangles, culled, plus (profiled frame only) display-list
     decode time and an **estimated fill in Mpx** and tiny/huge triangle counts.

### How to read the next log
| You see | Meaning | Next step |
|---|---|---|
| `FrameBegin ...` is most of `present()` and present() is most of `[perf]` | GPU-bound | Check fill: > ~3 Mpx/frame = overdraw or huge triangles (find what draws them); low fill with many tris = triangle rate, switch to indexed strips |
| "everything else" dominates, decode ms large | CPU vertex path | Cache per-vertex work, drop the fake headlight, skip lit maths for culled vertices |
| "everything else" dominates, decode ms small | Game logic itself (was ~30 ms at 1h) | Profile with `-DN3DS_TRACE_CALLS=ON` |
| `ring-full` / `ui-reserve` drops still > 0 | Scene still exceeds the ring | Raise `N3DS_RING_VERTS`, or move to indexed draws |

## What is and is not verified
- **Verified (host, g++ only):** `tools/host_test/run.sh` builds the real `n3ds_gfx.cpp` against a mock ring: 36 checks pass;
  60,000 random triangles, **0 visible triangles culled** (a deliberately broken test plane is caught, so the test has teeth);
  strip/quad/fan order; near/far/side/scissor cases; NaN/inf; the UI reserve. `n3ds_gpu.cpp` and `n3ds_window.cpp` pass
  `g++ -fsyntax-only` against throwaway stub headers.
- **Not verified:** any build with devkitARM, picasso or citro3d; the shader edit (picasso syntax is from memory of the
  official samples); every hardware behaviour claimed above (ubyte non-normalisation, `C3D_SetViewport` resetting the scissor).
  The upstream `pc_port/` and `src/` trees were not in the archive, so `gx_stubs.cpp` and `pc_gfx.h` could not be reviewed.
- Backface culling, textures, TEV and fog are still not implemented (as in 1i).

## Questions for you
1. Did you press HOME / close the software about 25-30 s in, or did the app return on its own?
2. What did the top screen actually show (white shapes, black, garbage, frozen)? That settles cause 2.
