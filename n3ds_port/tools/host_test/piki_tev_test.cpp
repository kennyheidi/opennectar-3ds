#include "n3ds_tev.h"
#include <cstdio>
#include <cstring>
// paste picaEval etc. by including the numeric test without its main
#define main numeric_main
#include "tev_numeric_test.cpp"   /* build: g++ -std=c++17 -I . -I ../../platform piki_tev_test.cpp ../../platform/n3ds_tev.cpp */
#undef main
int main() {
    // redModel.mod mat0: s0 chan4 C(15,2,10,15) A(5,7,7,7); s1 chan5 C(15,0,12,10) A(5,7,7,7) clamp; C0 = red (setColour)
    N3dsTevIn in = prog("C(15,2,10,15) A(5,7,7,7) map255; C(15,0,12,10) A(5,7,7,7) map255", 0, 0xff001eff, 0xffffffff, 0xffffffff);
    in.primaryMap = -1; in.nUnits = 0; in.unitMap[0] = -1;
    V ras = {{0.8f, 0.8f, 0.8f, 1}}, tex = {{1,1,1,1}};
    for (int pass = 0; pass < 2; ++pass) {
        in.st[0].chan = 4; in.st[1].chan = pass ? 5 : 4;
        N3dsTevOut out; bool ok = n3dsTevBuild(in, out);
        V o = picaEval(out, tex, ras);
        printf("%s: build %d, pixel %.0f %.0f %.0f\n", pass ? "with chan 5 (fix)" : "chan ignored (old)", ok, o.c[0]*255, o.c[1]*255, o.c[2]*255);
    }
    {   // r32: texture x channel-1 colour (multiplier use, the Wollywog case): must show the texture, not black
        N3dsTevIn m = prog("C(15,8,10,15) A(7,7,7,4) map0", 0, 0xffffffff, 0xffffffff, 0xffffffff);
        m.primaryMap = 0; m.nUnits = 0; m.unitMap[0] = 0; m.st[0].chan = 5;
        V t2 = {{0.2f, 0.6f, 0.3f, 1}};
        N3dsTevOut out; bool ok = n3dsTevBuild(m, out);
        V o = picaEval(out, t2, ras);
        printf("texture x chan 5: build %d, pixel %.0f %.0f %.0f (texture 51 153 77)%s\n", ok, o.c[0]*255, o.c[1]*255, o.c[2]*255,
               (o.c[1] > 0.5f && o.c[0] > 0.1f) ? "" : "  << FAIL");
    }
    {   // r33: Wollywog skin (log d20f810d): lerp(A0, C0, TEXC); PREV + A1 x chan-1 highlight; PREV + C2. Must equal GX without the highlight.
        N3dsTevIn f = prog("C(3,2,8,15) A(7,4,5,7) map0; C(15,5,10,0) A(7,4,1,7) map0; C(15,6,12,0) A(7,7,7,0) map255", 0, 0x28966499, 0xb4ffffff, 0xff311d22);
        f.st[0].chan = 255; f.st[1].chan = 5; f.st[2].chan = 4;
        N3dsTevOut out; bool ok = n3dsTevBuild(f, out);
        V t3 = {{0.6f, 0.6f, 0.6f, 1}}, zero = {{0, 0, 0, 0}};
        V g = gxEval(f, t3, zero), o = picaEval(out, t3, ras);
        const bool same = fabsf(g.c[0] - o.c[0]) < 0.01f && fabsf(g.c[1] - o.c[1]) < 0.01f && fabsf(g.c[2] - o.c[2]) < 0.01f;
        printf("wollywog skin: build %d, GX %.0f %.0f %.0f, PICA %.0f %.0f %.0f%s\n", ok, g.c[0]*255, g.c[1]*255, g.c[2]*255, o.c[0]*255, o.c[1]*255, o.c[2]*255, same ? "" : "  << FAIL");
    }
    {   // r42: Wollywog body (log 8f32b8b7 #84): C0 x RASC, then A0 x previous on channel 1. The channel-1 alpha input becomes ZERO and used to
        // overwrite the constant's alpha (= A0) -> black. Must be C0 x ras x A0.
        N3dsTevIn w = prog("C(15,2,10,15) A(5,7,7,7) map255; C(15,3,0,10) A(5,7,7,7) map255", 0, 0x80dcc8c8, 0xffffffff, 0xffffffff);
        w.primaryMap = -1; w.nUnits = 0; w.unitMap[0] = -1; w.st[0].chan = 4; w.st[1].chan = 5;
        N3dsTevOut out; bool ok = n3dsTevBuild(w, out);
        V r = {{0.6f, 0.6f, 0.6f, 1}}, t = {{1, 1, 1, 1}};
        V o = picaEval(out, t, r);
        const float want = 0xc8 / 255.0f * 0.6f * (0x80 / 255.0f);
        printf("wollywog body: build %d, pixel %.0f %.0f %.0f (want %.0f)%s\n", ok, o.c[0]*255, o.c[1]*255, o.c[2]*255, want * 255, fabsf(o.c[0] - want) < 0.01f ? "" : "  << FAIL");
    }
    {   // r48: Yellow Wollywog body (log 684fe42e): s0 TEX x ras; s1 PREV + ras x TEX -> REG2 (C2); s2 C2 + specular (channel 1). Before r48 the
        // REG2 write was dropped and C2 read as 0 = black. Must be 2 x TEX x ras (specular not drawn).
        N3dsTevIn y = prog("C(15,8,10,15) A(7,4,5,7) map0; C(15,10,8,0) A(5,7,7,7) map0; C(10,6,12,10) A(5,7,7,7) map255", 0, 0x00000000, 0xffffffff, 0xffffffff);
        y.st[0].chan = 4; y.st[1].chan = 4; y.st[2].chan = 5; y.st[1].cout = 3;
        N3dsTevOut out; bool ok = n3dsTevBuild(y, out);
        V r = {{0.5f, 0.6f, 0.4f, 1}}, t = {{0.8f, 0.7f, 0.2f, 1}};
        V o = picaEval(out, t, r);
        const float w0 = 2 * 0.8f * 0.5f, w1 = 2 * 0.7f * 0.6f, w2 = 2 * 0.2f * 0.4f;
        const bool good = fabsf(o.c[0] - (w0 > 1 ? 1 : w0)) < 0.01f && fabsf(o.c[1] - (w1 > 1 ? 1 : w1)) < 0.01f && fabsf(o.c[2] - w2) < 0.01f;
        printf("yellow wollywog body: build %d, %d stages, buffer %x/%x, pixel %.0f %.0f %.0f (want %.0f %.0f %.0f)%s\n", ok, out.n, out.bufRgb, out.bufA,
               o.c[0]*255, o.c[1]*255, o.c[2]*255, (w0 > 1 ? 1 : w0)*255, (w1 > 1 ? 1 : w1)*255, w2*255, good ? "" : "  << FAIL");
    }
}
