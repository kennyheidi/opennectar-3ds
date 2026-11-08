#pragma once
// Input record/replay for the netplay determinism harness.
//
// File format (little-endian binary, "PKNI" v1/v2/v3):
//   offset  size  field
//   0       4     magic: 'P' 'K' 'N' 'I'
//   4       2     version: u16, 1, 2 or 3
//   6       2     pad count: u16, currently 4
//   8       2     record size: u16, bytes per tick (44 for v1, 56 for v2,
//                 64 for v3)
//   10      N*rec tick records, one per tick, tick index = file order
//
// A v1 record holds the 4 pads in channel order, 11 PADStatus bytes each. A
// v2 record holds 14 bytes per pad: the same 11 bytes followed by
//   11      2     controlYaw: u16 LE, camera control yaw in 1/65536 turns
//   13      1     flags: u8, always 0 in v2; the ext bits in v3
// A v3 record holds 16 bytes per pad: the v2 14 followed by
//   14      2     cursorRotate: s16 LE, cursor rotation in 1/65536 turns
// Every field is serialised explicitly, never as a raw struct.
//
// Ext bits and cursor rotate (v3, and the netplay input bytes 10-12): the
// Bomb, Lock-On and Charge presses, the held Swarm button and the cursor
// rotation of a free-camera drag live outside PADStatus. In deterministic
// mode the sim reads them from these per-pad slots only (zero unless a
// session or a v3 replay fills them); outside it they are never read.
//
// Control yaw: in deterministic mode the camera yaw each player saw when
// they pushed the stick is part of that player's per-tick input, so the sim
// never reads a camera. Quantisation:
//   yaw = round(wrap(angle / 2pi) * 65536) & 0xFFFF
//   angle = yaw * 2pi / 65536
// The sim builds its basis from the quantised value only, so record and
// replay compute bit-identical values.
//
// Runtime behaviour:
//   PIKMIN_INPUT_RECORD=<file> (or --input-record <file>): before each tick's
//     simulation, append the 4 pads plus yaw/flags (always v2).
//   PIKMIN_INPUT_REPLAY=<file> (or --input-replay <file>): each tick,
//     overwrite the 4 polled pads with the recorded ones and feed the
//     recorded yaw (v2) to the sim. Past the end of the file the pads are
//     neutral (pad 0 connected, pads 1-3 no controller). A replay that cannot
//     be loaded exits the process with code 3, so a harness can never mistake
//     a live run for a replay.
//   Record and replay may be combined, even on the same path: the replay is
//     loaded into memory first, and recording a replayed v2 run reproduces
//     the replay file byte for byte.
//   Only PADStatus, yaw and (v3) the ext slots are covered. Mouse, wheel,
//     touch, gyro and other inputs outside them are neutralised in
//     deterministic mode rather than recorded. The record is v3 while a v3
//     file replays, else v2 (offline nothing fills the ext slots).
//   With neither switch set no file is touched and nothing is printed. The
//     yaw slots are still cleared and the capture hook still runs each tick,
//     so deterministic mode without a file gets a fresh yaw every tick.
//
// Tick split: pc_input_log_tick() runs after PADRead and before the sim; it
// clears the yaw slots, applies the replay, runs the capture hook and writes
// the record. pc_input_log_tick_end() runs after the sim and advances the
// tick index.

#include <cstddef>
#include <cstdint>
#include <vector>

struct PcInputPad {
	uint16_t button;
	int8_t stickX;
	int8_t stickY;
	int8_t substickX;
	int8_t substickY;
	uint8_t triggerLeft;
	uint8_t triggerRight;
	uint8_t analogA;
	uint8_t analogB;
	int8_t err;
	// v2 and v3; zero for v1 decodes.
	uint16_t controlYaw;
	uint8_t flags;
	// v3 only; zero for v1 and v2 decodes.
	int16_t cursorRotate;
};

namespace pc_input_log {
constexpr uint8_t kMagic[4]       = { 'P', 'K', 'N', 'I' };
constexpr uint16_t kVersion       = 2;
constexpr uint16_t kPadCount      = 4;
constexpr size_t kPadBytes        = 14;
constexpr size_t kRecordBytes     = kPadCount * kPadBytes; // 56
constexpr size_t kHeaderBytes     = 10;
constexpr int8_t kErrConnected    = 0;
constexpr int8_t kErrNoController = -1;
constexpr uint16_t kVersionV1     = 1;
constexpr size_t kPadBytesV1      = 11;
constexpr size_t kRecordBytesV1   = kPadCount * kPadBytesV1; // 44
constexpr float kYawTurnsToRad    = 6.28318530717958647692f / 65536.0f;
constexpr uint8_t kFlagsNone      = 0;
constexpr uint16_t kVersionV3     = 3;
constexpr size_t kPadBytesV3      = 16;
constexpr size_t kRecordBytesV3   = kPadCount * kPadBytesV3; // 64
constexpr uint8_t kExtBomb        = 0x01; // Bomb press
constexpr uint8_t kExtLockOn      = 0x02; // Lock-On press
constexpr uint8_t kExtCharge      = 0x04; // Charge press (the Swarm button's press)
constexpr uint8_t kExtSwarm       = 0x08; // Swarm button held
constexpr uint8_t kExtKnown       = 0x0F;
} // namespace pc_input_log

// Pure encode/decode API (no globals, no files).
size_t pc_input_log_encode_tick(const PcInputPad pads[4], uint8_t out[56]);
bool pc_input_log_decode_tick(const uint8_t* data, size_t avail, PcInputPad pads[4]);
// v1: 44-byte records; encode ignores controlYaw/flags, decode zeroes them.
size_t pc_input_log_encode_tick_v1(const PcInputPad pads[4], uint8_t out[44]);
bool pc_input_log_decode_tick_v1(const uint8_t* data, size_t avail, PcInputPad pads[4]);

// v3: 64-byte records with the ext bits (flags) and the cursor rotate.
size_t pc_input_log_encode_tick_v3(const PcInputPad pads[4], uint8_t out[64]);
bool pc_input_log_decode_tick_v3(const uint8_t* data, size_t avail, PcInputPad pads[4]);

// Appends the 10-byte header to out.
void pc_input_log_write_header(std::vector<uint8_t>& out);
void pc_input_log_write_header_v1(std::vector<uint8_t>& out);
void pc_input_log_write_header_v3(std::vector<uint8_t>& out);

enum PcInputHeaderResult {
	PC_INPUT_HEADER_OK          = 0,
	PC_INPUT_HEADER_TRUNCATED   = 1,
	PC_INPUT_HEADER_BAD_MAGIC   = 2,
	PC_INPUT_HEADER_UNSUPPORTED = 3, // version, pad count or record size mismatch
};

// Accepts v1 (1/4/44), v2 (2/4/56) and v3 (3/4/64); the outputs are set only on OK.
PcInputHeaderResult pc_input_log_read_header(const uint8_t* data, size_t len, uint16_t& version,
                                             uint16_t& padCount, uint16_t& recordSize);

// Yaw helpers. sincos uses the same sinf/cosf as Matrix4f::makeRotate, so
// the netplay libm covers both.
uint16_t pc_input_log_yaw_quantise(float angleRad);
float pc_input_log_yaw_to_angle(uint16_t yaw);
void pc_input_log_yaw_sincos(uint16_t yaw, float* sinYaw, float* cosYaw);

// Runtime API. argv must be captured before the first tick; env vars are
// read lazily on the first tick.
void pc_input_log_notify_argv(int argc, char** argv);

// Per-tick hook: after PADRead, before the sim.
void pc_input_log_tick(void);

// Pre-sim yaw capture hook, registered once by the engine (Navi) before the
// first tick. It fills every slot without a replayed value from that pad's
// live control camera.
typedef void (*PcYawCaptureFn)(void);
void pc_input_log_set_yaw_capture_fn(PcYawCaptureFn fn);

// Runs the capture hook now, without any record/replay logic. The _fresh
// variant clears the slots first so the hook refills them from the camera.
void pc_input_log_capture_yaw(void);
void pc_input_log_capture_yaw_fresh(void);

// Post-tick hook: after app->idle() returns.
void pc_input_log_tick_end(void);

// Per-pad yaw for the current tick, for the sim. False when no yaw is set
// (switch off, or no Navi/camera at capture time); in deterministic mode the
// sim then uses the neutral basis (yaw 0), never the camera.
bool pc_netplay_control_yaw(int pad, float* sinYaw, float* cosYaw);

// Slot access for the capture hook (never called from the sim).
void pc_input_log_yaw_set(int pad, uint16_t yaw, uint8_t flags);
bool pc_input_log_yaw_valid(int pad);
uint16_t pc_input_log_yaw_raw(int pad);
uint8_t pc_input_log_yaw_flags(int pad);

// Per-pad ext bits and cursor rotate for the current tick, for the sim in
// deterministic mode. Cleared with the yaw every tick; set by a v3 replay or
// by the session's input injection.
void pc_input_log_ext_set(int pad, uint8_t ext, int16_t cursorRotate);
uint8_t pc_netplay_ext_bits(int pad);
int16_t pc_netplay_ext_cursor_rotate(int pad);
// PIKMIN_NETPLAY_DEBUG_EXT=1: the sim logs every applied ext event. Cached.
bool pc_netplay_ext_trace(void);

// Local cursor rotation (this peer's own captain's free-camera drag, in
// radians) waiting for the session's next input. take() quantises to
// 1/65536 turns and keeps the remainder for the next take.
void pc_input_log_local_rotate_add(float angleRad);
int16_t pc_input_log_local_rotate_take(void);

bool pc_input_log_is_record_active(void);
bool pc_input_log_is_replay_active(void);

// Flush (every 300 ticks and on exit) / close the record file, if any.
void pc_input_log_flush(void);
void pc_input_log_close(void);
