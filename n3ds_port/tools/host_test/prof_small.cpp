#include <stddef.h>
// HOST PROFILE: a game-like frame = many SMALL models (own matrix + material each) + particle quads. For callgrind / timing.
extern unsigned n3ds_test_mesh_built(void); extern unsigned n3ds_test_arena_used(void); extern void n3ds_test_mesh_inval(const void*, unsigned long);
#define main test_main_unused
#include "test_mesh.cpp"
#undef main
static uint8_t sPool[48][16 * 16 * 4];
static std::vector<uint8_t> smallList(int cells, int ox, int oy, std::mt19937& rng)
{
    std::vector<uint8_t> dl;
    auto vtx = [&](int k) { for (int a = 0; a < 4; ++a) { dl.push_back((uint8_t)(k >> 8)); dl.push_back((uint8_t)k); } };
    for (int n = 0; n < cells; ++n) {
        const int i = ox + n % 6, j = oy + n / 6; const int a = j * (GN + 1) + i, b = a + 1, c = a + (GN + 1), d = c + 1;
        dl.push_back(0x90); dl.push_back(0); dl.push_back(6); vtx(a); vtx(c); vtx(b); vtx(b); vtx(c); vtx(d);
    }
    return dl;
}
int main(int argc, char** argv)
{
    const int models = argc > 1 ? atoi(argv[1]) : 240, frames = argc > 2 ? atoi(argv[2]) : 40, mode = argc > 3 ? atoi(argv[3]) : 1, quads = argc > 4 ? atoi(argv[4]) : 300;
    N3dsConfig& cf = const_cast<N3dsConfig&>(n3dsConfig());
    cf.gpuXform = 0; cf.meshCpu = mode; cf.fastXform = argc > 5 ? atoi(argv[5]) : 1; cf.meshCpuCheck = 0; cf.meshCacheKb = 6144;
    SceneOpt o = {false, true, true, true, false, false, false, 5};
    std::mt19937 rng(1); buildList(o, rng);   // fills sPos / sNrm / sClr / sUV arrays
    for (int t = 0; t < 48; ++t) memset(sPool[t], 40 + t * 4, sizeof sPool[t]);
    std::vector<std::vector<uint8_t>> lists;
    for (int m = 0; m < models; ++m) lists.push_back(smallList(36, (m * 5) % 16, (m * 3) % 16, rng));
    pc_gfx_init(); pc_gfx_set_viewport(0, 0, 640, 480, 0, 1); pc_gfx_set_scissor(0, 0, 640, 480); setProjection(); pc_gfx_set_z_mode(1, (GXCompare)3, 1);
    gRing.reset(4000000); gRing.quiet = true; gMesh.ready = false;
    setArrays(o);
    unsigned lateBuilds = 0;
    const auto t0 = std::chrono::steady_clock::now(); unsigned tris0 = 0;
    for (int f = 0; f < frames; ++f) {
        std::mt19937 r2(7); setCamera(1, r2, false);
        for (int m = 0; m < models; ++m) {
            // per-model state like the game: own material (texture, TEV, chan ctrl) + matrix
            static GXTexObj to; pc_gfx_init_tex_obj_rgba(&to, sPool[m % 48], 16, 16, 1, 1); pc_gfx_load_tex_obj(&to, 0);
            pc_gfx_set_chan_ctrl((GXChannelID)4, 1, (GXColorSrc)0, (GXColorSrc)1, 1, (GXDiffuseFn)0, (GXAttnFn)0);
            GXColor mc = {200, (uint8_t)(m & 255), 100, 180}; pc_gfx_set_chan_mat_color((GXChannelID)4, mc);
            pc_gfx_set_num_tev_stages(1); pc_gfx_set_tev_order(0, 0, 0, 4); pc_gfx_set_tev_color_in(0, 15, 8, 10, 15); pc_gfx_set_tev_alpha_in(0, 7, 4, 5, 7);
            pc_gfx_set_tex_coord_gen(0, 1, 4, 60);
            pc_gfx_call_display_list(lists[m].data(), (u32)lists[m].size());
            if (m < quads) { Mtx id = {{1,0,0,0},{0,1,0,0},{0,0,1,0}}; pc_gfx_load_pos_mtx(id, 30); pc_gfx_set_current_mtx(30); immQuad(-300 - (float)m); pc_gfx_set_current_mtx(0); }
        }
        n3ds_gfx_flush(); if (f >= 10) lateBuilds += n3ds_test_mesh_built(); n3ds_gfx_frame_end(f + 1);
        if (f == 0) tris0 = gRing.nVerts / 3;
    }
    { const unsigned before = n3ds_test_arena_used(); n3ds_test_mesh_inval((const void*)16, (unsigned long)1 << 47);
      for (int f = 0; f < 3; ++f) { n3ds_gfx_flush(); n3ds_gfx_frame_end(1000 + f); }
      printf("arena pages in use: %u with %d models cached, %u after invalidating everything and 3 frames\n", before, models, n3ds_test_arena_used()); }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
    printf("builds after frame 10: %u\n", lateBuilds); printf("mode %d: %d models, %d quads, %d frames: %.3f ms/frame on the host, ring verts after first frame %u\n", mode, models, quads, frames, ms, tris0 * 3);
    return 0;
}
