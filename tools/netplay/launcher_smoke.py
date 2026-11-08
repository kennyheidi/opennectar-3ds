"""Scripted Host Co-op / Join Co-op through the launcher (netplay builds).

Runs two launchers in --coop-smoke mode, which drive the launcher's own
co-op flow (launcher_coop.h) and start the game exactly as the Host Co-op and
Join Co-op buttons do; the copy and paste of the codes is replaced by files:

  1. the host launcher starts the game; the game's offer appears in the host
     exchange folder;
  2. the join launcher takes that offer as if pasted, writes it to its own
     exchange folder and starts the joining game, whose answer appears there;
  3. this script hands the answer to the host launcher as if pasted; the host
     launcher writes it for its game, and the two games connect over ICE.

Each game runs in a private run directory (assets junction, scripted input
via --coop-arg --netplay-input, bounded by PIKMIN_NETPLAY_EXIT_AFTER_TICKS),
hidden, with loopback-only ICE (PIKMIN_NETPLAY_STUN=none,
PIKMIN_NETPLAY_ICE_BIND=127.0.0.1). Only the launched processes are ever
killed. PASS: both launchers exit 0 and the per-tick hashes are identical.

    py -3.12 tools/netplay/launcher_smoke.py --launcher <build>/bin/nectar-launcher.exe \\
        --exe <build>/bin/nectar.exe --assets <assets> --ticks 900 --out <scratch>
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_pair as rp  # noqa: E402


def start(launcher, args, run, env, log_name):
    startup = None
    if sys.platform == "win32":
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    out = open(run / log_name, "w")
    proc = subprocess.Popen([str(launcher)] + args, cwd=str(run), env=env, startupinfo=startup, stdout=out,
                            stderr=subprocess.STDOUT)
    return proc, out


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--launcher", type=Path, required=True)
    p.add_argument("--exe", type=Path, required=True)
    p.add_argument("--assets", type=Path, required=True)
    p.add_argument("--ticks", type=int, default=900)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--timeout", type=float, default=600)
    a = p.parse_args(argv)
    out = a.out.resolve()
    assets = a.assets.resolve()
    procs, files = [], []
    runs = {}
    for role, seed in (("host", 101), ("join", 202)):
        run = out / role
        pkni = rp.prepare_run(run, assets, {}, a.ticks, seed)
        ex = run / "exchange"
        if ex.exists():
            for f in ex.glob("*"):
                if f.is_file():
                    f.unlink()
        runs[role] = (run, pkni, ex)
    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith(rp.SCRUB_PREFIXES)}
    base_env.update(SDL_AUDIODRIVER="dummy", PIKMIN_NETPLAY_EXIT_AFTER_TICKS=str(a.ticks),
                    PIKMIN_NETPLAY_UNTHROTTLED="1", PIKMIN_NETPLAY_DEBUG_NAVI_POS="1", PIKMIN_NETPLAY_STUN="none",
                    PIKMIN_NETPLAY_ICE_BIND="127.0.0.1", PIKMIN_NETPLAY_NO_CLIPBOARD="1",
                    PIKMIN_NETPLAY_BANNER_MS="0", PIKMIN_NETPLAY_ICE_TIMEOUT_MS=str(int(a.timeout * 1000)))
    print(f"launcher_smoke: launcher sha256={rp.sha256_file(a.launcher)} game sha256={rp.sha256_file(a.exe)}")

    def smoke_args(role):
        run, pkni, ex = runs[role]
        args = ["--coop-smoke", role, "--exchange", str(ex), "--game", str(a.exe.resolve()), "--data-root", str(run),
                "--coop-arg", "--netplay-input", "--coop-arg", str(pkni), "--coop-arg", "--netplay-run-root",
                "--coop-arg", str(run / "netplay")]
        if role == "host":
            args += ["--answer-file", str(run / "pasted-answer.txt")]
        else:
            args += ["--offer-file", str(run / "pasted-offer.txt")]
        return args

    def env_for(role):
        return dict(base_env, PIKMIN_STATE_HASH_LOG=str(runs[role][0] / "hashes.txt"))

    hrun, _, hex_ = runs["host"]
    jrun, _, jex = runs["join"]
    for stale in (hrun / "pasted-answer.txt", jrun / "pasted-offer.txt"):
        if stale.exists():
            stale.unlink()
    hproc, hout = start(a.launcher, smoke_args("host"), hrun, env_for("host"), "launcher.log")
    procs.append(("host", hproc))
    files.append(hout)
    ok = rp.wait_for_file(hex_ / "offer.txt", hproc, 120)
    if ok:
        (jrun / "pasted-offer.txt").write_text((hex_ / "offer.txt").read_text())
        print(f"launcher_smoke: host offer {hex_ / 'offer.txt'} pasted into the join launcher")
        jproc, jout = start(a.launcher, smoke_args("join"), jrun, env_for("join"), "launcher.log")
        procs.append(("join", jproc))
        files.append(jout)
        ok = rp.wait_for_file(jex / "answer.txt", jproc, 120)
        if ok:
            (hrun / "pasted-answer.txt").write_text((jex / "answer.txt").read_text())
            print(f"launcher_smoke: join answer {jex / 'answer.txt'} pasted into the host launcher")
    rcs = rp.wait_all(procs, a.timeout)
    for f in files:
        f.close()
    for role in ("host", "join"):
        log = runs[role][0] / "launcher.log"
        text = log.read_text(errors="replace") if log.exists() else ""
        for ln in text.splitlines():
            if ln.startswith("coop-smoke:") or "ice transport ready" in ln or "coop_active=" in ln or "exit after" in ln:
                print(f"launcher_smoke: {role}: {ln.strip()}")
        print(f"launcher_smoke: {role}: launcher exit={rcs.get(role)} log {log}")
    same, n, tick, cols = rp.compare_hashes(hrun / "hashes.txt", jrun / "hashes.txt")
    print(f"launcher_smoke: hashes {'identical' if same else 'DIFFER'} over {n} ticks"
          + ("" if same else f" (first tick {tick} {cols})"))
    ok = ok and rcs.get("host") == 0 and rcs.get("join") == 0 and same and n == a.ticks
    print(f"launcher_smoke: {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
