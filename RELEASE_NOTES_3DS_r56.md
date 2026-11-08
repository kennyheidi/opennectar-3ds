# Open Nectar 3DS r56 (Open Nectar 0.9.3 base)

This release is **source only**. To build it, see [README_3DS.md](README_3DS.md). The game data is not included: you need your own
Pikmin (GameCube) dump, USA Rev 1 or Europe. It runs on New 3DS / New 3DS XL / New 2DS XL only.

## Highlights

- **Transform and lighting on the GPU** (`gpu_xform = 1`, the default). The PICA200 vertex shader computes model-view-projection and
  the GameCube lighting. Before this, the render worker's CPU core did that work for every vertex. Any draw the GPU path cannot handle
  falls back to the CPU path, and the log counts the fallbacks by reason.
- **Cheaper culling.** Most models are classified once for the whole model, with an interval clip test. The per-piece box tests run
  only where the interval test cannot decide.
- **GPU head start** (`gpu_split`). The GPU starts drawing pieces of the frame while the CPU is still building the rest. The
  split now also counts mesh indices, so the GPU no longer sits idle in scenes made of models.
- **Adaptive draw distance** (`auto_lod`), presets, and a touch-screen settings menu on the bottom screen.
- **The Nectar Installer** (`nectar_installer.cia`): a setup guide, an SD-card layout check, the game installer, and an
  on-console `nectar.pak` builder for faster loading.
- **Built-in A/B speed test** (`ab_test` in `nectar.ini`) and a performance contract with automated gates
  (`n3ds_port/tools/runner/check.sh`, `n3ds_port/AGENTS.md`). A speed-up only counts when a capture on the console shows it.

## Known limits

- Busy areas do not hold 30 FPS everywhere. After the render worker, the game thread's own update (AI and the Pikmin
  manager) is the next limit.
- Environment-mapped materials, and materials that use more than two texture units, still use the CPU transform path.
- The 178 MB memory mode (`--mode 178`) has had little testing. The default is 124 MB.

## Upgrading

Replace `sdmc:/3ds/nectar/nectar.ini` with the one in `n3ds_port/`, or keep yours: any missing line uses the shipped value. Saves in
`sdmc:/3ds/nectar/save` are kept. If you change `assets/`, rebuild `nectar.pak`.
