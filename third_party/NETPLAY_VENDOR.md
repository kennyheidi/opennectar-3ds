# Vendored netplay libraries

Source-only copies for online co-op. Nothing here is built by default: CMake
only builds them with `-DPIKMIN_NETPLAY_BUILD=ON`.

| Directory | Upstream | Pinned commit | License |
|---|---|---|---|
| `gekkonet/` | https://github.com/HeatXD/GekkoNet | `3b21722ad09824638ed6eb43282c463008053a91` (2026-09-02, release v20260902101441-3b21722) | BSD-2-Clause (`gekkonet/LICENSE`) |
| `gekkonet/GekkoLib/thirdparty/zpp/` | zpp_bits, as bundled by GekkoNet | same | MIT (`gekkonet/GekkoLib/thirdparty/zpp/LICENSE`) |
| `libjuice/` | https://github.com/paullouisageneau/libjuice | `b89c792e3612faf2f12cf35bcc56857313a06be3` (tag v1.7.4) | MPL-2.0 (`libjuice/LICENSE`) |

These libraries keep their own licenses. The repository's CC0 waiver does not
cover them.

## What was copied

Files were copied with `git archive` at the pinned commits. Nothing in them has
been modified (line endings are normalised to LF by git).

GekkoNet:
- `LICENSE`, `README.md`, `GekkoLib/include`, `GekkoLib/src`, `GekkoLib/thirdparty/zpp`
- Excluded: the bundled ASIO (`GekkoLib/thirdparty/asio`), `Examples/`, `docs/`, the MSVC project files and
  the CI workflows.

libjuice:
- `LICENSE`, `README.md`, `include`, `src`, `CMakeLists.txt` (kept for reference; the game uses its own target)
- Excluded: `test/`, `fuzzer/`, `cmake/`, `Makefile` and CI.

MPL-2.0 is a file-level copyleft. If a libjuice file is ever modified, that file
stays under MPL-2.0 and its source must remain available; keep such patches as
separate, clearly marked commits.

## How it is built

GekkoNet:
- `GEKKONET_NO_ASIO` and `GEKKONET_STATIC`. The game supplies its own transport through `GekkoNetAdapter`
  (`pc_port/netplay/pc_netplay_udp.cpp`).
- The library needs C++20 (`-std=gnu++20`), so it is its own static target outside the game's C++17 options.
- Its quoted includes are searched with `-iquote` first: on case-insensitive file systems the game's
  `include/Event.h` would otherwise shadow GekkoNet's private `event.h`.
- The game's own code includes only `gekkonet.h`, which is C-compatible.

libjuice:
- Its own static C11 target from `libjuice/src/*.c`, with `JUICE_STATIC` and `USE_NETTLE=0` (the built-in
  SHA/HMAC, no Nettle). It links `ws2_32`, `bcrypt` and the platform threads.
- The game's ICE transport (`pc_port/netplay/pc_netplay_ice.cpp`) includes only `juice/juice.h`.

## Online transport dependencies (`nectar_net`, `NECTAR_NET_ENABLE_GNS=ON`)

These are not copied into the repository. `scripts/netplay-deps.sh <target>`
downloads the pinned archives into `~/.cache/open-nectar/netplay-deps`,
refuses any archive whose SHA-256 differs from the one below, and builds them
as static libraries. Everything is linked into the game binary: players never
install anything.

| Library | Version / commit | Archive SHA-256 | License | Used on |
|---|---|---|---|---|
| GameNetworkingSockets | `d534c19aa760df3fb75fd20db13ba1932b8a5463` | `099d4b0ff1837f0f27c83d4ce05869ec1d32136866f69a3336cc137e951189ab` | BSD-3-Clause | all |
| protobuf (runtime + host `protoc`) | 3.21.12 (`v21.12`, `f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c`) | `4eab9b524aa5913c6fffb20b2a8abf5ef7f95a80bc0701f3a6dbb4c607f73460` | BSD-3-Clause | all |
| OpenSSL `libcrypto` | 3.5.5 (LTS branch) | `b28c91532a8b65a1f983b4c28b7488174e4a01008e29ce8e69bd789f28bc2a89` | Apache-2.0 | Linux, Android |
| Windows BCrypt | system | — | Windows component | Windows |
| miniupnpc (UPnP IGD) | 2.3.3 | `d52a0afa614ad6c088cc9ddff1ae7d29c8c595ac5fdd321170a05f41e634bd1a` | BSD-3-Clause | all |

Verification done on 2026-10-14: the OpenSSL hash matches the
`.sha256` published with the release; the GNS archive is byte-identical to the
spike checkout of the pinned commit; the protobuf release archive matches the
`v21.12` commit except for two packaging files (`Makefile.in`,
`method_dump.sh`) not used by the build. miniupnpc comes from the official
tarball (miniupnp.tuxfamily.org); its code is identical to the GitHub tag
`miniupnpc_2_3_3` (`bf4215a7`) except for CVS `$Id$` lines. Its GPG
signature was not checked (the author's public key was not available).
The PCP/NAT-PMP client is our own code (`nectar_net/port_mapping.h`): both
protocols are small fixed-layout UDP exchanges, and no maintained library
covers PCP on all three platforms.

Build choices:
- GNS: static, `ENABLE_ICE=OFF`, no WebRTC, examples, tests or tools.
  `USE_CRYPTO=OpenSSL` on Linux/Android; `USE_CRYPTO=BCrypt` on Windows, which
  makes GNS use its bundled curve25519/ed25519-donna (public domain).
  libsodium was rejected: GNS then requires hardware AES and would fail on
  ARM CPUs without the crypto extensions.
- OpenSSL: only `libcrypto`, `no-shared no-module no-dso no-engine no-legacy`.
- protobuf: runtime only for the target, without zlib; `protoc` is built once
  for the build machine and must stay at the same version.
- MinGW cross-build from Linux needs two case fixes, applied by the script
  without touching GNS sources: a forwarding `Windows.h` header, and
  `Iphlpapi` → `iphlpapi` in the installed CMake export.
- Android: GNS logs through `__android_log_write`; `nectar_net` links `log`
  because the GNS export omits it.

Attribution: Apache-2.0 (OpenSSL) and BSD-3-Clause (GNS, protobuf) require
shipping their license texts with release packages that enable online play.

Updating: change the pin and hash together in `scripts/netplay-deps.sh` and in
this table, rebuild every target and rerun the `nectar_net` tests.
