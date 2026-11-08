// Numeric check: GX TEV program (reference evaluator) vs the PICA combiner stages n3dsTevBuild produces.
#include "n3ds_tev.h"
#include <stdio.h>
#include <string.h>
#include <random>
#include <math.h>
struct V { float c[4]; };
static float cl(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float ch(uint32_t v, int i) { return ((v >> (8 * i)) & 255) / 255.0f; }
// GX
static V gxEval(const N3dsTevIn& in, V tex, V ras) {
    V prev = {{ch(in.reg[0],0),ch(in.reg[0],1),ch(in.reg[0],2),ch(in.reg[0],3)}};
    V reg[4]; for (int r = 0; r < 4; ++r) for (int i = 0; i < 4; ++i) reg[r].c[i] = ch(in.reg[r], i);
    reg[0] = prev;
    for (int s = 0; s < in.nStages; ++s) {
        const N3dsTevRaw& st = in.st[s];
        auto cArg = [&](int a, int i) -> float {
            switch (a) { case 0: return reg[0].c[i]; case 1: return reg[0].c[3]; case 2: return reg[1].c[i]; case 3: return reg[1].c[3];
            case 4: return reg[2].c[i]; case 5: return reg[2].c[3]; case 6: return reg[3].c[i]; case 7: return reg[3].c[3];
            case 8: return tex.c[i]; case 9: return tex.c[3]; case 10: return ras.c[i]; case 11: return ras.c[3]; case 12: return 1; case 13: return 0.5f; default: return 0; } };
        auto aArg = [&](int a) -> float {
            switch (a) { case 0: return reg[0].c[3]; case 1: return reg[1].c[3]; case 2: return reg[2].c[3]; case 3: return reg[3].c[3]; case 4: return tex.c[3]; case 5: return ras.c[3]; default: return 0; } };
        const float sc[4] = {1, 2, 4, 0.5f};
        V o;
        const float bi[4] = {0, 0.5f, -0.5f, 0};
        for (int i = 0; i < 3; ++i) { float a = cArg(st.ci[0], i), b = cArg(st.ci[1], i), c = cArg(st.ci[2], i), d = cArg(st.ci[3], i); o.c[i] = cl((d + (1 - c) * a + c * b + bi[st.cbias & 3]) * sc[st.cscale]); }
        { float a = aArg(st.ai[0]), b = aArg(st.ai[1]), c = aArg(st.ai[2]), d = aArg(st.ai[3]); o.c[3] = cl((d + (1 - c) * a + c * b + bi[st.abias & 3]) * sc[st.ascale]); }
        reg[st.cout & 3].c[0] = o.c[0]; reg[st.cout & 3].c[1] = o.c[1]; reg[st.cout & 3].c[2] = o.c[2]; reg[st.aout & 3].c[3] = o.c[3];
    }
    return reg[0];
}
static V picaEval(const N3dsTevOut& out, V tex, V ras) {
    V prev = {{0, 0, 0, 0}}, buf = {{0, 0, 0, 0}};
    for (int s = 0; s < out.n; ++s) {
        const N3dsTevStage& t = out.st[s];
        V k = {{ch(t.color,0), ch(t.color,1), ch(t.color,2), ch(t.color,3)}};
        auto src = [&](int sidx) -> V { switch (sidx) { case N3DS_TS_PRIMARY: return ras; case N3DS_TS_TEX0: return tex; case N3DS_TS_CONST: return k; case N3DS_TS_PREV: return prev; case N3DS_TS_BUFFER: return buf; default: return tex; } };
        V o;
        float a[3][3]; float al[3];
        for (int j = 0; j < 3; ++j) {
            V v = src(t.rgbSrc[j]);
            for (int i = 0; i < 3; ++i) { float x; switch (t.rgbOp[j]) { case N3DS_TO_C: x = v.c[i]; break; case N3DS_TO_1MC: x = 1 - v.c[i]; break; case N3DS_TO_A: x = v.c[3]; break; default: x = 1 - v.c[3]; } a[j][i] = x; }
            V w = src(t.aSrc[j]); al[j] = (t.aOp[j] == N3DS_TO_1MA || t.aOp[j] == N3DS_TO_1MC) ? 1 - w.c[3] : w.c[3];
        }
        auto fn = [](int f, float x, float y, float z) { switch (f) { case N3DS_TFN_REPLACE: return x; case N3DS_TFN_MODULATE: return x * y; case N3DS_TFN_ADD: return x + y;
            case N3DS_TFN_INTERP: return x * z + y * (1 - z); case N3DS_TFN_MULADD: return x * y + z; default: return x - y; } };
        for (int i = 0; i < 3; ++i) o.c[i] = cl(fn(t.rgbFn, a[0][i], a[1][i], a[2][i]) * t.rgbScale);
        o.c[3] = cl(fn(t.aFn, al[0], al[1], al[2]) * t.aScale);
        if (s >= 1 && s - 1 < 4 && (out.bufRgb >> (s - 1) & 1)) { buf.c[0] = prev.c[0]; buf.c[1] = prev.c[1]; buf.c[2] = prev.c[2]; }   // r49: one stage of delay
        if (s >= 1 && s - 1 < 4 && (out.bufA >> (s - 1) & 1)) buf.c[3] = prev.c[3];
        prev = o;
    }
    return prev;
}
static N3dsTevIn prog(const char* text, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    N3dsTevIn in; memset(&in, 0, sizeof in); in.primaryMap = 0; in.nUnits = 1; in.unitMap[0] = 0; in.unitMap[1] = in.unitMap[2] = -1; in.lenient = true;
    in.reg[0] = r0; in.reg[1] = r1; in.reg[2] = r2; in.reg[3] = r3;
    int n = 0; const char* p = text;
    while (*p) { N3dsTevRaw& s = in.st[n++]; memset(&s, 0, sizeof s); int c[4], a[4], m;
        int used = 0; sscanf(p, " C(%d,%d,%d,%d) A(%d,%d,%d,%d) map%d%n", &c[0], &c[1], &c[2], &c[3], &a[0], &a[1], &a[2], &a[3], &m, &used);
        for (int i = 0; i < 4; ++i) { s.ci[i] = (uint8_t)c[i]; s.ai[i] = (uint8_t)a[i]; } s.map = (uint8_t)m; p += used; while (*p == ' ' || *p == ';') ++p; }
    in.nStages = n; return in;
}
int main() {
    std::mt19937 rng(7);
    struct T { const char* name; const char* text; uint32_t r[4]; };
    const T tests[] = {
        {"filesel sparkle A (palette remap)", "C(8,15,15,15) A(4,7,7,7) map0; C(2,6,0,15) A(1,2,0,7) map255; C(0,15,15,15) A(7,0,5,7) map255", {0, 0x0064ff96, 0xffffffff, 0xffffffff}},
        {"particle mode 2 lerp(C1,C0,TEXC)", "C(4,2,8,15) A(7,4,1,7) map0", {0, 0x00ffffff, 0x00ff6464, 0xffffffff}},
        {"particle mode 0 lerp(C0,ONE,TEXC)", "C(2,12,8,15) A(7,4,1,7) map0", {0, 0xc8204080, 0, 0}},
        {"mixed directions (approx expected)", "C(2,4,8,15) A(7,4,1,7) map0", {0, 0xff2000ff, 0xff00ff20, 0}},
        {"downward lerp", "C(2,4,8,15) A(1,2,4,7) map0", {0, 0x80ffeedd, 0x40102030, 0}},
    };
    int fails = 0;
    for (const T& t : tests) {
        N3dsTevIn in = prog(t.text, t.r[0], t.r[1], t.r[2], t.r[3]);
        N3dsTevOut out; const bool ok = n3dsTevBuild(in, out);
        double maxErr = 0;
        for (int it = 0; it < 2000; ++it) {
            V tex, ras; for (int i = 0; i < 4; ++i) { tex.c[i] = (rng() % 256) / 255.0f; ras.c[i] = (rng() % 256) / 255.0f; }
            V g = gxEval(in, tex, ras), p = picaEval(out, tex, ras);
            for (int i = 0; i < 4; ++i) maxErr = fmax(maxErr, fabs(g.c[i] - p.c[i]));
        }
        printf("%-38s build %s, %d PICA stages, %d approximations, max error %.4f\n", t.name, ok ? "ok" : "REFUSED", out.n, out.approximations, maxErr);
        if (out.approximations == 0 && maxErr > 0.01) ++fails;
    }
    printf("%s\n", fails ? "FAIL: an exact program does not match" : "all exact programs match");
    return fails;
}
