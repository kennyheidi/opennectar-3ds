#pragma once
// Netplay load guard: long blocking loads must not drop the session.
// Engine-free, header-only policy pieces that pc_netplay_session.cpp drives,
// so the host-run test links nothing else.
//
// GekkoNet's disconnect check is a wall-clock idle timer on the receiving
// side: now minus the last datagram received from that peer, against the
// timeout. A peer blocked inside one Advance tick (a synchronous stage load,
// or the presentation pass creating many TEV programs) sends nothing, so its
// peer drops it once the block outlasts the timeout. The guard has two parts;
// each can be switched off on its own.
//
// (a) Main-thread keep-alive network poll. The long loops call
//     pc_netplay_load_keepalive(site) between their steps: after each TEV
//     program is created (pc_gfx.cpp), on every DVDOpen/DVDRead
//     (dvd_stubs.cpp), at the stage-load hook, and while a save waits for its
//     card I/O. Inside an Advance tick, at most once per
//     kKeepAliveIntervalMs, it calls gekko_network_poll only: receive, ack,
//     resend unacked inputs and NetworkHealth. It never produces or consumes
//     an Advance, submits no input and touches no sim state. It cannot split
//     one single long step.
//
// (b) Load-window timeout extension. Every stage load runs inside the same
//     Advance frame F on both peers, so each peer raises its own disconnect
//     timeout to the load timeout (default 60 s) when the load starts,
//     without any message, and restores the normal timeout once its own
//     Advance of frame F + kLoadWindowFrames has run. That Advance needs the
//     peer's input for that frame, which the peer made only after its Advance
//     of frame F + kLoadWindowFrames - delay - 1 > F, so the peer is out of
//     its load when the timeout drops back. The cost: a peer that dies inside
//     a load window is detected after the load timeout.
//
// The window, the stage load count and the stall targeting are keyed to
// frames that run once on both peers, so the session skips them for
// rolling-back or running-ahead Advances (not used today: lockstep); the
// keep-alive still polls in those ticks.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

namespace pc_netplay_loadguard {

// Keep-alive call sites (pc_netplay_load_keepalive's argument).
enum Site {
	kSiteShader    = 1, // a specialised TEV program was created (pc_gfx.cpp)
	kSiteDvd       = 2, // DVDOpen / DVDRead (dvd_stubs.cpp)
	kSiteStageLoad = 3, // GameFlow::softReset (the N3 stage-load hook)
	kSiteStall     = 4, // between two slices of the test stall injector
	kSiteSave      = 5, // a save waiting for its card I/O
	kSiteCount     = 6, // per-site counters are indexed 0..kSiteCount-1
};

inline const char* site_name(int site)
{
	switch (site) {
	case kSiteShader: return "tev";
	case kSiteDvd: return "dvd";
	case kSiteStageLoad: return "load";
	case kSiteStall: return "stall";
	case kSiteSave: return "save";
	default: return "other";
	}
}

// Minimum wall time between two keep-alive pumps inside one tick. GekkoNet
// resends unacked inputs every 200 ms and sends NetworkHealth every 500 ms,
// so 50 ms keeps every one of its timers on schedule while a load that opens
// hundreds of files costs only a clock read per call.
constexpr double kKeepAliveIntervalMs = 50.0;
// Default GekkoNet disconnect timeout inside a load window.
constexpr unsigned kDefaultLoadDisconnectMs = 60000;
// Frames after the stage-load frame at which the window closes (see (b)).
constexpr uint32_t kLoadWindowFrames = 30;
// A tick that takes longer than this is logged with its keep-alive figures.
constexpr double kLongTickMs = 2000.0;
// Rate limit for the keep-alive pump.
class KeepAliveGate {
public:
	explicit KeepAliveGate(double intervalMs = kKeepAliveIntervalMs) : mIntervalMs(intervalMs) {}
	void reset() { mHave = false; mLastMs = 0; }
	// A pump is due when none ran yet or the interval has elapsed.
	bool due(double nowMs) const { return !mHave || nowMs - mLastMs >= mIntervalMs; }
	void pumped(double nowMs)
	{
		mHave   = true;
		mLastMs = nowMs;
	}
	double interval_ms() const { return mIntervalMs; }

private:
	double mIntervalMs;
	bool mHave    = false;
	double mLastMs = 0;
};

// The deterministic, frame-keyed disconnect-timeout extension of (b).
class LoadWindow {
public:
	void configure(bool enabled, unsigned normalMs, unsigned loadMs, uint32_t frames = kLoadWindowFrames)
	{
		mEnabled  = enabled;
		mNormalMs = normalMs;
		// Never shorter than the normal timeout: the window only ever widens.
		mLoadMs = loadMs < normalMs ? normalMs : loadMs;
		mFrames = frames;
		mOpen   = false;
		mOpenFrame = mCloseFrame = 0;
	}
	bool enabled() const { return mEnabled; }
	unsigned normal_ms() const { return mNormalMs; }
	unsigned load_ms() const { return mLoadMs; }
	bool is_open() const { return mOpen; }
	uint32_t open_frame() const { return mOpenFrame; }
	uint32_t close_frame() const { return mCloseFrame; }

	// A stage load started inside the Advance of `frame`. Returns true when
	// the caller must raise the GekkoNet timeout to load_ms() now (false when
	// disabled or already raised; a load inside an open window only moves
	// its close frame later).
	bool open(uint32_t frame)
	{
		if (!mEnabled) return false;
		mOpenFrame  = frame;
		mCloseFrame = frame + mFrames;
		if (mOpen) return false;
		mOpen = true;
		return true;
	}

	// The Advance of `frame` completed. Returns true when the window closes
	// now: the caller restores normal_ms().
	bool after_advance(uint32_t frame)
	{
		if (!mOpen || frame < mCloseFrame) return false;
		mOpen = false;
		return true;
	}

private:
	bool mEnabled      = false;
	unsigned mNormalMs = 15000;
	unsigned mLoadMs   = kDefaultLoadDisconnectMs;
	uint32_t mFrames   = kLoadWindowFrames;
	bool mOpen         = false;
	uint32_t mOpenFrame  = 0;
	uint32_t mCloseFrame = 0;
};

// ---- Opt-in test stall injector (netplay builds, test only) ----
//
//   PIKMIN_NETPLAY_TEST_STALL_MS=<ms>        total stall (1..600000); unset = off
//   PIKMIN_NETPLAY_TEST_STALL_AT=<where>     load (default; the first stage load
//                                            in the session) | load:<n> (the nth)
//                                            | shader (right after the first TEV
//                                            program created in the session)
//                                            | tick:<n> (inside the Advance of
//                                            hash tick n, before the sim runs)
//   PIKMIN_NETPLAY_TEST_STALL_ROLE=<who>     host | join | both (default), so the
//                                            same env can be given to both peers
//   PIKMIN_NETPLAY_TEST_STALL_SLICE_MS=<ms>  slice length (default 250);
//                                            0 = one uninterrupted block
//
// The stall is wall-clock only (it touches no sim state, RNG or hash) and
// runs inside an Advance tick, exactly where the real stalls block the main
// loop. Between two slices it calls the same keep-alive entry the real long
// loops call, so a sliced stall is a long loop of slow steps (a TEV loop, or
// a slow stage load's reads) and slice 0 is a single un-pumpable block.
enum class StallAt { Load, Shader, Tick };

enum StallRole {
	kStallHost = 1,
	kStallJoin = 2,
	kStallBoth = 3,
};

struct StallPlan {
	bool enabled     = false;
	unsigned totalMs = 0;
	StallAt at       = StallAt::Load;
	uint64_t index   = 1; // load:<n> (1-based) or tick:<n>
	int roles        = kStallBoth;
	unsigned sliceMs = 250;
};

// Strict decimal parse (no sign, no spaces, no trailing text).
inline bool parse_decimal(const char* s, uint64_t maxValue, uint64_t* out)
{
	if (s == nullptr || *s == '\0') return false;
	uint64_t v = 0;
	for (const char* p = s; *p != '\0'; ++p) {
		if (*p < '0' || *p > '9') return false;
		v = v * 10 + (uint64_t)(*p - '0');
		if (v > maxValue) return false;
	}
	*out = v;
	return true;
}

// Parses the four variables (nullptr = unset). ms == nullptr leaves the
// injector off and returns true. Returns false with *err set on any bad
// value, in which case the injector stays off.
inline bool parse_stall_plan(const char* ms, const char* at, const char* role, const char* slice, StallPlan* out,
                             std::string* err)
{
	StallPlan p;
	*out = p;
	if (ms == nullptr) return true;
	uint64_t v = 0;
	if (!parse_decimal(ms, 600000, &v) || v == 0) {
		if (err) *err = "PIKMIN_NETPLAY_TEST_STALL_MS wants 1..600000";
		return false;
	}
	p.totalMs = (unsigned)v;
	if (at != nullptr) {
		if (strcmp(at, "load") == 0) {
			p.at    = StallAt::Load;
			p.index = 1;
		} else if (strcmp(at, "shader") == 0) {
			p.at    = StallAt::Shader;
			p.index = 1;
		} else if (strncmp(at, "load:", 5) == 0) {
			if (!parse_decimal(at + 5, 1000000, &v) || v == 0) {
				if (err) *err = "PIKMIN_NETPLAY_TEST_STALL_AT load:<n> wants n >= 1";
				return false;
			}
			p.at    = StallAt::Load;
			p.index = v;
		} else if (strncmp(at, "tick:", 5) == 0) {
			if (!parse_decimal(at + 5, 100000000, &v) || v == 0) {
				if (err) *err = "PIKMIN_NETPLAY_TEST_STALL_AT tick:<n> wants n >= 1";
				return false;
			}
			p.at    = StallAt::Tick;
			p.index = v;
		} else {
			if (err) *err = "PIKMIN_NETPLAY_TEST_STALL_AT wants load | load:<n> | shader | tick:<n>";
			return false;
		}
	}
	if (role != nullptr) {
		if (strcmp(role, "host") == 0) p.roles = kStallHost;
		else if (strcmp(role, "join") == 0) p.roles = kStallJoin;
		else if (strcmp(role, "both") == 0) p.roles = kStallBoth;
		else {
			if (err) *err = "PIKMIN_NETPLAY_TEST_STALL_ROLE wants host | join | both";
			return false;
		}
	}
	if (slice != nullptr) {
		if (!parse_decimal(slice, 60000, &v)) {
			if (err) *err = "PIKMIN_NETPLAY_TEST_STALL_SLICE_MS wants 0..60000";
			return false;
		}
		p.sliceMs = (unsigned)v;
	}
	p.enabled = true;
	*out      = p;
	return true;
}

// True when the plan stalls this peer.
inline bool stall_applies_to(const StallPlan& p, bool isHost)
{
	if (!p.enabled) return false;
	return (p.roles & (isHost ? kStallHost : kStallJoin)) != 0;
}

// Length of slice `i` (0-based) of the plan; 0 once the total is spent. A
// slice length of 0 (or one not shorter than the total) is a single block.
inline unsigned stall_slice_ms(const StallPlan& p, unsigned i)
{
	if (!p.enabled || p.totalMs == 0) return 0;
	if (p.sliceMs == 0 || p.sliceMs >= p.totalMs) return i == 0 ? p.totalMs : 0;
	const uint64_t start = (uint64_t)i * p.sliceMs;
	if (start >= p.totalMs) return 0;
	const uint64_t left = p.totalMs - start;
	return left < p.sliceMs ? (unsigned)left : p.sliceMs;
}

inline const char* stall_role_name(int roles)
{
	return roles == kStallHost ? "host" : roles == kStallJoin ? "join" : "both";
}

} // namespace pc_netplay_loadguard
