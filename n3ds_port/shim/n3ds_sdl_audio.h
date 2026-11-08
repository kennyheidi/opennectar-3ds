// Minimal SDL2 audio API for the 3DS build: just what pc_port/audio/pc_audio.cpp uses, implemented on NDSP in
// platform/n3ds_sdl_audio.cpp. Not a general SDL replacement.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stddef.h>
/* Uint8..Uint64 come from the including SDL2/SDL.h shim */

#define SDL_INIT_AUDIO 0x00000010u
#define AUDIO_S16SYS   0x8010
#define SDL_zero(x)    memset(&(x), 0, sizeof(x))

typedef Uint32 SDL_AudioDeviceID;
typedef Uint16 SDL_AudioFormat;
typedef void (*SDL_AudioCallback)(void* userdata, Uint8* stream, int len);

typedef struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void* userdata;
} SDL_AudioSpec;

typedef struct SDL_AudioStream SDL_AudioStream;

#ifdef __cplusplus
extern "C" {
#endif
int  SDL_InitSubSystem(Uint32 flags);
void SDL_QuitSubSystem(Uint32 flags);
const char* SDL_GetError(void);
SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* desired, SDL_AudioSpec* obtained, int allowed_changes);
void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on);
void SDL_LockAudioDevice(SDL_AudioDeviceID dev);
void SDL_UnlockAudioDevice(SDL_AudioDeviceID dev);
void SDL_CloseAudioDevice(SDL_AudioDeviceID dev);
void SDL_ClearQueuedAudio(SDL_AudioDeviceID dev);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);
SDL_AudioStream* SDL_NewAudioStream(SDL_AudioFormat src_format, Uint8 src_channels, int src_rate, SDL_AudioFormat dst_format, Uint8 dst_channels, int dst_rate);
int  SDL_AudioStreamPut(SDL_AudioStream* stream, const void* buf, int len);
int  SDL_AudioStreamFlush(SDL_AudioStream* stream);
int  SDL_AudioStreamAvailable(SDL_AudioStream* stream);
int  SDL_AudioStreamGet(SDL_AudioStream* stream, void* buf, int len);
void SDL_FreeAudioStream(SDL_AudioStream* stream);
#ifdef __cplusplus
}
#endif
