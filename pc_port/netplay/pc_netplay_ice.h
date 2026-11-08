#pragma once
// Netplay ICE transport and copy-paste connection codes.
//
// Engine-free TU (libjuice C API + the C++ standard library only), so the
// host-run ICE test can link it without the game. Includes only `gekkonet.h`
// from GekkoNet (C-compatible) for the adapter.
//
// Wire format: identical to pc_netplay_udp. Every datagram sent through
// juice_send() carries a 1-byte channel prefix:
//   0x01  handshake (session hello/ack; consumed by pc_netplay_session)
//   0x02  gekko     (GekkoNet packets; the only channel the adapter exposes)
// Unknown channels are dropped. Every declared length is bounded before use
// (kMaxDatagram); oversized datagrams are dropped, never truncated.
//
// libjuice invokes its callbacks on its own thread, so incoming datagrams go
// into a mutex-locked bounded queue that the session drains from the main
// thread.
//
// Connection codes: one line of base64url over a compact binary form, with a
// text version prefix and a CRC32. The SDP is libjuice's local description,
// which bundles all gathered candidates, so no trickle exchange is needed:
// one code each way.
//
//   Answer (and the bare v1 offer):
//     "NPIX1-" + base64url(version:1=0x01 | kind:1('O'/'A') | sdp_len:u16be |
//                           sdp | crc32:u32be)
//   Offer with the session setup (what --netplay-host-ice prints):
//     "NPIX2-" + base64url(version:1=0x02 | kind:1('O') | sdp_len:u16be |
//                           cfg_len:u16be | desc_len:u16be | seed:u32be |
//                           sdp | cfg | desc | crc32:u32be)
//     cfg  = the host's sim-relevant settings text (the joiner adopts it for
//            the session only, never writing its settings file)
//     desc = an optional session descriptor, "key=value;" text (the
//            captains; for VS also the mode and the VS map's map.json);
//            empty is valid
//     seed = the netplay seed

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "gekkonet.h"
#include "juice/juice.h"

namespace pc_netplay_ice {
constexpr uint8_t kChannelHandshake = 0x01;
constexpr uint8_t kChannelGekko     = 0x02;
constexpr size_t kMaxDatagram      = 4096;
constexpr size_t kAddrBytes        = 6; // fixed placeholder blob over ICE
constexpr int kMaxRecvPerPoll      = 64;

// ---- network configuration (env-driven) ----

struct StunServer {
	std::string host;
	uint16_t port = 0;
};

struct TurnServer {
	std::string host;
	uint16_t port = 0;
	std::string user;
	std::string pass;
};

struct IceNetConfig {
	// libjuice v1.7.4 takes a single STUN server per agent, so the agent
	// uses stun.front(); the rest is kept for the log.
	std::vector<StunServer> stun;
	std::vector<TurnServer> turn;
	uint16_t portBegin = 0; // local port range (0/0 = ephemeral)
	uint16_t portEnd   = 0;
	bool turnOnly      = false; // no STUN + relay candidates only
	// libjuice bind address (juice_config_t.bind_address). Empty = any
	// interface. "127.0.0.1" gives a single loopback host candidate, which
	// the test harnesses use.
	std::string bindAddress;
};

// Defaults: no STUN and no TURN, so no third-party server is contacted;
// ephemeral local ports; any interface. Overridable:
//   PIKMIN_NETPLAY_STUN     "host:port[,host:port...]", "public" (the servers
//                           in kPublicStunServers) or "none" (the default)
//   PIKMIN_NETPLAY_TURN     "host:port:user:pass[,...]" (empty = none)
//   PIKMIN_NETPLAY_ICE_PORT_BEGIN / _END
//   PIKMIN_NETPLAY_ICE_TURN_ONLY=1
//   PIKMIN_NETPLAY_ICE_BIND "ip" (empty = any)
//   PIKMIN_NETPLAY_ICE_TIMEOUT_MS (answer and connect waits; default 600000)
//   PIKMIN_NETPLAY_ICE_GATHER_TIMEOUT_MS (default 15000)
extern const char* const kPublicStunServers;
IceNetConfig ice_net_config_from_env();
// One log line describing the configuration (servers, never TURN passwords).
std::string ice_net_config_text(const IceNetConfig& cfg);
// True when an ICE selected-local description is a relay candidate.
bool ice_selected_local_is_relay(const std::string& selectedLocal);

// ---- connection codes ----

// True when an "a=candidate:..." SDP line is a relay candidate.
bool ice_candidate_line_is_relay(const std::string& line);
// Drops every non-relay "a=candidate:" line from an SDP (TURN-only mode).
std::string ice_filter_relay_candidates(const std::string& sdp);
bool ice_sdp_has_relay(const std::string& sdp);

// Encodes kind+sdp into the "NPIX1-..." code. Fails on empty/oversized SDP.
bool ice_encode_code(bool isOffer, const std::string& sdp, std::string* out, std::string* err);
// Strict decode: prefix, base64, bounds, version, kind, length, CRC. On
// failure returns false with a one-line reason in *err (never throws).
bool ice_decode_code(const std::string& text, bool* isOffer, std::string* sdp, std::string* err);

struct SessionBundle {
	uint32_t seed = 0;
	std::string configText;
	std::string descriptor;
};

// Upper bounds enforced before any allocation in the v2 decode path. Every
// length travels in a u16 field.
constexpr size_t kMaxBundleSdpBytes    = 8192;
constexpr size_t kMaxBundleConfigBytes = 8192;
constexpr size_t kMaxBundleDescBytes   = 16384; // room for a VS map.json
static_assert(kMaxBundleSdpBytes <= 0xFFFF, "v2 SDP length is a u16 field");
static_assert(kMaxBundleConfigBytes <= 0xFFFF, "v2 config length is a u16 field");
static_assert(kMaxBundleDescBytes <= 0xFFFF, "v2 descriptor length is a u16 field");
// Largest v2 offer text: "NPIX2-" + base64url of the largest raw form.
constexpr size_t kMaxOfferV2Chars =
    6 + ((12 + kMaxBundleSdpBytes + kMaxBundleConfigBytes + kMaxBundleDescBytes + 4 + 2) / 3) * 4;

bool ice_encode_offer_v2(const std::string& sdp, const SessionBundle& bundle, std::string* out,
                         std::string* err);
bool ice_decode_offer_v2(const std::string& text, std::string* sdp, SessionBundle* bundle, std::string* err);

// v3 (what this build prints): "NPIX3-" + base64url of a compact binary
// form, an offer or an answer. libjuice's SDP is packed to its fields (six-
// bit ICE credentials, binary IPv4 candidates), the sim settings to their
// values in this build's key order (a 16-bit hash of the keys refuses
// another build); either falls back to text when it does not round-trip.
// Decoding a packed offer needs the joiner's own settings text for the key
// list (ice_set_local_config_text). v1/v2 codes still decode.
void ice_set_local_config_text(const std::string& text);
bool ice_encode_v3(bool isOffer, const std::string& sdp, const SessionBundle* bundle, std::string* out,
                   std::string* err);
bool ice_decode_v3(const std::string& text, bool* isOffer, std::string* sdp, SessionBundle* bundle, std::string* err);
// An offer of any version (v2 or v3), or an answer (v1 or v3).
bool ice_decode_any_offer(const std::string& text, std::string* sdp, SessionBundle* bundle, std::string* err);
bool ice_decode_any_answer(const std::string& text, std::string* sdp, std::string* err);

// Which code a player pasted.
enum CodeKind { kCodeEmpty, kCodeGarbage, kCodeV1, kCodeOfferV2, kCodeV3 };
CodeKind ice_classify_code(const std::string& text);

// Reads a code from a CLI value: "@file" reads the file (trimmed, bounded:
// a file larger than any valid code is refused unread), anything else is
// the literal code (trimmed).
bool ice_read_code_arg(const std::string& arg, std::string* code, std::string* err);
// Writes a code to a file atomically (a temporary file next to it, then a
// rename over the target), so a reader polling the path never sees a
// partial code. No-op when path is empty.
bool ice_write_code_file(const std::string& path, const std::string& code, std::string* err);

// ---- agent socket (same send/receive surface as UdpSocket) ----

class IceSocket {
public:
	IceSocket();
	~IceSocket();

	IceSocket(const IceSocket&)            = delete;
	IceSocket& operator=(const IceSocket&) = delete;

	// Host: creates the agent, gathers, returns the "NPIX2-" offer carrying
	// the session bundle. Pumps pump() while gathering.
	bool host_create_offer(const IceNetConfig& cfg, const SessionBundle& bundle, std::string* offerOut,
	                       std::string* err, std::function<void()> pump = std::function<void()>());
	// Joiner: validates the "NPIX2-" offer, creates the agent, gathers,
	// returns the "NPIX1-" answer code and the offer's bundle.
	bool join_create_answer(const IceNetConfig& cfg, const std::string& offer, std::string* answerOut,
	                        std::string* err, std::function<void()> pump = std::function<void()>(),
	                        SessionBundle* bundleOut = nullptr);
	// Host: validates the answer and applies it to the agent.
	bool host_apply_answer(const std::string& answer, std::string* err);
	// Blocks (polling) until JUICE_STATE_COMPLETED, FAILED, or timeoutMs.
	// On success *completedMsOut is the ICE connect time from the moment both
	// descriptions were known. Pumps pump() about every 50 ms. In TURN-only
	// mode fails unless the selected local candidate is a relay candidate.
	bool wait_connected(double timeoutMs, double* completedMsOut, std::string* err,
	                    std::function<void()> pump = std::function<void()>());

	// Sends channel + payload through the ICE agent. Returns false when not
	// connected, the frame exceeds kMaxDatagram, or the send fails.
	bool send_payload(uint8_t channel, const uint8_t* data, size_t len);
	// Address is ignored: the agent is connected 1:1.
	bool send_to(uint8_t channel, const uint8_t* data, size_t len, uint32_t ipHostOrder, uint16_t port);

	struct Datagram {
		uint8_t channel = 0;
		std::vector<uint8_t> payload;
		uint32_t fromIpHostOrder = 0; // always 127.0.0.1 over ICE
		uint16_t fromPort        = 0; // always 1 over ICE
	};
	// Drains the receive queue (up to kMaxRecvPerPoll). Never blocks.
	std::vector<Datagram> recv();

	void close();

	// Runs on this side's gathered SDP before it is encoded (not in TURN-only
	// mode): the session adds the router's mapped address as a candidate.
	void set_local_sdp_hook(std::function<void(std::string&)> hook) { mSdpHook = std::move(hook); }

	std::string selected_local() const;
	std::string selected_remote() const;
	std::string state_str() const;
	bool failed() const; // the agent reached JUICE_STATE_FAILED
	double completed_ms() const; // -1 until COMPLETED

private:
	static void on_state(juice_agent_t* agent, juice_state_t state, void* userPtr);
	static void on_candidate(juice_agent_t* agent, const char* sdp, void* userPtr);
	static void on_gathering_done(juice_agent_t* agent, void* userPtr);
	static void on_recv(juice_agent_t* agent, const char* data, size_t size, void* userPtr);

	bool create_agent(const IceNetConfig& cfg, std::string* err);
	bool gather(const IceNetConfig& cfg, std::string* sdpOut, std::string* err, std::function<void()> pump);
	std::function<void(std::string&)> mSdpHook;
	bool wait_gathering_done(double timeoutMs, std::string* err, std::function<void()> pump);
	bool local_description(std::string* out, std::string* err);
	bool apply_remote(const std::string& sdp, bool turnOnly, std::string* err);

	mutable void* mMutex = nullptr; // std::mutex* (opaque: header stays light)
	juice_agent_t* mAgent = nullptr;
	bool mGatheringDone = false;
	int mState = 0;
	std::vector<Datagram> mQueue;
	std::string mSelectedLocal;
	std::string mSelectedRemote;
	double mCreateMs = 0;
	double mConnectStartMs = 0;
	double mCompletedMs = -1;
	bool mTurnOnly = false;
	bool mClosed = false;
};

// GekkoNet link over an IceSocket, mirroring GekkoLink: the session drains
// handshake bytes itself, and GekkoNet sees only the gekko channel through
// the adapter. Address blobs are the fixed kAddrBytes placeholder
// (127.0.0.1:1); the agent is connected 1:1, so delivery ignores the content,
// but GekkoNet matches actors by blob equality, so the session registers the
// remote actor with these exact bytes. Same malloc/free contract as
// GekkoLink.
class IceLink {
public:
	explicit IceLink(IceSocket* sock);
	~IceLink();

	IceLink(const IceLink&)            = delete;
	IceLink& operator=(const IceLink&) = delete;

	GekkoNetAdapter* adapter();

	// Drains datagrams arrived on the handshake channel (0x01).
	std::vector<IceSocket::Datagram> drain_handshake();

	void send_to_peer(uint32_t ipHostOrder, uint16_t port, const uint8_t* data, size_t len);
	struct GekkoNetResult** receive_inner(int* length);

private:
	void route(std::vector<IceSocket::Datagram>& grams);

	IceSocket* mSock;
	GekkoNetAdapter mAdapter;
	std::vector<struct GekkoNetResult*> mResults;
	std::vector<IceSocket::Datagram> mGekkoPending;
	std::vector<IceSocket::Datagram> mHandshakePending;
};

// PIKMIN_NETPLAY_ICE_TIMEOUT_MS (1 s to 30 min), default 600000 = 10 min, so
// a human copy-paste round trip fits.
double ice_connect_timeout_ms();
// Polls a file until it holds a valid answer code (logs each new decode
// error instead of failing silently).
bool ice_poll_answer_file(const std::string& path, double timeoutMs, std::function<void()> pump,
                          std::string* answerOut, std::string* err);
// Reads answer-code lines from stdin on a helper thread while pump() keeps
// the window alive. A bad line prints its decode error and re-prompts; only
// EOF with no valid answer fails.
bool ice_read_answer_stdin(std::function<void()> pump, std::string* answerOut, std::string* err);

} // namespace pc_netplay_ice
