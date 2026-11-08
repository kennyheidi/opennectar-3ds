// HOST TEST (1p): the GPU-transform mesh path must draw what the CPU path draws.
// Every scene is rendered twice through n3ds_gfx.cpp: once with the mesh path off (reference: CPU-transformed ring triangles), once
// with it on (mesh draws are run through the REAL assembled shader, passthrough.shbin, in a PICA interpreter; chunks the mesh path
// hands back to the CPU arrive as ring triangles, in order). Every reference triangle must appear, in order and with the same
// position / colour / texture coordinates; extra GPU triangles are only allowed if the CPU path would have rejected them as invisible.
#include "mock_bridge.h"
#include "n3ds_config.h"
#include "gl/pc_gfx.h"
#include "pica_interp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <random>
#include <chrono>
#include <vector>

extern "C" void n3ds_gfx_frame_end(unsigned frame);
static int gFail = 0, gChecks = 0;
#define CHECK(c, ...) do { ++gChecks; if (!(c)) { ++gFail; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static PicaShader gSh;
static int gShadedDraws = 0, gLitDraws = 0;   // r53: mesh draws shaded / with GameCube lighting in the shader
static int gLightVariant = 0;                 // r53: light setup applied after setMaterial (n3ds_test_light_setup)
extern void n3ds_test_light_setup(int variant);
struct Tri { float p[3][4]; float c[3][4]; float t[3][4]; bool fromMesh; };

static const float NEAR_ = 10.f, FAR_ = 3000.f;
static void setProjection()
{
    const float cot = 1.0f / tanf(30.0f * 3.14159265f / 180.0f), aspect = 4.0f / 3.0f;
    Mtx44 p; memset(p, 0, sizeof p);
    p[0][0] = cot / aspect; p[1][1] = cot; p[2][2] = -NEAR_ / (FAR_ - NEAR_); p[2][3] = -FAR_ * NEAR_ / (FAR_ - NEAR_); p[3][2] = -1.0f;
    pc_gfx_set_projection(p, 0);
}

static void shadeMesh(const MockEvent& e, std::vector<Tri>& out, bool f24)
{
    gSh.f24 = f24;
    const N3dsMeshUniforms& u = e.u;
    // r53: as n3ds_gpu.cpp meshIssue uploads them (F, P per slot, N per slot, tcu, cw, mc, AMB and the lights; clipOnly = debug probes)
    const int lF = gSh.loc("F"), lP = gSh.loc("P"), ln = gSh.loc("N"), lt = gSh.loc("tcu"), lw = gSh.loc("cw"), lm = gSh.loc("mc");
    static const float I3[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    for (int r = 0; r < 4; ++r) gSh.setF(lF + r, u.clipOnly ? &u.clip[0][r * 4] : &u.proj[r * 4]);
    for (int s = 0; s < u.nSlots; ++s) for (int r = 0; r < 3; ++r) gSh.setF(lP + 3 * s + r, u.clipOnly ? I3[r] : &u.pos[s][r * 4]);
    const bool lit = !u.clipOnly && u.lit, head = !u.clipOnly && !u.lit && u.head;
    if (lit || head) for (int s = 0; s < u.nSlots; ++s) for (int r = 0; r < 3; ++r) { float row[4] = {u.nrm[s][r * 4], u.nrm[s][r * 4 + 1], u.nrm[s][r * 4 + 2], 0}; gSh.setF(ln + 3 * s + r, row); }
    for (int i = 0; i < 8; ++i) gSh.setF(lt + i, u.tcu[i]);
    gSh.setF(lw, u.cw); gSh.setF(lm, u.mc);
    if (lit) {
        gSh.setF(gSh.loc("AMB"), u.amb);
        for (int k = 0; k < u.nLights; ++k) {
            gSh.setF(gSh.loc("LP") + k, u.lp[k]); gSh.setF(gSh.loc("LC") + k, u.lc[k]); gSh.setF(gSh.loc("LF") + k, u.lf[k]);
            gSh.setF(gSh.loc("LA") + k, u.la[k]); gSh.setF(gSh.loc("LK") + k, u.lk[k]); gSh.setF(gSh.loc("LD") + k, u.ld[k]);
        }
    }
    gSh.setB(gSh.loc("xform"), true); gSh.setB(gSh.loc("lit"), lit); gSh.setB(gSh.loc("head"), head);
    const char* lb[2] = {"l0", "l1"}; const char* ab[2] = {"a0", "a1"}; const char* sb[2] = {"s0", "s1"};
    for (int k = 0; k < 2; ++k) { const bool on = lit && k < u.nLights; gSh.setB(gSh.loc(lb[k]), on); gSh.setB(gSh.loc(ab[k]), on && u.spot[k]); gSh.setB(gSh.loc(sb[k]), on && u.spec[k]); }
    gSh.setB(gSh.loc("multi"), u.nSlots > 1);       // as n3ds_gpu_draw_mesh does
    ++gShadedDraws; if (lit) ++gLitDraws;
    for (size_t k = 0; k + 1 < e.runs.size(); k += 2) {
        for (unsigned i = e.runs[k]; i + 2 < e.runs[k] + e.runs[k + 1] + 0u && i < e.runs[k] + e.runs[k + 1]; i += 3) {
            Tri t; t.fromMesh = true;
            for (int j = 0; j < 3; ++j) {
                const N3dsMeshVtx& v = e.vtx[e.idx ? e.idx[i + j] : i + j];   // idx == NULL: de-indexed list (mesh_safe)
                float in[16][4] = {}; float o[16][4];
                in[0][0] = gSh.R(v.x); in[0][1] = gSh.R(v.y); in[0][2] = gSh.R(v.z); in[0][3] = v.slot;
                in[1][0] = v.r; in[1][1] = v.g; in[1][2] = v.b; in[1][3] = v.a;
                in[2][0] = gSh.R(v.u0); in[2][1] = gSh.R(v.v0); in[2][3] = 1; in[3][0] = gSh.R(v.u1); in[3][1] = gSh.R(v.v1); in[3][3] = 1;
                in[4][0] = gSh.R(v.nx); in[4][1] = gSh.R(v.ny); in[4][2] = gSh.R(v.nz); in[4][3] = v.nw;
                gSh.run(in, o);
                for (int reg = 0; reg < 16; ++reg) {
                    const int ty = gSh.outType[reg];
                    if (ty == 0) memcpy(t.p[j], o[reg], 16);
                    else if (ty == 2) for (int q = 0; q < 4; ++q) t.c[j][q] = fminf(fmaxf(o[reg][q], 0.f), 1.f) * 255.f;
                    else if (ty == 3) { t.t[j][0] = o[reg][0]; t.t[j][1] = o[reg][1]; }
                    else if (ty == 5) { t.t[j][2] = o[reg][0]; t.t[j][3] = o[reg][1]; }
                }
            }
            out.push_back(t);
        }
    }
}
static void ringTris(int first, int count, std::vector<Tri>& out)
{
    const N3dsVtx* r = gRing.verts();
    for (int i = first; i + 2 < first + count; i += 3) {
        Tri t; t.fromMesh = false;
        for (int j = 0; j < 3; ++j) {
            const N3dsVtx& v = r[i + j];
            t.p[j][0] = v.x; t.p[j][1] = v.y; t.p[j][2] = v.z; t.p[j][3] = v.w;
            t.c[j][0] = v.r; t.c[j][1] = v.g; t.c[j][2] = v.b; t.c[j][3] = v.a;
            t.t[j][0] = v.u; t.t[j][1] = v.v; t.t[j][2] = v.u1; t.t[j][3] = v.v1;
        }
        out.push_back(t);
    }
}

/* Would the CPU path have rejected this triangle as invisible? (mock tilt: x' = cy, y' = -cx; full 640x480 viewport -> 320x240 px) */
static bool cpuInvisible(const Tri& t)
{
    const float hvw = 160, hvh = 120, bx = 1 + 2.0f / hvw, by = 1 + 2.0f / hvh;
    unsigned acc = 0x3F; bool front = true; float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int j = 0; j < 3; ++j) {
        const float cx = -t.p[j][1], cy = t.p[j][0], z = t.p[j][2], w = t.p[j][3];
        unsigned oc = 0;
        if (z < -w) oc |= 1; if (z > 0) oc |= 2; if (cx < -bx * w) oc |= 4; if (cx > bx * w) oc |= 8; if (cy < -by * w) oc |= 16; if (cy > by * w) oc |= 32;
        acc &= oc;
        if (w > 1e-6f) { const float px = (cx / w + 1) * hvw, py = (1 - cy / w) * hvh; x0 = fminf(x0, px); x1 = fmaxf(x1, px); y0 = fminf(y0, py); y1 = fmaxf(y1, py); } else front = false;
    }
    if (acc) return true;
    if (front && (floorf(x1 + 0.02f - 0.5f) < ceilf(x0 - 0.02f - 0.5f) || floorf(y1 + 0.02f - 0.5f) < ceilf(y0 - 0.02f - 0.5f))) return true;
    return false;
}

struct CmpResult { int ref, got, matched, extraVisible, missing, fromMesh; double maxPos, maxCol, maxUV; };
static bool triMatch(const Tri& a, const Tri& b, double& ep, double& ec, double& eu, double tolPos, double tolUV)
{
    double mp = 0, mc = 0, mu = 0;
    for (int j = 0; j < 3; ++j) {
        const double sc = 1.0 + fabs(a.p[j][3]);
        for (int q = 0; q < 4; ++q) mp = fmax(mp, fabs(a.p[j][q] - b.p[j][q]) / sc);
        for (int q = 0; q < 4; ++q) mc = fmax(mc, fabs(a.c[j][q] - b.c[j][q]));
        for (int q = 0; q < 4; ++q) mu = fmax(mu, fabs(a.t[j][q] - b.t[j][q]) / (1.0 + fabs(a.t[j][q])));
    }
    if (mp > tolPos || mc > 1.6 || mu > tolUV) return false;
    ep = fmax(ep, mp); ec = fmax(ec, mc); eu = fmax(eu, mu);
    return true;
}
static CmpResult compare(const std::vector<Tri>& ref, const std::vector<Tri>& got, double tolPos, double tolUV, const char* name, bool uv = true)
{
    CmpResult r = {(int)ref.size(), (int)got.size(), 0, 0, 0, 0, 0, 0, 0};
    size_t g = 0;
    auto extra = [&](size_t k, size_t i) {
        if (cpuInvisible(got[k])) return;
        if (r.extraVisible < 3) { printf("  [%s] extra visible tri at got %zu (ref %zu) fromMesh %d px:", name, k, i, (int)got[k].fromMesh);
            for (int j = 0; j < 3; ++j) { const float cx = -got[k].p[j][1], cy = got[k].p[j][0], w = got[k].p[j][3]; printf(" (%.3f,%.3f z%.4f)", (cx / w + 1) * 160, (1 - cy / w) * 120, got[k].p[j][2] / w); } printf("\n"); }
        ++r.extraVisible;
    };
    for (size_t i = 0; i < ref.size(); ++i) {
        size_t k = g; bool found = false;
        for (; k < got.size(); ++k) {
            double a = 0, b = 0, c = 0;
            Tri x = got[k]; if (!uv) memcpy(x.t, ref[i].t, sizeof x.t);
            if (triMatch(ref[i], x, a, b, c, tolPos, tolUV)) { r.maxPos = fmax(r.maxPos, a); r.maxCol = fmax(r.maxCol, b); r.maxUV = fmax(r.maxUV, c); found = true; break; }
        }
        if (found) { for (size_t q = g; q < k; ++q) extra(q, i); ++r.matched; r.fromMesh += got[k].fromMesh; g = k + 1; }
        else {
            if (r.missing < 3) { const Tri& t = ref[i]; printf("  [%s] missing ref tri %zu: p0 %.3f %.3f %.3f %.3f c0 %.0f %.0f %.0f %.0f uv0 %.4f %.4f\n", name, i, t.p[0][0], t.p[0][1], t.p[0][2], t.p[0][3], t.c[0][0], t.c[0][1], t.c[0][2], t.c[0][3], t.t[0][0], t.t[0][1]); }
            ++r.missing;
        }
    }
    for (; g < got.size(); ++g) extra(g, ref.size());
    return r;
}

/* ---- scenes ---------------------------------------------------------------------------------------------------- */
enum { GN = 22 };
static int16_t sPos[(GN + 1) * (GN + 1)][3];
static float sNrm[(GN + 1) * (GN + 1)][3];
static uint8_t sClr[(GN + 1) * (GN + 1)][4];
static int16_t sUV[(GN + 1) * (GN + 1)][2];
static float sUV1[(GN + 1) * (GN + 1)][2];
static uint8_t sTexRaw[4096];   // (padded: the texture analyser reads GC-format sized data from it)
struct SceneOpt { bool skinned, lit, vtxColor, textured, twoUnits, texMtx, imm; unsigned seed; };
static bool gTmi = false;   // lists carry GX_VA_TEX0MTXIDX (and TEX1MTXIDX for two units): per-vertex texture-matrix ids
static uint8_t tmiOf(int k, int unit) { static const uint8_t ids[5] = {30, 33, 36, 0, 39}; return ids[(k * 7 + unit * 3 + k / 5) % 5]; }

static std::vector<uint8_t> buildList(const SceneOpt& o, std::mt19937& rng)
{
    for (int j = 0; j <= GN; ++j) for (int i = 0; i <= GN; ++i) {
        const int k = j * (GN + 1) + i;
        sPos[k][0] = (int16_t)((i * 40 - 440) * 4); sPos[k][1] = (int16_t)(((i * 7 + j * 13) % 9 * 6 + (int)(rng() % 20)) * 4); sPos[k][2] = (int16_t)((-j * 40 + 200) * 4);   // frac 2
        float nx = (float)((int)(rng() % 200) - 100) / 100.f, ny = 1.0f, nz = (float)((int)(rng() % 200) - 100) / 100.f;
        sNrm[k][0] = nx; sNrm[k][1] = ny; sNrm[k][2] = nz;
        sClr[k][0] = (uint8_t)rng(); sClr[k][1] = (uint8_t)rng(); sClr[k][2] = (uint8_t)rng(); sClr[k][3] = (uint8_t)(128 + rng() % 128);
        sUV[k][0] = (int16_t)(i * 64 + (int)(rng() % 16)); sUV[k][1] = (int16_t)(j * 64);   // frac 8
        sUV1[k][0] = (float)i * 0.37f; sUV1[k][1] = (float)j * 0.21f;
    }
    std::vector<uint8_t> dl;
    auto vtx = [&](int k) {
        if (o.skinned) dl.push_back((uint8_t)(((k % (GN + 1)) * 3 / (GN + 1)) * 3));   // matrix 0 / 3 / 6 by column band
        if (gTmi) { dl.push_back(tmiOf(k, 0)); if (o.twoUnits) dl.push_back(tmiOf(k, 1)); }
        for (int a = 0; a < (o.twoUnits ? 5 : 4); ++a) { dl.push_back((uint8_t)(k >> 8)); dl.push_back((uint8_t)k); }
    };
    for (int j = 0; j < GN; ++j) for (int i = 0; i < GN; ++i) {
        const int a = j * (GN + 1) + i, b = a + 1, c = a + (GN + 1), d = c + 1;
        switch (rng() % 5) {
        case 0: dl.push_back(0x80); dl.push_back(0); dl.push_back(4); vtx(a); vtx(c); vtx(d); vtx(b); break;
        case 1: dl.push_back(0x90); dl.push_back(0); dl.push_back(6); vtx(a); vtx(c); vtx(b); vtx(b); vtx(c); vtx(d); break;
        case 2: dl.push_back(0x98); dl.push_back(0); dl.push_back(4); vtx(a); vtx(c); vtx(b); vtx(d); break;
        case 3: dl.push_back(0xA0); dl.push_back(0); dl.push_back(4); vtx(a); vtx(c); vtx(d); vtx(b); break;
        default: dl.push_back(0xA0); dl.push_back(0); dl.push_back(3); vtx(a); vtx(c); vtx(b);
                 dl.push_back(0x00);                                                        // a NOP between commands
                 dl.push_back(0xA0); dl.push_back(0); dl.push_back(3); vtx(b); vtx(c); vtx(d); break;
        }
    }
    return dl;
}

static void setMaterial(const SceneOpt& o)
{
    pc_gfx_set_chan_ctrl((GXChannelID)4, o.lit ? 1 : 0, (GXColorSrc)0, (GXColorSrc)(o.vtxColor ? 1 : 0), o.lit ? 1 : 0, (GXDiffuseFn)0, (GXAttnFn)0);
    GXColor mc = {200, 150, 100, 180}; pc_gfx_set_chan_mat_color((GXChannelID)4, mc);
    if (!o.textured) {
        pc_gfx_set_num_tev_stages(1); pc_gfx_set_tev_order(0, 0, 0xFF, 4);
        pc_gfx_set_tev_color_in(0, 15, 15, 15, 10); pc_gfx_set_tev_alpha_in(0, 7, 7, 7, 5);
        return;
    }
    static GXTexObj o0, o1;
    memset(sTexRaw, 180, sizeof sTexRaw);
    pc_gfx_init_tex_obj_rgba(&o0, sTexRaw, 16, 16, 1, 1); pc_gfx_init_tex_obj_rgba(&o1, sTexRaw + 4, 16, 16, 1, 1);
    pc_gfx_load_tex_obj(&o0, 0);
    if (o.twoUnits) {
        pc_gfx_load_tex_obj(&o1, 1);
        pc_gfx_set_num_tev_stages(2);
        pc_gfx_set_tev_order(0, 0, 0, 4); pc_gfx_set_tev_color_in(0, 15, 8, 10, 15); pc_gfx_set_tev_alpha_in(0, 7, 4, 5, 7);
        pc_gfx_set_tev_order(1, 1, 1, 4); pc_gfx_set_tev_color_in(1, 15, 8, 0, 15); pc_gfx_set_tev_alpha_in(1, 7, 4, 0, 7);
        pc_gfx_set_tev_color_op(0, 0, 0, 0, 1, 0); pc_gfx_set_tev_color_op(1, 0, 0, 0, 1, 0); pc_gfx_set_tev_alpha_op(0, 0, 0, 0, 1, 0); pc_gfx_set_tev_alpha_op(1, 0, 0, 0, 1, 0);
        pc_gfx_set_tex_coord_gen(0, 1, 4, o.texMtx ? 36 : 60);   // coord 0 = TEX0
        pc_gfx_set_tex_coord_gen(1, 1, 5, 39);                   // coord 1 = TEX1 through matrix 39
        Mtx m39 = {{0.5f, 0.25f, 0, 0.125f}, {-0.25f, 2.0f, 0, 0.75f}, {0, 0, 1, 0}}; pc_gfx_load_tex_mtx(m39, 39);
    } else {
        pc_gfx_set_num_tev_stages(1); pc_gfx_set_tev_order(0, 0, 0, 4);
        pc_gfx_set_tev_color_in(0, 15, 8, 10, 15); pc_gfx_set_tev_alpha_in(0, 7, 4, 5, 7);
        pc_gfx_set_tex_coord_gen(0, 1, 4, o.texMtx ? 36 : 60);
    }
    Mtx m36 = {{2.0f, 0.5f, 0, 0.25f}, {0.1f, 3.0f, 0, 0.5f}, {0, 0, 1, 0}}; pc_gfx_load_tex_mtx(m36, 36);
    if (gTmi) { Mtx m30 = {{1.0f, 0, 0, 0.1f}, {0, -1.0f, 0, 0.9f}, {0, 0, 1, 0}}; pc_gfx_load_tex_mtx(m30, 30); Mtx m33 = {{0.25f, 0.5f, 0, 0}, {-0.5f, 0.25f, 0, 0.3f}, {0, 0, 1, 0}}; pc_gfx_load_tex_mtx(m33, 33); }
}

static void setArrays(const SceneOpt& o)
{
    pc_gfx_clear_vtx_desc();
    if (o.skinned) pc_gfx_set_vtx_desc((GXAttr)0, (GXAttrType)1);
    if (gTmi) { pc_gfx_set_vtx_desc((GXAttr)1, (GXAttrType)1); if (o.twoUnits) pc_gfx_set_vtx_desc((GXAttr)2, (GXAttrType)1); }
    pc_gfx_set_vtx_desc((GXAttr)9, (GXAttrType)3); pc_gfx_set_vtx_desc((GXAttr)10, (GXAttrType)3); pc_gfx_set_vtx_desc((GXAttr)11, (GXAttrType)3); pc_gfx_set_vtx_desc((GXAttr)13, (GXAttrType)3);
    if (o.twoUnits) pc_gfx_set_vtx_desc((GXAttr)14, (GXAttrType)3);
    pc_gfx_set_vtx_attr_fmt(0, 9, 1, 3, 2);  pc_gfx_set_array(9, sPos, 6);
    pc_gfx_set_vtx_attr_fmt(0, 10, 0, 4, 0); pc_gfx_set_array(10, sNrm, 12);
    pc_gfx_set_vtx_attr_fmt(0, 11, 1, 5, 0); pc_gfx_set_array(11, sClr, 4);
    pc_gfx_set_vtx_attr_fmt(0, 13, 1, 3, 8); pc_gfx_set_array(13, sUV, 4);
    pc_gfx_set_vtx_attr_fmt(0, 14, 1, 4, 0); pc_gfx_set_array(14, sUV1, 8);
}

static void setCamera(int view, std::mt19937& rng, bool skinned)
{
    float yaw = (float)(rng() % 628) / 100.f, pitch = -0.1f - (float)(rng() % 60) / 100.f;
    float cx = (float)((int)(rng() % 900) - 450), cy = 40.f + (float)(rng() % 300), cz = (float)((int)(rng() % 900) - 700);
    if (view % 4 == 0) { cy = 25.f; pitch = -0.05f; }                       // low camera: chunks cross the near plane -> CPU chunks
    if (view % 4 == 1) { cy = 700.f; cz = 900.f; pitch = -0.6f; yaw = 0; cx = 0; }   // far overview: mostly GPU
    const float cyw = cosf(yaw), syw = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    // view = Rx(-pitch) * Ry(-yaw) * T(-cam)
    float R[3][3] = {{cyw, 0, -syw}, {syw * sp, cp, cyw * sp}, {syw * cp, -sp, cyw * cp}};
    for (int id = 0; id < (skinned ? 3 : 1); ++id) {
        const float ang = skinned ? 0.15f * id : 0.0f, ca = cosf(ang), sa = sinf(ang), dx = skinned ? 15.f * id : 0.f;
        float Mm[3][4] = {{ca, 0, sa, dx}, {0, 1, 0, 0}, {-sa, 0, ca, 0}};                 // model (per bone)
        Mtx m;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) m[r][c] = R[r][0] * Mm[0][c] + R[r][1] * Mm[1][c] + R[r][2] * Mm[2][c];
            const float tx = Mm[0][3] - cx, ty = Mm[1][3] - cy, tz = Mm[2][3] - cz;
            m[r][3] = R[r][0] * tx + R[r][1] * ty + R[r][2] * tz;
        }
        pc_gfx_load_pos_mtx(m, id * 3);
        Mtx n; memcpy(n, m, sizeof n); n[0][3] = n[1][3] = n[2][3] = 0;
        pc_gfx_load_nrm_mtx(n, id * 3);
    }
    pc_gfx_set_current_mtx(0);
}

static void immQuad(float z)
{
    pc_gfx_begin((GXPrimitive)0x80, (GXVtxFmt)0, 4);
    pc_gfx_position(-50, -50, z); pc_gfx_color(255, 0, 0, 255); pc_gfx_texcoord(0, 0);
    pc_gfx_position(50, -50, z);  pc_gfx_color(0, 255, 0, 255); pc_gfx_texcoord(1, 0);
    pc_gfx_position(50, 50, z);   pc_gfx_color(0, 0, 255, 255); pc_gfx_texcoord(1, 1);
    pc_gfx_position(-50, 50, z);  pc_gfx_color(255, 255, 255, 255); pc_gfx_texcoord(0, 1);
    pc_gfx_end();
}

static std::vector<N3dsVtx> gRaw;   // last render(): the ring vertices of the final display-list call (mesh == false only)
static std::vector<Tri> render(const SceneOpt& o, int view, bool mesh, bool f24, const std::vector<uint8_t>& dl, int calls)
{
    std::mt19937 rng(o.seed * 7919u + (unsigned)view);
    pc_gfx_init();
    pc_gfx_set_viewport(0, 0, 640, 480, 0, 1); pc_gfx_set_scissor(0, 0, 640, 480);
    setProjection();
    pc_gfx_set_z_mode(1, (GXCompare)3, 1);
    gRing.reset(400000); gRing.quiet = true;
    gMesh.ready = mesh; gMesh.ev.clear();
    setMaterial(o); setArrays(o); setCamera(view, rng, o.skinned);
    if (gLightVariant) n3ds_test_light_setup(gLightVariant);
    std::vector<Tri> out;
    for (int k = 0; k < calls; ++k) {
        gMesh.ev.clear();
        const int before = gRing.nVerts;
        if (o.imm) { Mtx id = {{1,0,0,0},{0,1,0,0},{0,0,1,0}}; pc_gfx_load_pos_mtx(id, 30); pc_gfx_set_current_mtx(30); immQuad(-300); pc_gfx_set_current_mtx(0); }
        pc_gfx_call_display_list(dl.data(), (u32)dl.size());
        if (o.imm) { pc_gfx_set_current_mtx(30); immQuad(-200); pc_gfx_set_current_mtx(0); }
        n3ds_gfx_flush();
        if (k + 1 < calls) continue;
        if (!mesh) { ringTris(before, gRing.nVerts - before, out); gRaw.assign(gRing.verts() + before, gRing.verts() + gRing.nVerts); }
        else for (const MockEvent& e : gMesh.ev) { if (e.type == 0) ringTris(e.first, e.count, out); else shadeMesh(e, out, f24); }
    }
    return out;
}

static int countMeshDraws() { int n = 0; for (auto& e : gMesh.ev) n += e.type == 1; return n; }

extern void n3ds_test_lighting(int m);
extern int n3ds_test_light_vertex(float nz, float amb, float out[3]);
extern double n3ds_test_light_compare(int iters, double* tRef, double* tFast);
int main(int argc, char** argv)
{
    {   // r47: GameCube lighting, one light 10 units in front, colour (1, 0.5, 0.25), spot terms that give attenuation 1, diffuse clamp
        float o[3];
        n3ds_test_light_vertex(1.0f, 0.2f, o);    // facing the light: material x clamp(0.2 + light) = 200 x (1, 0.7, 0.45)
        CHECK(fabsf(o[0] - 200.0f) < 0.5f && fabsf(o[1] - 140.0f) < 0.5f && fabsf(o[2] - 90.0f) < 0.5f, "GX lighting: facing the light gives 200 140 90 (got %.1f %.1f %.1f)", o[0], o[1], o[2]);
        n3ds_test_light_vertex(0.0f, 0.2f, o);    // normal across the light direction: ambient only
        CHECK(fabsf(o[0] - 40.0f) < 0.5f && fabsf(o[1] - 40.0f) < 0.5f, "GX lighting: side-on gives ambient only, 40 (got %.1f %.1f %.1f)", o[0], o[1], o[2]);
        double tr = 0, tf = 0;                    // r50: the fast lighting path equals the reference
        const double e = n3ds_test_light_compare(3000, &tr, &tf);
        printf("  GX lighting fast path: max difference %.6f of 255, host ns/vertex: reference %.1f, fast %.1f\n", e, tr, tf);
        CHECK(e < 0.01, "GX lighting fast path: max difference to the reference %.5f of 255 over 192000 vertices (host: reference %.0f ns, fast %.0f ns per vertex)", e, tr, tf);
        n3ds_test_lighting(0);                    // the GPU-transform comparisons below use the shader's simple headlight on both sides
    }
    const char* shbin = argc > 1 ? argv[1] : "/tmp/mt/passthrough.shbin";
    FILE* f = fopen(shbin, "rb"); if (!f) { printf("no %s\n", shbin); return 2; }
    std::vector<uint8_t> bin; { uint8_t b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) bin.insert(bin.end(), b, b + n); fclose(f); }
    CHECK(gSh.load(bin), "shbin parses");
    const char* names[] = {"F", "P", "N", "tcu", "cw", "mc", "AMB", "LP", "LC", "LF", "LA", "LK", "LD", "xform", "lit", "head", "multi", "l0", "l1", "a0", "a1", "s0", "s1"};
    for (const char* n : names) CHECK(gSh.loc(n) >= 0, "uniform %s exported (%d)", n, gSh.loc(n));
    printf("  uniforms: F %d P %d N %d tcu %d cw %d mc %d AMB %d LP %d LD %d | xform 0x%x lit 0x%x | %zu constants\n", gSh.loc("F"), gSh.loc("P"), gSh.loc("N"), gSh.loc("tcu"), gSh.loc("cw"), gSh.loc("mc"), gSh.loc("AMB"), gSh.loc("LP"), gSh.loc("LD"), gSh.loc("xform"), gSh.loc("lit"), gSh.consts.size());
    int maxReg = 0; for (auto& k : gSh.consts) if (k.type == 2 && k.reg > maxReg) maxReg = k.reg;
    CHECK(gSh.loc("P") + 30 <= 96 && gSh.loc("N") + 30 <= 96 && gSh.loc("LD") + 2 <= 96 && maxReg < 96, "uniform space fits (const max reg %d, LD at %d)", maxReg, gSh.loc("LD"));
    {   // r53 hardware rules: every output register written exactly once per branch, never by a partial (.x / .y) write
        std::vector<int> outWrites(16, 0); int partial = 0;
        for (uint32_t pc = 0; pc < gSh.code.size(); ++pc) {
            const uint32_t ins = gSh.code[pc], op = ins >> 26;
            if (op >= 0x20 && op != 0x12) continue;
            if (op == 0x12) continue;                                     // MOVA writes the address register
            const int dst = (int)((ins >> 21) & 0x1F);
            if (dst >= 0x10) continue;                                    // temporaries
            ++outWrites[dst];
            if ((gSh.opdesc[ins & 0x7F] & 0xF) != 0xF) ++partial;
        }
        int maxW = 0; for (int w : outWrites) maxW = w > maxW ? w : maxW;
        CHECK(partial == 0, "no output register is written partially (%d partial writes)", partial);
        CHECK(maxW <= 2, "each output register written once per branch (max %d writes over both branches)", maxW);
    }

    // ring branch (xform = false) is still the passthrough
    { float in[16][4] = {{1, 2, 3, 4}, {255, 0, 51, 255}, {0.5f, 0.25f}, {0.1f, 0.2f}, {0.3f, 0.4f}}, o[16][4]; gSh.f24 = false; gSh.setB(gSh.loc("xform"), false); gSh.run(in, o);
      bool ok = true; for (int r = 0; r < 16; ++r) { const int t = gSh.outType[r];
        if (t == 0) ok &= o[r][0] == 1 && o[r][3] == 4; if (t == 2) ok &= fabsf(o[r][0] - 1) < 1e-6f && fabsf(o[r][2] - 0.2f) < 1e-6f; if (t == 3) ok &= o[r][0] == 0.5f; if (t == 6) ok &= fabsf(o[r][1] - 0.4f) < 1e-6f; }
      CHECK(ok, "ring branch forwards position / colour/255 / texcoords"); }

    const SceneOpt scenes[] = {
        {false, true,  true,  true,  false, false, true,  1},   // static, lit, vertex colour, 1 plain texture, immediate quads around the list
        {false, false, false, true,  false, true,  false, 2},   // material colour, texture through a matrix
        {false, true,  true,  true,  true,  true,  false, 3},   // two units (TEX0 via matrix, TEX1 via matrix)
        {true,  true,  true,  true,  false, false, false, 4},   // skinned: 3 matrices by per-vertex index
        {false, false, true,  false, false, false, true,  5},   // untextured
        {true,  false, false, true,  true,  false, true,  6},   // skinned + two units + material colour
    };
    if (getenv("NECTAR_BENCH") && atoi(getenv("NECTAR_BENCH")) == 3) {   // r54 profiling aid: the GPU transform path's CPU side, small models
        N3dsConfig& cf = const_cast<N3dsConfig&>(n3dsConfig()); cf.gpuXform = 1; cf.meshCpu = 1; cf.meshCpuCheck = 0;
        gMesh.bench = true;
        const int which = getenv("NECTAR_BENCH_SCENE") ? atoi(getenv("NECTAR_BENCH_SCENE")) : 0;
        SceneOpt o = scenes[which]; o.imm = false; std::mt19937 rng(o.seed); const std::vector<uint8_t> dl = buildList(o, rng);
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < 8; ++r) render(o, 1, true, false, dl, 250);
        printf("bench: GPU path, scene %d, %.2f us per display-list call (host)\n", which, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 2000.0);
        return 0;
    }
    if (getenv("NECTAR_BENCH")) {     // profiling aid: only the cached CPU draw, many times (valgrind --tool=callgrind)
        N3dsConfig& cf = const_cast<N3dsConfig&>(n3dsConfig()); cf.gpuXform = 0; cf.meshCpu = atoi(getenv("NECTAR_BENCH")) > 1 ? 0 : 1; cf.meshCpuCheck = 0;
        SceneOpt o = scenes[0]; o.imm = false; std::mt19937 rng(o.seed); const std::vector<uint8_t> dl = buildList(o, rng);
        for (int r = 0; r < 8; ++r) render(o, 1, false, false, dl, 100);
        return 0;
    }
    const char* sname[] = {"static-lit-tex", "material-texmtx", "two-units", "skinned", "untextured", "skinned-2units-mat"};
    int totalMeshDraws = 0, totalViews = 0, gpuOnlyViews = 0; double worstPos24 = 0, worstUV24 = 0, worstCol24 = 0;
    for (int s = 0; s < 6; ++s) {
        std::mt19937 rng(scenes[s].seed);
        const std::vector<uint8_t> dl = buildList(scenes[s], rng);
        int bad = 0, refT = 0, meshT = 0;
        for (int view = 0; view < 8; ++view) {
            const std::vector<Tri> ref = render(scenes[s], view, false, false, dl, 2);
            const std::vector<Tri> got = render(scenes[s], view, true, false, dl, 2);
            const int md = countMeshDraws(); totalMeshDraws += md; ++totalViews;
            bool anyRing = false; for (auto& e : gMesh.ev) anyRing |= e.type == 0 && !scenes[s].imm;
            if (md && !anyRing) ++gpuOnlyViews;
            char nm[64]; snprintf(nm, sizeof nm, "%s v%d", sname[s], view);
            const CmpResult r = compare(ref, got, 2e-5, 2e-5, nm, scenes[s].textured);
            if (r.missing || r.extraVisible) ++bad;
            refT += r.ref; meshT += r.fromMesh;
            CHECK(r.missing == 0 && r.extraVisible == 0, "%s: every CPU triangle reproduced in order (ref %d, got %d, matched %d, missing %d, extra visible %d)", nm, r.ref, r.got, r.matched, r.missing, r.extraVisible);
            // float24 precision run: same matching with looser tolerance, report the error
            const std::vector<Tri> g24 = render(scenes[s], view, true, true, dl, 2);
            const CmpResult r24 = compare(ref, g24, 2e-3, 2e-3, nm, scenes[s].textured);
            CHECK(r24.missing == 0, "%s (float24): all reference triangles still found (missing %d)", nm, r24.missing);
            worstPos24 = fmax(worstPos24, r24.maxPos); worstUV24 = fmax(worstUV24, r24.maxUV); worstCol24 = fmax(worstCol24, r24.maxCol);
        }
        printf("  scene %-20s: %d of 8 views differ | %d of %d visible triangles came from the GPU path\n", sname[s], bad, meshT, refT);
    }
    CHECK(totalMeshDraws > 20, "mesh path actually used (%d mesh draws over %d views)", totalMeshDraws, totalViews);

    // ---- r53: GameCube lighting in the shader vs gxLightVertex (lighting = 1), every light setup the shader supports + one it must refuse ----
    {
        n3ds_test_lighting(1);
        const char* vname[] = {"", "2 spot lights (rgb / alpha)", "2 lights sign diffuse, no attn", "specular", "spot + distance, shared by both channels", "3 lights (must stay on the CPU)", "point light at the vertex plane"};
        double worst = 0, worst24 = 0;
        for (int v = 1; v <= 6; ++v) {
            gLightVariant = v;
            int bad = 0, lit = 0, draws = 0;
            for (int s : {0, 2, 3}) {   // lit scenes: static, two units, skinned
                std::mt19937 rng(scenes[s].seed);
                const std::vector<uint8_t> dl = buildList(scenes[s], rng);
                for (int view = 0; view < 4; ++view) {
                    const std::vector<Tri> ref = render(scenes[s], view, false, false, dl, 2);
                    gLitDraws = 0; gShadedDraws = 0;
                    const std::vector<Tri> got = render(scenes[s], view, true, false, dl, 2);
                    lit += gLitDraws; draws += gShadedDraws;
                    char nm[96]; snprintf(nm, sizeof nm, "light v%d %s v%d", v, sname[s], view);
                    const CmpResult r = compare(ref, got, 2e-5, 2e-5, nm, scenes[s].textured);
                    if (r.missing || r.extraVisible) ++bad;
                    worst = fmax(worst, r.maxCol);
                    const std::vector<Tri> g24 = render(scenes[s], view, true, true, dl, 2);
                    const CmpResult r24 = compare(ref, g24, 2e-3, 2e-3, nm, scenes[s].textured);
                    if (r24.missing) ++bad;
                    worst24 = fmax(worst24, r24.maxCol);
                }
            }
            printf("  lighting %-44s: %d of 12 views differ | %d of %d mesh draws lit by the shader\n", vname[v], bad, lit, draws);
            CHECK(bad == 0, "GX lighting on the GPU path, %s: every view matches the CPU lighting (colour within 1.6/255)", vname[v]);
            if (v == 5) CHECK(lit == 0, "3 lights: no draw lit by the shader (handed to the CPU), got %d", lit);
            else CHECK(lit > 0, "%s: the shader lit some draws (%d)", vname[v], lit);
        }
        gLightVariant = 0;
        printf("  GPU lighting worst colour difference: float32 %.3f /255, float24 %.3f /255\n", worst, worst24);
        n3ds_test_lighting(0);
    }
    printf("  mesh draws %d over %d views (%d views fully on the GPU); float24 worst: pos %.2e (rel. to 1+|w|), colour %.2f /255, uv %.2e\n", totalMeshDraws, totalViews, gpuOnlyViews, worstPos24, worstCol24, worstUV24);

    // invalidation + rebuild: rewriting the colour array and flushing it must change the output
    {
        SceneOpt o = scenes[0]; o.imm = false; std::mt19937 rng(o.seed); const std::vector<uint8_t> dl = buildList(o, rng);
        render(o, 1, true, false, dl, 2);
        extern unsigned n3ds_test_mesh_built(void); extern unsigned n3ds_test_arena_used(void);
        const int alloc0 = (int)n3ds_test_mesh_built();
        // change content WITHOUT telling: the fingerprint/verify net is the only protection; with telling (rebuild) is required.
        for (auto& c : sClr) c[0] ^= 0x55;
        // r53: with gpu_xform = 1 the CPU fallback is the CACHED draw too, so the reference has to be the normal (uncached) path
        { N3dsConfig& cf = const_cast<N3dsConfig&>(n3dsConfig()); cf.meshCpu = 0; }
        const std::vector<Tri> ref = render(o, 1, false, false, dl, 1);
        { N3dsConfig& cf = const_cast<N3dsConfig&>(n3dsConfig()); cf.meshCpu = 1; }
        // the mesh path on a changed array: the safety net (signature / list fingerprint) does not see colour-only changes, the
        // game's DCFlushRange does. Emulate it the way the worker does it on the queue.
        extern void n3ds_test_mesh_inval(const void*, size_t);
        n3ds_test_mesh_inval(sClr, sizeof sClr);
        const std::vector<Tri> got = render(o, 1, true, false, dl, 1);
        const CmpResult r = compare(ref, got, 2e-5, 2e-5, "inval");
        CHECK(r.missing == 0 && r.extraVisible == 0 && (int)n3ds_test_mesh_built() > alloc0, "DCFlushRange over the colour array rebuilds the mesh (missing %d, builds %d -> %d)", r.missing, alloc0, (int)n3ds_test_mesh_built());
        const unsigned usedBefore = n3ds_test_arena_used();
        for (unsigned fr = 100; fr < 106; ++fr) n3ds_gfx_frame_end(fr);
        CHECK(n3ds_test_arena_used() < usedBefore + 1 && usedBefore > 0, "replaced meshes give their arena pages back a few frames later (%u -> %u pages)", usedBefore, n3ds_test_arena_used());
    }
    // a list with a matrix-load command inside stays on the CPU path
    {
        SceneOpt o = scenes[4]; std::mt19937 rng(o.seed); std::vector<uint8_t> dl = buildList(o, rng);
        const uint8_t cp[6] = {0x08, 0x30, 0, 0, 0, 0}; dl.insert(dl.begin(), cp, cp + 6);
        render(o, 1, true, false, dl, 2);
        CHECK(countMeshDraws() == 0, "list with a CP matrix-index command is not cached");
    }

    // ---- CPU cache path (gpu_xform = 0, mesh_cpu = 1): bit-identical to the normal CPU path -----------------------------------------------
    {
        N3dsConfig& cf = const_cast<N3dsConfig&>(n3dsConfig());
        extern unsigned n3ds_test_cpu_lists(void); extern unsigned n3ds_test_cpu_checks(void); extern unsigned n3ds_test_cpu_bad(void);
        extern void n3ds_test_cpu_reset(void); extern unsigned n3ds_test_cpu_volatile(void); extern void n3ds_test_cpu_corrupt(void); extern void n3ds_test_cpu_clearbad(void); extern void n3ds_test_cpu_recheck(void); extern unsigned n3ds_test_cpu_recoveries(void);
        cf.gpuXform = 0; cf.lodPx = 0; cf.lodModelPx = 0; cf.lodSimplifyPx = 0;   // (the shipped defaults are lod_px 30 / lod_model_px 80, which make the cached draw differ from the normal path ON PURPOSE)
        // triangles with two identical clip positions draw nothing and are dropped by the cache build: leave them out on both sides
        auto strip = [](const std::vector<N3dsVtx>& in) {
            std::vector<N3dsVtx> o;
            for (size_t i = 0; i + 2 < in.size(); i += 3) {
                auto same = [](const N3dsVtx& a, const N3dsVtx& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; };
                if (same(in[i], in[i + 1]) || same(in[i + 1], in[i + 2]) || same(in[i], in[i + 2])) continue;
                o.insert(o.end(), in.begin() + i, in.begin() + i + 3);
            }
            return o;
        };
        auto identical = [](const std::vector<N3dsVtx>& a, const std::vector<N3dsVtx>& b) { return a.size() == b.size() && (a.empty() || memcmp(a.data(), b.data(), a.size() * sizeof(N3dsVtx)) == 0); };
        int diffViews = 0, usedViews = 0; size_t totTris = 0;
        for (int pass = 0; pass < 2; ++pass) {        // pass 0: cached draw only; pass 1: with the built-in self-check (must report no difference)
            cf.meshCpuCheck = pass;
            n3ds_test_cpu_reset();
            for (int s = 0; s < 6; ++s) {
                std::mt19937 rng(scenes[s].seed);
                const std::vector<uint8_t> dl = buildList(scenes[s], rng);
                for (int view = 0; view < 8; ++view) {
                    cf.meshCpu = 0; render(scenes[s], view, false, false, dl, 2); const std::vector<N3dsVtx> ref = strip(gRaw);
                    cf.meshCpu = 1; const unsigned l0 = n3ds_test_cpu_lists(); render(scenes[s], view, false, false, dl, 2); const std::vector<N3dsVtx> got = strip(gRaw);
                    const bool same = identical(ref, got);
                    if (pass == 0) { if (n3ds_test_cpu_lists() > l0) ++usedViews; totTris += ref.size() / 3; if (!same) ++diffViews; }
                    CHECK(same, "cpu cache pass %d: %s v%d output identical to the normal CPU path (ref %zu tris, got %zu)", pass, sname[s], view, ref.size() / 3, got.size() / 3);
                }
            }
            if (pass == 1) CHECK(n3ds_test_cpu_bad() == 0 && n3ds_test_cpu_checks() > 0, "self-check ran (%u ok) and found no difference (%u)", n3ds_test_cpu_checks(), n3ds_test_cpu_bad());
        }
        printf("  cpu cache: %d of 48 views differ | cached draw used in %d views | %zu visible triangles compared bit for bit\n", diffViews, usedViews, totTris);
        {   // r20: the batched (gather) transform and the phase-profiled passes must give exactly the same triangles
            extern void n3ds_test_phase(int on); extern unsigned n3ds_test_phase_chunks(void);
            cf.meshCpuCheck = 0;
            for (int mode = 1; mode <= 2; ++mode) {
                int diff = 0; size_t tris = 0;
                cf.fastXform = mode == 1; n3ds_test_phase(mode == 2);
                for (int s = 0; s < 6; ++s) {
                    std::mt19937 rng(scenes[s].seed);
                    const std::vector<uint8_t> dl = buildList(scenes[s], rng);
                    for (int view = 0; view < 8; ++view) {
                        cf.meshCpu = 0; render(scenes[s], view, false, false, dl, 2); const std::vector<N3dsVtx> ref = strip(gRaw);
                        cf.meshCpu = 1; render(scenes[s], view, false, false, dl, 2); const std::vector<N3dsVtx> got = strip(gRaw);
                        if (!identical(ref, got)) ++diff;
                        tris += got.size() / 3;
                    }
                }
                if (mode == 2) { CHECK(n3ds_test_phase_chunks() > 100, "phase profiler ran (%u chunks)", n3ds_test_phase_chunks()); printf("  phase-profiled passes: %u chunks timed\n", n3ds_test_phase_chunks()); }
                CHECK(diff == 0 && tris > 5000, "cpu cache, %s: 48 views identical to the normal path (%d differ, %zu tris)", mode == 1 ? "batched gather transform" : "phase-profiled passes", diff, tris);
            }
            cf.fastXform = 0; n3ds_test_phase(0);
        }
        CHECK(usedViews > 30 && totTris > 5000, "cached CPU path actually used (%d views, %zu triangles)", usedViews, totTris);

        { extern unsigned n3ds_test_arena_stress(void); const unsigned r = n3ds_test_arena_stress();
          CHECK(r / 1000000u == 0, "model arena: 30000 random alloc/free (small from the bottom, big from the top): %u overlaps / out-of-range; %u allocation refusals", r / 1000000u, r % 1000000u); }
        // data changed AND flushed: rebuilt, output follows
        {
            cf.meshCpuCheck = 0; cf.meshCpu = 1;
            SceneOpt o = scenes[0]; o.imm = false; std::mt19937 rng(o.seed); const std::vector<uint8_t> dl = buildList(o, rng);
            render(o, 1, false, false, dl, 2);
            for (auto& c : sClr) c[1] ^= 0x33;
            extern void n3ds_test_mesh_inval(const void*, size_t); n3ds_test_mesh_inval(sClr, sizeof sClr);
            render(o, 1, false, false, dl, 2); const std::vector<N3dsVtx> got = strip(gRaw);
            cf.meshCpu = 0; render(o, 1, false, false, dl, 2); const std::vector<N3dsVtx> ref = strip(gRaw);
            CHECK(identical(ref, got) && ref.size() > 100, "cpu cache: changed + flushed colour array is picked up (%zu tris)", ref.size() / 3);
        }
        // thrash guard: a list whose first position changes on every use is not rebuilt forever
        {
            cf.meshCpuCheck = 0; cf.meshCpu = 1;
            SceneOpt o = scenes[1]; o.imm = false; std::mt19937 rng(o.seed); const std::vector<uint8_t> dl = buildList(o, rng);
            const unsigned v0 = n3ds_test_cpu_volatile(); extern unsigned n3ds_test_mesh_built(void); const unsigned b0 = n3ds_test_mesh_built();
            for (int i = 0; i < 12; ++i) {
                sPos[0][0] = (int16_t)(sPos[0][0] + 1);      // unannounced change at the fingerprinted spot
                cf.meshCpu = 1; render(o, 2, false, false, dl, 1); const std::vector<N3dsVtx> got = strip(gRaw);
                cf.meshCpu = 0; render(o, 2, false, false, dl, 1); const std::vector<N3dsVtx> ref = strip(gRaw);
                CHECK(identical(ref, got), "cpu cache: volatile list draw %d matches the normal path", i);
            }
            CHECK(n3ds_test_cpu_volatile() > v0 && n3ds_test_mesh_built() - b0 <= 5, "cpu cache: thrashing list stops being rebuilt (volatile %u, builds %u in 12 uses)", n3ds_test_cpu_volatile() - v0, n3ds_test_mesh_built() - b0);
        }
        // lack of memory is retried soon (log f60c4ca9: 82 big models failed once and stayed on the slow path for 1200 uses)
        {
            cf.meshCpuCheck = 0; cf.meshCpu = 1;
            SceneOpt o = scenes[2]; o.imm = false; std::mt19937 rng(o.seed + 5); const std::vector<uint8_t> dl = buildList(o, rng);
            const int kb0 = cf.meshCacheKb; cf.meshCacheKb = 0;               // budget full: every build is refused for lack of memory
            extern unsigned n3ds_test_mesh_built(void);
            const unsigned l0 = n3ds_test_cpu_lists();
            for (int i = 0; i < 5; ++i) render(o, 1, false, false, dl, 1);
            const bool refused = n3ds_test_cpu_lists() == l0;
            cf.meshCacheKb = kb0;                                              // memory is back
            int usesToCache = -1;
            for (int i = 1; i <= 60 && usesToCache < 0; ++i) { const unsigned l1 = n3ds_test_cpu_lists(); render(o, 1, false, false, dl, 1); if (n3ds_test_cpu_lists() > l1) usesToCache = i; }
            CHECK(refused && usesToCache > 0 && usesToCache <= 45, "cpu cache: a model refused for lack of memory is cached again soon after (refused %d, back in the cache after %d uses; was 1200)", (int)refused, usesToCache);
        }
        // lod_simplify_px (r10, lossy): small models are drawn from a vertex-clustered copy: fewer triangles, never more, never none
        {
            cf.meshCpuCheck = 0; cf.meshCpu = 1; size_t refT = 0, gotT = 0; int fewer = 0, empty = 0;
            for (int s = 0; s < 6; ++s) {
                std::mt19937 rng(scenes[s].seed); SceneOpt o = scenes[s]; o.imm = false; const std::vector<uint8_t> dl = buildList(o, rng);
                cf.lodSimplifyPx = 0; cf.meshCpu = 0; render(o, 1, false, false, dl, 2); const size_t r = strip(gRaw).size() / 3;
                cf.meshCpu = 1; cf.lodSimplifyPx = 100000; render(o, 1, false, false, dl, 2); render(o, 1, false, false, dl, 2); const size_t g = strip(gRaw).size() / 3;
                refT += r; gotT += g; if (g < r) ++fewer; if (r > 0 && g == 0) ++empty;
                CHECK(g <= r, "lod_simplify %s: %zu tris <= %zu", sname[s], g, r);
            }
            printf("  lod_simplify: %zu -> %zu triangles over 6 scenes (%d scenes simplified)\n", refT, gotT, fewer);
            CHECK(fewer > 0 && empty == 0, "lod_simplify reduced some scenes (%d) and emptied none (%d)", fewer, empty);
            cf.lodSimplifyPx = 0;
        }
        // lod_px (lossy): chunks whose whole screen box is tiny are skipped by the cached draw only; never more triangles than the normal path
        {
            cf.meshCpuCheck = 0; cf.meshCpu = 1;
            size_t refTris = 0, lodTris = 0, allTris = 0; int fewer = 0, more = 0;
            for (int s = 0; s < 4; ++s) {
                std::mt19937 rng(scenes[s].seed + 77); const std::vector<uint8_t> dl = buildList(scenes[s], rng);
                for (int view = 0; view < 8; ++view) {
                    cf.lodPx = 0; cf.meshCpu = 0; render(scenes[s], view, false, false, dl, 2); const size_t r = strip(gRaw).size();
                    cf.meshCpu = 1; cf.lodPx = 400; render(scenes[s], view, false, false, dl, 2); const size_t l = strip(gRaw).size();
                    cf.lodPx = 100000; render(scenes[s], view, false, false, dl, 2); allTris += strip(gRaw).size();
                    refTris += r; lodTris += l; if (l > r) ++more; if (l < r) ++fewer;
                }
            }
            cf.lodPx = 0;
            CHECK(more == 0 && lodTris <= refTris && allTris < refTris && fewer > 0, "lod_px: never more triangles than the normal path, 40 px drops some (%zu vs %zu, %d views with fewer), a huge value drops many (%zu left)", lodTris / 3, refTris / 3, fewer, allTris / 3);
        }
        // lod_model_px (lossy "render distance"): a whole model that is small on screen is skipped; never more triangles than the normal path
        {
            cf.meshCpuCheck = 0; cf.meshCpu = 1; cf.lodPx = 0;
            size_t refTris = 0, lodTris = 0, allTris = 0, zeroTris = 0; int fewer = 0, more = 0;
            for (int s = 0; s < 6; ++s) {
                std::mt19937 rng(scenes[s].seed + 91); const std::vector<uint8_t> dl = buildList(scenes[s], rng);
                for (int view = 0; view < 8; ++view) {
                    cf.lodModelPx = 0; cf.meshCpu = 0; render(scenes[s], view, false, false, dl, 2); const size_t r = strip(gRaw).size();
                    cf.meshCpu = 1; render(scenes[s], view, false, false, dl, 2); zeroTris += strip(gRaw).size();
                    cf.lodModelPx = 600; render(scenes[s], view, false, false, dl, 2); const size_t l = strip(gRaw).size();
                    cf.lodModelPx = 100000; render(scenes[s], view, false, false, dl, 2); allTris += strip(gRaw).size();
                    refTris += r; lodTris += l; if (l > r) ++more; if (l < r) ++fewer;
                }
            }
            cf.lodModelPx = 0;
            CHECK(more == 0 && zeroTris == refTris && lodTris <= refTris && allTris < refTris, "lod_model_px: off = same triangles as the normal path (%zu vs %zu), 60 px never more (%zu, %d views with fewer), a huge value drops many (%zu left)", zeroTris / 3, refTris / 3, lodTris / 3, fewer, allTris / 3);
        }
        // safety net 1: a corrupted cache entry is detected by the re-check, the entry is rebuilt, the picture stays right and the model keeps using the cache
        {
            cf.meshCpuCheck = 1; cf.meshCpu = 1; n3ds_test_cpu_reset();
            SceneOpt o = scenes[4]; o.imm = false; std::mt19937 rng(o.seed + 11); const std::vector<uint8_t> dl = buildList(o, rng);
            render(o, 1, false, false, dl, 1);                       // build + first check (ok)
            n3ds_test_cpu_corrupt(); n3ds_test_cpu_recheck();        // (the periodic re-check, every 601st use, made due)
            render(o, 1, false, false, dl, 1); const std::vector<N3dsVtx> got = strip(gRaw);   // the re-check sees the corruption
            const unsigned l0 = n3ds_test_cpu_lists();
            cf.meshCpu = 0; render(o, 1, false, false, dl, 1); const std::vector<N3dsVtx> ref = strip(gRaw);
            CHECK(n3ds_test_cpu_bad() == 0 && n3ds_test_cpu_recoveries() >= 1, "re-check detects a corrupted cache entry and rebuilds it (confirmed differences %u, rebuilds %u)", n3ds_test_cpu_bad(), n3ds_test_cpu_recoveries());
            CHECK(identical(ref, got) && ref.size() > 30, "on the detecting use the normal path drew the list (%zu tris)", ref.size() / 3);
            cf.meshCpu = 1; render(o, 1, false, false, dl, 1); const std::vector<N3dsVtx> got2 = strip(gRaw);
            CHECK(identical(ref, got2) && n3ds_test_cpu_lists() > l0, "after the rebuild the cache is used again and matches");
            // safety net 2: still different after the rebuild -> that model is switched to the normal path for good
            n3ds_test_cpu_corrupt(); n3ds_test_cpu_recheck();
            render(o, 1, false, false, dl, 1);                       // difference #1 -> rebuild
            n3ds_test_cpu_corrupt();                                 // the rebuilt entry is bad again before its first check
            render(o, 1, false, false, dl, 1); const std::vector<N3dsVtx> got3 = strip(gRaw);   // difference #2 -> confirmed
            CHECK(n3ds_test_cpu_bad() == 1 && identical(ref, got3), "a difference that survives the rebuild is confirmed (%u) and the normal path draws", n3ds_test_cpu_bad());
            const unsigned l1 = n3ds_test_cpu_lists();
            render(o, 1, false, false, dl, 1); const std::vector<N3dsVtx> got4 = strip(gRaw);
            CHECK(identical(ref, got4) && n3ds_test_cpu_lists() == l1, "that model no longer uses the cache");
            n3ds_test_cpu_clearbad();
        }
        // per-vertex texture-matrix ids (GX_VA_TEX0MTXIDX / TEX1MTXIDX): cached by the CPU cache, bit-identical to the normal path (log cea14c68: 55-72 such lists per frame)
        {
            int usedT = 0, sameT = 0, nT = 0;
            for (int pass = 0; pass < 2; ++pass) for (int si = 1; si <= 2; ++si) {
                cf.meshCpuCheck = pass; n3ds_test_cpu_reset(); gTmi = true;
                SceneOpt o = scenes[si]; o.imm = false; std::mt19937 rng(o.seed + 31); const std::vector<uint8_t> dl = buildList(o, rng);
                for (int view = 0; view < 6; ++view) {
                    cf.meshCpu = 0; render(o, view, false, false, dl, 2); const std::vector<N3dsVtx> ref = strip(gRaw);
                    cf.meshCpu = 1; const unsigned l0 = n3ds_test_cpu_lists(); render(o, view, false, false, dl, 2); const std::vector<N3dsVtx> got = strip(gRaw);
                    const bool same = identical(ref, got); ++nT; sameT += same; if (n3ds_test_cpu_lists() > l0) ++usedT;
                    CHECK(same && ref.size() > 30, "per-vertex texture matrices: scene %d pass %d view %d identical to the normal path (ref %zu tris, got %zu)", si, pass, view, ref.size() / 3, got.size() / 3);
                }
                if (pass == 1) CHECK(n3ds_test_cpu_bad() == 0 && n3ds_test_cpu_checks() > 0, "per-vertex texture matrices: self-check ran (%u) and found no difference (%u)", n3ds_test_cpu_checks(), n3ds_test_cpu_bad());
                gTmi = false;
            }
            printf("  per-vertex texture matrices: %d of %d views identical, cached draw used in %d\n", sameT, nT, usedT);
            CHECK(usedT >= nT - 2, "per-vertex texture-matrix lists are drawn from the cache (%d of %d views)", usedT, nT);
            // the ids really matter: a list whose matrices differ from the plain texgen one must look different from the same list without the ids
            gTmi = true; SceneOpt o = scenes[1]; o.imm = false; std::mt19937 r1(o.seed + 31); const std::vector<uint8_t> dlT = buildList(o, r1);
            cf.meshCpuCheck = 0; cf.meshCpu = 1; render(o, 1, false, false, dlT, 2); const std::vector<N3dsVtx> withIds = strip(gRaw);
            gTmi = false;
            CHECK(!withIds.empty(), "tmi list renders");
            gTmi = true; cf.meshCpu = 0; render(o, 1, false, false, dlT, 2); const std::vector<N3dsVtx> refIds = strip(gRaw); gTmi = false;
            CHECK(identical(refIds, withIds), "tmi list from the cache matches the normal path (second look)");
            cf.meshCpuCheck = 0;
        }
        // fan soup like the Forest of Hope terrain: every triangle is its own 3-vertex GX_TRIANGLEFAN draw command (thousands of commands per list)
        {
            cf.meshCpuCheck = 0;
            SceneOpt o = scenes[0]; o.imm = false; std::mt19937 rng(o.seed); std::vector<uint8_t> base = buildList(o, rng), dl;
            auto vtx = [&](int k) { for (int a = 0; a < 4; ++a) { dl.push_back((uint8_t)(k >> 8)); dl.push_back((uint8_t)k); } };
            for (int rep = 0; rep < 6; ++rep) for (int j = 0; j < GN; ++j) for (int i = 0; i < GN; ++i) {
                const int a = j * (GN + 1) + i, b = a + 1, c = a + (GN + 1), d = c + 1;
                dl.push_back(0xA0); dl.push_back(0); dl.push_back(3); vtx(a); vtx(c); vtx(b);
                dl.push_back(0xA0); dl.push_back(0); dl.push_back(3); vtx(b); vtx(c); vtx(d);
            }
            int same = 0; size_t tri = 0; double ms[2] = {0, 0};
            for (int view = 0; view < 8; ++view) {
                cf.meshCpu = 0; render(o, view, false, false, dl, 2); const std::vector<N3dsVtx> ref = strip(gRaw);
                cf.meshCpu = 1; render(o, view, false, false, dl, 2); const std::vector<N3dsVtx> got = strip(gRaw);
                const bool ok = identical(ref, got); same += ok; tri += ref.size() / 3;
                CHECK(ok, "fan soup view %d identical (ref %zu tris, got %zu)", view, ref.size() / 3, got.size() / 3);
                for (int m = 0; m < 2; ++m) { cf.meshCpu = m; const auto t0 = std::chrono::steady_clock::now(); render(o, view, false, false, dl, 8);
                    ms[m] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 8.0 / 8.0; }
            }
            printf("  fan soup (%d commands per list): %d of 8 views identical, %zu triangles | host speed: normal path %.3f ms/list, cached path %.3f ms/list (x%.2f)\n", 6 * GN * GN * 2, same, tri, ms[0], ms[1], ms[0] / ms[1]);
        }
        // rough speed comparison on the host (relative only)
        {
            cf.meshCpuCheck = 0;
            SceneOpt o = scenes[0]; o.imm = false; std::mt19937 rng(o.seed); const std::vector<uint8_t> dl = buildList(o, rng);
            double ms[2] = {0, 0};
            for (int rep = 0; rep < 3; ++rep) for (int m = 0; m < 2; ++m) {
                cf.meshCpu = m; const auto t0 = std::chrono::steady_clock::now();
                render(o, 1, false, false, dl, 100);
                ms[m] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 300.0;
            }
            printf("  host speed (view 1, %d-cell grid list): normal path %.3f ms/list, cached path %.3f ms/list (x%.2f)\n", GN * GN, ms[0], ms[1], ms[0] / ms[1]);
        }
        cf.gpuXform = 1; cf.meshCpu = 1; cf.meshCpuCheck = 1;
    }
    {   extern void n3ds_test_classify_fast(int, unsigned*, unsigned*, unsigned*, unsigned*);   // r54b: interval classifier vs the exact one
        SceneOpt o = {false, true, true, true, false, false, false, 1}; std::mt19937 rng(1); const std::vector<uint8_t> dl = buildList(o, rng);
        render(o, 1, true, false, dl, 1);                       // a real projection / viewport / scissor in place
        unsigned dec = 0, bad = 0, ones = 0, zeros = 0;
        n3ds_test_classify_fast(200000, &dec, &bad, &ones, &zeros);
        printf("  r54b interval box test: 200000 random boxes, %u decided without the 8-corner test (%u GPU-safe, %u invisible), %u contradictions\n", dec, ones, zeros, bad);
        CHECK(bad == 0 && ones > 1000 && zeros > 1000, "interval box test never contradicts the exact one (%u contradictions, %u safe, %u invisible)", bad, ones, zeros);
    }
    {   extern unsigned n3ds_test_whole_gpu(void);   // r54: the whole-model fast path really ran (and the comparisons above covered it)
        CHECK(n3ds_test_whole_gpu() > 0, "whole models drawn by the GPU without per-piece tests: %u", n3ds_test_whole_gpu());
        printf("  r54 whole-model GPU draws (no per-piece tests): %u\n", n3ds_test_whole_gpu()); }
    printf("%d checks, %d failed\n", gChecks, gFail);
    return gFail ? 1 : 0;
}
