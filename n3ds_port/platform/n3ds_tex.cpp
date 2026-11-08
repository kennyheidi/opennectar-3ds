// Textures for the 3DS backend: the pc_gfx_init_tex_obj* / TLUT / release entry points, a decode cache, and upload.
//
// GXTexObj is an opaque 32-byte struct in the Dolphin SDK; the game only ever passes it back to us, so a small record is
// stored inside it (static_assert below). The record is copied out at GXLoadTexObj time, so temporaries are fine.
// Textures are decoded LAZILY on the first draw that uses them (many are initialised but never drawn), keyed by
// (data pointer, size, format, palette) and validated with a hash of 3 x 64 sampled bytes, so a heap block that the game
// frees and refills is re-decoded. Budget: N3DS_TEX_BUDGET_KB of linear memory, least-recently-used eviction, and a texture
// is only destroyed >= 3 frames after its last use (the GPU may still be reading it for two frames after the CPU is done).
#include "n3ds_bridge.h"
#include "n3ds_tex.h"
#include "n3ds_texconv.h"
#include "n3ds_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>

#ifndef N3DS_TEX_BUDGET_KB
#define N3DS_TEX_BUDGET_KB 10240
#endif
#ifndef N3DS_TEX_FLIPV
#define N3DS_TEX_FLIPV 0        /* 1: store rows bottom-up (if the first hardware run shows every texture upside down) */
#endif
#ifndef N3DS_TEX_DUMP
#define N3DS_TEX_DUMP 0         /* write the first N decoded textures as TGA to sdmc:/3ds/nectar/texdump/ (0 = off). Enable with tex_dump=N in nectar.ini.
                                   1k had this at 24 and wrote one pixel per fwrite(): ~35 us/px to the SD card = 21 s of the 1k startup stall. */
#endif
#ifndef N3DS_TEX_LOG
#define N3DS_TEX_LOG 40         /* log the first N texture creations */
#endif

using namespace n3dsconv;

namespace {

constexpr uint32_t kObjMagic = 0x58543344u;
constexpr uint8_t kTlutMagic = 0xA5;
struct ObjRec { const void* data; uint32_t magic, hash; uint16_t w, h; uint8_t fmt, wrapS, wrapT, pad; uint32_t tlutName; };
struct TlutRec { const void* data; uint32_t hash; uint16_t n; uint8_t fmt, magic; };   /* 12 bytes on ARM32 == sizeof(GXTlutObj) (3 x u32) */
static_assert(sizeof(ObjRec) <= sizeof(GXTexObj), "GXTexObj is smaller than the record n3ds_tex.cpp stores in it: switch to an address-keyed side table");
static_assert(sizeof(TlutRec) <= sizeof(GXTlutObj), "GXTlutObj is smaller than the record n3ds_tex.cpp stores in it");

TlutRec sTlut[32];
bool sTlutSet[32];

enum { ST_EMPTY = 0, ST_LIVE = 1, ST_FAILED = 2, ST_DYING = 3 };
struct Entry {
    const void* data; const void* pal;
    uint32_t hash, palHash, bytes, lastFrame, freeAt;    // freeAt: DYING = frame at which the GPU memory may go; FAILED = frame to retry
    uint16_t w, h, potW, potH;          // w,h = the ORIGINAL size (cache key); the stored image is (w + 2^shift - 1) >> shift wide, see sw/sh
    uint16_t sw, sh;
    uint8_t fmt, pica, state, shift;
    int gpuId;
    uint8_t gpuAvgL, gpuAvgA, gpuMaxL, gpuStat;   // r37 diagnostics: average / max of the CONVERTED GPU copy (intensity formats only; gpuStat 1 = measured)
};
constexpr int kSlots = 1024, kProbe = 8;
Entry sE[kSlots];
uint32_t sBytes = 0, sPeak = 0, sLive = 0, sMade = 0, sFreed = 0, sFailed = 0, sCreated = 0, sReleaseLogs = 0, sShrunkLogs = 0;
unsigned long long sDecodeTicks = 0;
unsigned sFrame = 0;

uint32_t fnv(const uint8_t* p, size_t n, uint32_t h) { for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u; return h; }
uint32_t sampleHash(const void* d, size_t size)
{
    const uint8_t* p = (const uint8_t*)d; uint32_t h = 2166136261u;
    if (!p) return 0;
    if (size <= 192) return fnv(p, size, h);
    h = fnv(p, 64, h); h = fnv(p + size / 2 - 32, 64, h); return fnv(p + size - 64, 64, h);
}

void initObj(GXTexObj* o, const void* data, int w, int h, int fmt, int ws, int wt, uint32_t tlutName)
{
    ObjRec r; memset(&r, 0, sizeof r);
    r.data = data; r.magic = kObjMagic; r.w = (uint16_t)w; r.h = (uint16_t)h; r.fmt = (uint8_t)fmt;
    r.wrapS = (uint8_t)ws; r.wrapT = (uint8_t)wt; r.tlutName = tlutName;
    const size_t sz = fmt == RAW_RGBA ? (size_t)w * h * 4 : gcDataSize(fmt, w, h);
    r.hash = sampleHash(data, sz);
    memset(o, 0, sizeof(GXTexObj));
    memcpy(o, &r, sizeof r);
}

uint32_t keyHash(const void* d, const void* pal, int w, int h, int fmt)
{
    uint32_t k = (uint32_t)((uintptr_t)d >> 2) * 2654435761u;
    k ^= (uint32_t)((uintptr_t)pal >> 2) * 40503u; k ^= (uint32_t)(w * 31 + h * 131 + fmt * 7);
    return k ^ (k >> 15);
}
int bucketOf(const N3dsTexRef& r) { return (int)(keyHash(r.data, r.pal, r.w, r.h, r.fmt) & (kSlots - 1) & ~(kProbe - 1)); }

int findEntry(const N3dsTexRef& r)
{
    const int home = bucketOf(r);
    for (int i = 0; i < kProbe; ++i) {
        const Entry& e = sE[home + i];
        if ((e.state == ST_LIVE || e.state == ST_FAILED) && e.data == r.data && e.pal == r.pal && e.w == r.w && e.h == r.h && e.fmt == r.fmt) return home + i;
    }
    return -1;
}

void destroyNow(int i)
{
    Entry& e = sE[i];
    if (e.gpuId) { n3ds_gpu_tex_destroy(e.gpuId); sBytes -= e.bytes; if (sLive) --sLive; ++sFreed; }
    memset(&e, 0, sizeof e);
}
void killDeferred(int i, unsigned frame)   // the GPU may still be reading it: destroy 3 frames after its last use
{
    Entry& e = sE[i];
    if (e.state == ST_LIVE) { e.state = ST_DYING; e.freeAt = (e.lastFrame > frame ? e.lastFrame : frame) + 3; }
    else destroyNow(i);
}

bool evictOldest(unsigned frame, int bucketLo, int bucketHi)   // oldest LIVE/FAILED entry not used in the last 3 frames, within [lo,hi)
{
    int best = -1; uint32_t bestF = 0xFFFFFFFFu;
    for (int i = bucketLo; i < bucketHi; ++i) {
        const Entry& e = sE[i];
        if (e.state != ST_LIVE && e.state != ST_FAILED) continue;
        if (e.lastFrame + 3 > frame) continue;
        if (e.lastFrame < bestF) { bestF = e.lastFrame; best = i; }
    }
    if (best < 0) return false;
    destroyNow(best);
    return true;
}

int freeSlot(const N3dsTexRef& r, unsigned frame)
{
    const int home = bucketOf(r);
    for (int i = 0; i < kProbe; ++i) if (sE[home + i].state == ST_EMPTY) return home + i;
    if (evictOldest(frame, home, home + kProbe)) for (int i = 0; i < kProbe; ++i) if (sE[home + i].state == ST_EMPTY) return home + i;
    return -1;
}

void dumpTga(int index, const N3dsTexRef& r, const Palette* pal)
{
    static bool made = false;
    if (!made) { mkdir("texdump", 0777); made = true; }
    if ((int)r.w > 512 || (int)r.h > 512) return;
    char path[96]; snprintf(path, sizeof path, "texdump/t%03d_%ux%u_f%u.tga", index, (unsigned)r.w, (unsigned)r.h, (unsigned)r.fmt);
    const size_t n = 18 + (size_t)r.w * r.h * 4;
    uint8_t* buf = (uint8_t*)malloc(n);                                     // ONE buffer, ONE fwrite: per-pixel fwrite to sdmc is very slow
    if (!buf) return;
    memset(buf, 0, 18); buf[2] = 2; buf[12] = (uint8_t)(r.w & 255); buf[13] = (uint8_t)(r.w >> 8); buf[14] = (uint8_t)(r.h & 255); buf[15] = (uint8_t)(r.h >> 8); buf[16] = 32; buf[17] = 0x28;   // top-left origin, 8 alpha bits
    uint8_t* o = buf + 18;
    for (int y = 0; y < r.h; ++y) for (int x = 0; x < r.w; ++x) {
        const uint32_t c = gcPixel(r.fmt, (const uint8_t*)r.data, r.w, r.h, x, y, pal);
        *o++ = (uint8_t)(c >> 16); *o++ = (uint8_t)(c >> 8); *o++ = (uint8_t)c; *o++ = (uint8_t)(c >> 24);   // B G R A
    }
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, n, f); fclose(f); }
    free(buf);
}

int ceilDiv(int a, int shift) { return (a + (1 << shift) - 1) >> shift; }

// How much to shrink a w x h texture (2^shift): the global nectar.ini shift, plus whatever it takes to get under tex_max_dim,
// but never below tex_min_dim on the larger side (small textures - fonts, icons - are left alone).
// Intensity / intensity+alpha formats are what every font sheet and text label is stored in. Their strokes are 1-2 texels wide at GameCube
// resolution (640x480), so shrinking them by 4x or more (which tex_max_dim=256 does to a 1024-wide sheet) leaves fragments, not letters
// (verified with tools/tex_roundtrip). They are cheap (16-bit LA8), so they are capped at 2x and never point-sampled.
bool textLike(int fmt) { return fmt == I4 || fmt == I8 || fmt == IA4 || fmt == IA8; }

int chooseShift(const N3dsConfig& c, int fmt, int w, int h)
{
    int sh = c.texShift;
    const int big = w > h ? w : h;
    int maxDim = c.texMaxDim;
    const bool text = textLike(fmt);
    if (text) { if (sh > 1) sh = 1; if (maxDim > 0 && maxDim < 512) maxDim = 512; }
    if (maxDim > 0) { int cap = 16; while (cap * 2 <= maxDim && cap < 1024) cap *= 2; while ((big >> sh) > cap && sh < 3) ++sh; }
    if (text) { int fit = 0; while ((big >> fit) > 1024 && fit < 3) ++fit; if (sh > 1 && sh > fit) sh = fit > 1 ? fit : 1; }   // only shrink text >2x if it cannot fit the GPU otherwise
    while (sh > 0 && (big >> sh) < c.texMinDim) --sh;
    return sh;
}

bool createTexture(int slot, const N3dsTexRef& r, unsigned frame)
{
    const N3dsConfig& cfg = n3dsConfig();
    static bool once = false;
    if (!once) { once = true; setLE16(cfg.texLE16 != 0); }
    const int budgetKb = cfg.texBudgetKb >= 0 ? cfg.texBudgetKb : N3DS_TEX_BUDGET_KB;
    const int logN = cfg.texLog >= 0 ? cfg.texLog : N3DS_TEX_LOG, dumpN = cfg.texDump >= 0 ? cfg.texDump : N3DS_TEX_DUMP;
    const unsigned long long t0 = n3ds_ticks();
    Palette pal{(const uint8_t*)r.pal, r.palFmt, r.palN};
    const bool ci = r.fmt == C4 || r.fmt == C8 || r.fmt == C14X2;
    const Palette* pp = ci ? &pal : nullptr;
    Entry e; memset(&e, 0, sizeof e);
    e.data = r.data; e.pal = r.pal; e.hash = r.hash; e.palHash = r.palHash; e.w = r.w; e.h = r.h; e.fmt = r.fmt; e.lastFrame = frame;
    e.shift = (uint8_t)chooseShift(cfg, r.fmt, r.w, r.h);
    const bool boxFilter = cfg.texBox != 0 || textLike(r.fmt);
    e.sw = (uint16_t)ceilDiv(r.w, e.shift); e.sh = (uint16_t)ceilDiv(r.h, e.shift);
    e.potW = (uint16_t)potSize(e.sw); e.potH = (uint16_t)potSize(e.sh);
    const Analysis an = analyse(r.fmt, (const uint8_t*)r.data, r.w, r.h, pp);
    e.pica = (uint8_t)chooseFormat(an, e.sw, e.sh);
    if (cfg.texForce16 && e.pica == P_RGBA8) e.pica = P_RGBA4;
    e.bytes = (uint32_t)picaBytes(e.pica, e.potW, e.potH);
    const unsigned long long t1 = n3ds_ticks();

    bool ok = e.potW <= 1024 && e.potH <= 1024 && r.w > 0 && r.h > 0 && gcDataSize(r.fmt, r.w, r.h) > 0;
    void* mem = nullptr; unsigned got = 0; int id = 0;
    const uint32_t budget = (uint32_t)budgetKb * 1024u;
    if (ok) {
        while (sBytes + e.bytes > budget && evictOldest(frame, 0, kSlots)) {}
        if (sBytes + e.bytes > budget) ok = false;
    }
    if (ok) {
        id = n3ds_gpu_tex_create(e.potW, e.potH, e.pica, &mem, &got);
        if (!id && evictOldest(frame, 0, kSlots)) id = n3ds_gpu_tex_create(e.potW, e.potH, e.pica, &mem, &got);
        ok = id != 0 && mem != nullptr && got >= e.bytes;
    }
    const unsigned long long t2 = n3ds_ticks();
    unsigned long long t3 = t2, t4 = t2;
    const bool diag = (int)sCreated < cfg.texDiag;
    if (ok) {
        convertScaled(r.fmt, (const uint8_t*)r.data, r.w, r.h, pp, e.pica, e.potW, e.potH, N3DS_TEX_FLIPV != 0, (uint8_t*)mem, e.shift, boxFilter);
        t3 = n3ds_ticks();
        if (diag)   // same work again on now-warm caches: a big gap between the two means the first pass was starved (other threads, cold memory)
            convertScaled(r.fmt, (const uint8_t*)r.data, r.w, r.h, pp, e.pica, e.potW, e.potH, N3DS_TEX_FLIPV != 0, (uint8_t*)mem, e.shift, boxFilter);
        t4 = n3ds_ticks();
        if (textLike(r.fmt) && (e.pica == P_LA8 || e.pica == P_L8)) {   /* r37: what the GPU will actually read (Wollywog body = black) */
            const uint8_t* q = (const uint8_t*)mem; const unsigned n = (unsigned)e.potW * e.potH; unsigned long sl = 0, sa = 0; unsigned ml = 0;
            for (unsigned t = 0; t < n; ++t) {
                const unsigned L = e.pica == P_LA8 ? q[t * 2 + 1] : q[t], A = e.pica == P_LA8 ? q[t * 2] : 255;
                sl += L; sa += A; if (L > ml) ml = L;
            }
            e.gpuAvgL = (uint8_t)(sl / n); e.gpuAvgA = (uint8_t)(sa / n); e.gpuMaxL = (uint8_t)ml; e.gpuStat = 1;
        }
        n3ds_gpu_tex_flush(id);
        e.gpuId = id; e.state = ST_LIVE; sBytes += e.bytes; if (sBytes > sPeak) sPeak = sBytes; ++sLive; ++sMade;
        sE[slot] = e;
    } else {
        if (id) n3ds_gpu_tex_destroy(id);
        e.state = ST_FAILED; e.freeAt = frame + 120; e.gpuId = 0; e.bytes = 0; ++sFailed; sE[slot] = e;
    }
    const unsigned long long tEnd = n3ds_ticks();
    const double ms = (double)(tEnd - t0) / N3DS_TICKS_PER_MS;
    sDecodeTicks += tEnd - t0;
    if ((int)sCreated < logN || !ok)
        n3ds_log("[tex] %s #%u: %ux%u gc-fmt %u data %p first bytes %02x%02x%02x%02x -> pica-fmt %d %ux%u (shift %d) %u KB in %.1f ms | pool %u KB, gpu free %u KB%s",
                 ok ? "created" : "FAILED", sCreated, (unsigned)r.w, (unsigned)r.h, (unsigned)r.fmt, r.data,
                 ((const uint8_t*)r.data)[0], ((const uint8_t*)r.data)[1], ((const uint8_t*)r.data)[2], ((const uint8_t*)r.data)[3],
                 e.pica, (unsigned)e.potW, (unsigned)e.potH, (int)e.shift, e.bytes / 1024, ms, sBytes / 1024, n3ds_gpu_tex_free_kb(), ok ? "" : " (draws untextured)");
    if (diag && ok)
        n3ds_log("[tex-diag] #%u phases (wall clock): analyse %.1f ms | alloc %.1f ms | convert %.1f ms | convert AGAIN (warm) %.1f ms | flush %.1f ms",
                 sCreated, (double)(t1 - t0) / N3DS_TICKS_PER_MS, (double)(t2 - t1) / N3DS_TICKS_PER_MS, (double)(t3 - t2) / N3DS_TICKS_PER_MS,
                 (double)(t4 - t3) / N3DS_TICKS_PER_MS, (double)(tEnd - t4) / N3DS_TICKS_PER_MS);
    if (ok && (int)sCreated < dumpN) {
        const unsigned long long d0 = n3ds_ticks();
        dumpTga((int)sCreated, r, pp);
        n3ds_log("[tex] dumped #%u to texdump/ in %.1f ms (not part of the decode time above)", sCreated, (double)(n3ds_ticks() - d0) / N3DS_TICKS_PER_MS);
    }
    if (ok && e.shift > 0 && textLike(r.fmt) && sShrunkLogs < 20) { ++sShrunkLogs; n3ds_log("[tex] text/label texture %ux%u (gc-fmt %u) stored at %ux%u (shrunk %dx, %s)", (unsigned)r.w, (unsigned)r.h, (unsigned)r.fmt, (unsigned)e.sw, (unsigned)e.sh, 1 << e.shift, boxFilter ? "averaged" : "point-sampled"); }
    ++sCreated;
    return ok;
}

}  // namespace

bool n3dstexCapture(const GXTexObj* obj, N3dsTexRef* out)
{
    ObjRec r; memcpy(&r, obj, sizeof r);
    out->valid = false;
    if (r.magic != kObjMagic || !r.data || !r.w || !r.h) return false;
    out->data = r.data; out->hash = r.hash; out->w = r.w; out->h = r.h; out->fmt = r.fmt; out->wrapS = r.wrapS; out->wrapT = r.wrapT;
    out->pal = nullptr; out->palHash = 0; out->palN = 0; out->palFmt = 0;
    if (r.fmt == C4 || r.fmt == C8 || r.fmt == C14X2) {
        const int n = (int)(r.tlutName & 31);
        if (!sTlutSet[n]) return false;                                    // no palette loaded: cannot decode
        out->pal = sTlut[n].data; out->palHash = sTlut[n].hash; out->palN = sTlut[n].n; out->palFmt = sTlut[n].fmt;
    }
    out->valid = true;
    return true;
}

N3dsTexBound n3dstexResolve(const N3dsTexRef& r, unsigned frame)
{
    N3dsTexBound b; b.id = 0; b.su = b.sv = 1.0f; b.ok = false;
    if (!r.valid || n3dsConfig().texOff) return b;
    int i = findEntry(r);
    if (i >= 0) {
        Entry& e = sE[i];
        if (e.state == ST_FAILED) { if (frame < e.freeAt) return b; destroyNow(i); i = -1; }
        else if (e.hash != r.hash || e.palHash != r.palHash) { killDeferred(i, frame); i = -1; }   // same address, new pixels
        else { e.lastFrame = frame; b.id = (uint16_t)e.gpuId; b.su = (float)e.sw / e.potW; b.sv = (float)e.sh / e.potH; b.ok = true; return b; }
    }
    const int slot = freeSlot(r, frame);
    if (slot < 0) return b;
    if (!createTexture(slot, r, frame)) return b;
    const Entry& e = sE[slot];
    b.id = (uint16_t)e.gpuId; b.su = (float)e.sw / e.potW; b.sv = (float)e.sh / e.potH; b.ok = true;
    return b;
}

void n3dstexFrame(unsigned frame)
{
    sFrame = frame;
    for (int i = 0; i < kSlots; ++i)
        if (sE[i].state == ST_DYING && frame >= sE[i].freeAt) destroyNow(i);
}

void n3dstexLogStats(unsigned frame)
{
    n3ds_log("[tex] frame %u: %u live, %u KB in pool (peak %u KB, budget %u KB), made %u, freed %u, failed %u, decode time so far %.0f ms | gpu free %u KB",
             frame, sLive, sBytes / 1024, sPeak / 1024, (unsigned)N3DS_TEX_BUDGET_KB, sMade, sFreed, sFailed,
             (double)sDecodeTicks / N3DS_TICKS_PER_MS, n3ds_gpu_tex_free_kb());
}

/* r37 diagnostics (black Wollywog / Puffy Blowhog): everything about one bound texture in one line - the GC data as it is NOW (min / average /
   max intensity and alpha, decoded texel by texel), whether its bytes changed since GXInitTexObj hashed them, and the cache entry with the
   average of the converted copy the GPU samples. */
void n3dstexDescribe(const N3dsTexRef& r, char* out, size_t n)
{
    if (!out || !n) return;
    out[0] = 0;
    if (!r.valid || !r.data) { snprintf(out, n, "invalid texture"); return; }
    Palette pal{(const uint8_t*)r.pal, r.palFmt, r.palN};
    const bool ci = r.fmt == C4 || r.fmt == C8 || r.fmt == C14X2;
    if (ci && !r.pal) { snprintf(out, n, "CI texture without palette"); return; }
    const int sx = r.w > 128 ? r.w / 128 : 1, sy = r.h > 128 ? r.h / 128 : 1;
    unsigned mnC = 255, mxC = 0, mnA = 255, mxA = 0, cnt = 0; unsigned long sC = 0, sA = 0;
    for (int y = 0; y < r.h; y += sy) for (int x = 0; x < r.w; x += sx) {
        const uint32_t c = gcPixel(r.fmt, (const uint8_t*)r.data, r.w, r.h, x, y, ci ? &pal : nullptr);
        const unsigned i = ((c & 255) + ((c >> 8) & 255) + ((c >> 16) & 255)) / 3, a = c >> 24;
        if (i < mnC) mnC = i; if (i > mxC) mxC = i; if (a < mnA) mnA = a; if (a > mxA) mxA = a; sC += i; sA += a; ++cnt;
    }
    const size_t sz = r.fmt == RAW_RGBA ? (size_t)r.w * r.h * 4 : gcDataSize(r.fmt, r.w, r.h);
    const uint32_t hNow = sampleHash(r.data, sz);
    const int i = findEntry(r);
    char eb[160] = "not in the cache";
    if (i >= 0) {
        const Entry& e = sE[i];
        snprintf(eb, sizeof eb, "cache: state %u gpu %d pica-fmt %u %ux%u shift %u, hash %s%s", (unsigned)e.state, e.gpuId, (unsigned)e.pica, (unsigned)e.potW, (unsigned)e.potH,
                 (unsigned)e.shift, e.hash == r.hash ? "same" : "DIFFERENT", e.gpuStat ? "" : "");
        if (e.gpuStat) { const size_t l = strlen(eb); snprintf(eb + l, sizeof eb - l, ", GPU copy avg I %u max I %u avg A %u", (unsigned)e.gpuAvgL, (unsigned)e.gpuMaxL, (unsigned)e.gpuAvgA); }
    }
    snprintf(out, n, "%ux%u gc-fmt %u data %p | GC data now: I min %u avg %lu max %u, A min %u avg %lu max %u | bytes %s since GXInitTexObj | %s",
             (unsigned)r.w, (unsigned)r.h, (unsigned)r.fmt, r.data, mnC, cnt ? sC / cnt : 0ul, mxC, mnA, cnt ? sA / cnt : 0ul, mxA,
             hNow == r.hash ? "unchanged" : "CHANGED", eb);
}

/* The GC texel a (u, v) coordinate lands on (nearest, with the texture's wrap modes), as 0xAABBGGRR. */
uint32_t n3dstexSampleGc(const N3dsTexRef& r, float u, float v)
{
    if (!r.valid || !r.data || !r.w || !r.h) return 0;
    Palette pal{(const uint8_t*)r.pal, r.palFmt, r.palN};
    const bool ci = r.fmt == C4 || r.fmt == C8 || r.fmt == C14X2;
    if (ci && !r.pal) return 0;
    auto wrap = [](float t, int size, int mode) -> int {
        int k = (int)floorf(t * (float)size);
        if (mode == 1) { k %= size; if (k < 0) k += size; }
        else if (mode == 2) { const int p = 2 * size; k %= p; if (k < 0) k += p; if (k >= size) k = p - 1 - k; }
        else { if (k < 0) k = 0; if (k >= size) k = size - 1; }
        return k;
    };
    return gcPixel(r.fmt, (const uint8_t*)r.data, r.w, r.h, wrap(u, r.w, r.wrapS), wrap(v, r.h, r.wrapT), ci ? &pal : nullptr);
}

extern "C" {

void pc_gfx_init_tex_obj(GXTexObj* o, void* data, u16 w, u16 h, GXTexFmt fmt, GXTexWrapMode ws, GXTexWrapMode wt, GXBool)
{
    initObj(o, data, w, h, (int)fmt, (int)ws, (int)wt, 0);
}
void pc_gfx_init_tex_obj_ci(GXTexObj* o, void* data, u16 w, u16 h, GXCITexFmt fmt, GXTexWrapMode ws, GXTexWrapMode wt, GXBool, u32 tlutName)
{
    initObj(o, data, w, h, (int)fmt, (int)ws, (int)wt, tlutName);
}
void pc_gfx_init_tex_obj_rgba(GXTexObj* o, void* data, u16 w, u16 h, GXTexWrapMode ws, GXTexWrapMode wt)
{
    initObj(o, data, w, h, RAW_RGBA, (int)ws, (int)wt, 0);
}
void pc_gfx_init_tlut_obj(GXTlutObj* o, void* lut, GXTlutFmt fmt, u16 n)
{
    TlutRec r; memset(&r, 0, sizeof r);
    r.data = lut; r.magic = kTlutMagic; r.fmt = (uint8_t)(int)fmt; r.n = n; r.hash = sampleHash(lut, (size_t)n * 2);
    memset(o, 0, sizeof(GXTlutObj)); memcpy(o, &r, sizeof r);
}
void n3dstex_rp_load_tlut(GXTlutObj* o, u32 name)
{
    TlutRec r; memcpy(&r, o, sizeof r);
    if (r.magic != kTlutMagic) return;
    sTlut[name & 31] = r; sTlutSet[name & 31] = true;
}
void n3dstex_rp_release_texture(void* p)
{
    unsigned hits = 0;
    for (int i = 0; i < kSlots; ++i)
        if ((sE[i].state == ST_LIVE || sE[i].state == ST_FAILED) && sE[i].data == p) { killDeferred(i, sFrame); ++hits; }
    if (sReleaseLogs < 6) { ++sReleaseLogs; n3ds_log("[tex] release_texture(%p): %u cache entries matched (0 on every call would mean the argument is not the pixel-data pointer)", p, hits); }
}
void pc_gfx_get_texture_stats(size_t* live, size_t* bytes, size_t* peak, size_t* made, size_t* freed)
{
    if (live) *live = sLive;
    if (bytes) *bytes = sBytes;
    if (peak) *peak = sPeak;
    if (made) *made = sMade;
    if (freed) *freed = sFreed;
}

}  // extern "C"
