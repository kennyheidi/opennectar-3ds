# Open Nectar -> New Nintendo 3DS

Status: **milestone 1j (graphics stage 2 diagnosed + fixed on paper). Ran on hardware up to 1i; 1j not yet built with devkitARM or run.**
Reference: 2gifts/melee-3ds (`native-3ds/port/`). Base: Open Nectar (`pc_port/`).

## Verified so far (in the authoring sandbox)
- `tools/check_arm.sh`: all 383 game sources (`sysCommon`, `sysDolphin`, `plugPiki*`) pass
  `arm-none-eabi-g++ -fsyntax-only -march=armv6k -mfloat-abi=hard` with Nectar's own flags.
  Only compile blockers were platform headers: `pc_gfx.h`->GL, `pc_window.h`->SDL2, `sys/mman.h`.
  Those are stood in by `shim/` (types only; nothing links).
- Endianness: no `setend be` trick needed (Melee uses one). Nectar byte-swaps in the stream layer
  (`sysCommon/stream.cpp`), and ARM11 is little-endian like x86.
- Seam for graphics: game code -> `gx_stubs.cpp` -> `pc_gfx_*` (~120 functions, GX-command level,
  no GL types in signatures). The 3DS backend re-implements exactly that API.

## NOT verified
- No devkitARM/libctru in the sandbox, so: no link, no real build, no hardware, no performance data.
  `cmake/toolchain-3ds.cmake` is written from the devkitPro layout and untested.
- Syntax-clean != correct. Runtime behaviour (32-bit pointer assumptions, alignment, VFP vs Gekko
  paired-single quirks) is unknown until it runs.

## Milestones
1. **Link + boot to a black screen.** Toolchain build of legacy lib + `n3ds_port/platform`
   (main, `n3ds_mmap`, libctru-backed OS/DVD/CARD stubs reusing `dolphin_stubs/*`).
   Audio off (`PIKI_USE_JAUDIO=0`), gfx = no-op `pc_gfx_*`.
2. **Files.** DVD stubs read extracted assets from `sdmc:/3ds/nectar/`. Extraction tool converts
   textures at install time (see 4).
3. **Input.** Implement the `pc_window_*` API the game uses on libctru HID (circle pad, C-stick/
   ZL-ZR, buttons; touch for menus). Bottom screen = HUD/map candidate.
4. **Graphics (the big one).** citro3d backend for `pc_gfx_*`.
   - Map TEV stages to PICA200 combiners (6 stages vs TEV's 16) -- port Melee's `gx.c`/`renderer.c`
     approach; Pikmin materials are expected (unverified) to be mostly 1-3 stages.
   - Textures: GC tiled formats/CMPR -> PICA swizzled RGB565/RGBA8/ETC1, POT limits, at extract time.
   - Fixed 4:3 (`pc_gfx_*` widescreen/HUD helpers become no-ops); 400x240 top, 320x240 bottom.
5. **Memory.** `sysNew.cpp` has a 64 MB mmap threshold and Nectar pools scale with the Pikmin limit;
   3DS apps get roughly 100-180 MB depending on memory mode (verify). Cap pools, keep limit at 100.
6. **Audio.** Software DSP on ARM11 is the CPU risk. Start with lowest voice count, output via ndsp.
7. **Perf.** Target 30 FPS (the game's native rate). Melee-3ds only reaches ~55-60 FPS on light
   scenes with heavy optimization, so treat this as a stretch, not a promise.
8. Stereo 3D last (Melee-3ds shows how).

## Legal
Own disc dump only, nothing bundled. Nectar code is CC0; check melee-3ds's `LICENSE-SCOPE.md`
before copying any of its code (this repo copies none so far).

## Milestone 1a (game code builds as a library)
`cmake -S n3ds_port -B build-3ds -DCMAKE_TOOLCHAIN_FILE=<3DS toolchain>` builds all 383 game sources into
`libpikmin_legacy_3ds.a`. Verified in the authoring sandbox with bare arm-none-eabi-gcc 13 (full compile, exit 0,
9 MB archive) -- NOT yet with devkitARM. ~1,190 symbols are unresolved at link time; the largest families are
Dolphin SDK calls (GX 75, pc_gfx 50, OS 33, CARD 23, pc_window 21, VI/PAD/DVD/MTX...) that upstream's
`pc_port/dolphin_stubs/` implements. Next: compile those with shims + write `n3ds_port/platform/`.

## Milestone 1b (this drop): platform layer + .3dsx
Layout: `platform/` = 3DS-specific code; everything else is upstream, unmodified.
- **Reused as-is from upstream:** `dolphin_stubs/{os,gx,dvd,vi,card,misc}_stubs.cpp`, `timing/*.cpp`.
- **Written for 3DS:** `n3ds_main.cpp` (entry, logging, game thread w/ 1 MB stack), `n3ds_ctru_io.cpp` (HID/APT/heartbeat),
  `n3ds_game.cpp` (Initialise -> NodeMgr -> run), `n3ds_pad.cpp` (PAD* on HID), `n3ds_window.cpp`, `n3ds_mman.cpp`.
- **Generated:** `n3ds_stubs_gen.cpp` (305 default definitions, from upstream headers by `tools/gen_stubs.py`): mods off,
  prompts auto-accept 1-player/normal, settings = upstream defaults, all `pc_gfx_*` no-ops (this list IS the API for the future
  citro3d backend). Replaced: nothing in upstream. Skipped on purpose: pc_settings.cpp (SDL/GL/fstream heavy), pc_window.cpp,
  pc_gfx.cpp, audio (Jac_* = no-ops, `pc_aram_init` doesn't allocate the 16 MiB ARAM).
- libctru-side and game-side files never include each other's headers (`n3ds_bridge.h` is the only meeting point).
- Verified in sandbox (bare arm-none-eabi GCC 13, no devkitPro): all game + port sources compile except `os_stubs.cpp`
  (needs threads; devkitARM has them); simulated link has 0 duplicate definitions and 0 unresolved symbols other than
  libctru calls, `__OSBusClock` (defined in os_stubs), and devkitARM runtime (`chdir`, `mkdir`, `__dso_handle`).
- NOT verified: real link, libctru API details written from memory (`hidCstickRead`, `APT_CheckNew3DS`, `threadCreate`
  core id -2, `KEY_ZL`), `os_stubs.cpp` compile, anything at runtime.

## Known risks, in the order I expect them to bite
1. **Memory.** Watch the `[mem ...]` lines in log.txt. Game `operator new` -> malloc; >=64 MB blocks go to n3ds_mmap.
2. **OS threads.** os_stubs.cpp makes game threads with std::thread, which ignores the game's requested stack size; libctru's
   pthread default stack is small. Likely first crash after Initialise. Fix = route OSCreateThread to libctru threadCreate.
3. **Assets.** DVD stubs read `assets/<path>` relative to `sdmc:/3ds/nectar/`. Extract with Open Nectar's own PC tooling.
4. `pc_aram_write` returns nullptr (audio off); only matters if something writes ARAM.

## Milestone 1c: first hardware run, diagnostics
First run on a New 3DS (user log): boots, assets found (15 entries), game thread starts, `gsys->Initialise()` succeeds
(+93 KB), enters `gsys->run(PlugPikiApp)`, then the log went silent. Malloc in-use 33.8 MB at start = the 32 MiB simulated
GC arena (CMake-patched copy of os_stubs.cpp; upstream reserves a 256 MiB static array) + 1 MiB game stack.
Changes: game thread's stdout/stderr now go to log.txt (upstream printf's every DVDOpen and OSReport there), a 5 s
watchdog logs `frame=N (NOT ADVANCING)` + heap free + last note, `pc_window_swap_buffers` logs frames 0-2, 10, 60, every 600.
Fixed (by user): `pc_vs_rules()` returned a dangling reference; generator now refuses to emit reference-returning stubs
without an override.

## Milestone 1d: where does the game stall? (log analysis)
Log (CIA, 124 MB mode, heap 88 MB, linear 32 MB): boot OK, asset loads OK, working set ~70.5 MB of ~88 MB, "Starting
System::run main loop", one app->idle() allocates two 13.4 MB heaps and prints the GameFlow line, then frame=3 forever with
NO further DVDOpen and NO "[PC Port] FPS" line (printed every 2 s by the loop). Process alive, watchdog ticking => the game
thread is blocked or spinning inside app->idle(), not crashed.
Candidates (all involve the game's real OS threads, implemented as std::thread in os_stubs.cpp): loader thread handshake
(System::startLoading/endLoading: loadMesgQueue/sysMesgQueue, driven by DGXGraphics::retraceProc from VIWaitForRetrace),
DVD thread (dvdFunc/dvdMesgQueue), CardUtil thread, or a busy-wait starving a same-priority thread (Horizon threads of equal
priority on one core may not time-slice).
Instrumentation added: `--wrap` on OSCreateThread/Resume/Join/Send/ReceiveMessage/WaitCond, DVDReadPrio, VIWaitForRetrace
recording into a ring buffer (`platform/n3ds_trace.cpp`); `Jac_Gsync` stub = per-loop heartbeat. On a stall the watchdog dumps
the last 40 events + each thread's last event at t=10 s and every 60 s.

## Milestone 1e: trace analysis -> the stall is in main thread iteration 3
Trace (5 threads): main thread logs `loop 1`, `loop 2` (one VIWaitForRetrace + DVD-thread nudge), `loop 3`, then NOTHING.
Every other thread is parked normally (DVD thread on dvdMesgQueue, both loader threads exited, card thread on its condvar).
So the main thread is spinning or blocked in something not wrapped, after `GameFlow::softReset()` allocated the second
13.4 MB app heap (section change NinLogo -> next) and before the next DVDOpen. ARQPostRequest completes synchronously and the
PC copyCacheToTexture path is synchronous, so the mDmaComplete/mTexComplete busy-waits are ruled out.
Next: `-DN3DS_TRACE_CALLS=ON` instruments game functions; the stall dump lists the last 120 enter/exit events;
`tools/resolve_calls.sh log.txt build-3ds/nectar_3ds.elf` names them.

## Milestone 1f: better stall diagnostics (this drop; nothing here fixes the stall yet)
1e log: main thread spins in one tiny function `0x2317a0` (32 M enter/exit events in ~10 s); the card thread (`0x232a5c`, 5 KB
away, probably the same translation unit) is parked in `OSWaitCond`; all other threads idle. Two hypotheses, not yet separated:
(a) game-requested priorities are ignored (all threads share one Horizon priority; a spinning thread never yields, so workers
starve), (b) a condvar wakeup is lost / never sent.
Changes (all diagnostics, unverified on hardware or with devkitARM):
- `n3ds_trace.cpp`: the call trace is now a per-thread **shadow stack** (16 threads x 48 frames). The stall dump prints `[stack]`
  lines with each thread's full call chain instead of 120 copies of one function. `tools/resolve_calls.sh` reads them.
- New `--wrap`s: `OSSignalCond`, `OSYieldThread`, `OSSleepThread`, `OSWakeupThread` (into the event ring); `OSLockMutex`,
  `OSUnlockMutex`, `OSGetTime` (counters only, printed as `[trace] counters:`; compare the t=10 s and t=70 s dumps: a frozen
  OSGetTime value means a time-based spin). `__real_*` are declared weak, so a function upstream lacks does not break the link.
- Watchdog thread priority pinned to 0x19 (was game prio - 1).
- Quick check with no rebuild: `arm-none-eabi-addr2line -Cfe build-3ds/nectar_3ds.elf 0x2317a0 0x232a5c`.
Next (needs upstream `os_stubs.cpp`): route `OSCreateThread` to libctru `threadCreate` with priority
`gameThreadPrio + (gcPrio - 16)` (card 14 -> game-2, loaders 15 -> game-1), and a real stack size.

## Milestone 1g: first shadow-stack dump (log analysis) + diagnostics fix
1f log (rebuilt, so addresses moved: card thread now 0x232c44, retrace/DVD thread 0x109504). All three stall dumps (10 s, 70 s,
125 s) show the SAME main-thread chain, 8 frames deep, innermost 0x231bc4, and the ring is 100% `OSYieldThread` from the main thread
(73 M events by t=125 s). So the game's own wait loop calls OSYieldThread, i.e. this is a poll-and-yield wait, not a raw spin.
The card thread is still parked in OSWaitCond; mutex counters (4 lock / 3 unlock) fit exactly one card-thread lock held across
the wait. OSGetTime = 0 calls through the wrap: no evidence for a time-based spin. NOT yet known: whether OSSignalCond was ever
called (the yield flood erased the ring), and what the 8 addresses are (needs the ELF from THAT build).
1g changes: OSYieldThread is now a counter (was ring, flooded it); added OSSignalCond/OSWakeupThread counters + last cond address;
the dump copies the ring before printing and prints signed ages. Still diagnostics only.

## Milestone 1h: yield fix + graphics stage 1 (nothing here built or run; no devkitARM in the authoring sandbox)
Stall (1g log): card thread parked in OSWaitCond with NO `<<` after the main thread's OSSignalCond, main thread at 11.8 M
OSYieldThread calls. Signal arrived after the wait began, so a lost wakeup is unlikely; the woken thread was never scheduled.
- `OSYieldThread` wrapper now calls `n3ds_yield()` = `svcSleepThread(100 us)` instead of the real (plain sched_yield) yield.
  If the stall persists after this, next suspects: os_stubs.cpp condvar, or priorities/core (rsf AffinityMask 1 = core 0 only).
- `platform/n3ds_gpu.cpp` (citro3d): C3D_Init, top render target, clear + present each `pc_window_swap_buffers`. Pulsing
  dark-blue clear = proof the GPU path is live. Falls back to the old software heartbeat if C3D_Init/target creation fails.
- CMake: links `citro3d` before `ctru`. Needs the citro3d package (dkp-pacman -S 3ds-dev has it).
Stage 2 (untextured geometry) needs upstream `pc_port/gl/pc_gfx.h` + `dolphin_stubs/gx_stubs.cpp` to write against.

## Milestone 1i: graphics stage 2 (geometry, no textures) -- log 1h analysis + new backend
1h log (hardware): stall FIXED by the sleeping OSYieldThread -- loop 3 passes, ~29.9 FPS, intro -> file select loads; citro3d up
(`linear free 30937 KB`). Game reports `matrices 218/8192, shapes 2/1000` = it was already issuing draws into the no-op stubs.
Ends with `game exited normally` at frame ~600 (ask: was START/HOME pressed?). Heap free is only 0.2-0.4 MB of 88 MB (72 MB in use).
New (uploaded upstream `pc_gfx.h` + `gx_stubs.cpp` used as the spec; nothing built with devkitARM):
- `platform/n3ds_gfx.cpp` (game side): 28 `pc_gfx_*` functions moved OUT of `n3ds_stubs_gen.cpp` / `tools/stub_symbols.txt`
  (projection, viewport/scissor, pos/nrm matrices, z/blend/alpha/cull state, vtx desc/fmt/arrays, immediate begin/position/color/
  texcoord/end, display lists incl. big-endian GX command parsing: NOP, CP MATINDEX_A, XF/BP skip, prim draws).
  Prims -> triangle lists (quads, strips, fans). CPU transforms clip = proj * pos-mtx, then the LCD tilt. Fake headlight if normals.
  Textured draws are TINTED by a colour hashed from the bound GXTexObj (debug aid until stage 3). No culling, no TEV, no fog, no lines.
- `platform/n3ds_gpu.cpp` (ctru side): frame kept open (End -> flip ring -> Begin/clear/DrawOn each swap), 2 linear rings of 98304
  vertices, passthrough PICA vertex shader (`platform/shader/passthrough.v.pica`, built by picasso, embedded by `tools/bin2c.cmake`),
  GX->PICA state mapping (depth compare INVERTED: PICA near = larger; blend factor 2/3 flips src<->dst), viewport/scissor via
  citro3d's own Mtx_OrthoTilt so the rotated-LCD mapping is not hand-derived.
- Screen: 4:3 pillarboxed to 320x240 centred (`-DN3DS_STRETCH=ON` to fill 400x240). Clear colour = the game's GXSetCopyClear.
- Host-tested (mock ring, real pc_gfx.h): perspective z=-0.96/w=5 for a point 5 units ahead, strip winding, indexed display list.
- ASSUMPTIONS to check on hardware: PICA clip z range [-w,0] == GX's (holds for citro3d's own Mtx_Persp); arrays native-endian while
  display-list bytes are big-endian; `C3D_DepthMap(true,-1,0)`; picasso syntax. The first 6 draws and 4 triangles are logged (`[gfx]`).

## Milestone 1j: log 1i diagnosis (stage-2 graphics on hardware) -- see DIAGNOSIS_1j.md
1i log (New 3DS, 124 MB): no crash; ~6.5 FPS (was ~30 with no-op gfx); `[gpu] frame 60: 64 draws, 97593 verts (ring 99%)`, ring-full
drops 219,621 by frame 60 and +~3.86k/frame after; game returned normally shortly after frame 128 (only exit path in the platform code: `aptMainLoop()==false`). Changes (nothing built with
devkitARM/picasso here; host-tested + syntax-checked only):
- **Shader:** vertex colour scaled by 1/255 (PICA does not normalise GPU_UNSIGNED_BYTE; the combiner saturates at 1.0 -> everything white).
- **n3ds_gfx.cpp:** homogeneous trivial-reject of triangles wholly outside near/far or the scissor rect; non-finite vertices dropped;
  UI reserve (when < 8 chunks free only depth-test-off draws may reserve); empty scissor skips the draw; counters -> `[gfx-perf]`
  (prims, drops by cause, in-verts -> tris, culled, decode ms and estimated fill on profiled frames).
- **n3ds_gpu.cpp:** scissor re-applied after every viewport change (C3D_SetViewport overwrites it); empty scissor never reaches
  citro3d; ring 98304 -> 131072 vertices (`-DN3DS_RING_VERTS`); `[gpu-perf]` split of present() into cache flush / FrameEnd / FrameBegin wait.
- **n3ds_window.cpp:** `[perf]` line (ms/frame, present() share); logs why the game exited.
- `tools/host_test/run.sh`: g++-only test of the culling (60k random triangles, 0 wrongly culled), assembly order, scissor, UI reserve.
Next: read `[perf]`/`[gpu-perf]`/`[gfx-perf]` of the 1j run (decision table in DIAGNOSIS_1j.md) before touching anything else.

## Milestone 1n: Melee reference -> audio output sink (see NOTES_1n.md)
- `platform/n3ds_audio.cpp`: NDSP stereo PCM16 sink (Melee-style prebuffer / underrun handling / direct cache flush), `audio_test = 1`
  in nectar.ini plays a two-tone test. RSF gains `FlushProcessDataCache: 84`; link needs only `ctru` (NDSP is part of libctru; there is no separate libndsp).
- JAudio engine NOT yet enabled: needs upstream's `pc_port/audio` + `third_party/nextos-audio` (could not be fetched) to write the SDL audio shim.
- Graphics: Melee's render-worker-on-core-2 design is the real lever for the 4 FPS geometry cost; not implemented, awaiting a decision.
