// NDSP output sink (milestone 1n). ctru side: includes <3ds.h>, never game headers.
//
// This is ONLY the speaker end: it takes interleaved signed-16-bit stereo PCM from whatever mixes the game's sound
// (Open Nectar's JAudio software DSP, once it is switched on and hooked up -- see NOTES_1n.md) and feeds the New 3DS DSP.
// Design borrowed from the Melee 3DS port's native-3ds/port/3ds/audio.c (read, not copied; this file is original code):
//   * a small ring of ndspWaveBufs in linear memory, several mixer slices combined per buffer so the DSP's short hardware
//     queue covers tens of ms instead of ~12 ms;
//   * prebuffer before un-pausing, and re-prebuffer after an underrun instead of stuttering;
//   * cache clean via the kernel's svcFlushProcessDataCache (one SVC, no service round trip), with DSP_FlushDataCache as the
//     fallback if the CIA's exheader does not grant SVC 0x54 (nectar.rsf.in now lists FlushProcessDataCache: 84).
// Never blocks: n3ds_audio_write() copies what fits and reports how many frames it took; the caller decides what to do with
// the rest (a mixer thread usually just retries after a short sleep).
//
// UNVERIFIED: not built with devkitPro and never run on hardware. A .3dsx (Homebrew Launcher) additionally needs the DSP
// firmware dumped to sdmc:/3ds/dspfirm.cdc; the installed CIA uses the system's.
#include <3ds.h>
#include <math.h>
#include <string.h>
#include "n3ds_bridge.h"

namespace {
const int kBlocks = 8;             // ndspWaveBufs in the ring
const int kBlockFrames = 512;      // frames per block: 16 ms at 32 kHz, 10.7 ms at 48 kHz
const int kPrefillBlocks = 3;      // queued before un-pausing (~48 ms at 32 kHz)

ndspWaveBuf sWave[kBlocks];
s16* sPcm = nullptr;               // kBlocks * kBlockFrames * 2 samples, linear memory
bool sReady = false, sPrebuffering = false, sDirectFlushBad = false;
int sNext = 0;                     // block being filled
int sFill = 0;                     // frames already written into it
unsigned sRate = 0;
unsigned sUnderruns = 0, sDropped = 0, sQueuedBlocks = 0;

bool busy(const ndspWaveBuf& w) { return w.status != NDSP_WBUF_FREE && w.status != NDSP_WBUF_DONE; }

int queuedBlocks()
{
    int n = 0;
    for (int i = 0; i < kBlocks; ++i) if (busy(sWave[i])) ++n;
    return n;
}

bool flushBlock(const void* p, unsigned bytes)
{
    Result r = -1;
    if (!sDirectFlushBad) {
        r = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(uintptr_t)p, bytes);
        if (R_FAILED(r)) { sDirectFlushBad = true; n3ds_log("[audio] svcFlushProcessDataCache refused (%08lx): using DSP_FlushDataCache", (unsigned long)r); }
    }
    if (sDirectFlushBad) r = DSP_FlushDataCache(p, bytes);
    return R_SUCCEEDED(r);
}
} // namespace

extern "C" int n3ds_audio_open(unsigned rate)
{
    if (sReady) return 1;
    Result r = ndspInit();
    if (R_FAILED(r)) {
        n3ds_log("[audio] ndspInit failed (%08lx): no sound. Homebrew Launcher builds need sdmc:/3ds/dspfirm.cdc (dump it with DSP1).", (unsigned long)r);
        return 0;
    }
    sPcm = (s16*)linearAlloc((size_t)kBlocks * kBlockFrames * 2 * sizeof(s16));
    if (!sPcm) { n3ds_log("[audio] linearAlloc for the PCM ring failed"); ndspExit(); return 0; }
    memset(sPcm, 0, (size_t)kBlocks * kBlockFrames * 2 * sizeof(s16));
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, (float)rate);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 1.0f, 1.0f };    // channel 0 -> main left/right at full volume
    ndspChnSetMix(0, mix);
    memset(sWave, 0, sizeof(sWave));
    for (int i = 0; i < kBlocks; ++i) { sWave[i].data_vaddr = sPcm + (size_t)i * kBlockFrames * 2; sWave[i].nsamples = kBlockFrames; }
    sNext = sFill = 0; sRate = rate; sPrebuffering = true;
    ndspChnSetPaused(0, true);
    sReady = true;
    n3ds_log("[audio] NDSP stereo PCM16 ready: %u Hz, %d x %d-frame blocks, prefill %d blocks, linear free %u KB",
             rate, kBlocks, kBlockFrames, kPrefillBlocks, n3ds_gpu_tex_free_kb());
    return 1;
}

extern "C" unsigned n3ds_audio_write(const short* interleaved, unsigned frames)
{
    if (!sReady) return 0;
    unsigned taken = 0;
    // Underrun: the DSP ran out of queued data while playing. Pause and refill rather than play a stream full of holes.
    if (!sPrebuffering && queuedBlocks() == 0 && sFill == 0) { ++sUnderruns; sPrebuffering = true; ndspChnSetPaused(0, true); }
    while (taken < frames) {
        ndspWaveBuf& w = sWave[sNext];
        if (sFill == 0 && busy(w)) { sDropped += frames - taken; break; }     // ring full: caller keeps its data / drops it
        unsigned n = kBlockFrames - sFill; if (n > frames - taken) n = frames - taken;
        memcpy(sPcm + ((size_t)sNext * kBlockFrames + sFill) * 2, interleaved + (size_t)taken * 2, (size_t)n * 2 * sizeof(s16));
        sFill += n; taken += n;
        if (sFill == kBlockFrames) {
            sFill = 0;
            if (flushBlock(sPcm + (size_t)sNext * kBlockFrames * 2, kBlockFrames * 2 * sizeof(s16))) {
                ndspChnWaveBufAdd(0, &w); ++sQueuedBlocks;
            } else { sDropped += kBlockFrames; }
            sNext = (sNext + 1) % kBlocks;
            if (sPrebuffering && queuedBlocks() >= kPrefillBlocks) { sPrebuffering = false; ndspChnSetPaused(0, false); }
        }
    }
    return taken;
}

extern "C" unsigned n3ds_audio_queued_frames(void)
{
    return sReady ? (unsigned)queuedBlocks() * kBlockFrames + (unsigned)sFill : 0;
}

extern "C" void n3ds_audio_stats(unsigned* underruns, unsigned* droppedFrames, unsigned* blocksQueued)
{
    if (underruns) *underruns = sUnderruns;
    if (droppedFrames) *droppedFrames = sDropped;
    if (blocksQueued) *blocksQueued = sQueuedBlocks;
}

extern "C" void n3ds_audio_close(void)
{
    if (!sReady) return;
    ndspChnWaveBufClear(0);
    ndspExit();
    linearFree(sPcm); sPcm = nullptr;
    sReady = false;
}

// Plays a two-tone test (440 Hz left, 660 Hz right, then both) through the same path the game will use. Run it from
// nectar.ini (audio_test = 1) to prove the DSP, the firmware and this sink work before involving the game's mixer.
extern "C" void n3ds_audio_selftest(unsigned ms)
{
    const unsigned rate = 32000;
    if (!n3ds_audio_open(rate)) return;
    const unsigned total = (unsigned)((unsigned long long)rate * ms / 1000);
    static short buf[256 * 2];
    double ph1 = 0, ph2 = 0;
    unsigned done = 0, spins = 0;
    n3ds_log("[audio] selftest: %u ms tone (440 Hz left, 660 Hz right)", ms);
    while (done < total && spins < 200000) {
        unsigned n = total - done; if (n > 256) n = 256;
        for (unsigned i = 0; i < n; ++i) {
            const bool second = (done + i) > total / 2;      // second half: both channels get both tones
            const double a = sin(ph1) * 0.25, b = sin(ph2) * 0.25;
            buf[i * 2 + 0] = (short)((second ? a + b : a) * 32767.0);
            buf[i * 2 + 1] = (short)((second ? a + b : b) * 32767.0);
            ph1 += 2.0 * M_PI * 440.0 / rate; ph2 += 2.0 * M_PI * 660.0 / rate;
        }
        unsigned took = n3ds_audio_write(buf, n);
        done += took;
        if (took < n) { svcSleepThread(2 * 1000 * 1000); ++spins; }   // ring full: wait 2 ms (never a busy spin)
    }
    unsigned u, d, q; n3ds_audio_stats(&u, &d, &q);
    n3ds_log("[audio] selftest done: %u frames written, blocks queued %u, underruns %u, dropped frames %u", done, q, u, d);
    svcSleepThread(300 * 1000 * 1000);   // let the DSP drain the last blocks before anything closes the channel
    n3ds_audio_close();
}
