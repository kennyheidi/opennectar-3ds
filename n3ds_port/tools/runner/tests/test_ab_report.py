"""ab_report.py on synthetic [ab] lines in the exact format n3ds_window.cpp prints (python3 -m pytest or python3 this file)."""
import io
import random
import sys
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ab_report  # noqa: E402

FMT = ("[ab] phase %d %c %s=%d frames %d | frame_ms mean %.2f p50 %.2f p95 %.2f | game_ms mean %.2f p50 %.2f p95 %.2f"
       " | wait_ms mean %.2f p50 %.2f p95 %.2f | worker_ms mean %.2f p50 %.2f p95 %.2f | dl_calls %.0f")
R53 = ("[ab] phase %d %c %s=%d frames %d | frame_ms mean %.2f p50 %.2f p95 %.2f | game_ms mean %.2f p50 %.2f p95 %.2f"
       " | wait_ms mean %.2f p50 %.2f p95 %.2f | worker_ms mean %.2f p50 %.2f p95 %.2f")   # r53: no dl_calls, order A B A B


def is_b(i: int, order: str) -> bool:
    return (i % 2 == 1) if order == "ABAB" else (i % 4 in (1, 2))      # r54 console order: A B B A


def synth(n_pairs: int, a_ms: float, b_ms: float, noise: float, seed: int = 1, order: str = "ABBA",
          drift_ms: float = 4.0, dl_a: float = 500.0, dl_b: float = 500.0) -> str:
    rnd = random.Random(seed)
    lines = []
    for i in range(2 * n_pairs):
        b = is_b(i, order)
        drift = drift_ms * i / max(1, 2 * n_pairs)        # the scene gets busier over the session
        fr = (b_ms if b else a_ms) + drift + rnd.gauss(0, noise)
        vals = (i, "B" if b else "A", "gpu_xform", 1 if b else 0, 240, fr, fr - 0.5, fr + 6,
                fr * 0.4, fr * 0.4, fr * 0.5, fr * 0.6, fr * 0.6, fr * 0.7, fr * 0.9, fr * 0.9, fr)
        lines.append(FMT % (vals + (dl_b if b else dl_a,)) if order == "ABBA" else R53 % vals)
    return "\n".join(lines) + "\n"


def run(text: str):
    phases = ab_report.parse_ab(text)
    buf = io.StringIO()
    with redirect_stdout(buf):
        rc = ab_report.report_ab(phases, False)
    return rc, buf.getvalue(), phases


def test_parses_every_phase_and_metric():
    _, _, ph = run(synth(5, 50, 40, 1.0))
    assert len(ph) == 10 and all(set(ab_report.METRICS) <= set(p) for p in ph)


def test_clear_win_is_better():
    rc, out, _ = run(synth(6, 50.0, 40.0, 1.0))
    line = [l for l in out.splitlines() if l.strip().startswith("frame_ms")][0]
    assert line.endswith("better"), out
    assert rc == 0


def test_clear_loss_is_worse_and_fails():
    rc, out, _ = run(synth(6, 40.0, 50.0, 1.0))
    assert "worse" in out and rc == 1


def test_same_is_no_change_or_inconclusive_never_a_win():
    _, out, _ = run(synth(10, 45.0, 45.0, 0.3))
    line = [l for l in out.splitlines() if l.strip().startswith("frame_ms")][0]
    assert line.endswith("no change") or line.endswith("inconclusive"), out


def test_r53_format_still_parses():
    _, out, ph = run(synth(6, 50.0, 40.0, 1.0, order="ABAB"))
    assert len(ph) == 12 and "dl_calls" not in ph[0] and "scene check" not in out


def test_abba_pairs_cancel_a_strong_drift():
    # equal sides, a scene that gets 30 ms busier over the session: the A B B A pairs must not invent a difference
    phases = ab_report.parse_ab(synth(12, 60.0, 60.0, 0.2, drift_ms=30.0))
    d, lo, hi, n = ab_report.paired(phases, "frame_ms")
    assert n == 12 and lo < 0.0 < hi and abs(d) < 0.5, (d, lo, hi)


def test_scene_check_flags_different_scenes():
    _, out, _ = run(synth(6, 50.0, 40.0, 1.0, dl_a=500.0, dl_b=300.0))
    assert "NOT comparable" in out, out
    _, out, _ = run(synth(6, 50.0, 40.0, 1.0, dl_a=500.0, dl_b=480.0))
    assert "comparable scenes" in out and "NOT" not in out, out


def test_from_phase_and_discards(tmp=None):
    import tempfile, os
    text = synth(6, 50.0, 40.0, 1.0) + "[ab] phase 3 A discarded at frame 900: load hitch (812 ms, 40 display-list calls); running it again\n"
    fd, path = tempfile.mkstemp(suffix=".txt"); os.write(fd, text.encode()); os.close(fd)
    try:
        buf = io.StringIO()
        with redirect_stdout(buf):
            ab_report.main([path, "--from-phase", "4"])
        out = buf.getvalue()
        assert "(4 phases), B = 1 (4 phases), 0 discarded" in out, out
        buf = io.StringIO()
        with redirect_stdout(buf):
            ab_report.main([path])
        assert "1 discarded" in buf.getvalue()
    finally:
        os.remove(path)


def test_too_few_phases():
    _, out, _ = run(synth(2, 50.0, 40.0, 1.0))
    line = [l for l in out.splitlines() if l.strip().startswith("frame_ms")][0]
    assert line.endswith("not enough data"), out


if __name__ == "__main__":
    fails = 0
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            try:
                fn(); print("ok  ", name)
            except AssertionError as e:
                fails += 1; print("FAIL", name, e)
    sys.exit(1 if fails else 0)
