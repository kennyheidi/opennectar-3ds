// New 3DS C-stick -> GameCube C-stick (PADStatus.substickX/Y). Pure header (host-testable).
//
// What the game expects (decomp: Controller::getSubStickX() = mSubStickX / 74.0f; ControllerMgr treats |value| > PAD_MIN_STICK_READ (32) as a
// C-stick "direction pressed"): a GameCube C-stick reports about +-74 at full travel, in a round gate, 0 at rest. The 3DS C-stick is a
// small nub that reports about +-100..150 depending on the unit, so the raw value is mapped radially:
//     radius <= dead            -> 0
//     dead..range               -> 0..74 (linear, direction kept)
//     radius >= range           -> 74
// Radial mapping keeps diagonals the right length (per-axis clamping would turn a 45-degree push into 1.41x), which matters because the
// game uses the C-stick vector as the squad steering direction and thresholds it at 32/74.
#pragma once
#include <math.h>

#define N3DS_GC_STICK_FULL 74

struct N3dsCStickMap {
    int   dead  = 12;        // raw counts
    int   fixedRange = 0;    // 0 = learn
    float peak  = 0.0f;      // largest raw radius seen so far (learned range)
    // out: GameCube substick values (-74..74). rawX/rawY as returned by hidCstickRead (up = +Y, right = +X, same as the GameCube).
    void map(int rawX, int rawY, signed char& ox, signed char& oy)
    {
        const float x = (float)rawX, y = (float)rawY, r = sqrtf(x * x + y * y);
        if (r > peak) peak = r > 160.0f ? 160.0f : r;
        // learned range: 85 % of the farthest push seen, but never less than 60 (a stick that has hardly been moved yet) nor more than 150
        float range = fixedRange > 0 ? (float)fixedRange : peak * 0.85f;
        if (fixedRange <= 0) { if (range < 60.0f) range = 60.0f; if (range > 150.0f) range = 150.0f; }
        const float dead_ = (float)dead;
        if (r <= dead_ || range <= dead_ + 1.0f) { ox = oy = 0; return; }
        float t = (r - dead_) / (range - dead_);
        if (t > 1.0f) t = 1.0f;
        const float k = t * (float)N3DS_GC_STICK_FULL / r;
        ox = (signed char)lroundf(x * k);
        oy = (signed char)lroundf(y * k);
    }
};
