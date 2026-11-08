#include <algorithm>
// HOST TEST for platform/n3ds_gfx.cpp (1j): primitive assembly order, trivial-reject culling (must never drop anything
// that could be visible), non-finite handling, scissor-aware culling, and the ring UI reserve.
//   ./run.sh      (needs only g++)
#include "mock_bridge.h"
#include "n3ds_texconv.h"
#include "n3ds_tev.h"
#include "n3ds_config.h"
#include "gl/pc_gfx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <random>
#include <vector>

static int gFail = 0, gChecks = 0;
#define CHECK(c, ...) do { ++gChecks; if (!(c)) { ++gFail; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { TRIANGLES = 0x90, QUADS = 0x80, STRIP = 0x98, FAN = 0xA0 };
static const float NEAR_ = 10.f, FAR_ = 1000.f;

static void setProjection()   // GX perspective (GXSetProjection layout): z_ndc = -1 at near, 0 at far
{
    const float cot = 1.0f / tanf(30.0f * 3.14159265f / 180.0f), aspect = 4.0f / 3.0f;
    Mtx44 p; memset(p, 0, sizeof p);
    p[0][0] = cot / aspect; p[1][1] = cot; p[2][2] = -NEAR_ / (FAR_ - NEAR_); p[2][3] = -FAR_ * NEAR_ / (FAR_ - NEAR_); p[3][2] = -1.0f;
    pc_gfx_set_projection(p, 0);
}
static void setup()
{
    pc_gfx_init();
    pc_gfx_set_viewport(0, 0, 640, 480, 0, 1); pc_gfx_set_scissor(0, 0, 640, 480);
    setProjection();
    Mtx id = {{1,0,0,0},{0,1,0,0},{0,0,1,0}}; pc_gfx_load_pos_mtx(id, 0); pc_gfx_set_current_mtx(0);
    pc_gfx_set_z_mode(1, 3, 1);                 // 3D: depth test on (LEQUAL), write on
    gRing.reset(98304); gRing.quiet = true;
}
struct P3 { float x, y, z; };
static int draw(int prim, const P3* v, int n)   // returns vertices committed to the ring by this draw
{
    const int before = gRing.nVerts;
    pc_gfx_begin(prim, 0, (u16)n);
    for (int i = 0; i < n; ++i) pc_gfx_position(v[i].x, v[i].y, v[i].z);
    pc_gfx_end();
    n3ds_gfx_flush();
    return gRing.nVerts - before;
}

static void testAssembly()
{
    setup();
    P3 s[5] = {{0,0,-100},{10,0,-100},{0,10,-100},{10,10,-100},{5,20,-100}};
    int n = draw(STRIP, s, 5);
    CHECK(n == 9, "strip of 5 -> 3 tris (9 verts), got %d", n);
    const N3dsVtx* r = gRing.verts();
    // expected orders (same as the hardware log of 1i): (P0,P1,P2) (P2,P1,P3) (P2,P3,P4); compare by clip w and x
    auto same = [&](const N3dsVtx& a, int idx) { return fabsf(a.w - 100.f) < 1e-3f; (void)idx; };
    CHECK(same(r[0], 0), "w");
    // x' = tilt[1]*cy, y' = tilt[3]... just check tri 2 = (P2,P1,P3): its first vertex equals tri 1's third
    CHECK(r[3].x == r[2].x && r[3].y == r[2].y, "tri2.v0 == P2");
    CHECK(r[4].x == r[1].x && r[4].y == r[1].y, "tri2.v1 == P1");
    CHECK(r[6].x == r[2].x && r[6].y == r[2].y, "tri3.v0 == P2");
    CHECK(r[7].x == r[5].x && r[7].y == r[5].y, "tri3.v1 == P3");
    setup();
    P3 q[4] = {{0,0,-100},{10,0,-100},{10,10,-100},{0,10,-100}};
    CHECK(draw(QUADS, q, 4) == 6, "quad -> 2 tris");
    P3 f[5] = {{0,0,-100},{10,0,-100},{10,10,-100},{0,10,-100},{-10,5,-100}};
    CHECK(draw(FAN, f, 5) == 9, "fan of 5 -> 3 tris");
    P3 t[3] = {{0,0,-100},{10,0,-100},{0,10,-100}};
    CHECK(draw(TRIANGLES, t, 3) == 3, "triangle passes");
    const N3dsVtx* v = gRing.verts() + 15;   // 6 + 9 verts before it
    CHECK(fabsf(v[0].w - 100.f) < 1e-3f, "w = -z_eye = 100, got %f", v[0].w);
    CHECK(fabsf(v[0].z - (-NEAR_ / (FAR_ - NEAR_) * -100.f - FAR_ * NEAR_ / (FAR_ - NEAR_))) < 1e-3f, "z_clip formula, got %f", v[0].z);
    CHECK(v[0].z <= 0 && v[0].z >= -v[0].w, "z within [-w, 0]");
}

static void testCullCases()
{
    setup();
    P3 vis[3]      = {{0,0,-100},{50,0,-100},{0,50,-100}};
    P3 beyondFar[3]= {{0,0,-2000},{50,0,-2000},{0,50,-2000}};
    P3 behind[3]   = {{0,0,50},{50,0,50},{0,50,50}};
    P3 tooNear[3]  = {{0,0,-5},{1,0,-5},{0,1,-5}};                  // in front of the camera but before the near plane
    P3 left[3]     = {{-5000,0,-100},{-4900,0,-100},{-5000,50,-100}};
    P3 above[3]    = {{0,5000,-100},{50,5000,-100},{0,5100,-100}};
    P3 straddle[3] = {{-5000,0,-100},{50,0,-100},{0,50,-100}};       // big triangle crossing the screen
    P3 nearCross[3]= {{0,0,-5},{50,0,-100},{0,50,-100}};             // crosses the near plane
    P3 farCross[3] = {{0,0,-500},{50,0,-500},{0,50,-2000}};          // crosses the far plane
    CHECK(draw(TRIANGLES, vis, 3) == 3, "visible kept");
    CHECK(draw(TRIANGLES, beyondFar, 3) == 0, "beyond far culled");
    CHECK(draw(TRIANGLES, behind, 3) == 0, "behind camera culled");
    CHECK(draw(TRIANGLES, tooNear, 3) == 0, "before near plane culled");
    CHECK(draw(TRIANGLES, left, 3) == 0, "off-screen left culled");
    CHECK(draw(TRIANGLES, above, 3) == 0, "off-screen top culled");
    {   // 1n: triangles that cross the near plane or reach far outside the viewport are clipped (not dropped, not passed through raw)
        auto clipped = [&](const P3* t, const char* what) {
            gRing.newFrame();
            const int n = draw(TRIANGLES, t, 3);
            CHECK(n >= 3 && n % 3 == 0, "%s: kept as whole triangles after clipping (%d verts)", what, n);
            bool inside = true; float minDist = 1e9f;
            for (int i = 0; i < n; ++i) { const N3dsVtx& v = gRing.verts()[i];
                inside = inside && v.w > 0.0f && v.z + v.w >= -1e-3f && fabsf(v.x) <= 3.0f * v.w * 1.0001f && fabsf(v.y) <= 3.0f * v.w * 1.0001f;
                if (v.z + v.w < minDist) minDist = v.z + v.w; }
            CHECK(inside, "%s: every emitted vertex lies inside the near plane and the +-3 viewport guard band", what);
        };
        clipped(straddle, "screen-crossing triangle"); clipped(nearCross, "near-plane-crossing triangle");
        // a triangle with one vertex BEHIND the camera must not produce the mirrored giant polygon: all output verts have w > 0
        P3 behindOne[3] = {{0,0,50},{50,0,-100},{0,50,-100}};
        clipped(behindOne, "one vertex behind the camera");
    }
    CHECK(draw(TRIANGLES, farCross, 3) == 3, "far-plane-crossing triangle kept");
    // NaN / inf must never reach the ring
    P3 nan3[3] = {{0,0,-100},{NAN,0,-100},{0,50,-100}};
    P3 inf3[3] = {{0,0,-100},{INFINITY,0,-100},{0,50,-100}};
    CHECK(draw(TRIANGLES, nan3, 3) == 0, "NaN triangle dropped");
    CHECK(draw(TRIANGLES, inf3, 3) == 0, "inf triangle dropped");
    // an orthographic 2D quad exactly covering the screen (w = 1, z in [-1,0]) must survive
    Mtx44 o; memset(o, 0, sizeof o);
    o[0][0] = 2.0f / 640; o[0][3] = -1.0f; o[1][1] = -2.0f / 480; o[1][3] = 1.0f; o[2][2] = -1.0f; o[2][3] = -1.0f; o[3][3] = 1.0f;
    pc_gfx_set_projection(o, 1);
    pc_gfx_set_z_mode(0, 7, 0);
    P3 ui[4] = {{0,0,0},{640,0,0},{640,480,0},{0,480,0}};
    CHECK(draw(QUADS, ui, 4) == 6, "full-screen ortho quad kept");
    P3 uiOff[4] = {{700,0,0},{800,0,0},{800,480,0},{700,480,0}};
    CHECK(draw(QUADS, uiOff, 4) == 0, "ortho quad fully right of the screen culled");
}

static void testScissor()
{
    setup();
    pc_gfx_set_viewport(0, 0, 640, 480, 0, 1);
    pc_gfx_set_scissor(0, 0, 320, 480);          // left half only
    P3 leftTri[3]  = {{-300,-100,-500},{-200,-100,-500},{-250,100,-500}};   // inside the left half
    P3 rightTri[3] = {{200,-100,-500},{300,-100,-500},{250,100,-500}};      // visible in the viewport but outside the scissor
    CHECK(draw(TRIANGLES, leftTri, 3) == 3, "inside scissor kept");
    CHECK(draw(TRIANGLES, rightTri, 3) == 0, "outside scissor culled");
    pc_gfx_set_scissor(0, 0, 640, 480);
    CHECK(draw(TRIANGLES, rightTri, 3) == 3, "same triangle kept once the scissor is full again");
    pc_gfx_set_scissor(100, 100, 0, 0);          // empty
    CHECK(draw(TRIANGLES, leftTri, 3) == 0, "empty scissor: nothing drawn");
}

// Property test: whatever the culler rejects must be genuinely invisible. Visibility reference: sample the triangle in
// homogeneous clip space (barycentric grid) and look for any point with w>0, -w<=z<=0, |x|<=w, |y|<=w.
static void testProperty()
{
    setup();
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> ux(-4000, 4000), uz(-3000, 300), ur(1, 800);
    std::uniform_real_distribution<float> ux2(-900, 900), uz2(-1200, -2);      // second population: mostly near the frustum
    const float cot = 1.0f / tanf(30.0f * 3.14159265f / 180.0f), asp = 4.0f / 3.0f;
    int culled = 0, kept = 0, wrongCull = 0, invisibleKept = 0;
    for (int it = 0; it < 60000; ++it) {
        P3 t[3]; const bool near = it & 1;
        const float cx = near ? ux2(rng) : ux(rng), cy = near ? ux2(rng) : ux(rng), cz = near ? uz2(rng) : uz(rng), r = ur(rng);
        for (int k = 0; k < 3; ++k) { t[k].x = cx + ux(rng) * r / 4000; t[k].y = cy + ux(rng) * r / 4000; t[k].z = cz + uz(rng) * r / 3000; }
        float H[3][4];
        for (int k = 0; k < 3; ++k) { H[k][0] = t[k].x * cot / asp; H[k][1] = t[k].y * cot; H[k][2] = -NEAR_ / (FAR_ - NEAR_) * t[k].z - FAR_ * NEAR_ / (FAR_ - NEAR_); H[k][3] = -t[k].z; }
        bool vis = false;
        for (int i = 0; i <= 40 && !vis; ++i) for (int j = 0; i + j <= 40 && !vis; ++j) {
            const float a = i / 40.f, b = j / 40.f, c = 1 - a - b, x = a*H[0][0]+b*H[1][0]+c*H[2][0], y = a*H[0][1]+b*H[1][1]+c*H[2][1],
                        z = a*H[0][2]+b*H[1][2]+c*H[2][2], w = a*H[0][3]+b*H[1][3]+c*H[2][3];
            if (w > 0 && z >= -w && z <= 0 && fabsf(x) <= w && fabsf(y) <= w) vis = true;
        }
        gRing.newFrame();
        const int n = draw(TRIANGLES, t, 3);
        bool coversCentre = vis;
        if (n == 0 && vis) {
            // The culler may also drop triangles that light no pixel: verify by brute force (pixel centres of the 400x240 screen).
            bool allW = H[0][3] > 1e-6f && H[1][3] > 1e-6f && H[2][3] > 1e-6f;
            if (allW) {
                float X[3], Y[3];
                for (int k = 0; k < 3; ++k) { X[k] = (H[k][0] / H[k][3] + 1) * 200.0f; Y[k] = (1 - H[k][1] / H[k][3]) * 120.0f; }
                const float mnx = std::min({X[0], X[1], X[2]}), mxx = std::max({X[0], X[1], X[2]}), mny = std::min({Y[0], Y[1], Y[2]}), mxy = std::max({Y[0], Y[1], Y[2]});
                coversCentre = false;
                for (int py = (int)floorf(mny) - 1; py <= (int)ceilf(mxy) + 1 && !coversCentre; ++py)
                    for (int px = (int)floorf(mnx) - 1; px <= (int)ceilf(mxx) + 1 && !coversCentre; ++px) {
                        const float cx0 = px + 0.5f, cy0 = py + 0.5f;
                        auto e = [&](int a, int b) { return (X[b] - X[a]) * (cy0 - Y[a]) - (Y[b] - Y[a]) * (cx0 - X[a]); };
                        const float e0 = e(0, 1), e1 = e(1, 2), e2 = e(2, 0);
                        if ((e0 >= -1e-3f && e1 >= -1e-3f && e2 >= -1e-3f) || (e0 <= 1e-3f && e1 <= 1e-3f && e2 <= 1e-3f)) coversCentre = true;
                    }
            }
        }
        if (n == 0) { ++culled; if (coversCentre) { ++wrongCull; if (wrongCull < 5) printf("  wrongly culled: (%f %f %f) (%f %f %f) (%f %f %f)\n", t[0].x,t[0].y,t[0].z,t[1].x,t[1].y,t[1].z,t[2].x,t[2].y,t[2].z); } }
        else { ++kept; if (!vis) ++invisibleKept; }
    }
    printf("  property: %d culled, %d kept (%d of the kept are invisible by sampling = conservative misses)\n", culled, kept, invisibleKept);
    CHECK(wrongCull == 0, "%d visible triangles were culled", wrongCull);
    CHECK(culled > 0 && kept > 0, "test scene degenerate");
}

static void testUiReserve()
{
    setup();
    P3 t[3] = {{0,0,-100},{50,0,-100},{0,50,-100}};
    gRing.freeOverride = 12288 - 1;              // below the UI reserve (8 chunks = 12288)
    CHECK(draw(TRIANGLES, t, 3) == 0, "3D draw refused while the ring is nearly full");
    pc_gfx_set_z_mode(0, 7, 0);                  // UI: depth test off
    CHECK(draw(TRIANGLES, t, 3) == 3, "UI draw still accepted from the reserve");
    gRing.freeOverride = -1;
    pc_gfx_set_z_mode(1, 3, 1);
    CHECK(draw(TRIANGLES, t, 3) == 3, "3D draw accepted with a healthy ring");
    // a real overflow: fill the ring with 3D quads and check that UI still lands afterwards
    setup();
    gRing.reset(3072);                           // 2 chunks only: the reserve rule makes 3D fail from the start
    CHECK(draw(TRIANGLES, t, 3) == 0, "tiny ring: 3D refused (below reserve)");
    pc_gfx_set_z_mode(0, 7, 0);
    CHECK(draw(TRIANGLES, t, 3) == 3, "tiny ring: UI accepted");
}


using namespace n3dsconv;

static void testTexConv()
{
    // RGB565 4x4 block: texel 0 = pure red 0xF800 -> r 255
    uint8_t blk[32]; memset(blk, 0, sizeof blk); blk[0] = 0xF8; blk[1] = 0x00; blk[2] = 0x07; blk[3] = 0xE0;   // texel0 red, texel1 green
    CHECK(gcPixel(RGB565, blk, 4, 4, 0, 0, nullptr) == 0xFF0000FFu, "RGB565 red -> 0xFF0000FF (AABBGGRR) got %08x", gcPixel(RGB565, blk, 4, 4, 0, 0, nullptr));
    CHECK(gcPixel(RGB565, blk, 4, 4, 1, 0, nullptr) == 0xFF00FF00u, "RGB565 green got %08x", gcPixel(RGB565, blk, 4, 4, 1, 0, nullptr));
    // I4 8x8 block: row 0 = 0x1F 0x00 .. -> texel0 = 1 (17), texel1 = 15 (255)
    uint8_t i4[32]; memset(i4, 0, sizeof i4); i4[0] = 0x1F;
    CHECK(gcPixel(I4, i4, 8, 8, 0, 0, nullptr) == 0x11111111u, "I4 texel 0");
    CHECK(gcPixel(I4, i4, 8, 8, 1, 0, nullptr) == 0xFFFFFFFFu, "I4 texel 1");
    // RGBA8 4x4 block: AR pairs then GB pairs
    uint8_t r8[64]; memset(r8, 0, sizeof r8); r8[0] = 0x80; r8[1] = 0x11; r8[32] = 0x22; r8[33] = 0x33;
    CHECK(gcPixel(RGBA8, r8, 4, 4, 0, 0, nullptr) == 0x80332211u, "RGBA8 got %08x", gcPixel(RGBA8, r8, 4, 4, 0, 0, nullptr));
    // RGB5A3: bit15 set = opaque 555; clear = 3-bit alpha + 444
    uint8_t a3[32]; memset(a3, 0, sizeof a3); a3[0] = 0xFC; a3[1] = 0x00; a3[2] = 0x7F; a3[3] = 0x00;    // 0xFC00: opaque, r=31; 0x7F00: a=7 r=15 g=15 b=0
    CHECK(gcPixel(RGB5A3, a3, 4, 4, 0, 0, nullptr) == 0xFF0000FFu, "RGB5A3 opaque red got %08x", gcPixel(RGB5A3, a3, 4, 4, 0, 0, nullptr));
    CHECK((gcPixel(RGB5A3, a3, 4, 4, 1, 0, nullptr) >> 24) == 255, "RGB5A3 a=7 -> 255");
    // CMPR: c0 = white, c1 = black (c0 > c1 -> 4 colour mode). index 2 = 2/3 white, 3 = 1/3
    uint8_t cm[32]; memset(cm, 0, sizeof cm); cm[0] = 0xFF; cm[1] = 0xFF; cm[2] = 0; cm[3] = 0; cm[4] = 0b00011011;   // row 0 indices 0,1,2,3
    CHECK((gcPixel(CMPR, cm, 8, 8, 0, 0, nullptr) & 0xFFFFFF) == 0xFFFFFF, "CMPR idx0 white");
    CHECK((gcPixel(CMPR, cm, 8, 8, 1, 0, nullptr) & 0xFFFFFF) == 0, "CMPR idx1 black");
    CHECK((gcPixel(CMPR, cm, 8, 8, 2, 0, nullptr) & 255) == 170, "CMPR idx2 = 2/3 got %u", gcPixel(CMPR, cm, 8, 8, 2, 0, nullptr) & 255);
    // C8 via IA8 palette
    uint8_t pal[4] = {0x80, 0x40, 0xFF, 0xC0}; uint8_t c8[32]; memset(c8, 0, sizeof c8); c8[0] = 1;
    Palette P{pal, 0, 2};
    CHECK(gcPixel(C8, c8, 8, 4, 0, 0, &P) == 0xFFC0C0C0u, "C8/IA8 palette got %08x", gcPixel(C8, c8, 8, 4, 0, 0, &P));
    CHECK(gcDataSize(CMPR, 16, 16) == 128 && gcDataSize(RGBA8, 8, 8) == 256 && gcDataSize(I4, 32, 8) == 128, "data sizes");

    // swizzle: 16x16 RGBA_RAW red at (9,2), everything else black -> PICA RGBA8, tile (1,0), in-tile Morton index of (1,2) = 0b000110? x=1,y=2
    uint8_t raw[16 * 16 * 4]; memset(raw, 0, sizeof raw);
    for (int i = 0; i < 256; ++i) raw[i * 4 + 3] = 255;
    raw[(2 * 16 + 9) * 4 + 0] = 255;
    uint8_t out[16 * 16 * 4]; convert(RAW_RGBA, raw, 16, 16, nullptr, P_RGBA8, 16, 16, false, out);
    const int mort = (1 & 1) | ((2 & 1) << 1) | (((1 >> 1) & 1) << 2) | (((2 >> 1) & 1) << 3);   // x0 y0 x1 y1 -> 1 + 0 + 0 + 8 = 9
    const uint8_t* px = out + (size_t)(1 * 64 + mort) * 4;      // tile index 1 (tilesX=2, ty=0, tx=1)
    CHECK(px[0] == 255 && px[3] == 255 && px[1] == 0 && px[2] == 0, "swizzled red texel: bytes %02x %02x %02x %02x", px[0], px[1], px[2], px[3]);
    // non power of two: 10x6 -> 16x8 pot, padding replicates the edge
    CHECK(potSize(10) == 16 && potSize(6) == 8 && potSize(1) == 8 && potSize(513) == 1024, "potSize");
    // format choice
    Analysis op{false, false, false}, gr{false, false, true}, cut{true, false, false}, tr{true, true, false};
    CHECK(chooseFormat(op, 64, 64) == P_RGB565 && chooseFormat(gr, 64, 64) == P_L8 && chooseFormat(cut, 64, 64) == P_RGBA5551, "format choice opaque/gray/cutout");
    CHECK(chooseFormat(tr, 64, 64) == P_RGBA8 && chooseFormat(tr, 256, 256) == P_RGBA4, "format choice translucent by size");
    Analysis ai = analyse(I8, i4, 8, 8, nullptr);
    CHECK(ai.alpha && ai.gray, "I8 analysed as gray + alpha");
}


static void testConfigAndScale()
{
    N3dsConfig c; char bad[64];
    const int nb = n3dsConfigParse("# comment\ntex_shift = 1   ; half size\n tex_max_dim=256\r\ntex_box=0\nbogus = 3\ntex_min_dim = 32\ntex_budget_kb=0x400\nnot a line\n", c, bad, sizeof bad);
    CHECK(c.texShift == 1 && c.texMaxDim == 256 && c.texBox == 0 && c.texMinDim == 32 && c.texBudgetKb == 1024, "ini values parsed (shift %d max %d box %d min %d budget %d)", c.texShift, c.texMaxDim, c.texBox, c.texMinDim, c.texBudgetKb);
    CHECK(nb == 2 && strstr(bad, "bogus") && strstr(bad, "not a line"), "unknown keys / junk lines reported (%d: '%s')", nb, bad);
    CHECK(c.fpsCap == 30, "fps_cap defaults to 30");
    N3dsConfig fc; n3dsConfigParse("fps_cap = 60\n", fc, nullptr, 0); CHECK(fc.fpsCap == 60, "fps_cap = 60 parsed");
    n3dsConfigParse("fps_cap = 0\n", fc, nullptr, 0); CHECK(fc.fpsCap == 0, "fps_cap = 0 (uncapped) parsed");
    N3dsConfig d; n3dsConfigParse("tex_shift=9\ntex_min_dim=1\n", d, nullptr, 0);
    CHECK(d.texShift == 3 && d.texMinDim == 8, "values are clamped");

    // 16x16 black/white checkerboard (RAW_RGBA), shrink by 2: box average = mid grey everywhere, point sample = pure black or white.
    static uint8_t raw[16 * 16 * 4];
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) { const uint8_t v = ((x + y) & 1) ? 255 : 0; uint8_t* p = raw + (y * 16 + x) * 4; p[0] = p[1] = p[2] = v; p[3] = 255; }
    uint8_t out[8 * 8 * 4];
    convertScaled(RAW_RGBA, raw, 16, 16, nullptr, P_RGBA8, 8, 8, false, out, 1, true);
    CHECK(out[3] > 120 && out[3] < 135 && out[0] == 255, "box shrink of a checkerboard = grey (r=%u a=%u)", out[3], out[0]);
    convertScaled(RAW_RGBA, raw, 16, 16, nullptr, P_RGBA8, 8, 8, false, out, 1, false);
    CHECK(out[3] == 255 || out[3] == 0, "point-sampled shrink keeps one source texel (r=%u)", out[3]);
    // 10x6 shrunk by 2 = 5x3 inside an 8x8 pot: the padding replicates the last column/row, so texel (7,7) == texel (4,2)
    static uint8_t raw2[10 * 6 * 4]; for (size_t i = 0; i < sizeof raw2; ++i) raw2[i] = (uint8_t)(i * 7);
    uint8_t o2[8 * 8 * 2];
    convertScaled(RAW_RGBA, raw2, 10, 6, nullptr, P_RGB565, 8, 8, false, o2, 1, false);
    auto texel = [&](int x, int y) { const uint8_t* p = o2 + ((size_t)(y >> 3) * 1 + (x >> 3)) * 64 * 2; (void)p; int i = 0; for (int b = 0; b < 3; ++b) i |= (((x >> b) & 1) << (2 * b)) | (((y >> b) & 1) << (2 * b + 1)); return (uint32_t)(o2[i * 2] | o2[i * 2 + 1] << 8); };
    CHECK(texel(7, 7) == texel(4, 2) && texel(6, 1) == texel(4, 1), "pot padding replicates the shrunk image's edge");
}


static void testVertexCache()
{
    setup();
    static float pos[8][3]; for (int i = 0; i < 8; ++i) { pos[i][0] = (float)((i * 97) % 400 - 200); pos[i][1] = (float)((i * 53) % 300 - 150); pos[i][2] = -500.0f - 10 * i; }
    pc_gfx_clear_vtx_desc(); pc_gfx_set_vtx_desc(9, 2);                          // GX_VA_POS, INDEX8
    pc_gfx_set_vtx_attr_fmt(0, 9, 1, 4, 0); pc_gfx_set_array(9, pos, 12);        // XYZ, F32
    uint8_t idx[60]; for (int i = 0; i < 60; ++i) idx[i] = (uint8_t)((i * 5 + (i >> 2)) & 7);   // heavy repetition
    std::vector<uint8_t> dl; dl.push_back(0x98); dl.push_back(0); dl.push_back(60); for (int i = 0; i < 60; ++i) dl.push_back(idx[i]);   // triangle strip, fmt 0
    const int before = gRing.nVerts;
    pc_gfx_call_display_list(dl.data(), (u32)dl.size()); n3ds_gfx_flush();
    const int nDl = gRing.nVerts - before;
    std::vector<N3dsVtx> a(gRing.verts() + before, gRing.verts() + before + nDl);
    // reference: the same strip through immediate mode (no vertex cache involved)
    setup();
    const int before2 = gRing.nVerts;
    pc_gfx_begin(0x98, 0, 60);
    for (int i = 0; i < 60; ++i) pc_gfx_position(pos[idx[i]][0], pos[idx[i]][1], pos[idx[i]][2]);
    pc_gfx_end(); n3ds_gfx_flush();
    const int nImm = gRing.nVerts - before2;
    CHECK(nDl > 0 && nDl == nImm, "display list (cached) and immediate mode emit the same number of vertices (%d vs %d)", nDl, nImm);
    bool same = nDl == nImm;
    for (int i = 0; same && i < nDl; ++i) same = memcmp(&a[i], gRing.verts() + before2 + i, sizeof(N3dsVtx)) == 0;
    CHECK(same, "vertex cache output is bit-identical to the uncached path");
}


extern "C" unsigned n3ds_gfx_debug_vcache_hits(void);

// Regression: skinned GX models carry a DIRECT 1-byte matrix index (GX_VA_PNMTXIDX) next to indexed position. 1l gated the vertex
// cache on "every attribute indexed", so these lists never hit the cache ("0 of 71010" in the field log).
static void testVertexCacheDirectMtx()
{
    setup();
    static float pos[6][3]; for (int i = 0; i < 6; ++i) { pos[i][0] = (float)(i * 60 - 150); pos[i][1] = (float)((i * 37) % 200 - 100); pos[i][2] = -500.0f - 5 * i; }
    Mtx id0 = {{1,0,0,0},{0,1,0,0},{0,0,1,0}}, id3 = {{1,0,0,70},{0,1,0,0},{0,0,1,0}};     // matrix at offset 3 shifts x by 70
    pc_gfx_load_pos_mtx(id0, 0); pc_gfx_load_pos_mtx(id3, 3);
    pc_gfx_clear_vtx_desc(); pc_gfx_set_vtx_desc(0, 1); pc_gfx_set_vtx_desc(9, 2);          // PNMTXIDX direct, POS index8
    pc_gfx_set_vtx_attr_fmt(0, 9, 1, 4, 0); pc_gfx_set_array(9, pos, 12);
    const int N = 48; uint8_t mt[N], ix[N];
    for (int i = 0; i < N; ++i) { mt[i] = (uint8_t)((i % 3 == 1) ? 3 : 0); ix[i] = (uint8_t)((i / 2 + i) % 6); }   // plenty of repeats, same pos under both matrices
    std::vector<uint8_t> dl; dl.push_back(0x98); dl.push_back(0); dl.push_back((uint8_t)N);
    for (int i = 0; i < N; ++i) { dl.push_back(mt[i]); dl.push_back(ix[i]); }
    const int before = gRing.nVerts;
    const unsigned hits0 = n3ds_gfx_debug_vcache_hits();           // the counter is cumulative until the frame ends: measure the delta
    pc_gfx_call_display_list(dl.data(), (u32)dl.size()); n3ds_gfx_flush();
    const unsigned hits = n3ds_gfx_debug_vcache_hits() - hits0;
    const int nDl = gRing.nVerts - before;
    std::vector<N3dsVtx> a(gRing.verts() + before, gRing.verts() + before + nDl);
    setup();
    pc_gfx_load_pos_mtx(id0, 0); pc_gfx_load_pos_mtx(id3, 3);
    const int before2 = gRing.nVerts;
    pc_gfx_begin(0x98, 0, (u16)N);
    for (int i = 0; i < N; ++i) { pc_gfx_set_current_mtx(mt[i]); pc_gfx_position(pos[ix[i]][0], pos[ix[i]][1], pos[ix[i]][2]); }
    pc_gfx_end(); n3ds_gfx_flush();
    const int nImm = gRing.nVerts - before2;
    CHECK(hits > 0, "display list with a direct matrix-index attribute uses the vertex cache (hits %u)", hits);
    CHECK(nDl > 0 && nDl == nImm, "cached DL and immediate reference emit the same vertex count (%d vs %d)", nDl, nImm);
    bool same = nDl == nImm;
    for (int i = 0; same && i < nDl; ++i) same = memcmp(&a[i], gRing.verts() + before2 + i, sizeof(N3dsVtx)) == 0;
    CHECK(same, "same index tuple under two different matrices is NOT merged by the cache (output bit-identical to uncached)");
}


extern "C" void n3ds_gfx_debug_dlcull(int on);
extern "C" unsigned n3ds_gfx_debug_seg_culled(void);

// Display-list segment culling must be invisible: same ring output, bit for bit, as processing every command; and it must actually skip work.
static void testDlCull()
{
    // a "map": 40 x 40 tiles of two single-triangle FAN commands each (the game's layout), row-major so neighbours are neighbours in the list
    const int GN = 40; static float pos[(GN + 1) * (GN + 1)][3];
    for (int j = 0; j <= GN; ++j) for (int i = 0; i <= GN; ++i) { float* q = pos[j * (GN + 1) + i]; q[0] = (float)(i * 40 - 800); q[1] = (float)((i * 7 + j * 13) % 9); q[2] = (float)(-j * 40 - 60); }
    std::vector<uint8_t> dl;
    auto vtx = [&](int idx) { dl.push_back(0); dl.push_back((uint8_t)(idx >> 8)); dl.push_back((uint8_t)idx); };       // PNMTXIDX direct (0) + POS index16
    for (int j = 0; j < GN; ++j) for (int i = 0; i < GN; ++i) {
        const int a = j * (GN + 1) + i, b = a + 1, c = a + (GN + 1), d = c + 1;
        dl.push_back(0xA0); dl.push_back(0); dl.push_back(3); vtx(a); vtx(c); vtx(b);
        dl.push_back(0xA0); dl.push_back(0); dl.push_back(3); vtx(b); vtx(c); vtx(d);
    }
    unsigned skippedTotal = 0; bool allSame = true; int views = 0, nonEmpty = 0;
    const float camX[] = {0, -600, 500, 0, 300, 0, -200}, camZ[] = {0, 0, 200, 900, 1300, 0, 300}, yaw[] = {0, 0.4f, -0.5f, 3.14159f, 1.0f, 0, 0.2f};
    for (int v = 0; v < 7; ++v) {
        std::vector<N3dsVtx> out[2];
        for (int mode = 0; mode < 2; ++mode) {
            n3ds_gfx_debug_dlcull(mode == 0);
            setup();
            const float cs = cosf(yaw[v]), sn = sinf(yaw[v]);
            Mtx m = {{cs, 0, sn, -camX[v]}, {0, 1, 0, -30}, {-sn, 0, cs, camZ[v]}};
            pc_gfx_load_pos_mtx(m, 0);
            if (v == 5) { pc_gfx_set_viewport(0, 0, 40, 30, 0, 1); pc_gfx_set_scissor(0, 0, 40, 30); }           // tiny viewport: most triangles are sub-pixel
            if (v == 6) { pc_gfx_set_scissor(200, 100, 200, 150); }                                           // scissor sub-rect
            pc_gfx_clear_vtx_desc(); pc_gfx_set_vtx_desc(0, 1); pc_gfx_set_vtx_desc(9, 3);
            pc_gfx_set_vtx_attr_fmt(0, 9, 1, 4, 0); pc_gfx_set_array(9, pos, 12);
            for (int rep = 0; rep < 3; ++rep) {                 // call the same list repeatedly: first call builds the bounds, later ones use them
                const int before = gRing.nVerts;
                pc_gfx_call_display_list(dl.data(), (u32)dl.size()); n3ds_gfx_flush();
                if (rep == 2) out[mode].assign(gRing.verts() + before, gRing.verts() + gRing.nVerts);
            }
            if (mode == 0) skippedTotal += n3ds_gfx_debug_seg_culled();
        }
        ++views;
        if (!out[0].empty()) ++nonEmpty;
        if (out[0].size() != out[1].size() || memcmp(out[0].data(), out[1].data(), out[0].size() * sizeof(N3dsVtx)) != 0) {
            allSame = false; printf("  dlcull view %d: %zu vs %zu verts\n", v, out[0].size(), out[1].size());
        }
    }
    n3ds_gfx_debug_dlcull(1);
    CHECK(allSame, "segment culling gives bit-identical ring output to per-triangle culling over %d views", views);
    CHECK(nonEmpty >= 4, "the test views actually show geometry (%d of %d)", nonEmpty, views);
    CHECK(skippedTotal > 0, "segments were skipped (%u)", skippedTotal);
    printf("  dlcull: %u segments skipped over %d views\n", skippedTotal, views);
}

static N3dsTevIn baseTev()
{
    N3dsTevIn t; memset(&t, 0, sizeof t);
    t.nStages = 1; t.primaryMap = 0; t.st[0].map = 0;
    for (int i = 0; i < 4; ++i) { t.st[0].ci[i] = 15; t.st[0].ai[i] = 7; }
    return t;
}

static void testTev()
{
    N3dsTevOut o;
    N3dsTevIn t = baseTev();
    t.primaryMap = -1; t.st[0].ci[3] = 10; t.st[0].ai[3] = 5;                         // PASSCLR
    CHECK(n3dsTevBuild(t, o) && o.n == 1 && o.st[0].rgbFn == N3DS_TFN_REPLACE && o.st[0].rgbSrc[0] == N3DS_TS_PRIMARY && o.approximations == 0, "PASSCLR = replace(primary)");
    t = baseTev(); t.st[0].ci[1] = 8; t.st[0].ci[2] = 10; t.st[0].ai[1] = 4; t.st[0].ai[2] = 5;   // MODULATE
    CHECK(n3dsTevBuild(t, o) && o.st[0].rgbFn == N3DS_TFN_MODULATE && o.st[0].aFn == N3DS_TFN_MODULATE && o.approximations == 0, "MODULATE maps exactly");
    CHECK((o.st[0].rgbSrc[0] == N3DS_TS_TEX0 && o.st[0].rgbSrc[1] == N3DS_TS_PRIMARY) || (o.st[0].rgbSrc[0] == N3DS_TS_PRIMARY && o.st[0].rgbSrc[1] == N3DS_TS_TEX0), "MODULATE operands = tex, primary");
    t = baseTev(); t.st[0].ci[3] = 8; t.st[0].ai[3] = 4;                              // REPLACE
    CHECK(n3dsTevBuild(t, o) && o.st[0].rgbFn == N3DS_TFN_REPLACE && o.st[0].rgbSrc[0] == N3DS_TS_TEX0, "REPLACE = tex");
    t = baseTev(); t.st[0].ci[0] = 15; t.st[0].ci[1] = 8; t.st[0].ci[2] = 2; t.st[0].ci[3] = 15; t.reg[1] = 0xFF804020u;   // tex * C0
    CHECK(n3dsTevBuild(t, o) && o.st[0].rgbFn == N3DS_TFN_MODULATE && (o.st[0].color & 0xFFFFFF) == 0x804020u, "tex * C0 uses the constant colour, got %08x", o.st[0].color);
    t = baseTev(); t.nStages = 2; t.st[1].map = 0; for (int k = 0; k < 4; ++k) { t.st[1].ci[k] = 15; t.st[1].ai[k] = 7; } t.st[0].ci[3] = 8; t.st[1].ci[1] = 0; t.st[1].ci[2] = 10; t.st[1].ci[3] = 15;   // stage 1: CPREV * RASC
    CHECK(n3dsTevBuild(t, o) && o.n == 2 && o.st[1].rgbSrc[0] + o.st[1].rgbSrc[1] == N3DS_TS_PREV + N3DS_TS_PRIMARY, "two stages chain through PREVIOUS");
    t = baseTev(); t.nStages = 7;
    CHECK(!n3dsTevBuild(t, o), "7 stages cannot be expressed");
    t = baseTev(); t.st[0].cout = 1;
    CHECK(!n3dsTevBuild(t, o), "results kept in a TEV register cannot be expressed");
    t = baseTev(); t.st[0].ci[0] = 2; t.st[0].ci[1] = 8; t.st[0].ci[2] = 10; t.st[0].ci[3] = 15; t.reg[1] = 0xFF804020u;   // lerp(C0, TEXC, RASC) (C0 non-trivial: exactly black/white would fold into ZERO/ONE and become a modulate)
    CHECK(n3dsTevBuild(t, o) && o.st[0].rgbFn == N3DS_TFN_INTERP, "a,b,c,0 = interpolate");
    t = baseTev(); t.st[0].ci[0] = 2; t.st[0].ci[1] = 8; t.st[0].ci[2] = 10; t.st[0].ci[3] = 4; t.reg[1] = 0xFF804020u; t.reg[2] = 0xFF102030u;   // full d + lerp form
    CHECK(n3dsTevBuild(t, o) && o.approximations > 0, "full a,b,c,d form is flagged approximate");
}


// Multi-texture combiner programs: the pellet glow material (4 stages, 3 textures, all results to PREVIOUS).
static void testTevMultiUnit()
{
    N3dsTevIn t; memset(&t, 0, sizeof t);
    t.nStages = 4; t.nUnits = 3; t.unitMap[0] = 0; t.unitMap[1] = 1; t.unitMap[2] = 2; t.primaryMap = 0; t.reg[1] = 0xC8FF3300u; t.reg[0] = 0xFFFFFFFFu;
    auto S = [&](int i, int map, int a, int b, int c, int d, int aa, int ab, int ac, int ad) {
        t.st[i].map = (uint8_t)map; t.st[i].ci[0] = (uint8_t)a; t.st[i].ci[1] = (uint8_t)b; t.st[i].ci[2] = (uint8_t)c; t.st[i].ci[3] = (uint8_t)d;
        t.st[i].ai[0] = (uint8_t)aa; t.st[i].ai[1] = (uint8_t)ab; t.st[i].ai[2] = (uint8_t)ac; t.st[i].ai[3] = (uint8_t)ad; };
    S(0, 0, 2, 15, 8, 8, 7, 7, 7, 7);        // T0 + (1-T0)*C0
    S(1, 1, 0, 8, 9, 15, 7, 7, 7, 7);        // lerp(PREV, T1.rgb, T1.a)
    S(2, 2, 15, 12, 0, 8, 7, 7, 7, 7);       // PREV + T2
    S(3, 255, 15, 10, 0, 15, 7, 7, 7, 7);    // RASC * PREV
    N3dsTevOut o;
    CHECK(n3dsTevBuild(t, o) && o.n == 4, "pellet-style 4 stage program builds");
    auto uses = [&](int stage, int src) { for (int k = 0; k < 3; ++k) if (o.st[stage].rgbSrc[k] == src) return true; return false; };
    CHECK(uses(0, N3DS_TS_TEX0) && !uses(0, N3DS_TS_TEX1), "stage 0 reads texture unit 0");
    CHECK(uses(1, N3DS_TS_TEX1) && o.st[1].rgbFn == N3DS_TFN_INTERP, "stage 1 interpolates with texture unit 1");
    CHECK(uses(2, N3DS_TS_TEX2) && uses(2, N3DS_TS_PREV) && o.st[2].rgbFn == N3DS_TFN_ADD, "stage 2 adds texture unit 2 to the previous result");
    CHECK(uses(3, N3DS_TS_PRIMARY) && uses(3, N3DS_TS_PREV) && o.st[3].rgbFn == N3DS_TFN_MODULATE, "stage 3 modulates with the vertex colour");
    // a map that has no unit reads as white (approximation), it must not alias unit 0
    t.unitMap[2] = -1; t.nUnits = 2;
    CHECK(n3dsTevBuild(t, o) && o.approximations > 0 && !uses(2, N3DS_TS_TEX2), "a texture without a PICA unit is flagged approximate");
}

// Programs that keep a result in a TEV register (the meteor / cave materials): lenient mode drops those stages and reads the register as 0.
static void testTevLenient()
{
    N3dsTevIn t; memset(&t, 0, sizeof t);
    t.nStages = 5; t.nUnits = 1; t.unitMap[0] = 0; t.primaryMap = 0; t.unitMap[1] = t.unitMap[2] = -1;
    for (int i = 0; i < 5; ++i) for (int k = 0; k < 4; ++k) { t.st[i].ci[k] = 15; t.st[i].ai[k] = 7; }
    t.reg[1] = t.reg[2] = t.reg[3] = 0xFFFFFFFFu; t.reg[0] = 0xFF000000u;
    t.st[0].map = 1; t.st[0].ci[3] = 8; t.st[0].cout = 3;                                 // C2 = T1
    t.st[1].map = 2; t.st[1].ci[1] = 8; t.st[1].ci[2] = 6; t.st[1].cout = 3;              // C2 = T2 * C2
    t.st[2].map = 0; t.st[2].ci[1] = 8; t.st[2].ci[2] = 10;                               // PREV = T0 * RASC
    t.st[3].map = 0; t.st[3].ci[1] = 8; t.st[3].ci[2] = 0;                                // PREV = T0 * PREV
    t.st[4].map = 255; t.st[4].ci[1] = 0; t.st[4].ci[2] = 6; t.st[4].ci[3] = 0;           // PREV = PREV + PREV * C2
    N3dsTevOut o;
    CHECK(!n3dsTevBuild(t, o), "strict mode refuses register results");
    t.lenient = true;
    CHECK(n3dsTevBuild(t, o) && o.n == 5 && o.approximations > 0, "lenient mode builds the program, register colour results dropped (n %d)", o.n);
    CHECK(o.st[0].rgbFn == N3DS_TFN_REPLACE && o.st[1].rgbFn == N3DS_TFN_REPLACE, "the two register stages only pass the colour through");
    CHECK(o.st[2].rgbFn == N3DS_TFN_MODULATE && o.st[2].rgbSrc[0] + o.st[2].rgbSrc[1] == N3DS_TS_TEX0 + N3DS_TS_PRIMARY, "stage 2 = T0 * RASC");
    CHECK(o.st[3].rgbFn == N3DS_TFN_MODULATE && o.st[3].rgbSrc[0] + o.st[3].rgbSrc[1] == N3DS_TS_TEX0 + N3DS_TS_PREV, "stage 3 = T0 * PREV");
    CHECK(o.st[4].rgbFn == N3DS_TFN_REPLACE && o.st[4].rgbSrc[0] == N3DS_TS_PREV, "PREV + PREV*C2 with C2 dropped = PREV passes through");
    // stage 0 of a program whose first stage only fills a register: the first real stage must still read the TEV register 0 for CPREV
    N3dsTevIn u = t; u.st[2].ci[1] = 0; u.st[2].ci[2] = 8;                                // PREV = PREV * T0 as the first emitted stage
    CHECK(n3dsTevBuild(u, o) && o.st[0].rgbSrc[0] == N3DS_TS_CONST, "CPREV before any stage was emitted is the constant register 0, not PREVIOUS");
}

// Texture coordinate generation + texture matrices: environment mapping from normals, tiling through a scale matrix, two units at once.
static void testTexgen()
{
    setup();
    static uint8_t raw[16 * 16 * 4]; memset(raw, 200, sizeof raw);
    GXTexObj o0, o1; pc_gfx_init_tex_obj_rgba(&o0, raw, 16, 16, 1, 1); pc_gfx_init_tex_obj_rgba(&o1, raw + 4, 16, 16, 1, 1);
    pc_gfx_load_tex_obj(&o0, 0); pc_gfx_load_tex_obj(&o1, 1);
    pc_gfx_set_num_tev_stages(2);
    pc_gfx_set_tev_order(0, 0, 0, 4); pc_gfx_set_tev_color_in(0, 15, 15, 15, 8); pc_gfx_set_tev_alpha_in(0, 7, 7, 7, 4);       // stage 0: T0 (coord 0)
    pc_gfx_set_tev_order(1, 1, 1, 4); pc_gfx_set_tev_color_in(1, 15, 8, 0, 15); pc_gfx_set_tev_alpha_in(1, 7, 4, 0, 7);       // stage 1: T1 (coord 1) * PREV
    pc_gfx_set_tev_color_op(0, 0, 0, 0, 1, 0); pc_gfx_set_tev_color_op(1, 0, 0, 0, 1, 0); pc_gfx_set_tev_alpha_op(0, 0, 0, 0, 1, 0); pc_gfx_set_tev_alpha_op(1, 0, 0, 0, 1, 0);
    pc_gfx_set_tex_coord_gen(0, 1, 1, 33);          // coord 0 = 2x4 matrix 33 x NORMAL (environment map)
    pc_gfx_set_tex_coord_gen(1, 1, 4, 36);          // coord 1 = 2x4 matrix 36 x TEX0 (tiling)
    Mtx env = {{0.5f, 0, 0, 0.5f}, {0, 0.5f, 0, 0.25f}, {0, 0, 1, 0}}, tile = {{2, 0, 0, 0}, {0, 3, 0, 0.5f}, {0, 0, 1, 0}};
    pc_gfx_load_tex_mtx(env, 33); pc_gfx_load_tex_mtx(tile, 36);
    static float pos[3][3] = {{-100, -100, -500}, {100, -100, -500}, {0, 100, -500}};
    static float nrm[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    static float uvs[3][2] = {{0.25f, 0.1f}, {0.5f, 0.2f}, {0.75f, 0.3f}};
    pc_gfx_clear_vtx_desc(); pc_gfx_set_vtx_desc(9, 2); pc_gfx_set_vtx_desc(10, 2); pc_gfx_set_vtx_desc(13, 2);   // POS, NRM, TEX0 all INDEX8
    pc_gfx_set_vtx_attr_fmt(0, 9, 1, 4, 0); pc_gfx_set_vtx_attr_fmt(0, 10, 0, 4, 0); pc_gfx_set_vtx_attr_fmt(0, 13, 1, 4, 0);
    pc_gfx_set_array(9, pos, 12); pc_gfx_set_array(10, nrm, 12); pc_gfx_set_array(13, uvs, 8);
    std::vector<uint8_t> dl = {0x90, 0, 3, 0, 0, 0, 1, 1, 1, 2, 2, 2};                 // 3 vertices, one index each for POS / NRM / TEX0
    const int before = gRing.nVerts;
    pc_gfx_call_display_list(dl.data(), (u32)dl.size()); n3ds_gfx_flush();
    CHECK(gRing.nVerts - before == 3, "textured triangle with generated coordinates drawn");
    CHECK(gRing.lastState.texId != 0 && gRing.lastState.tex1Id != 0 && gRing.lastState.tex2Id == 0, "two texture units reach the draw state (%u %u %u)", gRing.lastState.texId, gRing.lastState.tex1Id, gRing.lastState.tex2Id);
    CHECK(gRing.lastState.nTev == 2, "two combiner stages (%d)", gRing.lastState.nTev);
    bool t1 = false; for (int k = 0; k < 3; ++k) if (gRing.lastState.tev[1].rgbSrc[k] == N3DS_TS_TEX1) t1 = true;
    CHECK(t1, "stage 1 samples texture unit 1");
    const N3dsVtx* v = gRing.verts() + before;
    const float su = 1.0f, sv = 1.0f;
    float maxErr = 0;
    for (int i = 0; i < 3; ++i) {
        const float eu = (0.5f * nrm[i][0] + 0.5f) * su, ev = 1.0f - (0.5f * nrm[i][1] + 0.25f) * sv;          // unit 0: environment map
        const float fu = (2.0f * uvs[i][0]) * su, fv = 1.0f - (3.0f * uvs[i][1] + 0.5f) * sv;                    // unit 1: tiling matrix
        maxErr = fmaxf(maxErr, fmaxf(fmaxf(fabsf(v[i].u - eu), fabsf(v[i].v - ev)), fmaxf(fabsf(v[i].u1 - fu), fabsf(v[i].v1 - fv))));
    }
    CHECK(maxErr < 1e-4f, "unit 0 = environment map from normals, unit 1 = scaled texcoords (max error %g; v0 u %g v %g | u1 %g v1 %g)", maxErr, v[0].u, v[0].v, v[0].u1, v[0].v1);
    // the matrix is read live: reload it and draw again
    Mtx env2 = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}; pc_gfx_load_tex_mtx(env2, 33);
    const int b2 = gRing.nVerts;
    pc_gfx_call_display_list(dl.data(), (u32)dl.size()); n3ds_gfx_flush();
    CHECK(fabsf(gRing.verts()[b2].u - nrm[0][0]) < 1e-4f, "a reloaded texture matrix applies to the next draw");
    // a per-vertex texture matrix id (GX_VA_TEX0MTXIDX direct byte) selects the matrix and must not disturb the position matrix
    Mtx shift = {{1, 0, 0, 0.125f}, {0, 1, 0, 0}, {0, 0, 1, 0}}; pc_gfx_load_tex_mtx(shift, 36);
    pc_gfx_set_vtx_desc(1, 1);                                                                                     // GX_VA_TEX0MTXIDX direct
    std::vector<uint8_t> dl2 = {0x90, 0, 3, 36, 0, 0, 0, 36, 1, 1, 1, 36, 2, 2, 2};
    const int b3 = gRing.nVerts;
    pc_gfx_call_display_list(dl2.data(), (u32)dl2.size()); n3ds_gfx_flush();
    CHECK(gRing.nVerts - b3 == 3, "a list with a TEX0MTXIDX byte still draws (position matrix not overwritten)");
}

// 1n: clipping must not change what is visible. Reference: ray-cast the ORIGINAL eye-space triangle through each pixel centre
// (hit counts when it lies at or beyond the near plane); test: the emitted (clipped) triangles after the perspective divide.
static void testClipCoverage()
{
    setup();
    std::mt19937 rng(777);
    auto F = [&](float a, float b) { return a + (b - a) * (rng() % 100000) / 100000.0f; };
    const float cot = 1.0f / tanf(30.0f * 3.14159265f / 180.0f), aspect = 4.0f / 3.0f, p00 = cot / aspect, p11 = cot;
    long mism = 0, total = 0, covered = 0; int nTri = 0;
    for (int t = 0; t < 400; ++t) {
        P3 v[3]; bool anyFront = false;
        for (int k = 0; k < 3; ++k) { v[k] = {F(-400, 400), F(-300, 300), F(-300, 120)}; anyFront = anyFront || v[k].z < -NEAR_ * 1.5f; }
        if (!anyFront) continue;
        ++nTri;
        gRing.newFrame();
        const int n = draw(TRIANGLES, v, 3);
        for (int gy = 0; gy < 30; ++gy) for (int gx = 0; gx < 40; ++gx) {
            const float nx = -1.0f + 2.0f * (gx + 0.5f) / 40.0f, ny = -1.0f + 2.0f * (gy + 0.5f) / 30.0f;
            // reference: ray d = (nx/p00, ny/p11, -1) * s, Moller-Trumbore on the original triangle
            const float d[3] = {nx / p00, ny / p11, -1.0f};
            auto sub = [](const P3& a, const P3& b) { return P3{a.x - b.x, a.y - b.y, a.z - b.z}; };
            auto crs = [](const P3& a, const P3& b) { return P3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
            auto dot = [](const P3& a, const P3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
            const P3 D{d[0], d[1], d[2]}, e1 = sub(v[1], v[0]), e2 = sub(v[2], v[0]), pv = crs(D, e2);
            const float det = dot(e1, pv); bool ref = false;
            if (fabsf(det) > 1e-9f) { const float inv = 1.0f / det; const P3 tv = sub(P3{0,0,0}, v[0]); const float u = dot(tv, pv) * inv;
                const P3 qv = crs(tv, e1); const float w = dot(D, qv) * inv; const float s_ = dot(e2, qv) * inv;
                ref = u >= 0 && w >= 0 && u + w <= 1 && s_ >= NEAR_ && s_ <= FAR_; }
            // test: emitted triangles. o.x = cy, o.y = -cx (mock tilt) -> cx = -o.y, cy = o.x
            bool got = false;
            for (int i = 0; i + 2 < n && !got; i += 3) {
                float X[3], Y[3]; bool okw = true;
                for (int k = 0; k < 3; ++k) { const N3dsVtx& q = gRing.verts()[i + k]; if (q.w <= 0) okw = false; X[k] = -q.y / q.w; Y[k] = q.x / q.w; }
                if (!okw) continue;
                auto edge = [](float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); };
                const float e0 = edge(X[0], Y[0], X[1], Y[1], nx, ny), e1_ = edge(X[1], Y[1], X[2], Y[2], nx, ny), e2_ = edge(X[2], Y[2], X[0], Y[0], nx, ny);
                got = (e0 >= 0 && e1_ >= 0 && e2_ >= 0) || (e0 <= 0 && e1_ <= 0 && e2_ <= 0);
            }
            ++total; covered += ref; if (ref != got) ++mism;
        }
    }
    CHECK(nTri > 100 && covered > 2000, "clip coverage test has real coverage (%d tris, %ld covered samples)", nTri, covered);
    CHECK(mism * 200 <= total, "clipped output covers the same pixels as the original triangle (%ld of %ld samples differ)", mism, total);
    printf("  clip coverage: %ld/%ld samples differ, %ld covered\n", mism, total, covered);
}

static void testTexturedDraw()
{
    setup();
    static uint8_t raw[16 * 16 * 4]; memset(raw, 200, sizeof raw);
    GXTexObj obj; pc_gfx_init_tex_obj_rgba(&obj, raw, 16, 16, 1, 0);
    pc_gfx_load_tex_obj(&obj, 0);
    pc_gfx_set_num_tev_stages(1); pc_gfx_set_tev_order(0, 0, 0, 4);
    pc_gfx_set_tev_color_in(0, 15, 8, 10, 15); pc_gfx_set_tev_alpha_in(0, 7, 4, 5, 7);
    P3 tri[3] = {{-100,-100,-500},{100,-100,-500},{0,100,-500}};
    CHECK(draw(TRIANGLES, tri, 3) == 3, "textured triangle drawn");
    CHECK(gRing.lastState.texId != 0, "texture resolved to a gpu id");
    CHECK(gRing.lastState.nTev == 1 && gRing.lastState.tev[0].rgbFn == N3DS_TFN_MODULATE, "modulate program reached the draw state");
    CHECK(gRing.lastState.texWrapS == 1 && gRing.lastState.texWrapT == 0, "wrap modes reach the draw state");
    // untextured again: unbind the map
    pc_gfx_set_tev_order(0, 0, 0xFF, 4);
    pc_gfx_set_tev_color_in(0, 15, 15, 15, 10); pc_gfx_set_tev_alpha_in(0, 7, 7, 7, 5);
    CHECK(draw(TRIANGLES, tri, 3) == 3 && gRing.lastState.texId == 0, "no texture map -> untextured draw");
}

int main()
{
    testAssembly(); testCullCases(); testScissor(); testProperty(); testUiReserve(); testTexConv(); testConfigAndScale(); testVertexCache(); testVertexCacheDirectMtx(); testTev(); testTevMultiUnit(); testTevLenient(); testTexgen(); testDlCull(); testTexturedDraw(); testClipCoverage();
    printf("%d checks, %d failed\n", gChecks, gFail);
    return gFail ? 1 : 0;
}
