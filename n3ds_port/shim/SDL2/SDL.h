// Compile-time stand-in for the handful of SDL2 symbols the decompiled game code and
// pc_window.h reference. The 3DS build has no SDL: input comes from libctru HID via
// n3ds_port/platform, which implements the pc_window_* API. Types only -- nothing here links.
#ifndef N3DS_SDL_SHIM_H
#define N3DS_SDL_SHIM_H
#include <stdint.h>
typedef uint8_t  Uint8;  typedef uint16_t Uint16; typedef uint32_t Uint32; typedef uint64_t Uint64;
typedef int8_t   Sint8;  typedef int16_t  Sint16; typedef int32_t  Sint32; typedef int64_t  Sint64;
typedef enum { SDL_SCANCODE_UNKNOWN = 0, SDL_SCANCODE_F5 = 62, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8,
               SDL_NUM_SCANCODES = 512 } SDL_Scancode;
typedef enum { SDL_CONTROLLER_BUTTON_INVALID = -1, SDL_CONTROLLER_BUTTON_MAX = 21 } SDL_GameControllerButton;
typedef struct _SDL_GameController SDL_GameController;
#define SDL_BUTTON(X)  (1 << ((X) - 1))
#define SDL_BUTTON_X1  4
#ifdef __cplusplus
extern "C" {
#endif
static inline const Uint8* SDL_GetKeyboardState(int* numkeys) { if (numkeys) *numkeys = 0; return 0; }
unsigned n3ds_time_ms(void);   /* platform/n3ds_ctru_io.cpp */
static inline Uint32 SDL_GetTicks(void) { return (Uint32)n3ds_time_ms(); }   /* 0.9.3 game code (PC-port blocks) calls it */
#ifdef __cplusplus
}
#endif
#include "../n3ds_sdl_audio.h"   /* audio device/stream API for the native mixer (platform/n3ds_sdl_audio.cpp) */
#endif
