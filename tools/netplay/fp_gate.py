"""FP determinism gate for netplay builds.

Usage: fp_gate.py [--objdump PATH] <exe> [<exe> ...]

Runs objdump over the linked game executable(s) and FAILS if it finds:
  - an x87 transcendental instruction (fsin/fcos/fsincos/fpatan/fptan/
    f2xm1/fyl2x/fyl2xp1) -- mingw-w64 libm implements its transcendentals
    with these, and their microcode results can differ between CPU vendors;
  - an x87 fsqrt anywhere (the CRT sqrt is an x87 fsqrt whose rounding
    depends on the x87 precision-control field; the libm uses SSE sqrtsd);
  - a call to a CRT sqrt/sqrtf from inside a pc_netplay_libm function
    (the same issue, caught at call level);
  - an FMA instruction (vfmadd*/vfmsub*/vfnmadd*) -- the netplay build uses
    -ffp-contract=off, so none may appear (the x86-64 baseline has no FMA);
  - an import of tan/sin/cos/atan/exp/log/pow/hypot (any float/double/
    long-double or atan2/sincos spelling, plus log10/log2/exp2/sinh/cosh/
    tanh/cbrt/log1p/expm1 families) from msvcrt/ucrt -- e.g. tan resolved
    to the system DLL, which varies with the Windows version.

Allowed exceptions are listed in ALLOW below, each with the symbol, the
instruction and the reason. Only code that never feeds the simulation may
be listed.

Exit 0 when every exe is clean, 1 with diagnostics otherwise. Each hit is
attributed to its symbol via `nm` so the report names the libm function.
"""

import bisect
import re
import shutil
import subprocess
import sys
from pathlib import Path

X87 = {"fsin", "fcos", "fsincos", "fpatan", "fptan", "f2xm1", "fyl2x", "fyl2xp1",
       "fsqrt"}

# (symbol, insn) -> reason. Documents the exceptional hits the gate lets
# through, e.g. CRT startup helpers. Empty: no exception is needed.
# (symbol regex, insn) -> reason.
ALLOW = {
    # jaudio_host's level report (pc_port/audio/jaudio_host.cpp): an RMS in
    # long double, i.e. sqrtl, inlined into the audio thread. Log output only.
    (r"(_ZN12_GLOBAL__N_1L9pumpAudioEv.*|StopAudioThread|sqrtl)", "fsqrt"):
        "audio level statistics (long double RMS), never read by the simulation",
}


def allowed(sym, insn):
    return any(i == insn and re.fullmatch(rx, sym) for rx, i in ALLOW)

FMA_RE = re.compile(r"\bvfmadd\w*|\bvfmsub\w*|\bvfnmadd\w*|\bvfnmsub\w*", re.IGNORECASE)

MATH_IMPORT_STEMS = {
    "tan", "tanf", "sin", "sinf", "cos", "cosf", "sincos", "sincosf",
    "atan", "atanf", "atan2", "atan2f", "asin", "asinf", "acos", "acosf",
    "exp", "expf", "log", "logf", "pow", "powf",
    "hypot", "hypotf", "log10", "log10f", "log2", "log2f", "exp2", "exp2f",
    "sinh", "sinhf", "cosh", "coshf", "tanh", "tanhf", "cbrt", "cbrtf",
    "log1p", "log1pf", "expm1", "expm1f",
}

# Symbols owned by pc_netplay_libm.c: public entry points plus pc_* helpers.
# A call to a CRT sqrt from one of these is a regression even if the x87
# fsqrt itself lives in another object.
LIBM_PUBLIC = {
    "sinf", "cosf", "sincosf", "tanf", "atanf", "atan2f", "asinf", "acosf",
    "expf", "logf", "log10f", "log2f", "exp2f", "powf", "fmodf", "hypotf",
    "sqrtf", "sin", "cos", "sincos", "tan", "asin", "acos", "atan", "atan2",
    "exp", "log", "log10", "log2", "exp2", "pow", "fmod", "hypot", "sqrt",
}

SQRT_CALL_RE = re.compile(r"\bsqrtf?\b", re.IGNORECASE)


def is_libm_owner(sym):
    return sym.startswith("pc_") or sym in LIBM_PUBLIC

CRT_DLLS = ("msvcrt", "ucrt", "api-ms-win-crt")


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        raise SystemExit(f"fp_gate: command failed: {' '.join(cmd)}\n{p.stderr[:2000]}")
    return p.stdout


def main(argv):
    args = list(argv[1:])
    objdump = None
    if args[:1] == ["--objdump"]:
        if len(args) < 3:
            print(__doc__)
            return 2
        objdump = args[1]
        args = args[2:]
    if not args:
        print(__doc__)
        return 2
    if objdump is None:
        objdump = shutil.which("objdump")
    if not objdump:
        print("fp_gate: FAIL: objdump not on PATH (msys2 mingw64 provides it;"
              " cmake passes ${CMAKE_OBJDUMP} via --objdump)")
        return 1

    overall_fail = 0
    for exe_arg in args:
        rc = gate_one(Path(exe_arg), objdump)
        if rc != 0:
            overall_fail = 1
    return overall_fail


def gate_one(exe, objdump):
    if not exe.is_file():
        print(f"fp_gate: FAIL: no such file: {exe}")
        return 1
    dis = run([objdump, "-d", str(exe)])
    syms = []
    try:
        nm_out = run([shutil.which("nm") or "nm", str(exe)])
        for line in nm_out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "TtWwRrDd":
                try:
                    syms.append((int(parts[0], 16), parts[2]))
                except ValueError:
                    pass
        syms.sort()
    except SystemExit:
        syms = []
    addrs = [a for a, _ in syms]
    names = [n for _, n in syms]

    def owner(addr):
        if not addrs:
            return "?"
        i = bisect.bisect_right(addrs, addr) - 1
        return names[i] if i >= 0 else "?"

    fails = []
    counts = {}
    addr_re = re.compile(r"^\s*([0-9a-fA-F]+):\s+(?:[0-9a-fA-F]{2} )+\s*(\S+)")
    for line in dis.splitlines():
        m = addr_re.match(line)
        if not m:
            continue
        insn = m.group(2).lower()
        if insn in X87 or FMA_RE.match(insn):
            addr = int(m.group(1), 16)
            sym = owner(addr)
            if allowed(sym, insn):
                continue
            kind = "x87" if insn in X87 else "fma"
            fails.append(f"{kind}: {insn} at {m.group(1)} in <{sym}>")
            counts[insn] = counts.get(insn, 0) + 1
            continue
        # Call-level backstop: a CRT sqrt/sqrtf call from inside
        # a pc_netplay_libm function means the SSE helper was bypassed.
        if insn in ("call", "callq", "jmp"):
            addr = int(m.group(1), 16)
            sym = owner(addr)
            if is_libm_owner(sym) and SQRT_CALL_RE.search(line):
                fails.append(f"libm-sqrt-call: {line.strip()} in <{sym}>")

    imports = run([objdump, "-p", str(exe)])
    cur_dll = ""
    for line in imports.splitlines():
        dm = re.match(r"\s*DLL Name:\s*(\S+)", line)
        if dm:
            cur_dll = dm.group(1).lower()
            continue
        parts = line.split()
        # objdump -p import rows: vma, ordinal/hint, type?, name
        if len(parts) >= 4 and cur_dll:
            name = parts[-1].strip().lower().lstrip("_")
            # strip stdcall decorations (@N) and C++ mangling guard
            name = re.sub(r"@\d+$", "", name)
            is_crt = cur_dll.startswith(CRT_DLLS)
            if is_crt and name in MATH_IMPORT_STEMS:
                fails.append(f"msvcrt-import: {parts[-1].strip()} from {cur_dll}")

    if fails:
        print(f"fp_gate: FAIL: {exe}")
        print(f"fp_gate: {len(fails)} forbidden reference(s):")
        for f in fails[:50]:
            print(f"  {f}")
        if len(fails) > 50:
            print(f"  ... and {len(fails) - 50} more")
        if counts:
            print("fp_gate: insn counts: " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
        return 1
    print(f"fp_gate: PASS: {exe} (no x87, no FMA, no msvcrt math imports, no libm sqrt calls)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
