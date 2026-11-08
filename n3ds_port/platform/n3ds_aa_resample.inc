/* ---- 3DS: anti-aliased wave cache (spliced into the build-time copy of pc_audio.cpp, see n3ds_port/CMakeLists.txt) ----
   The mixer plays a decoded wave by stepping through it at sampleRate / outputRate and linearly interpolating between two neighbouring
   samples. With the GameCube's 32 kHz waves and nectar.ini audio_rate = 11025 the step is ~2.9: only every third sample is looked at and everything
   above 5.5 kHz folds back into the audible range as a constant hiss / grit (a low-pass AFTER the interpolation, like the per-voice cutoff filter,
   cannot remove it any more). So when a wave is decoded for the first time and its rate is above the output rate, it is low-passed and resampled
   ONCE to the output rate (windowed-sinc low-pass at ~0.46 of the output rate, 6 output periods per side). The cache gets 3x smaller too,
   and the mixer then steps by ~1.0 instead of ~2.9. nectar.ini audio_aa = 0 switches it off. */
extern "C" int n3ds_audio_aa(void);
static std::unordered_map<u64, double> gN3dsWaveRatio;   /* cacheKey -> outputRate / sourceRate, only for waves that were resampled */
static void n3dsAntiAlias(std::vector<s16>& v, double srcRate, int outRate, u64 key)
{
    if (!n3ds_audio_aa() || outRate <= 0 || srcRate <= outRate * 1.05 || v.size() < 32) return;
    const double ratio = (double)outRate / srcRate;            /* < 1: output samples per source sample */
    const long n = (long)v.size();
    const size_t m = (size_t)(n * ratio);
    if (m < 2) return;
    /* kernel table: h(t), t = distance in OUTPUT sample periods, |t| < L, Hann-windowed sinc with cutoff fc (cycles per output sample) */
    static const int L = 6, RES = 256;
    static float tab[6 * 256 + 2]; static bool tabInit = false;
    if (!tabInit) {
        const double fc = 0.46, pi = 3.14159265358979;
        for (int i = 0; i <= L * RES + 1; ++i) {
            const double x = (double)i / RES;
            const double s = x < 1e-9 ? 2 * fc : sin(2 * pi * fc * x) / (pi * x);
            const double w = x >= L ? 0.0 : 0.5 + 0.5 * cos(pi * x / L);
            tab[i] = (float)(s * w);
        }
        tabInit = true;
    }
    std::vector<s16> out(m);
    const double reach = L / ratio;                            /* source samples on each side */
    const float rf = (float)ratio;
    for (size_t k = 0; k < m; ++k) {
        const double c = (double)k / ratio;
        long j0 = (long)std::ceil(c - reach), j1 = (long)std::floor(c + reach);
        if (j0 < 0) j0 = 0;
        if (j1 > n - 1) j1 = n - 1;
        float acc = 0.0f, wsum = 0.0f;
        for (long j = j0; j <= j1; ++j) {
            float t = (float)((double)j - c) * rf; if (t < 0) t = -t;
            const float h = tab[(int)(t * RES + 0.5f)];
            acc += h * v[(size_t)j]; wsum += h;
        }
        const float r = wsum > 1e-6f ? acc / wsum : 0.0f;
        out[k] = (s16)(r > 32767.0f ? 32767.0f : (r < -32768.0f ? -32768.0f : r));
    }
    v.swap(out);
    gN3dsWaveRatio[key] = ratio;
}
