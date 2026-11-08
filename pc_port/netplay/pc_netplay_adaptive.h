#pragma once
// Netplay session stats and adaptive input delay. Engine-free, header-only
// policy pieces that pc_netplay_session.cpp drives, so the host-run test
// (pc_netplay_adaptive_test.cpp) links nothing else.
//
// SessionStats keeps, per session: stall events (runs of session turns with
// no Advance: count, total, max and the last 10 s), presented-frame times
// (wall time between Advances, p50/p95/p99/max) and GekkoNet's RTT samples
// (p50/p95, jitter).
//
// Adaptive delay. The local input delay d is how many frames ahead of the
// current frame a local input lands. When it is too small the REMOTE peer
// waits, so each peer reports its own counted stalls to the other (a small
// unreliable "advice" datagram every 250 ms with cumulative totals) and each
// peer's controller runs on the stalls the remote reports:
//   - up quickly: at least upStallMs of reported lateness inside upWindowMs,
//     not within settleMs of the last change, raises d by 1 (by 2 when it is
//     sustained), at most once per upCooldownMs and never past
//     need(RTT p50) + maxExtraOverRtt;
//   - down slowly: downHoldMs since the last change with at most
//     downStallTolMs of reported lateness, reports arriving throughout, and
//     need(RTT p50) <= d - 1, lowers d by 1. A raise soon after a lowering
//     doubles the hold (up to maxDownHoldMs);
//   - counted stalls exclude hitches of hitchMs or longer, stalls inside a
//     load window and the settle span at the session start;
//   - this peer's own lateness (a turn that starts behind its 30 Hz
//     schedule) is subtracted from the reported lateness;
//   - need(rtt) is the handshake auto-delay formula, ceil((rtt/2)/33.3 ms -
//     0.05) + 1, clamped to [min, max].
// The session freezes the controller inside a load window, mid-transition
// and during the settle span; a pause restarts its clock
// (DelayController::note_pause).
//
// SubmitGate is the frame arithmetic of a transition. GekkoNet stores the
// input added at current frame c on c + delay and accepts only the next
// sequential frame; nextLand is that frame and a submit is due when
// nextLand == advances + delay. Growing d -> d + k adds k + 1 inputs in one
// turn (gekko_set_local_delay_nofill before each add); shrinking d -> d - k
// submits nothing for k turns. Every frame gets exactly one local input, in
// frame order, so every transition is identical on both peers, and a
// scripted input file's record for frame F is always record F - d0.

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pc_netplay_adaptive {

constexpr double kSlotMs = 1000.0 / 30.0;

// The one local input delay cap. The session's numeric and auto clamps, the
// load window, the lead camera, the adaptive range, the test schedule and the
// advice decode below all use it as their maximum.
constexpr unsigned kMaxLocalDelay = 8;

// The handshake auto-delay formula (pc_netplay_session.cpp,
// start_gekko_session): ceil((rtt/2)/slot - 0.05) + 1, clamped.
inline unsigned delay_for_rtt(double rttMs, unsigned lo, unsigned hi)
{
	if (rttMs < 0) return lo;
	double adj = (rttMs / 2.0) / kSlotMs - 0.05;
	if (adj < 0) adj = 0;
	unsigned d = (unsigned)adj + 1;
	if ((double)(unsigned)adj < adj) ++d;
	if (d < lo) d = lo;
	if (d > hi) d = hi;
	return d;
}

// p in [0,100] of a sorted copy (nearest rank). -1 when empty.
inline double percentile_of(std::vector<double> v, double p)
{
	if (v.empty()) return -1.0;
	std::sort(v.begin(), v.end());
	if (p <= 0) return v.front();
	if (p >= 100) return v.back();
	size_t rank = (size_t)(p / 100.0 * (double)v.size());
	if ((double)rank < p / 100.0 * (double)v.size()) ++rank; // ceil
	if (rank < 1) rank = 1;
	if (rank > v.size()) rank = v.size();
	return v[rank - 1];
}

// ---- presented-frame-time histogram ----
class FrameTimeHist {
public:
	static constexpr double kResMs = 0.1;
	static constexpr size_t kBuckets = 5000; // 0 .. 500 ms at 0.1 ms
	void reset()
	{
		mBins.assign(kBuckets, 0);
		mOver.clear();
		mCount = 0;
		mSum = 0;
		mMax = 0;
	}
	void add(double ms)
	{
		if (mBins.empty()) mBins.assign(kBuckets, 0);
		if (ms < 0) ms = 0;
		++mCount;
		mSum += ms;
		if (ms > mMax) mMax = ms;
		const size_t b = (size_t)(ms / kResMs);
		if (b < kBuckets) ++mBins[b];
		else mOver.push_back(ms);
	}
	uint64_t count() const { return mCount; }
	double max_ms() const { return mMax; }
	double mean_ms() const { return mCount > 0 ? mSum / (double)mCount : 0.0; }
	// Nearest-rank percentile; a bucket reports its upper edge.
	double percentile(double p) const
	{
		if (mCount == 0) return 0.0;
		uint64_t rank = (uint64_t)(p / 100.0 * (double)mCount);
		if ((double)rank < p / 100.0 * (double)mCount) ++rank;
		if (rank < 1) rank = 1;
		if (rank > mCount) rank = mCount;
		uint64_t seen = 0;
		for (size_t i = 0; i < mBins.size(); ++i) {
			seen += mBins[i];
			if (seen >= rank) return (double)(i + 1) * kResMs;
		}
		std::vector<double> over = mOver;
		std::sort(over.begin(), over.end());
		const uint64_t idx = rank - seen - 1;
		return idx < over.size() ? over[(size_t)idx] : mMax;
	}
	// Frames at least `ms` long.
	uint64_t at_least(double ms) const
	{
		uint64_t n = 0;
		const size_t from = (size_t)(ms / kResMs);
		for (size_t i = from; i < mBins.size(); ++i) n += mBins[i];
		for (double v : mOver)
			if (v >= ms) ++n;
		return n;
	}
	// Coarse buckets for the log (upper edges in ms; the last is open).
	std::string summary() const
	{
		static const double edges[] = { 20, 30, 34, 37, 45, 67, 100, 200, 500 };
		std::string s;
		double lo = 0;
		char cell[64];
		for (double e : edges) {
			const uint64_t n = at_least(lo) - at_least(e);
			snprintf(cell, sizeof(cell), "%s[%g,%g)=%llu", s.empty() ? "" : " ", lo, e, (unsigned long long)n);
			s += cell;
			lo = e;
		}
		snprintf(cell, sizeof(cell), " [%g,+)=%llu", lo, (unsigned long long)at_least(lo));
		s += cell;
		return s;
	}

private:
	std::vector<uint32_t> mBins;
	std::vector<double> mOver;
	uint64_t mCount = 0;
	double mSum = 0;
	double mMax = 0;
};

// ---- stall events ----
struct StallEvent {
	double startMs = 0; // wall time the first stall turn began
	double durMs = 0;   // wall time of the stall turns
	bool excluded = false; // inside a load window (stats only, never counted)
};

// What a peer reports: counted stalls only (not excluded, shorter than a
// hitch).
inline bool stall_counted(const StallEvent& e, double hitchMs) { return !e.excluded && e.durMs < hitchMs; }

// Handshake-channel advice datagram (see the header comment). Layout, all
// little-endian: the session writes the stable 7-byte handshake header
// (kHsMagic, type kHsAdvice, protocol); this is the payload after it.
//   0  u32 seq            increments per send
//   4  u32 lateMs         cumulative counted stall ms of the sender
//   8  u32 lateEvents     cumulative counted stall events of the sender
//   12 u8  delay          the sender's current local delay
//   13 u16 rttP50         the sender's session RTT p50 (ms, 0xFFFF: none)
//   15 u8  flags          bit 0: the sender's controller is on
constexpr size_t kAdvicePayload = 16;
struct Advice {
	uint32_t seq = 0;
	uint32_t lateMs = 0;
	uint32_t lateEvents = 0;
	uint8_t delay = 0;
	uint16_t rttP50 = 0xFFFF;
	uint8_t flags = 0;
};

inline void advice_encode(const Advice& a, uint8_t out[kAdvicePayload])
{
	const uint32_t w[3] = { a.seq, a.lateMs, a.lateEvents };
	for (int i = 0; i < 3; ++i)
		for (int b = 0; b < 4; ++b) out[i * 4 + b] = (uint8_t)((w[i] >> (8 * b)) & 0xFF);
	out[12] = a.delay;
	out[13] = (uint8_t)(a.rttP50 & 0xFF);
	out[14] = (uint8_t)((a.rttP50 >> 8) & 0xFF);
	out[15] = a.flags;
}

inline bool advice_decode(const uint8_t* p, size_t len, Advice* a)
{
	if (p == nullptr || len != kAdvicePayload) return false;
	uint32_t w[3];
	for (int i = 0; i < 3; ++i) {
		w[i] = 0;
		for (int b = 0; b < 4; ++b) w[i] |= (uint32_t)p[i * 4 + b] << (8 * b);
	}
	a->seq = w[0];
	a->lateMs = w[1];
	a->lateEvents = w[2];
	a->delay = p[12];
	a->rttP50 = (uint16_t)(p[13] | ((uint16_t)p[14] << 8));
	a->flags = p[15];
	return a->delay <= kMaxLocalDelay;
}

// Receiver side: turns the cumulative reports into lateness deltas. Returns
// true for a report newer than the last one seen (older or duplicate ones
// are dropped); *deltaMs is the reported lateness since the previous report
// (0 for the first).
class AdviceReceiver {
public:
	void reset()
	{
		mHave = false;
		mSeq = 0;
		mLateMs = 0;
		mLateEvents = 0;
		mReports = 0;
	}
	bool take(const Advice& a, double* deltaMs, uint32_t* deltaEvents = nullptr)
	{
		*deltaMs = 0;
		if (deltaEvents != nullptr) *deltaEvents = 0;
		if (mHave && a.seq <= mSeq) return false;
		if (mHave && a.lateMs >= mLateMs) *deltaMs = (double)(a.lateMs - mLateMs);
		if (mHave && deltaEvents != nullptr && a.lateEvents >= mLateEvents) *deltaEvents = a.lateEvents - mLateEvents;
		mHave = true;
		mSeq = a.seq;
		mLateMs = a.lateMs;
		mLateEvents = a.lateEvents;
		mLast = a;
		++mReports;
		return true;
	}
	bool have() const { return mHave; }
	const Advice& last() const { return mLast; }
	uint64_t reports() const { return mReports; }

private:
	bool mHave = false;
	uint32_t mSeq = 0;
	uint32_t mLateMs = 0;
	uint32_t mLateEvents = 0;
	uint64_t mReports = 0;
	Advice mLast;
};

// ---- RTT samples ----
struct RttSample {
	double atMs = 0;
	double rttMs = 0;
};

// ---- the controller ----
struct Policy {
	unsigned minDelay = 1;
	unsigned maxDelay = kMaxLocalDelay;
	double upWindowMs = 3000;
	double upStallMs = 100;
	double upCooldownMs = 1000;
	// A change reaches the peer's stalls only after the new inputs land and
	// the next report comes back (about (d + 1) frames + the advice period +
	// one way), so lateness reported this soon after a change is stale.
	double settleMs = 800;
	unsigned maxExtraOverRtt = 2;
	double downHoldMs = 15000;
	double downStallTolMs = 50;
	double backoffWindowMs = 30000;
	double maxDownHoldMs = 120000;
	double backoffResetMs = 60000;
	double hitchMs = 1000;
	double rttWindowUpMs = 5000; // RTT p50 span for the raise cap
	size_t minRttSamples = 5;
	double reportPeriodMs = 250;  // the advice period
	double reportCoverage = 0.5;  // share of the hold's reports that must arrive
	// GekkoNet answers a ping at the next session turn, and a turn is one
	// 30 Hz slot (tick + pacing sleep, no polling), so a measured RTT carries
	// on average about one slot of turn quantization (U(0, slot) on each
	// side). An input does not pay it (it is needed at a turn start and read
	// at one), so need() subtracts it. (First jitter run: 60 +/- 30 ms
	// one-way measured p50 165 ms against 120 on a bare echo.)
	double rttBiasMs = kSlotMs;
	unsigned bigStepEvents = 3;   // a +2 step needs this many late events (sustained, not one spike)
	double selfLeadMs = 1000;     // own lag this far before the window still explains lateness
};

struct Decision {
	unsigned target = 0;
	bool changed = false;
	std::string reason;
};

class DelayController {
public:
	void configure(const Policy& p) { mP = p; mHoldMs = p.downHoldMs; }
	const Policy& policy() const { return mP; }
	double down_hold_ms() const { return mHoldMs; }

	// The session starts at `delay` at wall time `nowMs`.
	void start(double nowMs)
	{
		mLastChangeMs = nowMs;
		mLastUpMs = -1e18;
		mLastDownMs = -1e18;
		mHoldMs = mP.downHoldMs;
		mCapLogged = false;
	}
	// Lateness the remote reported at atMs (the stall ms its counted stall
	// events added since its previous report).
	void add_lateness(double atMs, double ms, uint32_t events = 1)
	{
		StallEvent e;
		e.startMs = atMs;
		e.durMs = ms;
		mStalls.push_back(e);
		mEvents.push_back(events);
		trim(atMs);
	}
	// Late events reported in [fromMs, toMs].
	uint32_t late_events(double fromMs, double toMs) const
	{
		uint32_t n = 0;
		for (size_t i = 0; i < mStalls.size(); ++i) {
			const StallEvent& e = mStalls[i];
			if (e.startMs < fromMs || e.startMs > toMs || e.durMs >= mP.hitchMs) continue;
			n += mEvents[i];
		}
		return n;
	}
	// need() from a measured RTT (turn quantization removed).
	unsigned need_for_measured(double rttMs) const
	{
		double net = rttMs - mP.rttBiasMs;
		if (net < 0) net = 0;
		return delay_for_rtt(net, mP.minDelay, mP.maxDelay);
	}
	// This peer's own input lateness (ms its turn start fell further behind
	// the 30 Hz schedule) at atMs.
	void add_self_overrun(double atMs, double ms)
	{
		mSelf.push_back(RttSample{ atMs, ms });
		trim(atMs);
	}
	double self_overrun_ms(double fromMs, double toMs) const
	{
		double ms = 0;
		for (const RttSample& o : mSelf)
			if (o.atMs >= fromMs && o.atMs <= toMs) ms += o.rttMs;
		return ms;
	}
	// A report arrived (with or without lateness): the down rule needs them.
	void add_report(double atMs)
	{
		mReports.push_back(atMs);
		trim(atMs);
	}
	void add_rtt(const RttSample& s) { mRtt.push_back(s); trim(s.atMs); }
	// Reports that arrived in [fromMs, toMs].
	size_t reports_in(double fromMs, double toMs) const
	{
		size_t n = 0;
		for (double t : mReports)
			if (t >= fromMs && t <= toMs) ++n;
		return n;
	}

	// Counted stall ms of events that started in [fromMs, toMs].
	double counted_stall_ms(double fromMs, double toMs) const
	{
		double ms = 0;
		for (const StallEvent& e : mStalls) {
			if (e.startMs < fromMs || e.startMs > toMs || e.excluded || e.durMs >= mP.hitchMs) continue;
			ms += e.durMs;
		}
		return ms;
	}
	// RTT percentile over samples taken in [fromMs, toMs] (-1: too few).
	double rtt_percentile(double fromMs, double toMs, double p) const
	{
		std::vector<double> v;
		for (const RttSample& s : mRtt)
			if (s.atMs >= fromMs && s.atMs <= toMs) v.push_back(s.rttMs);
		if (v.size() < mP.minRttSamples) return -1.0;
		return percentile_of(v, p);
	}

	// One decision at wall time nowMs for current delay `cur`. `frozen`
	// suppresses any change (the caller's hold / load window / transition /
	// warm-up), without losing the evidence gathered meanwhile.
	Decision decide(double nowMs, unsigned cur, bool frozen)
	{
		Decision d;
		d.target = cur;
		if (frozen) return d;
		// Backoff reset: a lowering that stuck long enough.
		if (mLastDownMs > mLastUpMs && nowMs - mLastDownMs >= mP.backoffResetMs) mHoldMs = mP.downHoldMs;
		// Up: lateness since the last change, inside the window.
		const double from = std::max(nowMs - mP.upWindowMs, mLastChangeMs + mP.settleMs);
		const double reported = counted_stall_ms(from, nowMs);
		const double own = self_overrun_ms(from - mP.selfLeadMs, nowMs);
		const double late = reported > own ? reported - own : 0.0;
		if (late >= mP.upStallMs && nowMs - mLastUpMs >= mP.upCooldownMs) {
			const double p50 = rtt_percentile(nowMs - mP.rttWindowUpMs, nowMs, 50);
			// Without enough RTT samples yet, one step at a time.
			unsigned cap = std::min(mP.maxDelay, cur + 1);
			if (p50 >= 0) {
				const unsigned need = need_for_measured(p50);
				cap = std::min(mP.maxDelay, need + mP.maxExtraOverRtt);
			}
			if (cur < cap) {
				const bool sustained = late_events(from, nowMs) >= mP.bigStepEvents;
				const unsigned step = (late >= 3.0 * mP.upStallMs && sustained) ? 2u : 1u;
				d.target = std::min(cap, cur + step);
				d.changed = true;
				char buf[160];
				snprintf(buf, sizeof(buf), "up: peer late %.0fms (own lag %.0fms) in %.1fs, rtt p50 %.0fms, cap %u",
				         reported, own, (nowMs - from) / 1000.0, p50, cap);
				d.reason = buf;
				if (nowMs - mLastDownMs < mP.backoffWindowMs) {
					mHoldMs = std::min(mP.maxDownHoldMs, mHoldMs * 2.0);
					snprintf(buf, sizeof(buf), "; lowering failed, hold now %.0fs", mHoldMs / 1000.0);
					d.reason += buf;
				}
				mLastUpMs = nowMs;
				mLastChangeMs = nowMs;
				mCapLogged = false;
				return d;
			}
			if (!mCapLogged) {
				mCapLogged = true;
				char buf[160];
				snprintf(buf, sizeof(buf), "capped: peer late %.0fms in %.1fs, rtt p50 %.0fms, cap %u", late,
				         (nowMs - from) / 1000.0, p50, cap);
				d.reason = buf; // not a change: the caller may log it once
			}
		}
		// Down: a clean hold since the last change (reported by a peer that
		// kept reporting) and RTT headroom. The RTT only sets a floor (p50,
		// quantization removed); the peer's clean reports are the evidence
		// that the jitter tail fits too, and a lowering that makes the peer
		// late is undone within seconds (and backs the next one off).
		if (cur > mP.minDelay && nowMs - mLastChangeMs >= mHoldMs) {
			const double since = nowMs - mHoldMs;
			const double stall = counted_stall_ms(since, nowMs);
			const double rttMid = rtt_percentile(since, nowMs, 50);
			const double wantReports = mHoldMs / mP.reportPeriodMs * mP.reportCoverage;
			const bool covered = (double)reports_in(since, nowMs) >= wantReports;
			if (stall <= mP.downStallTolMs && rttMid >= 0 && covered) {
				const unsigned need = need_for_measured(rttMid);
				if (need <= cur - 1) {
					d.target = cur - 1;
					d.changed = true;
					char buf[160];
					snprintf(buf, sizeof(buf), "down: peer late %.0fms in %.0fs, rtt p50 %.0fms needs %u", stall,
					         mHoldMs / 1000.0, rttMid, need);
					d.reason = buf;
					mLastDownMs = nowMs;
					mLastChangeMs = nowMs;
					return d;
				}
			}
		}
		return d;
	}

	// An externally forced change (test schedule): restarts the hold.
	void note_forced_change(double nowMs) { mLastChangeMs = nowMs; }

	// A pause at nowMs (a load window or the settle span; the session calls
	// this on every paused turn). A paused span is neither lateness nor clean
	// evidence, so it restarts the clock like a change: once it ends, the up
	// rule reads reports from settleMs after it and the down rule needs a
	// whole hold of play after it.
	void note_pause(double nowMs) { mLastChangeMs = nowMs; }

private:
	void trim(double nowMs)
	{
		// Keep what the longest window can still read (2x the longest hold).
		const double keep = 2.0 * std::max(mP.maxDownHoldMs, mP.upWindowMs);
		const double cut = nowMs - keep;
		mSelf.erase(mSelf.begin(),
		            std::find_if(mSelf.begin(), mSelf.end(), [cut](const RttSample& r) { return r.atMs >= cut; }));
		const auto keepFrom = std::find_if(mStalls.begin(), mStalls.end(),
		                                   [cut](const StallEvent& e) { return e.startMs >= cut; });
		mEvents.erase(mEvents.begin(), mEvents.begin() + (keepFrom - mStalls.begin()));
		mStalls.erase(mStalls.begin(), keepFrom);
		mRtt.erase(mRtt.begin(),
		           std::find_if(mRtt.begin(), mRtt.end(), [cut](const RttSample& r) { return r.atMs >= cut; }));
		mReports.erase(mReports.begin(),
		               std::find_if(mReports.begin(), mReports.end(), [cut](double t) { return t >= cut; }));
	}

	Policy mP;
	std::vector<StallEvent> mStalls;
	std::vector<uint32_t> mEvents; // late events per mStalls entry
	std::vector<RttSample> mRtt;
	std::vector<RttSample> mSelf; // own tick overruns (atMs, ms)
	std::vector<double> mReports;
	double mLastChangeMs = 0;
	double mLastUpMs = -1e18;
	double mLastDownMs = -1e18;
	double mHoldMs = 15000;
	bool mCapLogged = false;
};

// ---- session totals (stats lines) ----
class SessionStats {
public:
	void reset()
	{
		mFrames.reset();
		mStallCount = 0;
		mStallTotalMs = 0;
		mStallMaxMs = 0;
		mStallExcluded = 0;
		mRecent.clear();
		mRtt.clear();
	}
	void add_stall(const StallEvent& e)
	{
		++mStallCount;
		mStallTotalMs += e.durMs;
		if (e.durMs > mStallMaxMs) mStallMaxMs = e.durMs;
		if (e.excluded) ++mStallExcluded;
		mRecent.push_back(e);
		const double cut = e.startMs - 60000.0;
		mRecent.erase(mRecent.begin(), std::find_if(mRecent.begin(), mRecent.end(),
		                                            [cut](const StallEvent& r) { return r.startMs >= cut; }));
	}
	void add_frame(double ms) { mFrames.add(ms); }
	void add_rtt(double rttMs)
	{
		if (mRtt.size() < 1000000) mRtt.push_back(rttMs);
	}
	// Events that started in the last windowMs.
	void recent(double nowMs, double windowMs, uint64_t* count, double* ms) const
	{
		uint64_t n = 0;
		double t = 0;
		for (const StallEvent& e : mRecent) {
			if (e.startMs < nowMs - windowMs) continue;
			++n;
			t += e.durMs;
		}
		*count = n;
		*ms = t;
	}
	uint64_t stall_count() const { return mStallCount; }
	double stall_total_ms() const { return mStallTotalMs; }
	double stall_max_ms() const { return mStallMaxMs; }
	uint64_t stall_excluded() const { return mStallExcluded; }
	const FrameTimeHist& frames() const { return mFrames; }
	size_t rtt_samples() const { return mRtt.size(); }
	double rtt_percentile(double p) const { return percentile_of(mRtt, p); }
	double rtt_last() const { return mRtt.empty() ? -1.0 : mRtt.back(); }
	// Mean absolute difference of successive samples (GekkoNet's jitter).
	double rtt_jitter() const
	{
		if (mRtt.size() < 2) return 0.0;
		double s = 0;
		for (size_t i = 1; i < mRtt.size(); ++i) {
			const double d = mRtt[i] - mRtt[i - 1];
			s += d < 0 ? -d : d;
		}
		return s / (double)(mRtt.size() - 1);
	}

private:
	FrameTimeHist mFrames;
	uint64_t mStallCount = 0;
	double mStallTotalMs = 0;
	double mStallMaxMs = 0;
	uint64_t mStallExcluded = 0;
	std::vector<StallEvent> mRecent;
	std::vector<double> mRtt;
};

// ---- transition arithmetic (see the header comment) ----
// A submit is due this turn: the next local input, added now (current frame
// == advances), lands on advances + delay, and GekkoNet accepts only
// nextLand. False for the turns a shrink skips. A change may start only on a
// due turn (no shrink in progress).
inline bool submit_due(uint64_t nextLand, uint64_t advances, unsigned delay)
{
	return nextLand == advances + delay;
}

// The same state as one object (the two-session GekkoNet test drives it).
struct SubmitGate {
	uint64_t nextLand = 0; // frame the next local input lands on
	unsigned delay = 0;    // the local delay GekkoNet holds
	void start(unsigned d)
	{
		delay = d;
		nextLand = d; // the first add fills frames 0..d-1 with GekkoNet's empty input
	}
	bool due(uint64_t advances) const { return submit_due(nextLand, advances, delay); }
	// Turns left without a submit after a shrink (0 when due).
	uint64_t skip_turns(uint64_t advances) const
	{
		const uint64_t due_at = advances + delay;
		return nextLand > due_at ? nextLand - due_at : 0;
	}
};

// Test schedule PIKMIN_NETPLAY_TEST_DELAY_SCHEDULE="frame:delay,...": each
// step applies at the first steady submit turn with advances >= frame.
struct ScheduleStep {
	uint64_t frame = 0;
	unsigned delay = 0;
};

inline bool parse_schedule(const char* text, unsigned lo, unsigned hi, std::vector<ScheduleStep>* out,
                           std::string* err)
{
	out->clear();
	if (text == nullptr || *text == '\0') return true;
	const char* p = text;
	uint64_t lastFrame = 0;
	while (*p != '\0') {
		char* end = nullptr;
		const unsigned long long f = strtoull(p, &end, 10);
		if (end == p || *end != ':') {
			*err = "expected <frame>:<delay>";
			return false;
		}
		p = end + 1;
		const unsigned long d = strtoul(p, &end, 10);
		if (end == p || (*end != ',' && *end != '\0')) {
			*err = "expected <frame>:<delay>";
			return false;
		}
		if (d < lo || d > hi) {
			*err = "delay out of range";
			return false;
		}
		if (!out->empty() && f < lastFrame) {
			*err = "frames must not decrease";
			return false;
		}
		lastFrame = f;
		ScheduleStep s;
		s.frame = f;
		s.delay = (unsigned)d;
		out->push_back(s);
		p = *end == ',' ? end + 1 : end;
	}
	return true;
}

} // namespace pc_netplay_adaptive
