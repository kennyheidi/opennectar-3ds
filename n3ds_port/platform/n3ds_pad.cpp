// GameCube PAD API on top of the 3DS HID. Replaces upstream pad_stubs.cpp (which is SDL-based).
// Mapping: circle pad = stick, C-stick (New 3DS) = substick, D-pad = D-pad, A/B/X/Y/Start direct,
// L/R = L/R (digital + full analog), ZL / ZR = Z, Select = F1 (settings menu toggle). Rumble: not available.
// C-stick: radial dead zone + 74-count GameCube range (n3ds_cstick.h; nectar.ini cstick_dead / cstick_range).
// Stick scaling matches upstream pc_window.cpp: +-127 (never -128), per-axis dead zone of 8 counts (upstream default).
#include <string.h>
#include "n3ds_bridge.h"
#include "Dolphin/pad.h"
#include "n3ds_config.h"
#include "n3ds_cstick.h"

static PADStatus sPad[PAD_MAX_CONTROLLERS];

// F1 on the PC port calls this (pc_settings.cpp). The 3DS build does not compile that file yet, so it is declared weak:
// the call is skipped while the symbol is absent and starts working as soon as the settings menu is linked in.
extern "C" void pc_settings_request_toggle(void) __attribute__((weak));
static bool sSelectHeld = false;
static N3dsCStickMap sCStick;
static bool sCStickInit = false;

static s8 scaleStick(int v)
{
    v = v * 127 / 150; /* circle pad reports about +-156 */
    if (v > 127) v = 127;
    if (v < -127) v = -127;            /* -128 would wrap when the game negates it (upstream pc_pad_axis_from_sdl clamps the same way) */
    if (v <= 8 && v >= -8) v = 0;      /* upstream sStickDeadZone default = 8; circle pads rest at about +-3..8 */
    return (s8)v;
}

extern "C" {

BOOL PADInit(void)
{
    memset(sPad, 0, sizeof(sPad));
    for (int i = 0; i < PAD_MAX_CONTROLLERS; i++) sPad[i].err = PAD_ERR_NO_CONTROLLER;
    sPad[0].err = PAD_ERR_NONE;
    return TRUE;
}
BOOL PADReset(u32) { return TRUE; }

u32 PADRead(PADStatus* status)
{
    N3dsInput in;
    n3ds_input_poll(&in);
    PADStatus& p = sPad[0];
    p.button = 0;
    if (in.buttons & N3DS_BTN_A) p.button |= PAD_BUTTON_A;
    if (in.buttons & N3DS_BTN_B) p.button |= PAD_BUTTON_B;
    if (in.buttons & N3DS_BTN_X) p.button |= PAD_BUTTON_X;
    if (in.buttons & N3DS_BTN_Y) p.button |= PAD_BUTTON_Y;
    if (in.buttons & N3DS_BTN_START) p.button |= PAD_BUTTON_START;
    if (in.buttons & N3DS_BTN_UP) p.button |= PAD_BUTTON_UP;
    if (in.buttons & N3DS_BTN_DOWN) p.button |= PAD_BUTTON_DOWN;
    if (in.buttons & N3DS_BTN_LEFT) p.button |= PAD_BUTTON_LEFT;
    if (in.buttons & N3DS_BTN_RIGHT) p.button |= PAD_BUTTON_RIGHT;
    if (in.buttons & N3DS_BTN_L) p.button |= PAD_TRIGGER_L;
    if (in.buttons & N3DS_BTN_R) p.button |= PAD_TRIGGER_R;
    if (in.buttons & (N3DS_BTN_ZL | N3DS_BTN_ZR)) p.button |= PAD_TRIGGER_Z;
    const bool sel = (in.buttons & N3DS_BTN_SELECT) != 0;   /* SELECT = F1: fire once per press, not while held */
    if (sel && !sSelectHeld && pc_settings_request_toggle) pc_settings_request_toggle();
    sSelectHeld = sel;
    p.triggerLeft = (in.buttons & N3DS_BTN_L) ? 255 : 0;
    p.triggerRight = (in.buttons & N3DS_BTN_R) ? 255 : 0;
    p.stickX = scaleStick(in.stickX);
    p.stickY = scaleStick(in.stickY);
    if (!sCStickInit) {                                    /* C-stick: GameCube response (see n3ds_cstick.h) */
        sCStickInit = true;
        const N3dsConfig& cfg = n3dsConfig();
        sCStick.dead = cfg.cstickDead < 0 ? 0 : cfg.cstickDead; sCStick.fixedRange = cfg.cstickRange;
    }
    {
        const float pk0 = sCStick.peak;
        signed char sx, sy;
        sCStick.map(in.cstickX, in.cstickY, sx, sy);
        p.substickX = sx; p.substickY = sy;
        static unsigned sLogged = 0;                       /* verification: shows whether hidCstickRead delivers anything and how far the nub travels */
        if (sCStick.peak >= pk0 + 20.0f && sLogged < 12) { ++sLogged; n3ds_log("[pad] C-stick raw (%d,%d) radius peak %.0f -> GC substick (%d,%d)", in.cstickX, in.cstickY, sCStick.peak, (int)sx, (int)sy); }
    }
    p.err = PAD_ERR_NONE;

    memcpy(status, sPad, sizeof(sPad));
    return 0xf0000000; /* channel 0 connected (same as upstream) */
}
void PADSetSamplingRate(u32) {}
void PADClamp(PADStatus*) {}
void PADClampCircle(PADStatus*) {}
void PADControlAllMotors(const u32*) {}
void PADControlMotor(s32, u32) {}
BOOL PADRecalibrate(u32) { return TRUE; }
BOOL PADSync(void) { return TRUE; }
void PADSetAnalogMode(u32) {}
void PADSetSpec(u32) {}
u32  PADGetSpec(void) { return PAD_SPEC_5; }
int  PADGetType(s32, u32* type) { if (type) *type = 0; return 0; }
#if OS_BUILD_VERSION >= 20011112L
PADSamplingCallback PADSetSamplingCallback(PADSamplingCallback) { return nullptr; }
#else
void PADSetSamplingCallback(PADSamplingCallback) {}
#endif

} // extern "C"
