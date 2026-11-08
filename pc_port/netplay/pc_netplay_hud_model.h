#pragma once
// Netplay session HUD's pure pieces. Engine-free
// and header-only (no SDL, no game), so pc_netplay_hud_model_test links
// nothing else. pc_netplay_hud.cpp draws; pc_netplay_session.cpp feeds the
// numbers. Nothing here is ever read by the simulation.
//
//   StallWindow   the local stall counter behind "stalls (10 s)": a stall is
//                 one run of consecutive loop turns without an Advance (the
//                 game waiting for the other game's input) that lasted at
//                 least kStallMinMs, i.e. at least one 30 Hz frame was shown
//                 late. Stage-load windows are not stalls (the session
//                 filters them before add()).
//   classify      the connection-quality colour from ping, jitter and recent
//                 stalls.
//   format_*      the HUD text.

#include <cstdint>
#include <cstdio>

namespace pc_netplay_hud {

constexpr double kStallMinMs  = 34.0;    // one 30 Hz frame (33.3 ms) shown late
constexpr double kWindowMs    = 10000.0; // "stalls in the last 10 s"
// Every episode that can end inside the window: episodes are at least
// kStallMinMs long and do not overlap, so at most kWindowMs / kStallMinMs
// (294) end in it. Fix round 1: the ring was 64, which saturated the count
// (and the stalled time) on a bad link (review: a joiner read 64 while about
// 90 stalls per 10 s happened).
constexpr int kStallRing = (int)(kWindowMs / kStallMinMs) + 2;

// Ring of recent stall episodes (end time, duration), wall-clock ms.
class StallWindow {
public:
	void add(double endMs, double durMs)
	{
		if (durMs < kStallMinMs) return;
		mEnd[mHead] = endMs;
		mDur[mHead] = durMs;
		mHead       = (mHead + 1) % kStallRing;
		if (mCount < kStallRing) ++mCount;
		++mTotal;
		mTotalMs += durMs;
		if (durMs > mMaxMs) mMaxMs = durMs;
	}
	// Episodes that ended inside (now - window, now].
	unsigned count(double nowMs, double windowMs = kWindowMs) const
	{
		unsigned n = 0;
		for (int i = 0; i < mCount; ++i) {
			if (nowMs - mEnd[i] < windowMs) ++n;
		}
		return n;
	}
	double stalled_ms(double nowMs, double windowMs = kWindowMs) const
	{
		double ms = 0;
		for (int i = 0; i < mCount; ++i) {
			if (nowMs - mEnd[i] < windowMs) ms += mDur[i];
		}
		return ms;
	}
	uint64_t total() const { return mTotal; }
	double total_ms() const { return mTotalMs; }
	double max_ms() const { return mMaxMs; }

private:
	double mEnd[kStallRing] = {};
	double mDur[kStallRing] = {};
	int mHead               = 0;
	int mCount              = 0;
	uint64_t mTotal         = 0;
	double mTotalMs         = 0;
	double mMaxMs           = 0;
};

enum Quality { kQualityUnknown, kQualityGood, kQualityFair, kQualityPoor };

// Thresholds (one-way delay budget at 30 Hz: 33 ms per input-delay frame):
//   good  ping <= 100 ms, jitter <= 15 ms, no stall in the last 10 s
//   fair  ping <= 200 ms, jitter <= 40 ms, at most 3 stalls in the last 10 s
//   poor  anything worse
// Unknown until the first ping sample arrives.
constexpr float kGoodPingMs = 100.0f, kFairPingMs = 200.0f;
constexpr float kGoodJitterMs = 15.0f, kFairJitterMs = 40.0f;
constexpr unsigned kFairStalls = 3;

inline Quality classify(bool havePing, float pingMs, float jitterMs, unsigned stalls10s)
{
	if (!havePing) return stalls10s > kFairStalls ? kQualityPoor : kQualityUnknown;
	if (pingMs > kFairPingMs || jitterMs > kFairJitterMs || stalls10s > kFairStalls) return kQualityPoor;
	if (pingMs > kGoodPingMs || jitterMs > kGoodJitterMs || stalls10s > 0) return kQualityFair;
	return kQualityGood;
}

inline const char* quality_name(Quality q)
{
	switch (q) {
	case kQualityGood: return "good";
	case kQualityFair: return "fair";
	case kQualityPoor: return "poor";
	default: return "measuring";
	}
}

// RGBA of the quality colour (the dot and the label).
inline void quality_rgba(Quality q, uint8_t out[4])
{
	uint8_t c[4] = { 200, 200, 200, 255 };
	if (q == kQualityGood) { c[0] = 90;  c[1] = 230; c[2] = 90; }
	if (q == kQualityFair) { c[0] = 250; c[1] = 205; c[2] = 60; }
	if (q == kQualityPoor) { c[0] = 250; c[1] = 80;  c[2] = 70; }
	for (int i = 0; i < 4; ++i) out[i] = c[i];
}

// The numbers the HUD shows (filled by the session; see pc_netplay_hud.h).
struct Numbers {
	bool havePing = false;
	float pingMs = 0;   // GekkoNet's average of its last 10 round trips
	float jitterMs = 0; // mean change between consecutive round trips
	unsigned delay = 0; // current local input delay, frames
	unsigned stalls10s = 0;
	double stallMs10s = 0;
};

// Line 1: "NETPLAY  good" (the label carries the colour).
// Line 2: "ping 42 ms  jitter 3 ms"
// Line 3: "delay 2 (67 ms)  stalls 0 / 10 s"
inline void format_lines(const Numbers& n, char* l1, size_t n1, char* l2, size_t n2, char* l3, size_t n3)
{
	const Quality q = classify(n.havePing, n.pingMs, n.jitterMs, n.stalls10s);
	snprintf(l1, n1, "NETPLAY  %s", quality_name(q));
	if (n.havePing) snprintf(l2, n2, "ping %.0f ms  jitter %.0f ms", n.pingMs, n.jitterMs);
	else snprintf(l2, n2, "ping --  jitter --");
	if (n.stallMs10s >= 1000.0)
		snprintf(l3, n3, "delay %u (%u ms)  stalls %u / 10 s (%.1f s)", n.delay, (unsigned)((n.delay * 1000u + 15u) / 30u),
		         n.stalls10s, n.stallMs10s / 1000.0);
	else
		snprintf(l3, n3, "delay %u (%u ms)  stalls %u / 10 s", n.delay, (unsigned)((n.delay * 1000u + 15u) / 30u),
		         n.stalls10s);
}

// Edge detector for the toggle key / chord (true once per press).
class Toggle {
public:
	bool update(bool down)
	{
		const bool edge = down && !mWas;
		mWas            = down;
		return edge;
	}

private:
	bool mWas = false;
};

} // namespace pc_netplay_hud
