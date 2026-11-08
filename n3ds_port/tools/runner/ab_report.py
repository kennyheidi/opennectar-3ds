#!/usr/bin/env python3
"""ab_report.py -- A/B verdicts from the console's own log.txt (n3ds_port/AGENTS.md section 6).

    python3 ab_report.py LOG [--from-phase N] [--json]
    python3 ab_report.py --baseline contracts/perf_baseline.json LOG

A/B mode reads the "[ab] phase N A|B key=value frames n | metric mean m p50 x p95 y | ..." lines that
nectar.ini ab_test = 1 makes the console write (n3ds_window.cpp). One phase = one sample (its mean): the
phases interleave A B A B in one play session, so comparing phase means keeps scene drift and the
autocorrelation of consecutive frames out of the statistics. For every metric it prints the A and B
phase means, medians and p95s, the change B vs A with a 95 % interval (Welch's t on the phase means,
and a paired one on consecutive A/B pairs), and a verdict:

    better / worse          the whole 95 % interval is beyond the noise floor (project.json ab.noise_pct)
    no change               the whole interval is inside the noise floor
    inconclusive            neither: more phases are needed
    not enough data         fewer than ab.min_windows phases on a side

All metrics are milliseconds per frame: lower is better.

r54: the console orders the phases A B B A A B B A ... and pairs (0,1) (2,3) ... each hold one A and one B, so the paired interval is
taken over those pairs whatever their order (a scene that drifts then affects both sides alike). Phases it discarded (load hitch, menu)
are counted from their "[ab] phase N X discarded" lines. "dl_calls" (display-list calls per frame) is a scene check: when the two sides
differ by more than 15 %, they did not see comparable scenes and the report says so. --from-phase N drops the phases before N (for r53
logs, whose first phases were the title screen and an area load).

Baseline mode reads the plain "[perf]" / "[perf-2core]" lines of a normal capture and compares their
median to contracts/perf_baseline.json (same scene only -- the file names it). It exits 1 on a
regression beyond the noise floor, so it can gate.

Exit codes: 0 ok / better / no change, 1 worse or regression, 2 usage or no data.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import sys
from pathlib import Path

RUNNER = Path(__file__).resolve().parent
PROJECT = json.loads((RUNNER / "contracts" / "project.json").read_text(encoding="utf-8"))
AB = PROJECT.get("ab", {})
NOISE_PCT = float(AB.get("noise_pct", 3.0))
MIN_WINDOWS = int(AB.get("min_windows", 4))
METRICS = ("frame_ms", "game_ms", "wait_ms", "worker_ms")

AB_RE = re.compile(r"\[ab\] phase (\d+) ([AB]) (\S+)=(-?\d+) frames (\d+) \|(.*)")
MET_RE = re.compile(r"(\w+_ms) mean (-?[\d.]+) p50 (-?[\d.]+) p95 (-?[\d.]+)")
DL_RE = re.compile(r"\| dl_calls ([\d.]+)")
DISCARD_RE = re.compile(r"\[ab\] phase (\d+) ([AB]) discarded")
PERF_RE = re.compile(r"\[perf\] frames (\d+)-(\d+): ([\d.]+) ms/frame")
PERF2_RE = re.compile(r"\[perf-2core\] game thread \(core 0\): ([\d.]+) ms/frame of work \+ ([\d.]+) ms waiting for the worker \| worker \(core 2\): ([\d.]+) ms/frame busy")

# two-sided 95 % Student t quantiles (df 1..30); larger df -> 1.96
_T95 = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131,
        2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]


def t95(df: float) -> float:
    if df < 1:
        return float("inf")
    i = int(math.floor(df))
    return _T95[i - 1] if i <= 30 else 1.96


def parse_ab(text: str) -> list[dict]:
    phases = []
    for m in AB_RE.finditer(text):
        ph = {"phase": int(m.group(1)), "side": m.group(2), "key": m.group(3), "value": int(m.group(4)), "frames": int(m.group(5))}
        for mm in MET_RE.finditer(m.group(6)):
            ph[mm.group(1)] = {"mean": float(mm.group(2)), "p50": float(mm.group(3)), "p95": float(mm.group(4))}
        d = DL_RE.search(m.group(6))
        if d:
            ph["dl_calls"] = float(d.group(1))
        phases.append(ph)
    return phases


def welch(a: list[float], b: list[float]) -> tuple[float, float, float]:
    """(difference of means b - a, lower, upper) of the 95 % interval."""
    ma, mb = statistics.fmean(a), statistics.fmean(b)
    va, vb = statistics.variance(a), statistics.variance(b)
    se2 = va / len(a) + vb / len(b)
    if se2 <= 0:
        return mb - ma, mb - ma, mb - ma
    df = se2 ** 2 / ((va / len(a)) ** 2 / (len(a) - 1) + (vb / len(b)) ** 2 / (len(b) - 1))
    h = t95(df) * math.sqrt(se2)
    return mb - ma, mb - ma - h, mb - ma + h


def paired(phases: list[dict], metric: str) -> tuple[float, float, float, int] | None:
    """Phases (2k, 2k+1) as pairs, in either order (A B or B A): mean of B - A with its 95 % interval."""
    d = []
    by = {p["phase"]: p for p in phases}
    for k in sorted(by):
        if k % 2:
            continue
        x, y = by.get(k), by.get(k + 1)
        if not x or not y or x["side"] == y["side"] or metric not in x or metric not in y:
            continue
        a, b = (x, y) if x["side"] == "A" else (y, x)
        d.append(b[metric]["mean"] - a[metric]["mean"])
    if len(d) < 2:
        return None
    m = statistics.fmean(d)
    h = t95(len(d) - 1) * statistics.stdev(d) / math.sqrt(len(d))
    return m, m - h, m + h, len(d)


def verdict(base: float, lo: float, hi: float) -> str:
    if base <= 0:
        return "inconclusive"
    lo_p, hi_p = 100.0 * lo / base, 100.0 * hi / base
    if hi_p < -NOISE_PCT:
        return "better"
    if lo_p > NOISE_PCT:
        return "worse"
    if lo_p >= -NOISE_PCT and hi_p <= NOISE_PCT:
        return "no change"
    return "inconclusive"


def report_ab(phases: list[dict], as_json: bool, discards: int = 0) -> int:
    if not phases:
        print("no [ab] lines in the log (nectar.ini ab_test = 1)")
        return 2
    keys = {(p["key"]) for p in phases}
    a = [p for p in phases if p["side"] == "A"]
    b = [p for p in phases if p["side"] == "B"]
    out = {"key": ",".join(sorted(keys)), "a_value": a[0]["value"] if a else None, "b_value": b[0]["value"] if b else None,
           "phases_a": len(a), "phases_b": len(b), "discarded": discards, "noise_pct": NOISE_PCT, "metrics": {}}
    dla = [p["dl_calls"] for p in a if "dl_calls" in p]
    dlb = [p["dl_calls"] for p in b if "dl_calls" in p]
    if dla and dlb:
        ma, mb = statistics.fmean(dla), statistics.fmean(dlb)
        out["dl_calls"] = {"a": ma, "b": mb, "comparable": abs(mb - ma) <= 0.15 * max(ma, mb, 1.0)}
    worst = 0
    for metric in METRICS:
        av = [p[metric]["mean"] for p in a if metric in p]
        bv = [p[metric]["mean"] for p in b if metric in p]
        r = {"a_mean": statistics.fmean(av) if av else None, "b_mean": statistics.fmean(bv) if bv else None,
             "a_p50": statistics.median([p[metric]["p50"] for p in a if metric in p]) if av else None,
             "b_p50": statistics.median([p[metric]["p50"] for p in b if metric in p]) if bv else None,
             "a_p95": statistics.median([p[metric]["p95"] for p in a if metric in p]) if av else None,
             "b_p95": statistics.median([p[metric]["p95"] for p in b if metric in p]) if bv else None}
        if len(av) < MIN_WINDOWS or len(bv) < MIN_WINDOWS:
            r["verdict"] = "not enough data"
        else:
            d, lo, hi = welch(av, bv)
            base = statistics.fmean(av)
            r.update({"diff": d, "lo": lo, "hi": hi, "pct": 100.0 * d / base if base else 0.0, "verdict": verdict(base, lo, hi)})
            pr = paired(phases, metric)
            if pr:
                r.update({"paired_diff": pr[0], "paired_lo": pr[1], "paired_hi": pr[2], "pairs": pr[3], "paired_verdict": verdict(base, pr[1], pr[2])})
            if r["verdict"] == "worse":
                worst = 1
        out["metrics"][metric] = r
    if as_json:
        json.dump(out, sys.stdout, indent=2)
        print()
        return worst
    print(f"A/B {out['key']}: A = {out['a_value']} ({len(a)} phases), B = {out['b_value']} ({len(b)} phases), {discards} discarded by the console; noise floor +-{NOISE_PCT:g} %; lower is better")
    if "dl_calls" in out:
        dc = out["dl_calls"]
        note = "comparable scenes" if dc["comparable"] else "NOT comparable scenes (more than 15 % apart): treat the verdicts below with suspicion"
        print(f"  scene check: display-list calls per frame A {dc['a']:.0f}, B {dc['b']:.0f} -> {note}")
    print(f"  {'metric':10} {'A mean':>8} {'B mean':>8} {'A p50':>7} {'B p50':>7} {'A p95':>7} {'B p95':>7}   {'B vs A (95 % interval)':32} verdict")
    for metric, r in out["metrics"].items():
        if r["a_mean"] is None or r["b_mean"] is None:
            continue
        ci = "" if "diff" not in r else f"{r['diff']:+7.2f} ms ({r['pct']:+5.1f} %) [{r['lo']:+.2f}, {r['hi']:+.2f}]"
        print(f"  {metric:10} {r['a_mean']:8.2f} {r['b_mean']:8.2f} {r['a_p50']:7.2f} {r['b_p50']:7.2f} {r['a_p95']:7.2f} {r['b_p95']:7.2f}   {ci:32} {r['verdict']}")
        if "paired_diff" in r:
            print(f"  {'':10} paired over {r['pairs']} consecutive A/B pairs: {r['paired_diff']:+.2f} ms [{r['paired_lo']:+.2f}, {r['paired_hi']:+.2f}] -> {r['paired_verdict']}")
    return worst


def report_baseline(baseline_path: Path, text: str) -> int:
    base = json.loads(baseline_path.read_text(encoding="utf-8"))
    frames = [float(m.group(3)) for m in PERF_RE.finditer(text) if int(m.group(1)) > int(base.get("skip_frames", 600))]
    two = [(float(m.group(1)), float(m.group(2)), float(m.group(3))) for m in PERF2_RE.finditer(text)]
    if not frames:
        print("no [perf] lines past the warm-up in the log")
        return 2
    now = {"frame_ms": statistics.median(frames)}
    if two:
        now["game_ms"] = statistics.median(t[0] for t in two)
        now["wait_ms"] = statistics.median(t[1] for t in two)
        now["worker_ms"] = statistics.median(t[2] for t in two)
    print(f"baseline {base.get('name')} ({base.get('scene')}, {base.get('build')}): median of the capture's [perf] windows vs the baseline; noise floor +-{NOISE_PCT:g} %")
    rc = 0
    for metric, val in now.items():
        ref = base.get("metrics", {}).get(metric)
        if ref is None:
            continue
        pct = 100.0 * (val - ref) / ref
        v = "worse" if pct > NOISE_PCT else "better" if pct < -NOISE_PCT else "no change"
        if v == "worse":
            rc = 1
        print(f"  {metric:10} baseline {ref:7.2f}  now {val:7.2f}  ({pct:+5.1f} %)  {v}")
    print("  (a baseline comparison is only meaningful in the same scene; decisions need an interleaved A/B capture)")
    return rc


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("log", type=Path)
    ap.add_argument("--baseline", type=Path, default=None)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--from-phase", type=int, default=0, help="ignore phases before this one")
    args = ap.parse_args(argv)
    try:
        text = args.log.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        print(f"cannot read {args.log}: {e}")
        return 2
    if args.baseline:
        return report_baseline(args.baseline, text)
    phases = [p for p in parse_ab(text) if p["phase"] >= args.from_phase]
    discards = sum(1 for m in DISCARD_RE.finditer(text) if int(m.group(1)) >= args.from_phase)
    return report_ab(phases, args.json, discards)


if __name__ == "__main__":
    raise SystemExit(main())
