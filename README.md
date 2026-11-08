# Open Nectar for New 3DS

A native New 3DS port of [Open Nectar](README.md), the Pikmin (GameCube) decompilation and PC port, on its 0.9.3 base. The game code is
the decompiled source in `src/`, compiled for the 3DS's ARM11. The 3DS platform layer lives in `n3ds_port/`. This includes the GPU
renderer (vertex transform and lighting run in a PICA200 vertex shader), audio, input, the touch-screen settings menu, the
on-console installer and the pak builder.

**No game data is included, and none may be added to this repository or its releases** (see [LEGAL.md](LEGAL.md)). You need your own
dump of Pikmin for GameCube, from your own disc. The supported versions are USA Rev 1 and Europe.

## Requirements to play

- A **New 3DS, New 3DS XL or New 2DS XL**. The original 3DS / 2DS does not have the memory or the CPU for it.
- Custom firmware ([Luma3DS](https://3ds.hacks.guide)), to install a `.cia`.
- About 1 GB free on the SD card.

---

## 1. Building

### 1.1 Install the toolchain

**Windows.** Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the graphical installer and tick *3DS Development*.
Then open the **devkitPro MSYS2** shell (Start menu → devkitPro → MSYS2) and run:

```sh
pacman -Syu
pacman -S 3ds-dev cmake ninja python git
```

**Linux / macOS.** Install [devkitPro pacman](https://devkitpro.org/wiki/devkitPro_pacman), then run:

```sh
sudo dkp-pacman -S 3ds-dev        # (on Arch: sudo pacman -S 3ds-dev)
# plus cmake (3.16 or newer), ninja and python3 from your system package manager
```

`3ds-dev` provides devkitARM, libctru, citro3d, `picasso`, `3dsxtool` and `smdhtool`. All of these are required.

**For the `.cia` (recommended), you also need two tools that are not in the devkitPro repositories:**

| Tool | Where to get it |
|---|---|
| `makerom` | [Project_CTR releases](https://github.com/3DSGuy/Project_CTR/releases) |
| `bannertool` | [bannertool releases](https://github.com/diasurgical/bannertool/releases) (maintained fork) |

Put both executables in `$DEVKITPRO/tools/bin`. On Windows that is `C:\devkitPro\tools\bin`, where `makerom.exe` and `bannertool.exe`
go. Any folder on `PATH` also works. Without these two tools you can still build the `.3dsx` with `--no-cia`.

`ccache` is optional. When it is installed, rebuilds are much faster.

### 1.2 Build

From the root of this source tree (the folder that contains `build_3ds.sh`, `cmake/` and `n3ds_port/`), run:

```sh
./build_3ds.sh --release
```

The first build takes several minutes. The results go to `dist/<timestamp>/` and to `dist/latest/`:

| File | What it is |
|---|---|
| `nectar.cia` | The game. Install it with FBI or with the Nectar installer. |
| `nectar_installer.cia` | The *Nectar Installer*: a setup guide, an SD-card checker, the game installer and an on-console `nectar.pak` builder |
| `nectar_3ds.3dsx` | The game as a Homebrew Launcher app. The `.cia` is preferred because it gets the New 3DS extended memory mode |
| `nectar_pakbuild.3dsx` | A stand-alone on-console `nectar.pak` builder |
| `nectar_3ds.elf` | The symbols. **Keep it for any build you hand out**: crash-dump addresses only resolve against the exact ELF that crashed |

Useful options (`./build_3ds.sh --help` lists them all):

| Option | Effect |
|---|---|
| `--release` | The normal identity: title ID `0xEC400`, "Open Nectar", output `nectar.cia`. Without it you get the *test* identity (`0xEC402`, "Open Nectar TEST", `nectar_test.cia`), which installs beside a normal copy |
| `--clean` | Wipes `build-3ds/` first. Use it after switching the memory mode or when the build seems stale |
| `--no-cia` | Builds only the `.elf` and the `.3dsx` (no `makerom` / `bannertool` needed) |
| `--mode 178` | The 178 MB memory mode instead of 124 MB (it is less tested) |
| `--stretch` | Builds with the picture stretched over the full 400×240 by default (this is also a setting in `nectar.ini`) |
| `-D NAME=value` | Passes any extra CMake cache value |

To resolve a crash dump from Luma3DS (the `pc` / `lr` addresses), run:

```sh
./build_3ds.sh addr2line 0x002f5be8 0x002ed0a0                          # uses dist/latest
./build_3ds.sh addr2line --elf dist/<build>/nectar_3ds.elf 0x...         # a specific build
```

**Without the script.** `build_3ds.sh` is a wrapper around plain CMake:

```sh
cmake -G Ninja -S n3ds_port -B build-3ds -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-3ds.cmake \
      -DCMAKE_BUILD_TYPE=Release -DN3DS_TEST_BUILD=OFF -DN3DS_ARENA_MB=24
cmake --build build-3ds -j
```

Keep `-DN3DS_ARENA_MB=24`. CMake's own default of 8 is too small, and the game then stops right after the title screen with
`new[] 8388608 failed in heap 'ovl'`.

### 1.3 Checks (optional, no 3DS needed)

```sh
n3ds_port/tools/runner/check.sh          # all checks; needs python3 and g++, plus picasso for the GPU-vs-CPU transform test
n3ds_port/tools/runner/check.sh --quick  # only the static contract gates and their self-tests
```

These run the static performance-contract gates (`n3ds_port/AGENTS.md`) and host-side unit tests. The tests cover the TEV colour
combiner, the presets and the installer guide layout. A PICA200 shader interpreter also checks that the GPU transform path matches the
CPU path. A green result means the gates found nothing wrong. It does not prove a change is faster: only an A/B capture on the console
can show that (`ab_test` in `nectar.ini`, then `check.sh --log log.txt`).

---

## 2. Setting up the SD card

### 2.1 Get the game files (on a computer)

Use **one** of these:

- **Dolphin:** right-click Pikmin in the game list → *Properties* → *Filesystem*, then right-click the top entry → *Extract Entire
  Disc…*. The folder you need is the one that contains `dataDir`.
- **The Open Nectar PC launcher:**
  `nectar-launcher --rom Pikmin.iso --install-dir C:\OpenNectar --extract-only`. The files end up in `C:\OpenNectar\assets`.

### 2.2 Copy them to the SD card

```
SD card
├─ nectar.cia                     (only needed until it is installed)
└─ 3ds
   └─ nectar
      ├─ nectar.ini               (from n3ds_port/nectar.ini in this tree)
      └─ assets
         └─ dataDir
            ├─ parms
            ├─ SndData
            └─ ...
```

`dataDir` must end up at `sdmc:/3ds/nectar/assets/dataDir`. A common mistake is one folder too many, such as
`assets/files/dataDir`. The installer's SD check detects this and tells you what to move.

### 2.3 Install

- **With the Nectar Installer.** Install `nectar_installer.cia` once with FBI. Put `nectar.cia` in the **root** of the SD card, start
  *Nectar Installer* from the HOME Menu and press **A**. The installer replaces an older install of the same title and keeps your saves.
- **With FBI.** Install `nectar.cia` directly.

### 2.4 Faster loading (strongly recommended)

`nectar.pak` packs the game's thousands of small files into one file. Each separate SD-card open costs about 35 ms on the console,
so the pak makes a large difference to load times. Make it with any of these:

- **The Nectar Installer:** press **R** on its main screen, then **A**.
- **`nectar_pakbuild.3dsx`:** run it from the Homebrew Launcher.
- **On a PC, with the SD card inserted:** `python n3ds_port/tools/make_pak.py E:/3ds/nectar`

Keep the loose `assets/` folder: large files and cutscene streams are still read from it. Re-run the pak builder whenever you change
anything under `assets/`.

Saves are written to `sdmc:/3ds/nectar/save`, not inside the installed title, so reinstalling or updating keeps them. Still, back the
folder up before an update.

---

## 3. Settings

`sdmc:/3ds/nectar/nectar.ini` holds the settings. Every line has a comment explaining it, and any missing line falls back to the
shipped value. Most settings can also be changed while playing: tap the bottom screen, change options, then tap *Save settings*. The
same menu has three presets (Quality / Speed / Bare minimum).

If models look wrong or the game freezes during rendering, try `gpu_xform = 0`. This moves vertex transform back to the CPU (slower)
and helps narrow down a GPU-path problem.

## 4. Reporting a problem

Send `sdmc:/3ds/nectar/log.txt` from the run that went wrong, before you start the game again: every launch overwrites it. For a crash,
also send the Luma3DS crash dump (`sdmc:/luma/dumps/arm11/`) and say which build you ran. The crash addresses only resolve against that
build's `nectar_3ds.elf`.

Do not attach disc images, extracted game files or `nectar.pak` to issues.

## 5. Where things are

| Path | Contents |
|---|---|
| `build_3ds.sh` | One-command build |
| `cmake/toolchain-3ds.cmake` | The devkitARM toolchain file |
| `n3ds_port/CMakeLists.txt` | The 3DS build. It sits beside the upstream build and does not modify it. Two upstream game files are patched at configure time by exact string replacement (`platform/n3ds_src_patches.cmake`) |
| `n3ds_port/platform/` | The 3DS platform layer: renderer, PICA200 shaders, audio, input, settings menu, file I/O |
| `n3ds_port/shim/` | Stand-in headers for SDL / OpenGL used by the PC port's sources |
| `n3ds_port/audio_dropin/` | The audio sources this build uses where upstream's still lack the sound-effect port queue |
| `n3ds_port/tools/` | The CIA installer, the pak builders, the host tests and the contract runner (`runner/check.sh`) |
| `n3ds_port/AGENTS.md` | The performance and platform contract: rules, measurements and how to A/B-test a change |
| `n3ds_port/docs/validation/` | Saved console performance logs that the contract and its measurements refer to |
