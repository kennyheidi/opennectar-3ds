#pragma once
// Netplay handshake Hello: what the two peers must agree on before the
// lockstep session starts. Engine-free and header-only (the C++ standard
// library only), so pc_netplay_transfer_test links nothing else. Every field
// is little-endian.
//
// Hello (124 bytes). The 7-byte header is stable across protocol versions,
// so a peer of another version still refuses `protocol` fast:
//   0   magic "NPHU"            4
//   4   type (1 Hello, 2 Ack, 3 Refuse)
//   5   protocol u16
//   7   build manifest SHA-256  32  (platform-neutral source/simulation identity)
//   39  config SHA-256          32  (the session config text, see the session)
//   71  netplay seed u32
//   75  refuse field u8
//   76  nonce u64               Hello: fresh per send; Ack: the echoed Hello nonce
//   84  mode u8                 kModeCoopDirect or kModeVs
//   85  captain P1 u8
//   86  captain P2 u8
//   87  reserved u8[5]          zero
//   92  VS map digest[32]       SHA-256 of the compiled VS map (.mod + .ini); zero in co-op
//   124 end

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pc_netplay_xfer {

constexpr char kHsMagic[4]       = { 'N', 'P', 'H', 'U' };
// v3: platform-neutral build manifest in the Hello instead of the exe hash,
// and the 16-byte input carrying the ext bits and the cursor rotate (#93).
constexpr uint16_t kProtocolV3   = 3;
constexpr size_t kHsHeaderLen    = 7;
constexpr size_t kHsLen          = 124;
constexpr size_t kHsRefuseOffset = 75;
constexpr uint8_t kHsHello       = 1;
constexpr uint8_t kHsAck         = 2;
constexpr uint8_t kHsRefuse      = 3;

constexpr uint8_t kFieldProto  = 1;
constexpr uint8_t kFieldBuild  = 2;
constexpr uint8_t kFieldConfig = 3;
constexpr uint8_t kFieldSeed   = 4;
constexpr uint8_t kFieldMode   = 5;
constexpr uint8_t kFieldVsMap  = 6;
constexpr uint8_t kFieldCaptain = 7; // refuse only: a captain this PC cannot load

constexpr uint8_t kModeNone       = 0;
constexpr uint8_t kModeCoopDirect = 1; // 2-captain story co-op, direct boot, fresh save
constexpr uint8_t kModeVs         = 2; // 1v1 VS on a VS map, --vs-map direct boot

inline const char* field_name(uint8_t f)
{
	switch (f) {
	case kFieldProto: return "protocol";
	case kFieldBuild: return "build";
	case kFieldConfig: return "config";
	case kFieldSeed: return "seed";
	case kFieldMode: return "mode";
	case kFieldVsMap: return "vsmap";
	case kFieldCaptain: return "captain";
	default: return "unknown";
	}
}

inline const char* mode_name(uint8_t m)
{
	switch (m) {
	case kModeCoopDirect: return "coop-direct";
	case kModeVs: return "vs";
	default: return "none";
	}
}

struct ModeDesc {
	uint8_t mode        = kModeNone;
	uint8_t captainP1   = 0;
	uint8_t captainP2   = 1;
	uint8_t reserved[5] = {};

	bool operator==(const ModeDesc& o) const
	{
		return mode == o.mode && captainP1 == o.captainP1 && captainP2 == o.captainP2
		    && std::memcmp(reserved, o.reserved, sizeof(reserved)) == 0;
	}
	bool operator!=(const ModeDesc& o) const { return !(*this == o); }
};

struct Hello {
	uint8_t build[32]   = {};
	uint8_t cfg[32]     = {};
	uint32_t seed       = 0;
	uint64_t nonce      = 0;
	ModeDesc mode;
	uint8_t vsMapSha[32] = {};
};

inline void put_u16(uint8_t* p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)(v >> 8);
}
inline void put_u32(uint8_t* p, uint32_t v)
{
	for (int b = 0; b < 4; ++b) p[b] = (uint8_t)((v >> (b * 8)) & 0xFF);
}
inline void put_u64(uint8_t* p, uint64_t v)
{
	for (int b = 0; b < 8; ++b) p[b] = (uint8_t)((v >> (b * 8)) & 0xFF);
}
inline uint16_t get_u16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
inline uint32_t get_u32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t get_u64(const uint8_t* p)
{
	uint64_t v = 0;
	for (int b = 0; b < 8; ++b) v |= (uint64_t)p[b] << (b * 8);
	return v;
}

inline void encode_hello(uint8_t type, uint16_t proto, const Hello& h, uint8_t refuse, uint8_t out[kHsLen])
{
	std::memcpy(out, kHsMagic, 4);
	out[4] = type;
	put_u16(out + 5, proto);
	std::memcpy(out + 7, h.build, 32);
	std::memcpy(out + 39, h.cfg, 32);
	put_u32(out + 71, h.seed);
	out[kHsRefuseOffset] = refuse;
	put_u64(out + 76, h.nonce);
	out[84] = h.mode.mode;
	out[85] = h.mode.captainP1;
	out[86] = h.mode.captainP2;
	std::memcpy(out + 87, h.mode.reserved, 5);
	std::memcpy(out + 92, h.vsMapSha, 32);
}

// Stable header parse, valid for any version length.
inline bool decode_hello_header(const uint8_t* p, size_t len, uint8_t* type, uint16_t* proto)
{
	if (p == nullptr || len < kHsHeaderLen || std::memcmp(p, kHsMagic, 4) != 0) return false;
	*type  = p[4];
	*proto = get_u16(p + 5);
	return true;
}

// Full parse: exact length only.
inline bool decode_hello(const uint8_t* p, size_t len, uint8_t* type, uint16_t* proto, Hello* h, uint8_t* refuse)
{
	if (len != kHsLen || !decode_hello_header(p, len, type, proto)) return false;
	std::memcpy(h->build, p + 7, 32);
	std::memcpy(h->cfg, p + 39, 32);
	h->seed           = get_u32(p + 71);
	*refuse           = p[kHsRefuseOffset];
	h->nonce          = get_u64(p + 76);
	h->mode.mode      = p[84];
	h->mode.captainP1 = p[85];
	h->mode.captainP2 = p[86];
	std::memcpy(h->mode.reserved, p + 87, 5);
	std::memcpy(h->vsMapSha, p + 92, 32);
	return true;
}

// The first field on which a peer's Hello differs from ours, in wire order
// (0 when they agree). The nonce differs per send by design and is skipped.
inline uint8_t first_mismatch(const Hello& mine, const Hello& peer)
{
	if (std::memcmp(mine.build, peer.build, 32) != 0) return kFieldBuild;
	if (std::memcmp(mine.cfg, peer.cfg, 32) != 0) return kFieldConfig;
	if (mine.seed != peer.seed) return kFieldSeed;
	if (mine.mode != peer.mode) return kFieldMode;
	if (std::memcmp(mine.vsMapSha, peer.vsMapSha, 32) != 0) return kFieldVsMap;
	return 0;
}

} // namespace pc_netplay_xfer
