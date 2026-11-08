#include "n3ds_texconv.h"
#include <string.h>
#include <stdlib.h>

namespace n3dsconv {
namespace {

inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
bool gLE16 = false;
inline uint32_t be16(const uint8_t* p) { return gLE16 ? ((uint32_t)p[1] << 8 | p[0]) : ((uint32_t)p[0] << 8 | p[1]); }
inline uint32_t x5(uint32_t v) { return (v << 3) | (v >> 2); }
inline uint32_t x6(uint32_t v) { return (v << 2) | (v >> 4); }
inline uint32_t c565(uint32_t v) { return rgba(x5(v >> 11), x6((v >> 5) & 63), x5(v & 31), 255); }
inline uint32_t c5a3(uint32_t v)
{
    if (v & 0x8000) return rgba(x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), 255);
    const uint32_t a = (v >> 12) & 7;
    return rgba(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, (a << 5) | (a << 2) | (a >> 1));
}
inline uint32_t cia8(uint32_t v) { const uint32_t a = v >> 8, i = v & 255; return rgba(i, i, i, a); }

uint32_t palLookup(const Palette* p, uint32_t idx)
{
    if (!p || !p->data || (int)idx >= p->n) return 0;
    const uint32_t v = be16(p->data + idx * 2);
    return p->fmt == 0 ? cia8(v) : p->fmt == 1 ? c565(v) : c5a3(v);
}

// Block size in texels as shifts (no division: ARMv6 has no divide instruction) and bytes per block.
bool geometry(int fmt, int& bwS, int& bhS, int& bytes)
{
    switch (fmt) {
    case I4: case C4: case CMPR: bwS = 3; bhS = 3; bytes = 32; return true;
    case I8: case IA4: case C8: bwS = 3; bhS = 2; bytes = 32; return true;
    case IA8: case RGB565: case RGB5A3: case C14X2: bwS = 2; bhS = 2; bytes = 32; return true;
    case RGBA8: bwS = 2; bhS = 2; bytes = 64; return true;
    default: return false;
    }
}

uint32_t cmprPixel(const uint8_t* blk, int lx, int ly)
{
    const uint8_t* sub = blk + (((ly >> 2) << 1) + (lx >> 2)) * 8;
    const uint32_t v0 = be16(sub), v1 = be16(sub + 2);
    const int px = lx & 3, py = ly & 3;
    const int idx = (sub[4 + py] >> (6 - 2 * px)) & 3;
    const uint32_t a = c565(v0), b = c565(v1);
    if (idx == 0) return a;
    if (idx == 1) return b;
    uint32_t out[3];
    for (int c = 0; c < 3; ++c) {
        const uint32_t ca = (a >> (8 * c)) & 255, cb = (b >> (8 * c)) & 255;
        uint32_t m;
        if (v0 > v1) m = idx == 2 ? (2 * ca + cb) / 3 : (ca + 2 * cb) / 3;
        else m = (ca + cb) / 2;                                           // idx 2, or idx 3 (transparent, keeps the average colour)
        out[c] = m;
    }
    const bool transparent = !(v0 > v1) && idx == 3;
    return rgba(out[0], out[1], out[2], transparent ? 0 : 255);
}

}  // namespace

void setLE16(bool le) { gLE16 = le; }

size_t gcDataSize(int fmt, int w, int h)
{
    if (fmt == RAW_RGBA) return (size_t)w * h * 4;
    int bwS, bhS, bytes;
    if (!geometry(fmt, bwS, bhS, bytes)) return 0;
    const size_t bpr = ((size_t)w + (1u << bwS) - 1) >> bwS, rows = ((size_t)h + (1u << bhS) - 1) >> bhS;
    return bpr * rows * bytes;
}

uint32_t gcPixel(int fmt, const uint8_t* d, int w, int h, int x, int y, const Palette* pal)
{
    (void)h;
    if (fmt == RAW_RGBA) { const uint8_t* p = d + ((size_t)y * w + x) * 4; return rgba(p[0], p[1], p[2], p[3]); }
    int bwS, bhS, bytes;
    if (!geometry(fmt, bwS, bhS, bytes)) return 0;
    const int bpr = (w + (1 << bwS) - 1) >> bwS;
    const uint8_t* blk = d + ((size_t)(y >> bhS) * bpr + (x >> bwS)) * bytes;
    const int lx = x & ((1 << bwS) - 1), ly = y & ((1 << bhS) - 1);
    switch (fmt) {
    case I4: { const uint32_t b = blk[ly * 4 + (lx >> 1)], n = (lx & 1) ? (b & 15) : (b >> 4), i = n * 17; return rgba(i, i, i, i); }
    case I8: { const uint32_t i = blk[ly * 8 + lx]; return rgba(i, i, i, i); }
    case IA4: { const uint32_t b = blk[ly * 8 + lx], a = (b >> 4) * 17, i = (b & 15) * 17; return rgba(i, i, i, a); }
    case IA8: return cia8(be16(blk + (ly * 4 + lx) * 2));
    case RGB565: return c565(be16(blk + (ly * 4 + lx) * 2));
    case RGB5A3: return c5a3(be16(blk + (ly * 4 + lx) * 2));
    case RGBA8: { const int i = ly * 4 + lx; return rgba(blk[i * 2 + 1], blk[32 + i * 2], blk[32 + i * 2 + 1], blk[i * 2]); }
    case C4: { const uint32_t b = blk[ly * 4 + (lx >> 1)]; return palLookup(pal, (lx & 1) ? (b & 15) : (b >> 4)); }
    case C8: return palLookup(pal, blk[ly * 8 + lx]);
    case C14X2: return palLookup(pal, be16(blk + (ly * 4 + lx) * 2) & 0x3FFF);
    case CMPR: return cmprPixel(blk, lx, ly);
    }
    return 0;
}

static void palAnalysis(const Palette* p, Analysis& a)
{
    a.alpha = a.translucent = false; a.gray = true;
    if (!p || !p->data) return;
    const int n = p->n > 16384 ? 16384 : p->n;
    for (int i = 0; i < n; ++i) {
        const uint32_t c = palLookup(p, (uint32_t)i), al = c >> 24;
        const int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255;
        if (al != 255) { a.alpha = true; if (al != 0) a.translucent = true; }
        if (r - g > 3 || g - r > 3 || g - b > 3 || b - g > 3) a.gray = false;
    }
}

Analysis analyse(int fmt, const uint8_t* d, int w, int h, const Palette* pal)
{
    Analysis a; a.alpha = a.translucent = false; a.gray = false;
    switch (fmt) {
    case I4: case I8: case IA4: case IA8: a.alpha = a.translucent = a.gray = true; return a;   // alpha == intensity (I) or explicit
    case RGB565: return a;
    case C4: case C8: case C14X2: palAnalysis(pal, a); return a;
    case RGBA8: case RGB5A3: case RAW_RGBA: {
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const uint32_t al = gcPixel(fmt, d, w, h, x, y, pal) >> 24;
            if (al != 255) { a.alpha = true; if (al != 0) a.translucent = true; }
        }
        return a;
    }
    case CMPR: {
        const size_t bytes = gcDataSize(CMPR, w, h);
        for (size_t o = 0; o + 8 <= bytes; o += 8) {
            if (be16(d + o) > be16(d + o + 2)) continue;                    // 4-colour mode: opaque
            for (int r = 0; r < 4; ++r) {
                const uint8_t b = d[o + 4 + r];
                if ((b & 3) == 3 || ((b >> 2) & 3) == 3 || ((b >> 4) & 3) == 3 || ((b >> 6) & 3) == 3) { a.alpha = true; return a; }
            }
        }
        return a;
    }
    }
    return a;
}

int chooseFormat(const Analysis& a, int w, int h)
{
    if (!a.alpha) return a.gray ? P_L8 : P_RGB565;
    if (a.gray) return P_LA8;
    if (!a.translucent) return P_RGBA5551;
    return (w * h <= 128 * 128) ? P_RGBA8 : P_RGBA4;
}

int bytesPerTexel(int pica) { return pica == P_RGBA8 ? 4 : pica == P_L8 ? 1 : 2; }
int potSize(int n) { int p = 8; while (p < n) p <<= 1; return p; }
size_t picaBytes(int pica, int potW, int potH) { return (size_t)potW * potH * bytesPerTexel(pica); }

// One texel of the image shrunk by 2^shift: point sample (centre of the source block) or box average of the block.
static uint32_t scaledPixel(int fmt, const uint8_t* d, int w, int h, int sx, int sy, const Palette* pal, int shift, bool box)
{
    const int n = 1 << shift;
    if (shift == 0) return gcPixel(fmt, d, w, h, sx, sy, pal);
    const int x0 = sx << shift, y0 = sy << shift;
    if (!box) {
        int x = x0 + (n >> 1), y = y0 + (n >> 1);
        if (x >= w) x = w - 1;
        if (y >= h) y = h - 1;
        return gcPixel(fmt, d, w, h, x, y, pal);
    }
    // Alpha-WEIGHTED average of the colour: a fully transparent texel carries no colour. Without the weights, glyph edges (white
    // text on transparent black, i.e. every I4/IA4/IA8 font and label) blend towards black and come out dark and dim.
    uint32_t r = 0, g = 0, b = 0, a = 0, pr = 0, pg = 0, pb = 0, cnt = 0;
    for (int yy = y0; yy < y0 + n && yy < h; ++yy)
        for (int xx = x0; xx < x0 + n && xx < w; ++xx) {
            const uint32_t c = gcPixel(fmt, d, w, h, xx, yy, pal), ca = c >> 24;
            r += (c & 255) * ca; g += ((c >> 8) & 255) * ca; b += ((c >> 16) & 255) * ca; a += ca;
            pr += c & 255; pg += (c >> 8) & 255; pb += (c >> 16) & 255; ++cnt;
        }
    if (!cnt) return gcPixel(fmt, d, w, h, w - 1, h - 1, pal);
    if (!a) return rgba(pr / cnt, pg / cnt, pb / cnt, 0);                 // wholly transparent block: any colour; keep the plain average
    return rgba(r / a, g / a, b / a, a / cnt);
}


// ---- fast path for CMPR (DXT1-style, gc-fmt 14: every big world texture) ----------------------------------------------------------
// Log af7acc26: a 1024x1024 CMPR texture took 270 ms to create (512x512: 90 ms) because every output texel called gcPixel() once per
// source texel (16 per texel at shift 2) and each call re-derived the block geometry and the 4-colour palette. Here each 4x4 sub-block's
// palette is built once, whole strips of source rows are decoded into an RGBA line buffer, and the box average is a plain sum with a shift
// for opaque, fully-inside regions. The result is bit-identical to scaledPixel()/gcPixel() (host test tools/host_test/texfast_test.cpp).
uint32_t cmprLerp(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t div)
{
    const uint32_t r = (wa * (a & 255) + wb * (b & 255)) / div, g = (wa * ((a >> 8) & 255) + wb * ((b >> 8) & 255)) / div, bl = (wa * ((a >> 16) & 255) + wb * ((b >> 16) & 255)) / div;
    return rgba(r, g, bl, 255);
}

// Decode source rows [y0, y0 + rows) (y0 and rows multiples of 4, clipped to h) into buf (stride w).
void decodeCmprStrip(const uint8_t* d, int w, int h, int y0, int rows, uint32_t* buf)
{
    const int bpr = (w + 7) >> 3;
    for (int sy = y0; sy < y0 + rows && sy < h; sy += 4) {
        const int by = sy >> 3, subRow = (sy >> 2) & 1;
        for (int bx = 0; bx < bpr; ++bx) {
            const uint8_t* blk = d + ((size_t)by * bpr + bx) * 32;
            for (int sc = 0; sc < 2; ++sc) {
                const int x = bx * 8 + sc * 4;
                if (x >= w) break;
                const uint8_t* sub = blk + (subRow * 2 + sc) * 8;
                const uint32_t v0 = be16(sub), v1 = be16(sub + 2), a = c565(v0), b = c565(v1);
                uint32_t pal[4]; pal[0] = a; pal[1] = b;
                if (v0 > v1) { pal[2] = cmprLerp(a, b, 2, 1, 3); pal[3] = cmprLerp(a, b, 1, 2, 3); }
                else { pal[2] = cmprLerp(a, b, 1, 1, 2); pal[3] = pal[2] & 0x00FFFFFFu; }          // idx 3 = transparent, keeps the average colour
                const int nx = w - x < 4 ? w - x : 4;
                for (int py = 0; py < 4; ++py) {
                    const int row = sy + py;
                    if (row >= h) break;
                    const uint32_t ib = sub[4 + py];
                    uint32_t* o = buf + (size_t)(row - y0) * w + x;
                    for (int px = 0; px < nx; ++px) o[px] = pal[(ib >> (6 - 2 * px)) & 3];
                }
            }
        }
    }
}

// The whole image shrunk by 2^shift (box average, alpha-weighted exactly like scaledPixel) into dst (sw x sh). false: not applicable / no memory.
uint32_t* sStrip = nullptr; size_t sStripCap = 0;
bool shrinkCmpr(const uint8_t* d, int w, int h, int shift, uint32_t* dst, int sw)
{
    const int n = 1 << shift, H = n < 4 ? 4 : n;
    if (w > 2048) return false;
    const size_t need = (size_t)w * H;
    if (sStripCap < need) {
        uint32_t* nb = (uint32_t*)realloc(sStrip, need * 4);
        if (!nb) return false;
        sStrip = nb; sStripCap = need;
    }
    const int sh2 = 2 * shift;
    for (int y0 = 0; y0 < h; y0 += H) {
        decodeCmprStrip(d, w, h, y0, H, sStrip);
        for (int oyl = 0; oyl < H / n; ++oyl) {
            const int sy0 = y0 + oyl * n;
            if (sy0 >= h) break;
            const int oy = sy0 >> shift, nr = h - sy0 < n ? h - sy0 : n;
            const uint32_t* rows = sStrip + (size_t)oyl * n * w;
            uint32_t* out = dst + (size_t)oy * sw;
            if (n == 1) { memcpy(out, rows, (size_t)w * 4); continue; }
            for (int ox = 0; ox < sw; ++ox) {
                const int sx0 = ox << shift, nc = w - sx0 < n ? w - sx0 : n;
                uint32_t pr = 0, pg = 0, pb = 0, andA = 255;
                for (int yy = 0; yy < nr; ++yy) {
                    const uint32_t* px = rows + (size_t)yy * w + sx0;
                    for (int xx = 0; xx < nc; ++xx) { const uint32_t c = px[xx]; pr += c & 255; pg += (c >> 8) & 255; pb += (c >> 16) & 255; andA &= c >> 24; }
                }
                const uint32_t cnt = (uint32_t)(nr * nc);
                if (andA == 255) {                                              // opaque region: the alpha-weighted mean equals the plain mean
                    if (nr == n && nc == n) out[ox] = rgba(pr >> sh2, pg >> sh2, pb >> sh2, 255);
                    else out[ox] = rgba(pr / cnt, pg / cnt, pb / cnt, 255);
                    continue;
                }
                uint32_t r = 0, g = 0, b = 0, a = 0;
                for (int yy = 0; yy < nr; ++yy) {
                    const uint32_t* px = rows + (size_t)yy * w + sx0;
                    for (int xx = 0; xx < nc; ++xx) { const uint32_t c = px[xx], ca = c >> 24; r += (c & 255) * ca; g += ((c >> 8) & 255) * ca; b += ((c >> 16) & 255) * ca; a += ca; }
                }
                out[ox] = a ? rgba(r / a, g / a, b / a, a / cnt) : rgba(pr / cnt, pg / cnt, pb / cnt, 0);
            }
        }
    }
    return true;
}

void convertScaled(int fmt, const uint8_t* d, int w, int h, const Palette* pal, int pica, int potW, int potH, bool flipRows, uint8_t* out, int shift, bool box)
{
    static uint8_t mx[64], my[64];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 64; ++i) {                                       // Z-order inside an 8x8 tile: x0 y0 x1 y1 x2 y2
            mx[i] = (uint8_t)((i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4));
            my[i] = (uint8_t)(((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4));
        }
        init = true;
    }
    const int sw = (w + (1 << shift) - 1) >> shift, sh = (h + (1 << shift) - 1) >> shift;   // the shrunk image; it sits in the top-left of the pot area
    const int bpp = bytesPerTexel(pica), tilesX = potW >> 3, tilesY = potH >> 3;
    // CMPR fast path (see shrinkCmpr): the shrunk image is built once, then only swizzled/packed below. Falls back to the per-texel path when the
    // shrunk image would not fit the 512 KB scratch budget or memory is short.
    uint32_t* fastImg = nullptr;
    if (fmt == CMPR && (shift == 0 || box) && (size_t)sw * sh * 4 <= 512 * 1024) {
        fastImg = (uint32_t*)malloc((size_t)sw * sh * 4);
        if (fastImg && !shrinkCmpr(d, w, h, shift, fastImg, sw)) { free(fastImg); fastImg = nullptr; }
    }
    for (int ty = 0; ty < tilesY; ++ty) for (int tx = 0; tx < tilesX; ++tx) {
        uint8_t* tile = out + ((size_t)ty * tilesX + tx) * 64 * bpp;
        for (int i = 0; i < 64; ++i) {
            const int xm = tx * 8 + mx[i], ym = ty * 8 + my[i];
            int sy = flipRows ? potH - 1 - ym : ym, sx = xm;
            if (sx >= sw) sx = sw - 1;
            if (sy >= sh) sy = sh - 1;
            const uint32_t c = fastImg ? fastImg[(size_t)sy * sw + sx] : scaledPixel(fmt, d, w, h, sx, sy, pal, shift, box);
            const uint32_t r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;
            uint8_t* o = tile + i * bpp;
            switch (pica) {
            case P_RGBA8: o[0] = (uint8_t)a; o[1] = (uint8_t)b; o[2] = (uint8_t)g; o[3] = (uint8_t)r; break;
            case P_RGB565: { const uint32_t v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3); o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); break; }
            case P_RGBA5551: { const uint32_t v = (r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >= 128 ? 1u : 0u); o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); break; }
            case P_RGBA4: { const uint32_t v = (r >> 4) << 12 | (g >> 4) << 8 | (b >> 4) << 4 | (a >> 4); o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); break; }
            case P_LA8: o[0] = (uint8_t)a; o[1] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); break;   // 16-bit value = L << 8 | A
            default: o[0] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); break;                          // P_L8
            }
        }
    }
    free(fastImg);
}

void convert(int fmt, const uint8_t* d, int w, int h, const Palette* pal, int pica, int potW, int potH, bool flipRows, uint8_t* out)
{
    convertScaled(fmt, d, w, h, pal, pica, potW, potH, flipRows, out, 0, false);
}

}  // namespace n3dsconv
