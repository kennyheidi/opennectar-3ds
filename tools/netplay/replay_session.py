"""Replay a netplay session input log offline, on one machine.

Every launcher session (--netplay-host-ice / --netplay-join-ice) writes
session-inputs.pknl into its run folder, and a UDP session does when
PIKMIN_NETPLAY_INPUT_LOG=<file> is set: both players' confirmed inputs for
every frame, plus that game's state hash after each frame
(pc_port/netplay/pc_netplay_inlog.h). This tool starts the netplay exe with
--netplay-replay-log <log>: the same session driver and per-Advance body run
with no network, one logged frame at a time, from the same start (the co-op
direct boot on a fresh private save, the recorded netplay seed, captains and
sim settings, which the replay adopts from the log). The exe compares its
own hash with the recorded one every frame.

Nothing is simulated by this script; it builds a private run directory and
launches one hidden, bounded process (only that PID is ever signalled).

Exit code: the exe's (0 every recorded hash reproduced, 8 a mismatch, whose
first frame is printed). With --expect the script checks the outcome:
  match               every recorded hash reproduced
  mismatch[:FRAME]    a mismatch (first at FRAME when given): the replay
                      reproduces the other peer's game, so a desynced peer's
                      log diverges where that peer's sim diverged

    py -3.12 tools/netplay/replay_session.py --exe <netplay nectar.exe> \\
        --assets <assets> --log <run>/session-inputs.pknl --out <scratch dir>
"""

import argparse
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_pair as rp  # noqa: E402


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--exe", type=Path, required=True, help="netplay nectar.exe (the one that recorded)")
    p.add_argument("--log", type=Path, required=True, help="session-inputs.pknl")
    p.add_argument("--assets", type=Path, required=True, help="extracted game files (the directory holding dataDir/)")
    p.add_argument("--out", type=Path, required=True, help="private output directory")
    p.add_argument("--compare-hashes", type=Path, default=None,
                   help="a recorded hashes.txt to diff the replay's own hash log against")
    p.add_argument("--expect", default=None, help="match, mismatch or mismatch:FRAME")
    p.add_argument("--timeout", type=float, default=900)
    p.add_argument("--env", nargs="*", default=[], metavar="K=V")
    a = p.parse_args(argv)

    run = a.out.resolve()
    assets = a.assets.resolve()
    if not (assets / "dataDir").is_dir():
        raise SystemExit(f"replay_session: {assets} has no dataDir/")
    run.mkdir(parents=True, exist_ok=True)
    rp.link_assets(assets, run / "assets")
    save = run / "save"
    if save.exists():
        for f in save.rglob("*"):
            if f.is_file():
                f.unlink()
    save.mkdir(exist_ok=True)
    for stale in ("hashes.txt", "native.log", "pikmin_settings.conf"):
        if (run / stale).exists():
            (run / stale).unlink()
    print(f"replay_session: exe={a.exe.resolve()} sha256={rp.sha256_file(a.exe)}")
    print(f"replay_session: log={a.log.resolve()}")
    env = dict(PIKMIN_NETPLAY_BANNER_MS="0", PIKMIN_NETPLAY_INPUT_LOG="0", **rp.parse_kv(a.env, "env"))
    # run_pair.launch sets the private save dir, the hash log and unthrottled
    # pacing; its tick bound is irrelevant here (the log ends the replay).
    proc, out = rp.launch(a.exe, run, ["--netplay-replay-log", str(a.log.resolve())], env, True, 10 ** 9)
    print(f"replay_session: pid {proc.pid}, log {run / 'native.log'}")
    start = time.time()
    rcs = rp.wait_all([("replay", proc)], a.timeout)
    out.close()
    rc = rcs["replay"]
    text = (run / "native.log").read_text(errors="replace") if (run / "native.log").exists() else ""
    for ln in text.splitlines():
        if "[netplay] replay:" in ln:
            print(f"replay_session:   {ln.strip()}")
    print(f"replay_session: exit={rc} in {time.time() - start:.1f}s")
    ok = rc in (0, 8)
    m = re.search(r"first hash mismatch at frame=(\d+)", text)
    first = int(m.group(1)) if m else None
    if a.compare_hashes is not None:
        same, n, tick, cols = rp.compare_hashes(a.compare_hashes, run / "hashes.txt")
        print(f"replay_session: hash log vs {a.compare_hashes}: {'identical' if same else 'differs'} over {n} ticks"
              + ("" if same else f", first differing tick {tick} columns {cols}"))
    if a.expect:
        want = a.expect.split(":")
        if want[0] == "match":
            ok = rc == 0
        elif want[0] == "mismatch":
            ok = rc == 8 and (len(want) < 2 or first == int(want[1]))
        else:
            raise SystemExit(f"replay_session: bad --expect {a.expect}")
        print(f"replay_session: {'PASS' if ok else 'FAIL'} (expect {a.expect}; first mismatch frame={first})")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
