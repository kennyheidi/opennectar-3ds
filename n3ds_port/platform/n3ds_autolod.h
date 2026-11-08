// Adaptive render distance ("auto_lod"). Pure C++ with no 3DS dependencies so tools/host_test can exercise it.
//
// WHY: the model cache draw (lod_px / lod_model_px) trades picture for speed with FIXED thresholds. Logs from the field show the worker needing 25 ms
// per frame in one spot and 65 ms in the next, so a fixed threshold is either too timid (the field runs at 10 FPS) or throws detail away in
// places that were already fast. This controller moves both thresholds up and down between LEVELS, only when it can help:
//   * it RAISES a level when frames are being presented slower than the target AND the render worker's display-list time says the worker is the
//     bottleneck (the display-list time is exactly the part the thresholds can reduce);
//   * it never raises while the game thread is the bottleneck (menus, the world map): culling models cannot help there;
//   * it LOWERS a level only after several quiet windows in a row (hysteresis), and waits longer each time it had to come straight back up.
// One call to frame() per presented frame, on the render worker (the thread that reads the thresholds), so no locking is needed.
#pragma once

struct N3dsAutoLod {
    // tunables (public so the host test can print them)
    static constexpr double kWindowMs     = 500.0;   // decide at most every ~0.5 s of presented frames ...
    static constexpr int    kWindowFrames = 6;       // ... and never from fewer than this many frames
    static constexpr double kHitchMs      = 250.0;   // a frame period longer than this (stage load, pause, HOME menu) is ignored
    static constexpr double kMissFactor   = 1.07;    // average period above target * this = "not making the target"
    static constexpr double kOkFactor     = 1.04;    // at or below target * this = "making it"
    static constexpr double kBusyDlMs     = 16.0;    // display-list time per frame above this = the worker is the bottleneck, culling can help
    static constexpr double kQuietDlMs    = 12.0;    // below this (and making the target) the window counts as quiet
    static constexpr double kCachedShare  = 0.35;    // the cached draws must be at least this share of the display-list time for culling to be able to help
    static constexpr int    kCalmStart    = 6;       // quiet windows (~3 s) needed before a level is given back
    static constexpr int    kCalmMax      = 96;
    static constexpr int    kBounceWindows = 10;     // going up again within this many windows of coming down = the step down was too early

    int level = 0;               // 0 = thresholds exactly as in nectar.ini
    int maxLevel = 0;            // 0 = controller off
    double targetMs = 1000.0 / 30.0;

    void configure(int maxLvl, double target)
    {
        maxLevel = maxLvl < 0 ? 0 : maxLvl > kLevels - 1 ? kLevels - 1 : maxLvl;
        targetMs = target > 1.0 ? target : 1000.0 / 30.0;
        if (level > maxLevel) level = maxLevel;
    }

    // Threshold multiplier of a level. Level 3 doubles both thresholds: lod_model_px 80 -> 160 (models under 16 px across), lod_px 30 -> 60.
    static constexpr int kLevels = 6;
    static float mult(int lvl)
    {
        static const float m[kLevels] = { 1.0f, 1.25f, 1.6f, 2.0f, 2.5f, 3.2f };
        return m[lvl < 0 ? 0 : lvl >= kLevels ? kLevels - 1 : lvl];
    }

    // periodMs: time since the previous presented frame. dlMs: the worker's display-list time for the frame just drawn.
    // cachedMs: the part of dlMs spent drawing models from the model cache (the only draws the thresholds can skip); < 0 = unknown, treated as all of it.
    // Field log 4a5c3ceb: the model cache was refused (linear memory), every list took the normal path (124-135 ms/frame) and the controller climbed to
    // its top level for nothing. With no cached draws culling cannot help, so a level is raised only when the cached draws are a real share of the work.
    // Returns true when `level` changed.
    bool frame(double periodMs, double dlMs, double cachedMs = -1.0)
    {
        if (maxLevel <= 0) return false;
        if (periodMs > kHitchMs || periodMs <= 0.0) return false;
        sumP_ += periodMs; sumD_ += dlMs; sumC_ += (cachedMs < 0.0 ? dlMs : cachedMs); ++n_;
        if (sumP_ < kWindowMs || n_ < kWindowFrames) return false;
        const double avgP = sumP_ / n_, avgD = sumD_ / n_, avgC = sumC_ / n_;
        sumP_ = sumD_ = sumC_ = 0.0; n_ = 0;
        ++windows_;
        const bool missing = avgP > targetMs * kMissFactor;
        const bool making  = avgP <= targetMs * kOkFactor;
        if (missing && avgD > kBusyDlMs && avgC >= kCachedShare * avgD) {
            calm_ = 0;
            if (level < maxLevel) {
                if (lastDownWindow_ >= 0 && windows_ - lastDownWindow_ <= kBounceWindows) { calmNeeded_ = calmNeeded_ * 2 > kCalmMax ? kCalmMax : calmNeeded_ * 2; }
                ++level; return true;
            }
            return false;
        }
        if (making && avgD < kQuietDlMs) {
            if (level > 0 && ++calm_ >= calmNeeded_) { calm_ = 0; --level; lastDownWindow_ = windows_; return true; }
            return false;
        }
        calm_ = 0;   // neither clearly missing nor clearly quiet: hold
        return false;
    }

    int calmNeeded() const { return calmNeeded_; }

private:
    double sumP_ = 0.0, sumD_ = 0.0, sumC_ = 0.0;
    int n_ = 0, calm_ = 0, calmNeeded_ = kCalmStart;
    int windows_ = 0, lastDownWindow_ = -1000;
};
