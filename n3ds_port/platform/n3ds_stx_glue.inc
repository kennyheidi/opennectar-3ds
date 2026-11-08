// Spliced into the build-time copy of pc_port/audio/pc_audio.cpp in place of the head of pc_audio_play_stx() (see n3ds_port/CMakeLists.txt).
// The original function body follows this text as pc_audio_play_stx_impl(), which is no longer called.
extern "C" int  n3ds_stx_probe(const char* path, int dstRate, unsigned* outFrames, unsigned* srcRate);
extern "C" void n3ds_stx_start(const char* path, short* dst, unsigned outFrames, int dstRate);
extern "C" int  n3ds_stx_wait_ready(unsigned wantFrames, unsigned timeoutMs);
extern "C" void n3ds_stx_cancel(void);
__attribute__((unused)) static bool pc_audio_play_stx_impl(const char* path);
bool pc_audio_play_stx(const char* path) {
    if (!n3ds_audio_stx_enabled()) return false;
    if (!path || !pc_audio_init()) return false;
    n3ds_stx_cancel();                                   // the old decoder must be gone before its buffer is replaced
    unsigned frames = 0, srcRate = 0;
    if (!n3ds_stx_probe(path, sAudioSpec.freq, &frames, &srcRate)) return false;
    std::vector<s16> buffer(static_cast<size_t>(frames) * 2);          // zeroed; a background thread fills it in far ahead of playback
    SDL_LockAudioDevice(sAudioDevice);
    sStreamVoice.active = false;
    sStreamVoice.samples = std::move(buffer);
    sStreamVoice.cursor = 0;
    sStreamFadeGain = 1.0f;
    sStreamFadeStep = 0.0f;
    SDL_UnlockAudioDevice(sAudioDevice);
    n3ds_stx_start(path, sStreamVoice.samples.data(), frames, sAudioSpec.freq);
    n3ds_stx_wait_ready(frames < 16000u ? frames : 16000u, 400);       // ~0.5 s decoded first, so playback never starts on silence
    SDL_LockAudioDevice(sAudioDevice);
    sStreamVoice.active = true;
    SDL_UnlockAudioDevice(sAudioDevice);
    printf("[PC Port] Playing STX stream: %s (%u Hz, %u frames, background decode)\n", path, srcRate, frames);
    return true;
}
static bool pc_audio_play_stx_impl(const char* path) {
