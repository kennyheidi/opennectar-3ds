// CHECKER ONLY -- not libctru. Declares just the calls n3ds_port/platform uses, written from memory of
// libctru 2.x, so the sandbox (no devkitPro) can catch typos. The real check is devkitARM on your PC.
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32; typedef int16_t s16;
#define BIT(n) (1U << (n))
#define U64_MAX UINT64_MAX
#define CUR_THREAD_HANDLE 0xFFFF8000
enum { KEY_A=BIT(0),KEY_B=BIT(1),KEY_SELECT=BIT(2),KEY_START=BIT(3),KEY_DRIGHT=BIT(4),KEY_DLEFT=BIT(5),KEY_DUP=BIT(6),
       KEY_DDOWN=BIT(7),KEY_R=BIT(8),KEY_L=BIT(9),KEY_X=BIT(10),KEY_Y=BIT(11),KEY_ZL=BIT(14),KEY_ZR=BIT(15) };
typedef struct { s16 dx, dy; } circlePosition;
typedef enum { GFX_TOP=0, GFX_BOTTOM=1 } gfxScreen_t;
typedef enum { GFX_LEFT=0, GFX_RIGHT=1 } gfx3dSide_t;
typedef enum { MEMREGION_ALL=0, MEMREGION_APPLICATION=1 } MemRegion;
typedef struct Thread_tag* Thread;
typedef void (*ThreadFunc)(void*);
typedef int32_t Result;
extern "C" {
void osSetSpeedupEnable(bool); void gfxInitDefault(void); void gfxExit(void);
void* consoleInit(gfxScreen_t, void*);
u8* gfxGetFramebuffer(gfxScreen_t, gfx3dSide_t, u16* w, u16* h);
void gfxFlushBuffers(void); void gfxSwapBuffers(void); void gspWaitForVBlank(void);
bool aptMainLoop(void); Result APT_CheckNew3DS(bool*);
void hidScanInput(void); u32 hidKeysHeld(void); u32 hidKeysDown(void);
void hidCircleRead(circlePosition*); void hidCstickRead(circlePosition*);
u32 osGetMemRegionFree(MemRegion); u32 osGetMemRegionSize(MemRegion); bool envIsHomebrew(void); u32 linearSpaceFree(void);
void svcSleepThread(int64_t);
Result svcGetThreadPriority(s32*, u32);
Thread threadCreate(ThreadFunc, void*, size_t, int, int, bool);
Thread threadGetCurrent(void); int64_t osGetTime(void);
Result threadJoin(Thread, u64); void threadFree(Thread);
}

extern "C" { extern u32 __ctru_heap_size, __ctru_linear_heap_size; }
