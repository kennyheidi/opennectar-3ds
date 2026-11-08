// Host-run test for the netplay handshake Hello codec (pc_netplay_transfer.h).

#include "netplay/pc_netplay_transfer.h"

#include <cstdio>
#include <cstring>

namespace {

int sFailures = 0;

void check(bool ok, const char* what)
{
	if (!ok) {
		std::printf("FAIL: %s\n", what);
		++sFailures;
	}
}

pc_netplay_xfer::Hello sample()
{
	pc_netplay_xfer::Hello h;
	for (int i = 0; i < 32; ++i) {
		h.build[i]    = (uint8_t)(i + 1);
		h.cfg[i]      = (uint8_t)(0x80 + i);
		h.vsMapSha[i] = (uint8_t)(0x40 + i);
	}
	h.seed           = 0xA1B2C3D4u;
	h.nonce          = 0x0102030405060708ull;
	h.mode.mode      = pc_netplay_xfer::kModeCoopDirect;
	h.mode.captainP1 = 3;
	h.mode.captainP2 = 9;
	return h;
}

} // namespace

int main()
{
	using namespace pc_netplay_xfer;
	const Hello h = sample();
	uint8_t wire[kHsLen];
	encode_hello(kHsAck, kProtocolV3, h, kFieldMode, wire);
	check(std::memcmp(wire, "NPHU", 4) == 0, "magic");
	check(wire[4] == kHsAck && get_u16(wire + 5) == kProtocolV3, "header type and protocol");
	check(wire[kHsRefuseOffset] == kFieldMode, "refuse field offset");

	uint8_t type = 0, refuse = 0;
	uint16_t proto = 0;
	Hello d;
	check(decode_hello(wire, sizeof(wire), &type, &proto, &d, &refuse), "decode");
	check(type == kHsAck && proto == kProtocolV3 && refuse == kFieldMode, "decoded header");
	check(std::memcmp(d.build, h.build, 32) == 0 && std::memcmp(d.cfg, h.cfg, 32) == 0, "decoded digests");
	check(d.seed == h.seed && d.nonce == h.nonce, "decoded seed and nonce");
	check(d.mode == h.mode, "decoded mode descriptor");
	check(std::memcmp(d.vsMapSha, h.vsMapSha, 32) == 0, "decoded VS map digest");
	check(first_mismatch(h, d) == 0, "round trip agrees");

	check(!decode_hello(wire, sizeof(wire) - 1, &type, &proto, &d, &refuse), "short frame refused");
	check(decode_hello_header(wire, kHsHeaderLen, &type, &proto), "header parses from 7 bytes");
	uint8_t bad[kHsLen];
	std::memcpy(bad, wire, sizeof(bad));
	bad[0] = 'X';
	check(!decode_hello_header(bad, sizeof(bad), &type, &proto), "bad magic refused");

	// Every field, in wire order.
	Hello m = h;
	m.build[31] ^= 1;
	check(first_mismatch(h, m) == kFieldBuild, "build mismatch");
	m = h;
	m.cfg[0] ^= 1;
	check(first_mismatch(h, m) == kFieldConfig, "config mismatch");
	m = h;
	m.seed ^= 1;
	check(first_mismatch(h, m) == kFieldSeed, "seed mismatch");
	m = h;
	m.mode.mode = kModeVs;
	check(first_mismatch(h, m) == kFieldMode, "mode mismatch");
	m = h;
	m.mode.captainP2 = 0;
	check(first_mismatch(h, m) == kFieldMode, "captain mismatch");
	m = h;
	m.vsMapSha[5] ^= 1;
	check(first_mismatch(h, m) == kFieldVsMap, "VS map mismatch");
	m = h;
	m.nonce = 99;
	check(first_mismatch(h, m) == 0, "nonce is not compared");
	m = h;
	m.build[0] ^= 1;
	m.seed ^= 1;
	check(first_mismatch(h, m) == kFieldBuild, "first field wins");

	check(std::strcmp(field_name(kFieldConfig), "config") == 0 && std::strcmp(field_name(77), "unknown") == 0,
	      "field names");

	if (sFailures == 0) std::printf("pc_netplay_transfer_test: PASS\n");
	else std::printf("pc_netplay_transfer_test: %d FAILURES\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
