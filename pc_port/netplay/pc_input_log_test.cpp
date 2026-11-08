// Host-run unit test for the PKNI v1/v2 encode/decode API.
// Round-trip (v2 and v1), v1 back-compat, truncated file, bad magic.
// Engine-free: links only pc_input_log.cpp's pure functions (no PADStatus,
// no globals).

#include "netplay/pc_input_log.h"

#include "Dolphin/pad.h"
#include "netplay/pc_state_hash.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

// Link stubs: this test exercises only the pure encode/decode API, but it
// links pc_input_log.cpp whole, so the engine hooks it calls need symbols.
// They are never invoked below.
static PADStatus sStubPads[4];
PADStatus* pc_netplay_pad_status(void) { return sStubPads; }
void pc_state_hash_before_first_tick(void) {}

// Hook probe: counts pre-sim capture calls and stores a fixed yaw.
static int sHookCalls = 0;
static void sTestCaptureHook(void)
{
	++sHookCalls;
	pc_input_log_yaw_set(0, 0x1234, 0);
}

// Probe: mirrors the production Navi hook (navi.cpp), which leaves slots
// already marked valid alone and fills the rest from the live camera.
static void sTestCaptureHookChecked(void)
{
	++sHookCalls;
	if (pc_input_log_yaw_valid(0)) return;
	pc_input_log_yaw_set(0, 0x1234, 0);
}

namespace {
int sFailures = 0;

void check(bool cond, const char* what)
{
	if (!cond) {
		std::printf("FAIL: %s\n", what);
		++sFailures;
	}
}

void fillPads(PcInputPad pads[4])
{
	// Cover edges: full button word, stick extremes, trigger extremes,
	// both err signs, yaw extremes, flags.
	const uint16_t buttons[4] = { 0x0000, 0xFFFF, 0x0100 | 0x0040, 0x1000 };
	const int8_t sticks[4][4] = { { 0, 0, 0, 0 }, { 127, 127, -128, -128 }, { -1, 1, 100, -100 }, { 32, -33, 0, 7 } };
	const uint16_t yaws[4]    = { 0x0000, 0xFFFF, 0x1234, 0x8000 };
	for (int p = 0; p < 4; ++p) {
		pads[p].button       = buttons[p];
		pads[p].stickX       = sticks[p][0];
		pads[p].stickY       = sticks[p][1];
		pads[p].substickX    = sticks[p][2];
		pads[p].substickY    = sticks[p][3];
		pads[p].triggerLeft  = (uint8_t)(p * 85);
		pads[p].triggerRight = (uint8_t)(255 - p * 85);
		pads[p].analogA      = (uint8_t)(17 + p);
		pads[p].analogB      = (uint8_t)(200 - p);
		pads[p].err          = (p == 0) ? (int8_t)0 : (int8_t)-1;
		pads[p].controlYaw   = yaws[p];
		pads[p].flags        = (uint8_t)(p & 1);
		pads[p].cursorRotate = (int16_t)(p * 1000 - 1500);
	}
}

bool padsEqual(const PcInputPad a[4], const PcInputPad b[4], bool withYaw)
{
	// Field-wise: PcInputPad may carry padding bytes that encode/decode
	// never touches.
	for (int p = 0; p < 4; ++p) {
		if (a[p].button != b[p].button || a[p].stickX != b[p].stickX || a[p].stickY != b[p].stickY
		    || a[p].substickX != b[p].substickX || a[p].substickY != b[p].substickY
		    || a[p].triggerLeft != b[p].triggerLeft || a[p].triggerRight != b[p].triggerRight
		    || a[p].analogA != b[p].analogA || a[p].analogB != b[p].analogB || a[p].err != b[p].err) {
			return false;
		}
		if (withYaw && (a[p].controlYaw != b[p].controlYaw || a[p].flags != b[p].flags)) return false;
	}
	return true;
}
} // namespace

int main()
{
	// v2 round-trip through encode/decode.
	PcInputPad pads[4];
	fillPads(pads);
	uint8_t rec[56];
	check(pc_input_log_encode_tick(pads, rec) == 56, "v2 encode returns 56 bytes");
	PcInputPad back[4];
	std::memset(back, 0xAA, sizeof(back));
	check(pc_input_log_decode_tick(rec, sizeof(rec), back), "v2 decode of full record succeeds");
	check(padsEqual(pads, back, true), "v2 round-trip preserves every field incl yaw/flags");

	// Byte layout is explicit little-endian, not a struct dump.
	check(rec[0] == 0x00 && rec[1] == 0x00, "pad0 button LE");
	PcInputPad hi[4];
	std::memset(hi, 0, sizeof(hi));
	hi[1].button     = 0x0100;
	hi[1].controlYaw = 0x0201;
	hi[1].flags      = 0x03;
	uint8_t recHi[56];
	pc_input_log_encode_tick(hi, recHi);
	check(recHi[14] == 0x00 && recHi[15] == 0x01, "button serialised LE, not host order dependent");
	check(recHi[16] == 0 && recHi[14 + 14] == 0, "sticks at fixed offsets");
	// Pad 1 starts at byte 14: 11 PAD bytes, then yaw LE, then flags.
	check(recHi[14 + 11] == 0x01 && recHi[14 + 12] == 0x02, "yaw serialised LE after the 11 PAD bytes");
	check(recHi[14 + 13] == 0x03, "flags byte follows yaw");

	// Truncated file: decode must fail for every short length.
	for (size_t len = 0; len < 56; ++len) {
		PcInputPad tmp[4];
		char what[64];
		std::snprintf(what, sizeof(what), "v2 decode rejects %llu bytes", (unsigned long long)len);
		check(!pc_input_log_decode_tick(rec, len, tmp), what);
	}
	check(!pc_input_log_decode_tick(nullptr, 56, back), "decode rejects null data");
	check(!pc_input_log_decode_tick(rec, 56, nullptr), "decode rejects null pads");

	// v1 compat: encode ignores yaw/flags, decode zeroes them.
	uint8_t recV1[44];
	check(pc_input_log_encode_tick_v1(pads, recV1) == 44, "v1 encode returns 44 bytes");
	PcInputPad backV1[4];
	std::memset(backV1, 0xAA, sizeof(backV1));
	check(pc_input_log_decode_tick_v1(recV1, sizeof(recV1), backV1), "v1 decode succeeds");
	check(padsEqual(pads, backV1, false), "v1 round-trip preserves the 11 PAD fields");
	for (int p = 0; p < 4; ++p) {
		char what[64];
		std::snprintf(what, sizeof(what), "v1 decode zeroes yaw/flags pad %d", p);
		check(backV1[p].controlYaw == 0 && backV1[p].flags == 0, what);
	}
	for (size_t len = 0; len < 44; ++len) {
		PcInputPad tmp[4];
		check(!pc_input_log_decode_tick_v1(recV1, len, tmp), "v1 decode rejects short");
	}

	// v3: the v2 14 bytes per pad plus the cursor rotate s16 LE.
	uint8_t recV3[64];
	check(pc_input_log_encode_tick_v3(pads, recV3) == 64, "v3 encode returns 64 bytes");
	check(std::memcmp(recV3, rec, 14) == 0, "v3 pad 0 starts with the v2 pad bytes");
	check(recV3[14] == (uint8_t)(-1500 & 0xFF) && recV3[15] == (uint8_t)((-1500 >> 8) & 0xFF), "v3 rotate LE");
	PcInputPad backV3[4];
	std::memset(backV3, 0xAA, sizeof(backV3));
	check(pc_input_log_decode_tick_v3(recV3, sizeof(recV3), backV3), "v3 decode succeeds");
	check(padsEqual(pads, backV3, true), "v3 round-trip preserves the v2 fields");
	for (int p = 0; p < 4; ++p) check(backV3[p].cursorRotate == pads[p].cursorRotate, "v3 round-trip keeps the rotate");
	check(!pc_input_log_decode_tick_v3(recV3, 63, backV3), "v3 decode rejects short");
	PcInputPad backV2[4];
	pc_input_log_decode_tick(rec, sizeof(rec), backV2);
	check(backV2[3].cursorRotate == 0, "v2 decode zeroes the rotate");
	std::vector<uint8_t> headerV3;
	pc_input_log_write_header_v3(headerV3);
	uint16_t v3v = 0, v3p = 0, v3r = 0;
	check(pc_input_log_read_header(headerV3.data(), headerV3.size(), v3v, v3p, v3r) == PC_INPUT_HEADER_OK && v3v == 3
	          && v3p == 4 && v3r == 64,
	      "v3 header reads back v3/4/64");

	// Ext slots and the local rotate accumulator.
	pc_input_log_ext_set(1, pc_input_log::kExtBomb | pc_input_log::kExtSwarm, -42);
	check(pc_netplay_ext_bits(1) == 0x09 && pc_netplay_ext_cursor_rotate(1) == -42 && pc_netplay_ext_bits(0) == 0,
	      "ext slots per pad");
	pc_input_log_ext_set(1, 0, 0);
	const float quarterLsb = pc_input_log::kYawTurnsToRad * 0.25f;
	pc_input_log_local_rotate_add(quarterLsb);
	check(pc_input_log_local_rotate_take() == 0, "a quarter LSB rounds to 0");
	pc_input_log_local_rotate_add(quarterLsb);
	pc_input_log_local_rotate_add(quarterLsb);
	check(pc_input_log_local_rotate_take() == 1, "the remainder carries to the next take");
	pc_input_log_local_rotate_add(-1.0f);
	const int16_t q = pc_input_log_local_rotate_take();
	check(q <= -10429 && q >= -10431, "one radian is about 10430 LSB");

	// Header round-trip (v2 default).
	std::vector<uint8_t> header;
	pc_input_log_write_header(header);
	check(header.size() == 10, "header is 10 bytes");
	check(header[0] == 'P' && header[1] == 'K' && header[2] == 'N' && header[3] == 'I', "header magic");
	uint16_t version = 0, padCount = 0, recordSize = 0;
	check(pc_input_log_read_header(header.data(), header.size(), version, padCount, recordSize)
	              == PC_INPUT_HEADER_OK
	      && version == 2 && padCount == 4 && recordSize == 56, "header reads back v2/4/56");

	// v1 header still reads.
	std::vector<uint8_t> headerV1;
	pc_input_log_write_header_v1(headerV1);
	check(pc_input_log_read_header(headerV1.data(), headerV1.size(), version, padCount, recordSize)
	              == PC_INPUT_HEADER_OK
	      && version == 1 && padCount == 4 && recordSize == 44, "v1 header reads back v1/4/44");

	// Bad magic.
	std::vector<uint8_t> bad = header;
	bad[0]                 = 'X';
	check(pc_input_log_read_header(bad.data(), bad.size(), version, padCount, recordSize)
	          == PC_INPUT_HEADER_BAD_MAGIC,
	      "bad magic rejected");

	// Truncated header.
	for (size_t len = 0; len < 10; ++len) {
		check(pc_input_log_read_header(header.data(), len, version, padCount, recordSize)
		              == PC_INPUT_HEADER_TRUNCATED,
		      "short header rejected");
	}

	// Unsupported version / pad count / record size.
	std::vector<uint8_t> wrong = header;
	wrong[4]                   = 4; // version 4
	check(pc_input_log_read_header(wrong.data(), wrong.size(), version, padCount, recordSize)
	          == PC_INPUT_HEADER_UNSUPPORTED,
	      "future version rejected");
	wrong = header;
	wrong[6] = 2; // pad count 2
	check(pc_input_log_read_header(wrong.data(), wrong.size(), version, padCount, recordSize)
	          == PC_INPUT_HEADER_UNSUPPORTED,
	      "wrong pad count rejected");
	wrong = header;
	wrong[8] = 44; // v2 version with v1 size
	check(pc_input_log_read_header(wrong.data(), wrong.size(), version, padCount, recordSize)
	          == PC_INPUT_HEADER_UNSUPPORTED,
	      "version/size mismatch rejected");

	// Yaw quantisation: round-trip through angle is within half an LSB,
	// and known angles map to known codes.
	check(pc_input_log_yaw_quantise(0.0f) == 0, "yaw 0 -> 0");
	check(pc_input_log_yaw_quantise(6.28318530717958647692f) == 0, "yaw 2pi wraps to 0");
	// Non-finite camera input maps to neutral 0 instead of UB (m7).
	check(pc_input_log_yaw_quantise(std::numeric_limits<float>::quiet_NaN()) == 0, "yaw NaN -> 0");
	check(pc_input_log_yaw_quantise(std::numeric_limits<float>::infinity()) == 0, "yaw inf -> 0");
	check(pc_input_log_yaw_quantise(-std::numeric_limits<float>::infinity()) == 0, "yaw -inf -> 0");

	// The pre-sim capture hook runs and the yaw slots reset every
	// tick even with no record or replay file (det mode without files must
	// get a fresh yaw each tick, never a stale one). Needs
	// PIKMIN_INPUT_RECORD/PIKMIN_INPUT_REPLAY unset in the test env.
	{
		float s = 0.0f, c = 0.0f;
		check(!pc_netplay_control_yaw(0, &s, &c), "no yaw before the first tick");
		pc_input_log_set_yaw_capture_fn(&sTestCaptureHook);
		pc_input_log_tick();
		check(sHookCalls == 1, "capture hook runs without record/replay");
		check(pc_input_log_yaw_valid(0), "hook yaw stored for pad 0");
		check(pc_netplay_control_yaw(0, &s, &c), "stored yaw consumable after tick");
		// With the hook removed the slots must still clear every tick.
		pc_input_log_set_yaw_capture_fn(nullptr);
		pc_input_log_tick();
		check(sHookCalls == 1, "removed hook is not called");
		check(!pc_input_log_yaw_valid(0), "yaw cleared every tick without record/replay");
	}
	// The fresh capture clears stale injected yaw before running the
	// hook, so the lockstep session gets the live camera yaw every submit.
	{
		pc_input_log_set_yaw_capture_fn(&sTestCaptureHookChecked);
		pc_input_log_yaw_set(0, 0x5555, 0);
		check(pc_input_log_yaw_raw(0) == 0x5555, "injected yaw stored");
		pc_input_log_capture_yaw(); // Navi-style hook skips valid slots
		check(pc_input_log_yaw_raw(0) == 0x5555, "plain capture keeps stale injected yaw");
		pc_input_log_capture_yaw_fresh();
		check(pc_input_log_yaw_raw(0) == 0x1234, "fresh capture refreshes from the hook");
		check(sHookCalls == 3, "fresh capture runs the hook");
		pc_input_log_set_yaw_capture_fn(nullptr);
		pc_input_log_tick();
	}
	{
		const float pi = 3.14159265358979323846f;
		check(pc_input_log_yaw_quantise(pi) == 0x8000, "yaw pi -> 0x8000");
		float s = 0.0f, c = 0.0f;
		pc_input_log_yaw_sincos(0x4000, &s, &c);
		check(std::fabs(s - 1.0f) < 1e-5f && std::fabs(c) < 1e-4f, "yaw 0x4000 is +90deg");
		pc_input_log_yaw_sincos(0, &s, &c);
		check(std::fabs(s) < 1e-4f && std::fabs(c - 1.0f) < 1e-5f, "yaw 0 is 0deg");
		// Negative angles wrap: -pi/2 == 3pi/2.
		check(pc_input_log_yaw_quantise(-pi / 2.0f) == 0xC000, "yaw -pi/2 wraps to 0xC000");
	}

	if (sFailures == 0) {
		std::printf("pc_input_log_test: all checks passed\n");
		return 0;
	}
	std::printf("pc_input_log_test: %d failure(s)\n", sFailures);
	return 1;
}
