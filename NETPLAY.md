# Online co-op (netplay)

Two players on two PCs play the two-captain story co-op together: the host is
Olimar (P1), the joiner Louie (P2), and each PC shows its own captain. The two
games run in lockstep and exchange only the players' inputs, so both must run
the very same build.

Online co-op needs a netplay build of the game (`-DPIKMIN_NETPLAY_BUILD=ON`,
see "Building" below). A session starts a new co-op run on Impact Site day 1
with a fresh save of its own; it never touches your normal save or your
settings file.

## What both players need

- Builds from the same netplay source/simulation identity and game region.
  Windows and Linux executable bytes may differ; an incompatible source tree
  or simulation ABI is refused (`handshake refused: build`).
- Their own installed game data (the launcher's install from your own disc).
- A way to send each other one line of text: a chat message is enough.

The host's settings decide the session: gameplay settings that change the
simulation (Pikmin limit, day length, speed and health percentages, the
quality-of-life rows...) are taken from the host for this session only. The
joiner's own settings file keeps its own values. Window, graphics, sound and
control settings stay each player's own.

## Host and join from the launcher

The launcher of a netplay build shows **Host Co-op** and **Join Co-op** under
Pikmin.

Host:

1. Click **Host Co-op**. The game starts and the panel shows your **offer
   code** (a long line starting with `NPIX2-`).
2. Click **Copy code** and send it to the other player.
3. Paste the **answer code** they send back (it starts with `NPIX1-`) and
   click **Connect**. The session starts in the game window.

Join:

1. Click **Join Co-op**, paste the host's offer code and click **Join**.
2. The game starts and the panel shows your **answer code**. Click **Copy
   code** and send it to the host. The session starts once the host enters it.

Closing the panel does not stop the game. Each code works once: start a new
session with new codes every time.

## Online VS

**Host VS** and **Join VS** work the same way and start a 1v1 VS match
instead: the host is P1, the joiner P2, and each PC shows its own side. The
host's VS rules (the vs* rows of its VS menu) and VS map decide the match; the
offer code carries the host's `map.json`, so the joiner does not need the map
file (a `map.json` over 15000 bytes does not fit: the joiner then needs the
same map, under the same name). Both PCs compile the map from their own game
files and the games compare the result before they start: a different map, or
game files of another version, is refused (`handshake refused: vsmap`).

- The countdown, the pause after the match and the end screen run on the
  shared game clock, never on either PC's wall clock: a slow frame on one PC
  cannot make the two games disagree.
- The in-game pause menu (Start) is shared: either player's Start pauses both
  games, and its Rematch / Back to title choices apply to both. The F1 menu
  is local and pauses nothing (the match keeps running while it is open).
- On the end screen either player moves the cursor (D-pad or stick) and
  either player confirms (A, or Enter on the keyboard). **Rematch** starts the
  next match on both PCs; **Back to title** ends the session.
- Captains: the host's (`--coop-captains=<p1>,<p2>`). A captain that needs a
  local Pikmin 2 install is refused when the other PC cannot load it
  (`handshake refused: captain`).
- The Bomb, Lock-On and Charge buttons, the Swarm button and turning the
  cursor with a mouse, touch or stick camera drag travel with each player's
  input, so they work online in co-op and VS (Bomb Control, Lock-On and Charge
  come from the host's settings). Every captain can lock on and charge, online
  and in local co-op. A press reaches the game a couple of frames later, like
  every button.
- Not online yet (left out on purpose, so both games stay in step): the mouse
  wheel and touch icon colour pickers (the D-pad still picks the colour) and
  the F5-F8 debug keys.

Command line:

```powershell
# ICE (codes), host and joiner:
.\nectar.exe --netplay-host-ice --netplay-mode vs
.\nectar.exe --netplay-join-ice "@clipboard" --netplay-mode vs
# A map file instead of the VS menu's map (the host only; the offer carries it):
.\nectar.exe --netplay-host-ice --vs-map C:\maps\my_arena\map.json
# UDP on a LAN: both sides name the mode (or the same --vs-map file).
.\nectar.exe --netplay-host 48810 --netplay-mode vs
.\nectar.exe --netplay-join 192.168.1.20:48810 --netplay-mode vs
```

`--vs-map <file>` with a netplay switch is VS; the joiner of an ICE session
follows the host's mode (`--netplay-mode` there only checks it). Over UDP
each side uses its own settings, so the VS rules and the VS menu map must
already match (`handshake refused: config` otherwise).

## Host and join from the command line

From the game folder (PowerShell):

```powershell
# Host: prints the offer code (also copied to the clipboard) and waits for the
# answer on the console: paste it and press Enter.
.\nectar.exe --netplay-host-ice

# Joiner: the offer from the clipboard, a file, or the code itself.
.\nectar.exe --netplay-join-ice "@clipboard"
.\nectar.exe --netplay-join-ice "@offer.txt"
```

Options for both: `--netplay-code-out <file>` also writes this side's code to
a file; the host's `--netplay-answer-in <file>` reads the answer from a file
instead of the console; `--netplay-input keyboard` or `--netplay-input
gamepad[:N]` picks the device that plays your captain; `--netplay-run-root
<dir>` puts the run folders elsewhere.

Each session gets a private run folder, `netplay\run-<date>-<time>-host|join-pid<N>\`
next to the exe (or under `%LOCALAPPDATA%\Nectar\netplay` when the game folder
is read-only), with the session's save, both codes, `launch.txt`, the session
input log `session-inputs.pknl` and, after a desync, the desync report.

The plain UDP switches remain for a LAN or a VPN where the host's address is
known: `--netplay-host <port>` and `--netplay-join <ip>:<port>`, with
`$env:PIKMIN_NETPLAY_UDP_BIND = "0.0.0.0"` on both PCs (they bind 127.0.0.1
otherwise). They take each side's own settings, so the gameplay settings must
already match.

## Firewall and NAT

The codes carry each PC's own addresses (LAN, VPN and any public IPv6
address), and the two games try every pair until one connects. By default the
game contacts no third-party server, so out of the box it connects:

- on the same LAN;
- over a VPN such as **Tailscale** or ZeroTier: install it on both PCs and sign
  in to the same network. The codes then include the VPN addresses. No
  setting is needed;
- between two PCs that both have public IPv6.

Across the internet without a VPN, the games also need each PC's public IPv4
address. That is what a STUN server reports:

- `$env:PIKMIN_NETPLAY_STUN = "public"` on both PCs uses the public servers
  `stun.l.google.com:19302` and `stun.cloudflare.com:3478`. Those servers see
  each PC's IP address; no gameplay goes through them. This works for most
  home routers.
- `$env:PIKMIN_NETPLAY_STUN = "host:port"` uses a STUN server of your choice.

Other notes:

- The first time, Windows may ask whether the game may use the network.
  Allow it (private networks are enough between two PCs at home). The game
  never adds a firewall rule itself.
- If the games never connect (`ICE connect timed out`), there is no direct
  path between the two networks. Fallbacks:
  - a VPN, as above;
  - **Your own TURN server** (for example coturn), set on both PCs:
    `$env:PIKMIN_NETPLAY_TURN = "turn.example.org:3478:user:password"`.
    `$env:PIKMIN_NETPLAY_ICE_TURN_ONLY = "1"` forces the relay.
- A fixed UDP port range (for a router port forward):
  `$env:PIKMIN_NETPLAY_ICE_PORT_BEGIN` / `PIKMIN_NETPLAY_ICE_PORT_END`.

## In the game

- The HUD box on the right shows the connection quality, the ping and its
  jitter, the input delay and the hitches of the last 10 seconds. F4, or both
  stick buttons together on a pad, hides or shows it.
- The input delay starts at 2 frames (`$env:PIKMIN_NETPLAY_DELAY`, or `auto`
  from the measured ping) and adapts to the hitches the two games report.
- The F1 menu is local: opening it pauses only your input. Gameplay rows are
  locked during a session; your other settings still save.
- When the session ends, a banner says why (the other player left, the
  connection was lost, or a desync) and closes after 10 seconds or on any key.

## Troubleshooting

- `handshake refused: build` or `protocol`: incompatible source trees,
  simulation ABIs or protocol versions. Use packages from the same release.
- `handshake refused: config` with the UDP switches: the gameplay settings
  differ. Use `--netplay-host-ice` / `--netplay-join-ice`, which adopt the
  host's, or make them match.
- `handshake refused: mode`: one side hosts or joins co-op, the other VS (or
  the captains differ).
- `handshake refused: vsmap`: the two PCs compiled different VS maps (another
  map file, or game files of another version).
- `bad ICE code: ...`: the code was cut off or belongs to the other direction
  (offers start `NPIX2-`, answers `NPIX1-`). Copy the whole single line.
- `ICE connect timed out` or `ICE failed: no working candidate pair`: no
  network path between the PCs; see "Firewall and NAT".
- `connection lost`: the other game stopped answering for 15 s (60 s while a
  stage loads), or it crashed.
- `desync detected` (exit code 5): the two games stopped agreeing. Both write
  `desync-report.txt`, `desync-subs.txt`, `desync-objects.txt` and
  `session-inputs.pknl` into their run folder; please send both folders with
  a bug report. A recorded session replays offline with
  `tools/netplay/replay_session.py`.

## One-code online mode (experimental, host-authoritative)

The new online layer (`libs/nectar_net`, see `ONLINE_COOP_IMPLEMENTATION_PLAN.md`)
replaces the two-code ICE exchange and lockstep with one code from the host
and a host that never waits for the guest. Only the two captains are
replicated so far, so it is a development preview, not a way to play a
campaign yet.

```sh
# Build: the dependencies once (pinned, checksummed, static), then the game.
scripts/netplay-deps.sh linux
cmake -S games/pikmin1 -B build-online -DPIKMIN_NETPLAY_BUILD=ON -DNECTAR_ONLINE=ON \
      -DNECTAR_NET_ENABLE_GNS=ON -DCMAKE_PREFIX_PATH=$HOME/.cache/open-nectar/netplay-deps/install/linux
# Host: opens the room (PCP/NAT-PMP/UPnP, IPv6) and writes its code to a file.
nectar --online-host 0 --online-code-out code.txt
# Guest: the code itself, or @file.
nectar --online-join @code.txt
```

By default the guest's game is an exact replica: the host sends the inputs it
simulated every tick and the guest replays them, so the whole world matches
(checked with state hashes twice a second). The guest's own captain responds
after about its ping plus 60 ms. A guest can join a running session or come
back after a drop: it replays the session so far at about 12 times real time.
`--online-predictive` keeps the older captain-only mode with instant response.

To continue a file instead of a new run, the host adds `--online-save 1`
(or 2, 3): it plays on its own memory card, the only one that records
progress, and sends a copy to the guest when it joins.

In the game, the online box under the day badge (F4 toggles it) shows the
link, the delay the guest feels and the session state. A guest can leave
co-op from F1 > Data > "Online co-op: leave / rejoin".

The guest plays by the host's sim settings and seed (sent when it joins),
sees its own captain, and the host's Start pauses both games. A guest can leave co-op and
watch the host's captain while keeping its slot; Start asks to rejoin, which
the host grants outside menus and cutscenes.

The code is a secret: share it only with the friend who joins. The log never
prints it. A wrong code, another game version or region, a full room and an
unreachable host are reported as such.

Two processes on one PC, with an optional bad link simulated by GNS:

```sh
SDL_VIDEODRIVER=offscreen python3 tools/netplay/run_online_pair.py --exe <build>/bin/nectar \
    --assets <assets> --ticks 3000 --lag 75 --jitter 30 --loss 5 --out run/online
```

## Building

```powershell
cmake -S games/pikmin1 -B build-netplay -G Ninja -DPIKMIN_NETPLAY_BUILD=ON
cmake --build build-netplay --target pikmin_pc pikmin_launcher
```

`PIKMIN_NETPLAY_BUILD` cannot be combined with `PIKMIN_NATIVE_OPTIMIZE` (two
PCs must run the same code). A build from a dirty Git tree must set
`PIKMIN_NETPLAY_SOURCE_ID` explicitly; release builds use the clean Git tree
identity so two different local edits can never be mistaken for compatible.
The netplay libraries (GekkoNet, libjuice) and their licences are listed in
`third_party/NETPLAY_VENDOR.md`.

## Engineering tests

`tools/netplay/run_pair.py` runs a host and a joiner as hidden, bounded
processes with scripted input and per-tick state hashes. `--ice` uses the
connection codes, carried through files; by default only loopback candidates
are used (`PIKMIN_NETPLAY_STUN=none`, `PIKMIN_NETPLAY_ICE_BIND=127.0.0.1`).

```powershell
py -3.12 tools\netplay\run_pair.py --ice --exe <build>\bin\nectar.exe --assets <assets> --ticks 3000 --out <scratch>\ice
# Settings adoption: the joiner prefers other settings, the session still syncs.
py -3.12 tools\netplay\run_pair.py --ice ... --config-join fpsMode=1 hideOlimarText=1
# Desync forensics: a TEST ONLY nudge on the joiner; both exit 5 with reports.
py -3.12 tools\netplay\run_pair.py --ice ... --expect desync --desync-nudge 1500:navi:1
# Replay a recorded log offline (8 = a recorded hash did not reproduce).
py -3.12 tools\netplay\replay_session.py --exe <build>\bin\nectar.exe --assets <assets> `
    --log <run folder>\session-inputs.pknl --out <scratch>\replay --expect match
# Online VS on impact_arena: a 60 s match (TEST ONLY knob), scoring, the end
# screen, a rematch driven by the scripted A press, and the second match.
py -3.12 tools\netplay\run_pair.py --mode vs --ice --exe <build>\bin\nectar.exe --assets <assets> `
    --ticks 2700 --vs-matches 2 --out <scratch>\vs `
    --env PIKMIN_NETPLAY_TEST_KNOBS=1 PIKMIN_NETPLAY_TEST_VS_MATCH_SECONDS=60
# Bomb, Lock-On, Charge and cursor turning online: seeded events on both peers
# (gen_inputs --ext); the sim's [EXT] trace must match on both.
py -3.12 tools\netplay\run_pair.py --ice --exe <build>\bin\nectar.exe --assets <assets> --ticks 3000 `
    --out <scratch>\ext --config bombControl=1 lockOn=1 charge=1 --ext --require-ext input,rotate
# The launcher's Host Co-op / Join Co-op flow, scripted.
py -3.12 tools\netplay\launcher_smoke.py --launcher <build>\bin\nectar-launcher.exe `
    --exe <build>\bin\nectar.exe --assets <assets> --out <scratch>\launcher
```

Two PCs over ICE, with the codes carried between them by any means (a shared
folder, scp). Host PC:

```powershell
py -3.12 tools\netplay\run_pair.py --peer-only host --ice --ice-bind any --stun public `
    --exe <build>\bin\nectar.exe --assets <assets> --ticks 3000 --out <scratch>\two-pc
# then copy <scratch>\two-pc\host\offer.txt to the joiner's PC
```

Joiner PC:

```powershell
py -3.12 tools\netplay\run_pair.py --peer-only join --ice --ice-bind any --stun public `
    --exe <build>\bin\nectar.exe --assets <assets> --ticks 3000 --out <scratch>\two-pc `
    --offer <copied offer.txt>
# then copy <scratch>\two-pc\join\answer.txt to the host's <scratch>\two-pc\host\answer-in.txt
```

Afterwards compare the two `hashes.txt` with `tools\netplay\compare_hashes.py`.
Use the same exe, `--ticks` and `--netplay-seed` on both PCs.
