// Host test for platform/n3ds_autolod.h (the auto_lod render-distance governor). g++ only.
// Synthetic workloads: each "frame" has a period and a display-list time that depend on the current level, like the real renderer where a higher
// level culls more models and so shortens the worker's display-list time.
#include "n3ds_autolod.h"
#include <cstdio>
#include <cmath>

static int sFailed = 0, sChecks = 0;
#define CHECK(c, ...) do { ++sChecks; if (!(c)) { ++sFailed; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

// worker-bound scene: period = max(gameMs, dl + overhead) and dl shrinks 15% per level.
struct Scene { double gameMs, dlBase, overhead; };
static void frameCost(const Scene& s, int level, double& period, double& dl)
{
    dl = s.dlBase * std::pow(0.82, level);
    period = std::fmax(s.gameMs, dl + s.overhead);
    if (period < 33.34) period = 33.34;   // fps_cap 30 paces to vblank: a faster worker still presents at 30
}

static int run(N3dsAutoLod& a, const Scene& s, int frames, int* changes = nullptr)
{
    int ch = 0;
    for (int i = 0; i < frames; ++i) {
        double p, d; frameCost(s, a.level, p, d);
        if (a.frame(p, d)) ++ch;
    }
    if (changes) *changes = ch;
    return a.level;
}

int main()
{
    // 1. off: never moves
    { N3dsAutoLod a; a.configure(0, 1000.0 / 30);
      int ch; run(a, {30, 80, 10}, 2000, &ch);
      CHECK(a.level == 0 && ch == 0, "off controller moved (level %d)", a.level); }

    // 2. menu-bound (game thread slow, display lists tiny): culling cannot help -> must stay at 0
    { N3dsAutoLod a; a.configure(5, 1000.0 / 30);
      int ch; run(a, {70, 6, 4}, 3000, &ch);
      CHECK(a.level == 0 && ch == 0, "menu-bound scene raised level to %d (%d changes)", a.level, ch); }

    // 3. already at target: stays at 0
    { N3dsAutoLod a; a.configure(5, 1000.0 / 30);
      int ch; run(a, {20, 14, 8}, 3000, &ch);
      CHECK(a.level == 0 && ch == 0, "scene at target raised level to %d", a.level); }

    // 4. worker-bound (dl 60 ms): climbs, and settles once the target is met, without running off to the max
    { N3dsAutoLod a; a.configure(5, 1000.0 / 30);
      int ch; run(a, {20, 60, 8}, 4000, &ch);
      double p, d; frameCost({20, 60, 8}, a.level, p, d);
      printf("  worker-bound dl60: settled level %d, period %.1f ms, dl %.1f ms, %d changes\n", a.level, p, d, ch);
      CHECK(a.level >= 2, "did not climb (level %d)", a.level);
      CHECK(p <= 1000.0 / 30 * 1.07, "settled period %.1f ms still misses the target", p);
      CHECK(ch <= 12, "too many level changes (%d)", ch); }

    // 5. hopeless scene (dl 200 ms): climbs to the max and stays there, no oscillation
    { N3dsAutoLod a; a.configure(3, 1000.0 / 30);
      int ch; run(a, {20, 200, 8}, 4000, &ch);
      CHECK(a.level == 3, "hopeless scene ended at level %d (max 3)", a.level);
      CHECK(ch == 3, "hopeless scene changed level %d times", ch); }

    // 6. scene changes: heavy field then back to a light corridor -> level returns to 0 eventually, but not instantly
    { N3dsAutoLod a; a.configure(5, 1000.0 / 30);
      run(a, {20, 60, 8}, 1500);
      const int heavy = a.level;
      int early = run(a, {20, 10, 6}, 60);          // ~2 s of calm
      CHECK(early == heavy, "gave a level back after only ~2 s of calm (%d -> %d)", heavy, early);
      int late = run(a, {20, 10, 6}, 20000);
      printf("  heavy->light: level %d -> %d after 2 s -> %d after a long calm\n", heavy, early, late);
      CHECK(late == 0, "never returned to 0 (level %d)", late); }

    // 7. bounce: a scene whose cost jumps back up right after each step down. The wait before the next step down must grow (hysteresis),
    //    so the number of level changes over a long run stays bounded.
    { N3dsAutoLod a; a.configure(5, 1000.0 / 30);
      int total = 0;
      for (int cycle = 0; cycle < 40; ++cycle) {
          int ch; run(a, {20, 75, 8}, 90, &ch);   total += ch;   // busy burst
          run(a, {20, 11, 6}, 110, &ch);          total += ch;   // quiet gap just long enough to step down once
      }
      printf("  bounce: %d level changes in 40 busy/quiet cycles, calm wait grew to %d windows\n", total, a.calmNeeded());
      CHECK(a.calmNeeded() > N3dsAutoLod::kCalmStart, "calm wait never grew");
      CHECK(total < 40 * 3, "bounce produced %d changes (hysteresis too weak)", total); }

    // 8. hitches (stage load / HOME menu) are ignored, not counted as a miss
    { N3dsAutoLod a; a.configure(5, 1000.0 / 30);
      for (int i = 0; i < 400; ++i) { a.frame(1200.0, 90.0); a.frame(33.4, 8.0); }
      CHECK(a.level == 0, "hitch frames raised the level to %d", a.level); }

    // 9. bad input
    { N3dsAutoLod a; a.configure(99, -5.0);
      CHECK(a.maxLevel == N3dsAutoLod::kLevels - 1, "maxLevel not clamped (%d)", a.maxLevel);
      CHECK(std::fabs(a.targetMs - 1000.0 / 30) < 1e-9, "bad target not replaced (%f)", a.targetMs);
      a.frame(0.0, 10.0); a.frame(-3.0, 10.0);
      CHECK(a.level == 0, "non-positive periods changed the level");
      CHECK(N3dsAutoLod::mult(-4) == 1.0f && N3dsAutoLod::mult(99) == N3dsAutoLod::mult(5), "mult() not clamped"); }

    // 9b. worker-bound but the model cache is not used at all (cachedMs = 0): culling cannot help, so the level must not move
    { N3dsAutoLod a; a.configure(5, 1000.0 / 30);
      int ch = 0; for (int i = 0; i < 3000; ++i) if (a.frame(150.0, 125.0, 0.0)) ++ch;
      CHECK(a.level == 0 && ch == 0, "raised the level (%d) although nothing was drawn from the cache", a.level);
      // ... and with the cache doing most of the work it does climb
      for (int i = 0; i < 3000; ++i) a.frame(80.0, 60.0, 50.0);
      CHECK(a.level > 0, "did not climb with 50 of 60 ms in cached draws"); }

    // 10. 60 fps target works too
    { N3dsAutoLod a; a.configure(5, 1000.0 / 60);
      int n = 0; for (int i = 0; i < 3000; ++i) { if (a.frame(33.4, 30.0)) ++n; }
      CHECK(a.level >= 1, "60 fps target never raised the level against 33 ms frames"); }

    printf("%d checks, %d failed\n", sChecks, sFailed);
    return sFailed ? 1 : 0;
}
