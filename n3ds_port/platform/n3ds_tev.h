// GX TEV stage description -> PICA200 texture-combiner stages. Pure function (host-testable).
#pragma once
#include <stdint.h>
#include "n3ds_bridge.h"

struct N3dsTevRaw {                       // what the game programmed for one GX stage (GX enum values, Dolphin SDK numbering)
    uint8_t ci[4], ai[4];                 // colour args a,b,c,d (GX_CC_*: CPREV 0 APREV 1 C0 2 A0 3 C1 4 A1 5 C2 6 A2 7 TEXC 8 TEXA 9 RASC 10 RASA 11 ONE 12 HALF 13 KONST 14 ZERO 15)
                                          // alpha args a,b,c,d (GX_CA_*: APREV 0 A0 1 A1 2 A2 3 TEXA 4 RASA 5 KONST 6 ZERO 7)
    uint8_t cop, cbias, cscale, cout;     // op 0 add / 1 sub, bias 0/1/2, scale 0=1x 1=2x 2=4x 3=1/2x, out reg 0 = PREV
    uint8_t aop, abias, ascale, aout;
    uint8_t map;                          // texture map for this stage (>= 8: none)
    uint8_t ksel, kasel;                  // GX_TEV_KCSEL_* / GX_TEV_KASEL_*
    uint8_t chan;                         // r15: GX colour channel of this stage (GX_COLOR0A0 4, GX_COLOR1A1 5 = specular, GX_COLOR_ZERO 6 ...)
};
struct N3dsTevIn {
    int nStages;                          // stages the game enabled (1..16)
    N3dsTevRaw st[16];
    uint32_t reg[4];                      // GX_TEVPREV, REG0, REG1, REG2 as 0xAABBGGRR
    uint32_t kcolor[4];                   // GX_KCOLOR0..3 as 0xAABBGGRR
    int primaryMap;                       // the texture map that is bound to PICA texture unit 0; -1 = no texture
    int nUnits;                           // multi-texture: PICA texture units in use (0 = legacy, only primaryMap / unit 0)
    int unitMap[3];                       // GX texture map bound to PICA texture unit 0..2 (-1 = none); unitMap[0] == primaryMap
    bool lenient;                         // programs that keep results in TEV registers: drop those stages / read the registers as 0 instead of refusing
};
struct N3dsTevOut { int n; N3dsTevStage st[6]; int approximations; uint8_t bufRgb, bufA; };   // bufRgb / bufA: PICA combiner buffer update masks (r48: TEV register outputs)   // approximations: things that could not be mapped exactly

// Returns false when the program cannot be expressed at all (out.n = 0); callers then use a modulate/vertex-colour fallback.
bool n3dsTevBuild(const N3dsTevIn& in, N3dsTevOut& out);
// Fallback: texture * vertex colour (textured) or vertex colour only.
void n3dsTevFallback(bool textured, N3dsTevOut& out);
