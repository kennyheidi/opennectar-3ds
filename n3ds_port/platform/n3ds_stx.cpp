// Background STX decoder: replaces upstream's pc_audio_play_stx() decode-everything-then-resample-everything path on the 3DS.
// Upstream reads the whole .stx, decodes ALL of it to 44.1 kHz PCM (~10 MB for opening.stx), copies it into an SDL_AudioStream and
// resamples it into a third buffer (~7 MB): ~27 MB of transient allocations (the malloc heap overflows into the linear heap, which
// is what textures and meshes need) and a multi-second stall of the game thread whenever a cutscene starts.
// Here the game thread only reads the 32-byte header and allocates the (zeroed) output buffer; this thread reads the file in 4.6 KB
// steps, decodes the ADPCM, resamples with exactly the same fixed-point linear interpolation as the SDL shim in n3ds_sdl_audio.cpp
// and writes straight into the final buffer, far ahead of the play cursor (decoding is ~50x faster than real time).
// The output is bit-identical to the old path (tools/host_test/test_stx.cpp checks that).
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#ifdef __3DS__
#include <3ds.h>
#else
#include <thread>
#include <chrono>
#endif

extern "C" void n3ds_log(const char* fmt, ...);
extern "C" void n3ds_note(const char* what);
extern "C" void n3ds_vfp_runfast(void);

namespace {
inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
inline uint32_t be16(const uint8_t* p) { return ((uint32_t)p[0] << 8) | p[1]; }

struct Hdr { uint32_t inFrames, srcRate; };

// Same validation and frame count as upstream: 18-byte blocks (2 channels x 9 bytes), format 4 = ADPCM.
bool readHeader(FILE* f, Hdr* h)
{
    uint8_t hd[32];
    if (fseek(f, 0, SEEK_END) != 0) return false;
    const long size = ftell(f);
    if (size < 32 || fseek(f, 0, SEEK_SET) != 0 || fread(hd, 1, 32, f) != 32) return false;
    const uint32_t encodedSize = be32(hd), sampleCount = be32(hd + 4), rate = be16(hd + 8), format = be16(hd + 10);
    const size_t body = std::min<size_t>(encodedSize, (size_t)size - 32);
    if (format != 4 || rate == 0 || body < 18) return false;
    h->inFrames = (uint32_t)std::min<size_t>(sampleCount, (body / 18) * 16);
    h->srcRate = rate;
    return h->inFrames > 0;
}

const int filters[16][2] = {
    { 0x0000,  0x0000}, { 0x0800,  0x0000}, { 0x0000,  0x0800}, { 0x0400,  0x0400},
    { 0x1000, -0x0800}, { 0x0e00, -0x0600}, { 0x0c00, -0x0400}, { 0x1200, -0x0a00},
    { 0x1068, -0x08c8}, { 0x12c0, -0x08fc}, { 0x1400, -0x0c00}, { 0x0800, -0x0800},
    { 0x0400, -0x0400}, {-0x0400,  0x0400}, {-0x0400,  0x0000}, {-0x0800,  0x0000},
};
inline short clamp16(int v) { return (short)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
}

// Decode + resample the whole file into dst (outFrames stereo frames). Returns frames written. cancel: polled once per 4096 input frames;
// progress (optional): output frames completed so far, updated after every batch.
extern "C" unsigned n3ds_stx_decode(const char* path, short* dst, unsigned outFrames, int dstRate, volatile int* cancel, volatile unsigned* progress)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    Hdr h;
    if (!readHeader(f, &h) || fseek(f, 32, SEEK_SET) != 0) { fclose(f); return 0; }
    const uint64_t step = ((uint64_t)h.srcRate << 16) / (unsigned)dstRate;
    enum { BATCH_BLOCKS = 256, BATCH_FRAMES = BATCH_BLOCKS * 16 };
    std::vector<short> in((size_t)(BATCH_FRAMES + 64) * 2);       // frames [bufBase, bufBase + bufN)
    uint8_t* raw = (uint8_t*)malloc((size_t)BATCH_BLOCKS * 18);
    if (!raw) { fclose(f); return 0; }
    size_t bufBase = 0, bufN = 0, decoded = 0, outIdx = 0;
    uint64_t pos = 0;
    int hist[2][2] = {};
    bool final = false;
    while (outIdx < outFrames && !final) {
        if (cancel && *cancel) break;
        // ---- decode one batch ----
        const size_t left = h.inFrames - decoded;
        size_t blocks = (left + 15) / 16;
        if (blocks > BATCH_BLOCKS) blocks = BATCH_BLOCKS;
        const size_t got = fread(raw, 18, blocks, f);
        for (size_t b = 0; b < got; ++b) {
            for (int ch = 0; ch < 2; ++ch) {
                const uint8_t* e = raw + b * 18 + ch * 9;
                const int scale = (e[0] >> 4) & 15, pred = e[0] & 15;
                int newer = hist[ch][0], older = hist[ch][1];
                for (int n = 0; n < 16 && decoded + n < h.inFrames; ++n) {
                    const uint8_t packed = e[1 + n / 2];
                    int nib = (n & 1) ? (packed & 15) : (packed >> 4);
                    if (nib >= 8) nib -= 16;
                    const short s = clamp16(nib * (1 << scale) + ((filters[pred][0] * newer + filters[pred][1] * older) >> 11));
                    in[(bufN + n) * 2 + ch] = s;
                    older = newer; newer = s;
                }
                hist[ch][0] = newer; hist[ch][1] = older;
            }
            const size_t add = std::min<size_t>(16, h.inFrames - decoded);
            bufN += add; decoded += add;
        }
        final = got < blocks || decoded >= h.inFrames;           // short read = file ended early: finish with what we have
        // ---- resample what is available (same arithmetic as SDL_AudioStreamFlush in n3ds_sdl_audio.cpp) ----
        while (outIdx < outFrames) {
            const size_t i0 = (size_t)(pos >> 16);
            if (i0 < bufBase) break;                              // cannot happen
            const size_t l0 = i0 - bufBase;
            if (l0 >= bufN) break;                                // needs more input (or the input is exhausted)
            size_t l1 = l0 + 1;
            if (l1 >= bufN) { if (!final) break; l1 = l0; }       // last input frame: interpolate with itself, like the SDL shim
            const int fr = (int)(pos & 0xFFFF);
            for (int c = 0; c < 2; ++c) {
                const int a = in[l0 * 2 + c], b = in[l1 * 2 + c];
                dst[outIdx * 2 + c] = (short)(a + (int)(((int64_t)(b - a) * fr) >> 16));
            }
            ++outIdx; pos += step;
        }
        if (progress) *progress = (unsigned)outIdx;
        // ---- drop the input frames that can no longer be needed ----
        size_t keep = (size_t)(pos >> 16) - bufBase;
        if (keep > bufN) keep = bufN;
        if (keep) { memmove(in.data(), in.data() + keep * 2, (bufN - keep) * 2 * sizeof(short)); bufBase += keep; bufN -= keep; }
    }
    free(raw);
    fclose(f);
    return (unsigned)outIdx;
}

// ---- game-thread interface ---------------------------------------------------------------------------------------------------
// 1 = ok: *outFrames = frames at dstRate (what upstream's resampler would produce). 0 = missing / unsupported file.
extern "C" int n3ds_stx_probe(const char* path, int dstRate, unsigned* outFrames, unsigned* srcRate)
{
    FILE* f = fopen(path, "rb");
    if (!f) { n3ds_log("[stx] cannot open %s", path); return 0; }
    Hdr h;
    const bool ok = readHeader(f, &h);
    fclose(f);
    if (!ok || dstRate <= 0) { n3ds_log("[stx] unsupported or empty stream %s", path); return 0; }
    *srcRate = h.srcRate;
    *outFrames = (unsigned)((uint64_t)h.inFrames * (unsigned)dstRate / h.srcRate);
    return *outFrames > 0;
}

namespace {
struct Job { char path[256]; short* dst; unsigned frames; int rate; };
Job sJob;
volatile int sCancel = 0;
volatile unsigned sProgress = 0;
volatile int sRunning = 0;
bool sAtExit = false;
#ifdef __3DS__
Thread sThread = nullptr;
#else
std::thread* sThread = nullptr;
#endif

void jobMain(void*)
{
    /* R1 (n3ds_port/AGENTS.md): FPSCR is per-thread, so every thread that does float work sets flush-to-zero (FZ) + default-NaN (DN)
       at its entry, before any float work: decay can reach denormals, FZ|DN is the chosen numerical mode, and each thread's state
       must be established. Current libctru already does this in initThreadVars() for main and threadCreate() threads; the call is
       the defensive, version-independent guarantee (it also turns the VFP exception traps off). */
    n3ds_vfp_runfast();
    const uint64_t t0 =
#ifdef __3DS__
        svcGetSystemTick();
#else
        0;
#endif
    const unsigned n = n3ds_stx_decode(sJob.path, sJob.dst, sJob.frames, sJob.rate, &sCancel, &sProgress);
#ifdef __3DS__
    n3ds_log("[stx] decoded %u of %u frames in %u ms%s", n, sJob.frames, (unsigned)((svcGetSystemTick() - t0) * 1000ULL / SYSCLOCK_ARM11), sCancel ? " (cancelled)" : "");
#else
    (void)t0; n3ds_log("[stx] decoded %u of %u frames%s", n, sJob.frames, sCancel ? " (cancelled)" : "");
#endif
    sRunning = 0;
}
}

// Stop the decoder and wait for it. MUST be called before the buffer it writes into is replaced or freed.
extern "C" void n3ds_stx_cancel(void)
{
    if (!sThread) return;
    sCancel = 1;
#ifdef __3DS__
    n3ds_note("stx: cancel, joining decoder");
    threadJoin(sThread, U64_MAX); threadFree(sThread);
    n3ds_note("stx: decoder joined");
#else
    sThread->join(); delete sThread;
#endif
    sThread = nullptr;
}

// Start decoding into dst (which must stay allocated and unmoved until n3ds_stx_cancel() returns or the decoder finishes).
extern "C" void n3ds_stx_start(const char* path, short* dst, unsigned outFrames, int dstRate)
{
    n3ds_stx_cancel();
    if (!sAtExit) { sAtExit = true; atexit(n3ds_stx_cancel); }     // registered after the audio statics were constructed, so it runs before they are destroyed
    snprintf(sJob.path, sizeof sJob.path, "%s", path);
    sJob.dst = dst; sJob.frames = outFrames; sJob.rate = dstRate;
    sCancel = 0; sRunning = 1; sProgress = 0;
#ifdef __3DS__
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    const s32 low = prio + 8 > 0x3F ? 0x3F : prio + 8;             // below the game thread, the renderer and the mixer
    sThread = threadCreate(jobMain, nullptr, 32 * 1024, low, 2, false);
    if (!sThread) sThread = threadCreate(jobMain, nullptr, 32 * 1024, low, -2, false);
    if (!sThread) { n3ds_log("[stx] threadCreate failed, decoding on the calling thread"); jobMain(nullptr); }
#else
    sThread = new std::thread(jobMain, nullptr);
#endif
}

// Wait (at most timeoutMs) until the first `want` output frames are decoded or the decoder has finished, so playback never starts on
// silence. Called by the game thread right after n3ds_stx_start(); normally returns within a few milliseconds.
extern "C" int n3ds_stx_wait_ready(unsigned want, unsigned timeoutMs)
{
    for (unsigned waited = 0; waited <= timeoutMs; ++waited) {
        if (sProgress >= want || !sRunning) return 1;
#ifdef __3DS__
        svcSleepThread(1000000);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
    return 0;
}
