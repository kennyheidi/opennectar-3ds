// Host-run test for the netplay load guard.
//
// Part 1 covers the engine-free policy in pc_netplay_loadguard.h: the stall
// injector's env parser, role match and slicing, the load window's
// open/extend/close rules and the keep-alive rate gate.
//
// Part 2 checks the GekkoNet behaviour the design relies on against the
// vendored library itself (two GekkoGameSessions over an in-memory link, a
// short 1 s disconnect timeout so the test stays quick), instead of
// guessing its semantics:
//   A. a peer that stops pumping for longer than the timeout is dropped by
//      the other peer, and then drops that peer itself once it resumes (both
//      peers disconnect);
//   B. the same stall survives when the stalled peer calls only
//      gekko_network_poll every 50 ms (the keep-alive): no disconnect, no
//      Advance produced by the polls, identical checksums afterwards;
//   C. network polls issued while an Advance event returned by
//      gekko_update_session is being executed leave that event and its inputs
//      untouched (the keep-alive runs inside the tick);
//   D. a single un-pumped block longer than the normal timeout survives when
//      both peers raised the timeout before it (the load window), and the
//      normal timeout can be restored once inputs flow again;
//   E. a peer that really dies while the other keeps pumping is detected
//      after the timeout (not earlier, not much later).
// Engine-free: links gekkonet only (no game, no SDL).

#include "netplay/pc_netplay_loadguard.h"

#include "gekkonet.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
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

double now_ms()
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void sleep_ms(double ms)
{
	std::this_thread::sleep_for(std::chrono::microseconds((long long)(ms * 1000.0)));
}

// ---- in-memory endpoint pair (no sockets; addresses are 1-byte side ids) ----

std::vector<std::vector<uint8_t>> gInbound[2];
std::vector<GekkoNetResult*> gResults[2];
bool gDead[2] = { false, false }; // a dead side neither sends nor receives

void mem_send(int side, GekkoNetAddress* addr, const char* data, int length)
{
	if (gDead[side] || addr == nullptr || data == nullptr || length <= 0) return;
	if (addr->data == nullptr || addr->size != 1 || length > 4096) return;
	const int dest = ((const uint8_t*)addr->data)[0];
	if (dest != 0 && dest != 1) return;
	gInbound[dest].emplace_back(data, data + length);
}

GekkoNetResult** mem_recv(int side, int* length)
{
	gResults[side].clear();
	*length = 0;
	for (std::vector<uint8_t>& raw : gInbound[side]) {
		GekkoNetResult* res = (GekkoNetResult*)std::malloc(sizeof(GekkoNetResult));
		uint8_t* addrBuf    = (uint8_t*)std::malloc(1);
		void* pay           = std::malloc(raw.size());
		if (res == nullptr || addrBuf == nullptr || pay == nullptr) {
			std::free(res);
			std::free(addrBuf);
			std::free(pay);
			continue;
		}
		addrBuf[0] = (uint8_t)(side == 0 ? 1 : 0); // the sender
		std::memcpy(pay, raw.data(), raw.size());
		res->addr.data = addrBuf;
		res->addr.size = 1;
		res->data_len  = (unsigned)raw.size();
		res->data      = pay;
		gResults[side].push_back(res);
	}
	gInbound[side].clear();
	*length = (int)gResults[side].size();
	return gResults[side].empty() ? nullptr : gResults[side].data();
}

void send0(GekkoNetAddress* a, const char* d, int l) { mem_send(0, a, d, l); }
void send1(GekkoNetAddress* a, const char* d, int l) { mem_send(1, a, d, l); }
GekkoNetResult** recv0(int* l) { return mem_recv(0, l); }
GekkoNetResult** recv1(int* l) { return mem_recv(1, l); }
void mem_free(void* p) { std::free(p); }

uint64_t toy_advance(uint64_t state, int frame, const uint8_t* inputs)
{
	uint64_t mix = (uint64_t)(uint32_t)frame * 0x9E3779B97F4A7C15ULL;
	for (int i = 0; i < 32; ++i) mix = mix * 0x100000001B3ULL ^ inputs[i];
	return state * 0x100000001B3ULL ^ mix;
}

uint32_t toy_checksum(uint64_t s) { return (uint32_t)(s ^ (s >> 32)); }

// Two role-ordered lockstep sessions (window 0, delay 2), as the game runs.
struct Pair {
	GekkoNetAdapter ad[2];
	GekkoSession* s[2] = { nullptr, nullptr };
	int handle[2]      = { 0, 1 };
	uint64_t state[2]  = { 0x1234ULL, 0x1234ULL };
	int adv[2]         = { 0, 0 };
	int fed[2]         = { 0, 0 };
	bool started[2]    = { false, false };
	bool dropped[2]    = { false, false }; // saw GekkoPlayerDisconnected
	double droppedAt[2] = { 0, 0 };
	int desyncs        = 0;
	std::vector<uint32_t> sums[2];
	uint8_t blob0[1] = { 0 };
	uint8_t blob1[1] = { 1 };

	explicit Pair(unsigned timeoutMs)
	{
		for (int i = 0; i < 2; ++i) {
			gInbound[i].clear();
			gDead[i] = false;
		}
		ad[0].send_data = send0;
		ad[0].receive_data = recv0;
		ad[0].free_data = mem_free;
		ad[1].send_data = send1;
		ad[1].receive_data = recv1;
		ad[1].free_data = mem_free;
		GekkoConfig cfg;
		std::memset(&cfg, 0, sizeof(cfg));
		cfg.num_players             = 2;
		cfg.input_prediction_window = 0;
		cfg.input_size              = 16;
		cfg.state_size              = 8;
		cfg.desync_detection        = true;
		cfg.check_distance          = 7;
		for (int p = 0; p < 2; ++p) {
			gekko_create(&s[p], GekkoGameSession);
			gekko_start(s[p], &cfg);
			gekko_net_adapter_set(s[p], &ad[p]); // after gekko_start
		}
		GekkoNetAddress to1 = { blob1, 1 };
		GekkoNetAddress to0 = { blob0, 1 };
		handle[0] = gekko_add_actor(s[0], GekkoLocalPlayer, nullptr);
		(void)gekko_add_actor(s[0], GekkoRemotePlayer, &to1);
		(void)gekko_add_actor(s[1], GekkoRemotePlayer, &to0);
		handle[1] = gekko_add_actor(s[1], GekkoLocalPlayer, nullptr);
		for (int p = 0; p < 2; ++p) {
			gekko_set_local_delay(s[p], handle[p], 2);
			gekko_set_disconnect_timeout(s[p], timeoutMs);
		}
	}
	~Pair()
	{
		for (int p = 0; p < 2; ++p)
			if (s[p] != nullptr) gekko_destroy(&s[p]);
	}

	void input(int p, int frame, uint8_t w[16])
	{
		std::memset(w, 0, 16);
		w[0] = (uint8_t)(p + 1);
		w[1] = (uint8_t)(frame * 37 + p);
		w[2] = (uint8_t)(frame >> 3);
	}

	// Handles one batch of game events (Advance/Save). Returns the Advances.
	int run_events(int p, GekkoGameEvent** ev, int n)
	{
		int a = 0;
		for (int i = 0; ev != nullptr && i < n; ++i) {
			if (ev[i] == nullptr) continue;
			if (ev[i]->type == GekkoAdvanceEvent) {
				state[p] = toy_advance(state[p], ev[i]->data.adv.frame, ev[i]->data.adv.inputs);
				++adv[p];
				++a;
			} else if (ev[i]->type == GekkoSaveEvent) {
				if (ev[i]->data.save.checksum != nullptr) *ev[i]->data.save.checksum = toy_checksum(state[p]);
				if (ev[i]->data.save.state != nullptr && ev[i]->data.save.state_len != nullptr
				    && *ev[i]->data.save.state_len >= 8) {
					std::memset(ev[i]->data.save.state, 0, 8);
					*ev[i]->data.save.state_len = 8;
				}
				sums[p].push_back(toy_checksum(state[p]));
			}
		}
		return a;
	}

	void session_events(int p)
	{
		int m                   = 0;
		GekkoSessionEvent** sev = gekko_session_events(s[p], &m);
		for (int i = 0; sev != nullptr && i < m; ++i) {
			if (sev[i] == nullptr) continue;
			if (sev[i]->type == GekkoSessionStarted) started[p] = true;
			if (sev[i]->type == GekkoDesyncDetected) ++desyncs;
			if (sev[i]->type == GekkoPlayerDisconnected && !dropped[p]) {
				dropped[p]   = true;
				droppedAt[p] = now_ms();
			}
		}
	}

	// One main-loop turn of peer p: submit (one per Advance), update, events.
	int turn(int p)
	{
		if (started[p] && fed[p] == adv[p]) {
			uint8_t w[16];
			input(p, fed[p], w);
			gekko_add_local_input(s[p], handle[p], w);
			++fed[p];
		}
		int n               = 0;
		GekkoGameEvent** ev = gekko_update_session(s[p], &n);
		const int a         = run_events(p, ev, n);
		session_events(p);
		return a;
	}

	// Both peers turn every ~1 ms until both advanced `frames` or a drop.
	bool run_until(int frames, double budgetMs)
	{
		const double t0 = now_ms();
		while ((adv[0] < frames || adv[1] < frames) && now_ms() - t0 < budgetMs) {
			for (int p = 0; p < 2; ++p)
				if (adv[p] < frames) turn(p);
			if (dropped[0] || dropped[1]) return false;
			sleep_ms(1.0);
		}
		return adv[0] >= frames && adv[1] >= frames;
	}

	// Peer `stalled` blocks for `ms` (pumping gekko_network_poll every
	// `pumpMs` when pumpMs > 0, never gekko_update_session) while the other
	// peer keeps turning. Returns the Advances the stalled peer made (0).
	int stall(int stalled, double ms, double pumpMs)
	{
		const int other = 1 - stalled;
		const int advBefore = adv[stalled];
		const double t0 = now_ms();
		double lastPump = t0;
		while (now_ms() - t0 < ms) {
			turn(other);
			if (pumpMs > 0 && now_ms() - lastPump >= pumpMs) {
				gekko_network_poll(s[stalled]);
				lastPump = now_ms();
			}
			sleep_ms(1.0);
		}
		return adv[stalled] - advBefore;
	}

	bool same_sums() const
	{
		const size_t n = sums[0].size() < sums[1].size() ? sums[0].size() : sums[1].size();
		if (n == 0) return false;
		for (size_t i = 0; i < n; ++i)
			if (sums[0][i] != sums[1][i]) return false;
		return true;
	}
};

void policy_tests()
{
	using namespace pc_netplay_loadguard;
	// Stall plan parsing.
	{
		StallPlan p;
		std::string err;
		CHECK(parse_stall_plan(nullptr, "tick:5", "host", "0", &p, &err) && !p.enabled, "unset ms = off");
		CHECK(parse_stall_plan("25000", nullptr, nullptr, nullptr, &p, &err) && p.enabled && p.totalMs == 25000
		          && p.at == StallAt::Load && p.index == 1 && p.roles == kStallBoth && p.sliceMs == 250,
		      "defaults: load#1, both, 250 ms slices");
		CHECK(parse_stall_plan("1000", "load:3", "join", "0", &p, &err) && p.at == StallAt::Load && p.index == 3
		          && p.roles == kStallJoin && p.sliceMs == 0,
		      "load:3 join slice 0");
		CHECK(parse_stall_plan("1000", "shader", "host", "100", &p, &err) && p.at == StallAt::Shader
		          && p.roles == kStallHost && p.sliceMs == 100,
		      "shader host 100");
		CHECK(parse_stall_plan("1000", "tick:1500", "both", nullptr, &p, &err) && p.at == StallAt::Tick
		          && p.index == 1500,
		      "tick:1500");
		const char* badMs[] = { "0", "-5", "25s", "", "600001", " 5" };
		for (const char* b : badMs)
			CHECK(!parse_stall_plan(b, nullptr, nullptr, nullptr, &p, &err) && !p.enabled, "bad ms rejected");
		const char* badAt[] = { "tick", "tick:", "tick:0", "load:0", "load:x", "Load", "shader:1", "" };
		for (const char* b : badAt)
			CHECK(!parse_stall_plan("1000", b, nullptr, nullptr, &p, &err) && !p.enabled, "bad at rejected");
		CHECK(!parse_stall_plan("1000", nullptr, "joiner", nullptr, &p, &err) && !p.enabled, "bad role rejected");
		CHECK(!parse_stall_plan("1000", nullptr, nullptr, "60001", &p, &err) && !p.enabled, "bad slice rejected");
	}
	// Role match.
	{
		StallPlan p;
		std::string err;
		parse_stall_plan("1000", nullptr, "join", nullptr, &p, &err);
		CHECK(!stall_applies_to(p, true) && stall_applies_to(p, false), "join only");
		parse_stall_plan("1000", nullptr, "host", nullptr, &p, &err);
		CHECK(stall_applies_to(p, true) && !stall_applies_to(p, false), "host only");
		parse_stall_plan("1000", nullptr, "both", nullptr, &p, &err);
		CHECK(stall_applies_to(p, true) && stall_applies_to(p, false), "both");
		StallPlan off;
		CHECK(!stall_applies_to(off, true) && !stall_applies_to(off, false), "off applies to nobody");
	}
	// Slicing: slices sum to the total; slice 0 is one block.
	{
		StallPlan p;
		std::string err;
		parse_stall_plan("25000", nullptr, nullptr, "250", &p, &err);
		unsigned sum = 0, n = 0;
		for (unsigned i = 0; stall_slice_ms(p, i) != 0; ++i, ++n) sum += stall_slice_ms(p, i);
		CHECK(sum == 25000 && n == 100, "25 s in 100 slices of 250 ms");
		parse_stall_plan("1000", nullptr, nullptr, "300", &p, &err);
		CHECK(stall_slice_ms(p, 0) == 300 && stall_slice_ms(p, 3) == 100 && stall_slice_ms(p, 4) == 0,
		      "last slice is the remainder");
		parse_stall_plan("25000", nullptr, nullptr, "0", &p, &err);
		CHECK(stall_slice_ms(p, 0) == 25000 && stall_slice_ms(p, 1) == 0, "slice 0 = one 25 s block");
	}
	// Load window.
	{
		LoadWindow w;
		w.configure(true, 15000, 60000);
		CHECK(!w.is_open() && w.load_ms() == 60000 && w.normal_ms() == 15000, "configured closed");
		CHECK(w.open(3) && w.is_open() && w.close_frame() == 3 + kLoadWindowFrames, "opens at the load frame");
		CHECK(!w.after_advance(3) && !w.after_advance(3 + kLoadWindowFrames - 1), "stays open before F+30");
		CHECK(!w.open(10) && w.close_frame() == 10 + kLoadWindowFrames, "second load extends, no re-raise");
		CHECK(!w.after_advance(3 + kLoadWindowFrames), "old close frame no longer closes");
		CHECK(w.after_advance(10 + kLoadWindowFrames) && !w.is_open(), "closes at F+30 of the last load");
		CHECK(!w.after_advance(100), "closing is reported once");
		CHECK(w.open(200) && w.is_open(), "a later load reopens");
		LoadWindow off;
		off.configure(false, 15000, 60000);
		CHECK(!off.open(3) && !off.is_open() && !off.after_advance(40), "disabled window never opens");
		LoadWindow narrow;
		narrow.configure(true, 15000, 5000);
		CHECK(narrow.load_ms() == 15000, "load timeout never below the normal one");
		// The close rule's claim: a local Advance of frame X needs the peer's
		// input for X, submitted with index X - d after the peer completed
		// frames 0..X-d-1; with d <= 8 and X = F+30 the peer is past F+21.
		const uint32_t F = 1000, X = F + kLoadWindowFrames;
		for (uint32_t d = 1; d <= 8; ++d) CHECK(X - d - 1 >= F + 21, "peer past its load frame + 21");
	}
	{
		CHECK(std::strcmp(site_name(kSiteSave), "save") == 0 && std::strcmp(site_name(kSiteShader), "tev") == 0
		          && std::strcmp(site_name(99), "other") == 0 && kSiteSave < kSiteCount,
		      "site names and the save site");
	}
	// Keep-alive gate.
	{
		KeepAliveGate g;
		CHECK(g.due(0.0), "first pump due");
		g.pumped(1000.0);
		CHECK(!g.due(1000.0) && !g.due(1049.9) && g.due(1050.0), "50 ms spacing");
		g.reset();
		CHECK(g.due(1000.0), "reset makes the next pump due");
	}
}

void gekko_tests()
{
	const unsigned kTimeout = 1000; // ms, short so the test is quick
	const double kStall     = 2.5 * kTimeout;
	// The detection-latency bounds allow
	// +1500 ms, so a heavily loaded machine (ctest -j, other builds) cannot
	// push a correct drop out of the bound; A's stall is long enough that
	// the drop still happens inside it.
	const double kLateMs    = 1500.0;
	const double kStallA    = 3.5 * kTimeout;
	// A. Stall without any pump: both peers end up disconnected.
	{
		Pair pr(kTimeout);
		CHECK(pr.run_until(30, 10000), "A: warm-up to frame 30");
		const double t0 = now_ms();
		pr.stall(1, kStallA, 0.0); // the joiner blocks, no pump
		CHECK(pr.dropped[0], "A: running peer drops the silent peer");
		if (pr.dropped[0]) {
			const double lat = pr.droppedAt[0] - t0;
			std::printf("A: running peer dropped the stalled peer %.0f ms into a %.0f ms stall (timeout %u ms)\n", lat,
			            kStallA, kTimeout);
			CHECK(lat >= kTimeout - 50 && lat <= kTimeout + kLateMs, "A: drop after about the timeout");
		}
		// The stalled peer resumes: it reads the Disconnect messages.
		for (int i = 0; i < 500 && !pr.dropped[1]; ++i) {
			pr.turn(1);
			sleep_ms(1.0);
		}
		CHECK(pr.dropped[1], "A: the stalled peer drops too after resuming");
	}
	// B. Same stall with the keep-alive (network poll every 50 ms).
	{
		Pair pr(kTimeout);
		CHECK(pr.run_until(30, 10000), "B: warm-up to frame 30");
		const int stalledAdv = pr.stall(1, kStall, 50.0);
		CHECK(!pr.dropped[0] && !pr.dropped[1], "B: no disconnect with the keep-alive");
		CHECK(stalledAdv == 0, "B: network polls never advance the stalled peer");
		CHECK(pr.adv[0] - pr.adv[1] <= 3, "B: the running peer waited within its delay horizon");
		CHECK(pr.run_until(200, 20000), "B: both advance to frame 200 afterwards");
		CHECK(pr.desyncs == 0 && pr.state[0] == pr.state[1] && pr.same_sums(), "B: identical, 0 desyncs");
	}
	// C. Polls inside an executing Advance leave its event and inputs intact.
	{
		Pair pr(kTimeout);
		CHECK(pr.run_until(30, 10000), "C: warm-up to frame 30");
		bool checked = false;
		for (int tries = 0; tries < 2000 && !checked; ++tries) {
			pr.turn(0);
			// Joiner turn by hand: keep the batch, poll inside it.
			if (pr.started[1] && pr.fed[1] == pr.adv[1]) {
				uint8_t w[16];
				pr.input(1, pr.fed[1], w);
				gekko_add_local_input(pr.s[1], pr.handle[1], w);
				++pr.fed[1];
			}
			int n               = 0;
			GekkoGameEvent** ev = gekko_update_session(pr.s[1], &n);
			int advIdx          = -1;
			for (int i = 0; ev != nullptr && i < n; ++i)
				if (ev[i] != nullptr && ev[i]->type == GekkoAdvanceEvent) advIdx = i;
			if (advIdx >= 0) {
				const int frame = ev[advIdx]->data.adv.frame;
				uint8_t copy[32];
				std::memcpy(copy, ev[advIdx]->data.adv.inputs, 32);
				for (int k = 0; k < 20; ++k) { // the keep-alive inside the tick
					pr.turn(0);                // the peer keeps sending meanwhile
					gekko_network_poll(pr.s[1]);
					sleep_ms(1.0);
				}
				CHECK(ev[advIdx]->type == GekkoAdvanceEvent && ev[advIdx]->data.adv.frame == frame
				          && std::memcmp(copy, ev[advIdx]->data.adv.inputs, 32) == 0,
				      "C: the Advance being executed is untouched by network polls");
				checked = true;
			}
			pr.run_events(1, ev, n);
			pr.session_events(1);
			sleep_ms(1.0);
		}
		CHECK(checked, "C: an Advance was observed");
		CHECK(pr.run_until(120, 20000), "C: both advance to frame 120 afterwards");
		CHECK(pr.desyncs == 0 && pr.state[0] == pr.state[1], "C: identical, 0 desyncs");
	}
	// D. The load window: both peers raise the timeout at the same frame, a
	// single un-pumped block of 2.5x the normal timeout survives, and the
	// normal timeout is restored once inputs flow again.
	{
		Pair pr(kTimeout);
		CHECK(pr.run_until(30, 10000), "D: warm-up to frame 30");
		gekko_set_disconnect_timeout(pr.s[0], 4 * kTimeout);
		gekko_set_disconnect_timeout(pr.s[1], 4 * kTimeout);
		pr.stall(1, kStall, 0.0); // one block, no pump
		CHECK(!pr.dropped[0] && !pr.dropped[1], "D: raised timeout survives a single block");
		const int resumeAt = pr.adv[0];
		CHECK(pr.run_until(resumeAt + (int)pc_netplay_loadguard::kLoadWindowFrames, 20000), "D: inputs flow again");
		gekko_set_disconnect_timeout(pr.s[0], kTimeout);
		gekko_set_disconnect_timeout(pr.s[1], kTimeout);
		CHECK(pr.run_until(resumeAt + 150, 20000) && !pr.dropped[0] && !pr.dropped[1],
		      "D: normal timeout restored without a drop");
		CHECK(pr.desyncs == 0 && pr.state[0] == pr.state[1] && pr.same_sums(), "D: identical, 0 desyncs");
	}
	// E. A really dead peer is still detected after the timeout while the
	// survivor keeps pumping (only the keep-alive's own peer is spared).
	{
		Pair pr(kTimeout);
		CHECK(pr.run_until(30, 10000), "E: warm-up to frame 30");
		gDead[1]        = true; // the joiner dies: silent both ways
		const double t0 = now_ms();
		while (!pr.dropped[0] && now_ms() - t0 < 5000) {
			pr.turn(0);
			sleep_ms(1.0);
		}
		CHECK(pr.dropped[0], "E: survivor reports the dead peer");
		if (pr.dropped[0]) {
			const double lat = pr.droppedAt[0] - t0;
			std::printf("E: survivor reported the dead peer after %.0f ms (timeout %u ms)\n", lat, kTimeout);
			CHECK(lat >= kTimeout - 50 && lat <= kTimeout + kLateMs, "E: detected after about the timeout");
		}
	}
}

} // namespace

int main()
{
	policy_tests();
	gekko_tests();
	if (sFailures != 0) {
		std::printf("pc_netplay_loadguard_test: %d failure(s)\n", sFailures);
		return 1;
	}
	std::printf("pc_netplay_loadguard_test: all passed\n");
	return 0;
}
