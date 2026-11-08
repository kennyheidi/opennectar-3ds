#!/usr/bin/env bash
# build_3ds.sh -- one command to build Open Nectar for the 3DS: .elf + .3dsx + .cia, archived per build.
#
#   ./build_3ds.sh                      build everything (124MB mode), keep the CMake cache
#   ./build_3ds.sh --release            public build: normal identity (title ID 0xEC400, "Open Nectar"), game .cia named nectar.cia
#                                       (without it: the TEST identity 0xEC402 "Open Nectar TEST", installs beside a normal copy, nectar_test.cia)
#   ./build_3ds.sh --clean              wipe the build folder first (do this after changing memory mode or when something is stale)
#   ./build_3ds.sh --mode 178           CIA memory mode 178MB (default 124); passes the matching linear heap so the cache cannot go stale
#   ./build_3ds.sh --arena 24           size in MB of the game's simulated GameCube memory (default 24 = what worked; CMake's own default of 8 is TOO SMALL:
#                                       the game then panics "new[] 8388608 failed in heap 'ovl'" right after the title)
#   ./build_3ds.sh --cmdbuf 2048        GPU command buffer in KB (default: CMake's 2048); 256 = citro3d's default
#   ./build_3ds.sh --no-cia             skip the .cia (only .elf + .3dsx), e.g. when makerom/bannertool are not installed
#   ./build_3ds.sh --jobs 8             parallel compile jobs (default: all cores)
#   ./build_3ds.sh --trace-calls        instrument game code (slow) -- for hang hunting
#   ./build_3ds.sh --stretch            stretch 4:3 over the whole 400x240 top screen
#   ./build_3ds.sh --recovered          link the recovered sysDolphin objects (N3DS_USE_RECOVERED_SYSDOLPHIN=ON)
#   ./build_3ds.sh -D N3DS_FOO=bar      pass any extra CMake cache value (repeatable)
#   ./build_3ds.sh addr2line 0x2f5be8 0x2ed0a0 ...    resolve crash-dump addresses with the ELF of the NEWEST build
#   ./build_3ds.sh addr2line --elf dist/20251002-1530/nectar_3ds.elf 0x...   ... or of a specific build
#
# Every build is copied to dist/<timestamp>/ (elf, 3dsx, cia, nectar_installer.cia, smdh, bnr, build.log, flags.txt) and dist/latest/. (The ELF is detected by its header: the linker writes it without a .elf suffix.)
# KEEP THE ELF OF ANY BUILD YOU RUN ON HARDWARE: Luma crash-dump addresses only match the exact ELF that crashed.
#
# Run it from the devkitPro MSYS2 shell (or Linux/macOS with devkitPro installed). Put this file next to the cmake/ and n3ds_port/ folders.
set -u
set -o pipefail

# ------------------------------------------------------------------------------------------------------------------ helpers
if [ -t 1 ]; then RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; BLD=$'\033[1m'; OFF=$'\033[0m'; else RED=; GRN=; YEL=; BLD=; OFF=; fi
say()  { printf '%s\n' "${BLD}==>${OFF} $*"; }
ok()   { printf '%s\n' "${GRN}ok${OFF}  $*"; }
warn() { printf '%s\n' "${YEL}warn${OFF} $*" >&2; }
die()  { printf '%s\n' "${RED}error${OFF} $*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT=""
for cand in "$SCRIPT_DIR" "$SCRIPT_DIR/.." "$PWD"; do
  if [ -f "$cand/cmake/toolchain-3ds.cmake" ] && [ -f "$cand/n3ds_port/CMakeLists.txt" ]; then ROOT="$(cd "$cand" && pwd)"; break; fi
done
[ -n "$ROOT" ] || die "cannot find the repo root (a folder with cmake/toolchain-3ds.cmake and n3ds_port/CMakeLists.txt). Put build_3ds.sh in it."
BUILD="$ROOT/build-3ds"
DIST="$ROOT/dist"
TARGET=nectar_3ds
CIA_OUT=nectar_test   # TEST build: the game .cia is delivered as $CIA_OUT.cia (cmake still makes nectar_3ds.cia in the build folder; this script renames the copy)

latest_elf() { local e; e="$DIST/latest/$TARGET.elf"; [ -f "$e" ] && printf '%s' "$e"; }

# ------------------------------------------------------------------------------------------------------- addr2line sub-command
if [ "${1:-}" = "addr2line" ]; then
  shift
  ELF=""
  if [ "${1:-}" = "--elf" ]; then ELF="${2:-}"; shift 2 || die "--elf needs a path"; fi
  [ -n "$ELF" ] || ELF="$(latest_elf || true)"
  [ -n "$ELF" ] && [ -f "$ELF" ] || die "no ELF found (build first, or pass --elf path/to/$TARGET.elf)"
  [ $# -gt 0 ] || die "give at least one address, e.g. ./build_3ds.sh addr2line 0x2f5be8 0x2ed0a0"
  A2L="${DEVKITARM:-${DEVKITPRO:-/opt/devkitpro}/devkitARM}/bin/arm-none-eabi-addr2line"
  [ -x "$A2L" ] || A2L="$(command -v arm-none-eabi-addr2line || true)"
  [ -n "$A2L" ] || die "arm-none-eabi-addr2line not found (install devkitARM: pacman -S 3ds-dev)"
  say "resolving with $ELF"
  "$A2L" -Cfpe "$ELF" "$@"
  exit $?
fi

# -------------------------------------------------------------------------------------------------------------------- options
MODE=124; ARENA=24; CMDBUF=""; RELEASE=0; CLEAN=0; CIA=1; JOBS=""; TRACE=0; STRETCH=0; RECOVERED=0; EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    --clean) CLEAN=1 ;;
    --release) RELEASE=1 ;;
    --mode) shift; MODE="${1:-}"; MODE="${MODE%MB}" ;;
    --arena) shift; ARENA="${1:-}" ;;
    --cmdbuf) shift; CMDBUF="${1:-}" ;;
    --no-cia) CIA=0 ;;
    --jobs|-j) shift; JOBS="${1:-}" ;;
    --trace-calls) TRACE=1 ;;
    --stretch) STRETCH=1 ;;
    --recovered) RECOVERED=1 ;;
    -D) shift; EXTRA+=("-D${1:-}") ;;
    -D*) EXTRA+=("$1") ;;
    -h|--help) sed -n '2,21p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done
case "$ARENA" in ''|*[!0-9]*) die "--arena needs a number of MB";; esac
[ -z "$CMDBUF" ] || case "$CMDBUF" in *[!0-9]*) die "--cmdbuf needs a number of KB";; esac
if [ "$RELEASE" = 1 ]; then CIA_OUT=nectar; EXTRA=(-DN3DS_TEST_BUILD=OFF "${EXTRA[@]+"${EXTRA[@]}"}"); else EXTRA=(-DN3DS_TEST_BUILD=ON "${EXTRA[@]+"${EXTRA[@]}"}"); fi
case "$MODE" in 124|178) ;; *) die "--mode must be 124 or 178 (got '$MODE')";; esac
if [ -z "$JOBS" ]; then JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"; fi
case "$JOBS" in ''|*[!0-9]*) die "--jobs needs a number";; esac

# ----------------------------------------------------------------------------------------------------------- environment check
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
[ -d "$DEVKITPRO" ] || die "DEVKITPRO ($DEVKITPRO) does not exist. Install devkitPro and run: pacman -S 3ds-dev"
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"

need() { command -v "$1" >/dev/null 2>&1 || die "'$1' not found. $2"; }
need cmake "Install it: pacman -S cmake"
GEN="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then GEN="Ninja"; else need make "Install it: pacman -S make"; fi
[ -x "$DEVKITARM/bin/arm-none-eabi-g++" ] || die "devkitARM compiler missing at $DEVKITARM/bin. Run: pacman -S 3ds-dev"
[ -d "$DEVKITPRO/libctru/include" ] || die "libctru missing under $DEVKITPRO. Run: pacman -S 3ds-dev"
[ -f "$DEVKITPRO/libctru/include/citro3d.h" ] || die "citro3d missing. Run: pacman -S 3ds-dev"
need picasso "Part of 3ds-dev (pacman -S 3ds-dev)."
need 3dsxtool "Part of 3ds-dev (pacman -S 3ds-dev)."
need smdhtool "Part of 3ds-dev (pacman -S 3ds-dev)."
if [ "$CIA" = 1 ]; then
  MISSING=""
  command -v makerom >/dev/null 2>&1    || MISSING="$MISSING makerom"
  command -v bannertool >/dev/null 2>&1 || MISSING="$MISSING bannertool"
  if [ -n "$MISSING" ]; then
    die "the .cia needs:$MISSING -- not in the devkitPro repos. Put makerom(.exe) and bannertool(.exe) in $DEVKITPRO/tools/bin (or on PATH), or run with --no-cia."
  fi
fi

# -------------------------------------------------------------------------------------------------------------------- configure
[ "$CLEAN" = 1 ] && { say "removing $BUILD"; rm -rf "$BUILD"; }
PKG=both; [ "$CIA" = 1 ] || PKG=3dsx
# Memory options are passed EVERY time: a stale cached N3DS_LINEAR_HEAP_MB from another memory mode is exactly what crashed the CIA at launch.
ARGS=(-G "$GEN" -S "$ROOT/n3ds_port" -B "$BUILD"
      -DCMAKE_BUILD_TYPE=Release
      "-DN3DS_PACKAGE=$PKG"
      "-DN3DS_SYSMODE_EXT=${MODE}MB"
      -DN3DS_LINEAR_HEAP_MB=0 -DN3DS_TEX_BUDGET_KB=0 "-DN3DS_ARENA_MB=$ARENA"
      "-DN3DS_TRACE_CALLS=$([ $TRACE = 1 ] && echo ON || echo OFF)"
      "-DN3DS_STRETCH=$([ $STRETCH = 1 ] && echo ON || echo OFF)"
      "-DN3DS_USE_RECOVERED_SYSDOLPHIN=$([ $RECOVERED = 1 ] && echo ON || echo OFF)")
# The toolchain file only works on a FRESH build folder (CMake ignores it, with a warning, once a cache exists).
[ -f "$BUILD/CMakeCache.txt" ] || ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$ROOT/cmake/toolchain-3ds.cmake")
[ -z "$CMDBUF" ] || ARGS+=("-DN3DS_CMDBUF_KB=$CMDBUF")
ARGS+=("${EXTRA[@]+"${EXTRA[@]}"}")      # extra -D values last, so they override the defaults above

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$DIST/$STAMP"
mkdir -p "$OUT" || die "cannot create $OUT"
LOG="$OUT/build.log"
{
  echo "date:     $(date)"
  echo "root:     $ROOT"
  echo "git:      $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo n/a)$(git -C "$ROOT" diff --quiet 2>/dev/null || echo ' (+uncommitted changes)')"
  echo "mode:     ${MODE}MB   arena: ${ARENA}MB   cmdbuf: ${CMDBUF:-default}   package: $PKG   generator: $GEN   jobs: $JOBS"
  echo "cmake:    ${ARGS[*]}"
} | tee "$OUT/flags.txt" >/dev/null

say "configuring ($GEN, ${MODE}MB mode, package=$PKG)"
if ! cmake "${ARGS[@]}" 2>&1 | tee "$LOG"; then die "configure failed -- see $LOG (first error above). A stale cache? retry with --clean"; fi

# ------------------------------------------------------------------------------------------------------------------------ build
say "building with $JOBS jobs (a first build takes several minutes)"
T0=$(date +%s)
if ! cmake --build "$BUILD" -j "$JOBS" 2>&1 | tee -a "$LOG"; then
  echo
  printf '%s\n' "${RED}first errors:${OFF}" >&2
  grep -m5 -E 'error|Error 1' "$LOG" | cut -c1-200 >&2
  die "build failed -- full log: $LOG"
fi
say "built in $(( $(date +%s) - T0 )) s"

# -------------------------------------------------------------------------------------------------------------------- collect
FAIL=0
collect() {   # name  required(1/0)  [name to store it under]
  local f="$BUILD/$1" dst="${3:-$1}"
  if [ -f "$f" ]; then cp -f "$f" "$OUT/$dst" && ok "$(printf '%-18s %9s bytes' "$dst" "$(wc -c < "$f" | tr -d ' ')")"
  elif [ "$2" = 1 ]; then printf '%s\n' "${RED}missing${OFF} $1 (expected from this build)" >&2; FAIL=1
  fi
}
say "outputs"
# The linker output has no suffix on this toolchain ("Linking CXX executable nectar_3ds"): find it by ELF magic, store it as nectar_3ds.elf.
ELF_SRC=""
for cand in "$BUILD/$TARGET.elf" "$BUILD/$TARGET" "$BUILD/$TARGET.exe"; do
  if [ -f "$cand" ] && [ "$(head -c 4 "$cand" 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; then ELF_SRC="$cand"; break; fi
done
if [ -n "$ELF_SRC" ]; then
  cp -f "$ELF_SRC" "$OUT/$TARGET.elf" && ok "$(printf '%-18s %9s bytes (from %s)' "$TARGET.elf" "$(wc -c < "$ELF_SRC" | tr -d ' ')" "$(basename "$ELF_SRC")")"
else
  printf '%s\n' "${RED}missing${OFF} the ELF: no file named $TARGET(.elf/.exe) with an ELF header in $BUILD" >&2; FAIL=1
fi
collect "$TARGET.3dsx" 1
[ "$CIA" = 1 ] && collect "$TARGET.cia" 1 "$CIA_OUT.cia"
collect "$TARGET.smdh" 0
collect "$TARGET.bnr" 0
# The Nectar CIA installer (n3ds_port/tools/cia_installer): built by the same cmake --build, copied next to the game files.
if [ "$CIA" = 1 ]; then
  if [ -f "$BUILD/nectar_installer.cia" ]; then collect "nectar_installer.cia" 0
  else warn "nectar_installer.cia was NOT produced (is N3DS_BUILD_INSTALLER=OFF, or is tools/cia_installer missing? look for 'nectar_installer' in $LOG)"; fi
fi
[ -f "$BUILD/$TARGET.map" ] && cp -f "$BUILD/$TARGET.map" "$OUT/"
[ "$FAIL" = 0 ] || die "some outputs are missing: the build log is $LOG (look for 'not found' warnings from the packaging step)"

rm -rf "$DIST/latest"; mkdir -p "$DIST/latest"; cp -f "$OUT"/* "$DIST/latest/"
echo
say "done: $OUT"
echo "  copy to the SD card:  $TARGET.3dsx -> sdmc:/3ds/   (Homebrew Launcher)    $CIA_OUT.cia -> install with FBI"
[ -f "$OUT/nectar_installer.cia" ] && echo "  installer:            nectar_installer.cia -> install once with FBI; then copy $CIA_OUT.cia to the SD card ROOT (any *nectar*.cia is found) and start 'Nectar Installer' from HOME"
echo "  settings:             sdmc:/3ds/nectar/nectar.ini     assets: sdmc:/3ds/nectar/assets"
echo "  after a crash:        ./build_3ds.sh addr2line 0x<pc> 0x<lr> ...   (uses dist/latest, or --elf <the build you ran>)"
exit 0
