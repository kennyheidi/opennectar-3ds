// Input record/replay for the netplay determinism harness.
// See pc_input_log.h for the file format.

#include "netplay/pc_input_log.h"

#include "netplay/pc_netplay_pad.h"
#include "netplay/pc_state_hash.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

size_t pc_input_log_encode_tick(const PcInputPad pads[4], uint8_t out[56])
{
	size_t at = 0;
	for (int p = 0; p < 4; ++p) {
		out[at++] = (uint8_t)(pads[p].button & 0xFF);
		out[at++] = (uint8_t)((pads[p].button >> 8) & 0xFF);
		out[at++] = (uint8_t)pads[p].stickX;
		out[at++] = (uint8_t)pads[p].stickY;
		out[at++] = (uint8_t)pads[p].substickX;
		out[at++] = (uint8_t)pads[p].substickY;
		out[at++] = pads[p].triggerLeft;
		out[at++] = pads[p].triggerRight;
		out[at++] = pads[p].analogA;
		out[at++] = pads[p].analogB;
		out[at++] = (uint8_t)pads[p].err;
		out[at++] = (uint8_t)(pads[p].controlYaw & 0xFF);
		out[at++] = (uint8_t)((pads[p].controlYaw >> 8) & 0xFF);
		out[at++] = pads[p].flags;
	}
	return at;
}

bool pc_input_log_decode_tick(const uint8_t* data, size_t avail, PcInputPad pads[4])
{
	if (data == nullptr || pads == nullptr) return false;
	if (avail < pc_input_log::kRecordBytes) return false;
	size_t at = 0;
	for (int p = 0; p < 4; ++p) {
		pads[p].button       = (uint16_t)(data[at] | ((uint16_t)data[at + 1] << 8));
		pads[p].stickX       = (int8_t)data[at + 2];
		pads[p].stickY       = (int8_t)data[at + 3];
		pads[p].substickX    = (int8_t)data[at + 4];
		pads[p].substickY    = (int8_t)data[at + 5];
		pads[p].triggerLeft  = data[at + 6];
		pads[p].triggerRight = data[at + 7];
		pads[p].analogA      = data[at + 8];
		pads[p].analogB      = data[at + 9];
		pads[p].err          = (int8_t)data[at + 10];
		pads[p].controlYaw   = (uint16_t)(data[at + 11] | ((uint16_t)data[at + 12] << 8));
		pads[p].flags        = data[at + 13];
		pads[p].cursorRotate = 0;
		at += pc_input_log::kPadBytes;
	}
	return true;
}

size_t pc_input_log_encode_tick_v1(const PcInputPad pads[4], uint8_t out[44])
{
	size_t at = 0;
	for (int p = 0; p < 4; ++p) {
		out[at++] = (uint8_t)(pads[p].button & 0xFF);
		out[at++] = (uint8_t)((pads[p].button >> 8) & 0xFF);
		out[at++] = (uint8_t)pads[p].stickX;
		out[at++] = (uint8_t)pads[p].stickY;
		out[at++] = (uint8_t)pads[p].substickX;
		out[at++] = (uint8_t)pads[p].substickY;
		out[at++] = pads[p].triggerLeft;
		out[at++] = pads[p].triggerRight;
		out[at++] = pads[p].analogA;
		out[at++] = pads[p].analogB;
		out[at++] = (uint8_t)pads[p].err;
	}
	return at;
}

bool pc_input_log_decode_tick_v1(const uint8_t* data, size_t avail, PcInputPad pads[4])
{
	if (data == nullptr || pads == nullptr) return false;
	if (avail < pc_input_log::kRecordBytesV1) return false;
	size_t at = 0;
	for (int p = 0; p < 4; ++p) {
		pads[p].button       = (uint16_t)(data[at] | ((uint16_t)data[at + 1] << 8));
		pads[p].stickX       = (int8_t)data[at + 2];
		pads[p].stickY       = (int8_t)data[at + 3];
		pads[p].substickX    = (int8_t)data[at + 4];
		pads[p].substickY    = (int8_t)data[at + 5];
		pads[p].triggerLeft  = data[at + 6];
		pads[p].triggerRight = data[at + 7];
		pads[p].analogA      = data[at + 8];
		pads[p].analogB      = data[at + 9];
		pads[p].err          = (int8_t)data[at + 10];
		pads[p].controlYaw   = 0;
		pads[p].flags        = 0;
		pads[p].cursorRotate = 0;
		at += pc_input_log::kPadBytesV1;
	}
	return true;
}

size_t pc_input_log_encode_tick_v3(const PcInputPad pads[4], uint8_t out[64])
{
	size_t at = 0;
	for (int p = 0; p < 4; ++p) {
		const PcInputPad one[4] = { pads[p], pads[p], pads[p], pads[p] };
		uint8_t rec[pc_input_log::kRecordBytes];
		pc_input_log_encode_tick(one, rec);
		for (size_t i = 0; i < pc_input_log::kPadBytes; ++i) out[at++] = rec[i];
		out[at++] = (uint8_t)((uint16_t)pads[p].cursorRotate & 0xFF);
		out[at++] = (uint8_t)(((uint16_t)pads[p].cursorRotate >> 8) & 0xFF);
	}
	return at;
}

bool pc_input_log_decode_tick_v3(const uint8_t* data, size_t avail, PcInputPad pads[4])
{
	if (data == nullptr || pads == nullptr) return false;
	if (avail < pc_input_log::kRecordBytesV3) return false;
	for (int p = 0; p < 4; ++p) {
		const uint8_t* in = data + p * pc_input_log::kPadBytesV3;
		uint8_t rec[pc_input_log::kRecordBytes] = {};
		for (size_t i = 0; i < pc_input_log::kPadBytes; ++i) rec[i] = in[i];
		PcInputPad one[4];
		pc_input_log_decode_tick(rec, sizeof(rec), one);
		pads[p]              = one[0];
		pads[p].cursorRotate = (int16_t)(uint16_t)(in[14] | ((uint16_t)in[15] << 8));
	}
	return true;
}

void pc_input_log_write_header(std::vector<uint8_t>& out)
{
	out.push_back(pc_input_log::kMagic[0]);
	out.push_back(pc_input_log::kMagic[1]);
	out.push_back(pc_input_log::kMagic[2]);
	out.push_back(pc_input_log::kMagic[3]);
	out.push_back((uint8_t)(pc_input_log::kVersion & 0xFF));
	out.push_back((uint8_t)((pc_input_log::kVersion >> 8) & 0xFF));
	out.push_back((uint8_t)(pc_input_log::kPadCount & 0xFF));
	out.push_back((uint8_t)((pc_input_log::kPadCount >> 8) & 0xFF));
	const uint16_t rec = (uint16_t)pc_input_log::kRecordBytes;
	out.push_back((uint8_t)(rec & 0xFF));
	out.push_back((uint8_t)((rec >> 8) & 0xFF));
}

void pc_input_log_write_header_v1(std::vector<uint8_t>& out)
{
	out.push_back(pc_input_log::kMagic[0]);
	out.push_back(pc_input_log::kMagic[1]);
	out.push_back(pc_input_log::kMagic[2]);
	out.push_back(pc_input_log::kMagic[3]);
	out.push_back((uint8_t)(pc_input_log::kVersionV1 & 0xFF));
	out.push_back((uint8_t)((pc_input_log::kVersionV1 >> 8) & 0xFF));
	out.push_back((uint8_t)(pc_input_log::kPadCount & 0xFF));
	out.push_back((uint8_t)((pc_input_log::kPadCount >> 8) & 0xFF));
	const uint16_t rec = (uint16_t)pc_input_log::kRecordBytesV1;
	out.push_back((uint8_t)(rec & 0xFF));
	out.push_back((uint8_t)((rec >> 8) & 0xFF));
}

void pc_input_log_write_header_v3(std::vector<uint8_t>& out)
{
	out.push_back(pc_input_log::kMagic[0]);
	out.push_back(pc_input_log::kMagic[1]);
	out.push_back(pc_input_log::kMagic[2]);
	out.push_back(pc_input_log::kMagic[3]);
	out.push_back((uint8_t)(pc_input_log::kVersionV3 & 0xFF));
	out.push_back((uint8_t)((pc_input_log::kVersionV3 >> 8) & 0xFF));
	out.push_back((uint8_t)(pc_input_log::kPadCount & 0xFF));
	out.push_back((uint8_t)((pc_input_log::kPadCount >> 8) & 0xFF));
	const uint16_t rec = (uint16_t)pc_input_log::kRecordBytesV3;
	out.push_back((uint8_t)(rec & 0xFF));
	out.push_back((uint8_t)((rec >> 8) & 0xFF));
}

PcInputHeaderResult pc_input_log_read_header(const uint8_t* data, size_t len, uint16_t& version,
                                             uint16_t& padCount, uint16_t& recordSize)
{
	if (data == nullptr || len < pc_input_log::kHeaderBytes) return PC_INPUT_HEADER_TRUNCATED;
	if (data[0] != pc_input_log::kMagic[0] || data[1] != pc_input_log::kMagic[1]
	    || data[2] != pc_input_log::kMagic[2] || data[3] != pc_input_log::kMagic[3]) {
		return PC_INPUT_HEADER_BAD_MAGIC;
	}
	version    = (uint16_t)(data[4] | ((uint16_t)data[5] << 8));
	padCount   = (uint16_t)(data[6] | ((uint16_t)data[7] << 8));
	recordSize = (uint16_t)(data[8] | ((uint16_t)data[9] << 8));
	const bool v1ok = version == pc_input_log::kVersionV1 && padCount == pc_input_log::kPadCount
	               && recordSize == pc_input_log::kRecordBytesV1;
	const bool v2ok = version == pc_input_log::kVersion && padCount == pc_input_log::kPadCount
	               && recordSize == pc_input_log::kRecordBytes;
	const bool v3ok = version == pc_input_log::kVersionV3 && padCount == pc_input_log::kPadCount
	               && recordSize == pc_input_log::kRecordBytesV3;
	if (!v1ok && !v2ok && !v3ok) return PC_INPUT_HEADER_UNSUPPORTED;
	return PC_INPUT_HEADER_OK;
}

uint16_t pc_input_log_yaw_quantise(float angleRad)
{
	// Non-finite camera axes (never seen, but cheap to guard) map to 0
	// instead of tripping UB in the float->long conversion below.
	if (!std::isfinite(angleRad)) return 0;
	// Map to [0,1) turns, then to 16 bits. floorf keeps negatives exact;
	// +0.5f rounds to nearest (ties away from the quantised grid edge).
	const float turns = angleRad / 6.28318530717958647692f;
	float wrapped     = turns - std::floor(turns);
	// Guard the wrapped==1.0f edge from float error (angle == +2pi*k).
	if (wrapped >= 1.0f) wrapped -= 1.0f;
	if (!std::isfinite(wrapped)) return 0;
	long q = (long)(wrapped * 65536.0f + 0.5f);
	return (uint16_t)(q & 0xFFFF);
}

float pc_input_log_yaw_to_angle(uint16_t yaw) { return (float)yaw * pc_input_log::kYawTurnsToRad; }

void pc_input_log_yaw_sincos(uint16_t yaw, float* sinYaw, float* cosYaw)
{
	// Same sinf/cosf as Matrix4f::makeRotate(angle): the netplay libm covers both.
	const float angle = pc_input_log_yaw_to_angle(yaw);
	if (sinYaw != nullptr) *sinYaw = std::sin(angle);
	if (cosYaw != nullptr) *cosYaw = std::cos(angle);
}

namespace {
// Runtime state. All inert until the first tick resolves the switches.
bool sArgvSeen       = false;
const char* sArgvRecord = nullptr;
const char* sArgvReplay = nullptr;
bool sInitialised    = false;
bool sRecordActive   = false;
bool sReplayActive   = false;
FILE* sRecordFile    = nullptr;
std::vector<uint8_t> sReplayBytes;
size_t sReplayTicks  = 0;
uint16_t sReplayVersion = 0;
size_t sReplayRec    = 0;
uint64_t sTickIndex  = 0;
// Current-tick yaw. Cleared in pc_input_log_tick; filled from
// the v2 file when replaying, or by the pre-sim capture hook (live camera,
// quantised) for slots without a replayed value. The pre-sim record writes
// what the sim will use.
bool sYawValid[4]     = { false, false, false, false };
uint16_t sYawRaw[4]   = { 0, 0, 0, 0 };
uint8_t sYawFlags[4]  = { 0, 0, 0, 0 };
PcYawCaptureFn sYawCaptureFn = nullptr;
// Current-tick ext bits and cursor rotate, cleared with the yaw.
uint8_t sExt[4]    = { 0, 0, 0, 0 };
int16_t sRotate[4] = { 0, 0, 0, 0 };
bool sRecordV3     = false;
float sLocalRotate = 0.0f;

void clear_tick_slots()
{
	for (int p = 0; p < 4; ++p) {
		sYawValid[p] = false;
		sExt[p]      = 0;
		sRotate[p]   = 0;
	}
}

const char* argvValue(int argc, char** argv, const char* flag)
{
	for (int i = 1; i + 1 < argc; ++i) {
		if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
	}
	return nullptr;
}
} // namespace

void pc_input_log_notify_argv(int argc, char** argv)
{
	sArgvSeen = true;
	if (argv == nullptr) return;
	sArgvRecord = argvValue(argc, argv, "--input-record");
	sArgvReplay = argvValue(argc, argv, "--input-replay");
}

bool pc_input_log_is_record_active(void) { return sRecordActive; }
bool pc_input_log_is_replay_active(void) { return sReplayActive; }

void pc_input_log_set_yaw_capture_fn(PcYawCaptureFn fn) { sYawCaptureFn = fn; }

// Only the hook runs; the tick index, replay and record are untouched.
void pc_input_log_capture_yaw(void)
{
	if (sYawCaptureFn != nullptr) sYawCaptureFn();
}

void pc_input_log_capture_yaw_fresh(void)
{
	for (int p = 0; p < 4; ++p) sYawValid[p] = false;
	if (sYawCaptureFn != nullptr) sYawCaptureFn();
}

void pc_input_log_yaw_set(int pad, uint16_t yaw, uint8_t flags)
{
	if (pad < 0 || pad >= 4) return;
	sYawRaw[pad]   = yaw;
	sYawFlags[pad] = flags;
	sYawValid[pad] = true;
}

bool pc_input_log_yaw_valid(int pad)
{
	if (pad < 0 || pad >= 4) return false;
	return sYawValid[pad];
}

uint16_t pc_input_log_yaw_raw(int pad)
{
	if (pad < 0 || pad >= 4) return 0;
	return sYawRaw[pad];
}

uint8_t pc_input_log_yaw_flags(int pad)
{
	if (pad < 0 || pad >= 4) return 0;
	return sYawFlags[pad];
}

void pc_input_log_ext_set(int pad, uint8_t ext, int16_t cursorRotate)
{
	if (pad < 0 || pad >= 4) return;
	sExt[pad]    = ext;
	sRotate[pad] = cursorRotate;
}

uint8_t pc_netplay_ext_bits(int pad)
{
	if (pad < 0 || pad >= 4) return 0;
	return sExt[pad];
}

int16_t pc_netplay_ext_cursor_rotate(int pad)
{
	if (pad < 0 || pad >= 4) return 0;
	return sRotate[pad];
}

bool pc_netplay_ext_trace(void)
{
	static const bool on = [] {
		const char* v = std::getenv("PIKMIN_NETPLAY_DEBUG_EXT");
		return v != nullptr && v[0] == '1';
	}();
	return on;
}

void pc_input_log_local_rotate_add(float angleRad)
{
	if (!std::isfinite(angleRad)) return;
	sLocalRotate += angleRad / pc_input_log::kYawTurnsToRad;
	if (sLocalRotate > 32767.0f) sLocalRotate = 32767.0f;
	if (sLocalRotate < -32767.0f) sLocalRotate = -32767.0f;
}

int16_t pc_input_log_local_rotate_take(void)
{
	const long q = std::lround(sLocalRotate);
	sLocalRotate -= (float)q;
	return (int16_t)q;
}

bool pc_netplay_control_yaw(int pad, float* sinYaw, float* cosYaw)
{
	if (pad < 0 || pad >= 4 || !sYawValid[pad]) return false;
	pc_input_log_yaw_sincos(sYawRaw[pad], sinYaw, cosYaw);
	return true;
}

void pc_input_log_flush(void)
{
	if (sRecordFile != nullptr) std::fflush(sRecordFile);
}

void pc_input_log_close(void)
{
	if (sRecordFile != nullptr) {
		std::fclose(sRecordFile);
		sRecordFile  = nullptr;
		sRecordActive = false;
	}
}

void pc_input_log_tick(void)
{
	if (!sInitialised) {
		sInitialised = true;
		const char* recordPath = sArgvRecord;
		const char* replayPath = sArgvReplay;
		if (recordPath == nullptr) recordPath = std::getenv("PIKMIN_INPUT_RECORD");
		if (replayPath == nullptr) replayPath = std::getenv("PIKMIN_INPUT_REPLAY");
		// Load the replay first, before the record file is opened: the
		// replay lives fully in memory, so record and replay may name the
		// same path for an identity check without truncating the source.
		if (replayPath != nullptr && *replayPath != '\0') {
			FILE* in = std::fopen(replayPath, "rb");
			if (in != nullptr) {
				std::fseek(in, 0, SEEK_END);
				long size = std::ftell(in);
				std::fseek(in, 0, SEEK_SET);
				if (size >= (long)pc_input_log::kHeaderBytes) {
					sReplayBytes.resize((size_t)size);
					size_t got = std::fread(sReplayBytes.data(), 1, (size_t)size, in);
					sReplayBytes.resize(got);
					uint16_t version = 0, pads = 0, rec = 0;
					if (pc_input_log_read_header(sReplayBytes.data(), sReplayBytes.size(), version, pads,
					                             rec)
					    == PC_INPUT_HEADER_OK) {
						size_t body = sReplayBytes.size() - pc_input_log::kHeaderBytes;
						if (body % rec != 0) {
							std::printf("[netplay] input replay: trailing %llu bytes ignored in %s\n",
							            (unsigned long long)(body % rec), replayPath);
						}
						sReplayTicks   = body / rec;
						sReplayVersion = version;
						sReplayRec     = rec;
						sReplayActive  = true;
						std::printf("[netplay] input replay: %s (v%u, %llu ticks)\n", replayPath,
						            (unsigned)version, (unsigned long long)sReplayTicks);
					} else {
						std::printf("[netplay] input replay: bad header in %s\n", replayPath);
						sReplayBytes.clear();
						std::fflush(stdout);
						std::exit(3);
					}
				} else {
					std::printf("[netplay] input replay: truncated file %s\n", replayPath);
					std::fflush(stdout);
					std::fclose(in);
					std::exit(3);
				}
				std::fclose(in);
				std::fflush(stdout);
			} else {
				std::printf("[netplay] input replay: cannot open %s\n", replayPath);
				std::fflush(stdout);
				std::exit(3);
			}
		}
		if (recordPath != nullptr && *recordPath != '\0') {
			sRecordFile = std::fopen(recordPath, "wb");
			if (sRecordFile != nullptr) {
				sRecordV3 = sReplayActive && sReplayVersion == pc_input_log::kVersionV3;
				std::vector<uint8_t> header;
				if (sRecordV3) pc_input_log_write_header_v3(header);
				else pc_input_log_write_header(header);
				std::fwrite(header.data(), 1, header.size(), sRecordFile);
				sRecordActive = true;
				std::printf("[netplay] input record: %s (v%d)\n", recordPath, sRecordV3 ? 3 : 2);
				std::fflush(stdout);
			} else {
				std::printf("[netplay] input record: cannot open %s\n", recordPath);
				std::fflush(stdout);
			}
		}
		// Optional RNG pre-roll (test knob), before the first tick's sim.
		pc_state_hash_before_first_tick();
		if (!sRecordActive && !sReplayActive) {
			// Det mode without a record or replay file (the mode a netplay
			// session runs in) still needs a fresh yaw every tick, so clear
			// the slots and run the pre-sim capture before returning. The hook itself
			// checks det/record, so the switch-off path stays untouched.
			clear_tick_slots();
			if (sYawCaptureFn != nullptr) sYawCaptureFn();
			return;
		}
	}

	if (!sRecordActive && !sReplayActive) {
		clear_tick_slots();
		if (sYawCaptureFn != nullptr) sYawCaptureFn();
		return;
	}

	// Fresh yaw every tick: replayed v2 refills it below; the pre-sim
	// capture hook fills the rest (live camera, quantised). The sim never
	// reads the camera, so the input for tick N is complete before the sim
	// for tick N runs.
	clear_tick_slots();

	PADStatus* pads = pc_netplay_pad_status();

	if (sReplayActive) {
		if (sTickIndex < sReplayTicks) {
			const uint8_t* rec = sReplayBytes.data() + pc_input_log::kHeaderBytes + sTickIndex * sReplayRec;
			if (sReplayVersion == pc_input_log::kVersionV1) {
				PcInputPad decoded[4];
				if (pc_input_log_decode_tick_v1(rec, sReplayRec, decoded)) {
					for (int p = 0; p < 4; ++p) {
						pads[p].button       = decoded[p].button;
						pads[p].stickX       = decoded[p].stickX;
						pads[p].stickY       = decoded[p].stickY;
						pads[p].substickX    = decoded[p].substickX;
						pads[p].substickY    = decoded[p].substickY;
						pads[p].triggerLeft  = decoded[p].triggerLeft;
						pads[p].triggerRight = decoded[p].triggerRight;
						pads[p].analogA      = decoded[p].analogA;
						pads[p].analogB      = decoded[p].analogB;
						pads[p].err          = decoded[p].err;
					}
					// v1 carries no yaw: left invalid so the pre-sim capture
					// hook fills it live from the camera, as today.
				}
			} else {
				PcInputPad decoded[4];
				const bool v3 = sReplayVersion == pc_input_log::kVersionV3;
				if (v3 ? pc_input_log_decode_tick_v3(rec, sReplayRec, decoded)
				       : pc_input_log_decode_tick(rec, sReplayRec, decoded)) {
					for (int p = 0; p < 4; ++p) {
						pads[p].button       = decoded[p].button;
						pads[p].stickX       = decoded[p].stickX;
						pads[p].stickY       = decoded[p].stickY;
						pads[p].substickX    = decoded[p].substickX;
						pads[p].substickY    = decoded[p].substickY;
						pads[p].triggerLeft  = decoded[p].triggerLeft;
						pads[p].triggerRight = decoded[p].triggerRight;
						pads[p].analogA      = decoded[p].analogA;
						pads[p].analogB      = decoded[p].analogB;
						pads[p].err          = decoded[p].err;
						sYawRaw[p]           = decoded[p].controlYaw;
						sYawFlags[p]         = v3 ? pc_input_log::kFlagsNone : decoded[p].flags;
						sYawValid[p]         = true;
						if (v3) {
							sExt[p]    = decoded[p].flags;
							sRotate[p] = decoded[p].cursorRotate;
						}
					}
				}
			}
		} else {
			// Past the end of the file: neutral pads. Pad 0 stays
			// connected; the other channels report no controller.
			for (int p = 0; p < 4; ++p) {
				pads[p].button       = 0;
				pads[p].stickX       = 0;
				pads[p].stickY       = 0;
				pads[p].substickX    = 0;
				pads[p].substickY    = 0;
				pads[p].triggerLeft  = 0;
				pads[p].triggerRight = 0;
				pads[p].analogA      = 0;
				pads[p].analogB      = 0;
				pads[p].err          = (p == 0) ? pc_input_log::kErrConnected
				                                : pc_input_log::kErrNoController;
			}
		}
	}

	// Pre-sim capture: fill every slot without a replayed value from the
	// live control cameras (quantised). The hook skips slots already valid
	// and is a no-op unless det mode or record mode needs it.
	if (sYawCaptureFn != nullptr) sYawCaptureFn();

	// Pre-sim record write: file the pads plus the yaw the sim will use, so
	// the filed input is exactly what was fed (in-tick pad writers cannot
	// skew it).
	if (sRecordActive) {
		PADStatus* pads = pc_netplay_pad_status();
		PcInputPad cur[4];
		for (int p = 0; p < 4; ++p) {
			cur[p].button       = pads[p].button;
			cur[p].stickX       = pads[p].stickX;
			cur[p].stickY       = pads[p].stickY;
			cur[p].substickX    = pads[p].substickX;
			cur[p].substickY    = pads[p].substickY;
			cur[p].triggerLeft  = pads[p].triggerLeft;
			cur[p].triggerRight = pads[p].triggerRight;
			cur[p].analogA      = pads[p].analogA;
			cur[p].analogB      = pads[p].analogB;
			cur[p].err          = pads[p].err;
			cur[p].controlYaw   = sYawValid[p] ? sYawRaw[p] : (uint16_t)0;
			cur[p].flags        = sYawValid[p] ? sYawFlags[p] : (uint8_t)0;
			cur[p].cursorRotate = 0;
			if (sRecordV3) {
				cur[p].flags        = sExt[p];
				cur[p].cursorRotate = sRotate[p];
			}
		}
		uint8_t out[pc_input_log::kRecordBytesV3];
		const size_t n = sRecordV3 ? pc_input_log_encode_tick_v3(cur, out) : pc_input_log_encode_tick(cur, out);
		std::fwrite(out, 1, n, sRecordFile);
		if ((sTickIndex + 1) % 300 == 0) std::fflush(sRecordFile);
	}
}

void pc_input_log_tick_end(void)
{
	if (!sInitialised) return;
	if (!sRecordActive && !sReplayActive) return;

	++sTickIndex;
}
