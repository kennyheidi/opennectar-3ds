#pragma once
// Netplay lead (instant) camera, engine-free core. Header-only so the host
// test (pc_netplay_camlead_test) needs no game code; the engine side lives in
// pc_netplay_camlead.cpp.
//
// Problem. In a lockstep session each peer's own camera is a sim-side
// PcamCamera: it is updated inside GameCoreSection::update from its captain's
// Kontroller, which the session fills from the synced, delayed input. The
// presentation pass renders that camera, so a camera control (L-hold
// rotation, L click attention, R zoom, Z angle) shows up `delay` + 1 frames
// after the player pressed it (+1: the camera reads the Kontroller state from
// the previous tick's updateAI).
//
// Lead camera. The presentation pass renders the sim camera plus a
// correction, lead = sim + C, where C tracks the difference between an
// "ideal" local camera (one that has consumed every local input the moment
// it was sampled) and the sim camera. Both cameras run the same dynamics
// (PcamCamera::makePosture), which are linear in the smoothed positions:
//   W' = W + (Tgt - W) h           (watchpoint homing, h = HomingSpeed)
//   V' = V + (pv + W' - V) h       (viewpoint homing toward polar + target)
//   f' = f + (curFov - f) hf       (fov homing, hf = FovHomingSpeed)
// where Tgt, pv and curFov come from the camera's control state (azimuth,
// zoom and angle motion, attention). So the difference obeys the same
// recurrences driven by the difference of the control-derived targets:
//   dW' = dW + (dTgt - dW) h
//   dV' = dV + (dW' + dPv - dV) h
//   df' = df + (dCurFov - df) hf
// The ideal control state is obtained each presented frame by running the
// real PcamCamera control/update forward from a byte snapshot of the sim
// camera over the local inputs it has not consumed yet (the Kontroller's
// current input plus the submitted-but-not-applied frames), then restoring
// the snapshot. Control inputs are applied at the tick the sim will apply
// them, so each input's control effect has progressed exactly as far as it
// would have in a camera that applied it on the frame it was sampled.
// Consequences:
//   - the view responds on the next presented frame;
//   - the follow smoothing, camera shake and sim-driven snaps stay the sim
//     camera's (they are not part of C);
//   - when the pending inputs carry no camera control the targets agree
//     exactly (dTgt = dPv = dCurFov = 0), C decays with the camera's own
//     homing and snaps to exactly zero below a visual epsilon: the view is
//     then bit-for-bit the sim camera again ("blends back").
// Nothing here is sim state: C and the history live only in presentation.

#include "netplay/pc_netplay_adaptive.h"
#include "netplay/pc_netplay_gekko_input.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace pc_netplay_camlead {

// Controller.h KeyboardButtons bits the camera controls read (plus Start),
// duplicated so the host test needs no engine header. The engine TU
// static_asserts them against Controller.h.
constexpr uint32_t kKeyX     = 1u << 14;
constexpr uint32_t kKeyZ     = 1u << 16;
constexpr uint32_t kKeyL     = 1u << 17;
constexpr uint32_t kKeyR     = 1u << 18;
constexpr uint32_t kKeyStart = 1u << 24;

// Dolphin/pad.h PAD bits (same duplication rule).
constexpr uint16_t kPadZ     = 0x0010;
constexpr uint16_t kPadR     = 0x0020;
constexpr uint16_t kPadL     = 0x0040;
constexpr uint16_t kPadX     = 0x0400;
constexpr uint16_t kPadStart = 0x1000;

// The session's local input delay cap (frames).
constexpr unsigned kMaxLocalDelay = pc_netplay_adaptive::kMaxLocalDelay;

// Longest prediction: the Kontroller's current input plus up to 15 pending
// frames.
constexpr int kMaxSteps = 16;
// The walk needs the current input plus every pending frame the history can
// hold: frames past the sim frame land at most kMaxLocalDelay ahead, so
// kMaxLocalDelay + 1 steps at most. If the delay cap ever grows past this
// table, the prediction would silently truncate.
static_assert(kMaxSteps > kMaxLocalDelay + 1,
              "camlead step table must cover the current input plus kMaxLocalDelay pending frames");

// Camera-relevant KeyboardButtons of one input, mapped exactly as
// ControllerMgr::updateController maps PAD bits (Start included so the
// prediction can stop at a pause).
inline uint32_t camera_keys(const PcNetplayInput& in)
{
	uint32_t k = 0;
	if (in.buttons & kPadX) k |= kKeyX;
	if (in.buttons & kPadZ) k |= kKeyZ;
	if (in.buttons & kPadR) k |= kKeyR;
	if (in.buttons & kPadL) k |= kKeyL;
	if (in.buttons & kPadStart) k |= kKeyStart;
	return k;
}

// The local player's submitted inputs, keyed by the GekkoNet frame they land
// on (submit index + local delay). Small ring: the lead only ever looks a few
// frames past the current one.
class InputHistory {
public:
	static constexpr unsigned kSize = 64;

	void clear()
	{
		for (unsigned i = 0; i < kSize; ++i) mSlots[i].valid = false;
		mAny    = false;
		mFirst  = 0;
		mNewest = 0;
	}

	void note(uint64_t frame, const PcNetplayInput& in)
	{
		Slot& s = mSlots[frame % kSize];
		s.valid = true;
		s.frame = frame;
		s.in    = in;
		if (!mAny) {
			mAny    = true;
			mFirst  = frame;
			mNewest = frame;
		} else {
			if (frame < mFirst) mFirst = frame;
			if (frame > mNewest) mNewest = frame;
		}
	}

	bool get(uint64_t frame, PcNetplayInput* out) const
	{
		const Slot& s = mSlots[frame % kSize];
		if (!s.valid || s.frame != frame) return false;
		if (out != nullptr) *out = s.in;
		return true;
	}

	// Frames after `frame`, up to the newest noted one, that the history
	// does not hold although it should: every frame from the first noted
	// one on carries a local input, so a hole means a submit path did not
	// note its landing frame. That silently shortens the prediction window
	// (the view then leads less, or not at all), so the engine counts it
	// (`gaps` in the summary line: every submit site must note the frame
	// its input lands on).
	int missing_after(uint64_t frame) const
	{
		if (!mAny) return 0;
		const uint64_t from = (frame + 1 > mFirst) ? frame + 1 : mFirst;
		int missing = 0;
		for (uint64_t f = from; f <= mNewest && f < from + kSize; ++f) {
			if (!get(f, nullptr)) ++missing;
		}
		return missing;
	}

private:
	struct Slot {
		bool valid     = false;
		uint64_t frame = 0;
		PcNetplayInput in;
	};
	Slot mSlots[kSize];
	bool mAny        = false;
	uint64_t mFirst  = 0; // first noted landing frame
	uint64_t mNewest = 0; // newest noted landing frame
};

// One prediction step: the keys held and newly pressed plus the analog
// values PcamCamera::controlPad reads.
struct Step {
	uint32_t keys    = 0;
	uint32_t pressed = 0;
	uint8_t triggerL = 0;
	int8_t stickX    = 0;
	int8_t substickY = 0;
};

// Builds the prediction steps for the presentation of frame `frame`.
// Step 0 is the Kontroller's current state (the input of `frame`, which the
// sim camera reads at the next tick); steps 1.. are the pending local inputs
// frame+1, frame+2, ... while the history has them (a gap ends the window).
// A frozen controller (its captain's map menu open) feeds the sim camera
// neutral input, so every pending step is neutral too.
// Pause rule: the pause opens in the section update of the tick whose input
// carries the Start press, before that tick's camera update, so the camera
// never consumes the input just before a Start press, nor anything after it.
// The window therefore ends before input x when input x+1 presses Start.
// Returns the number of steps written (0 when even step 0 is cut).
inline int build_steps(const InputHistory& hist, uint64_t frame, const Step& current, bool frozen, Step* out,
                       int maxSteps = kMaxSteps, bool* stoppedAtStart = nullptr)
{
	if (stoppedAtStart != nullptr) *stoppedAtStart = false;
	if (maxSteps <= 0) return 0;
	Step seq[kMaxSteps];
	int n = 0;
	seq[n++] = current;
	uint32_t prevKeys = current.keys;
	while (n < maxSteps && n < kMaxSteps) {
		PcNetplayInput in;
		if (!hist.get(frame + (uint64_t)n, &in)) break;
		if (frozen) in = pc_netplay_input_neutral();
		Step s;
		s.keys      = camera_keys(in);
		s.pressed   = s.keys & ~prevKeys;
		s.triggerL  = in.triggerL;
		s.stickX    = in.stickX;
		s.substickY = in.substickY;
		prevKeys    = s.keys;
		seq[n++]    = s;
	}
	int m = n;
	for (int i = 1; i < n; ++i) {
		if (seq[i].pressed & kKeyStart) {
			m = i - 1;
			if (stoppedAtStart != nullptr) *stoppedAtStart = true;
			break;
		}
	}
	// m <= n <= kMaxSteps already; the explicit bound keeps GCC's LTO
	// -Wstringop-overflow from assuming otherwise once this is inlined.
	if (m > kMaxSteps) m = kMaxSteps;
	for (int i = 0; i < m; ++i) out[i] = seq[i];
	return m;
}

// The lead-minus-sim correction (see the header comment).
struct Correction {
	// Visual epsilon for the blend-back snap: 0.05 world units against a
	// camera distance of several hundred, and a thousandth of a degree.
	static constexpr float kPosEps = 0.05f;
	static constexpr float kFovEps = 0.001f;

	float dv[3] = { 0.0f, 0.0f, 0.0f };
	float dw[3] = { 0.0f, 0.0f, 0.0f };
	float df    = 0.0f;

	void reset()
	{
		for (int i = 0; i < 3; ++i) dv[i] = dw[i] = 0.0f;
		df = 0.0f;
	}

	bool zero() const
	{
		for (int i = 0; i < 3; ++i) {
			if (dv[i] != 0.0f || dw[i] != 0.0f) return false;
		}
		return df == 0.0f;
	}

	float magnitude() const
	{
		float m = 0.0f;
		for (int i = 0; i < 3; ++i) {
			m = std::fabs(dv[i]) > m ? std::fabs(dv[i]) : m;
			m = std::fabs(dw[i]) > m ? std::fabs(dw[i]) : m;
		}
		return m;
	}

	// One tick of the camera's homing filters on the difference. dTgt, dPv
	// and dCurFov are ideal minus sim for this tick. Returns false (and
	// resets) on a non-finite result.
	bool step(const float dTgt[3], const float dPv[3], float dCurFov, float h, float hf)
	{
		bool inputsAgree = dCurFov == 0.0f;
		for (int i = 0; i < 3; ++i) {
			dw[i] += (dTgt[i] - dw[i]) * h;
			if (dTgt[i] != 0.0f || dPv[i] != 0.0f) inputsAgree = false;
		}
		for (int i = 0; i < 3; ++i) {
			dv[i] += (dw[i] + dPv[i] - dv[i]) * h;
		}
		df += (dCurFov - df) * hf;
		for (int i = 0; i < 3; ++i) {
			if (!std::isfinite(dv[i]) || !std::isfinite(dw[i])) {
				reset();
				return false;
			}
		}
		if (!std::isfinite(df)) {
			reset();
			return false;
		}
		if (inputsAgree && magnitude() < kPosEps && std::fabs(df) < kFovEps) {
			reset();
		}
		return true;
	}
};

// PIKMIN_NETPLAY_CAMERA_LEAD: unset or anything but "0" = on; "0" = off
// (the view is the sim camera).
inline bool env_lead_enabled(const char* value)
{
	if (value == nullptr) return true;
	return !(value[0] == '0' && value[1] == '\0');
}

// PIKMIN_NETPLAY_JOINER_OWN_CAMERA: unset or anything but "0" = on (this
// peer's own pad samples, and its free-camera drags turn, this peer's own
// captain's camera); "0" = the per-slot routing (the pad samples the camera
// Navi::controlCamera() names, which for the joiner is P1's camera, and each
// drag goes to its own slot's camera).
inline bool env_joiner_own_camera(const char* value)
{
	return env_lead_enabled(value);
}

} // namespace pc_netplay_camlead
