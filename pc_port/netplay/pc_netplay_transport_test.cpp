// Host-run netplay transport test.
//
// Two GekkoGameSessions in one process over an in-memory adapter pair with
// the lossy wrapper (latency/jitter/loss, seeded), a toy deterministic
// state, prediction window 0 and 2,000 frames. Expects 0 desyncs and
// identical per-frame checksums on both peers.
//
// Also covers the PcNetplayInput 16-byte round trip and parse_endpoint().
// Engine-free: links gekkonet + pc_netplay_udp only (no game, no SDL).

#include "netplay/pc_netplay_gekko_input.h"
#include "netplay/pc_netplay_udp.h"

#include "gekkonet.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <random>
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

// ---- in-memory endpoint pair (no sockets) ----

struct MemPipe {
	// queued inbound frames per endpoint (0/1)
	std::vector<std::vector<uint8_t>> inbound[2];
	GekkoNetAdapter adapter[2];
};

MemPipe* g_pipe = nullptr;
int g_side = 0; // trampoline side selector (set around each call is avoided;
// instead each adapter struct below closes over its side via two static fns)

void mem_send_side0(GekkoNetAddress* addr, const char* data, int length);
void mem_send_side1(GekkoNetAddress* addr, const char* data, int length);
GekkoNetResult** mem_recv_side0(int* length);
GekkoNetResult** mem_recv_side1(int* length);
void mem_free(void* p) { std::free(p); }

std::vector<GekkoNetResult*> g_results[2];

void mem_send_to(int side, GekkoNetAddress* addr, const char* data, int length)
{
	if (g_pipe == nullptr || addr == nullptr || data == nullptr || length <= 0) return;
	if (addr->data == nullptr || addr->size != 1) return;
	const int dest = ((const uint8_t*)addr->data)[0];
	if (dest != 0 && dest != 1) return;
	if ((size_t)length > 4096) return; // bounded before use
	g_pipe->inbound[dest].emplace_back(data, data + length);
	(void)side;
}

void mem_send_side0(GekkoNetAddress* addr, const char* data, int length) { mem_send_to(0, addr, data, length); }
void mem_send_side1(GekkoNetAddress* addr, const char* data, int length) { mem_send_to(1, addr, data, length); }

GekkoNetResult** mem_recv_side(int side, int* length)
{
	g_results[side].clear();
	if (length == nullptr || g_pipe == nullptr) {
		if (length != nullptr) *length = 0;
		return nullptr;
	}
	for (std::vector<uint8_t>& raw : g_pipe->inbound[side]) {
		if (raw.size() > 4096) continue; // bounded before use
		GekkoNetResult* res = (GekkoNetResult*)std::malloc(sizeof(GekkoNetResult));
		uint8_t* addrBuf    = (uint8_t*)std::malloc(1);
		void* pay           = raw.empty() ? nullptr : std::malloc(raw.size());
		if (res == nullptr || addrBuf == nullptr || (!raw.empty() && pay == nullptr)) {
			std::free(res);
			std::free(addrBuf);
			std::free(pay);
			continue;
		}
		addrBuf[0]     = (uint8_t)(side == 0 ? 1 : 0); // sender id (unused by routing)
		if (!raw.empty()) std::memcpy(pay, raw.data(), raw.size());
		res->addr.data = addrBuf;
		res->addr.size = 1;
		res->data_len  = (unsigned)raw.size();
		res->data      = pay;
		g_results[side].push_back(res);
	}
	g_pipe->inbound[side].clear();
	*length = (int)g_results[side].size();
	return g_results[side].empty() ? nullptr : g_results[side].data();
}

GekkoNetResult** mem_recv_side0(int* length) { return mem_recv_side(0, length); }
GekkoNetResult** mem_recv_side1(int* length) { return mem_recv_side(1, length); }

// ---- toy deterministic state ----
// state = state * 0x100000001B3 ^ mix(frame, inputs); checksum = fold(state)

uint64_t toy_advance(uint64_t state, int frame, const uint8_t inputs[32])
{
	uint64_t mix = (uint64_t)(uint32_t)frame | ((uint64_t)(uint32_t)frame << 32);
	for (int i = 0; i < 32; ++i) mix = mix * 0x100000001B3ULL ^ inputs[i];
	return state * 0x100000001B3ULL ^ mix;
}

uint32_t toy_checksum(uint64_t state) { return (uint32_t)(state ^ (state >> 32)); }

void set_test_env(const char* name, const char* value)
{
#ifdef _WIN32
	_putenv((std::string(name) + "=" + value).c_str());
#else
	setenv(name, value, 1);
#endif
}

void clear_test_env(const char* name)
{
#ifdef _WIN32
	_putenv((std::string(name) + "=").c_str());
#else
	unsetenv(name);
#endif
}

} // namespace

int main()
{
	// 1. Input record round trip.
	{
		PcNetplayInput in;
		in.buttons    = 0xABCD;
		in.stickX     = -100;
		in.stickY     = 96;
		in.substickX  = -128;
		in.substickY  = 127;
		in.triggerL   = 255;
		in.triggerR   = 1;
		in.controlYaw = 0x1234;
		in.ext        = 0x0A;
		in.cursorRotate = -2;
		uint8_t wire[16];
		CHECK(pc_netplay_input_encode(in, wire) == 16, "encode 16 bytes");
		PcNetplayInput out;
		CHECK(pc_netplay_input_decode(wire, 16, out), "decode ok");
		CHECK(out.buttons == 0xABCD && out.stickX == -100 && out.stickY == 96
		          && out.substickX == -128 && out.substickY == 127 && out.triggerL == 255
		          && out.triggerR == 1 && out.controlYaw == 0x1234 && out.ext == 0x0A && out.cursorRotate == -2,
		      "round trip fields");
		CHECK(!pc_netplay_input_decode(wire, 15, out), "short decode fails");
		PcNetplayInput n = pc_netplay_input_neutral();
		uint8_t wn[16];
		pc_netplay_input_encode(n, wn);
		bool allZero = true;
		for (int i = 0; i < 16; ++i) allZero = allZero && wn[i] == 0;
		CHECK(allZero, "neutral is zeros");
	}
	// 2. Endpoint parsing.
	{
		uint32_t ip = 0;
		uint16_t port = 0;
		CHECK(pc_netplay_transport::parse_endpoint("127.0.0.1:5077", &ip, &port) && ip == 0x7F000001
		          && port == 5077,
		      "loopback endpoint");
		CHECK(!pc_netplay_transport::parse_endpoint("127.0.0.1", &ip, &port), "missing port");
		CHECK(!pc_netplay_transport::parse_endpoint("127.0.0.1:0", &ip, &port), "port 0");
		CHECK(!pc_netplay_transport::parse_endpoint("999.0.0.1:5", &ip, &port), "bad octet");
		CHECK(!pc_netplay_transport::parse_endpoint("127.0.0.1:5x", &ip, &port), "trailing junk");
		CHECK(!pc_netplay_transport::parse_endpoint("127.0.0.1 :5077", &ip, &port), "space junk");
	}
	// 2b. Input accumulator: button and ext bits OR
	// across turns between submits, sticks/triggers/yaw keep the latest
	// sample, the cursor rotate sums, take() clears the latches.
	{
		PcNetplayAccum ac;
		PcNetplayInput t0 = ac.take();
		CHECK(t0.buttons == 0, "accum starts neutral");
		// Tap A on turn 1, tap B + move stick on turn 2, submit once: the
		// short tap must survive.
		ac.add(0x0001, 0, 0, 0, 0, 0, 0, 100);
		ac.add(0x0002, 50, -60, 7, -8, 9, 10, 200);
		PcNetplayInput m = ac.take();
		CHECK(m.buttons == 0x0003, "accum ORs buttons across turns");
		CHECK(m.stickX == 50 && m.stickY == -60 && m.substickX == 7 && m.substickY == -8
		          && m.triggerL == 9 && m.triggerR == 10,
		      "accum keeps latest sticks/triggers");
		CHECK(m.controlYaw == 200, "accum keeps latest yaw");
		CHECK(m.ext == 0 && m.cursorRotate == 0, "accum ext zero");
		ac.add_ext(0x01, 1000);
		ac.add_ext(0x08, -300);
		PcNetplayInput e = ac.take();
		CHECK(e.ext == 0x09 && e.cursorRotate == 700, "accum ORs ext and sums the rotate");
		ac.add_ext(0, 30000);
		ac.add_ext(0, 30000);
		CHECK(ac.take().cursorRotate == 32767, "accum rotate saturates");
		uint8_t wire[16];
		PcNetplayInput w;
		w.ext          = 0x0F;
		w.cursorRotate = -12345;
		pc_netplay_input_encode(w, wire);
		PcNetplayInput wd;
		CHECK(pc_netplay_input_decode(wire, 16, wd) && wd.ext == 0x0F && wd.cursorRotate == -12345
		          && wire[13] == 0 && wire[14] == 0 && wire[15] == 0,
		      "ext and rotate round-trip");
		PcNetplayInput t1 = ac.take();
		CHECK(t1.buttons == 0 && t1.ext == 0 && t1.cursorRotate == 0, "take clears the button and ext latches");
		CHECK(t1.stickX == 50 && t1.controlYaw == 200, "sticks/yaw stay at latest");
		ac.reset();
		PcNetplayInput t2 = ac.take();
		CHECK(t2.buttons == 0 && t2.stickX == 0 && t2.controlYaw == 0, "reset clears all");
	}
	// 2c. Handshake-loss drop hook: the first
	// N handshake-channel sends are dropped (reported as sent, so the
	// session must recover through its Hello/Ack resends); gekko traffic
	// is unaffected and delivery resumes after N.
	{
		using namespace pc_netplay_transport;
		set_test_env("PIKMIN_NETPLAY_TEST_DROP_HS_FIRST_N", "2");
		UdpSocket a, b;
		CHECK(a.bind(0) && b.bind(0), "loopback bind");
		CHECK(a.set_peer(0x7F000001, b.local_port()), "a peer");
		CHECK(b.set_peer(0x7F000001, a.local_port()), "b peer");
		const uint8_t hello[4] = { 'N', 'P', 'H', '3' };
		CHECK(a.send_payload(kChannelHandshake, hello, sizeof(hello)), "a hs send 1");
		CHECK(a.send_payload(kChannelHandshake, hello, sizeof(hello)), "a hs send 2");
		CHECK(a.send_payload(kChannelHandshake, hello, sizeof(hello)), "a hs send 3");
		CHECK(a.send_payload(kChannelGekko, hello, sizeof(hello)), "a gekko send");
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		std::vector<UdpSocket::Datagram> got = b.recv();
		int hs = 0, gekko = 0;
		for (size_t i = 0; i < got.size(); ++i) {
			if (got[i].channel == kChannelHandshake) {
				++hs;
				CHECK(got[i].payload.size() == sizeof(hello)
				          && memcmp(got[i].payload.data(), hello, sizeof(hello)) == 0,
				      "hs payload intact");
			} else if (got[i].channel == kChannelGekko) {
				++gekko;
			}
		}
		CHECK(hs == 1, "first 2 handshake sends dropped, 3rd delivered");
		CHECK(gekko == 1, "gekko channel unaffected by hs drop");
		clear_test_env("PIKMIN_NETPLAY_TEST_DROP_HS_FIRST_N");
		// A fresh socket with no env set drops nothing.
		UdpSocket c;
		CHECK(c.bind(0), "c bind");
		CHECK(c.set_peer(0x7F000001, b.local_port()), "c peer");
		CHECK(c.send_payload(kChannelHandshake, hello, sizeof(hello)), "c hs send");
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		got = b.recv();
		hs  = 0;
		for (size_t i = 0; i < got.size(); ++i) {
			if (got[i].channel == kChannelHandshake) ++hs;
		}
		CHECK(hs == 1, "no drop without env");
	}

	// 3. Two sessions, window 0, lossy in-memory link, 2000 frames.
	MemPipe pipe;
	g_pipe = &pipe;
	pipe.adapter[0].send_data    = mem_send_side0;
	pipe.adapter[0].receive_data = mem_recv_side0;
	pipe.adapter[0].free_data    = mem_free;
	pipe.adapter[1].send_data    = mem_send_side1;
	pipe.adapter[1].receive_data = mem_recv_side1;
	pipe.adapter[1].free_data    = mem_free;

	pc_netplay_transport::LossyParams lp;
	lp.latencyMs = 2.0;
	lp.jitterMs  = 1.0;
	lp.lossPct   = 2.0;
	lp.seed      = 1234;
	pc_netplay_transport::LossyLink lossy0(&pipe.adapter[0], lp);
	lp.seed = 987;
	pc_netplay_transport::LossyLink lossy1(&pipe.adapter[1], lp);

	GekkoSession* s[2] = { nullptr, nullptr };
	CHECK(gekko_create(&s[0], GekkoGameSession) && s[0] != nullptr, "create s0");
	CHECK(gekko_create(&s[1], GekkoGameSession) && s[1] != nullptr, "create s1");
	GekkoConfig cfg;
	std::memset(&cfg, 0, sizeof(cfg));
	cfg.num_players             = 2;
	cfg.max_spectators          = 0;
	cfg.input_prediction_window = 0;
	cfg.input_size              = 16;
	cfg.state_size              = 8;
	cfg.limited_saving          = false;
	cfg.desync_detection        = true;
	cfg.check_distance          = 7;
	gekko_start(s[0], &cfg);
	gekko_start(s[1], &cfg);
	// NOTE: the adapter must be set AFTER gekko_start: Init() clears it.
	gekko_net_adapter_set(s[0], lossy0.adapter());
	gekko_net_adapter_set(s[1], lossy1.adapter());
	// Role-ordered actors, mirroring pc_netplay_session: peer 0 is host/P1
	// (local handle 0), peer 1 is joiner/P2 (local handle 1).
	uint8_t blobTo1[1] = { 1 };
	uint8_t blobTo0[1] = { 0 };
	GekkoNetAddress ra1 = { blobTo1, 1 };
	GekkoNetAddress ra0 = { blobTo0, 1 };
	const int h0 = gekko_add_actor(s[0], GekkoLocalPlayer, nullptr);
	CHECK(h0 == 0, "s0 local handle 0");
	CHECK(gekko_add_actor(s[0], GekkoRemotePlayer, &ra1) == 1, "s0 remote handle 1");
	CHECK(gekko_add_actor(s[1], GekkoRemotePlayer, &ra0) == 0, "s1 remote handle 0");
	const int h1 = gekko_add_actor(s[1], GekkoLocalPlayer, nullptr);
	CHECK(h1 == 1, "s1 local handle 1");
	gekko_set_local_delay(s[0], h0, 2);
	gekko_set_local_delay(s[1], h1, 2);

	const int kFrames = 2000;
	uint64_t state[2] = { 0x12345678abcdefULL, 0x12345678abcdefULL };
	std::vector<uint32_t> sums[2];
	int adv[2]      = { 0, 0 };
	int desyncs     = 0;
	int fed[2]      = { 0, 0 };
	bool started[2] = { false, false };
	// Input content is a pure function of the frame index: peer 0 feeds
	// f0(F), peer 1 feeds f1(F), so both sessions apply identical inputs
	// per frame. Feeding is strictly 1:1 with advances (at most one
	// Advance per update): AddLocalInput stores at the session's current
	// frame, so feeding ahead would overwrite one frame's input with the
	// next frame's content.
	auto build_input = [](int peer, int frame, uint8_t w[16]) {
		// Inputs change every frame (worst case for prediction; at
		// window 0 there is no prediction, only delay).
		PcNetplayInput in;
		if (peer == 0) {
			in.buttons    = (uint16_t)(0x100 | (frame & 0xF));
			in.stickX     = (int8_t)((frame * 37) & 0xFF);
			in.stickY     = (int8_t)((frame * 91 + 7) & 0xFF);
			in.controlYaw = (uint16_t)((frame * 257) & 0xFFFF);
		} else {
			in.buttons    = (uint16_t)(0x200 | ((frame * 3) & 0xF));
			in.stickX     = (int8_t)((frame * 53 + 11) & 0xFF);
			in.substickY  = (int8_t)((frame * 29 + 5) & 0xFF);
			in.triggerR   = (uint8_t)(frame & 0xFF);
			in.controlYaw = (uint16_t)((frame * 511 + 13) & 0xFFFF);
		}
		pc_netplay_input_encode(in, w);
	};
	const double deadline = 120.0; // wall-clock seconds
	const double t0 = (double)clock() / CLOCKS_PER_SEC;
	auto elapsed = [&]() { return (double)clock() / CLOCKS_PER_SEC - t0; };
	double lastReport = 0;
	while ((adv[0] < kFrames || adv[1] < kFrames) && elapsed() < deadline) {
		if (elapsed() - lastReport > 5.0) {
			lastReport = elapsed();
			std::printf("progress t=%.0f adv=%d/%d fed=%d/%d started=%d/%d\n", elapsed(),
			             adv[0], adv[1], fed[0], fed[1], (int)started[0],
			             (int)started[1]);
			std::fflush(stdout);
		}
		for (int p = 0; p < 2; ++p) {
			// B3: once a peer reaches kFrames, stop updating it. With
			// local delay 2 the buffer holds kFrames + 2 frames of input,
			// so a trailing update would advance it to 2001/2002 and the
			// exact-count checks below would flake (about half the runs).
			if (adv[p] >= kFrames) continue;
			// Feed the current frame's input exactly once: after N
			// advances the session's current frame is N.
			if (started[p] && fed[p] == adv[p] && fed[p] < kFrames) {
				uint8_t w[16];
				build_input(p, fed[p], w);
				gekko_add_local_input(s[p], p == 0 ? h0 : h1, w);
				++fed[p];
			}
			int n             = 0;
			GekkoGameEvent** ev = gekko_update_session(s[p], &n);
			for (int i = 0; ev != nullptr && i < n; ++i) {
				if (ev[i] == nullptr) continue;
				if (ev[i]->type == GekkoAdvanceEvent) {
					CHECK(ev[i]->data.adv.input_len == 32, "advance 32B");
					state[p] = toy_advance(state[p], ev[i]->data.adv.frame, ev[i]->data.adv.inputs);
					++adv[p];
				} else if (ev[i]->type == GekkoSaveEvent) {
					if (ev[i]->data.save.checksum != nullptr)
						*ev[i]->data.save.checksum = toy_checksum(state[p]);
					if (ev[i]->data.save.state != nullptr && ev[i]->data.save.state_len != nullptr
					    && *ev[i]->data.save.state_len >= 8) {
						uint64_t f = (uint64_t)ev[i]->data.save.frame;
						for (int b = 0; b < 8; ++b)
							ev[i]->data.save.state[b] = (uint8_t)((f >> (b * 8)) & 0xFF);
						*ev[i]->data.save.state_len = 8;
					}
					if (p == 0) sums[0].push_back(toy_checksum(state[p]));
					else sums[1].push_back(toy_checksum(state[p]));
				} else if (ev[i]->type == GekkoLoadEvent) {
					CHECK(false, "no loads at window 0");
				}
			}
			int m                  = 0;
			GekkoSessionEvent** sev = gekko_session_events(s[p], &m);
			for (int i = 0; sev != nullptr && i < m; ++i) {
				if (sev[i] == nullptr) continue;
				if (sev[i]->type == GekkoDesyncDetected) ++desyncs;
				if (sev[i]->type == GekkoSessionStarted) started[p] = true;
			}
		}
	}
	CHECK(started[0] && started[1], "both sessions started");
	CHECK(adv[0] == kFrames && adv[1] == kFrames, "both peers advanced 2000 frames");
	CHECK(state[0] == state[1], "final toy states identical");
	CHECK(desyncs == 0, "0 desyncs");
	CHECK(sums[0].size() == sums[1].size() && !sums[0].empty(), "both peers saved checksums");
	if (sums[0].size() == sums[1].size() && !sums[0].empty()) {
		bool same = true;
		for (size_t i = 0; i < sums[0].size(); ++i) {
			if (sums[0][i] != sums[1][i]) {
				same = false;
				break;
			}
		}
		CHECK(same, "per-frame checksums identical");
	}
	gekko_destroy(&s[0]);
	gekko_destroy(&s[1]);
	g_pipe = nullptr;

	if (sFailures == 0) std::printf("pc_netplay_transport_test: PASS (2000 frames, 0 desyncs)\n");
	else std::printf("pc_netplay_transport_test: %d FAILURES\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
