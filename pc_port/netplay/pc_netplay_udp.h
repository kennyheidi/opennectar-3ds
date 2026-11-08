#pragma once
// Netplay UDP transport and lossy test wrapper.
//
// Engine-free TU (Winsock + gekkonet.h + the C++ standard library only), so
// the host-run transport test can link it without the game. All netplay code
// here compiles as C++17 and includes only `gekkonet.h` from GekkoNet, which
// is C-compatible.
//
// Wire format: every datagram carries a 1-byte channel prefix:
//   0x01  handshake (session hello/ack; consumed by pc_netplay_session)
//   0x02  gekko     (GekkoNet packets; the only channel the GekkoNetAdapter
//                   sees)
// Unknown channels are dropped. Every declared length is bounded before use
// (kMaxDatagram); oversized datagrams are dropped, never truncated into a
// smaller buffer.
//
// Address format inside GekkoNetAddress: 6 bytes, IPv4 (network order) +
// port (network order). The session passes these to gekko_add_actor; this
// adapter is the only code that interprets them.

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

// The only GekkoNet header our code includes (C-compatible, per the brief).
#include "gekkonet.h"

namespace pc_netplay_transport {
constexpr uint8_t kChannelHandshake = 0x01;
constexpr uint8_t kChannelGekko     = 0x02;
constexpr size_t kMaxDatagram      = 4096;
constexpr size_t kAddrBytes        = 6; // IPv4 + port, network order
constexpr int kMaxRecvPerPoll      = 64;

// Parses "ip:port" (IPv4 dotted quad + decimal port). Returns false on any
// malformed input; outIp/outPort are untouched then.
bool parse_endpoint(const char* text, uint32_t* outIpHostOrder, uint16_t* outPort);

// Non-blocking Winsock UDP socket. Bind to a local port (0 = ephemeral);
// setPeer directs gekko-channel sends. All channel framing is explicit:
// send_payload prepends the channel byte; recv() returns only the payload
// bytes (channel stripped) tagged with the channel and sender.
class UdpSocket {
public:
	UdpSocket();
	~UdpSocket();

	UdpSocket(const UdpSocket&)            = delete;
	UdpSocket& operator=(const UdpSocket&) = delete;

	// Binds port (0 = ephemeral) on 127.0.0.1, or on the IPv4 address in
	// PIKMIN_NETPLAY_UDP_BIND (0.0.0.0 for every interface). Returns false on
	// failure.
	bool bind(uint16_t port);
	// Local port actually bound (0 when unbound).
	uint16_t local_port() const { return mLocalPort; }
 	// Sets the default remote for send_payload().
	bool set_peer(uint32_t ipHostOrder, uint16_t port);
	// Sends channel + payload to the peer. Returns false when no peer is
	// set, the frame exceeds kMaxDatagram, or the send fails.
	bool send_payload(uint8_t channel, const uint8_t* data, size_t len);
	// Sends channel + payload to an explicit endpoint.
	bool send_to(uint8_t channel, const uint8_t* data, size_t len, uint32_t ipHostOrder,
	             uint16_t port);

	struct Datagram {
		uint8_t channel = 0;
		std::vector<uint8_t> payload;
		uint32_t fromIpHostOrder = 0;
		uint16_t fromPort        = 0;
	};
	// Pumps the socket (up to kMaxRecvPerPoll datagrams). Never blocks.
	std::vector<Datagram> recv();

	void close();

private:
	intptr_t mSock = -1;
	uint16_t mLocalPort = 0;
	uint32_t mPeerIp = 0;
	uint16_t mPeerPort = 0;
	bool mHasPeer = false;
	// Handshake-loss test hook. Drops the first N handshake-channel (0x01)
	// sends, reporting success to the caller so
	// the peer must recover via its Hello/Ack resends. N comes from
	// PIKMIN_NETPLAY_TEST_DROP_HS_FIRST_N (default 0 = no drop). Gekko
	// traffic (0x02) is never affected.
	unsigned mHsDropFirstN = 0;
	unsigned mHsSends = 0;
	bool mHsDropInit = false;
	// Handshake-channel test impairment. PIKMIN_NETPLAY_TEST_
	// LATENCY/JITTER/LOSS_MS/PCT apply to handshake datagrams too (receive-
	// side delay/loss, mirroring the LossyLink one-way model for gekko),
	// so DELAY=auto measures the impaired RTT. Gekko traffic is unaffected
	// here (it goes through LossyLink); unknown channels are dropped.
	bool mHsImpInit = false;
	double mHsLatMs = 0.0;
	double mHsJitMs = 0.0;
	double mHsLossPct = 0.0;
	uint64_t mHsRng = 0;
	struct HsDelayed {
		double deliverAtMs = 0.0;
		Datagram gram;
	};
	std::vector<HsDelayed> mHsDelayed;
	double hs_now_ms() const;
	double hs_draw_uniform(double lo, double hi);
	bool hs_draw_drop();
 };

// GekkoNet link: a UdpSocket filtered to the gekko channel, exposed as a
// GekkoNetAdapter. Address blobs are kAddrBytes (IPv4 + port, network
// order). Memory contract (see backend.cpp HandleData): receive_data()
// returns an array the adapter owns (GekkoNet never frees the array
// itself); every result struct, addr blob and payload is malloc'd and
// released through free_data().
class GekkoLink {
public:
	explicit GekkoLink(UdpSocket* sock);
	~GekkoLink();

	GekkoLink(const GekkoLink&)            = delete;
	GekkoLink& operator=(const GekkoLink&) = delete;

	GekkoNetAdapter* adapter();

	// Test hook: inject a received gekko payload without a socket.
	void inject_for_test(const uint8_t* data, size_t len);

	// Drains datagrams arrived on the handshake channel (0x01). The
	// session handshake pump calls this; GekkoNet never sees them.
	std::vector<UdpSocket::Datagram> drain_handshake();

	// Called by the C send_data trampoline.
	void send_to_peer(uint32_t ipHostOrder, uint16_t port, const uint8_t* data, size_t len);
	void send_inner(uint32_t ipHostOrder, uint16_t port, const uint8_t* data, size_t len);
	// Called by the C receive_data trampoline.
	struct GekkoNetResult** receive_inner(int* length);

 private:
	UdpSocket* mSock;
	GekkoNetAdapter mAdapter;
	std::vector<struct GekkoNetResult*> mResults;
	std::vector<std::vector<uint8_t>> mInjected;
	std::vector<UdpSocket::Datagram> mGekkoPending;
	std::vector<UdpSocket::Datagram> mHandshakePending;
};

// Lossy wrapper adapter over any inner GekkoNetAdapter: one-way latency,
// jitter, loss % and reordering. Loss is applied once, on receive, so the
// configured lossPct is the effective one-way rate. Parameters are
// explicit (the session reads PIKMIN_NETPLAY_TEST_LATENCY_MS / _JITTER_MS /
// _LOSS_PCT / _SEED); the randomness is a local std::mt19937 and never
// touches the sim RNG.
struct LossyParams {
	double latencyMs = 0.0; // base one-way delay
	double jitterMs  = 0.0; // extra uniform [0, jitterMs]
	double lossPct   = 0.0; // 0..100, dropped before delay
	double reorderExtraMs = 0.0; // share of packets held this much longer
	double reorderPct     = 0.0; // ... (0..100)
	uint32_t seed = 0;
};

class LossyLink {
public:
	LossyLink(GekkoNetAdapter* inner, const LossyParams& params);
	~LossyLink();

	LossyLink(const LossyLink&)            = delete;
	LossyLink& operator=(const LossyLink&) = delete;

	GekkoNetAdapter* adapter();

	// Called by the C trampolines. Instances are distinguished by a small
	// static slot (at most two sessions per process: the transport test;
	// production runs one).
	void send_inner(struct GekkoNetAddress* addr, const char* data, int length);
	struct GekkoNetResult** receive_inner(int* length);
	int slot() const { return mSlot; }

private:
	GekkoNetAdapter* mInner;
	GekkoNetAdapter mAdapter;
	LossyParams mParams;
	int mSlot = 0;
	std::vector<struct GekkoNetResult*> mResults;
	struct Delayed {
		double deliverAtMs = 0.0;
		struct GekkoNetResult* res = nullptr;
	};
	std::vector<Delayed> mPending;
	// Local RNG state (opaque; defined in the .cpp so this header stays
	// engine-free without <random> in the interface).
	struct Rng;
	Rng* mRng;

	static double now_ms();
	double draw_delay_ms();
	bool draw_drop();
};
} // namespace pc_netplay_transport
