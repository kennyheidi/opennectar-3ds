// Netplay ICE transport and copy-paste connection codes. See the header for
// the wire and code contract. Engine-free: libjuice C API + the standard
// library only (no game, no SDL, no Winsock).

#include "netplay/pc_netplay_ice.h"

#include "juice/juice.h"

#include <stdlib.h>
#include <cstdlib>
#include <string.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// The session talks to either transport through the same channel bytes and
// sizes, so they must match the UDP transport exactly.
#include "netplay/pc_netplay_udp.h"
static_assert(pc_netplay_ice::kChannelHandshake == pc_netplay_transport::kChannelHandshake,
              "ICE/UDP handshake channel must match");
static_assert(pc_netplay_ice::kChannelGekko == pc_netplay_transport::kChannelGekko,
              "ICE/UDP gekko channel must match");
static_assert(pc_netplay_ice::kMaxDatagram == pc_netplay_transport::kMaxDatagram,
              "ICE/UDP max datagram must match");
static_assert(pc_netplay_ice::kAddrBytes == pc_netplay_transport::kAddrBytes, "ICE/UDP addr bytes must match");

namespace pc_netplay_ice {
namespace {

double now_ms()
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

const char* getenv_nonempty(const char* name)
{
	const char* v = std::getenv(name);
	return (v != nullptr && *v != '\0') ? v : nullptr;
}

std::string trim(const std::string& s)
{
	size_t b = 0;
	while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
	size_t e = s.size();
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
	return s.substr(b, e - b);
}

bool parse_host_port(const std::string& text, std::string* host, uint16_t* port)
{
	size_t colon = text.rfind(':');
	if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) return false;
	std::string h = text.substr(0, colon);
	std::string p = text.substr(colon + 1);
	if (h.empty() || h.size() > 253 || p.empty() || p.size() > 5) return false;
	for (char c : p) {
		if (c < '0' || c > '9') return false;
	}
	long n = strtol(p.c_str(), nullptr, 10);
	if (n <= 0 || n > 65535) return false;
	*host = h;
	*port = (uint16_t)n;
	return true;
}

// CRC32 (IEEE 802.3). libjuice's own crc32.c is internal, not an API.
uint32_t crc32_of(const uint8_t* data, size_t len)
{
	static std::once_flag once;
	static uint32_t table[256];
	std::call_once(once, []() {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			table[i] = c;
		}
	});
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

// base64url (RFC 4648 section 5): unpadded output, padded input accepted.
int b64_value(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '-') return 62;
	if (c == '_') return 63;
	if (c == '=') return -2;
	return -1;
}

std::string b64url_encode(const uint8_t* data, size_t len)
{
	static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	std::string out;
	out.reserve((len + 2) / 3 * 4);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t n = (uint32_t)data[i] << 16;
		size_t rem = len - i;
		if (rem > 1) n |= (uint32_t)data[i + 1] << 8;
		if (rem > 2) n |= data[i + 2];
		out.push_back(digits[(n >> 18) & 63]);
		out.push_back(digits[(n >> 12) & 63]);
		if (rem > 1) out.push_back(digits[(n >> 6) & 63]);
		if (rem > 2) out.push_back(digits[n & 63]);
	}
	return out;
}

bool b64url_decode(const std::string& text, std::vector<uint8_t>* out, std::string* err)
{
	out->clear();
	if (text.empty() || text.size() > kMaxOfferV2Chars) {
		*err = "bad length";
		return false;
	}
	size_t pad = 0;
	while (pad < text.size() && text[text.size() - 1 - pad] == '=') ++pad;
	if (pad > 2) {
		*err = "bad padding";
		return false;
	}
	const size_t body = text.size() - pad;
	if (body % 4 == 1) {
		*err = "bad length";
		return false;
	}
	uint32_t acc = 0;
	int bits     = 0;
	for (size_t i = 0; i < body; ++i) {
		int v = b64_value(text[i]);
		if (v < 0) {
			*err = "bad alphabet";
			return false;
		}
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out->push_back((uint8_t)((acc >> bits) & 0xFF));
		}
	}
	return true;
}

constexpr size_t kMaxSdpBytes = kMaxBundleSdpBytes;
constexpr size_t kMaxRawBytes = 4 + kMaxSdpBytes + 4;

void put_u16be(std::vector<uint8_t>& raw, size_t v)
{
	raw.push_back((uint8_t)((v >> 8) & 0xFF));
	raw.push_back((uint8_t)(v & 0xFF));
}

void put_crc(std::vector<uint8_t>& raw)
{
	const uint32_t crc = crc32_of(raw.data(), raw.size());
	raw.push_back((uint8_t)((crc >> 24) & 0xFF));
	raw.push_back((uint8_t)((crc >> 16) & 0xFF));
	raw.push_back((uint8_t)((crc >> 8) & 0xFF));
	raw.push_back((uint8_t)(crc & 0xFF));
}

uint32_t get_u32be(const uint8_t* p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool sdp_has_credentials(const std::string& sdp)
{
	return sdp.find("a=ice-ufrag:") != std::string::npos && sdp.find("a=ice-pwd:") != std::string::npos;
}

std::vector<std::string> split_list(const std::string& text)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t comma = text.find(',', pos);
		std::string entry = trim(comma == std::string::npos ? text.substr(pos) : text.substr(pos, comma - pos));
		if (!entry.empty()) out.push_back(entry);
		if (comma == std::string::npos) break;
		pos = comma + 1;
	}
	return out;
}

} // namespace

const char* const kPublicStunServers = "stun.l.google.com:19302,stun.cloudflare.com:3478";

IceNetConfig ice_net_config_from_env()
{
	IceNetConfig cfg;
	// No STUN unless asked for: by default the codes carry this PC's own
	// addresses only (LAN, VPN such as Tailscale/ZeroTier, public IPv6) and
	// no third-party server is contacted. "public" opts into public STUN.
	const char* stun = getenv_nonempty("PIKMIN_NETPLAY_STUN");
	std::string stunText = stun != nullptr ? stun : "none";
	std::string stunLower = trim(stunText);
	for (char& c : stunLower)
		if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
	if (stunLower == "public") stunText = kPublicStunServers;
	if (stunLower != "none" && stunLower != "off" && stunLower != "empty") {
		for (const std::string& entry : split_list(stunText)) {
			StunServer s;
			if (parse_host_port(entry, &s.host, &s.port)) cfg.stun.push_back(s);
		}
	}
	if (const char* turn = getenv_nonempty("PIKMIN_NETPLAY_TURN")) {
		for (const std::string& entry : split_list(turn)) {
			// host:port:user:pass (hostname or IPv4 only: no ':' inside).
			size_t c1 = entry.find(':');
			size_t c2 = c1 == std::string::npos ? std::string::npos : entry.find(':', c1 + 1);
			size_t c3 = c2 == std::string::npos ? std::string::npos : entry.find(':', c2 + 1);
			TurnServer t;
			bool ok = c1 != std::string::npos && c2 != std::string::npos && c3 != std::string::npos
			       && entry.find(':', c3 + 1) == std::string::npos;
			if (ok) {
				long port = strtol(entry.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10);
				t.host    = entry.substr(0, c1);
				t.user    = entry.substr(c2 + 1, c3 - c2 - 1);
				t.pass    = entry.substr(c3 + 1);
				ok        = !t.host.empty() && port > 0 && port <= 65535 && !t.user.empty() && !t.pass.empty();
				if (ok) t.port = (uint16_t)port;
			}
			if (ok) {
				cfg.turn.push_back(t);
			} else {
				printf("[netplay] ice: ignoring a malformed PIKMIN_NETPLAY_TURN entry (want host:port:user:pass)\n");
				fflush(stdout);
			}
		}
	}
	auto read_port = [](const char* name) -> uint16_t {
		const char* v = getenv_nonempty(name);
		if (v == nullptr) return 0;
		char* end = nullptr;
		long n    = strtol(v, &end, 10);
		if (end == v || *end != '\0' || n < 0 || n > 65535) return 0;
		return (uint16_t)n;
	};
	cfg.portBegin = read_port("PIKMIN_NETPLAY_ICE_PORT_BEGIN");
	cfg.portEnd   = read_port("PIKMIN_NETPLAY_ICE_PORT_END");
	// A single bound means a single port (libjuice wants begin <= end).
	if (cfg.portBegin != 0 && cfg.portEnd == 0) cfg.portEnd = cfg.portBegin;
	if (cfg.portBegin == 0 && cfg.portEnd != 0) cfg.portBegin = cfg.portEnd;
	if (cfg.portEnd != 0 && cfg.portBegin > cfg.portEnd) cfg.portBegin = cfg.portEnd;
	if (const char* bind = getenv_nonempty("PIKMIN_NETPLAY_ICE_BIND")) cfg.bindAddress = trim(bind);
	if (const char* to = getenv_nonempty("PIKMIN_NETPLAY_ICE_TURN_ONLY")) cfg.turnOnly = (to[0] == '1' && to[1] == '\0');
	if (cfg.turnOnly) cfg.stun.clear();
	return cfg;
}

std::string ice_net_config_text(const IceNetConfig& cfg)
{
	std::string s = "stun=";
	if (cfg.stun.empty()) s += "none";
	for (size_t i = 0; i < cfg.stun.size(); ++i)
		s += (i ? "," : "") + cfg.stun[i].host + ":" + std::to_string(cfg.stun[i].port);
	s += " turn=";
	if (cfg.turn.empty()) s += "none";
	for (size_t i = 0; i < cfg.turn.size(); ++i)
		s += (i ? "," : "") + cfg.turn[i].host + ":" + std::to_string(cfg.turn[i].port);
	s += " bind=" + (cfg.bindAddress.empty() ? std::string("any") : cfg.bindAddress);
	if (cfg.portBegin != 0) s += " ports=" + std::to_string(cfg.portBegin) + "-" + std::to_string(cfg.portEnd);
	if (cfg.turnOnly) s += " turn-only";
	return s;
}

bool ice_candidate_line_is_relay(const std::string& line) { return line.find(" typ relay") != std::string::npos; }

bool ice_selected_local_is_relay(const std::string& selectedLocal)
{
	return selectedLocal.find("typ relay") != std::string::npos;
}

std::string ice_filter_relay_candidates(const std::string& sdp)
{
	// Split on LF, tolerating CRLF, and rejoin with CRLF. A CRLF input with
	// no candidate to drop round-trips byte-identically.
	std::string out;
	size_t pos = 0;
	while (pos <= sdp.size()) {
		size_t eol       = sdp.find('\n', pos);
		std::string line = (eol == std::string::npos) ? sdp.substr(pos) : sdp.substr(pos, eol - pos);
		if (!line.empty() && line[line.size() - 1] == '\r') line.resize(line.size() - 1);
		if (line.empty() && pos == sdp.size()) break;
		bool isCandidate = line.compare(0, 12, "a=candidate:") == 0;
		if (!isCandidate || ice_candidate_line_is_relay(line)) {
			out += line;
			out += "\r\n";
		}
		if (eol == std::string::npos) break;
		pos = eol + 1;
	}
	if (!sdp.empty() && sdp[sdp.size() - 1] != '\n' && out.size() >= 2 && out.compare(out.size() - 2, 2, "\r\n") == 0)
		out.resize(out.size() - 2);
	return out;
}

bool ice_sdp_has_relay(const std::string& sdp)
{
	size_t pos = 0;
	while (pos <= sdp.size()) {
		size_t eol       = sdp.find('\n', pos);
		std::string line = (eol == std::string::npos) ? sdp.substr(pos) : sdp.substr(pos, eol - pos);
		if (line.compare(0, 12, "a=candidate:") == 0 && ice_candidate_line_is_relay(line)) return true;
		if (eol == std::string::npos) break;
		pos = eol + 1;
	}
	return false;
}

bool ice_encode_code(bool isOffer, const std::string& sdp, std::string* out, std::string* err)
{
	if (sdp.empty() || sdp.size() > kMaxSdpBytes) {
		if (err != nullptr) *err = "SDP empty or oversized";
		return false;
	}
	std::vector<uint8_t> raw;
	raw.reserve(sdp.size() + 8);
	raw.push_back(0x01);
	raw.push_back(isOffer ? (uint8_t)'O' : (uint8_t)'A');
	put_u16be(raw, sdp.size());
	raw.insert(raw.end(), sdp.begin(), sdp.end());
	put_crc(raw);
	*out = "NPIX1-" + b64url_encode(raw.data(), raw.size());
	return true;
}

bool ice_decode_code(const std::string& text, bool* isOffer, std::string* sdp, std::string* err)
{
	auto fail = [&](const char* why) {
		if (err != nullptr) *err = std::string("bad ICE code: ") + why;
		return false;
	};
	std::string t = trim(text);
	if (t.compare(0, 6, "NPIX1-") != 0) return fail("missing NPIX1- version prefix");
	if (t.size() > 6 + 16384) return fail("too long (limit 16384)");
	std::vector<uint8_t> raw;
	std::string derr;
	if (!b64url_decode(t.substr(6), &raw, &derr)) return fail(derr.c_str());
	if (raw.size() < 2 + 2 + 1 + 4) return fail("too short");
	if (raw.size() > kMaxRawBytes) return fail("too long");
	if (raw[0] != 0x01) return fail("unsupported version");
	if (raw[1] != 'O' && raw[1] != 'A') return fail("bad kind");
	const size_t sdpLen = ((size_t)raw[2] << 8) | raw[3];
	if (4 + sdpLen + 4 != raw.size()) return fail("length mismatch");
	if (sdpLen == 0 || sdpLen > kMaxSdpBytes) return fail("bad SDP length");
	if (crc32_of(raw.data(), 4 + sdpLen) != get_u32be(raw.data() + 4 + sdpLen)) return fail("CRC mismatch");
	*isOffer = (raw[1] == 'O');
	*sdp     = std::string(raw.begin() + 4, raw.begin() + 4 + sdpLen);
	if (!sdp_has_credentials(*sdp)) return fail("SDP missing ice credentials");
	return true;
}

bool ice_encode_offer_v2(const std::string& sdp, const SessionBundle& bundle, std::string* out, std::string* err)
{
	auto fail = [&](const char* why) {
		if (err != nullptr) *err = why;
		return false;
	};
	if (sdp.empty() || sdp.size() > kMaxSdpBytes) return fail("SDP empty or oversized");
	if (bundle.configText.empty() || bundle.configText.size() > kMaxBundleConfigBytes)
		return fail("session config block empty or oversized");
	if (bundle.descriptor.size() > kMaxBundleDescBytes) return fail("session descriptor oversized");
	std::vector<uint8_t> raw;
	raw.reserve(16 + sdp.size() + bundle.configText.size() + bundle.descriptor.size());
	raw.push_back(0x02);
	raw.push_back((uint8_t)'O');
	put_u16be(raw, sdp.size());
	put_u16be(raw, bundle.configText.size());
	put_u16be(raw, bundle.descriptor.size());
	raw.push_back((uint8_t)((bundle.seed >> 24) & 0xFF));
	raw.push_back((uint8_t)((bundle.seed >> 16) & 0xFF));
	raw.push_back((uint8_t)((bundle.seed >> 8) & 0xFF));
	raw.push_back((uint8_t)(bundle.seed & 0xFF));
	raw.insert(raw.end(), sdp.begin(), sdp.end());
	raw.insert(raw.end(), bundle.configText.begin(), bundle.configText.end());
	raw.insert(raw.end(), bundle.descriptor.begin(), bundle.descriptor.end());
	put_crc(raw);
	*out = "NPIX2-" + b64url_encode(raw.data(), raw.size());
	return true;
}

bool ice_decode_offer_v2(const std::string& text, std::string* sdp, SessionBundle* bundle, std::string* err)
{
	auto fail = [&](const char* why) {
		if (err != nullptr) *err = std::string("bad ICE code: ") + why;
		return false;
	};
	std::string t = trim(text);
	if (t.compare(0, 6, "NPIX2-") != 0) return fail("missing NPIX2- version prefix");
	if (t.size() > kMaxOfferV2Chars) return fail(("too long (limit " + std::to_string(kMaxOfferV2Chars) + ")").c_str());
	std::vector<uint8_t> raw;
	std::string derr;
	if (!b64url_decode(t.substr(6), &raw, &derr)) return fail(derr.c_str());
	if (raw.size() < 12 + 4) return fail("too short");
	if (raw.size() > 12 + kMaxSdpBytes + kMaxBundleConfigBytes + kMaxBundleDescBytes + 4) return fail("too long");
	if (raw[0] != 0x02) return fail("unsupported version");
	if (raw[1] != 'O') return fail("bad kind");
	const size_t sdpLen  = ((size_t)raw[2] << 8) | raw[3];
	const size_t cfgLen  = ((size_t)raw[4] << 8) | raw[5];
	const size_t descLen = ((size_t)raw[6] << 8) | raw[7];
	if (sdpLen == 0 || sdpLen > kMaxSdpBytes) return fail("bad SDP length");
	if (cfgLen == 0 || cfgLen > kMaxBundleConfigBytes) return fail("bad session config length");
	if (descLen > kMaxBundleDescBytes) return fail("bad session descriptor length");
	const size_t body = 12 + sdpLen + cfgLen + descLen;
	if (body + 4 != raw.size()) return fail("length mismatch");
	if (crc32_of(raw.data(), body) != get_u32be(raw.data() + body)) return fail("CRC mismatch");
	*sdp               = std::string(raw.begin() + 12, raw.begin() + 12 + sdpLen);
	bundle->configText = std::string(raw.begin() + 12 + sdpLen, raw.begin() + 12 + sdpLen + cfgLen);
	bundle->descriptor = std::string(raw.begin() + 12 + sdpLen + cfgLen, raw.begin() + body);
	bundle->seed       = get_u32be(raw.data() + 8);
	if (!sdp_has_credentials(*sdp)) return fail("SDP missing ice credentials");
	return true;
}

CodeKind ice_classify_code(const std::string& text)
{
	const std::string t = trim(text);
	if (t.empty()) return kCodeEmpty;
	if (t.compare(0, 6, "NPIX3-") == 0) return kCodeV3;
	if (t.compare(0, 6, "NPIX2-") == 0) return kCodeOfferV2;
	if (t.compare(0, 6, "NPIX1-") == 0) return kCodeV1;
	return kCodeGarbage;
}

// ---- v3: the compact codes ----
namespace {

std::string sLocalConfigText; // ice_set_local_config_text

void put_varint(std::vector<uint8_t>& raw, uint64_t v)
{
	while (v >= 0x80) {
		raw.push_back((uint8_t)(v | 0x80));
		v >>= 7;
	}
	raw.push_back((uint8_t)v);
}

bool get_varint(const uint8_t* p, size_t n, size_t* at, uint64_t* v)
{
	*v = 0;
	for (int shift = 0; shift < 64; shift += 7) {
		if (*at >= n) return false;
		const uint8_t b = p[(*at)++];
		*v |= (uint64_t)(b & 0x7F) << shift;
		if ((b & 0x80) == 0) return true;
	}
	return false;
}

void put_bytes(std::vector<uint8_t>& raw, const std::string& s)
{
	put_varint(raw, s.size());
	raw.insert(raw.end(), s.begin(), s.end());
}

bool get_bytes(const uint8_t* p, size_t n, size_t* at, size_t limit, std::string* s)
{
	uint64_t len = 0;
	if (!get_varint(p, n, at, &len) || len > limit || len > n - *at) return false;
	s->assign((const char*)p + *at, (size_t)len);
	*at += (size_t)len;
	return true;
}

// Canonical decimal (no sign, no leading zeros) within [0, max].
bool parse_canonical_u32(const std::string& s, uint32_t max, uint32_t* out)
{
	if (s.empty() || s.size() > 10 || (s.size() > 1 && s[0] == '0')) return false;
	uint64_t v = 0;
	for (char c : s) {
		if (c < '0' || c > '9') return false;
		v = v * 10 + (uint64_t)(c - '0');
	}
	if (v > max) return false;
	*out = (uint32_t)v;
	return true;
}

bool parse_ipv4(const std::string& s, uint8_t out[4])
{
	size_t pos = 0;
	for (int i = 0; i < 4; ++i) {
		const size_t dot = i < 3 ? s.find('.', pos) : s.size();
		if (dot == std::string::npos) return false;
		uint32_t v = 0;
		if (!parse_canonical_u32(s.substr(pos, dot - pos), 255, &v)) return false;
		out[i] = (uint8_t)v;
		pos    = dot + 1;
	}
	return true;
}

std::string ipv4_text(const uint8_t* b)
{
	return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." + std::to_string(b[3]);
}

std::vector<std::string> split_tokens(const std::string& s, char sep)
{
	std::vector<std::string> out;
	size_t pos = 0;
	for (;;) {
		const size_t at = s.find(sep, pos);
		out.push_back(s.substr(pos, at == std::string::npos ? std::string::npos : at - pos));
		if (at == std::string::npos) return out;
		pos = at + 1;
	}
}

// ICE credentials are ice-chars (RFC 8445: ALPHA / DIGIT / "+" / "/"), so
// they pack at six bits per character.
const char kIceChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool put_ice_chars(std::vector<uint8_t>& raw, const std::string& s)
{
	if (s.empty() || s.size() > 255) return false;
	raw.push_back((uint8_t)s.size());
	uint32_t acc = 0;
	int bits     = 0;
	for (char c : s) {
		const char* at = strchr(kIceChars, c);
		if (c == '\0' || at == nullptr) return false;
		acc = (acc << 6) | (uint32_t)(at - kIceChars);
		bits += 6;
		while (bits >= 8) {
			bits -= 8;
			raw.push_back((uint8_t)(acc >> bits));
		}
	}
	if (bits > 0) raw.push_back((uint8_t)(acc << (8 - bits)));
	return true;
}

bool get_ice_chars(const uint8_t* p, size_t n, size_t* at, std::string* s)
{
	if (*at >= n) return false;
	const size_t len   = p[(*at)++];
	const size_t bytes = (len * 6 + 7) / 8;
	if (len == 0 || bytes > n - *at) return false;
	s->clear();
	uint32_t acc = 0;
	int bits     = 0;
	size_t used  = 0;
	while (s->size() < len) {
		if (bits < 6) {
			acc = (acc << 8) | p[*at + used++];
			bits += 8;
		}
		bits -= 6;
		*s += kIceChars[(acc >> bits) & 0x3F];
	}
	*at += bytes;
	return true;
}

// The SDP libjuice writes, as the fields it carries:
//   flags u8: 1 end-of-candidates, 2 ice-options:ice2
//   ufrag, pwd (six-bit ice-chars), candidate count u8, then each candidate
//     kind u8: bits 0-1 type (host/srflx/prflx/relay), 2 text address (not
//              IPv4), 3 same port as the previous one, 4 raddr/rport follow
//     foundation varint, priority (first u32be, then zigzag varint delta),
//     address (4 bytes, or length + text), port u16be unless bit 3,
//     raddr (length + text) and rport varint when bit 4
// Anything else (another line, component, transport) is not packed; the
// caller then sends the SDP text.
const char* const kCandidateTypes[] = { "host", "srflx", "prflx", "relay" };

bool sdp_pack(const std::string& sdp, std::vector<uint8_t>* out)
{
	std::vector<std::string> lines = split_tokens(sdp, '\n');
	if (lines.empty() || !lines.back().empty()) return false;
	lines.pop_back();
	std::string ufrag, pwd;
	uint8_t flags = 0;
	std::vector<std::string> candidates;
	for (std::string& line : lines) {
		if (line.empty() || line.back() != '\r') return false;
		line.pop_back();
		if (line.compare(0, 12, "a=ice-ufrag:") == 0) ufrag = line.substr(12);
		else if (line.compare(0, 10, "a=ice-pwd:") == 0) pwd = line.substr(10);
		else if (line.compare(0, 12, "a=candidate:") == 0) candidates.push_back(line.substr(12));
		else if (line == "a=end-of-candidates") flags |= 1;
		else if (line == "a=ice-options:ice2") flags |= 2;
		else return false;
	}
	if (candidates.size() > 32) return false;
	std::vector<uint8_t> raw;
	raw.push_back(flags);
	if (!put_ice_chars(raw, ufrag) || !put_ice_chars(raw, pwd)) return false;
	raw.push_back((uint8_t)candidates.size());
	uint32_t lastPriority = 0;
	uint32_t lastPort     = 0x10000;
	for (size_t i = 0; i < candidates.size(); ++i) {
		const std::vector<std::string> t = split_tokens(candidates[i], ' ');
		if ((t.size() != 8 && t.size() != 12) || t[1] != "1" || t[2] != "UDP" || t[6] != "typ") return false;
		uint32_t foundation = 0, priority = 0, port = 0;
		if (!parse_canonical_u32(t[0], 0xFFFFFFFFu, &foundation) || !parse_canonical_u32(t[3], 0xFFFFFFFFu, &priority)
		    || !parse_canonical_u32(t[5], 65535, &port))
			return false;
		int type = -1;
		for (int k = 0; k < 4; ++k)
			if (t[7] == kCandidateTypes[k]) type = k;
		if (type < 0) return false;
		uint8_t v4[4];
		const bool isV4 = parse_ipv4(t[4], v4);
		uint8_t kind    = (uint8_t)type;
		if (!isV4) kind |= 4;
		if (port == lastPort) kind |= 8;
		uint32_t rport = 0;
		if (t.size() == 12) {
			if (t[8] != "raddr" || t[10] != "rport" || !parse_canonical_u32(t[11], 65535, &rport)) return false;
			kind |= 16;
		}
		raw.push_back(kind);
		put_varint(raw, foundation);
		if (i == 0) {
			raw.push_back((uint8_t)(priority >> 24));
			raw.push_back((uint8_t)(priority >> 16));
			raw.push_back((uint8_t)(priority >> 8));
			raw.push_back((uint8_t)priority);
		} else {
			const int64_t d = (int64_t)priority - (int64_t)lastPriority;
			put_varint(raw, d < 0 ? ((uint64_t)(-d) << 1) - 1 : (uint64_t)d << 1);
		}
		if (isV4) raw.insert(raw.end(), v4, v4 + 4);
		else put_bytes(raw, t[4]);
		if (!(kind & 8)) put_u16be(raw, port);
		if (kind & 16) {
			put_bytes(raw, t[9]);
			put_varint(raw, rport);
		}
		lastPriority = priority;
		lastPort     = port;
	}
	*out = raw;
	return true;
}

bool sdp_unpack(const uint8_t* p, size_t n, size_t* at, std::string* sdp)
{
	if (*at >= n) return false;
	const uint8_t flags = p[(*at)++];
	std::string ufrag, pwd;
	if (!get_ice_chars(p, n, at, &ufrag) || !get_ice_chars(p, n, at, &pwd) || *at >= n) return false;
	const size_t count = p[(*at)++];
	if (count > 32) return false;
	std::string out = "a=ice-ufrag:" + ufrag + "\r\na=ice-pwd:" + pwd + "\r\n";
	uint32_t lastPriority = 0, lastPort = 0;
	for (size_t i = 0; i < count; ++i) {
		if (*at >= n) return false;
		const uint8_t kind = p[(*at)++];
		if (kind & 0xE0) return false;
		uint64_t foundation = 0;
		if (!get_varint(p, n, at, &foundation) || foundation > 0xFFFFFFFFu) return false;
		uint32_t priority = 0;
		if (i == 0) {
			if (n - *at < 4) return false;
			priority = get_u32be(p + *at);
			*at += 4;
		} else {
			uint64_t z = 0;
			if (!get_varint(p, n, at, &z)) return false;
			const int64_t d = (z & 1) ? -(int64_t)((z + 1) >> 1) : (int64_t)(z >> 1);
			const int64_t v = (int64_t)lastPriority + d;
			if (v < 0 || v > 0xFFFFFFFFll) return false;
			priority = (uint32_t)v;
		}
		std::string address;
		if (kind & 4) {
			if (!get_bytes(p, n, at, 64, &address) || address.empty()) return false;
			for (char c : address)
				if (c <= ' ' || c > '~') return false;
		} else {
			if (n - *at < 4) return false;
			address = ipv4_text(p + *at);
			*at += 4;
		}
		uint32_t port = lastPort;
		if (!(kind & 8)) {
			if (n - *at < 2) return false;
			port = ((uint32_t)p[*at] << 8) | p[*at + 1];
			*at += 2;
		} else if (i == 0) {
			return false;
		}
		out += "a=candidate:" + std::to_string(foundation) + " 1 UDP " + std::to_string(priority) + " " + address + " "
		     + std::to_string(port) + " typ " + kCandidateTypes[kind & 3];
		if (kind & 16) {
			std::string raddr;
			uint64_t rport = 0;
			if (!get_bytes(p, n, at, 64, &raddr) || raddr.empty() || !get_varint(p, n, at, &rport) || rport > 65535)
				return false;
			for (char c : raddr)
				if (c <= ' ' || c > '~') return false;
			out += " raddr " + raddr + " rport " + std::to_string(rport);
		}
		out += "\r\n";
		lastPriority = priority;
		lastPort     = port;
	}
	if (flags & ~3u) return false;
	if (flags & 1) out += "a=end-of-candidates\r\n";
	if (flags & 2) out += "a=ice-options:ice2\r\n";
	*sdp = out;
	return true;
}

// FNV-1a over the keys, folded to 16 bits: a different key list (another
// build) is refused instead of misread.
uint16_t keys_hash(const std::vector<std::string>& keys)
{
	uint32_t h = 2166136261u;
	for (const std::string& k : keys) {
		for (char c : k) h = (h ^ (uint8_t)c) * 16777619u;
		h = (h ^ ';') * 16777619u;
	}
	return (uint16_t)(h ^ (h >> 16));
}

// "k=v;" pairs (the trailing ';' included) split into keys and values.
bool split_config(const std::string& text, std::vector<std::string>* keys, std::vector<std::string>* values)
{
	if (text.empty() || text.back() != ';') return false;
	std::vector<std::string> pairs = split_tokens(text.substr(0, text.size() - 1), ';');
	for (const std::string& pair : pairs) {
		const size_t eq = pair.find('=');
		if (eq == std::string::npos || eq == 0) return false;
		keys->push_back(pair.substr(0, eq));
		values->push_back(pair.substr(eq + 1));
	}
	return true;
}

// The sim settings as values only, in the order of this build's key list:
//   keys hash u16be, then per value a varint: zigzag(n) << 1 for a canonical
//   integer, (length << 1) | 1 and the bytes for anything else.
bool config_pack(const std::string& text, std::vector<uint8_t>* out)
{
	std::vector<std::string> keys, values;
	if (!split_config(text, &keys, &values)) return false;
	std::vector<uint8_t> raw;
	put_u16be(raw, keys_hash(keys));
	for (const std::string& v : values) {
		char* end        = nullptr;
		const long long n = v.empty() ? 0 : std::strtoll(v.c_str(), &end, 10);
		if (!v.empty() && end != nullptr && *end == '\0' && std::to_string(n) == v && n > -(1ll << 40) && n < (1ll << 40)) {
			const uint64_t z = n < 0 ? ((uint64_t)(-n) << 1) - 1 : (uint64_t)n << 1;
			put_varint(raw, z << 1);
		} else {
			put_varint(raw, ((uint64_t)v.size() << 1) | 1);
			raw.insert(raw.end(), v.begin(), v.end());
		}
	}
	*out = raw;
	return true;
}

bool config_unpack(const uint8_t* p, size_t n, const std::string& localText, std::string* text, std::string* err)
{
	std::vector<std::string> keys, unused;
	if (!split_config(localText, &keys, &unused) || n < 2) {
		*err = "this build has no session settings list";
		return false;
	}
	if ((((uint16_t)p[0] << 8) | p[1]) != keys_hash(keys)) {
		*err = "the host's game is a different build (its settings list differs): both need the same build";
		return false;
	}
	size_t at = 2;
	std::string out;
	for (const std::string& key : keys) {
		uint64_t tag = 0;
		if (!get_varint(p, n, &at, &tag)) {
			*err = "truncated session settings";
			return false;
		}
		std::string value;
		if (tag & 1) {
			const uint64_t len = tag >> 1;
			if (len > 1024 || len > n - at) {
				*err = "bad session setting";
				return false;
			}
			value.assign((const char*)p + at, (size_t)len);
			at += (size_t)len;
			for (char c : value)
				if (c < ' ' || c > '~' || c == ';') {
					*err = "bad session setting";
					return false;
				}
		} else {
			const uint64_t z = tag >> 1;
			const long long v = (z & 1) ? -(long long)((z + 1) >> 1) : (long long)(z >> 1);
			value = std::to_string(v);
		}
		out += key + "=" + value + ";";
	}
	if (at != n) {
		*err = "trailing session settings";
		return false;
	}
	*text = out;
	return true;
}

} // namespace

void ice_set_local_config_text(const std::string& text) { sLocalConfigText = text; }

// v3 raw form: version 0x03 | kind 'O'/'A' | sdp block | offer only: seed
// varint, config block, descriptor bytes | crc32 u32be. The sdp block is
// mode u8 (0 text bytes, 1 packed) + data; the config block likewise, the
// packed form needing the joiner's own key list (ice_set_local_config_text).
bool ice_encode_v3(bool isOffer, const std::string& sdp, const SessionBundle* bundle, std::string* out, std::string* err)
{
	auto fail = [&](const char* why) {
		if (err != nullptr) *err = why;
		return false;
	};
	if (sdp.empty() || sdp.size() > kMaxSdpBytes) return fail("SDP empty or oversized");
	if (isOffer && bundle == nullptr) return fail("an offer needs the session bundle");
	std::vector<uint8_t> raw = { 0x03, isOffer ? (uint8_t)'O' : (uint8_t)'A' };
	std::vector<uint8_t> packed;
	std::string check;
	size_t at = 0;
	if (sdp_pack(sdp, &packed) && sdp_unpack(packed.data(), packed.size(), &at, &check) && check == sdp
	    && at == packed.size()) {
		raw.push_back(1);
		raw.insert(raw.end(), packed.begin(), packed.end());
	} else {
		raw.push_back(0);
		put_bytes(raw, sdp);
	}
	if (isOffer) {
		if (bundle->configText.empty() || bundle->configText.size() > kMaxBundleConfigBytes)
			return fail("session config block empty or oversized");
		if (bundle->descriptor.size() > kMaxBundleDescBytes) return fail("session descriptor oversized");
		put_varint(raw, bundle->seed);
		std::string unpacked, why;
		if (config_pack(bundle->configText, &packed)
		    && config_unpack(packed.data(), packed.size(), bundle->configText, &unpacked, &why)
		    && unpacked == bundle->configText) {
			raw.push_back(1);
			put_varint(raw, packed.size());
			raw.insert(raw.end(), packed.begin(), packed.end());
		} else {
			raw.push_back(0);
			put_bytes(raw, bundle->configText);
		}
		put_bytes(raw, bundle->descriptor);
	}
	put_crc(raw);
	*out = "NPIX3-" + b64url_encode(raw.data(), raw.size());
	return true;
}

bool ice_decode_v3(const std::string& text, bool* isOffer, std::string* sdp, SessionBundle* bundle, std::string* err)
{
	auto fail = [&](const std::string& why) {
		if (err != nullptr) *err = "bad ICE code: " + why;
		return false;
	};
	const std::string t = trim(text);
	if (t.compare(0, 6, "NPIX3-") != 0) return fail("missing NPIX3- version prefix");
	if (t.size() > kMaxOfferV2Chars) return fail("too long");
	std::vector<uint8_t> raw;
	std::string derr;
	if (!b64url_decode(t.substr(6), &raw, &derr)) return fail(derr);
	if (raw.size() < 2 + 1 + 4) return fail("too short");
	const size_t body = raw.size() - 4;
	if (crc32_of(raw.data(), body) != get_u32be(raw.data() + body)) return fail("CRC mismatch (copy the whole code)");
	if (raw[0] != 0x03) return fail("unsupported version");
	if (raw[1] != 'O' && raw[1] != 'A') return fail("bad kind");
	const uint8_t* p = raw.data();
	size_t at        = 2;
	if (p[at] == 1) {
		++at;
		if (!sdp_unpack(p, body, &at, sdp)) return fail("bad connection data");
	} else if (p[at] == 0) {
		++at;
		if (!get_bytes(p, body, &at, kMaxSdpBytes, sdp) || sdp->empty()) return fail("bad SDP");
	} else {
		return fail("bad SDP mode");
	}
	if (!sdp_has_credentials(*sdp)) return fail("SDP missing ice credentials");
	*isOffer = raw[1] == 'O';
	if (*isOffer) {
		SessionBundle b;
		uint64_t seed = 0;
		if (!get_varint(p, body, &at, &seed) || seed > 0xFFFFFFFFu || at >= body) return fail("bad seed");
		b.seed           = (uint32_t)seed;
		const uint8_t mode = p[at++];
		if (mode == 1) {
			uint64_t len = 0;
			if (!get_varint(p, body, &at, &len) || len > body - at || len > kMaxBundleConfigBytes)
				return fail("bad session settings");
			std::string why;
			if (!config_unpack(p + at, (size_t)len, sLocalConfigText, &b.configText, &why)) return fail(why);
			at += (size_t)len;
		} else if (mode == 0) {
			if (!get_bytes(p, body, &at, kMaxBundleConfigBytes, &b.configText) || b.configText.empty())
				return fail("bad session settings");
		} else {
			return fail("bad session settings mode");
		}
		if (!get_bytes(p, body, &at, kMaxBundleDescBytes, &b.descriptor)) return fail("bad session descriptor");
		if (bundle != nullptr) *bundle = b;
	}
	if (at != body) return fail("length mismatch");
	return true;
}

bool ice_decode_any_offer(const std::string& text, std::string* sdp, SessionBundle* bundle, std::string* err)
{
	if (ice_classify_code(text) != kCodeV3) return ice_decode_offer_v2(text, sdp, bundle, err);
	bool isOffer = false;
	if (!ice_decode_v3(text, &isOffer, sdp, bundle, err)) return false;
	if (!isOffer) {
		if (err != nullptr) *err = "that is an answer code: it goes to the host, who sent you a room code";
		return false;
	}
	return true;
}

bool ice_decode_any_answer(const std::string& text, std::string* sdp, std::string* err)
{
	bool isOffer = true;
	if (ice_classify_code(text) == kCodeV3) {
		if (!ice_decode_v3(text, &isOffer, sdp, nullptr, err)) return false;
	} else if (!ice_decode_code(text, &isOffer, sdp, err)) {
		return false;
	}
	if (isOffer) {
		if (err != nullptr) *err = "bad ICE code: expected an answer, got an offer";
		return false;
	}
	return true;
}

bool ice_read_code_arg(const std::string& arg, std::string* code, std::string* err)
{
	std::string a = trim(arg);
	if (a.size() > 1 && a[0] == '@') {
		std::string path = a.substr(1);
		FILE* f          = fopen(path.c_str(), "rb");
		if (f == nullptr) {
			if (err != nullptr) *err = "cannot open code file " + path;
			return false;
		}
		const size_t kMaxCodeFileBytes = kMaxOfferV2Chars + 4096;
		std::string raw;
		char chunk[4096];
		bool tooBig = false;
		while (true) {
			size_t n = fread(chunk, 1, sizeof(chunk), f);
			if (n > 0) raw.append(chunk, n);
			if (raw.size() > kMaxCodeFileBytes) {
				tooBig = true;
				break;
			}
			if (n < sizeof(chunk)) break;
		}
		fclose(f);
		if (tooBig) {
			if (err != nullptr) *err = "code file is too large to be a connection code: " + path;
			return false;
		}
		*code = trim(raw);
		if (code->empty()) {
			if (err != nullptr) *err = "code file is empty: " + path;
			return false;
		}
		return true;
	}
	*code = a;
	if (code->empty()) {
		if (err != nullptr) *err = "empty code";
		return false;
	}
	return true;
}

bool ice_write_code_file(const std::string& path, const std::string& code, std::string* err)
{
	if (path.empty()) return true;
	const std::string tmp = path + ".tmp";
	FILE* f               = fopen(tmp.c_str(), "wb");
	if (f == nullptr) {
		if (err != nullptr) *err = "cannot write code file " + tmp;
		return false;
	}
	bool ok = fwrite(code.c_str(), 1, code.size(), f) == code.size() && fwrite("\n", 1, 1, f) == 1;
	ok      = (fclose(f) == 0) && ok;
	if (!ok) {
		remove(tmp.c_str());
		if (err != nullptr) *err = "short write to " + tmp;
		return false;
	}
#ifdef _WIN32
	const bool moved = MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	const bool moved = rename(tmp.c_str(), path.c_str()) == 0;
#endif
	if (!moved) {
		remove(tmp.c_str());
		if (err != nullptr) *err = "cannot rename " + tmp + " to " + path;
		return false;
	}
	return true;
}

// ---- IceSocket ----

IceSocket::IceSocket() : mMutex(new std::mutex()) {}

IceSocket::~IceSocket() { close(); }

void IceSocket::close()
{
	std::mutex* mu = (std::mutex*)mMutex;
	if (mu == nullptr) return;
	juice_agent_t* a = nullptr;
	{
		std::lock_guard<std::mutex> lock(*mu);
		a       = mAgent;
		mAgent  = nullptr;
		mClosed = true;
	}
	// Outside the lock: juice_destroy joins libjuice's thread, and a racing
	// callback may be waiting on the mutex.
	if (a != nullptr) juice_destroy(a);
	delete mu;
	mMutex = nullptr;
}

void IceSocket::on_state(juice_agent_t* /*agent*/, juice_state_t state, void* userPtr)
{
	IceSocket* self = (IceSocket*)userPtr;
	if (self == nullptr || self->mMutex == nullptr) return;
	juice_agent_t* a = nullptr;
	{
		std::lock_guard<std::mutex> lock(*(std::mutex*)self->mMutex);
		if (self->mClosed) return;
		self->mState = (int)state;
		a            = self->mAgent;
	}
	printf("[netplay] ice state: %s\n", juice_state_to_string(state));
	fflush(stdout);
	if (state == JUICE_STATE_COMPLETED && a != nullptr) {
		char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
		char remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
		local[0] = remote[0] = '\0';
		std::string ls, rs;
		if (juice_get_selected_candidates(a, local, sizeof(local), remote, sizeof(remote)) == JUICE_ERR_SUCCESS) {
			ls = local;
			rs = remote;
		} else {
			char la[JUICE_MAX_ADDRESS_STRING_LEN], ra[JUICE_MAX_ADDRESS_STRING_LEN];
			if (juice_get_selected_addresses(a, la, sizeof(la), ra, sizeof(ra)) == JUICE_ERR_SUCCESS) {
				ls = std::string("addr ") + la;
				rs = std::string("addr ") + ra;
			}
		}
		std::lock_guard<std::mutex> lock(*(std::mutex*)self->mMutex);
		if (self->mClosed) return;
		self->mSelectedLocal  = ls;
		self->mSelectedRemote = rs;
		if (self->mCompletedMs < 0) {
			const double base = self->mConnectStartMs > 0 ? self->mConnectStartMs : self->mCreateMs;
			if (base > 0) self->mCompletedMs = now_ms() - base;
		}
		printf("[netplay] ice selected: local [%s] remote [%s]\n", ls.c_str(), rs.c_str());
		if (self->mCompletedMs >= 0) printf("[netplay] ice completed in %.0fms\n", self->mCompletedMs);
		fflush(stdout);
		return;
	}
	if (state == JUICE_STATE_FAILED) {
		printf("[netplay] ice failed: no working candidate pair\n");
		fflush(stdout);
	}
}

void IceSocket::on_candidate(juice_agent_t* /*agent*/, const char* /*sdp*/, void* /*userPtr*/) {}

void IceSocket::on_gathering_done(juice_agent_t* /*agent*/, void* userPtr)
{
	IceSocket* self = (IceSocket*)userPtr;
	if (self == nullptr || self->mMutex == nullptr) return;
	std::lock_guard<std::mutex> lock(*(std::mutex*)self->mMutex);
	self->mGatheringDone = true;
}

void IceSocket::on_recv(juice_agent_t* /*agent*/, const char* data, size_t size, void* userPtr)
{
	IceSocket* self = (IceSocket*)userPtr;
	if (self == nullptr || self->mMutex == nullptr || data == nullptr) return;
	if (size < 2 || size > kMaxDatagram) return;
	std::lock_guard<std::mutex> lock(*(std::mutex*)self->mMutex);
	if (self->mClosed || self->mAgent == nullptr) return;
	if (self->mQueue.size() >= 512) self->mQueue.erase(self->mQueue.begin());
	Datagram d;
	d.channel = (uint8_t)data[0];
	d.payload.assign((const uint8_t*)data + 1, (const uint8_t*)data + size);
	d.fromIpHostOrder = 0x7F000001;
	d.fromPort        = 1;
	self->mQueue.push_back(std::move(d));
}

bool IceSocket::create_agent(const IceNetConfig& cfg, std::string* err)
{
	if (mAgent != nullptr) {
		if (err != nullptr) *err = "agent already created";
		return false;
	}
	const char* dbg = getenv_nonempty("PIKMIN_NETPLAY_ICE_DEBUG");
	juice_set_log_level(dbg != nullptr && dbg[0] == '1' && dbg[1] == '\0' ? JUICE_LOG_LEVEL_VERBOSE
	                                                                      : JUICE_LOG_LEVEL_WARN);
	juice_config_t jc;
	memset(&jc, 0, sizeof(jc));
	jc.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
	if (!cfg.stun.empty()) {
		// juice_create deep-copies every string, and cfg outlives the call.
		jc.stun_server_host = cfg.stun.front().host.c_str();
		jc.stun_server_port = cfg.stun.front().port;
	}
	if (!cfg.bindAddress.empty()) jc.bind_address = cfg.bindAddress.c_str();
	std::vector<juice_turn_server_t> turn;
	for (const TurnServer& t : cfg.turn) {
		juice_turn_server_t ts;
		memset(&ts, 0, sizeof(ts));
		ts.host     = t.host.c_str();
		ts.port     = t.port;
		ts.username = t.user.c_str();
		ts.password = t.pass.c_str();
		turn.push_back(ts);
	}
	if (!turn.empty()) {
		jc.turn_servers       = turn.data();
		jc.turn_servers_count = (int)turn.size();
	}
	jc.local_port_range_begin = cfg.portBegin;
	jc.local_port_range_end   = cfg.portEnd;
	jc.cb_state_changed       = &IceSocket::on_state;
	jc.cb_candidate           = &IceSocket::on_candidate;
	jc.cb_gathering_done      = &IceSocket::on_gathering_done;
	jc.cb_recv                = &IceSocket::on_recv;
	jc.user_ptr               = this;
	mTurnOnly                 = cfg.turnOnly;
	juice_agent_t* a          = juice_create(&jc);
	if (a == nullptr) {
		if (err != nullptr) *err = "juice_create failed";
		return false;
	}
	mAgent    = a;
	mCreateMs = now_ms();
	return true;
}

bool IceSocket::wait_gathering_done(double timeoutMs, std::string* err, std::function<void()> pump)
{
	const double start = now_ms();
	double lastPump    = 0;
	while (true) {
		{
			std::lock_guard<std::mutex> lock(*(std::mutex*)mMutex);
			if (mGatheringDone) return true;
		}
		if (now_ms() - start > timeoutMs) {
			if (err != nullptr) *err = "candidate gathering timed out";
			return false;
		}
		if (pump && now_ms() - lastPump > 50) {
			pump();
			lastPump = now_ms();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

bool IceSocket::local_description(std::string* out, std::string* err)
{
	char buf[JUICE_MAX_SDP_STRING_LEN];
	if (juice_get_local_description(mAgent, buf, sizeof(buf)) != JUICE_ERR_SUCCESS) {
		if (err != nullptr) *err = "juice_get_local_description failed";
		return false;
	}
	*out = buf;
	return true;
}

bool IceSocket::apply_remote(const std::string& sdp, bool turnOnly, std::string* err)
{
	const std::string use = turnOnly ? ice_filter_relay_candidates(sdp) : sdp;
	// The connect clock starts once both descriptions are known, not at agent
	// creation (which includes the human copy-paste).
	mConnectStartMs = now_ms();
	if (juice_set_remote_description(mAgent, use.c_str()) != JUICE_ERR_SUCCESS) {
		if (err != nullptr) *err = "juice_set_remote_description failed";
		return false;
	}
	// The codes carry every candidate (no trickle).
	juice_set_remote_gathering_done(mAgent);
	return true;
}

namespace {
double ice_gather_timeout_ms()
{
	const char* v = getenv_nonempty("PIKMIN_NETPLAY_ICE_GATHER_TIMEOUT_MS");
	if (v != nullptr) {
		char* end = nullptr;
		double n  = strtod(v, &end);
		if (end != v && *end == '\0' && n >= 1000 && n <= 600000) return n;
	}
	return 15000.0;
}
} // namespace

bool IceSocket::gather(const IceNetConfig& cfg, std::string* sdpOut, std::string* err, std::function<void()> pump)
{
	if (juice_gather_candidates(mAgent) != JUICE_ERR_SUCCESS) {
		if (err != nullptr)
			*err = "juice_gather_candidates failed (port range " + std::to_string(cfg.portBegin) + "-"
			     + std::to_string(cfg.portEnd) + (cfg.bindAddress.empty() ? "" : ", bind " + cfg.bindAddress) + ")";
		return false;
	}
	if (!wait_gathering_done(ice_gather_timeout_ms(), err, pump)) return false;
	if (!local_description(sdpOut, err)) return false;
	if (cfg.turnOnly) {
		if (!ice_sdp_has_relay(*sdpOut)) {
			if (err != nullptr)
				*err = "TURN-only mode gathered no relay candidates; is the TURN server reachable "
				       "(PIKMIN_NETPLAY_TURN)?";
			return false;
		}
		*sdpOut = ice_filter_relay_candidates(*sdpOut);
		printf("[netplay] ice: TURN-only mode, host candidates filtered out\n");
		fflush(stdout);
	} else if (mSdpHook) {
		mSdpHook(*sdpOut);
	}
	return true;
}

bool IceSocket::host_create_offer(const IceNetConfig& cfg, const SessionBundle& bundle, std::string* offerOut,
                                  std::string* err, std::function<void()> pump)
{
	if (!create_agent(cfg, err)) return false;
	std::string sdp;
	if (!gather(cfg, &sdp, err, pump)) return false;
	return ice_encode_v3(true, sdp, &bundle, offerOut, err);
}

bool IceSocket::join_create_answer(const IceNetConfig& cfg, const std::string& offer, std::string* answerOut,
                                   std::string* err, std::function<void()> pump, SessionBundle* bundleOut)
{
	std::string offerSdp;
	SessionBundle bundle;
	if (!ice_decode_any_offer(offer, &offerSdp, &bundle, err)) return false;
	if (bundleOut != nullptr) *bundleOut = bundle;
	// The remote description goes in before gathering, so this side takes the
	// controlled role (RFC 8445 6.1.1); the host does the reverse.
	if (!create_agent(cfg, err)) return false;
	if (!apply_remote(offerSdp, cfg.turnOnly, err)) return false;
	std::string sdp;
	if (!gather(cfg, &sdp, err, pump)) return false;
	return ice_encode_v3(false, sdp, nullptr, answerOut, err);
}

bool IceSocket::host_apply_answer(const std::string& answer, std::string* err)
{
	std::string sdp;
	if (!ice_decode_any_answer(answer, &sdp, err)) return false;
	return apply_remote(sdp, mTurnOnly, err);
}

bool IceSocket::wait_connected(double timeoutMs, double* completedMsOut, std::string* err, std::function<void()> pump)
{
	const double start = now_ms();
	double lastPump    = 0;
	while (true) {
		int st = 0;
		{
			std::lock_guard<std::mutex> lock(*(std::mutex*)mMutex);
			if (mCompletedMs >= 0) {
				// TURN-only must select a relay candidate locally, or the direct
				// path leaked through and the run proves nothing.
				if (mTurnOnly && !ice_selected_local_is_relay(mSelectedLocal)) {
					if (err != nullptr)
						*err = std::string("TURN-only violated: selected local is not relay: ")
						     + (mSelectedLocal.empty() ? "<unknown>" : mSelectedLocal);
					return false;
				}
				if (completedMsOut != nullptr) *completedMsOut = mCompletedMs;
				return true;
			}
			st = mState;
		}
		if (st == (int)JUICE_STATE_FAILED) {
			if (err != nullptr) *err = "ICE failed: no working candidate pair";
			return false;
		}
		if (now_ms() - start > timeoutMs) {
			if (err != nullptr) *err = "ICE connect timed out";
			return false;
		}
		if (pump && now_ms() - lastPump > 50) {
			pump();
			lastPump = now_ms();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

bool IceSocket::send_payload(uint8_t channel, const uint8_t* data, size_t len)
{
	if (mAgent == nullptr || len + 1 > kMaxDatagram) return false;
	if (len > 0 && data == nullptr) return false;
	uint8_t frame[kMaxDatagram];
	frame[0] = channel;
	if (len > 0) memcpy(frame + 1, data, len);
	return juice_send(mAgent, (const char*)frame, len + 1) == JUICE_ERR_SUCCESS;
}

bool IceSocket::send_to(uint8_t channel, const uint8_t* data, size_t len, uint32_t /*ip*/, uint16_t /*port*/)
{
	return send_payload(channel, data, len);
}

std::vector<IceSocket::Datagram> IceSocket::recv()
{
	std::vector<Datagram> out;
	if (mMutex == nullptr) return out;
	std::lock_guard<std::mutex> lock(*(std::mutex*)mMutex);
	const size_t n = mQueue.size() < (size_t)kMaxRecvPerPoll ? mQueue.size() : (size_t)kMaxRecvPerPoll;
	out.reserve(n);
	for (size_t i = 0; i < n; ++i) out.push_back(std::move(mQueue[i]));
	mQueue.erase(mQueue.begin(), mQueue.begin() + (ptrdiff_t)n);
	return out;
}

std::string IceSocket::selected_local() const
{
	if (mMutex == nullptr) return std::string();
	std::lock_guard<std::mutex> lock(*(std::mutex*)mMutex);
	return mSelectedLocal;
}

std::string IceSocket::selected_remote() const
{
	if (mMutex == nullptr) return std::string();
	std::lock_guard<std::mutex> lock(*(std::mutex*)mMutex);
	return mSelectedRemote;
}

std::string IceSocket::state_str() const
{
	if (mMutex == nullptr) return "closed";
	std::lock_guard<std::mutex> lock(*(std::mutex*)mMutex);
	if (mAgent == nullptr) return "closed";
	return juice_state_to_string((juice_state_t)mState);
}

bool IceSocket::failed() const
{
	if (mMutex == nullptr) return false;
	std::lock_guard<std::mutex> lock(*(std::mutex*)mMutex);
	return mState == (int)JUICE_STATE_FAILED;
}

double IceSocket::completed_ms() const
{
	if (mMutex == nullptr) return -1;
	std::lock_guard<std::mutex> lock(*(std::mutex*)mMutex);
	return mCompletedMs;
}

// ---- IceLink ----

namespace {
IceLink* g_ice_link = nullptr;

void ice_send_trampoline(GekkoNetAddress* addr, const char* data, int length)
{
	if (g_ice_link == nullptr || addr == nullptr || data == nullptr || length <= 0) return;
	g_ice_link->send_to_peer(0x7F000001, 1, (const uint8_t*)data, (size_t)length);
}

GekkoNetResult** ice_receive_trampoline(int* length)
{
	if (length == nullptr) return nullptr;
	if (g_ice_link == nullptr) {
		*length = 0;
		return nullptr;
	}
	return g_ice_link->receive_inner(length);
}

void ice_free_trampoline(void* data_ptr) { free(data_ptr); }

constexpr size_t kMaxPendingQueue = 512;
} // namespace

IceLink::IceLink(IceSocket* sock) : mSock(sock)
{
	mAdapter.send_data    = ice_send_trampoline;
	mAdapter.receive_data = ice_receive_trampoline;
	mAdapter.free_data    = ice_free_trampoline;
	g_ice_link            = this;
}

IceLink::~IceLink()
{
	if (g_ice_link == this) g_ice_link = nullptr;
}

GekkoNetAdapter* IceLink::adapter() { return &mAdapter; }

void IceLink::send_to_peer(uint32_t /*ip*/, uint16_t /*port*/, const uint8_t* data, size_t len)
{
	if (mSock != nullptr) mSock->send_payload(kChannelGekko, data, len);
}

void IceLink::route(std::vector<IceSocket::Datagram>& grams)
{
	for (IceSocket::Datagram& g : grams) {
		if (g.channel == kChannelGekko) mGekkoPending.push_back(std::move(g));
		else if (g.channel == kChannelHandshake) mHandshakePending.push_back(std::move(g));
	}
	auto cap = [](std::vector<IceSocket::Datagram>& q) {
		if (q.size() > kMaxPendingQueue) q.erase(q.begin(), q.begin() + (ptrdiff_t)(q.size() - kMaxPendingQueue));
	};
	cap(mGekkoPending);
	cap(mHandshakePending);
}

GekkoNetResult** IceLink::receive_inner(int* length)
{
	mResults.clear();
	if (mSock != nullptr) {
		std::vector<IceSocket::Datagram> grams = mSock->recv();
		route(grams);
	}
	for (IceSocket::Datagram& g : mGekkoPending) {
		const size_t len = g.payload.size();
		if (len > kMaxDatagram - 1) continue;
		GekkoNetResult* res = (GekkoNetResult*)malloc(sizeof(GekkoNetResult));
		uint8_t* addrBuf    = (uint8_t*)malloc(kAddrBytes);
		void* payBuf        = len > 0 ? malloc(len) : nullptr;
		if (res == nullptr || addrBuf == nullptr || (len > 0 && payBuf == nullptr)) {
			free(res);
			free(addrBuf);
			free(payBuf);
			continue;
		}
		// Fixed placeholder 127.0.0.1:1, the blob the session registered.
		const uint8_t blob[kAddrBytes] = { 127, 0, 0, 1, 0, 1 };
		memcpy(addrBuf, blob, kAddrBytes);
		if (len > 0) memcpy(payBuf, g.payload.data(), len);
		res->addr.data = addrBuf;
		res->addr.size = (unsigned)kAddrBytes;
		res->data_len  = (unsigned)len;
		res->data      = payBuf;
		mResults.push_back(res);
	}
	mGekkoPending.clear();
	*length = (int)mResults.size();
	return mResults.empty() ? nullptr : mResults.data();
}

std::vector<IceSocket::Datagram> IceLink::drain_handshake()
{
	if (mSock != nullptr) {
		std::vector<IceSocket::Datagram> grams = mSock->recv();
		route(grams);
	}
	std::vector<IceSocket::Datagram> out;
	out.swap(mHandshakePending);
	return out;
}

// ---- waits ----

double ice_connect_timeout_ms()
{
	const char* v = getenv_nonempty("PIKMIN_NETPLAY_ICE_TIMEOUT_MS");
	if (v != nullptr) {
		char* end = nullptr;
		double n  = strtod(v, &end);
		if (end != v && *end == '\0' && n >= 1000 && n <= 1800000) return n;
	}
	return 600000.0;
}

bool ice_poll_answer_file(const std::string& path, double timeoutMs, std::function<void()> pump,
                          std::string* answerOut, std::string* err)
{
	const double start = now_ms();
	double lastPump    = 0;
	std::string lastErr, lastSeen;
	while (true) {
		FILE* f = fopen(path.c_str(), "rb");
		if (f != nullptr) {
			std::string raw;
			char chunk[4096];
			while (raw.size() <= 65536) {
				size_t n = fread(chunk, 1, sizeof(chunk), f);
				if (n > 0) raw.append(chunk, n);
				if (n < sizeof(chunk)) break;
			}
			fclose(f);
			std::string t = trim(raw);
			if (!t.empty()) {
				std::string sdp, derr;
				if (ice_decode_any_answer(t, &sdp, &derr)) {
					*answerOut = t;
					return true;
				}
				if (t != lastSeen) {
					lastSeen = t;
					lastErr  = derr;
					printf("[netplay] ice: answer file not yet valid (%s), waiting\n", derr.c_str());
					fflush(stdout);
				}
			}
		}
		if (now_ms() - start > timeoutMs) {
			if (err != nullptr)
				*err = "timed out waiting for the answer code in " + path
				     + (lastErr.empty() ? "" : " (last decode: " + lastErr + ")");
			return false;
		}
		if (pump && now_ms() - lastPump > 50) {
			pump();
			lastPump = now_ms();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

bool ice_read_answer_stdin(std::function<void()> pump, std::string* answerOut, std::string* err)
{
	printf("[netplay] ice: paste the joiner's answer code, then Enter:\n");
	fflush(stdout);
	// stdin blocks for as long as the human takes, so it is read on a helper
	// thread while this thread keeps pumping the window. A bad line prints
	// why and the prompt repeats; only EOF ends the wait.
	const size_t kMaxAnswerChars = 16390;
	std::string answer;
	std::string lastErr;
	std::atomic<bool> done(false);
	std::thread reader([&]() {
		while (true) {
			std::string acc;
			int c         = 0;
			bool overflow = false;
			while ((c = getchar()) != EOF && c != '\n') {
				if (acc.size() >= kMaxAnswerChars) {
					overflow = true;
					continue;
				}
				acc.push_back((char)c);
			}
			const std::string line = trim(acc);
			if (overflow) {
				lastErr = "bad ICE code: pasted line longer than " + std::to_string(kMaxAnswerChars) + " characters";
			} else if (!line.empty()) {
				std::string sdp, derr;
				if (!ice_decode_any_answer(line, &sdp, &derr)) {
					lastErr = derr;
				} else {
					answer = line;
					done   = true;
					return;
				}
			}
			if (c == EOF) {
				done = true;
				return;
			}
			if (!line.empty() || overflow) {
				printf("[netplay] ice: %s\n[netplay] ice: paste the joiner's answer code again, then Enter:\n",
				       lastErr.c_str());
				fflush(stdout);
			}
		}
	});
	while (!done) {
		if (pump) pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	reader.join();
	if (answer.empty()) {
		if (err != nullptr)
			*err = lastErr.empty() ? std::string("no answer code on stdin")
			                       : "no valid answer code on stdin (last: " + lastErr + ")";
		return false;
	}
	*answerOut = answer;
	return true;
}

} // namespace pc_netplay_ice
