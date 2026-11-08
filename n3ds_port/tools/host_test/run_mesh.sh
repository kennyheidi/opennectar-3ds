#!/bin/sh
# 1p mesh-path test: needs g++ and picasso (devkitPro tools/bin). Run from n3ds_port/tools/host_test.
set -e
P=../../platform; O=${TMPDIR:-/tmp}/nectar_mesh_test; mkdir -p $O
picasso -o $O/passthrough.shbin $P/shader/passthrough.v.pica
L=$(grep -n "two-thread split" $P/n3ds_gfx.cpp | head -1 | cut -d: -f1)
head -n $((L-1)) $P/n3ds_gfx.cpp | sed -e 's/\brp_\([a-z_0-9]*\)/pc_gfx_\1/g' -e '/n3ds_gfxq_start(/d' > $O/gfx_copy.cpp
echo "}" >> $O/gfx_copy.cpp; echo 'extern "C" int n3ds_gfxq_running(void) { return 0; } extern "C" unsigned n3ds_gfxq_busy_ticks(void) { return 0; }' >> $O/gfx_copy.cpp
echo 'namespace { void invPost(uintptr_t, uintptr_t); } void n3ds_test_mesh_inval(const void* p, size_t n) { invPost((uintptr_t)p, (uintptr_t)p + n); }' >> $O/gfx_copy.cpp
echo 'void n3ds_test_lighting(int m) { sLightMode = m; }
int n3ds_test_light_vertex(float nz, float amb, float out[3]) { G.ch[0] = Gfx::ChanCtl{true, false, false, 1, 2, 1}; G.amb[0] = G.amb[1] = G.amb[2] = amb;
  float v[16] = {0, 0, 10, 0, 0, -1, 1, 0, 0, 1, 0, 0, 1, 0.5f, 0.25f, 1}; lightLoad(v, 1); for (int i = 0; i < 12; ++i) G.pos[0][i] = (i % 5 == 0) ? 1.0f : 0.0f; G.nrmSet[0] = false;
  Vin vin; memset(&vin, 0, sizeof vin); vin.nz = nz; vin.nx = 1.0f - fabsf(nz); vin.hasN = true; float r = 200, g = 200, b = 200, a = 255; gxLightVertex(vin, r, g, b, a); out[0] = r; out[1] = g; out[2] = b; return 1; }' >> $O/gfx_copy.cpp
cat >> $O/gfx_copy.cpp <<'HOOK'
#include <chrono>
#include <vector>
/* r50: the fast lighting against the reference, on random lights / channels / vertices; plus a host timing of both */
double n3ds_test_light_compare(int iters, double* tRef, double* tFast)
{
    uint32_t rs = 987654321u; auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (rs >> 8) / 16777216.0f; };
    auto pick = [&](int n) { return (int)(rnd() * n) % n; };
    double worst = 0;
    for (int it = 0; it < iters; ++it) {
        for (int k = 0; k < 2; ++k) { Gfx::ChanCtl& c = G.ch[k]; c.en = rnd() < 0.85f; c.ambVtx = rnd() < 0.3f; c.matVtx = false;
            c.mask = (uint8_t)(rnd() < 0.2f ? 0 : (int)(rnd() * 256)); c.diff = (uint8_t)pick(3); c.attn = (uint8_t)pick(3); }
        for (int i = 0; i < 8; ++i) { Gfx::Light& L = G.light[i];
            const float far = rnd() < 0.2f ? 1048576.0f : 1000.0f;
            for (int j = 0; j < 3; ++j) L.pos[j] = (rnd() * 2 - 1) * far;
            float dx = rnd() * 2 - 1, dy = rnd() * 2 - 1, dz = rnd() * 2 - 1, m = sqrtf(dx * dx + dy * dy + dz * dz) + 1e-6f; L.dir[0] = dx / m; L.dir[1] = dy / m; L.dir[2] = dz / m;
            const int am = pick(3); if (am == 0) { L.a[0] = 1; L.a[1] = 0; L.a[2] = 0; } else if (am == 1) { L.a[0] = -10.6f; L.a[1] = 11.6f; L.a[2] = 0; } else for (int j = 0; j < 3; ++j) L.a[j] = rnd() * 4 - 2;
            const int km = pick(4); if (km == 0) { L.k[0] = 1; L.k[1] = 0; L.k[2] = 0; } else if (km == 1) { L.k[0] = 0; L.k[1] = 0; L.k[2] = 0; } else if (km == 2) { L.k[0] = 0.5f + rnd(); L.k[1] = 0; L.k[2] = 0; } else for (int j = 0; j < 3; ++j) L.k[j] = rnd() * 0.01f;
            for (int j = 0; j < 4; ++j) L.col[j] = rnd(); }
        for (int j = 0; j < 4; ++j) G.amb[j] = rnd() * 0.5f;
        ++sLitGen;
        for (int m = 0; m < 4; ++m) { for (int i = 0; i < 12; ++i) G.pos[m][i] = rnd() * 2 - 1; G.pos[m][3] *= 500; G.pos[m][7] *= 500; G.pos[m][11] = -300 - 500 * rnd();
            G.nrmSet[m] = rnd() < 0.5f; for (int i = 0; i < 12; ++i) G.nrm[m][i] = rnd() * 2 - 1; }
        for (int vi = 0; vi < 64; ++vi) {
            Vin v; memset(&v, 0, sizeof v); v.mtx = (uint8_t)pick(4); v.x = rnd() * 200 - 100; v.y = rnd() * 200 - 100; v.z = rnd() * 200 - 100;
            v.hasN = rnd() < 0.85f; v.nx = rnd() * 2 - 1; v.ny = rnd() * 2 - 1; v.nz = rnd() * 2 - 1; v.hasC = rnd() < 0.5f; v.r = rnd() * 255; v.g = rnd() * 255; v.b = rnd() * 255; v.a = rnd() * 255;
            float r0 = 200, g0 = 150, b0 = 100, a0 = 255, r1 = r0, g1 = g0, b1 = b0, a1 = a0;
            gxLightVertexRef(v, r0, g0, b0, a0); gxLightVertex(v, r1, g1, b1, a1);
            const double e = fmax(fmax(fabs(r0 - r1), fabs(g0 - g1)), fmax(fabs(b0 - b1), fabs(a0 - a1)));
            if (e > worst) worst = e;
        }
    }
    /* timing: the common Pikmin setup (colour channel: light 0, spot, clamped diffuse; alpha channel: light 1, spot) */
    G.ch[0] = Gfx::ChanCtl{true, false, false, 1, 2, 1}; G.ch[1] = Gfx::ChanCtl{true, false, false, 2, 2, 1};
    float l0[16] = {110, 329, -461, 0.2f, 0.64f, 0.74f, -10.6f, 11.6f, 0, 1, 0, 0, 1, 0.9f, 1, 1}; lightLoad(l0, 1);
    float l1[16] = {-270, -749, -489, 0, 0.86f, 0.51f, -2.41f, 3.41f, 0, 1, 0, 0, 1, 0, 0, 1}; lightLoad(l1, 2);
    std::vector<Vin> vs(4096); for (Vin& v : vs) { memset(&v, 0, sizeof v); v.x = rnd() * 200; v.y = rnd() * 200; v.z = rnd() * 200; v.hasN = true; v.nx = rnd(); v.ny = rnd(); v.nz = rnd(); }
    float sink = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 200; ++rep) for (const Vin& v : vs) { float r = 200, g = 200, b = 200, a = 255; gxLightVertexRef(v, r, g, b, a); sink += r + a; }
    auto t1 = std::chrono::steady_clock::now();
    for (int rep = 0; rep < 200; ++rep) for (const Vin& v : vs) { float r = 200, g = 200, b = 200, a = 255; gxLightVertex(v, r, g, b, a); sink += r + a; }
    auto t2 = std::chrono::steady_clock::now();
    *tRef = std::chrono::duration<double, std::nano>(t1 - t0).count() / (200.0 * vs.size());
    *tFast = std::chrono::duration<double, std::nano>(t2 - t1).count() / (200.0 * vs.size()) + (sink == 12345.0f ? 1 : 0);
    return worst;
}
HOOK
cat >> $O/gfx_copy.cpp <<'HOOK2'
/* r53: light setups for the GPU-lighting comparison (test_mesh.cpp). Channel ids: 0 COLOR0, 2 ALPHA0, 4 both; diffuse 0 none 1 sign 2 clamp;
   attenuation 0 spec 1 spot 2 none. lightLoad(v, mask): v = pos 3, dir 3, angle attn 3, distance attn 3, colour rgba (0..1). */
void n3ds_test_light_setup(int v)
{
    auto L = [](int i, float px, float py, float pz, float dx, float dy, float dz, float a0, float a1, float a2, float k0, float k1, float k2, float r, float g, float b, float a) {
        const float t[16] = {px, py, pz, dx, dy, dz, a0, a1, a2, k0, k1, k2, r, g, b, a}; lightLoad(t, 1u << i); };
    auto amb = [](float r, float g, float b, float a) { GXColor c = {(u8)(r * 255), (u8)(g * 255), (u8)(b * 255), (u8)(a * 255)}; chanAmbSet(4, c); };
    switch (v) {
    case 1:
        pc_gfx_set_chan_ctrl((GXChannelID)0, 1, (GXColorSrc)0, (GXColorSrc)1, 1, (GXDiffuseFn)2, (GXAttnFn)1);
        pc_gfx_set_chan_ctrl((GXChannelID)2, 1, (GXColorSrc)0, (GXColorSrc)1, 2, (GXDiffuseFn)2, (GXAttnFn)1);
        L(0, 100, 300, -400, 0.2f, 0.64f, 0.74f, -10.6f, 11.6f, 0, 1, 0, 0, 1, 0.9f, 1, 1);
        L(1, -270, -749, -489, 0, 0.86f, 0.51f, -2.41f, 3.41f, 0, 1, 0, 0, 1, 0, 0, 1);
        amb(0.24f, 0.24f, 0.27f, 0.19f); break;
    case 2:
        pc_gfx_set_chan_ctrl((GXChannelID)0, 1, (GXColorSrc)0, (GXColorSrc)1, 3, (GXDiffuseFn)1, (GXAttnFn)2);
        pc_gfx_set_chan_ctrl((GXChannelID)2, 0, (GXColorSrc)0, (GXColorSrc)1, 0, (GXDiffuseFn)0, (GXAttnFn)2);
        L(0, 1e5f, 2e5f, -1e5f, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0.6f, 0.5f, 0.4f, 1);
        L(1, 0, -500, -300, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0.3f, 0.4f, 0.5f, 1);
        amb(0.1f, 0.15f, 0.2f, 1); break;
    case 3: {
        pc_gfx_set_chan_ctrl((GXChannelID)0, 1, (GXColorSrc)0, (GXColorSrc)1, 1, (GXDiffuseFn)2, (GXAttnFn)0);
        pc_gfx_set_chan_ctrl((GXChannelID)2, 1, (GXColorSrc)0, (GXColorSrc)1, 0, (GXDiffuseFn)0, (GXAttnFn)2);
        const float n[3] = {0.3f, -0.5f, 0.8f}, hn = sqrtf(0.01f + 0.04f + 0.9409f);
        L(0, -n[0] * 1048576.f, -n[1] * 1048576.f, -n[2] * 1048576.f, 0.1f / hn, 0.2f / hn, 0.97f / hn, 0, 0, 1, 0.5f, 0, 0.5f, 1, 1, 0.8f, 1);
        amb(0.2f, 0.2f, 0.2f, 0.5f); break; }
    case 4:
        pc_gfx_set_chan_ctrl((GXChannelID)4, 1, (GXColorSrc)0, (GXColorSrc)1, 1, (GXDiffuseFn)2, (GXAttnFn)1);
        L(0, 50, 200, -600, 0, 0.7071f, 0.7071f, 1, 0, 0, 0.2f, 0.002f, 0.00001f, 0.9f, 0.8f, 0.7f, 0.6f);
        amb(0.1f, 0.1f, 0.1f, 0.2f); break;
    case 5:
        pc_gfx_set_chan_ctrl((GXChannelID)0, 1, (GXColorSrc)0, (GXColorSrc)1, 7, (GXDiffuseFn)2, (GXAttnFn)2);
        L(0, 100, 300, -400, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0.5f, 0.5f, 0.5f, 1);
        L(1, -100, 300, -400, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0.3f, 0.3f, 0.3f, 1);
        L(2, 0, -300, -400, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0.2f, 0.2f, 0.2f, 1);
        amb(0.1f, 0.1f, 0.1f, 1); break;
    case 6:
        pc_gfx_set_chan_ctrl((GXChannelID)0, 1, (GXColorSrc)0, (GXColorSrc)1, 1, (GXDiffuseFn)2, (GXAttnFn)1);
        pc_gfx_set_chan_ctrl((GXChannelID)2, 0, (GXColorSrc)0, (GXColorSrc)1, 0, (GXDiffuseFn)0, (GXAttnFn)2);
        L(0, 0, 30, -200, 0, 0, 1, 1, 0, 0, 0, 0, 0.0001f, 1, 1, 1, 1);
        amb(0.05f, 0.05f, 0.05f, 1); break;
    }
}
HOOK2
echo 'unsigned n3ds_test_whole_gpu(void) { return sWholeGpuTot; }' >> $O/gfx_copy.cpp
cat >> $O/gfx_copy.cpp <<'HOOK3'
/* r54b: the interval classifier never contradicts the exact 8-corner one: fast 0 => exact 0, fast 1 => exact 0 or 1 (never 2) */
void n3ds_test_classify_fast(int iters, unsigned* decided, unsigned* bad, unsigned* ones, unsigned* zeros)
{
    uint32_t rs = 24681357u; auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return (rs >> 8) / 16777216.0f; };
    *decided = *bad = *ones = *zeros = 0;
    if (!tiltOk) tiltOk = n3ds_gpu_tilt(tilt) != 0;
    computeCullBounds();
    for (int it = 0; it < iters; ++it) {
        const int nb = 1 + (int)(rnd() * 4);
        float Ms[4][16]; const float* Cs[4]; float mn[4][3], mx[4][3];
        for (int b = 0; b < nb; ++b) {
            float P[12]; const float a = rnd() * 6.2832f, sc = 0.2f + rnd() * 3.0f;
            P[0] = cosf(a) * sc; P[1] = 0; P[2] = sinf(a) * sc; P[3] = (rnd() * 2 - 1) * 400;
            P[4] = 0; P[5] = sc; P[6] = 0; P[7] = (rnd() * 2 - 1) * 300;
            P[8] = -sinf(a) * sc; P[9] = 0; P[10] = cosf(a) * sc; P[11] = -rnd() * 2500 + (rnd() < 0.15f ? 200.0f : 0.0f);
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
                const float m0 = P[c], m1 = P[4 + c], m2 = P[8 + c], m3 = c == 3 ? 1.0f : 0.0f;
                Ms[b][r * 4 + c] = G.proj[r * 4 + 0] * m0 + G.proj[r * 4 + 1] * m1 + G.proj[r * 4 + 2] * m2 + G.proj[r * 4 + 3] * m3;
            }
            Cs[b] = Ms[b];
            for (int k = 0; k < 3; ++k) { const float c0 = (rnd() * 2 - 1) * 50, e = rnd() < 0.5f ? rnd() * 5 : rnd() * 150; mn[b][k] = c0 - e; mx[b][k] = c0 + e; }
        }
        const int f = meshClassifyFast(Cs, mn, mx, nb), e = meshClassify(Cs, mn, mx, nb);
        if (f >= 0) ++*decided;
        if (f == 1) ++*ones;
        if (f == 0) ++*zeros;
        if ((f == 0 && e != 0) || (f == 1 && e == 2)) ++*bad;
    }
}
HOOK3
echo 'unsigned n3ds_test_mesh_built(void) { return MS.built; } unsigned n3ds_test_arena_used(void) { unsigned n = 0; for (uint32_t i = 0; i < sArenaPages; ++i) if (sArenaRun[i]) ++n; return n; }' >> $O/gfx_copy.cpp
echo 'unsigned n3ds_test_cpu_lists(void) { return CS.lists; } unsigned n3ds_test_cpu_checks(void) { return sCpuChecksOk; } unsigned n3ds_test_cpu_recoveries(void) { return sCpuRecTotal; } unsigned n3ds_test_cpu_bad(void) { return sCpuBadTotal; } void n3ds_test_cpu_reset(void) { sCpuRecTotal = 0; sCpuOff = false; sCpuBadTotal = 0; memset(&CS, 0, sizeof CS); } unsigned n3ds_test_cpu_volatile(void) { return MStot.volatileLists + MS.volatileLists; } void n3ds_test_cpu_corrupt(void) { for (int i = 0; i < MC_SLOTS; ++i) if (sMC[i].state == 1 && sMC[i].cpu) for (uint32_t k = 0; k < sMC[i].nv; ++k) sMC[i].vtx[k].x += 7.0f; } void n3ds_test_cpu_recheck(void) { for (int i = 0; i < MC_SLOTS; ++i) sMC[i].checked = 0; } void n3ds_test_phase(int on) { sPhaseOn = on != 0; } unsigned n3ds_test_phase_chunks(void) { return sPhChunks; } void n3ds_test_cpu_clearbad(void) { for (int i = 0; i < MC_SLOTS; ++i) sMC[i].cpuBad = 0; sCpuOff = false; sCpuBadTotal = 0; }' >> $O/gfx_copy.cpp
echo '#include <vector>
unsigned n3ds_test_arena_stress(void) { std::vector<std::pair<uint8_t*, uint32_t> > live; unsigned bad = 0, fails = 0; uint32_t rs = 12345; auto rnd = [&]() { rs = rs * 1664525u + 1013904223u; return rs >> 8; };
  for (int it = 0; it < 30000; ++it) { if (live.size() > 30 || (rnd() % 3 == 0 && !live.empty())) { size_t k = rnd() % live.size(); arenaFree(live[k].first); live.erase(live.begin() + k); }
    else { const uint32_t b = (rnd() % 2) ? 4096u * (1 + rnd() % 10) : 4096u * (24 + rnd() % 120); uint8_t* p = (uint8_t*)arenaAlloc(b); if (!p) { ++fails; continue; }
      if (p < sArena || p + b > sArena + (size_t)sArenaPages * 4096u) ++bad; for (auto& q : live) if (p < q.first + q.second && q.first < p + b) ++bad; live.push_back(std::make_pair(p, (b + 4095u) / 4096u * 4096u)); } }
  for (auto& q : live) arenaFree(q.first); live.clear();
  return bad * 1000000u + fails; }' >> $O/gfx_copy.cpp
g++ -std=c++17 -O1 -I mock -I . -I $P -I ../../../pc_port -o $O/test test_mesh.cpp mock_bridge.cpp $O/gfx_copy.cpp $P/n3ds_tex.cpp $P/n3ds_texconv.cpp $P/n3ds_config.cpp $P/n3ds_tev.cpp
$O/test $O/passthrough.shbin
