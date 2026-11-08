#!/bin/sh
# Open Nectar 3DS contract check (n3ds_port/AGENTS.md section 7). Runs from anywhere; needs python3, plus g++/cc and picasso
# (devkitPro tools/bin) for the host tests -- a missing tool skips that step and says so, it never passes silently.
#
#   tools/runner/check.sh                 gates + R11 screen + gate self-tests + host tests
#   tools/runner/check.sh --quick         gates + self-tests only (no compiler needed)
#   tools/runner/check.sh --log LOG       + A/B report of LOG (ab_test = 1 capture) or, without [ab] lines, the baseline check
#   tools/runner/check.sh --log LOG --from-phase N   A/B report without the phases before N (r53 logs: title screen / area load)
#
# Exit 0 = every step that ran passed. A green run means "nothing the screens can see is wrong", NOT "faster":
# only an A/B capture on hardware says that (AGENTS.md section 6).
set -u
RUN=$(cd "$(dirname "$0")" && pwd)
PORT=$(cd "$RUN/../.." && pwd)
HT="$PORT/tools/host_test"
TMP=${TMPDIR:-/tmp}/nectar_check.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

QUICK=0; LOG=""; FROM=0
while [ $# -gt 0 ]; do
    case "$1" in
        --quick) QUICK=1 ;;
        --log) shift; LOG="${1:-}" ;;
        --from-phase) shift; FROM="${1:-0}" ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown option $1"; exit 2 ;;
    esac
    shift
done

FAIL=0; SKIP=0
step() { printf '\n== %s\n' "$1"; }
ok()   { echo "   ok: $1"; }
bad()  { echo "   FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "   skipped: $1"; SKIP=$((SKIP + 1)); }
have() { command -v "$1" >/dev/null 2>&1; }

PY=$(command -v python3 || command -v python || true)
[ -n "$PY" ] || { echo "python3 is needed"; exit 2; }

step "VFP / FPSCR / ABI gates (R1-R11, T1/T2, exemptions, corpus clock)"
if "$PY" "$RUN/gates_vfp.py" --out "$TMP/vfp_audit.json" > "$TMP/gates.txt" 2>&1; then
    grep -E '^(PASS|FAIL)' "$TMP/gates.txt" | head -1 | sed 's/^/   /'; ok "gates"
else
    sed 's/^/   /' "$TMP/gates.txt" | grep -E 'MUST|^   (PASS|FAIL)' | head -40; bad "gates (MUST findings above)"
fi

step "R11 integer divide / modulo screen (information: review new sites, pin or fix)"
"$PY" "$RUN/audit_r11_idiv.py" 2>&1 | grep -E '^ *n3ds_port/' | sed 's/^ */   /' | head -20

step "gate self-tests"
if (cd "$PORT" && "$PY" -m unittest -q tools.runner.tests.test_gates_vfp tools.runner.tests.test_audit_r11_idiv) > "$TMP/ut.txt" 2>&1; then
    tail -1 "$TMP/ut.txt" | sed 's/^/   /'; ok "gates_vfp / audit_r11_idiv tests"
else
    tail -20 "$TMP/ut.txt" | sed 's/^/   /'; bad "gate self-tests"
fi
if "$PY" "$RUN/tests/test_ab_report.py" > "$TMP/ab.txt" 2>&1; then ok "ab_report tests ($(grep -c '^ok' "$TMP/ab.txt"))"; else sed 's/^/   /' "$TMP/ab.txt"; bad "ab_report tests"; fi

if [ $QUICK = 0 ]; then
    step "host tests"
    if have g++; then
        if g++ -std=c++17 -I "$HT" -I "$PORT/platform" "$HT/tev_numeric_test.cpp" "$PORT/platform/n3ds_tev.cpp" -o "$TMP/tn" 2> "$TMP/tn.txt" && "$TMP/tn" > "$TMP/tn.txt" 2>&1; then
            ok "TEV numeric ($(tail -1 "$TMP/tn.txt"))"; else tail -5 "$TMP/tn.txt" | sed 's/^/   /'; bad "TEV numeric"; fi
        if g++ -std=c++17 -I "$HT" -I "$PORT/platform" "$HT/piki_tev_test.cpp" "$PORT/platform/n3ds_tev.cpp" -o "$TMP/pt" 2> "$TMP/pt.txt" && "$TMP/pt" > "$TMP/pt.txt" 2>&1; then
            ok "Pikmin TEV programs ($(tail -1 "$TMP/pt.txt"))"; else tail -5 "$TMP/pt.txt" | sed 's/^/   /'; bad "Pikmin TEV programs"; fi
        if g++ -std=c++17 -I "$PORT/platform" "$HT/preset_test.cpp" "$PORT/platform/n3ds_config.cpp" -o "$TMP/ps" 2> "$TMP/ps.txt" && "$TMP/ps" > "$TMP/ps.txt" 2>&1; then
            ok "presets ($(tail -1 "$TMP/ps.txt"))"; else tail -5 "$TMP/ps.txt" | sed 's/^/   /'; bad "presets"; fi
        if have picasso; then
            if (cd "$HT" && TMPDIR="$TMP" sh run_mesh.sh) > "$TMP/mesh.txt" 2>&1; then
                ok "GPU T&L vs CPU path, PICA interpreter ($(grep -E 'checks' "$TMP/mesh.txt" | tail -1))"
            else tail -15 "$TMP/mesh.txt" | sed 's/^/   /'; bad "GPU T&L vs CPU path (run_mesh.sh)"; fi
        else skip "run_mesh.sh: picasso not on PATH (devkitPro tools/bin)"; fi
    else skip "C++ host tests: g++ not on PATH"; fi
    CC=$(command -v cc || command -v gcc || true)
    if [ -n "$CC" ]; then
        if "$CC" -std=c99 -I "$PORT/tools/cia_installer" "$PORT/tools/cia_installer/guide_test.c" -o "$TMP/gt" 2> "$TMP/gt.txt" && "$TMP/gt" > "$TMP/gt.txt" 2>&1; then
            ok "installer guide fits the screen"; else tail -5 "$TMP/gt.txt" | sed 's/^/   /'; bad "installer guide"; fi
    else skip "installer guide: no C compiler"; fi
fi

if [ -n "$LOG" ]; then
    step "performance evidence: $LOG"
    if grep -q '^\[ab\] phase\|\] \[ab\] phase\|\[ab\] phase' "$LOG" 2>/dev/null; then
        "$PY" "$RUN/ab_report.py" "$LOG" --from-phase "$FROM"; rc=$?
        [ $rc = 1 ] && bad "A/B: B is worse than A"
        [ $rc = 2 ] && bad "A/B: no usable data"
    else
        "$PY" "$RUN/ab_report.py" --baseline "$RUN/contracts/perf_baseline.json" "$LOG"; rc=$?
        [ $rc = 1 ] && bad "regression against contracts/perf_baseline.json"
        [ $rc = 2 ] && bad "no [perf] data in the log"
    fi
fi

printf '\n'
if [ $FAIL = 0 ]; then echo "check: OK ($SKIP skipped)"; else echo "check: $FAIL FAILED ($SKIP skipped)"; fi
[ $FAIL = 0 ]
