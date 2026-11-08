// Host-run test for the netplay session HUD's pure
// pieces (pc_netplay_hud_model.h, header-only, no SDL/game): the local stall
// window, the quality classification, the HUD text and the toggle edge.

#include "netplay/pc_netplay_hud_model.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int sFailures = 0;
int sChecks   = 0;

void check(bool ok, const char* what, int line)
{
	++sChecks;
	if (!ok) {
		++sFailures;
		std::printf("FAIL line %d: %s\n", line, what);
	}
}
#define CHECK(ok, what) check((ok), (what), __LINE__)

using namespace pc_netplay_hud;

} // namespace

int main()
{
	// 1. Stall window.
	{
		StallWindow w;
		w.add(1000.0, 10.0); // shorter than one frame: not a stall
		CHECK(w.count(1000.0) == 0 && w.total() == 0, "sub-frame waits are not stalls");
		w.add(2000.0, 40.0);
		w.add(5000.0, 250.0);
		CHECK(w.count(5000.0) == 2, "two stalls in the window");
		CHECK(w.stalled_ms(5000.0) == 290.0, "stalled ms in the window");
		CHECK(w.count(12500.0) == 1, "the 2 s stall has left the 10 s window at 12.5 s");
		CHECK(w.count(15001.0) == 0, "both have left by 15 s");
		CHECK(w.total() == 2 && w.total_ms() == 290.0 && w.max_ms() == 250.0, "session totals");
		// Fix round 1 (evidence review MINOR-1): the worst real link, one-frame
		// stalls back to back (34 ms waits, one Advance turn between them),
		// is counted in full: the ring used to cap the count at 64.
		StallWindow v;
		double t = 100000.0;
		for (int i = 0; i < 600; ++i) {
			t += 35.0;
			v.add(t, 34.0);
		}
		unsigned want = 0;
		for (int i = 0; i < 600; ++i) {
			if (t - (100000.0 + 35.0 * (i + 1)) < kWindowMs) ++want;
		}
		CHECK(want == 286 && v.count(t) == want, "back-to-back one-frame stalls: every one in the window counts");
		CHECK(v.stalled_ms(t) == 34.0 * want, "and all of their time");
		CHECK(kStallRing >= (int)(kWindowMs / kStallMinMs) + 1, "the ring holds every episode the window can hold");
		// Physically impossible spacing (1 ms apart): the ring keeps the newest.
		for (int i = 0; i < 400; ++i) w.add(20000.0 + i, 50.0);
		CHECK(w.count(20400.0) == (unsigned)kStallRing, "the ring keeps the newest episodes");
		CHECK(w.total() == 402, "totals keep counting past the ring");
	}

	// 2. Quality.
	{
		CHECK(classify(false, 0, 0, 0) == kQualityUnknown, "no ping yet");
		CHECK(classify(false, 0, 0, 5) == kQualityPoor, "many stalls before the first ping");
		CHECK(classify(true, 40, 3, 0) == kQualityGood, "good");
		CHECK(classify(true, 40, 3, 1) == kQualityFair, "one stall: fair");
		CHECK(classify(true, 150, 3, 0) == kQualityFair, "150 ms: fair");
		CHECK(classify(true, 40, 25, 0) == kQualityFair, "jittery: fair");
		CHECK(classify(true, 250, 3, 0) == kQualityPoor, "250 ms: poor");
		CHECK(classify(true, 40, 60, 0) == kQualityPoor, "very jittery: poor");
		CHECK(classify(true, 40, 3, 4) == kQualityPoor, "4 stalls in 10 s: poor");
		CHECK(std::strcmp(quality_name(kQualityFair), "fair") == 0, "names");
		uint8_t g[4], r[4];
		quality_rgba(kQualityGood, g);
		quality_rgba(kQualityPoor, r);
		CHECK(g[1] > g[0] && r[0] > r[1] && g[3] == 255, "green and red");
	}

	// 3. Text.
	{
		Numbers n;
		n.havePing = true;
		n.pingMs = 42.4f;
		n.jitterMs = 3.2f;
		n.delay = 2;
		n.stalls10s = 1;
		n.stallMs10s = 120;
		char a[64], b[64], c[64];
		format_lines(n, a, sizeof a, b, sizeof b, c, sizeof c);
		CHECK(std::string(a) == "NETPLAY  fair", "line 1");
		CHECK(std::string(b) == "ping 42 ms  jitter 3 ms", "line 2");
		CHECK(std::string(c) == "delay 2 (67 ms)  stalls 1 / 10 s", "line 3");
		n.havePing = false;
		n.stallMs10s = 2500;
		format_lines(n, a, sizeof a, b, sizeof b, c, sizeof c);
		CHECK(std::string(b) == "ping --  jitter --", "no ping yet");
		CHECK(std::string(c).find("(2.5 s)") != std::string::npos, "long stall time shown in seconds");
	}

	// 4. Toggle edge.
	{
		Toggle t;
		CHECK(!t.update(false), "idle");
		CHECK(t.update(true), "press");
		CHECK(!t.update(true), "held");
		CHECK(!t.update(false), "release");
		CHECK(t.update(true), "press again");
	}

	std::printf("pc_netplay_hud_model_test: %s (%d checks, %d failures)\n", sFailures == 0 ? "PASS" : "FAIL",
	            sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
