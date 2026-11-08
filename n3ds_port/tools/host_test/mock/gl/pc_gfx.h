// HOST TEST ONLY: minimal stand-in for upstream pc_port/gl/pc_gfx.h -- just the types and prototypes that
// platform/n3ds_gfx.cpp uses, so its geometry / culling logic can be compiled and exercised with a normal g++.
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef float f32;
typedef f32 Mtx[3][4]; typedef f32 Mtx44[4][4];
typedef int GXBool, GXCompare, GXBlendMode, GXBlendFactor, GXLogicOp, GXCullMode, GXAlphaOp, GXChannelID, GXColorSrc, GXDiffuseFn,
            GXAttnFn, GXTevStageID, GXTexCoordID, GXTexMapID, GXAttr, GXAttrType, GXVtxFmt, GXCompCnt, GXCompType, GXPrimitive,
            GXProjectionType, GXTevColorArg, GXTevAlphaArg, GXTevOp, GXTevBias, GXTevScale, GXTevRegID, GXTevKColorID, GXTevKColorSel,
            GXTevKAlphaSel, GXTexFmt, GXCITexFmt, GXTexWrapMode, GXTlutFmt, GXTexGenType, GXTexGenSrc;
struct GXColorS10 { short r, g, b, a; };
struct GXColor { u8 r, g, b, a; };
struct GXTexObj { uint32_t dummy[8]; };
struct GXTlutObj { uint32_t dummy[4]; };   /* real one is 3 x u32 (asserted in the 3DS build); host pointers are 8 bytes, hence 4 */
extern "C" {
void pc_gfx_init(void);
void pc_gfx_set_projection(const Mtx44 m, GXProjectionType);
void pc_gfx_set_viewport(f32 x, f32 y, f32 w, f32 h, f32, f32);
void pc_gfx_set_scissor(u32 x, u32 y, u32 w, u32 h);
void pc_gfx_load_pos_mtx(const Mtx m, u32 id);
void pc_gfx_load_nrm_mtx(const Mtx m, u32 id);
void pc_gfx_set_current_mtx(u32 id);
void pc_gfx_set_z_mode(GXBool en, GXCompare func, GXBool upd);
void pc_gfx_set_blend_mode(GXBlendMode type, GXBlendFactor s, GXBlendFactor d, GXLogicOp);
void pc_gfx_set_cull_mode(GXCullMode m);
void pc_gfx_set_color_update(GXBool b);
void pc_gfx_set_alpha_update(GXBool b);
void pc_gfx_set_alpha_compare(GXCompare c0, u8 r0, GXAlphaOp, GXCompare c1, u8 r1);
void pc_gfx_set_copy_clear(GXColor c, u32);
void pc_gfx_set_chan_ctrl(GXChannelID chan, GXBool enable, GXColorSrc, GXColorSrc matSrc, u32 lightMask, GXDiffuseFn, GXAttnFn);
void pc_gfx_set_chan_mat_color(GXChannelID chan, GXColor col);
/* r28 lights / ambient / fog */
void pc_gfx_set_chan_amb_color(GXChannelID chan, GXColor color);
void pc_gfx_init_light_pos(void* ltObj, f32 x, f32 y, f32 z);
void pc_gfx_init_light_dir(void* ltObj, f32 x, f32 y, f32 z);
void pc_gfx_init_light_color(void* ltObj, GXColor color);
void pc_gfx_init_light_attn(void* ltObj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2);
void pc_gfx_init_light_attn_a(void* ltObj, f32 a0, f32 a1, f32 a2);
void pc_gfx_init_light_attn_k(void* ltObj, f32 k0, f32 k1, f32 k2);
void pc_gfx_init_specular_dir(void* ltObj, f32 x, f32 y, f32 z);
void pc_gfx_load_light(void* ltObj, u32 lightMask);
void pc_gfx_set_fog(int enabled, float startZ, float endZ, float nearZ, float farZ, unsigned char r, unsigned char g, unsigned char b);
void pc_gfx_set_fog_allowed(int allowed);
void pc_gfx_set_tev_order(GXTevStageID stage, GXTexCoordID, GXTexMapID map, GXChannelID);
void pc_gfx_set_tex_coord_gen(GXTexCoordID coord, GXTexGenType type, GXTexGenSrc src, u32 matrixIdx);
void pc_gfx_load_tex_mtx(const Mtx m, u32 id);
void pc_gfx_load_tex_obj(GXTexObj* obj, GXTexMapID id);
void pc_gfx_init_tex_obj(GXTexObj*, void*, u16, u16, GXTexFmt, GXTexWrapMode, GXTexWrapMode, GXBool);
void pc_gfx_init_tex_obj_ci(GXTexObj*, void*, u16, u16, GXCITexFmt, GXTexWrapMode, GXTexWrapMode, GXBool, u32);
void pc_gfx_init_tex_obj_rgba(GXTexObj*, void*, u16, u16, GXTexWrapMode, GXTexWrapMode);
void pc_gfx_init_tlut_obj(GXTlutObj*, void*, GXTlutFmt, u16);
void pc_gfx_load_tlut(GXTlutObj*, u32);
void pc_gfx_release_texture(void*);
void pc_gfx_get_texture_stats(size_t*, size_t*, size_t*, size_t*, size_t*);
void pc_gfx_set_num_tev_stages(u8);
void pc_gfx_set_tev_color_in(GXTevStageID, GXTevColorArg, GXTevColorArg, GXTevColorArg, GXTevColorArg);
void pc_gfx_set_tev_alpha_in(GXTevStageID, GXTevAlphaArg, GXTevAlphaArg, GXTevAlphaArg, GXTevAlphaArg);
void pc_gfx_set_tev_color_op(GXTevStageID, GXTevOp, GXTevBias, GXTevScale, GXBool, GXTevRegID);
void pc_gfx_set_tev_alpha_op(GXTevStageID, GXTevOp, GXTevBias, GXTevScale, GXBool, GXTevRegID);
void pc_gfx_set_tev_color(GXTevRegID, GXColor);
void pc_gfx_set_tev_color_s10(GXTevRegID, GXColorS10);
void pc_gfx_set_tev_kcolor(GXTevKColorID, GXColor);
void pc_gfx_set_tev_kcolor_sel(GXTevStageID, GXTevKColorSel);
void pc_gfx_set_tev_kalpha_sel(GXTevStageID, GXTevKAlphaSel);
void pc_gfx_perf_scope_begin(const char*);
void pc_gfx_perf_scope_end(void);
void pc_gfx_clear_vtx_desc(void);
void pc_gfx_set_vtx_desc(GXAttr a, GXAttrType t);
void pc_gfx_set_vtx_attr_fmt(GXVtxFmt f, GXAttr a, GXCompCnt cnt, GXCompType type, u8 frac);
void pc_gfx_set_array(GXAttr a, void* base, u8 stride);
void pc_gfx_begin(GXPrimitive type, GXVtxFmt, u16);
void pc_gfx_position(f32 x, f32 y, f32 z);
void pc_gfx_color(u8 r, u8 g, u8 b, u8 a);
void pc_gfx_texcoord(f32 u, f32 v);
void pc_gfx_end(void);
void pc_gfx_call_display_list(const void* list, u32 nbytes);
}
