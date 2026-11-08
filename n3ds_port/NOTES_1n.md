# Milestone 1n: what the Melee 3DS port teaches us (audio + graphics), and the audio output sink

Reference read: `melee-3ds-3ds/native-3ds/port/` (GPL-3 per its LICENSE-SCOPE.md). **No code was copied** (this repo's PORT_PLAN
rule: check licences before copying); what follows is design, re-implemented from scratch. Nothing here is built with devkitPro
or run on hardware.

## Audio: what Melee does, and what Pikmin needs
Melee implements the GameCube **AX** voice API on the CPU (`port/engine/audio.c`): 64 voices, ADPCM decode from a 16 MB ARAM
image, linear-interpolated resampling, volume ramps, a mix of 96 samples per block at 32 kHz, stereo. The result goes through one
function (`mp_native_audio`, `port/3ds/audio.c`) into NDSP.
Pikmin is different one level up: Open Nectar's sound comes from the **JAudio engine** (`PIKI_USE_JAUDIO=1`), whose software DSP
and bank loader were taken from `pikmin-nextos` and moved from SDL3 to SDL2 (release notes 0.3). Your build has it OFF
(`PIKI_USE_JAUDIO=0`, all `Jac_*` stubbed in `n3ds_stubs_gen.cpp`), and `pc_aram_init` is stubbed so the 16 MB ARAM host copy is not
allocated. So the mixer already exists upstream; what the 3DS lacks is its **speaker end** and the SDL audio calls under it.
- DONE in this drop: `platform/n3ds_audio.cpp`, the speaker end. `n3ds_audio_write(interleaved s16 L,R, frames)` -> NDSP, never
  blocks, returns frames accepted. Melee's lessons applied: ring of buffers with several slices per buffer, prebuffer (~48 ms) before
  un-pausing, re-prebuffer on underrun, cache clean with `svcFlushProcessDataCache` (RSF now lists `FlushProcessDataCache: 84`; falls
  back to `DSP_FlushDataCache` if refused).
- `nectar.ini`: `audio_test = 1` plays a 3 s two-tone (440 Hz left, 660 Hz right, then both) through that sink before the game starts.
  **Do this first on hardware.** A clean tone proves DSP + firmware + sink; `.3dsx` builds also need `sdmc:/3ds/dspfirm.cdc`.
  Expected log: `[audio] NDSP stereo PCM16 ready ...` then `[audio] selftest done: ... underruns 0 dropped frames 0`.
- NOT done: hooking the JAudio engine to it. I could not read upstream's `pc_port/audio/` or `third_party/nextos-audio/` (GitHub
  blocks automated fetches from my side), so I do not know which SDL audio calls it makes (callback-driven `SDL_OpenAudioDevice`
  vs `SDL_QueueAudio`), its output rate, or its source file list. Guessing would produce a shim that compiles and does the wrong thing.
  To finish it I need either the two folders, or this output from an Open Nectar checkout:
      grep -rhoE 'SDL_[A-Za-z0-9_]+' pc_port/audio third_party/nextos-audio | sort | uniq -c | sort -rn
      ls pc_port/audio third_party/nextos-audio
  Then the work is: add those sources to `UPSTREAM_PORT_SOURCES`, flip `PIKI_USE_JAUDIO=1`, give `shim/SDL2/SDL.h` a real
  audio section whose callback is pumped by a mixer thread calling `n3ds_audio_write`, and decide the ARAM question (16 MB host copy
  vs the 124 MB app region: heap was 72 of 87 MB in the 1k log, so ARAM probably has to live in linear memory or be trimmed).
- CPU risk stays: the software DSP competes with the game thread. Melee splits work across cores for exactly this reason.

## Graphics: what Melee does that your 1l/1m notes ask for
Your 1l finding: ~4 FPS because ~71k vertices/frame cost ~3 us each (212 ms) on ONE thread, with most triangles sub-pixel.
Melee's answer (`port/3ds/render_worker.*`, `early_queue.*`, README "records the game's graphics commands on the main CPU core and
translates and renders them on another core"): the game thread only RECORDS immutable draw packets; a worker pinned to the New
3DS system core (core 2) does vertex transform, state translation and C3D submission; explicit fences protect inputs that the game
may overwrite; completed command prefixes are submitted early inside the frame.
Today `n3ds_main.cpp` creates every thread on core -2 (default, core 0) and `n3ds_gfx.cpp` transforms inside `pc_gfx_*` calls.
Applying the idea is a real restructuring of `n3ds_gfx.cpp`/`n3ds_gpu.cpp`, so I have NOT done it blind. Smaller things, in order of
value:
1. Move the display-list vertex transform + assembly to a worker on core 2 (needs the game's vertex arrays / display lists to stay
   unchanged until a fence; Pikmin model data is static, textures/dynamic arrays are the hazard).
2. Per-object culling before submission (your own 1l conclusion; most triangles are sub-pixel at 400x240).
3. Replace per-frame `GSPGPU_FlushDataCache` (IPC) with `svcFlushProcessDataCache` in `n3ds_gpu.cpp` (now permitted by the RSF). Only
   one call per frame today, so small; it matters once submission is split into several segments.
Decision for you: do you want (1) next? It changes the threading model, so it should be your call.

## Verify on hardware, in this order
1. `audio_test = 1` -> hear two tones; log shows underruns 0.
2. If `ndspInit failed`: firmware (`dspfirm.cdc` for .3dsx) or the CIA `dsp::DSP` service line in the RSF.
3. If `svcFlushProcessDataCache refused`: the CIA exheader did not take the new syscall entry; harmless (falls back).
