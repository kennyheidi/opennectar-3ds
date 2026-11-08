// r52 host check: in-game presets (n3ds_config.cpp). g++ -std=c++17 -I ../../platform preset_test.cpp ../../platform/n3ds_config.cpp -o preset_test
#include "n3ds_config.h"
#include <stdio.h>
#include <stdarg.h>
extern "C" void n3ds_log(const char* fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\n"); }
int n3ds_allocprof_on = 0;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)
int main()
{
    for (int p = 1; p < N3DS_PRESET_COUNT; ++p) {
        N3dsConfig c; n3dsPresetApply(c, p);
        CHECK(n3dsPresetMatch(c) == p, "preset %d (%s) matches itself", p, n3dsPresetName(p));
    }
    { N3dsConfig c; n3dsPresetApply(c, N3DS_PRESET_SPEED); c.lighting = 1; CHECK(n3dsPresetMatch(c) == 0, "a changed setting makes it custom"); }
    { N3dsConfig c; char bad[64]; n3dsConfigParse("lighting = 1\nfps_cap = 30\npreset = 3\n", c, bad, sizeof bad);
      CHECK(c.preset == 3 && c.fpsCap == 20 && c.lighting == 0 && c.lodModelPx == 80, "preset = 3 in nectar.ini wins over the individual keys (fps_cap %d lighting %d)", c.fpsCap, c.lighting); }
    { N3dsConfig c; char bad[64]; n3dsConfigParse("preset = 0\nlod_model_px = 55\n", c, bad, sizeof bad);
      CHECK(c.preset == 0 && c.lodModelPx == 55, "preset = 0 keeps the individual keys"); }
    { N3dsConfig c; char bad[64]; n3dsConfigParse("preset = 9\n", c, bad, sizeof bad); CHECK(c.preset == 0, "out-of-range preset ignored"); }
    { N3dsConfig c; CHECK(n3dsPresetMatch(c) == 0, "built-in defaults are their own setting (custom): %s", n3dsPresetName(n3dsPresetMatch(c))); }
    printf("%s\n", fails ? "FAILED" : "all preset checks pass");
    return fails;
}
