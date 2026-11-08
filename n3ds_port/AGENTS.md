# Open Nectar 3DS — performance and platform contract

Adapted on 2026-11-01 from T2K's `AGENTS.md` (https://github.com/colbyshores/t2k, commit 94964668,
MIT License, copy in `tools/runner/LICENSE.t2k`). The rules R1–R11, T1/T2, the exemption grammar and the
"hot is a measurement" doctrine are T2K's. What is different here is written down below, with the reason.
T2K's own text, unabridged, is the reference for every rule: `tools/runner/contracts/vfp_hotpath_t2k.md`
and the upstream file.

## 0. The one-paragraph version

The target is the **New 3DS** (804 MHz, L2 on; the port does not run on an original 3DS, so there is no
"OG profile"). A change is a performance change only if an **A/B capture on hardware** says so (§6). Every
rule below is checked by `tools/runner/check.sh`; a green check is "nothing the screens can see is wrong",
**not** a performance pass. Model-view-projection and lighting belong on the **PICA200 vertex shader**; the
CPU transform path is a fallback, and every draw that falls back says why in the log (§5).

## 1. What the hardware taxes

T2K §1 applies unchanged (VFP11 transfer/convert/compare latencies, subnormals, 32 single registers, code
footprint). Two additions for this port:

1. **The game thread, the render worker and the mixer run on three different cores** (0, 2 and the
   30 %-limited core 1). A frame takes as long as the slowest of them; a saving on a core that is waiting
   anyway changes nothing. Look at `[perf-2core]` before optimising a core.
2. **The PICA200 transforms and lights vertices in its own shader units.** Up to r52 the default
   (`gpu_xform = 0`) did that work on the render worker's CPU core: in the r48 busy capture
   (`docs/validation/perf-2026-10-27-r48-forest-busy.log`) the cached-draw triangle loop is 16.5 ms of the
   worker's ~36 ms busy frame (7186 triangles, 2.3 µs each) plus 1.4 ms of per-draw matrix setup, and the
   worker is the core the game thread waits on (26 ms/frame). Moving that work to the GPU is the single
   largest available saving; see §5.

## 2. HOT PATH

T2K §2 verbatim: hot = runs per frame and is in the top 15 of a **captured** profile, OR runs ≥ 256 times
per frame, OR is annotated `/* @hotpath */`. The corpus is derived from a profile, never from memory.

- Profile = the console's own `log.txt`, saved as `docs/validation/perf-YYYY-MM-DD-<what>.log`.
  `contracts/project.json` maps the per-frame stage figures in it (`[lite2]`, `[lite3]`, `[prof]`,
  `[prof2]`, `[audioprof]`) to the files and functions that spend them. `gates_vfp.py` ranks the stages
  and makes those functions hot.
- Seeds (`contracts/hotpath_corpus.json`) are for the ≥ 256/frame criterion: per-vertex, per-triangle,
  per-draw and per-sample code.
- The corpus carries its capture date and goes stale after 45 days (T2K's clock, **not** frozen here:
  perf work is live). The gate rejects a `captured` date newer than the newest saved log.
- Cold: init, area load, texture conversion, wave decoding, menus, probes and log formatting.

## 3. Rules

R1–R11 and T1/T2 as in T2K, with these project facts:

- **R1** — helper: `n3ds_vfp_runfast()` (`n3ds_ctru_io.cpp`), called first on `main`, the game thread,
  the render worker, the mixer, the STX decoder, the menu and the watchdog threads, each with the
  rationale comment. Game threads the decompiled code creates through `OSCreateThread` get libctru's
  `initThreadVars()` FPSCR setup (FZ|DN on current libctru).
- **R10** — the build is CMake: the devkitPro flags (`-march=armv6k -mfloat-abi=hard -mtp=soft`) come from
  `cmake/toolchain-3ds.cmake`, `-fno-math-errno` from `n3ds_port/CMakeLists.txt`. Never `-ffast-math` /
  `-Ofast`.
- **R11** — the GX command queue wraps with a mask (its size is a power of two), not `%`.
- **Fidelity** — the mixer's output stage keeps the PC mixer's int truncation so 3DS and PC sound the same;
  those sites are pinned, not "fixed". The decompiled game (`src/`) is out of scope: it is the game.

### Exemptions

T2K §5 grammar, inline, near the site (the gate looks at most 800 characters back):

```
/* @vfp-exempt R<id>[,R<id>...] — <reason>. measured <cost-or-n/a>. Verified <YYYY-MM-DD>. */
```

A screen false positive (an integer cast the regex reads as float) is pinned with "screen false positive"
in the reason. Pins expire after 180 days. Standing exemptions live in `contracts/project.json`.

## 4. Not adopted from T2K, and why

- **§10 "C with classes"** (no templates, virtuals, RTTI, `std::function`): T2K's house style, not a
  performance rule (T2K says so itself). The decompiled game is C++ with virtual dispatch by design, and the
  port's platform code uses small templates (the TEV builder, the mixer kernels). T2K's
  `audit_c_with_classes.py` and `audit_arm11_math.py` (T2K-tree specific) are not copied.
- **§9 CIA identity** — T2K-specific.
- **OG profile** — the port requires a New 3DS.

## 5. Transform & lighting live on the GPU

- `nectar.ini gpu_xform = 1` (default from r53): cached display lists are drawn indexed from linear memory
  and the PICA vertex shader (`platform/shader/passthrough.v.pica`) does position (per-matrix model-view,
  then projection), normals, GameCube lighting (ambient + up to two lights: spot / specular / none
  attenuation, none / sign / clamp diffuse), texture-coordinate generation and material colour.
- The CPU path is the fallback for what the shader cannot express exactly. Each fallback is counted by
  reason in the `[mesh]` log line ("why lists stay on the CPU"): more than two lights, ambient from the
  vertex colour, normal- or position-sourced texture coordinates (environment maps), a model crossing the
  near plane, immediate-mode draws. A new fallback reason needs a counter.
- The GPU path's CPU side must stay small: a model whose bounding box needs no clipping goes to the GPU as
  one run with no per-piece tests (r54); per-piece box tests are only for models that cross the near plane
  or the guard band; an interval (AABB) test decides most boxes before the exact 8-corner test (r55).
  `[lite5]` in the log splits the path's worker time per call.
- The frame goes to the GPU in pieces (`gpu_split`): ring vertices and, since r55, mesh indices both count,
  so the PICA works while the CPU is still recording instead of after `C3D_FrameEnd`.
- Face culling on the GPU path is the PICA's (`C3D_CullFace`), mapped from `GXSetCullMode` with the same
  sign convention as the CPU path (`nectar.ini gpu_cull` flips it if a console disagrees).
- Shader hygiene (both lessons were paid for with GPU hangs): every output register is written exactly
  once, whole (build it in a temporary, then one `mov`); no `mad`.
- Correctness gate: `tools/host_test/run_mesh.sh` runs the real assembled shader in a PICA interpreter and
  requires the GPU path to reproduce the CPU path's triangles, colours and texture coordinates (lighting
  included) within float24 rounding.

## 6. Evidence: A/B or it did not happen

A performance claim needs an A/B capture on hardware, interleaved in the same scene:

1. Set `ab_test = 1` (and `ab_key`, `ab_a`, `ab_b`, `ab_frames` if not the default gpu_xform 0 vs 1) in
   `nectar.ini`, go into an area and play / stand in a busy spot for several minutes, copy `log.txt` to
   `docs/validation/perf-YYYY-MM-DD-<what>.log`. Since r54 the console starts the test by itself once 120
   frames in a row drew game models, discards (and re-runs on the same side) any phase with a load hitch
   (a frame > 250 ms) or a frame without game models, and orders phases A B B A so each pair (0,1) (2,3) ...
   holds one A and one B and a drifting scene affects both sides alike.
2. `python3 tools/runner/ab_report.py <log>` prints per-metric means, medians, p95, the A→B change with a
   95 % interval (Welch over phases, and paired over the (2k, 2k+1) pairs), and a verdict: **better / worse /
   no change** (inside the noise floor, `project.json ab.noise_pct`), **inconclusive** (more phases needed)
   or **not enough data**. Its scene check compares the display-list calls per frame of the two sides and
   says when they did not see comparable scenes. `--from-phase N` drops early phases (r53 logs, whose first
   phases were the title screen and an area load).
3. `python3 tools/runner/ab_report.py --baseline contracts/perf_baseline.json <log>` checks a plain
   (non-A/B) capture against the recorded baseline and fails on a regression beyond the noise floor.

Inconclusive is a result, not a failure of the tool: it means the scene varied more than the change is big.
Stand still in one spot, or capture longer. Where the time goes inside a side is in the stage lines of the
same log (`[lite2]`/`[lite3]` for the CPU path, `[lite5]` for the GPU transform path's own CPU cost).

Record console, area, build, `nectar.ini` preset and what was on screen. A host-side timing (the PC runs
the same code) is a smoke test, never evidence.

## 7. Running the checks

```
n3ds_port/tools/runner/check.sh            # gates (R1-R11, T1/T2, exemptions, corpus clock), R11 screen, host tests
n3ds_port/tools/runner/check.sh --log L    # + A/B report / baseline check for a captured log
n3ds_port/tools/runner/check.sh --quick    # gates and their self-tests only (no compiler / picasso needed)
```
