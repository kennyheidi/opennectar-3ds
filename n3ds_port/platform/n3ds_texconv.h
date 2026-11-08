// GameCube texture formats -> PICA200 texture layout. Pure functions (no libctru, no game headers) so the host test can
// exercise them with a plain g++. All GC data is the raw big-endian blob as stored on the disc (tiled blocks).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace n3dsconv {

enum GcFmt { I4 = 0, I8 = 1, IA4 = 2, IA8 = 3, RGB565 = 4, RGB5A3 = 5, RGBA8 = 6, C4 = 8, C8 = 9, C14X2 = 10, CMPR = 14,
             RAW_RGBA = 0x40 /* pc_gfx_init_tex_obj_rgba: tightly packed r,g,b,a bytes, row-major */ };
enum PicaFmt { P_RGBA8 = 0, P_RGB565 = 1, P_RGBA5551 = 2, P_RGBA4 = 3, P_LA8 = 4, P_L8 = 5 };   // == N3DS_TF_*

// Debug switch: treat 16-bit texel / palette words as little-endian (only if the game byte-swaps texture data on load).
void setLE16(bool le);

struct Palette { const uint8_t* data; int fmt; /* 0 IA8, 1 RGB565, 2 RGB5A3 */ int n; };

// One texel as 0xAABBGGRR (r in the low byte). x < w, y < h.
uint32_t gcPixel(int fmt, const uint8_t* d, int w, int h, int x, int y, const Palette* pal);

// Bytes of the level-0 image as stored on disc (blocks are padded to whole blocks); 0 for an unknown format.
size_t gcDataSize(int fmt, int w, int h);

struct Analysis { bool alpha; bool translucent; bool gray; };
// Cheap content scan (reads the raw blob / palette, does not decode every texel) used to pick the smallest exact PICA format.
Analysis analyse(int fmt, const uint8_t* d, int w, int h, const Palette* pal);
int chooseFormat(const Analysis& a, int w, int h);

int bytesPerTexel(int pica);
int potSize(int n);                                   // next power of two, at least 8
size_t picaBytes(int pica, int potW, int potH);

// Decode + swizzle into out (picaBytes(...) long). The image sits in the top-left of the pot x pot area; the remainder
// replicates the last row/column so CLAMP_TO_EDGE and bilinear filtering stay clean. flipRows stores memory rows bottom-up.
void convert(int gcFmt, const uint8_t* d, int w, int h, const Palette* pal, int pica, int potW, int potH, bool flipRows, uint8_t* out);

// Same, but the image is first shrunk by 2^shift (shift 0..3; potW/potH are the pot size of the SHRUNK image). box: average the
// source block (quality) instead of point-sampling its centre (about 4^shift times fewer source texels decoded).
void convertScaled(int gcFmt, const uint8_t* d, int w, int h, const Palette* pal, int pica, int potW, int potH, bool flipRows, uint8_t* out, int shift, bool box);

}  // namespace n3dsconv
