// Host-run test for the netplay adaptive input delay.
//
//  1. pc_netplay_adaptive.h: delay_for_rtt, percentiles, the frame-time
//     histogram, the test-schedule parser and the DelayController on
//     synthetic clocks (quick raise on sustained reported lateness up to the
//     RTT cap; hitches ignored; slow lowering on a clean, reported hold with
//     RTT headroom, never on silence; backoff after a failed lowering;
//     frozen = no change), the counted-stall rule and the advice datagram
//     (round trip, stale / duplicate reports dropped, cumulative deltas).
//  2. pc_netplay_impair.h: schedule / spike parsing, the piecewise-linear
//     latency and the deterministic link-block schedule.
//  3. Delay transitions against the vendored GekkoNet: two in-memory
//     sessions, window 0, a few turns of latency, scripted per-frame inputs,
//     each peer growing and shrinking its delay mid-run with the session's
//     arithmetic (submit_due + gekko_set_local_delay_nofill). Both peers must
//     advance the same frames with byte-identical inputs, and frame F must
//     carry peer p's record F - d0(p) (the input a fixed-delay run puts
//     there), so scripted pairs stay byte-identical to a fixed-delay run.
//     A control run grows with gekko_set_local_delay instead and shows why
//     the no-fill setter is needed (the filled frames repeat an input).
// Engine-free: links gekkonet only.

#include "netplay/pc_netplay_adaptive.h"
#include "netplay/pc_netplay_impair.h"

#include "gekkonet.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int sFailures = 0;

void check(bool ok, const char* what, int line)
{
	if (!ok) {
		++sFailures;
		std::printf("FAIL line %d: %s\n", line, what);
	}
}
#define CHECK(ok, what) check((ok), (what), __LINE__)

using namespace pc_netplay_adaptive;

// ---- 1. policy pieces ----

void test_delay_for_rtt()
{
	CHECK(delay_for_rtt(-1, 1, 8) == 1, "no rtt -> min");
	CHECK(delay_for_rtt(0, 1, 8) == 1, "rtt 0 -> 1");
	CHECK(delay_for_rtt(40, 1, 8) == 2, "rtt 40 -> 2");
	CHECK(delay_for_rtt(60, 1, 8) == 2, "rtt 60 -> 2");
	CHECK(delay_for_rtt(120, 1, 8) == 3, "rtt 120 -> 3");
	CHECK(delay_for_rtt(160, 1, 8) == 4, "rtt 160 -> 4");
	CHECK(delay_for_rtt(200, 1, 8) == 4, "rtt 200 -> 4 (boundary tolerance)");
	CHECK(delay_for_rtt(1000, 1, 8) == 8, "clamped to max");
	CHECK(delay_for_rtt(10, 3, 8) == 3, "clamped to min");
}

void test_hist()
{
	FrameTimeHist h;
	h.reset();
	for (int i = 0; i < 98; ++i) h.add(33.3);
	h.add(120.0);
	h.add(900.0);
	CHECK(h.count() == 100, "hist count");
	CHECK(h.percentile(50) > 33.2 && h.percentile(50) < 33.5, "hist p50 ~33.3");
	CHECK(h.percentile(98) < 33.5, "hist p98 still 33.3");
	CHECK(h.percentile(99) > 119.9 && h.percentile(99) < 120.2, "hist p99 = 120");
	CHECK(h.percentile(100) == 900.0, "hist p100 = the exact overflow value");
	CHECK(h.max_ms() == 900.0, "hist max");
	CHECK(h.at_least(100) == 2, "hist >=100");
	CHECK(h.at_least(50) == 2, "hist >=50");
	CHECK(!h.summary().empty(), "hist summary");
	std::vector<double> v = { 5, 1, 3, 2, 4 };
	CHECK(percentile_of(v, 50) == 3, "percentile_of p50");
	CHECK(percentile_of(v, 100) == 5, "percentile_of p100");
	CHECK(percentile_of({}, 50) < 0, "percentile_of empty");
}

void test_schedule_parse()
{
	std::vector<ScheduleStep> s;
	std::string err;
	CHECK(parse_schedule("300:4,600:2,900:8", 1, 8, &s, &err) && s.size() == 3, "schedule parses");
	CHECK(s[0].frame == 300 && s[0].delay == 4 && s[2].delay == 8, "schedule values");
	CHECK(parse_schedule("", 1, 8, &s, &err) && s.empty(), "empty schedule");
	CHECK(!parse_schedule("300:9", 1, 8, &s, &err), "delay above max refused");
	CHECK(!parse_schedule("300:0", 1, 8, &s, &err), "delay 0 refused");
	CHECK(!parse_schedule("600:2,300:4", 1, 8, &s, &err), "decreasing frames refused");
	CHECK(!parse_schedule("300-4", 1, 8, &s, &err), "garbage refused");
	// The session parses the schedule with the Policy's
	// range, which ends at the one cap (kMaxLocalDelay == 8), so the forced
	// schedules (1..8) still fit and 9 is still refused.
	Policy pol;
	CHECK(pol.minDelay == 1 && pol.maxDelay == kMaxLocalDelay && kMaxLocalDelay == 8, "policy range is 1..cap");
	CHECK(parse_schedule("300:1,600:8", pol.minDelay, pol.maxDelay, &s, &err), "schedule 1..8 fits the policy");
	CHECK(!parse_schedule("300:9", pol.minDelay, pol.maxDelay, &s, &err), "above the cap refused");
	Advice big;
	big.delay = (uint8_t)(kMaxLocalDelay + 1);
	uint8_t wire[kAdvicePayload];
	advice_encode(big, wire);
	Advice back;
	CHECK(!advice_decode(wire, sizeof(wire), &back), "advice past the cap refused");
}

// Synthetic clock helpers: RTT samples every 500 ms.
void feed_rtt(DelayController& c, double fromMs, double toMs, double rtt)
{
	for (double t = fromMs; t < toMs; t += 500.0) {
		RttSample s;
		s.atMs = t;
		s.rttMs = rtt;
		c.add_rtt(s);
	}
}

// Lateness the peer reported (the session feeds report deltas like this).
void add_stall(DelayController& c, double at, double dur, uint32_t events = 1)
{
	c.add_lateness(at, dur, events);
}

// Reports arriving every 250 ms (the advice period).
void feed_reports(DelayController& c, double fromMs, double toMs)
{
	for (double t = fromMs; t < toMs; t += 250.0) c.add_report(t);
}

// Runs decide() every frame over [fromMs, toMs); returns the final delay and
// counts the changes.
unsigned run_frames(DelayController& c, double fromMs, double toMs, unsigned cur, int* ups, int* downs,
                    bool frozen = false)
{
	for (double t = fromMs; t < toMs; t += kSlotMs) {
		Decision d = c.decide(t, cur, frozen);
		if (d.changed) {
			if (d.target > cur && ups != nullptr) ++*ups;
			if (d.target < cur && downs != nullptr) ++*downs;
			cur = d.target;
		}
	}
	return cur;
}

void test_controller()
{
	Policy pol;
	// Quick raise on sustained lateness, stopping at need(rtt p50) + 2.
	{
		DelayController c;
		c.configure(pol);
		c.start(0);
		feed_rtt(c, 0, 60000, 93); // measured 93 = net 60 + one slot: need 2 -> cap 4
		unsigned cur = run_frames(c, 0, 10000, 2, nullptr, nullptr);
		CHECK(cur == 2, "no stalls: no change");
		for (double t = 10000; t < 20000; t += 300) add_stall(c, t, 40);
		int ups = 0;
		cur = run_frames(c, 10000, 11000, cur, &ups, nullptr);
		CHECK(cur == 3 && ups == 1, "first raise within 1 s of sustained lateness");
		cur = run_frames(c, 11000, 20000, cur, &ups, nullptr);
		CHECK(cur == 4, "raises stop at the rtt cap (need 2 + 2)");
		CHECK(ups == 2, "one step per raise at this lateness");
	}
	// Heavy lateness raises by two.
	{
		DelayController c;
		c.configure(pol);
		c.start(0);
		feed_rtt(c, 0, 30000, 183); // net 150: need 4 -> cap 7
		add_stall(c, 5000, 350, 4);
		int ups = 0;
		unsigned cur = run_frames(c, 5000, 5400, 2, &ups, nullptr);
		CHECK(cur == 4 && ups == 1, "350 ms of sustained lateness raises by 2");
		DelayController c1;
		c1.configure(pol);
		c1.start(0);
		feed_rtt(c1, 0, 30000, 183);
		add_stall(c1, 5000, 350, 1);
		ups = 0;
		cur = run_frames(c1, 5000, 5400, 2, &ups, nullptr);
		CHECK(cur == 3 && ups == 1, "one 350 ms spike raises by 1");
	}
	// No RTT samples yet (the first seconds): one step at a time, even on
	// heavy sustained lateness.
	{
		DelayController c;
		c.configure(pol);
		c.start(0);
		add_stall(c, 5000, 350, 4);
		int ups = 0;
		unsigned cur = run_frames(c, 5000, 5400, 2, &ups, nullptr);
		CHECK(cur == 3 && ups == 1, "no rtt samples: heavy lateness raises by 1 only");
	}
	// Lateness explained by this peer's own slow ticks never raises.
	{
		DelayController c;
		c.configure(pol);
		c.start(0);
		feed_rtt(c, 0, 30000, 93);
		c.add_self_overrun(4900, 250); // a 283 ms tick: our next inputs were 250 ms late
		add_stall(c, 5100, 200, 1);    // ... and the peer waited for them
		unsigned cur = run_frames(c, 5000, 8000, 2, nullptr, nullptr);
		CHECK(cur == 2, "own slow tick explains the reported lateness");
		c.add_self_overrun(9000, 40);
		add_stall(c, 9100, 200, 4); // more than the overrun: genuine lateness
		cur = run_frames(c, 9000, 9500, cur, nullptr, nullptr);
		CHECK(cur == 3, "lateness beyond own slow ticks raises");
	}
	// Hitches (>= 1 s) never count; the reporter drops load-window stalls.
	{
		DelayController c;
		c.configure(pol);
		c.start(0);
		feed_rtt(c, 0, 30000, 60);
		add_stall(c, 5000, 2500);
		unsigned cur = run_frames(c, 5000, 12000, 2, nullptr, nullptr);
		CHECK(cur == 2, "hitch ignored");
		CHECK(c.counted_stall_ms(0, 1e18) == 0, "counted stall 0");
		StallEvent e;
		e.durMs = 400;
		CHECK(stall_counted(e, pol.hitchMs), "a 400 ms stall is reported");
		e.excluded = true;
		CHECK(!stall_counted(e, pol.hitchMs), "a load-window stall is not reported");
		e.excluded = false;
		e.durMs = 1000;
		CHECK(!stall_counted(e, pol.hitchMs), "a 1 s hitch is not reported");
	}
	// Frozen: evidence gathered, no change.
	{
		DelayController c;
		c.configure(pol);
		c.start(0);
		feed_rtt(c, 0, 30000, 93);
		for (double t = 1000; t < 4000; t += 200) add_stall(c, t, 50, 1);
		unsigned cur = run_frames(c, 1000, 4000, 2, nullptr, nullptr, true);
		CHECK(cur == 2, "frozen: no change");
		cur = run_frames(c, 4000, 4100, cur, nullptr, nullptr, false);
		CHECK(cur == 4, "unfrozen: the gathered lateness raises at once");
	}
	// A pause (a load window) is no evidence: the clean reports that
	// keep arriving while paused never lower the delay on the first frame
	// after it, and the resume's own catch-up waits (inside settleMs) never
	// raise it.
	{
		DelayController c;
		c.configure(pol);
		c.start(0);
		feed_rtt(c, 0, 80000, 93); // need 2
		feed_reports(c, 0, 80000);
		unsigned cur = 3;
		for (double t = 5000; t < 27000; t += kSlotMs) {
			c.note_pause(t);
			Decision d = c.decide(t, cur, true); // the session's freeze
			if (d.changed) cur = d.target;
		}
		add_stall(c, 27300, 300, 4); // resume catch-up, 0.3 s after the pause
		int ups = 0, downs = 0;
		cur = run_frames(c, 27000, 41900, cur, &ups, &downs);
		CHECK(cur == 3 && ups == 0 && downs == 0, "pause: no change for a hold after it ends");
		// The catch-up wait (300 ms at 27.3 s) is inside the first clean
		// window, so the lowering waits until the window has moved past it.
		cur = run_frames(c, 41900, 42700, cur, &ups, &downs);
		CHECK(cur == 2 && downs == 1 && ups == 0, "pause: lowered a whole clean hold after it");
		// Without note_pause the same span would have lowered at once.
		DelayController c2;
		c2.configure(pol);
		c2.start(0);
		feed_rtt(c2, 0, 80000, 93);
		feed_reports(c2, 0, 80000);
		cur = run_frames(c2, 5000, 27000, 3, nullptr, nullptr, true);
		downs = 0;
		cur = run_frames(c2, 27000, 27100, cur, nullptr, &downs);
		CHECK(cur == 2 && downs == 1, "pause control: a frozen-only span lowers on the first frame");
		// Genuine lateness after the settle span still raises.
		DelayController c3;
		c3.configure(pol);
		c3.start(0);
		feed_rtt(c3, 0, 80000, 93);
		for (double t = 5000; t < 27000; t += kSlotMs) c3.note_pause(t);
		add_stall(c3, 28500, 150, 2);
		ups = 0;
		cur = run_frames(c3, 27000, 29000, 2, &ups, nullptr);
		CHECK(cur == 3 && ups == 1, "pause: lateness after the settle span raises");
	}
	// Slow lowering: a clean hold with RTT headroom, one step per hold.
	{
		DelayController c;
		c.configure(pol);
		c.start(0);
		feed_rtt(c, 0, 200000, 93); // net 60: need 2
		feed_reports(c, 0, 200000);
		int downs = 0;
		unsigned cur = run_frames(c, 0, 14900, 5, nullptr, &downs);
		CHECK(cur == 5 && downs == 0, "no lowering before the 15 s hold");
		cur = run_frames(c, 14900, 15200, cur, nullptr, &downs);
		CHECK(cur == 4 && downs == 1, "lowered by one after 15 s clean");
		cur = run_frames(c, 15200, 200000, cur, nullptr, &downs);
		CHECK(cur == 2 && downs == 3, "stops at need(rtt p50 - one slot) = 2");
	}
	// No lowering when the RTT needs the delay, or stalls exceed the tolerance.
	{
		DelayController c;
		c.configure(pol);
		c.start(0);
		feed_rtt(c, 0, 60000, 193); // net 160: need 4
		feed_reports(c, 0, 60000);
		unsigned cur = run_frames(c, 0, 60000, 4, nullptr, nullptr);
		CHECK(cur == 4, "rtt needs 4: no lowering");
		DelayController c2;
		c2.configure(pol);
		c2.start(0);
		feed_rtt(c2, 0, 60000, 60);
		feed_reports(c2, 0, 60000);
		for (double t = 0; t < 60000; t += 5000) add_stall(c2, t, 30); // 90 ms per 15 s, under the raise bar
		cur = run_frames(c2, 0, 60000, 4, nullptr, nullptr);
		CHECK(cur == 4, "stall above the down tolerance blocks lowering");
		// Silence is not a clean hold: no reports, no lowering.
		DelayController c3;
		c3.configure(pol);
		c3.start(0);
		feed_rtt(c3, 0, 60000, 60);
		feed_reports(c3, 0, 5000); // then the peer's reports stop
		cur = run_frames(c3, 0, 60000, 4, nullptr, nullptr);
		CHECK(cur == 4, "no reports: no lowering");
	}
	// Backoff: a raise soon after a lowering doubles the hold.
	{
		DelayController c;
		c.configure(pol);
		c.start(0);
		feed_rtt(c, 0, 200000, 93); // need 2: the floor
		feed_reports(c, 0, 200000);
		int downs = 0, ups = 0;
		unsigned cur = run_frames(c, 0, 15100, 3, &ups, &downs);
		CHECK(cur == 2 && downs == 1, "backoff: lowered");
		for (double t = 16000; t < 16600; t += 100) add_stall(c, t, 20); // 120 ms: one raise
		cur = run_frames(c, 15100, 17100, cur, &ups, &downs);
		CHECK(cur == 3 && ups == 1, "backoff: raised again");
		CHECK(c.down_hold_ms() == 30000, "backoff: hold doubled to 30 s");
		cur = run_frames(c, 17100, 17100 + 29000, cur, &ups, &downs);
		CHECK(cur == 3 && downs == 1, "backoff: no lowering inside the doubled hold");
		cur = run_frames(c, 46100, 48000, cur, &ups, &downs);
		CHECK(cur == 2 && downs == 2, "backoff: lowered after the doubled hold");
		cur = run_frames(c, 48000, 48000 + 61000, cur, &ups, &downs);
		CHECK(c.down_hold_ms() == 15000, "backoff: reset after a lowering sticks for 60 s");
	}
}

void test_advice()
{
	Advice a;
	a.seq = 7;
	a.lateMs = 123456;
	a.lateEvents = 42;
	a.delay = 5;
	a.rttP50 = 187;
	a.flags = 1;
	uint8_t buf[kAdvicePayload];
	advice_encode(a, buf);
	Advice b;
	CHECK(advice_decode(buf, sizeof(buf), &b), "advice decodes");
	CHECK(b.seq == 7 && b.lateMs == 123456 && b.lateEvents == 42 && b.delay == 5 && b.rttP50 == 187 && b.flags == 1,
	      "advice round trip");
	CHECK(!advice_decode(buf, sizeof(buf) - 1, &b), "advice: wrong length refused");
	buf[12] = 9;
	CHECK(!advice_decode(buf, sizeof(buf), &b), "advice: delay above 8 refused");
	AdviceReceiver r;
	double d = -1;
	Advice x;
	x.seq = 1;
	x.lateMs = 100;
	CHECK(r.take(x, &d) && d == 0, "first report: no delta");
	x.seq = 3;
	x.lateMs = 180;
	CHECK(r.take(x, &d) && d == 80, "a lost report costs nothing (cumulative)");
	x.seq = 2;
	x.lateMs = 150;
	CHECK(!r.take(x, &d), "stale report dropped");
	x.seq = 3;
	CHECK(!r.take(x, &d), "duplicate report dropped");
	x.seq = 4;
	x.lateMs = 180;
	CHECK(r.take(x, &d) && d == 0 && r.reports() == 3, "no new lateness");
}

void test_stats()
{
	SessionStats s;
	s.reset();
	StallEvent e;
	e.startMs = 1000;
	e.durMs = 80;
	s.add_stall(e);
	e.startMs = 9000;
	e.durMs = 20;
	e.excluded = true;
	s.add_stall(e);
	uint64_t n = 0;
	double ms = 0;
	s.recent(12000, 10000, &n, &ms);
	CHECK(n == 1 && ms == 20, "stats: last 10 s");
	CHECK(s.stall_count() == 2 && s.stall_total_ms() == 100 && s.stall_max_ms() == 80, "stats totals");
	CHECK(s.stall_excluded() == 1, "stats excluded count");
	s.add_rtt(60);
	s.add_rtt(70);
	s.add_rtt(50);
	CHECK(s.rtt_last() == 50, "stats rtt last");
	CHECK(s.rtt_jitter() == 15, "stats jitter = mean |diff|");
	CHECK(s.rtt_percentile(50) == 60, "stats rtt p50");
}

// ---- 2. impairment model ----

void test_impair()
{
	using namespace pc_netplay_impair;
	std::vector<Point> s;
	std::string err;
	CHECK(parse_schedule("0:20,30:20,120:80,160:80,200:20", &s, &err) && s.size() == 5, "impair schedule parses");
	CHECK(schedule_ms(s, 0, 99) == 20, "impair at start");
	CHECK(schedule_ms(s, 75, 99) == 50, "impair mid-ramp");
	CHECK(schedule_ms(s, 140, 99) == 80, "impair plateau");
	CHECK(schedule_ms(s, 180, 99) == 50, "impair ramp down");
	CHECK(schedule_ms(s, 500, 99) == 20, "impair after the end");
	CHECK(schedule_ms({}, 5, 33) == 33, "impair fallback");
	CHECK(!parse_schedule("10:5,5:5", &s, &err), "impair decreasing seconds refused");
	SpikeSpec sp;
	CHECK(parse_spikes("20000:250:400", &sp, &err) && sp.on(), "spikes parse");
	CHECK(!parse_spikes("20000:400:250", &sp, &err), "spikes min > max refused");
	CHECK(!parse_spikes("300:250:400", &sp, &err), "spikes max >= every refused");
	CHECK(parse_spikes("", &sp, &err) && !sp.on(), "no spikes");
	CHECK(parse_spikes("20000:250:400", &sp, &err), "spikes reparse");
	Blocks a, b;
	a.configure(sp, 7);
	b.configure(sp, 7);
	std::vector<double> starts, durs;
	double lastStart = 0;
	bool inBlock = false;
	for (double t = 0; t < 300000; t += 1.0) {
		double end = 0;
		bool entered = false;
		const bool ba = a.blocked(t, &end, &entered);
		double endB = 0;
		bool enteredB = false;
		const bool bb = b.blocked(t, &endB, &enteredB);
		if (ba != bb || entered != enteredB) {
			CHECK(false, "blocks: same seed, same schedule");
			break;
		}
		if (entered) {
			starts.push_back(a.start_ms());
			durs.push_back(a.end_ms() - a.start_ms());
			if (starts.size() > 1) {
				const double gap = a.start_ms() - lastStart;
				CHECK(gap >= 0.8 * 20000 - 1 && gap <= 1.2 * 20000 + 1, "blocks: spacing ~20 s");
			}
			lastStart = a.start_ms();
		}
		(void)inBlock;
		inBlock = ba;
	}
	CHECK(starts.size() >= 12 && starts.size() <= 19, "blocks: 12-19 blocks in 300 s");
	for (double d : durs) CHECK(d >= 250 && d <= 400, "blocks: 250-400 ms");
	Blocks c;
	c.configure(sp, 8);
	double firstC = -1;
	for (double t = 0; t < 30000 && firstC < 0; t += 1.0) {
		bool entered = false;
		if (c.blocked(t, nullptr, &entered) && entered) firstC = c.start_ms();
	}
	CHECK(!starts.empty() && firstC >= 0 && firstC != starts[0], "blocks: another seed, another schedule");
}

// ---- 3. delay transitions against GekkoNet ----

// In-memory link with a fixed latency in turns (deterministic).
struct Flight {
	int dueTurn = 0;
	std::vector<uint8_t> bytes;
};
std::vector<Flight> gWire[2]; // inbound per side
std::vector<GekkoNetResult*> gResults[2];
int gTurn = 0;
int gLatencyTurns = 3;

void mem_send(int side, GekkoNetAddress* addr, const char* data, int length)
{
	if (addr == nullptr || data == nullptr || length <= 0 || addr->size != 1 || length > 4096) return;
	const int dest = ((const uint8_t*)addr->data)[0];
	if (dest != 0 && dest != 1) return;
	(void)side;
	Flight f;
	f.dueTurn = gTurn + gLatencyTurns;
	f.bytes.assign(data, data + length);
	gWire[dest].push_back(f);
}

GekkoNetResult** mem_recv(int side, int* length)
{
	gResults[side].clear();
	*length = 0;
	std::vector<Flight> keep;
	for (Flight& f : gWire[side]) {
		if (f.dueTurn > gTurn) {
			keep.push_back(f);
			continue;
		}
		GekkoNetResult* res = (GekkoNetResult*)std::malloc(sizeof(GekkoNetResult));
		uint8_t* addrBuf = (uint8_t*)std::malloc(1);
		void* pay = std::malloc(f.bytes.size());
		addrBuf[0] = (uint8_t)(side == 0 ? 1 : 0);
		std::memcpy(pay, f.bytes.data(), f.bytes.size());
		res->addr.data = addrBuf;
		res->addr.size = 1;
		res->data_len = (unsigned)f.bytes.size();
		res->data = pay;
		gResults[side].push_back(res);
	}
	gWire[side].swap(keep);
	*length = (int)gResults[side].size();
	return gResults[side].empty() ? nullptr : gResults[side].data();
}

void send0(GekkoNetAddress* a, const char* d, int l) { mem_send(0, a, d, l); }
void send1(GekkoNetAddress* a, const char* d, int l) { mem_send(1, a, d, l); }
GekkoNetResult** recv0(int* l) { return mem_recv(0, l); }
GekkoNetResult** recv1(int* l) { return mem_recv(1, l); }
void mem_free(void* p) { std::free(p); }

// Scripted record i of peer p (16 bytes, never all zero, so it cannot be
// mistaken for GekkoNet's empty prefill input).
void record(int p, uint64_t i, uint8_t w[16])
{
	std::memset(w, 0, 16);
	w[0] = (uint8_t)(0xA0 + p);
	w[1] = (uint8_t)(i & 0xFF);
	w[2] = (uint8_t)((i >> 8) & 0xFF);
	w[3] = (uint8_t)((i * 37 + (uint64_t)p * 11) & 0xFF);
}

struct Peer {
	GekkoSession* s = nullptr;
	int handle = 0;
	SubmitGate gate;
	unsigned d0 = 0;
	uint64_t advances = 0;
	uint64_t scriptIdx = 0;
	std::vector<ScheduleStep> sched;
	size_t schedIdx = 0;
	std::vector<std::vector<uint8_t>> frames; // 32 bytes per advanced frame
	int changes = 0;
};

// One peer's submit turn, the session's arithmetic: on a due turn a
// scheduled change shrinks (skipping submits) or grows (k + 1 adds with the
// no-fill setter, or with the filling setter for the control run).
void submit_turn(Peer& pr, int p, bool useFill)
{
	unsigned grow = 0;
	if (pr.gate.due(pr.advances) && pr.schedIdx < pr.sched.size() && pr.advances >= pr.sched[pr.schedIdx].frame) {
		const unsigned target = pr.sched[pr.schedIdx++].delay;
		if (target < pr.gate.delay) {
			gekko_set_local_delay_nofill(pr.s, pr.handle, (unsigned char)target);
			pr.gate.delay = target;
			++pr.changes;
		} else if (target > pr.gate.delay) {
			grow = target - pr.gate.delay;
		}
	}
	if (!pr.gate.due(pr.advances)) return;
	if (useFill && grow > 0) {
		// Control: GekkoNet's own setter after one add fills the new frames
		// (with more copies than the growth when the old delay is not 0).
		uint8_t w[16];
		record(p, pr.scriptIdx++, w);
		gekko_add_local_input(pr.s, pr.handle, w);
		++pr.gate.nextLand;
		gekko_set_local_delay(pr.s, pr.handle, (unsigned char)(pr.gate.delay + grow));
		pr.gate.nextLand += pr.gate.delay + grow; // what SetDelay appended
		pr.gate.delay += grow;
		++pr.changes;
		return;
	}
	const unsigned from = pr.gate.delay;
	for (unsigned j = 0; j <= grow; ++j) {
		if (j > 0) gekko_set_local_delay_nofill(pr.s, pr.handle, (unsigned char)(from + j));
		uint8_t w[16];
		record(p, pr.scriptIdx++, w);
		gekko_add_local_input(pr.s, pr.handle, w);
		++pr.gate.nextLand;
	}
	if (grow > 0) {
		pr.gate.delay = from + grow;
		++pr.changes;
	}
}

struct TransitionResult {
	bool ok = false;
	uint64_t frames = 0;
	int mismatches = 0;     // peer frames that differ between the two sessions
	int wrongRecords = 0;   // frames whose input is not record F - d0
	int desyncs = 0;
	int changes[2] = { 0, 0 };
};

TransitionResult run_transitions(unsigned d0a, const char* schedA, unsigned d0b, const char* schedB, int frames,
                                 int latencyTurns, bool useFill)
{
	TransitionResult r;
	gWire[0].clear();
	gWire[1].clear();
	gTurn = 0;
	gLatencyTurns = latencyTurns;
	GekkoNetAdapter ad[2];
	ad[0].send_data = send0;
	ad[0].receive_data = recv0;
	ad[0].free_data = mem_free;
	ad[1].send_data = send1;
	ad[1].receive_data = recv1;
	ad[1].free_data = mem_free;
	GekkoConfig cfg;
	std::memset(&cfg, 0, sizeof(cfg));
	cfg.num_players = 2;
	cfg.input_prediction_window = 0;
	cfg.input_size = 16;
	cfg.state_size = 8;
	cfg.desync_detection = true;
	cfg.check_distance = 7;
	Peer pr[2];
	uint8_t blob0[1] = { 0 };
	uint8_t blob1[1] = { 1 };
	for (int p = 0; p < 2; ++p) {
		gekko_create(&pr[p].s, GekkoGameSession);
		gekko_start(pr[p].s, &cfg);
		gekko_net_adapter_set(pr[p].s, &ad[p]);
	}
	GekkoNetAddress to1 = { blob1, 1 };
	GekkoNetAddress to0 = { blob0, 1 };
	pr[0].handle = gekko_add_actor(pr[0].s, GekkoLocalPlayer, nullptr);
	(void)gekko_add_actor(pr[0].s, GekkoRemotePlayer, &to1);
	(void)gekko_add_actor(pr[1].s, GekkoRemotePlayer, &to0);
	pr[1].handle = gekko_add_actor(pr[1].s, GekkoLocalPlayer, nullptr);
	pr[0].d0 = d0a;
	pr[1].d0 = d0b;
	std::string err;
	parse_schedule(schedA, 1, 8, &pr[0].sched, &err);
	parse_schedule(schedB, 1, 8, &pr[1].sched, &err);
	for (int p = 0; p < 2; ++p) {
		gekko_set_local_delay(pr[p].s, pr[p].handle, (unsigned char)pr[p].d0);
		gekko_set_disconnect_timeout(pr[p].s, 0);
		pr[p].gate.start(pr[p].d0);
	}
	bool started[2] = { false, false };
	// Bounded by wall time, not turns: GekkoNet's session sync retries on a
	// 200 ms wall-clock timer, so a fast loop must not give up early.
	const auto wallStart = std::chrono::steady_clock::now();
	for (int turn = 0;; ++turn) {
		if (std::chrono::steady_clock::now() - wallStart > std::chrono::seconds(20)) break;
		gTurn = turn;
		for (int p = 0; p < 2; ++p) {
			if (started[p] && (int)pr[p].advances < frames) submit_turn(pr[p], p, useFill);
			int n = 0;
			GekkoGameEvent** ev = gekko_update_session(pr[p].s, &n);
			for (int i = 0; ev != nullptr && i < n; ++i) {
				if (ev[i] == nullptr) continue;
				if (ev[i]->type == GekkoAdvanceEvent) {
					const uint8_t* in = ev[i]->data.adv.inputs;
					pr[p].frames.emplace_back(in, in + 32);
					++pr[p].advances;
				} else if (ev[i]->type == GekkoSaveEvent) {
					if (ev[i]->data.save.checksum != nullptr) *ev[i]->data.save.checksum = 1;
					if (ev[i]->data.save.state_len != nullptr) *ev[i]->data.save.state_len = 8;
				}
			}
			int m = 0;
			GekkoSessionEvent** sev = gekko_session_events(pr[p].s, &m);
			for (int i = 0; sev != nullptr && i < m; ++i) {
				if (sev[i] == nullptr) continue;
				if (sev[i]->type == GekkoSessionStarted) started[p] = true;
				if (sev[i]->type == GekkoDesyncDetected) ++r.desyncs;
			}
		}
		if ((int)pr[0].advances >= frames && (int)pr[1].advances >= frames) break;
	}
	r.frames = std::min(pr[0].frames.size(), pr[1].frames.size());
	for (uint64_t f = 0; f < r.frames; ++f) {
		if (pr[0].frames[f] != pr[1].frames[f]) ++r.mismatches;
		for (int p = 0; p < 2; ++p) {
			uint8_t want[16];
			if (f < pr[p].d0) std::memset(want, 0, 16); // GekkoNet's empty prefill
			else record(p, f - pr[p].d0, want);
			if (std::memcmp(pr[0].frames[f].data() + 16 * p, want, 16) != 0) ++r.wrongRecords;
		}
	}
	r.changes[0] = pr[0].changes;
	r.changes[1] = pr[1].changes;
	r.ok = (int)r.frames >= frames;
	for (int p = 0; p < 2; ++p) gekko_destroy(&pr[p].s);
	return r;
}

void test_transitions()
{
	// Many growths and shrinks, both peers, different starting delays, up to
	// the maximum (8) and down to the minimum (1).
	TransitionResult r = run_transitions(2, "60:4,120:1,180:8,240:3,300:5,360:2,420:7,480:1,540:3", 3,
	                                     "90:1,150:6,210:2,330:8,400:1,470:4,560:2", 640, 3, false);
	std::printf("transitions: frames=%llu mismatches=%d wrong=%d desyncs=%d changes=%d/%d\n",
	            (unsigned long long)r.frames, r.mismatches, r.wrongRecords, r.desyncs, r.changes[0], r.changes[1]);
	CHECK(r.ok, "transitions: both peers advanced every frame");
	CHECK(r.changes[0] == 9 && r.changes[1] == 7, "transitions: every scheduled change applied");
	CHECK(r.mismatches == 0, "transitions: both peers ran identical inputs");
	CHECK(r.wrongRecords == 0, "transitions: frame F carries record F - d0 (fixed-delay inputs)");
	CHECK(r.desyncs == 0, "transitions: no desync");
	// Zero latency and a long latency behave the same.
	TransitionResult r0 = run_transitions(1, "40:8,80:1,120:8", 8, "50:1,90:8,130:2", 200, 0, false);
	std::printf("transitions latency 0: frames=%llu mismatches=%d wrong=%d desyncs=%d changes=%d/%d\n",
	            (unsigned long long)r0.frames, r0.mismatches, r0.wrongRecords, r0.desyncs, r0.changes[0],
	            r0.changes[1]);
	CHECK(r0.ok && r0.mismatches == 0 && r0.wrongRecords == 0, "transitions: latency 0 turns");
	TransitionResult r9 = run_transitions(4, "40:8,80:1,120:8", 2, "50:1,90:8,130:2", 200, 20, false);
	std::printf("transitions latency 20: frames=%llu mismatches=%d wrong=%d desyncs=%d changes=%d/%d\n",
	            (unsigned long long)r9.frames, r9.mismatches, r9.wrongRecords, r9.desyncs, r9.changes[0],
	            r9.changes[1]);
	CHECK(r9.ok && r9.mismatches == 0 && r9.wrongRecords == 0, "transitions: latency 20 turns");
	// Control: GekkoNet's filling setter keeps the peers identical (lockstep)
	// but puts copies on the grown frames and drops later local inputs, so a
	// scripted pair would no longer replay the fixed-delay inputs.
	TransitionResult rc = run_transitions(2, "60:4", 3, "", 200, 3, true);
	std::printf("fill control: frames=%llu mismatches=%d wrong=%d\n", (unsigned long long)rc.frames, rc.mismatches,
	            rc.wrongRecords);
	CHECK(rc.ok && rc.mismatches == 0, "fill control: still lockstep-identical");
	CHECK(rc.wrongRecords > 0, "fill control: frames no longer carry record F - d0");
}

} // namespace

int main()
{
	test_delay_for_rtt();
	test_hist();
	test_schedule_parse();
	test_controller();
	test_advice();
	test_stats();
	test_impair();
	test_transitions();
	if (sFailures == 0) {
		std::printf("pc_netplay_adaptive_test: PASS\n");
		return 0;
	}
	std::printf("pc_netplay_adaptive_test: %d failure(s)\n", sFailures);
	return 1;
}
