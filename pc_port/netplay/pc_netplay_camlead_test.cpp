// Host test for the netplay lead camera core
// (pc_netplay_camlead_core.h). No game code: key mapping, input history
// (and its missing-landing-frame count), prediction window (gaps, frozen
// controller, the pause rule), the correction filter (it tracks two
// simulated homing cameras to float rounding while active, and the
// correction itself blends back to exactly zero) and the switch parsing.

#include "netplay/pc_netplay_camlead_core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace pc_netplay_camlead;

static int sFailures = 0;

static void check(bool ok, const char* what)
{
	if (!ok) {
		std::printf("FAIL: %s\n", what);
		++sFailures;
	}
}

static PcNetplayInput input(uint16_t buttons, int8_t stickX = 0, uint8_t trigL = 0, int8_t subY = 0)
{
	PcNetplayInput in;
	in.buttons   = buttons;
	in.stickX    = stickX;
	in.triggerL  = trigL;
	in.substickY = subY;
	return in;
}

static void test_keys()
{
	check(camera_keys(input(0)) == 0, "neutral maps to no keys");
	check(camera_keys(input(kPadL)) == kKeyL, "PAD L -> KBBTN_L");
	check(camera_keys(input(kPadR)) == kKeyR, "PAD R -> KBBTN_R");
	check(camera_keys(input(kPadZ)) == kKeyZ, "PAD Z -> KBBTN_Z");
	check(camera_keys(input(kPadX)) == kKeyX, "PAD X -> KBBTN_X");
	check(camera_keys(input(kPadStart)) == kKeyStart, "PAD Start -> KBBTN_START");
	// A, B, Y and the d-pad are not camera controls.
	check(camera_keys(input(0x0100 | 0x0200 | 0x0800 | 0x000F)) == 0, "non-camera buttons ignored");
}

static void test_history()
{
	InputHistory h;
	h.clear();
	PcNetplayInput out;
	check(!h.get(5, &out), "empty history misses");
	h.note(5, input(kPadR));
	check(h.get(5, &out) && out.buttons == kPadR, "noted frame found");
	check(!h.get(5 + InputHistory::kSize, &out), "aliased frame misses");
	h.note(5 + InputHistory::kSize, input(kPadZ));
	check(!h.get(5, &out), "overwritten slot misses the old frame");
	check(h.get(5 + InputHistory::kSize, &out) && out.buttons == kPadZ, "new frame found");
}

// Review M2: a submit path that does not note its landing frame leaves a
// hole in the pending window, which the engine counts as `gaps`.
static void test_history_gaps()
{
	InputHistory h;
	h.clear();
	check(h.missing_after(0) == 0, "empty history: no gaps");
	// Session start at delay 4: the first submit lands on 4; frames 1..3
	// were never submitted locally and are not gaps.
	for (uint64_t f = 4; f <= 8; ++f) h.note(f, input(0));
	check(h.missing_after(0) == 0, "frames before the first noted one are not gaps");
	check(h.missing_after(5) == 0, "contiguous pending window: no gaps");
	// A catch-up that noted only its own landing frame (not the GekkoNet
	// repeats) or a delay increase whose extra adds were not noted.
	h.note(11, input(0));
	check(h.missing_after(5) == 2, "frames 9 and 10 missing");
	check(h.missing_after(10) == 0, "consumed frames no longer count");
	h.note(9, input(0));
	h.note(10, input(0));
	check(h.missing_after(5) == 0, "filled");
	h.clear();
	check(h.missing_after(5) == 0, "clear resets the range");
}

static void test_steps()
{
	InputHistory h;
	h.clear();
	Step cur;
	cur.keys = kKeyL; // L held at the current frame
	Step out[kMaxSteps];
	// Only step 0 when nothing is pending.
	check(build_steps(h, 100, cur, false, out) == 1, "no pending frames: step 0 only");
	check(out[0].keys == kKeyL, "step 0 is the Kontroller state");
	// Pending 101..104: L released, R click, R held, Z click.
	h.note(101, input(0, 20, 30));
	h.note(102, input(kPadR));
	h.note(103, input(kPadR));
	h.note(104, input(kPadZ, -40, 0, 50));
	int n = build_steps(h, 100, cur, false, out);
	check(n == 5, "four pending frames");
	check(out[1].keys == 0 && out[1].pressed == 0 && out[1].stickX == 20 && out[1].triggerL == 30, "step 1 values");
	check(out[2].pressed == kKeyR, "R press edge");
	check(out[3].keys == kKeyR && out[3].pressed == 0, "R held has no edge");
	check(out[4].pressed == kKeyZ && out[4].stickX == -40 && out[4].substickY == 50, "Z press edge and analogs");
	// A gap ends the window.
	h.note(106, input(kPadZ));
	check(build_steps(h, 100, cur, false, out) == 5, "gap at 105 ends the window");
	// maxSteps caps it.
	check(build_steps(h, 100, cur, false, out, 3) == 3, "maxSteps caps the window");
	// Frozen: pending steps are neutral.
	n = build_steps(h, 100, cur, true, out);
	check(n == 5 && out[2].pressed == 0 && out[4].stickX == 0, "frozen controller feeds neutral steps");
	// Pause rule: a Start press at 103 cuts before 102 (m = 2: steps 0, 1).
	h.clear();
	h.note(101, input(0));
	h.note(102, input(kPadR));
	h.note(103, input(kPadStart));
	h.note(104, input(kPadZ));
	bool cut = false;
	n = build_steps(h, 100, cur, false, out, kMaxSteps, &cut);
	check(n == 2 && cut, "Start at 103: steps for 100 and 101 only");
	// Start already held at the current frame is not a new press.
	cur.keys = kKeyStart;
	h.clear();
	h.note(101, input(kPadStart));
	h.note(102, input(kPadStart | kPadR));
	n = build_steps(h, 100, cur, false, out, kMaxSteps, &cut);
	check(n == 3 && !cut, "held Start does not cut");
	// Start press at 101 cuts everything.
	cur.keys = 0;
	h.clear();
	h.note(101, input(kPadStart));
	n = build_steps(h, 100, cur, false, out, kMaxSteps, &cut);
	check(n == 0 && cut, "Start at 101 cuts step 0 too");
}

// Two 1-D cameras with the PcamCamera homing (watch toward a target, view
// toward polar + watch, fov toward a goal). The sim one applies a target
// change `lag` ticks after the ideal one; the correction driven by the
// target differences must reproduce ideal - sim exactly.
static void test_correction_matches_cameras()
{
	const float h = 0.15f, hf = 0.15f;
	float wS = 0, vS = 1000, fS = 30;
	float wI = 0, vI = 1000, fI = 30;
	Correction c;
	float maxErr = 0.0f;     // whole run (includes the blend-back snap)
	float maxErrLive = 0.0f; // while the correction is active
	const int lag = 5;
	for (int t = 0; t < 400; ++t) {
		// Target sequences: the ideal camera turns at t=20 and zooms at 60;
		// the sim camera sees the same `lag` ticks later.
		const float tgtI = (t >= 20) ? 50.0f : 0.0f;
		const float pvI  = (t >= 20) ? ((t >= 60) ? 700.0f : 900.0f) : 1000.0f;
		const float fovI = (t >= 60) ? 45.0f : 30.0f;
		const int ts     = t - lag;
		const float tgtS = (ts >= 20) ? 50.0f : 0.0f;
		const float pvS  = (ts >= 20) ? ((ts >= 60) ? 700.0f : 900.0f) : 1000.0f;
		const float fovS = (ts >= 60) ? 45.0f : 30.0f;
		wI += (tgtI - wI) * h;
		vI += (pvI + wI - vI) * h;
		fI += (fovI - fI) * hf;
		wS += (tgtS - wS) * h;
		vS += (pvS + wS - vS) * h;
		fS += (fovS - fS) * hf;
		const float dT[3] = { tgtI - tgtS, 0, 0 };
		const float dP[3] = { pvI - pvS, 0, 0 };
		check(c.step(dT, dP, fovI - fovS, h, hf), "finite step");
		const float err = std::fabs((vS + c.dv[0]) - vI) + std::fabs((wS + c.dw[0]) - wI) + std::fabs((fS + c.df) - fI);
		if (err > maxErr) maxErr = err;
		if (!c.zero() && err > maxErrLive) maxErrLive = err;
	}
	std::printf("correction vs cameras: max |lead - ideal| = %g while active, %g overall\n", maxErrLive, maxErr);
	check(maxErrLive < 1e-3f, "lead tracks the ideal camera (float rounding only)");
	check(maxErr < 3.0f * Correction::kPosEps, "the blend-back snap stays below the visual epsilon");
	check(c.zero(), "correction blended back to exactly zero");
}

static void test_correction_edges()
{
	Correction c;
	const float z[3] = { 0, 0, 0 };
	check(c.zero(), "starts at zero");
	check(c.step(z, z, 0.0f, 0.15f, 0.15f) && c.zero(), "zero inputs stay zero");
	const float big[3] = { 0, 0, 300.0f };
	c.step(z, big, 0.0f, 0.15f, 0.15f);
	check(!c.zero() && std::fabs(c.dv[2] - 45.0f) < 1e-3f, "one step moves 15% of a polar difference");
	// Disagreeing inputs never snap, however small the residual.
	Correction d;
	const float tiny[3] = { 0, 0, 1e-4f };
	d.step(z, tiny, 0.0f, 0.15f, 0.15f);
	check(!d.zero(), "a nonzero target difference is kept");
	const float inf[3] = { INFINITY, 0, 0 };
	check(!c.step(inf, z, 0.0f, 0.15f, 0.15f) && c.zero(), "non-finite resets");
	int ticks = 0;
	Correction e;
	e.step(z, big, 0.0f, 0.15f, 0.15f);
	while (!e.zero() && ticks < 1000) {
		e.step(z, z, 0.0f, 0.15f, 0.15f);
		++ticks;
	}
	std::printf("blend back from a 300-unit polar step: %d ticks to exactly zero\n", ticks);
	check(e.zero() && ticks < 120, "blends back within 4 s");
}

static void test_env()
{
	check(env_lead_enabled(nullptr), "unset: on");
	check(env_lead_enabled("1"), "1: on");
	check(!env_lead_enabled("0"), "0: off");
	check(env_lead_enabled("00"), "anything but 0: on");
	check(env_lead_enabled(""), "empty: on");
	check(env_joiner_own_camera(nullptr), "joiner own camera: unset on");
	check(!env_joiner_own_camera("0"), "joiner own camera: 0 off");
	check(env_joiner_own_camera("1"), "joiner own camera: 1 on");
}

int main()
{
	test_keys();
	test_history();
	test_history_gaps();
	test_steps();
	test_correction_matches_cameras();
	test_correction_edges();
	test_env();
	if (sFailures != 0) {
		std::printf("pc_netplay_camlead_test: %d failure(s)\n", sFailures);
		return 1;
	}
	std::printf("pc_netplay_camlead_test: all checks passed\n");
	return 0;
}
