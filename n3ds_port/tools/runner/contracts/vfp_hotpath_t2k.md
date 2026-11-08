# VFP Hot-Path Contract

**Target:** Nintendo 3DS (ARM11 MPCore, VFP11 / VFPv2)
**Scope:** all T2K source compiled into the 3DS build — shared and
3DS-specific alike. See §7.
**Status:** Enforceable — agents MUST report violations by rule ID

---

## 0. Rationale

The ARM11 MPCore has a real hardware FPU (VFP11, VFPv2, IEEE 754). Raw
floating-point arithmetic is not the problem. Three things are:

1. **The coprocessor boundary.** Moving a value between the ARM integer
   register file and the VFP register file costs 20+ cycles. Anything that
   forces a round trip per iteration dominates the loop.
2. **Trap-to-support-code on exceptional values.** Denormals stall hard
   unless flush-to-zero is enabled. Decay loops generate denormals by
   construction.
3. **Register file size.** 32 single-precision registers. Doubles consume
   two. Spills are memory traffic on a core with a small cache.

This contract targets those three things specifically. It does **not**
prohibit floating-point math, and it does **not** prohibit mixing integer
and float types in the same function.

---

## 1. Definitions

**HOT PATH** — a function or loop body that satisfies at least one of:
- Executes per-frame at a rate ≥ 1× frame count, AND appears in the top 15
  entries of a captured profile, OR
- Executes ≥ 256 times per frame regardless of profile rank, OR
- Is explicitly annotated `/* @hotpath */`

Nothing in this contract applies to code that is not HOT PATH. See §5.

**BOUNDARY CROSSING** — any operation that moves a value between the ARM
integer register file and the VFP register file, or that requires a VFP
status flag to be read by ARM-side control flow. Includes:
- `vmov` / `vmrs` between register files
- float→int and int→float conversion (`vcvt`)
- float comparison whose result drives a branch
- float value used as an array index

**COLD PATH** — everything else. Init, level load, menu logic, texture
generation, parse, teardown.

---

## 2. Rules

Severity: **MUST** = build-blocking violation. **SHOULD** = flag for review.

### R1 — FPSCR configuration (MUST)

Flush-to-Zero and Default-NaN MUST be set in `FPSCR` on every thread that
executes float math, at thread entry, before any hot path runs.

Every FPSCR helper and every call site MUST carry a comment stating the VFPU
rationale: denormals stall the VFP11 unless FZ is set, decay loops generate
denormals by construction, FPSCR is per-thread so each worker needs its own
setup. Uncommented FPSCR code is a MUST finding — the next reader must not
have to reverse-engineer why the asm is there.

### R2 — No float→int conversion in a hot loop body (MUST)

### R3 — No float comparison driving control flow in a hot loop (MUST)

### R4 — No double precision in hot paths (MUST)

### R5 — Loop counters and indices MUST be integer

### R6 — No transcendentals in hot paths (MUST)

### R7 — Division and sqrt SHOULD be avoided in hot paths

### R8 — Fixed-point domains MUST NOT implicitly promote

### R9 — VFP register pressure SHOULD stay under budget

### R11 — Integer division/modulo by a runtime value (SHOULD)

ARMv6 has **no integer divide instruction**. A runtime `%` or `/` on integers
is a `bl __aeabi_idivmod` / `__aeabi_idiv`: tens of cycles plus a call barrier
that spills the loop's live registers — categorically worse than R7's float
division, since VFPv2 has hardware `vdiv`. A compile-time-constant divisor is
NOT a finding (GCC emits a magic-number multiply).

`((x % n) + n) % n` is two calls and is usually wrapping a value at most one
period out of range; where that bound is provable, a conditional add/subtract
is an exact integer identity. **Not yet detected by `gates_vfp` — hand-check.**
Added 2026-09-07; see `docs/validation/arm11-second-opinion-verdict-2026-09-07.md`.

### R10 — Build flags MUST be verified, not assumed

`t2k_3ds/Makefile` must contain
`ARCH := -march=armv6k -mtpcs-leaf-frame -mfloat-abi=hard -mtune=mpcore -mfpu=vfp`
and `-fno-math-errno`.

---

## 3. Explicit non-rules

The following are **NOT** violations and MUST NOT be reported:

- An integer loop counter driving float arithmetic in the body. This is
  correct and idiomatic.
- Mixed int and float types in the same *function*, where the hot loop
  itself stays in one domain.
- Float math in COLD PATH code of any shape.
- Float comparisons outside a loop.
- Any pattern in code the profile does not show as hot.

---

## 4. Exemptions

An exemption is granted inline:

```c
/* @vfp-exempt R3 — predicate depends on per-vertex data that cannot be
   hoisted; measured cost 0.4ms, accepted. Verified 2026-01-01. */
```

Grammar:

```
/* @vfp-exempt R<id>[,R<id>...] — <reason>. measured <cost-or-n/a>. Verified <YYYY-MM-DD>. */
```

Standing exemptions (corpus, not source comments):

| ID | File | Rule | Standing | Why |
|---|---|---|---|---|
| E-LUT-1 | t2k_core/src/game/math_lut.h:fastSin | R2,R3,R5 | yes | Range reduction; leaf vcvt+flag; documented cheaper than floorf |
| E-LUT-2 | t2k_core/src/game/math_lut.h:fastAtan2 | R3,R7 | yes | Octant fold requires float predicates; one vdiv is the atan2 |
| E-LUT-3 | t2k_core/src/game/math_lut.cpp:mathLutInit | R4,R6 | yes | COLD table build; libm allowed |
| E-MECH-1 | t2k_core/src/game/engine.cpp spawn-count trig | R4,R6 | yes | DOUBLE mechanics; not HOT PATH |
| E-TEX-1 | t2k_core/src/rendering/textures.cpp | all | yes | COLD procedural DSL |
| E-FFT-1 | t2k_core/src/audio/fft.cpp | R6,R7 | yes | Own twiddle tables; audio worker, not render hot path unless profile says so |

## 5. Applicability

If a function is not HOT PATH, emit nothing.

## 6. Profile ingest

HOT PATH = union of `/* @hotpath */`, `hotpath_corpus.json` seed, parsed
`docs/validation/perf-*.log` top-15 stages, and static ≥256×/frame loops.

## 7. Scope

Include: `t2k_core/src/**`, `t2k_3ds/src/**`, `t2k_3ds/Makefile`.
Exclude: `t2k_pc/**`, `t2k_core/third_party/**`, `t2k_core/tools/**`, `t2k_3ds/tools/**`.

## 8. Report schema

Every finding has `rule`, `severity`, `file`, `line`, `symbol`, `hotpath`,
`hotpath_reason`, `exemption`, `detail`.
FAIL iff any unexempted MUST finding.
