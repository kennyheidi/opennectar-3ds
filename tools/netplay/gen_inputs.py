"""Seeded scripted input generator for the netplay determinism harness.

Writes the PKNI binary format (see pc_port/netplay/pc_input_log.h):
10-byte header (magic, version, pad count, record size) followed by one
record per tick, 4 pads each.

v2 (default): 56-byte records, 4 pads of 14 bytes each (11 PADStatus bytes
+ control-yaw u16 LE + flags u8). Each pad carries a plausible slowly
varying control yaw, so replays exercise the yaw-as-input path.
v1 (--v1): 44-byte records, 4 pads of 11 bytes (no yaw).
v3 (--ext or --ext-events): 64-byte records, 4 pads of 16 bytes: the v2 14
with the flags byte holding the ext bits (1 Bomb press, 2 Lock-On press,
4 Charge press, 8 Swarm held) and then the cursor rotate s16 LE (1/65536
turns per tick). The PAD and yaw streams are the same as without them.
--ext adds seeded events on every scripted pad (bomb and lock-on presses,
swarm holds that start with a charge press, and cursor-rotate bursts);
--ext-events T:KIND[:ARG...],... adds explicit ones on pad 0 (or pads 0 and
1 with --players 2), with T in ticks (VS: ticks into each match):
  bomb, lock          a one-tick press
  charge:H            a charge press, then the swarm button held H ticks
  swarm:H             the swarm button held H ticks
  rotate:A:N          a cursor rotate of A per tick for N ticks

The generator plays a plausible field session on pad 0: smooth main-stick
walk patterns, periodic A (throw/pluck), B holds (whistle), rare X
(disband), C-stick nudges and trigger presses. It never presses Start or Y
and never opens menus. Pads 1-3 are neutral (no controller).

--vs-starts writes a VS script for pad 0 instead (gen_vs_ticks): walk legs to
a ship piece, A throws in --vs-tap-windows, a constant control yaw.
"""

import argparse
import math
import random
import struct
import sys
from pathlib import Path

MAGIC = b"PKNI"
VERSION_V3 = 3
VERSION_V2 = 2
VERSION_V1 = 1
PAD_COUNT = 4
RECORD_SIZE_V3 = 64
RECORD_SIZE_V2 = 56
RECORD_SIZE_V1 = 44

# Ext bits (pc_input_log::kExt*).
EXT_BOMB = 0x01
EXT_LOCKON = 0x02
EXT_CHARGE = 0x04
EXT_SWARM = 0x08

# PAD buttons (include/Dolphin/pad.h). Start and Y are never emitted.
BTN_LEFT = 0x0001
BTN_RIGHT = 0x0002
BTN_DOWN = 0x0004
BTN_UP = 0x0008
TRIG_Z = 0x0010
TRIG_R = 0x0020
TRIG_L = 0x0040
BTN_A = 0x0100
BTN_B = 0x0200
BTN_X = 0x0400

STICK_MAX = 96  # comfortable deflection, inside the s8 range


class _PadScript:
    """Plausible field-session script for one pad (walk + buttons)."""

    def __init__(self, rng: random.Random):
        self.rng = rng
        self.wx, self.wy = 0, 0
        self.leg_left = 0
        self.holds = {}
        self.next_a = rng.randint(20, 60)
        self.next_b = rng.randint(80, 200)
        self.next_x = rng.randint(300, 700)
        self.next_c = rng.randint(30, 120)
        self.next_t = rng.randint(100, 300)
        self.cx, self.cy = 0, 0
        self.c_left = 0
        self.trig_l, self.trig_r = 0, 0
        self.t_left = 0

    def tick(self):
        rng = self.rng
        if self.leg_left <= 0:
            # New walk leg: mostly mid deflections, sometimes idle.
            if rng.random() < 0.15:
                self.wx, self.wy = 0, 0
            else:
                ang = rng.uniform(0, 2 * 3.141592653589793)
                mag = rng.randint(40, STICK_MAX)
                self.wx = int(round(mag * math.cos(ang)))
                self.wy = int(round(mag * math.sin(ang)))
            self.leg_left = rng.randint(20, 90)

        # Jitter the stick a little so consecutive ticks are not identical.
        jx = max(-127, min(127, self.wx + rng.randint(-6, 6)))
        jy = max(-127, min(127, self.wy + rng.randint(-6, 6)))
        self.leg_left -= 1

        buttons = 0
        # A presses: short 2-3 tick taps.
        self.next_a -= 1
        if self.next_a <= 0:
            self.holds[BTN_A] = rng.randint(2, 3)
            self.next_a = rng.randint(40, 120)
        # B holds: whistle, 10-30 ticks.
        self.next_b -= 1
        if self.next_b <= 0:
            self.holds[BTN_B] = rng.randint(10, 30)
            self.next_b = rng.randint(120, 300)
        # X: rare single-tick disband.
        self.next_x -= 1
        if self.next_x <= 0:
            self.holds[BTN_X] = 1
            self.next_x = rng.randint(400, 900)
        for btn, left in list(self.holds.items()):
            if left > 0:
                buttons |= btn
                self.holds[btn] = left - 1

        # C-stick nudges.
        self.next_c -= 1
        if self.next_c <= 0:
            self.cx = rng.randint(-80, 80)
            self.cy = rng.randint(-80, 80)
            self.c_left = rng.randint(3, 10)
            self.next_c = rng.randint(50, 200)
        if self.c_left > 0:
            self.c_left -= 1
        else:
            self.cx, self.cy = 0, 0

        # Triggers: occasional analog squeezes plus digital bit.
        self.next_t -= 1
        if self.next_t <= 0:
            self.trig_l = rng.randint(60, 200)
            self.trig_r = rng.randint(60, 200)
            self.t_left = rng.randint(4, 12)
            self.next_t = rng.randint(150, 400)
        if self.t_left > 0:
            self.t_left -= 1
            if self.trig_l > 120:
                buttons |= TRIG_L
            if self.trig_r > 120:
                buttons |= TRIG_R
        else:
            self.trig_l, self.trig_r = 0, 0

        assert not buttons & 0x1800, "Start/Y must never be pressed"
        return (buttons, jx, jy, self.cx, self.cy, self.trig_l, self.trig_r)


class _ExtScript:
    """Seeded ext events for one pad: Bomb and Lock-On presses, Swarm holds
    that start with a Charge press, and cursor-rotate bursts."""

    def __init__(self, rng: random.Random):
        self.rng = rng
        self.next_bomb = rng.randint(60, 200)
        self.next_lock = rng.randint(40, 120)
        self.next_charge = rng.randint(80, 240)
        self.next_rot = rng.randint(30, 120)
        self.hold = 0
        self.rot_left = 0
        self.rot = 0

    def tick(self):
        rng = self.rng
        bits, rot = 0, 0
        self.next_bomb -= 1
        if self.next_bomb <= 0:
            bits |= EXT_BOMB
            self.next_bomb = rng.randint(120, 260)
        self.next_lock -= 1
        if self.next_lock <= 0:
            bits |= EXT_LOCKON
            self.next_lock = rng.randint(60, 160)
        self.next_charge -= 1
        if self.next_charge <= 0 and self.hold <= 0:
            bits |= EXT_CHARGE
            self.hold = rng.randint(8, 25)
            self.next_charge = rng.randint(150, 350)
        if self.hold > 0:
            bits |= EXT_SWARM
            self.hold -= 1
        self.next_rot -= 1
        if self.next_rot <= 0:
            self.rot_left = rng.randint(4, 15)
            self.rot = rng.choice((-1, 1)) * rng.randint(150, 900)
            self.next_rot = rng.randint(80, 200)
        if self.rot_left > 0:
            rot = self.rot
            self.rot_left -= 1
        return bits, rot


def parse_ext_events(text):
    """T:KIND[:ARG...],... -> {tick: (bits, rotate)} (see the module docstring)."""
    out = {}

    def add(t, bits=0, rot=0):
        b0, r0 = out.get(t, (0, 0))
        out[t] = (b0 | bits, max(-32767, min(32767, r0 + rot)))

    for item in (text or "").split(","):
        item = item.strip()
        if not item:
            continue
        parts = item.split(":")
        t, kind, args = int(parts[0]), parts[1], [int(v) for v in parts[2:]]
        if kind == "bomb":
            add(t, EXT_BOMB)
        elif kind == "lock":
            add(t, EXT_LOCKON)
        elif kind in ("charge", "swarm"):
            hold = args[0] if args else 10
            if kind == "charge":
                add(t, EXT_CHARGE)
            for k in range(hold):
                add(t + k, EXT_SWARM)
        elif kind == "rotate":
            amount, n = args[0], (args[1] if len(args) > 1 else 1)
            for k in range(n):
                add(t + k, 0, amount)
        else:
            raise SystemExit(f"gen_inputs: unknown ext event {item!r}")
    return out


def gen_ticks(nticks: int, seed: int, players: int = 1):
    rng = random.Random(seed)
    # Yaw parameters come from a SEPARATE generator so the PAD stream (rng)
    # stays the same with or without yaw: --v1 output for a fixed seed
    # matches the v2 pad bytes.
    yaw_rng = random.Random(seed * 1000003 + 0x59415701)
    # Pad 1 (co-op partner) draws from its own generator so pad 0's stream is
    # byte-identical whether --players is 1 or 2.
    rng2 = random.Random(seed * 1000033 + 0x50320001)
    pad0 = _PadScript(rng)
    pad1 = _PadScript(rng2)
    # Slowly varying control yaw per pad (turns): a base drift plus a
    # gentle sinusoid, so consecutive ticks differ by a fraction of a
    # degree like a real camera, plus per-pad offsets so pads disagree.
    yaw_base = [yaw_rng.uniform(0.0, 1.0) for _ in range(4)]
    yaw_rate = [yaw_rng.uniform(-0.0006, 0.0006) for _ in range(4)]
    yaw_amp = [yaw_rng.uniform(0.0, 0.004) for _ in range(4)]
    yaw_period = [yaw_rng.uniform(300.0, 1200.0) for _ in range(4)]

    for tick in range(nticks):
        b0 = pad0.tick()
        b1 = pad1.tick() if players >= 2 else None
        yaws = []
        for p in range(4):
            turns = (
                yaw_base[p]
                + yaw_rate[p] * tick
                + yaw_amp[p] * math.sin(2 * math.pi * tick / yaw_period[p])
            ) % 1.0
            yaws.append(int(turns * 65536.0 + 0.5) & 0xFFFF)
        yield (b0, b1, yaws)


def gen_vs_ticks(nticks: int, seed: int, yaw: int, starts, path, swarm=(0, 100), tap=(15, 30), tap_windows=((0, 1 << 30),),
                 tap_button=BTN_A):
    """VS script for one player's pad 0: neutral until each match start in
    `starts` (session ticks), then the walk legs of `path` ((ticks, stickX,
    stickY[, cX, cY]) each, towards a ship piece of the VS arena), then the C-stick
    forward (the squad swarms into the piece and carries it) with an A tap now
    and then. The stick is back to neutral when a match ends, so the end screen
    keeps its default choice (Rematch) and the next A tap takes it. The yaw is
    constant (the control basis that faces the arena from this player's base);
    the seed only jitters the stick and the tap timing."""
    rng = random.Random(seed)
    starts = sorted(starts)
    walk = sum(leg[0] for leg in path)
    next_a = rng.randint(30, 60)
    for tick in range(nticks):
        begun = [s for s in starts if s <= tick]
        buttons, sx, sy, cx, cy = 0, 0, 0, 0, 0
        if begun:
            t = tick - begun[-1]
            if t < walk:
                left = t
                for leg in path:
                    ticks, lx, ly = leg[:3]
                    if left < ticks:
                        sx, sy = lx + rng.randint(-4, 4), ly + rng.randint(-4, 4)
                        if len(leg) >= 5:
                            cx, cy = leg[3] + rng.randint(-4, 4), leg[4] + rng.randint(-4, 4)
                        break
                    left -= ticks
            else:
                cx, cy = (swarm[0] + rng.randint(-4, 4), swarm[1] + rng.randint(-4, 4)) if swarm else (0, 0)
            next_a -= 1
            tapping = any(lo <= t < hi for lo, hi in tap_windows)
            if next_a <= 5 and tapping:  # a 5-tick press: long enough to grab, then throw
                buttons |= tap_button
            if next_a <= 0:
                next_a = rng.randint(tap[0], tap[1])
        yield ((buttons, sx, sy, cx, cy, 0, 0), None, [yaw & 0xFFFF, 0, 0, 0])


def with_ext(ticks, seed, players=1, seeded=False, events=None, starts=None):
    """Adds per-pad (bits, rotate) to every (b0, b1, yaws) tick: seeded events
    from their own generators (the PAD stream stays the same) and/or the
    explicit `events` ({tick: (bits, rotate)}), relative to each of `starts`
    when given (VS), else absolute."""
    scripts = [_ExtScript(random.Random(seed * 1000081 + 0x45585401 + p)) for p in range(players)] if seeded else []
    starts = sorted(starts) if starts else None
    for tick, (b0, b1, yaws) in enumerate(ticks):
        ext = [(0, 0)] * 4
        for p, sc in enumerate(scripts):
            ext[p] = sc.tick()
        if events:
            begun = [s for s in starts if s <= tick] if starts else [0]
            rel = tick - begun[-1] if begun else None
            if rel is not None and rel in events:
                for p in range(players):
                    bits, rot = ext[p]
                    eb, er = events[rel]
                    ext[p] = (bits | eb, max(-32767, min(32767, rot + er)))
        yield (b0, b1, yaws, ext)


def _pack_pad(buttons, sx, sy, cx, cy, tl, tr, connected, yaw, v1: bool, ext=None):
    if ext is not None:
        return struct.pack(
            "<HbbbbBBBBbHBh", buttons, sx, sy, cx, cy, tl, tr, 0, 0, connected,
            yaw, ext[0], ext[1],
        )
    if v1:
        return struct.pack(
            "<HbbbbBBBBb", buttons, sx, sy, cx, cy, tl, tr, 0, 0, connected
        )
    return struct.pack(
        "<HbbbbBBBBbHb", buttons, sx, sy, cx, cy, tl, tr, 0, 0, connected,
        yaw, 0,
    )


def write_file(path: Path, ticks, v1: bool = False, v3: bool = False):
    version = VERSION_V3 if v3 else VERSION_V1 if v1 else VERSION_V2
    record_size = RECORD_SIZE_V3 if v3 else RECORD_SIZE_V1 if v1 else RECORD_SIZE_V2
    with open(path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<HHH", version, PAD_COUNT, record_size))
        for row in ticks:
            b0, b1, yaws = row[:3]
            ext = row[3] if v3 else [None] * 4
            # Pad 0: live input, connected.
            buttons, sx, sy, cx, cy, tl, tr = b0
            f.write(_pack_pad(buttons, sx, sy, cx, cy, tl, tr, 0, yaws[0], v1, ext[0]))
            # Pad 1: scripted partner when --players 2, else neutral.
            if b1 is not None:
                buttons, sx, sy, cx, cy, tl, tr = b1
                f.write(_pack_pad(buttons, sx, sy, cx, cy, tl, tr, 0, yaws[1], v1, ext[1]))
            else:
                f.write(_pack_pad(0, 0, 0, 0, 0, 0, 0, -1, yaws[1], v1, (0, 0) if v3 else None))
            # Pads 2-3: neutral, no controller (yaw still varies for realism).
            for p in range(2, 4):
                f.write(_pack_pad(0, 0, 0, 0, 0, 0, 0, -1, yaws[p], v1, (0, 0) if v3 else None))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ticks", type=int, required=True)
    p.add_argument("--seed", type=int, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--v1", action="store_true",
                   help="write the v1 format (no yaw); default is v2 with yaw")
    p.add_argument("--players", type=int, default=1, choices=(1, 2),
                   help="script pad 1 as a co-op partner when 2 (default: 1)")
    p.add_argument("--vs-starts", default=None, metavar="T[,T...]",
                   help="VS script instead (gen_vs_ticks): the session ticks each match's control starts")
    p.add_argument("--vs-yaw", type=int, default=0, help="VS script: constant control yaw (1/65536 turns)")
    p.add_argument("--vs-swarm", default="0:100",
                   help="VS script: C-stick after the walk, STICKX:STICKY (0:0 = none; same basis as the walk)")
    p.add_argument("--vs-tap", default="15:30", help="VS script: ticks between A taps, LO:HI")
    p.add_argument("--vs-tap-windows", default="0:1000000",
                   help="VS script: A taps only in these windows, ticks into a match, LO:HI[,...]")
    p.add_argument("--vs-path", default="75:0:96",
                   help="VS script: walk legs before the swarm, TICKS:STICKX:STICKY[:CX:CY][,...] (stick -127..127; "
                        "the optional C-stick holds for that leg)")
    p.add_argument("--vs-tap-button", default="0x100", help="VS script: the tapped PAD button mask (default A)")
    p.add_argument("--ext", action="store_true",
                   help="v3: seeded Bomb, Lock-On, Charge/Swarm and cursor-rotate events on every scripted pad")
    p.add_argument("--ext-events", default=None, metavar="T:KIND[:ARG...][,...]",
                   help="v3: explicit ext events (bomb, lock, charge:H, swarm:H, rotate:A:N); VS: ticks into a match")
    a = p.parse_args(argv)
    if a.ticks <= 0:
        print("gen_inputs: --ticks must be positive", file=sys.stderr)
        return 2
    v3 = a.ext or bool(a.ext_events)
    if v3 and a.v1:
        print("gen_inputs: --v1 has no ext bytes", file=sys.stderr)
        return 2
    events = parse_ext_events(a.ext_events) if a.ext_events else None
    if a.vs_starts is not None:
        starts = [int(s) for s in a.vs_starts.split(",") if s.strip()]
        path = [tuple(int(v) for v in leg.split(":")) for leg in a.vs_path.split(",") if leg.strip()]
        tap = tuple(int(v) for v in a.vs_tap.split(":"))
        swarm = tuple(int(v) for v in a.vs_swarm.split(":"))
        swarm = swarm if any(swarm) else None
        windows = [tuple(int(v) for v in w.split(":")) for w in a.vs_tap_windows.split(",") if w.strip()]
        ticks = gen_vs_ticks(a.ticks, a.seed, a.vs_yaw, starts, path, swarm, tap, windows, int(a.vs_tap_button, 0))
        if v3:
            ticks = with_ext(ticks, a.seed, 1, a.ext, events, starts)
        write_file(a.out, ticks, v1=a.v1, v3=v3)
        print(f"gen_inputs: wrote {a.ticks} ticks (seed {a.seed}) VS script starts={starts} yaw={a.vs_yaw} "
              f"path={path}{' ext' if v3 else ''} to {a.out}")
        return 0
    ticks = gen_ticks(a.ticks, a.seed, players=a.players)
    if v3:
        ticks = with_ext(ticks, a.seed, a.players, a.ext, events)
    write_file(a.out, ticks, v1=a.v1, v3=v3)
    print(f"gen_inputs: wrote {a.ticks} ticks (seed {a.seed}) v{'3' if v3 else '1' if a.v1 else '2'} "
          f"players={a.players} to {a.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
