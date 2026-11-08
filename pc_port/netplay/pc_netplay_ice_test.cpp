// Host-run ICE test: the connection codes and two libjuice agents in one
// process.
//
//   1. v1 and v2 codes round-trip; garbage, corruption, truncation and
//      oversized fields are refused with a reason; code files are atomic and
//      bounded; the env configuration parses STUN "none" and TURN lists.
//   2. The copy-paste flow (offer -> answer -> apply) reaches COMPLETED on both
//      agents. Both bind 127.0.0.1 with no STUN or TURN, so the only candidate
//      is loopback and no network is needed.
//   3. 64 KiB through the gekko channel arrives intact, a handshake-channel
//      ping-pong, and the IceLink adapter routes each channel to its queue.
// Engine-free: links juice + pc_netplay_ice only.

#include "netplay/pc_netplay_ice.h"

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

const char* kFakeSdp = "a=ice-ufrag:hostUfrag1234\r\n"
                       "a=ice-pwd:hostPasswordPassword1234567890\r\n"
                       "a=candidate:1 1 UDP 2113937151 192.168.2.61 50001 typ host\r\n"
                       "a=candidate:2 1 UDP 16777215 74.125.250.129 19302 typ srflx\r\n"
                       "a=end-of-candidates\r\n"
                       "a=ice-options:ice2\r\n";

void set_env(const char* name, const char* value)
{
#ifdef _WIN32
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

} // namespace

int main()
{
	using namespace pc_netplay_ice;

	// 0. Waits and the env configuration.
	{
		set_env("PIKMIN_NETPLAY_ICE_TIMEOUT_MS", "");
		CHECK(ice_connect_timeout_ms() == 600000.0, "connect wait defaults to 10 min");
		set_env("PIKMIN_NETPLAY_ICE_TIMEOUT_MS", "45000");
		CHECK(ice_connect_timeout_ms() == 45000.0, "the variable sets the wait");
		const char* unusable[] = { "999", "1800001", "soon", "30000ms", "-5" };
		for (const char* u : unusable) {
			set_env("PIKMIN_NETPLAY_ICE_TIMEOUT_MS", u);
			CHECK(ice_connect_timeout_ms() == 600000.0, "an unusable variable leaves the wait alone");
		}
		set_env("PIKMIN_NETPLAY_ICE_TIMEOUT_MS", "");

		set_env("PIKMIN_NETPLAY_STUN", "");
		set_env("PIKMIN_NETPLAY_TURN", "");
		IceNetConfig def = ice_net_config_from_env();
		CHECK(def.stun.empty(), "no STUN by default");
		set_env("PIKMIN_NETPLAY_STUN", "public");
		IceNetConfig pub = ice_net_config_from_env();
		CHECK(pub.stun.size() == 2 && pub.stun[0].host == "stun.l.google.com" && pub.stun[0].port == 19302
		          && pub.stun[1].host == "stun.cloudflare.com" && pub.stun[1].port == 3478,
		      "public STUN servers");
		CHECK(def.turn.empty() && def.bindAddress.empty() && !def.turnOnly, "no TURN, any interface by default");
		set_env("PIKMIN_NETPLAY_STUN", "none");
		CHECK(ice_net_config_from_env().stun.empty(), "STUN none");
		set_env("PIKMIN_NETPLAY_STUN", "stun.example.org:3478");
		set_env("PIKMIN_NETPLAY_TURN", "turn.example.org:3478:user:secret,bad-entry");
		IceNetConfig t = ice_net_config_from_env();
		CHECK(t.stun.size() == 1 && t.stun[0].host == "stun.example.org", "custom STUN");
		CHECK(t.turn.size() == 1 && t.turn[0].host == "turn.example.org" && t.turn[0].port == 3478
		          && t.turn[0].user == "user" && t.turn[0].pass == "secret",
		      "TURN parsed, malformed entry dropped");
		CHECK(ice_net_config_text(t).find("secret") == std::string::npos, "the log line never shows a TURN password");
		set_env("PIKMIN_NETPLAY_STUN", "");
		set_env("PIKMIN_NETPLAY_TURN", "");
	}

	// 1. v1 codes.
	std::string v1offer;
	{
		std::string answer, err, sdp;
		bool isOffer = false;
		CHECK(ice_encode_code(true, kFakeSdp, &v1offer, &err), "encode offer");
		CHECK(ice_encode_code(false, kFakeSdp, &answer, &err), "encode answer");
		CHECK(v1offer.compare(0, 6, "NPIX1-") == 0 && v1offer.find('\n') == std::string::npos,
		      "offer is one line with version prefix");
		CHECK(ice_decode_code(v1offer, &isOffer, &sdp, &err) && isOffer && sdp == kFakeSdp, "offer round trip");
		CHECK(ice_decode_code(answer, &isOffer, &sdp, &err) && !isOffer && sdp == kFakeSdp, "answer round trip");
		const char* bad[] = { "", "not-a-code", "NPIX1-", "NPIX1-!!!not-base64!!!", "NPIX0-AAAA" };
		for (const char* b : bad) {
			bool io = false;
			std::string s, e;
			CHECK(!ice_decode_code(b, &io, &s, &e) && !e.empty(), "garbage rejected");
		}
		std::string corrupt = v1offer;
		corrupt[12]         = (corrupt[12] == 'A' ? 'B' : 'A');
		CHECK(!ice_decode_code(corrupt, &isOffer, &sdp, &err), "corrupted code rejected");
		CHECK(!ice_decode_code(v1offer.substr(0, v1offer.size() / 2), &isOffer, &sdp, &err), "truncated code rejected");
		CHECK(ice_classify_code(" " + answer + "\r\n") == kCodeV1, "classify v1");
		CHECK(ice_classify_code("  ") == kCodeEmpty, "classify empty");
		CHECK(ice_classify_code("hello") == kCodeGarbage, "classify garbage");
	}

	// 1b. v2 offers with the session bundle.
	{
		SessionBundle bundle;
		bundle.seed       = 0x12345678;
		bundle.configText = "fpsMode=0;chainActions=1;vsMap=;";
		bundle.descriptor = "captainP1=0;captainP2=1;";
		std::string offer2, err, sdp;
		SessionBundle got;
		CHECK(ice_encode_offer_v2(kFakeSdp, bundle, &offer2, &err), "encode v2 offer");
		CHECK(offer2.compare(0, 6, "NPIX2-") == 0 && offer2.find('\n') == std::string::npos,
		      "v2 offer is one line with version prefix");
		CHECK(ice_classify_code(offer2) == kCodeOfferV2, "classify v2");
		CHECK(ice_decode_offer_v2(offer2, &sdp, &got, &err) && sdp == kFakeSdp, "v2 SDP round trip");
		CHECK(got.seed == bundle.seed && got.configText == bundle.configText && got.descriptor == bundle.descriptor,
		      "v2 bundle round trip");
		std::printf("ice_test: v2 offer chars=%llu\n", (unsigned long long)offer2.size());
		{
			SessionBundle empty = bundle;
			empty.descriptor.clear();
			std::string enc, e, s;
			SessionBundle g;
			CHECK(ice_encode_offer_v2(kFakeSdp, empty, &enc, &e), "encode v2 with an empty descriptor");
			CHECK(ice_decode_offer_v2(enc, &s, &g, &e) && g.descriptor.empty() && g.configText == bundle.configText,
			      "empty descriptor round trip");
		}
		const char* bad2[] = { "", "not-a-code", "NPIX2-", "NPIX2-!!!not-base64!!!", "NPIX1-AAAA", "NPIX2-AAAA" };
		for (const char* b : bad2) {
			std::string s, e;
			SessionBundle g;
			CHECK(!ice_decode_offer_v2(b, &s, &g, &e) && !e.empty(), "v2 garbage rejected");
		}
		std::string corrupt = offer2;
		corrupt[12]         = (corrupt[12] == 'A' ? 'B' : 'A');
		CHECK(!ice_decode_offer_v2(corrupt, &sdp, &got, &err), "v2 corrupted code rejected");
		CHECK(!ice_decode_offer_v2(offer2.substr(0, offer2.size() / 2), &sdp, &got, &err), "v2 truncated rejected");
		{
			SessionBundle huge = bundle;
			huge.configText.assign(kMaxBundleConfigBytes + 1, 'x');
			std::string enc, e;
			CHECK(!ice_encode_offer_v2(kFakeSdp, huge, &enc, &e) && !e.empty(), "oversized config refused");
			huge = bundle;
			huge.configText.clear();
			CHECK(!ice_encode_offer_v2(kFakeSdp, huge, &enc, &e), "empty config refused");
			huge = bundle;
			huge.descriptor.assign(kMaxBundleDescBytes, 'd');
			huge.configText.assign(kMaxBundleConfigBytes, 'c');
			std::string s;
			SessionBundle g;
			CHECK(ice_encode_offer_v2(kFakeSdp, huge, &enc, &e), "largest bundle encodes");
			CHECK(ice_decode_offer_v2(enc, &s, &g, &e) && g.descriptor == huge.descriptor, "largest bundle round trip");
			CHECK(enc.size() <= kMaxOfferV2Chars, "largest offer fits the text cap");
			huge.descriptor.push_back('d');
			CHECK(!ice_encode_offer_v2(kFakeSdp, huge, &enc, &e), "oversized descriptor refused");
		}
		{
			std::string tooLong = "NPIX2-" + std::string(kMaxOfferV2Chars, 'A');
			std::string s, e;
			SessionBundle g;
			CHECK(!ice_decode_offer_v2(tooLong, &s, &g, &e) && e.find("too long") != std::string::npos,
			      "v2 text over the cap refused");
		}
		{
			std::string s, e;
			SessionBundle g;
			CHECK(!ice_decode_offer_v2(v1offer, &s, &g, &e) && e.find("NPIX2-") != std::string::npos,
			      "v1 code refused by the v2 decoder");
		}
		{
			const std::string path = "pc_netplay_ice_test_code.txt";
			std::string e, back;
			CHECK(ice_write_code_file(path, offer2, &e), "atomic code-file write");
			FILE* tmp = std::fopen((path + ".tmp").c_str(), "rb");
			CHECK(tmp == nullptr, "no temporary file left behind");
			if (tmp != nullptr) std::fclose(tmp);
			CHECK(ice_read_code_arg("@" + path, &back, &e) && back == offer2, "@file reads the code back");
			FILE* f = std::fopen(path.c_str(), "wb");
			if (f != nullptr) {
				const std::string junk(kMaxOfferV2Chars + 8192, 'A');
				std::fwrite(junk.data(), 1, junk.size(), f);
				std::fclose(f);
			}
			CHECK(!ice_read_code_arg("@" + path, &back, &e) && e.find("too large") != std::string::npos,
			      "@file larger than any code refused");
			std::remove(path.c_str());
		}
	}

	// 2. Relay filter.
	{
		CHECK(ice_candidate_line_is_relay("a=candidate:3 1 UDP 41819903 10.0.0.5 50002 typ relay"), "relay line");
		CHECK(!ice_candidate_line_is_relay("a=candidate:1 1 UDP 2113937151 192.168.2.61 50001 typ host"), "host line");
		std::string filtered = ice_filter_relay_candidates(kFakeSdp);
		CHECK(filtered.find(" typ host") == std::string::npos, "host filtered out");
		CHECK(filtered.find(" typ srflx") == std::string::npos, "srflx filtered out");
		CHECK(filtered.find("a=ice-ufrag:") != std::string::npos, "ufrag kept");
		const std::string noCand = "a=ice-ufrag:x\r\na=ice-pwd:y\r\n";
		CHECK(ice_filter_relay_candidates(noCand) == noCand, "CRLF round trip");
		CHECK(!ice_sdp_has_relay(kFakeSdp), "fake SDP has no relay");
		CHECK(ice_sdp_has_relay("a=candidate:3 1 UDP 1 10.0.0.5 9 typ relay\r\n"), "relay SDP detected");
	}

	// 3. In-process loopback pair.
	IceNetConfig cfg;
	cfg.bindAddress = "127.0.0.1";
	IceSocket hostSock, joinSock;
	{
		SessionBundle bundle;
		bundle.seed       = 7;
		bundle.configText = "fpsMode=0;";
		std::string offer, answer, err;
		CHECK(hostSock.host_create_offer(cfg, bundle, &offer, &err), "host offer gathers");
		if (!err.empty()) std::printf("ice_test: %s\n", err.c_str());
		std::printf("ice_test: offer chars=%llu\n", (unsigned long long)offer.size());
		SessionBundle got;
		CHECK(joinSock.join_create_answer(cfg, offer, &answer, &err, std::function<void()>(), &got),
		      "join answer gathers");
		CHECK(got.seed == 7 && got.configText == "fpsMode=0;", "the joiner gets the bundle");
		std::printf("ice_test: answer chars=%llu\n", (unsigned long long)answer.size());
		CHECK(!hostSock.host_apply_answer(offer, &err), "an offer is not an answer");
		CHECK(hostSock.host_apply_answer(answer, &err), "host applies answer");
		double hostMs = -1, joinMs = -1;
		CHECK(hostSock.wait_connected(30000, &hostMs, &err), "host completed");
		CHECK(joinSock.wait_connected(30000, &joinMs, &err), "join completed");
		std::printf("ice_test: completed host=%.0fms join=%.0fms pair=[%s]\n", hostMs, joinMs,
		            hostSock.selected_local().c_str());
		CHECK(hostMs >= 0 && joinMs >= 0, "completed times reported");
		CHECK(hostSock.selected_local().find("127.0.0.1") != std::string::npos, "the selected pair is loopback");
	}

	// 4. 64 KiB through the gekko channel + handshake ping-pong.
	{
		const size_t kChunks = 64, kPayload = 1024;
		std::vector<uint8_t> want(kChunks * kPayload);
		for (size_t i = 0; i < want.size(); ++i) want[i] = (uint8_t)((i * 2654435761u) >> 8);
		for (size_t c = 0; c < kChunks; ++c) {
			uint8_t payload[kPayload];
			payload[0] = (uint8_t)((c >> 24) & 0xFF);
			payload[1] = (uint8_t)((c >> 16) & 0xFF);
			payload[2] = (uint8_t)((c >> 8) & 0xFF);
			payload[3] = (uint8_t)(c & 0xFF);
			for (size_t i = 4; i < kPayload; ++i) payload[i] = want[c * kPayload + i];
			CHECK(hostSock.send_payload(kChannelGekko, payload, kPayload), "gekko send");
		}
		std::vector<char> seen(kChunks, 0);
		size_t gotChunks = 0;
		bool intact      = true;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		while (gotChunks < kChunks && std::chrono::steady_clock::now() < deadline) {
			std::vector<IceSocket::Datagram> grams = joinSock.recv();
			if (grams.empty()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				continue;
			}
			for (const IceSocket::Datagram& g : grams) {
				CHECK(g.channel == kChannelGekko, "gekko channel intact");
				if (g.payload.size() != kPayload) continue;
				const size_t c = ((size_t)g.payload[0] << 24) | ((size_t)g.payload[1] << 16)
				               | ((size_t)g.payload[2] << 8) | g.payload[3];
				if (c >= kChunks || seen[c]) continue;
				seen[c] = 1;
				++gotChunks;
				if (memcmp(g.payload.data() + 4, want.data() + c * kPayload + 4, kPayload - 4) != 0) intact = false;
			}
		}
		CHECK(gotChunks == kChunks, "all 64 chunks arrived");
		CHECK(intact, "64 KiB intact");
		CHECK(!hostSock.send_payload(kChannelGekko, want.data(), kMaxDatagram), "oversized datagram refused");
		const uint8_t ping[5] = { 'p', 'i', 'n', 'g', '!' };
		CHECK(joinSock.send_payload(kChannelHandshake, ping, sizeof(ping)), "hs send");
		bool pong = false;
		const auto d2 = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!pong && std::chrono::steady_clock::now() < d2) {
			for (const IceSocket::Datagram& g : hostSock.recv())
				if (g.channel == kChannelHandshake && g.payload.size() == 5 && memcmp(g.payload.data(), ping, 5) == 0)
					pong = true;
			if (!pong) std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		CHECK(pong, "handshake ping-pong");
	}

	// 5. IceLink: gekko datagrams reach the adapter, handshake datagrams
	// drain_handshake, an unknown channel is dropped.
	{
		IceLink joinLink(&joinSock);
		const uint8_t gk[6]   = { 'g', 'e', 'k', 'k', 'o', '!' };
		const uint8_t hs[4]   = { 'h', 's', '0', '1' };
		const uint8_t junk[3] = { 'x', 'y', 'z' };
		CHECK(hostSock.send_payload(kChannelGekko, gk, sizeof(gk)), "icelink gekko send");
		CHECK(hostSock.send_payload(kChannelHandshake, hs, sizeof(hs)), "icelink hs send");
		CHECK(hostSock.send_payload(0x09, junk, sizeof(junk)), "unknown-channel send");
		bool gotGk = false, gotHs = false, gotJunk = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while ((!gotGk || !gotHs) && std::chrono::steady_clock::now() < deadline) {
			for (const IceSocket::Datagram& g : joinLink.drain_handshake()) {
				if (g.payload.size() == sizeof(hs) && memcmp(g.payload.data(), hs, sizeof(hs)) == 0) gotHs = true;
				else gotJunk = true;
			}
			int n                = 0;
			GekkoNetResult** res = joinLink.receive_inner(&n);
			for (int i = 0; i < n; ++i) {
				if (res[i] == nullptr) continue;
				const uint8_t blob[6] = { 127, 0, 0, 1, 0, 1 };
				if (res[i]->data_len == sizeof(gk) && memcmp(res[i]->data, gk, sizeof(gk)) == 0
				    && res[i]->addr.size == kAddrBytes && memcmp(res[i]->addr.data, blob, 6) == 0)
					gotGk = true;
				else gotJunk = true;
				free(res[i]->addr.data);
				free(res[i]->data);
				free(res[i]);
			}
			if (!gotGk || !gotHs) std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		CHECK(gotGk, "icelink adapter round trip with the placeholder address");
		CHECK(gotHs, "handshake datagram reaches drain_handshake");
		CHECK(!gotJunk, "no datagram on the wrong queue; unknown channel dropped");
	}

	hostSock.close();
	joinSock.close();
	if (sFailures == 0) std::printf("pc_netplay_ice_test: PASS\n");
	else std::printf("pc_netplay_ice_test: %d FAILURES\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
