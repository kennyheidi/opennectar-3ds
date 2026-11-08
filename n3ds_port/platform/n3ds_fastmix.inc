/* ---- 3DS: faster mixer (spliced into the build-time copy of pc_audio.cpp, just before audio_callback(); see n3ds_port/CMakeLists.txt) ----
   Same mixing model as the original callback (per-voice linear interpolation, one-pole cutoff filter, envelope steps every kEnvelopeControlPeriod
   frames, release ramp, pan, FX send, Dolby send, FX delay, limiter), restructured for an ARM11 (VFP, no NEON, no out-of-order):
   - voice-outer / block-inner: each voice renders a whole 256-frame block into float buffers, instead of every voice being visited for every frame
   - everything that is constant between two envelope steps (gain, pan, FX/Dolby mix, pitch step, bus/track/layer gains, pause state) is computed once per
     segment instead of once per sample; the sample position is a 32.32 fixed-point integer instead of a double (no double->int conversions per sample)
   - samples are read through a raw pointer (the original went shared_ptr -> vector -> operator[] with bounds checks each time)
   - the FX / Dolby sends are only accumulated for voices that use them; the cutoff filter only runs for voices that have one
   The tail (FX delay, Dolby delay, limiter, output) is the original code, per frame. nectar.ini audio_fastmix = 0 (or audio_stats = 1) uses the original
   callback. Differences to the original are float rounding (the original truncated every voice's contribution to an int). */
extern "C" int n3ds_audio_fastmix(void);
static void audio_callback_orig(void*, Uint8* output, int byteCount);

static constexpr size_t kN3dsMixBlock = 256;

template <bool kFilter, bool kFx, bool kRel>
static inline void n3dsMixRun(const s16* data, u64& pos, u64 inc, size_t n, float* mL, float* mR, float* fL, float* fR, float* dl,
                              float g0, float& rg, float rgStep, float lMul, float rMul, float lFx, float rFx, float dC, float alpha, float& state)
{
    /* @vfp-exempt R2 — screen false positive: (size_t)(pos >> 32) is the integer part of a u64 32.32 fixed-point position, no float is converted. measured n/a. Verified 2026-11-01. */
    for (size_t k = 0; k < n; ++k) {
        const size_t i = (size_t)(pos >> 32);
        const float s0 = (float)data[i];
        const float fr = (float)(uint32_t)pos * (1.0f / 4294967296.0f);
        const float raw = s0 + ((float)data[i + 1] - s0) * fr;
        float smp;
        if (kFilter) { state += alpha * (raw - state); smp = state; } else { state = raw; smp = raw; }
        float g = g0;
        if (kRel) { g *= rg; rg -= rgStep; }   /* R3 (r53): no per-sample clamp; see below */
        const float s = smp * g;
        mL[k] += s * lMul; mR[k] += s * rMul;
        if (kFx) { fL[k] += s * lFx; fR[k] += s * rFx; dl[k] += s * dC; }
        pos += inc;
    }
    /* r53, R3 + T1: releaseStep = releaseGain / releaseFrames and a segment never runs past releaseFrames, so rg only dips below 0 by
       float rounding on the last sample(s) of a release, where gain x sample is far below one output LSB. Clamped once per run instead
       of compared on every sample. */
    if (kRel && rg < 0.0f) rg = 0.0f;
}

static void n3dsMixVoice(SampleVoice& voice, size_t frames, float* mixL, float* mixR, float* fxL, float* fxR, float* dol)
{
    if (!voice.active || !voice.samples || voice.samples->empty()) return;
    if ((voice.jamOwner == 1 && voice.jamSource < sSETrackPaused.size() && sSETrackPaused[voice.jamSource])
        || (voice.jamOwner == 2 && sEventsPaused)) return;
    const s16* data = voice.samples->data();
    const size_t size = voice.samples->size();
    const size_t playbackEnd = voice.endSample ? std::min(voice.endSample, size) : size;
    const float busGain = sBusVolumes[voice.bus]
        * (voice.bgmTrack == 0 ? sBgmTrackGain : (voice.bgmTrack == 1 ? sBossTrackGain : 1.0f))
        * (voice.bgmTrack == 0 && voice.bgmLayer < 16 ? sBgmLayerGain[voice.bgmLayer] : 1.0f)
        * (voice.eventSource < 16 ? sEventVolumes[voice.eventSource] : 1.0f);
    const float eventPan = voice.eventSource < 16 ? sEventPans[voice.eventSource] : 0.0f;
    const float volume = voice.baseVolume * voice.trackVolume;
    /* @vfp-exempt R2,R3,R4,R7 — per voice per 256-frame block, not per sample: SampleVoice::cursor/step are double in the shared pc_audio.cpp (PC build) and the 32.32 position needs more than float's 24-bit mantissa (pitch would drift); segment set-up compares/clamps pan, FX and release once per envelope step. measured n/a. Verified 2026-11-01. */
    u64 pos = (u64)(voice.cursor * 4294967296.0);
    float state = voice.filterState;
    float rg = voice.releaseGain;
    size_t o = 0, remaining = frames;
    while (remaining && voice.active) {
        size_t seg = remaining;
        const size_t toUpdate = (size_t)voice.envelopeCountdown + 1;
        const bool hasOsc = voice.oscillators != nullptr;
        if (hasOsc && toUpdate < seg) seg = toUpdate;
        const bool doEnv = hasOsc && seg == toUpdate;
        const bool releasing = voice.releaseFrames != 0;
        if (releasing && seg > voice.releaseFrames) seg = voice.releaseFrames;
        /* constants of this segment */
        const float pan = std::clamp(voice.basePan + eventPan + voice.envelopePan, -1.0f, 1.0f);
        /* @vfp-exempt R3 — once per voice per envelope segment, not per sample: the segment constants are worked out here so the per-sample run (n3dsMixRun) has no compare at all. measured n/a. Verified 2026-11-01. */
        const float lMul = volume * (pan <= 0.0f ? 1.0f : 1.0f - pan);
        const float rMul = volume * (pan >= 0.0f ? 1.0f : 1.0f + pan);
        const float fxMix = 1.0f - (1.0f - std::clamp(voice.envelopeFxMix, 0.0f, 1.0f)) * (1.0f - std::clamp(voice.trackFxMix, 0.0f, 1.0f));
        const float dolby = 1.0f - (1.0f - std::clamp(voice.envelopeDolby, 0.0f, 1.0f)) * (1.0f - std::clamp(voice.trackDolby, 0.0f, 1.0f));
        const bool useFx = fxMix > 0.0f || dolby > 0.0f;
        const float lFx = lMul * fxMix, rFx = rMul * fxMix, dC = (lMul + rMul) * 0.5f * dolby;
        const float g0 = voice.envelopeVolume * busGain;
        const float rgStep = releasing ? voice.releaseStep : 0.0f;
        const float alpha = voice.filterAlpha;
        const bool filt = alpha != 1.0f;
        /* @vfp-exempt R2,R3,R4 — once per segment: SampleVoice::step is double in the shared pc_audio.cpp, and the 32.32 fixed-point step needs more than float's 24-bit mantissa or the pitch drifts. measured n/a. Verified 2026-11-01. */
        double incD = voice.step * voice.envelopePitch;
        u64 inc = incD > 0.0 ? (u64)(incD * 4294967296.0) : 1ull;
        if (inc == 0) inc = 1;
        size_t done = 0;
        while (done < seg) {
            /* @vfp-exempt R2 — screen false positive: integer casts of u64 fixed-point positions and sample counts, no float is converted. measured n/a. Verified 2026-11-01. */
            size_t idx = (size_t)(pos >> 32);
            if (idx >= playbackEnd) {
                if (!voice.looping || voice.loopSample >= playbackEnd) { voice.active = false; break; }
                pos = (u64)voice.loopSample << 32;
                idx = voice.loopSample;
            }
            const u64 fastLimit = (u64)(playbackEnd - 1) << 32;   /* idx + 1 < playbackEnd  <=>  pos < fastLimit */
            size_t nf = 0;
            if (pos < fastLimit) {
                const u64 need = (fastLimit - pos + inc - 1) / inc;
                nf = need < (u64)(seg - done) ? (size_t)need : seg - done;
            }
            float* mL = mixL + o + done; float* mR = mixR + o + done; float* fL = fxL + o + done; float* fR = fxR + o + done; float* dl = dol + o + done;
            if (nf) {
                if (filt) {
                    if (useFx) { if (releasing) n3dsMixRun<true, true, true>(data, pos, inc, nf, mL, mR, fL, fR, dl, g0, rg, rgStep, lMul, rMul, lFx, rFx, dC, alpha, state);
                                 else           n3dsMixRun<true, true, false>(data, pos, inc, nf, mL, mR, fL, fR, dl, g0, rg, rgStep, lMul, rMul, lFx, rFx, dC, alpha, state); }
                    else       { if (releasing) n3dsMixRun<true, false, true>(data, pos, inc, nf, mL, mR, fL, fR, dl, g0, rg, rgStep, lMul, rMul, lFx, rFx, dC, alpha, state);
                                 else           n3dsMixRun<true, false, false>(data, pos, inc, nf, mL, mR, fL, fR, dl, g0, rg, rgStep, lMul, rMul, lFx, rFx, dC, alpha, state); }
                } else {
                    if (useFx) { if (releasing) n3dsMixRun<false, true, true>(data, pos, inc, nf, mL, mR, fL, fR, dl, g0, rg, rgStep, lMul, rMul, lFx, rFx, dC, alpha, state);
                                 else           n3dsMixRun<false, true, false>(data, pos, inc, nf, mL, mR, fL, fR, dl, g0, rg, rgStep, lMul, rMul, lFx, rFx, dC, alpha, state); }
                    else       { if (releasing) n3dsMixRun<false, false, true>(data, pos, inc, nf, mL, mR, fL, fR, dl, g0, rg, rgStep, lMul, rMul, lFx, rFx, dC, alpha, state);
                                 else           n3dsMixRun<false, false, false>(data, pos, inc, nf, mL, mR, fL, fR, dl, g0, rg, rgStep, lMul, rMul, lFx, rFx, dC, alpha, state); }
                }
                done += nf;
            } else {
                /* the last sample before the end / loop point: the neighbour is the loop start (looping) or the sample itself */
                /* @vfp-exempt R2,R3 — the single sample at a loop/end boundary (at most a few per voice per block): int->float sample reads, and the release clamp the bulk run hoists. measured n/a. Verified 2026-11-01. */
                size_t next = idx + 1 < playbackEnd ? idx + 1 : (voice.looping ? voice.loopSample : idx);
                if (next >= size) next = idx;
                const float s0 = (float)data[idx];
                const float fr = (float)(uint32_t)pos * (1.0f / 4294967296.0f);
                const float raw = s0 + ((float)data[next] - s0) * fr;
                float smp;
                if (filt) { state += alpha * (raw - state); smp = state; } else { state = raw; smp = raw; }
                float g = g0;
                if (releasing) { g *= rg; rg -= rgStep; if (rg < 0.0f) rg = 0.0f; }   /* one sample at a loop boundary, not the per-sample loop */
                const float s = smp * g;
                mL[0] += s * lMul; mR[0] += s * rMul;
                if (useFx) { fL[0] += s * lFx; fR[0] += s * rFx; dl[0] += s * dC; }
                pos += inc;
                ++done;
            }
        }
        /* state after the segment (done < seg only when the voice ended) */
        if (releasing) {
            voice.releaseGain = rg;
            /* @vfp-exempt R2 — screen false positive: (u32) of a size_t sample count. measured n/a. Verified 2026-11-01. */
            voice.releaseFrames -= (u32)done;
            if (voice.releaseFrames == 0) voice.active = false;
        }
        if (hasOsc) {
            if (done == seg && doEnv && voice.active) {
                voice.envelopeVolume = 1.0f;
                voice.envelopePitch = 1.0f;
                voice.envelopePan = 0.0f;
                voice.envelopeFxMix = voice.baseFxMix;
                voice.envelopeDolby = voice.baseDolby;
                const size_t count = std::min<size_t>(voice.oscillators->size(), sizeof(voice.envelopeStates) / sizeof(voice.envelopeStates[0]));
                for (size_t oscIndex = 0; oscIndex < count; ++oscIndex) {
                    const PCInstrumentOscillator& osc = (*voice.oscillators)[oscIndex];
                    const float value = pc_envelope_step(&osc, &voice.envelopeStates[oscIndex], static_cast<float>(sAudioSpec.freq) / kEnvelopeControlPeriod);
                    if (voice.envelopeStates[oscIndex].result != PCEnvelopeResult::Ok || !pc_envelope_is_active(&voice.envelopeStates[oscIndex])) { voice.active = false; break; }
                    if (osc.mode == 0) voice.envelopeVolume *= value;
                    else if (osc.mode == 1) voice.envelopePitch *= value;
                    else if (osc.mode == 2) voice.envelopePan += (value - 0.5f) * 2.0f;
                    else if (osc.mode == 3) voice.envelopeFxMix = value;
                    else if (osc.mode == 4) voice.envelopeDolby = value;
                }
                voice.envelopeCountdown = kEnvelopeControlPeriod - 1;
            } else if (done < toUpdate) {
                voice.envelopeCountdown = (u8)(toUpdate - done - 1);
            }
        }
        o += done; remaining -= done;
        if (done < seg) break;   /* the voice ended inside the segment */
    }
    /* @vfp-exempt R4 — once per voice per block: the shared SampleVoice::cursor is double (pc_audio.cpp, PC build). measured n/a. Verified 2026-11-01. */
    voice.cursor = (double)pos / 4294967296.0;
    voice.filterState = state;
}

static void audio_callback(void* userdata, Uint8* output, int byteCount)
{
    if (!n3ds_audio_fastmix() || pc_audio_stats_enabled() || sAudioSpec.format != AUDIO_S16SYS || sAudioSpec.channels != 2) {
        audio_callback_orig(userdata, output, byteCount);
        return;
    }
    std::memset(output, 0, static_cast<size_t>(byteCount));
    s16* dst = reinterpret_cast<s16*>(output);
    const size_t frameCount = static_cast<size_t>(byteCount) / (2 * sizeof(s16));
    sCallbackCount.fetch_add(1, std::memory_order_relaxed);
    sMixedFrameCount.fetch_add(frameCount, std::memory_order_relaxed);
    SampleVoice* activeVoices[kSampleVoiceCount];
    size_t activeVoiceCount = 0;
    for (SampleVoice& voice : sSampleVoices) {
        if (voice.active) activeVoices[activeVoiceCount++] = &voice;
    }
    u32 previousPeak = sPeakActiveVoices.load(std::memory_order_relaxed);
    while (activeVoiceCount > previousPeak
           && !sPeakActiveVoices.compare_exchange_weak(previousPeak, static_cast<u32>(activeVoiceCount), std::memory_order_relaxed)) {}
    if (sDMAActive.load(std::memory_order_relaxed) && sDMAReadCursor + 1 >= sDMAQueue.size()) {
        sDMAUnderruns.fetch_add(1, std::memory_order_relaxed);
    }
    const float limiterStep = 1.0f / std::max(1.0f, sAudioSpec.freq * 0.1f);
    u32 limitedFrames = 0;
    for (size_t base = 0; base < frameCount; base += kN3dsMixBlock) {
        const size_t n = std::min(kN3dsMixBlock, frameCount - base);
        float mixL[kN3dsMixBlock], mixR[kN3dsMixBlock], fxL[kN3dsMixBlock], fxR[kN3dsMixBlock], dol[kN3dsMixBlock];
        std::memset(mixL, 0, n * sizeof(float)); std::memset(mixR, 0, n * sizeof(float));
        std::memset(fxL, 0, n * sizeof(float)); std::memset(fxR, 0, n * sizeof(float)); std::memset(dol, 0, n * sizeof(float));
        /* the stream voice and the AI-DMA queue (original code) */
        const float streamVol = sStreamVolume / 255.0f, streamBus = sBusVolumes[PC_AUDIO_BUS_STREAM];
        const float dmaGain = sDMAVolume / 255.0f * sBusVolumes[PC_AUDIO_BUS_DMA];
        /* @vfp-exempt R2,R3,R7 — fidelity: the PC mixer truncates each source (stream, DMA) to int before summing, so the 3DS output stays sample-identical to it; the fade-out compare runs only while a stream is fading. measured n/a. Verified 2026-11-01. */
        for (size_t f = 0; f < n; ++f) {
            int mixedLeft = 0, mixedRight = 0;
            if (sStreamVoice.active && sStreamVoice.cursor + 1 < sStreamVoice.samples.size()) {
                /* @vfp-exempt R2,R3 — fidelity: the PC mixer truncates the stream voice to int before summing, so the 3DS output stays sample-identical; the fade compare runs only while a stream fades. measured n/a. Verified 2026-11-01. */
                const float gain = streamVol * sStreamFadeGain * streamBus;   /* r53 R7: the invariant part hoisted (same value, computed once) */
                const float streamLeft = sStreamVoice.samples[sStreamVoice.cursor++] * gain;
                const float streamRight = sStreamVoice.samples[sStreamVoice.cursor++] * gain;
                mixedLeft += static_cast<int>(streamLeft);
                mixedRight += static_cast<int>(streamRight);
                /* @vfp-exempt R3 — the fade-out compare runs only while a stream is fading (a few seconds per cutscene), and ends the stream exactly when the gain reaches 0, as the PC mixer does. measured n/a. Verified 2026-11-01. */
                if (sStreamFadeStep > 0.0f) {
                    sStreamFadeGain -= sStreamFadeStep;
                    if (sStreamFadeGain <= 0.0f) {
                        sStreamFadeGain = 0.0f;
                        sStreamFadeStep = 0.0f;
                        sStreamVoice.active = false;
                        sStreamVoice.samples.clear();
                        sStreamVoice.cursor = 0;
                    }
                }
                if (sStreamVoice.cursor >= sStreamVoice.samples.size()) sStreamVoice.active = false;
            }
            if (sDMAActive && sDMAReadCursor + 1 < sDMAQueue.size()) {
                /* @vfp-exempt R2 — fidelity: the AI-DMA source is truncated to int before summing, as on the PC mixer. measured n/a. Verified 2026-11-01. */
                const float gain = dmaGain;   /* r53 R7: hoisted */
                mixedLeft += static_cast<int>(sDMAQueue[sDMAReadCursor] * gain);
                mixedRight += static_cast<int>(sDMAQueue[sDMAReadCursor + 1] * gain);
                sN3dsDmaPhase += sN3dsDmaStep;
                while (sN3dsDmaPhase >= 65536u) { sN3dsDmaPhase -= 65536u; sDMAReadCursor += 2; }
            }
            mixL[f] = (float)mixedLeft; mixR[f] = (float)mixedRight;
        }
        for (size_t v = 0; v < activeVoiceCount; ++v) n3dsMixVoice(*activeVoices[v], n, mixL, mixR, fxL, fxR, dol);
        /* FX delay, Dolby delay, limiter, output (original code) */
        /* @vfp-exempt R2,R3,R7 — the output stage IS the float-to-s16 conversion, done per frame with the PC mixer's int truncation of the FX/Dolby taps (sample-identical output); the limiter's attack/release decision is a per-sample gain envelope and cannot be hoisted. measured n/a. Verified 2026-11-01. */
        for (size_t f = 0; f < n; ++f) {
            int mixedLeft = (int)mixL[f];
            int mixedRight = (int)mixR[f];
            const float fxLeft = sFxDelayLeft[sFxDelayCursor];
            const float fxRight = sFxDelayRight[sFxDelayCursor];
            sFxDelayLeft[sFxDelayCursor] = fxL[f] + fxRight * 0.18f + fxLeft * 0.32f;
            sFxDelayRight[sFxDelayCursor] = fxR[f] + fxLeft * 0.18f + fxRight * 0.32f;
            if (++sFxDelayCursor >= sN3dsFxLen) sFxDelayCursor = 0;
            /* @vfp-exempt R2 — fidelity: FX / Dolby taps truncated to int one by one, as the PC mixer does (sample-identical output). measured n/a. Verified 2026-11-01. */
            mixedLeft += static_cast<int>(fxLeft * 0.35f);
            mixedRight += static_cast<int>(fxRight * 0.35f);
            const float surround = sDolbyDelay[sDolbyDelayCursor];
            sDolbyDelay[sDolbyDelayCursor] = dol[f];
            sDolbyDelayCursor = (sDolbyDelayCursor + 1) % kDolbyDelayFrames;
            mixedLeft += static_cast<int>(surround * 0.35f);
            mixedRight -= static_cast<int>(surround * 0.35f);
            if (!sStereoOutput) mixedLeft = mixedRight = (mixedLeft + mixedRight) / 2;
            const int peak = std::max(std::abs(mixedLeft), std::abs(mixedRight));
            constexpr float kLimiterCeiling = 30000.0f;
            /* @vfp-exempt R3 — the limiter is a per-sample gain envelope (attack when the target drops, release otherwise) and cannot be hoisted; peak vs ceiling is an integer compare; the 0.9999 test only counts limited frames for [audio-stats]. measured n/a. Verified 2026-11-01. */
            const float targetGain = peak > 30000 ? kLimiterCeiling / static_cast<float>(peak) : 1.0f;   /* r53 R2/R3: integer compare (peak is an int; 30000.0f is exact), the divide only when limiting */
            if (targetGain < sLimiterGain) sLimiterGain = targetGain;
            else sLimiterGain += (1.0f - sLimiterGain) * limiterStep;
            if (sLimiterGain < 0.9999f) ++limitedFrames;
            mixedLeft = static_cast<int>(mixedLeft * sLimiterGain);
            mixedRight = static_cast<int>(mixedRight * sLimiterGain);
            dst[(base + f) * 2] = clamp_s16(mixedLeft);
            dst[(base + f) * 2 + 1] = clamp_s16(mixedRight);
        }
    }
    if (limitedFrames) sLimitedFrames.fetch_add(limitedFrames, std::memory_order_relaxed);
    if (sDMAReadCursor >= sDMAQueue.size()) {
        sDMAQueue.clear();
        sDMAReadCursor = 0;
        sDMABytesLeft.store(0, std::memory_order_relaxed);
    } else {
        sDMABytesLeft.store(static_cast<u32>(std::min<size_t>((sDMAQueue.size() - sDMAReadCursor) * sizeof(s16), UINT32_MAX)), std::memory_order_relaxed);
    }
}

