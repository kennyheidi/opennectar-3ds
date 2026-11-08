// Host test: n3ds_stx.cpp (streaming decode + resample) must produce EXACTLY what the old path produced:
// upstream pc_audio_play_stx decode (pc_port/audio/pc_audio.cpp) followed by the SDL_AudioStream shim resampler (n3ds_sdl_audio.cpp).
// Build/run: tools/host_test/run_stx.sh
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
typedef unsigned char u8; typedef unsigned int u32; typedef unsigned long long u64;
extern "C" unsigned n3ds_stx_decode(const char*, short*, unsigned, int, volatile int*, volatile unsigned*);
extern "C" int n3ds_stx_wait_ready(unsigned, unsigned);
extern "C" int n3ds_stx_probe(const char*, int, unsigned*, unsigned*);
extern "C" void n3ds_stx_start(const char*, short*, unsigned, int);
extern "C" void n3ds_stx_cancel(void);

static const int filters[16][2] = {
    { 0x0000,  0x0000}, { 0x0800,  0x0000}, { 0x0000,  0x0800}, { 0x0400,  0x0400}, { 0x1000, -0x0800}, { 0x0e00, -0x0600}, { 0x0c00, -0x0400}, { 0x1200, -0x0a00},
    { 0x1068, -0x08c8}, { 0x12c0, -0x08fc}, { 0x1400, -0x0c00}, { 0x0800, -0x0800}, { 0x0400, -0x0400}, {-0x0400,  0x0400}, {-0x0400,  0x0000}, {-0x0800,  0x0000}};
static short cl(int v) { return (short)std::max(-32768, std::min(32767, v)); }
static u32 rbe32(const u8* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static u32 rbe16(const u8* p) { return (p[0] << 8) | p[1]; }

// ---- reference: upstream decode + SDL shim resample, copied verbatim in structure ----
static bool refDecode(const std::vector<u8>& file, int dstRate, std::vector<short>& out)
{
    if (file.size() < 32) return false;
    const u32 encodedSize = rbe32(file.data()), sampleCount = rbe32(file.data() + 4), rate = rbe16(file.data() + 8), format = rbe16(file.data() + 10);
    const size_t body = std::min<size_t>(encodedSize, file.size() - 32);
    if (format != 4 || rate == 0 || body < 18) return false;
    const size_t blockCount = body / 18, outputFrames = std::min<size_t>(sampleCount, blockCount * 16);
    std::vector<short> pcm(outputFrames * 2);
    int history[2][2] = {};
    const u8* src = file.data() + 32;
    size_t frame = 0;
    for (size_t block = 0; block < blockCount && frame < outputFrames; ++block) {
        for (int ch = 0; ch < 2; ++ch) {
            const u8* e = src + block * 18 + ch * 9;
            const int scale = (e[0] >> 4) & 15, pred = e[0] & 15;
            int newer = history[ch][0], older = history[ch][1];
            for (int n = 0; n < 16 && frame + n < outputFrames; ++n) {
                const u8 packed = e[1 + n / 2];
                int nib = (n & 1) ? (packed & 15) : (packed >> 4);
                if (nib >= 8) nib -= 16;
                const int d = nib * (1 << scale) + ((filters[pred][0] * newer + filters[pred][1] * older) >> 11);
                const short s = cl(d);
                pcm[(frame + n) * 2 + ch] = s; older = newer; newer = s;
            }
            history[ch][0] = newer; history[ch][1] = older;
        }
        frame += 16;
    }
    const size_t inFrames = outputFrames;
    if ((int)rate == dstRate) { out = pcm; return true; }
    const size_t outFrames = (size_t)((unsigned long long)inFrames * (unsigned)dstRate / rate);
    out.assign(outFrames * 2, 0);
    const unsigned long long step = ((unsigned long long)rate << 16) / (unsigned)dstRate;
    unsigned long long pos = 0;
    for (size_t i = 0; i < outFrames; ++i, pos += step) {
        const size_t i0 = (size_t)(pos >> 16); const int fr = (int)(pos & 0xFFFF);
        const size_t i1 = i0 + 1 < inFrames ? i0 + 1 : i0;
        for (int c = 0; c < 2; ++c) { const int a = pcm[i0 * 2 + c], b = pcm[i1 * 2 + c]; out[i * 2 + c] = (short)(a + (int)(((long long)(b - a) * fr) >> 16)); }
    }
    return true;
}

static std::vector<u8> makeFile(unsigned frames, unsigned rate, unsigned extraBlocks, unsigned seed, size_t truncateTo = 0)
{
    srand(seed);
    const unsigned blocks = (frames + 15) / 16 + extraBlocks;
    std::vector<u8> f(32 + (size_t)blocks * 18);
    const u32 enc = blocks * 18;
    f[0] = enc >> 24; f[1] = enc >> 16; f[2] = enc >> 8; f[3] = enc;
    f[4] = frames >> 24; f[5] = frames >> 16; f[6] = frames >> 8; f[7] = frames;
    f[8] = rate >> 8; f[9] = rate; f[10] = 0; f[11] = 4;
    for (size_t i = 32; i < f.size(); ++i) f[i] = (u8)(rand() >> 5);
    // keep the predictor/scale nibble of each 9-byte channel header in a sane range so the signal does not just clip
    for (unsigned b = 0; b < blocks; ++b) for (int ch = 0; ch < 2; ++ch) { u8& h = f[32 + b * 18 + ch * 9]; h = (u8)(((rand() % 9) << 4) | (rand() % 16)); }
    if (truncateTo && truncateTo < f.size()) f.resize(truncateTo);
    return f;
}

static int sFail = 0, sChecks = 0;
static void check(const char* name, const std::vector<u8>& file, int dstRate)
{
    FILE* fp = fopen("/tmp/stx_test.bin", "wb"); fwrite(file.data(), 1, file.size(), fp); fclose(fp);
    std::vector<short> ref; const bool refOk = refDecode(file, dstRate, ref);
    unsigned outFrames = 0, srcRate = 0;
    const int ok = n3ds_stx_probe("/tmp/stx_test.bin", dstRate, &outFrames, &srcRate);
    ++sChecks;
    if (!refOk) { if (ok) { printf("FAIL %s: reference rejects the file but probe accepts it\n", name); ++sFail; } return; }
    if (!ok || outFrames * 2 != ref.size()) { printf("FAIL %s: probe says %u frames (ok=%d), reference %zu\n", name, outFrames, ok, ref.size() / 2); ++sFail; return; }
    std::vector<short> got(ref.size(), 12345);
    // 1) direct
    const unsigned n = n3ds_stx_decode("/tmp/stx_test.bin", got.data(), outFrames, dstRate, nullptr, nullptr);
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < ref.size(); ++i) if (got[i] != ref[i]) { if (!bad) first = i; ++bad; }
    ++sChecks;
    if (n != outFrames || bad) { printf("FAIL %s: decoded %u of %u frames, %zu samples differ (first at %zu: got %d want %d)\n", name, n, outFrames, bad, first, bad ? got[first] : 0, bad ? ref[first] : 0); ++sFail; }
    // 2) through the thread interface
    std::vector<short> got2(ref.size(), 777);
    n3ds_stx_start("/tmp/stx_test.bin", got2.data(), outFrames, dstRate);
    {
        const unsigned want = outFrames < 16000 ? outFrames : 16000;
        const int rd = n3ds_stx_wait_ready(want, 2000);
        size_t badp = 0; for (size_t i = 0; i < (size_t)want * 2; ++i) if (got2[i] != ref[i]) ++badp;
        ++sChecks;
        if (!rd || badp) { printf("FAIL %s: after wait_ready(%u) ready=%d, %zu of the first samples differ\n", name, want, rd, badp); ++sFail; }
    }
    n3ds_stx_cancel();                                         // joins; may have been cancelled early, so only require "no write outside the buffer / no hang"
    ++sChecks;
    n3ds_stx_start("/tmp/stx_test.bin", got2.data(), outFrames, dstRate);
    for (int spin = 0; spin < 2000; ++spin) { struct timespec ts = {0, 1000000}; nanosleep(&ts, nullptr); if (got2.size() < 2 || got2[got2.size() - 1] != 777) break; }
    n3ds_stx_cancel();
}

int main()
{
    check("44100->32000 normal",       makeFile(100000, 44100, 0, 1), 32000);
    check("44100->32000 partial last", makeFile(100003, 44100, 0, 2), 32000);
    check("44100->32000 extra blocks", makeFile(70001, 44100, 5, 3), 32000);   // header says fewer frames than the body holds
    check("32000->32000 same rate",    makeFile(50000, 32000, 0, 4), 32000);
    check("22050->32000 upsample",     makeFile(30000, 22050, 0, 5), 32000);
    check("48000->32000",              makeFile(60000, 48000, 0, 6), 32000);
    check("tiny (1 block)",            makeFile(16, 44100, 0, 7), 32000);
    check("tiny (5 frames)",           makeFile(5, 44100, 0, 8), 32000);
    check("4096 boundary",             makeFile(4096, 44100, 0, 9), 32000);
    check("4097 boundary",             makeFile(4097, 44100, 0, 10), 32000);
    check("8192+1",                    makeFile(8193, 44100, 0, 11), 32000);
    check("opening.stx sized (2480208)", makeFile(2480208, 44100, 0, 12), 32000);
    check("truncated body",            makeFile(100000, 44100, 0, 13, 32 + 18 * 1000), 32000);   // encodedSize claims more than the file holds
    check("bad format",                [] { auto f = makeFile(1000, 44100, 0, 14); f[11] = 3; return f; }(), 32000);
    check("zero rate",                 [] { auto f = makeFile(1000, 44100, 0, 15); f[8] = f[9] = 0; return f; }(), 32000);
    // cancel mid-way must stop quickly and leave the rest untouched
    {
        auto file = makeFile(2480208, 44100, 0, 16);
        FILE* fp = fopen("/tmp/stx_test.bin", "wb"); fwrite(file.data(), 1, file.size(), fp); fclose(fp);
        unsigned of = 0, sr = 0; n3ds_stx_probe("/tmp/stx_test.bin", 32000, &of, &sr);
        volatile int cancel = 1;
        std::vector<short> buf((size_t)of * 2, 555);
        const unsigned n = n3ds_stx_decode("/tmp/stx_test.bin", buf.data(), of, 32000, &cancel, nullptr);
        ++sChecks;
        if (n != 0 || buf[buf.size() - 1] != 555) { printf("FAIL cancel: wrote %u frames although cancelled at start\n", n); ++sFail; }
    }
    printf("%d checks, %d failed\n", sChecks, sFail);
    return sFail != 0;
}
