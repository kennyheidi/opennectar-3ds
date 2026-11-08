#pragma once
// Netplay network impairment profiles for tests.
// Engine-free, header-only. Used by tools/netplay/udp_impair_proxy.cpp (the
// netplay_impair_proxy test tool) and host-tested by
// pc_netplay_adaptive_test.cpp.
//
// The in-exe lossy adapter (LossyLink, PIKMIN_NETPLAY_TEST_*) has a fixed
// latency + uniform jitter + loss model on the GekkoNet channel only. The
// proxy sits between the two peers on loopback (the joiner connects to it, it
// forwards to the host) and applies this model to every datagram in both
// directions, whatever the executable.
//
// Model, per datagram, from the proxy's start (t, seconds):
//   - dropped with probability lossPct;
//   - held base(t) + uniform [0, jitterMs] ms, base(t) the piecewise-linear
//     latency schedule (clamped at both ends) or the fixed latencyMs;
//   - link blocks ("spikes"): every everyMs * uniform [0.8, 1.2], the link
//     is blocked for uniform [minMs, maxMs]; nothing is delivered while it is
//     blocked, and every datagram that became due meanwhile is delivered when
//     the block ends (a Wi-Fi scan or a bufferbloat burst). The block
//     schedule has its own seeded generator, so it is the same in every run
//     with the same seed.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <random>
#include <string>
#include <vector>

namespace pc_netplay_impair {

struct Point {
	double sec = 0;
	double ms = 0;
};

// "<sec>:<ms>,<sec>:<ms>,..." with non-decreasing seconds.
inline bool parse_schedule(const char* text, std::vector<Point>* out, std::string* err)
{
	out->clear();
	if (text == nullptr || *text == '\0') return true;
	const char* p = text;
	while (*p != '\0') {
		char* end = nullptr;
		Point pt;
		pt.sec = strtod(p, &end);
		if (end == p || *end != ':') {
			*err = "expected <sec>:<ms>";
			return false;
		}
		p = end + 1;
		pt.ms = strtod(p, &end);
		if (end == p || (*end != ',' && *end != '\0') || pt.ms < 0 || pt.sec < 0) {
			*err = "expected <sec>:<ms>";
			return false;
		}
		if (!out->empty() && pt.sec < out->back().sec) {
			*err = "seconds must not decrease";
			return false;
		}
		out->push_back(pt);
		p = *end == ',' ? end + 1 : end;
	}
	return true;
}

// Piecewise-linear value at `sec`; `fallback` when the schedule is empty.
inline double schedule_ms(const std::vector<Point>& s, double sec, double fallback)
{
	if (s.empty()) return fallback;
	if (sec <= s.front().sec) return s.front().ms;
	for (size_t i = 1; i < s.size(); ++i) {
		if (sec <= s[i].sec) {
			const double span = s[i].sec - s[i - 1].sec;
			if (span <= 0) return s[i].ms;
			const double f = (sec - s[i - 1].sec) / span;
			return s[i - 1].ms + f * (s[i].ms - s[i - 1].ms);
		}
	}
	return s.back().ms;
}

struct SpikeSpec {
	double everyMs = 0;
	double minMs = 0;
	double maxMs = 0;
	bool on() const { return everyMs > 0 && maxMs > 0; }
};

// "<everyMs>:<minMs>:<maxMs>".
inline bool parse_spikes(const char* text, SpikeSpec* out, std::string* err)
{
	*out = SpikeSpec();
	if (text == nullptr || *text == '\0') return true;
	char* end = nullptr;
	out->everyMs = strtod(text, &end);
	if (end == text || *end != ':') {
		*err = "expected <everyMs>:<minMs>:<maxMs>";
		return false;
	}
	const char* p = end + 1;
	out->minMs = strtod(p, &end);
	if (end == p || *end != ':') {
		*err = "expected <everyMs>:<minMs>:<maxMs>";
		return false;
	}
	p = end + 1;
	out->maxMs = strtod(p, &end);
	if (end == p || *end != '\0') {
		*err = "expected <everyMs>:<minMs>:<maxMs>";
		return false;
	}
	if (out->everyMs <= 0 || out->minMs < 0 || out->maxMs < out->minMs || out->maxMs >= out->everyMs) {
		*err = "need 0 <= min <= max < every";
		return false;
	}
	return true;
}

// Deterministic block windows (times in ms from the proxy's start).
class Blocks {
public:
	void configure(const SpikeSpec& spec, uint32_t seed)
	{
		mSpec = spec;
		mRng.seed(seed ^ 0x9e3779b9u);
		mCount = 0;
		mStart = mEnd = -1;
		if (mSpec.on()) next(0.0);
	}
	// Advances to tMs (non-decreasing). True while tMs lies inside a block;
	// *endMs is its end. *entered is set when this call moved into a block
	// not reported before (for a log line).
	bool blocked(double tMs, double* endMs, bool* entered)
	{
		if (entered != nullptr) *entered = false;
		if (!mSpec.on()) return false;
		while (tMs >= mEnd) next(mStart);
		if (tMs < mStart) return false;
		if (endMs != nullptr) *endMs = mEnd;
		if (!mReported) {
			mReported = true;
			++mCount;
			if (entered != nullptr) *entered = true;
		}
		return true;
	}
	uint64_t count() const { return mCount; }
	double start_ms() const { return mStart; }
	double end_ms() const { return mEnd; }

private:
	void next(double prevStart)
	{
		std::uniform_real_distribution<double> gap(0.8, 1.2);
		std::uniform_real_distribution<double> dur(mSpec.minMs, mSpec.maxMs);
		double start = prevStart + mSpec.everyMs * gap(mRng);
		if (mEnd > 0 && start < mEnd + 500.0) start = mEnd + 500.0;
		mStart = start;
		mEnd = start + dur(mRng);
		mReported = false;
	}

	SpikeSpec mSpec;
	std::mt19937 mRng;
	double mStart = -1;
	double mEnd = -1;
	bool mReported = false;
	uint64_t mCount = 0;
};

} // namespace pc_netplay_impair
