#include "n3ds_cstick.h"
#include <stdio.h>
#include <stdlib.h>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
int main()
{
    signed char x, y;
    N3dsCStickMap m; m.fixedRange = 120; m.dead = 12;
    m.map(0, 0, x, y);        CHECK(x == 0 && y == 0, "rest");
    m.map(8, -6, x, y);       CHECK(x == 0 && y == 0, "inside dead zone");
    m.map(120, 0, x, y);      CHECK(x == 74 && y == 0, "full right = 74 (%d,%d)", x, y);
    m.map(0, 140, x, y);      CHECK(x == 0 && y == 74, "beyond range clamps to 74, up positive (%d,%d)", x, y);
    m.map(-130, 0, x, y);     CHECK(x == -74, "full left");
    m.map(0, -130, x, y);     CHECK(y == -74, "full down");
    m.map(85, 85, x, y);      CHECK(abs(x) == abs(y) && x > 0 && x * x + y * y <= 75 * 75 && x * x + y * y >= 73 * 73, "diagonal keeps radius 74 (%d,%d)", x, y);
    m.map(66, 0, x, y);       CHECK(x == 37, "half travel (%d) -> 37 (just over the game's 32 threshold)", x);
    m.map(40, 0, x, y);       CHECK(x < 32 && x > 0, "light push stays under the game's 32 'pressed' threshold (%d)", x);
    N3dsCStickMap a; a.dead = 12;                                  // learned range
    a.map(70, 0, x, y);       CHECK(x > 0 && x <= 74, "learning: first push is usable (%d)", x);
    for (int i = 0; i < 20; ++i) a.map(100, 0, x, y);
    a.map(85, 0, x, y);       CHECK(x == 74, "after pushing to 100, 85 (=range 85) is full deflection (%d)", x);
    a.map(0, 100, x, y);      CHECK(y == 74, "other axis full too");
    for (int v = -160; v <= 160; v += 7) for (int w = -160; w <= 160; w += 7) { a.map(v, w, x, y); CHECK(x >= -74 && x <= 74 && y >= -74 && y <= 74, "bounded"); }
    printf("%s (%d failures)\n", fails ? "FAILED" : "all cstick checks passed", fails);
    return fails != 0;
}
