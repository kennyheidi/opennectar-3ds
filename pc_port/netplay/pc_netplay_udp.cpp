// Netplay UDP transport and lossy wrapper. See the header for the wire
// contract. Engine-free: Winsock + gekkonet.h + STL only.

#include "netplay/pc_netplay_udp.h"

#include "gekkonet.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pc_netplay_transport {
namespace {

#ifdef _WIN32
typedef SOCKET SocketHandle;
const SocketHandle kBadSock = INVALID_SOCKET;
#else
typedef int SocketHandle;
const SocketHandle kBadSock = -1;
#endif

void net_init_once()
{
#ifdef _WIN32
	static bool done = false;
	if (!done) {
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) done = true;
	}
#else
	(void)0;
#endif
}

void sock_close(SocketHandle s)
{
#ifdef _WIN32
	closesocket(s);
#else
	::close(s);
#endif
}

bool sock_nonblock(SocketHandle s)
{
#ifdef _WIN32
	u_long mode = 1;
	return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
	int flags = fcntl(s, F_GETFL, 0);
	if (flags < 0) return false;
	return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

} // namespace

bool parse_endpoint(const char* text, uint32_t* outIpHostOrder, uint16_t* outPort)
{
	if (text == nullptr || outIpHostOrder == nullptr || outPort == nullptr) return false;
	std::string s(text);
	size_t colon = s.rfind(':');
	if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) return false;
	std::string ip = s.substr(0, colon);
	std::string port = s.substr(colon + 1);
	if (port.empty() || port.size() > 5) return false;
	for (char c : port) {
		if (c < '0' || c > '9') return false;
	}
	// No whitespace anywhere (sscanf %u would silently skip it).
	for (char c : ip) {
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return false;
	}
	long p = strtol(port.c_str(), nullptr, 10);
	if (p <= 0 || p > 65535) return false;
	unsigned a = 0, b = 0, c = 0, d = 0;
	char tail[8] = { 0 };
	// One scan: exactly four octets, nothing trailing.
	if (sscanf(ip.c_str(), "%u.%u.%u.%u%7s", &a, &b, &c, &d, tail) != 4) return false;
	if (a > 255 || b > 255 || c > 255 || d > 255) return false;
	*outIpHostOrder = (a << 24) | (b << 16) | (c << 8) | d;
	*outPort        = (uint16_t)p;
	return true;
}

UdpSocket::UdpSocket() { net_init_once(); }

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::bind(uint16_t port)
{
	close();
	net_init_once();
	SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == kBadSock) return false;
	if (!sock_nonblock(s)) {
		sock_close(s);
		return false;
	}
	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	// Loopback unless PIKMIN_NETPLAY_UDP_BIND names another address: a
	// loopback-only socket never raises a firewall prompt, and a LAN game
	// opts in with 0.0.0.0 (or one adapter's address).
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (const char* b = getenv("PIKMIN_NETPLAY_UDP_BIND")) {
		in_addr a;
		if (*b && inet_pton(AF_INET, b, &a) == 1) {
			addr.sin_addr = a;
		} else if (*b) {
			sock_close(s);
			return false;
		}
	}
	addr.sin_port        = htons(port);
	if (::bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
		sock_close(s);
		return false;
	}
	if (port == 0) {
		sockaddr_in bound;
		memset(&bound, 0, sizeof(bound));
		socklen_t len = sizeof(bound);
		if (getsockname(s, (sockaddr*)&bound, &len) == 0) port = ntohs(bound.sin_port);
	}
	mSock      = (intptr_t)s;
	mLocalPort = port;
	return true;
}

bool UdpSocket::set_peer(uint32_t ipHostOrder, uint16_t port)
{
	if (port == 0) return false;
	mPeerIp   = ipHostOrder;
	mPeerPort = port;
	mHasPeer  = true;
	return true;
}

bool UdpSocket::send_to(uint8_t channel, const uint8_t* data, size_t len, uint32_t ipHostOrder,
                        uint16_t port)
{
	if (mSock == -1 || port == 0) return false;
	if (len + 1 > kMaxDatagram) return false; // bounded before use
	if (len > 0 && data == nullptr) return false;
	// Handshake-loss test hook. Drops the first N handshake-channel sends (N from
	// PIKMIN_NETPLAY_TEST_DROP_HS_FIRST_N), reporting success so the
	// session must recover through its resends. With N >= 2 the very
	// first Hello+Ack burst is gone (the lost final Ack case).
	if (channel == kChannelHandshake) {
		if (!mHsDropInit) {
			mHsDropInit = true;
			if (const char* e = getenv("PIKMIN_NETPLAY_TEST_DROP_HS_FIRST_N")) {
				char* end       = nullptr;
				unsigned long n = strtoul(e, &end, 10);
				if (end != e && *end == '\0') mHsDropFirstN = (unsigned)n;
			}
		}
		if (mHsSends < mHsDropFirstN) {
			++mHsSends;
			printf("[netplay] test: dropped handshake datagram %u/%u\n", mHsSends,
			       mHsDropFirstN);
			fflush(stdout);
			return true;
		}
		++mHsSends;
	}
	uint8_t frame[kMaxDatagram];
	frame[0] = channel;
	if (len > 0) memcpy(frame + 1, data, len);
	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(ipHostOrder);
	addr.sin_port        = htons(port);
	int sent = sendto((SocketHandle)mSock, (const char*)frame, (int)(len + 1), 0, (sockaddr*)&addr,
	                  sizeof(addr));
	return sent == (int)(len + 1);
}

bool UdpSocket::send_payload(uint8_t channel, const uint8_t* data, size_t len)
{
	if (!mHasPeer) return false;
	return send_to(channel, data, len, mPeerIp, mPeerPort);
}

std::vector<UdpSocket::Datagram> UdpSocket::recv()
{
	std::vector<UdpSocket::Datagram> out;
	if (mSock == -1) return out;
	// Handshake-channel impairment shares the gekko test knobs
	// (LATENCY/JITTER/LOSS_MS/PCT + SEED), applied receive-side so the
	// one-way delay model matches LossyLink and DELAY=auto measures it.
	if (!mHsImpInit) {
		mHsImpInit = true;
		double lat = 0.0, jit = 0.0, loss = 0.0;
		uint32_t seed = 0;
		if (const char* e = getenv("PIKMIN_NETPLAY_TEST_LATENCY_MS")) {
			char* end = nullptr;
			double v  = strtod(e, &end);
			if (end != e && *end == '\0' && v > 0) lat = v;
		}
		if (const char* e = getenv("PIKMIN_NETPLAY_TEST_JITTER_MS")) {
			char* end = nullptr;
			double v  = strtod(e, &end);
			if (end != e && *end == '\0' && v > 0) jit = v;
		}
		if (const char* e = getenv("PIKMIN_NETPLAY_TEST_LOSS_PCT")) {
			char* end = nullptr;
			double v  = strtod(e, &end);
			if (end != e && *end == '\0' && v > 0) loss = v;
		}
		if (const char* e = getenv("PIKMIN_NETPLAY_TEST_SEED")) {
			char* end       = nullptr;
			unsigned long n = strtoul(e, &end, 10);
			if (end != e && *end == '\0') seed = (uint32_t)n;
		}
		if (loss < 0) loss = 0;
		if (loss > 100) loss = 100;
		if (lat < 0) lat = 0;
		if (jit < 0) jit = 0;
		mHsLatMs   = lat;
		mHsJitMs   = jit;
		mHsLossPct = loss;
		// Local stream: same seed env as the gekko wrapper, mixed with the
		// local port so the two peers do not draw identical sequences.
		uint64_t s = (uint64_t)seed + 0x9E3779B97F4A7C15ULL + (uint64_t)mLocalPort * 0xBF58476D1CE4E5B9ULL;
		if (s == 0) s = 0x853C49E6748FEA9BULL;
		mHsRng = s;
	}
	for (int i = 0; i < kMaxRecvPerPoll; ++i) {
		uint8_t frame[kMaxDatagram];
		sockaddr_in from;
		memset(&from, 0, sizeof(from));
		socklen_t fromLen = sizeof(from);
		int got = recvfrom((SocketHandle)mSock, (char*)frame, (int)sizeof(frame), 0,
		                   (sockaddr*)&from, &fromLen);
		if (got <= 0) break; // none available (non-blocking) or error: stop
		if (got < 2) continue; // channel + at least one payload byte
		// Stamp each datagram's arrival time
		// individually, not the poll-start time for the whole batch, so an
		// impaired handshake hop carries one delay, not delay + poll phase.
		const double arrival = hs_now_ms();
		UdpSocket::Datagram d;
		d.channel         = frame[0];
		d.payload.assign(frame + 1, frame + got);
		d.fromIpHostOrder = ntohl(from.sin_addr.s_addr);
		d.fromPort        = ntohs(from.sin_port);
		if (d.channel != kChannelHandshake) {
			out.push_back(std::move(d));
			continue;
		}
		// Handshake channel: loss then delay.
		if (hs_draw_drop()) continue;
		double delay = mHsLatMs;
		if (mHsJitMs > 0.0) delay += hs_draw_uniform(0.0, mHsJitMs);
		if (delay <= 0.0) {
			out.push_back(std::move(d));
		} else {
			HsDelayed hd;
			hd.deliverAtMs = arrival + delay;
			hd.gram        = std::move(d);
			mHsDelayed.push_back(std::move(hd));
			if (mHsDelayed.size() > 512)
				mHsDelayed.erase(mHsDelayed.begin(),
				                 mHsDelayed.begin() + (mHsDelayed.size() - 512));
		}
	}
	if (!mHsDelayed.empty()) {
		std::stable_sort(mHsDelayed.begin(), mHsDelayed.end(),
		                  [](const HsDelayed& a, const HsDelayed& b) {
			                  return a.deliverAtMs < b.deliverAtMs;
		                  });
		size_t due = 0;
		while (due < mHsDelayed.size() && mHsDelayed[due].deliverAtMs <= hs_now_ms()) ++due;
		for (size_t i = 0; i < due; ++i) out.push_back(std::move(mHsDelayed[i].gram));
		if (due > 0) mHsDelayed.erase(mHsDelayed.begin(), mHsDelayed.begin() + due);
	}
	return out;
}

void UdpSocket::close()
{
	if (mSock != -1) {
		sock_close((SocketHandle)mSock);
		mSock = -1;
	}
	mLocalPort = 0;
	mHasPeer   = false;
}

double UdpSocket::hs_now_ms() const
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static uint64_t hs_xorshift64(uint64_t& s)
{
	// xorshift64*: self-contained so the header stays engine-free without
	// <random>; only the handshake test impairment uses it, never the sim.
	uint64_t x = s;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	s = x;
	return x * 0x2545F4914F6CDD1DULL;
}

double UdpSocket::hs_draw_uniform(double lo, double hi)
{
	if (!(hi > lo)) return lo;
	uint64_t r = hs_xorshift64(mHsRng);
	// 53-bit mantissa uniform in [0,1).
	double u = (double)(r >> 11) * (1.0 / 9007199254740992.0);
	return lo + u * (hi - lo);
}

bool UdpSocket::hs_draw_drop()
{
	if (mHsLossPct <= 0.0) return false;
	if (mHsLossPct >= 100.0) return true;
	return hs_draw_uniform(0.0, 100.0) < mHsLossPct;
}

// ---- GekkoLink ----
//
// The C API carries no user context, so each trampoline forwards to a
// file-static slot. The game runs a single session per process, which is all the
// slot supports; the constructor installs it and the destructor clears it.

namespace {
GekkoLink* g_send_link = nullptr;
GekkoLink* g_recv_link = nullptr;

void gekko_send_trampoline(GekkoNetAddress* addr, const char* data, int length)
{
	if (g_send_link == nullptr) return;
	// Real path: decode the 6-byte address blob owned by the session.
	if (addr == nullptr || data == nullptr || length <= 0) return;
	if (addr->data == nullptr || addr->size != kAddrBytes) return;
	const uint8_t* b = (const uint8_t*)addr->data;
	uint32_t ip      = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8)
	              | (uint32_t)b[3];
	uint16_t port    = (uint16_t)(((uint16_t)b[4] << 8) | b[5]);
	g_send_link->send_to_peer(ip, port, (const uint8_t*)data, (size_t)length);
}

GekkoNetResult** gekko_receive_trampoline(int* length)
{
	if (length == nullptr || g_recv_link == nullptr) {
		if (length != nullptr) *length = 0;
		return nullptr;
	}
	return g_recv_link->receive_inner(length);
}

void gekko_free_trampoline(void* data_ptr) { free(data_ptr); }
} // namespace

GekkoLink::GekkoLink(UdpSocket* sock) : mSock(sock)
{
	mAdapter.send_data    = gekko_send_trampoline;
	mAdapter.receive_data = gekko_receive_trampoline;
	mAdapter.free_data    = gekko_free_trampoline;
	g_send_link           = this;
	g_recv_link           = this;
}

GekkoLink::~GekkoLink()
{
	if (g_send_link == this) g_send_link = nullptr;
	if (g_recv_link == this) g_recv_link = nullptr;
}

GekkoNetAdapter* GekkoLink::adapter() { return &mAdapter; }

void GekkoLink::send_to_peer(uint32_t ipHostOrder, uint16_t port, const uint8_t* data, size_t len)
{
	if (mSock == nullptr) return;
	mSock->send_to(kChannelGekko, data, len, ipHostOrder, port);
}

void GekkoLink::send_inner(uint32_t ipHostOrder, uint16_t port, const uint8_t* data, size_t len)
{
	// Compatibility entry point for the header declaration; the live path
	// goes through send_to_peer() from the trampoline above.
	send_to_peer(ipHostOrder, port, data, len);
}

// Both pending queues are capped (brief: every declared length is
// bounded). A stray sender can otherwise grow them without bound; beyond
// the cap the oldest datagram is dropped.
constexpr size_t kMaxPendingQueue = 512;

void cap_queue(std::vector<UdpSocket::Datagram>& q)
{
	if (q.size() > kMaxPendingQueue) q.erase(q.begin(), q.begin() + (q.size() - kMaxPendingQueue));
}

void route_gram(UdpSocket::Datagram& g, std::vector<UdpSocket::Datagram>& gekko,
                std::vector<UdpSocket::Datagram>& hs)
{
	if (g.channel == kChannelGekko) {
		gekko.push_back(std::move(g));
	} else if (g.channel == kChannelHandshake) {
		hs.push_back(std::move(g));
	}
	// Unknown channels are dropped.
}

GekkoNetResult** GekkoLink::receive_inner(int* length)
{
	mResults.clear();
	if (mSock != nullptr) {
		std::vector<UdpSocket::Datagram> grams = mSock->recv();
		for (UdpSocket::Datagram& g : grams) route_gram(g, mGekkoPending, mHandshakePending);
		cap_queue(mGekkoPending);
		cap_queue(mHandshakePending);
	}
	auto emit = [&](const uint8_t* payload, size_t len, uint32_t ip, uint16_t port) {
		if (len > kMaxDatagram - 1) return; // bounded before use
		GekkoNetResult* res = (GekkoNetResult*)malloc(sizeof(GekkoNetResult));
		uint8_t* addrBuf    = (uint8_t*)malloc(kAddrBytes);
		void* payBuf        = len > 0 ? malloc(len) : nullptr;
		if (res == nullptr || addrBuf == nullptr || (len > 0 && payBuf == nullptr)) {
			free(res);
			free(addrBuf);
			free(payBuf);
			return;
		}
		addrBuf[0] = (uint8_t)((ip >> 24) & 0xFF);
		addrBuf[1] = (uint8_t)((ip >> 16) & 0xFF);
		addrBuf[2] = (uint8_t)((ip >> 8) & 0xFF);
		addrBuf[3] = (uint8_t)(ip & 0xFF);
		addrBuf[4] = (uint8_t)((port >> 8) & 0xFF);
		addrBuf[5] = (uint8_t)(port & 0xFF);
		if (len > 0) memcpy(payBuf, payload, len);
		res->addr.data = addrBuf;
		res->addr.size = (unsigned)kAddrBytes;
		res->data_len  = (unsigned)len;
		res->data      = payBuf;
		mResults.push_back(res);
	};
	for (UdpSocket::Datagram& g : mGekkoPending)
		emit(g.payload.data(), g.payload.size(), g.fromIpHostOrder, g.fromPort);
	mGekkoPending.clear();
	for (std::vector<uint8_t>& raw : mInjected) emit(raw.data(), raw.size(), 0x7F000001, 1);
	mInjected.clear();
	*length = (int)mResults.size();
	return mResults.empty() ? nullptr : mResults.data();
}

void GekkoLink::inject_for_test(const uint8_t* data, size_t len)
{
	if (data == nullptr || len == 0 || len > kMaxDatagram - 1) return;
	mInjected.emplace_back(data, data + len);
}

std::vector<UdpSocket::Datagram> GekkoLink::drain_handshake()
{
	// Pump once so handshake bytes arriving between drains are not stuck
	// behind an idle GekkoNet poll.
	if (mSock != nullptr) {
		std::vector<UdpSocket::Datagram> grams = mSock->recv();
		for (UdpSocket::Datagram& g : grams) route_gram(g, mGekkoPending, mHandshakePending);
		cap_queue(mGekkoPending);
		cap_queue(mHandshakePending);
	}
	std::vector<UdpSocket::Datagram> out;
	out.swap(mHandshakePending);
	return out;
}

// ---- LossyLink ----

struct LossyLink::Rng {
	std::mt19937 gen;
};

namespace {
constexpr int kLossySlots = 2;
LossyLink* g_lossy_send[kLossySlots] = { nullptr, nullptr };
LossyLink* g_lossy_recv[kLossySlots] = { nullptr, nullptr };
int g_lossy_count = 0;

void lossy_send_trampoline_0(GekkoNetAddress* addr, const char* data, int length)
{
	if (g_lossy_send[0] == nullptr) return;
	g_lossy_send[0]->send_inner(addr, data, length);
}

void lossy_send_trampoline_1(GekkoNetAddress* addr, const char* data, int length)
{
	if (g_lossy_send[1] == nullptr) return;
	g_lossy_send[1]->send_inner(addr, data, length);
}

GekkoNetResult** lossy_receive_trampoline_0(int* length)
{
	if (length == nullptr || g_lossy_recv[0] == nullptr) {
		if (length != nullptr) *length = 0;
		return nullptr;
	}
	return g_lossy_recv[0]->receive_inner(length);
}

GekkoNetResult** lossy_receive_trampoline_1(int* length)
{
	if (length == nullptr || g_lossy_recv[1] == nullptr) {
		if (length != nullptr) *length = 0;
		return nullptr;
	}
	return g_lossy_recv[1]->receive_inner(length);
}
} // namespace

double LossyLink::now_ms()
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

LossyLink::LossyLink(GekkoNetAdapter* inner, const LossyParams& params)
    : mInner(inner), mParams(params), mRng(new Rng())
{
	mRng->gen.seed(params.seed);
	// Slot 0/1 selects the trampoline pair baked into this instance's
	// adapter. Production creates one; the transport test creates two.
	mSlot = g_lossy_count < kLossySlots ? g_lossy_count++ : 0;
	if (mSlot == 0) {
		mAdapter.send_data    = lossy_send_trampoline_0;
		mAdapter.receive_data = lossy_receive_trampoline_0;
	} else {
		mAdapter.send_data    = lossy_send_trampoline_1;
		mAdapter.receive_data = lossy_receive_trampoline_1;
	}
	mAdapter.free_data = gekko_free_trampoline;
	g_lossy_send[mSlot] = this;
	g_lossy_recv[mSlot] = this;
}

LossyLink::~LossyLink()
{
	if (g_lossy_send[mSlot] == this) g_lossy_send[mSlot] = nullptr;
	if (g_lossy_recv[mSlot] == this) g_lossy_recv[mSlot] = nullptr;
	// Release the slot so a third instance in the same process does
	// not silently steal slot 0. Stack discipline (test creates two, then
	// destroys both) pops the top; anything else parks at slot 0 reuse.
	if (mSlot == g_lossy_count - 1) --g_lossy_count;
	// Anything still delayed belongs to the inner adapter's heap contract;
	// release it through the inner free triple.
	for (Delayed& d : mPending) {
		if (d.res == nullptr || mInner == nullptr) continue;
		mInner->free_data(d.res->addr.data);
		mInner->free_data(d.res->data);
		mInner->free_data(d.res);
	}
	delete mRng;
}

GekkoNetAdapter* LossyLink::adapter() { return &mAdapter; }

bool LossyLink::draw_drop()
{
	if (mParams.lossPct <= 0.0) return false;
	if (mParams.lossPct >= 100.0) return true;
	std::uniform_real_distribution<double> dist(0.0, 100.0);
	return dist(mRng->gen) < mParams.lossPct;
}

double LossyLink::draw_delay_ms()
{
	double d = mParams.latencyMs;
	if (mParams.jitterMs > 0.0) {
		std::uniform_real_distribution<double> jit(0.0, mParams.jitterMs);
		d += jit(mRng->gen);
	}
	if (mParams.reorderPct > 0.0 && mParams.reorderExtraMs > 0.0) {
		std::uniform_real_distribution<double> r(0.0, 100.0);
		if (r(mRng->gen) < mParams.reorderPct) d += mParams.reorderExtraMs;
	}
	return d < 0.0 ? 0.0 : d;
}

void LossyLink::send_inner(GekkoNetAddress* addr, const char* data, int length)
{
	// Loss applies on receive only, so the configured lossPct is the
	// effective one-way rate. Local RNG only, never the sim RNG.
	if (mInner == nullptr || addr == nullptr || data == nullptr || length <= 0) return;
	mInner->send_data(addr, data, length);
}

GekkoNetResult** LossyLink::receive_inner(int* length)
{
	mResults.clear();
	int n                 = 0;
	GekkoNetResult** got  = mInner->receive_data(&n);
	const double now      = now_ms();
	if (got != nullptr && n > 0) {
		for (int i = 0; i < n; ++i) {
			if (got[i] == nullptr) continue;
			if (draw_drop()) {
				mInner->free_data(got[i]->addr.data);
				mInner->free_data(got[i]->data);
				mInner->free_data(got[i]);
				continue;
			}
			Delayed d;
			d.deliverAtMs = now + draw_delay_ms();
			d.res         = got[i];
			mPending.push_back(d);
		}
	}
	// Stable delivery: due packets first, preserving arrival order.
	std::stable_sort(mPending.begin(), mPending.end(),
	                 [](const Delayed& a, const Delayed& b) { return a.deliverAtMs < b.deliverAtMs; });
	for (Delayed& d : mPending) {
		if (d.deliverAtMs <= now) {
			mResults.push_back(d.res);
			d.res = nullptr;
		} else {
			break; // sorted: nothing later is due
		}
	}
	mPending.erase(std::remove_if(mPending.begin(), mPending.end(),
	                              [](const Delayed& d) { return d.res == nullptr; }),
	               mPending.end());
	*length = (int)mResults.size();
	return mResults.empty() ? nullptr : mResults.data();
}

} // namespace pc_netplay_transport
