#pragma once
// Netplay session input log: every confirmed per-frame input pair GekkoNet
// handed to Advance, so a desynced session can be replayed offline from its
// start. Engine-free (<cstdio>/<cstdint> only).
//
// File "PKNL" v1 (little-endian, every field explicit):
//   0    4   magic 'P','K','N','L'
//   4    2   version (1)
//   6    2   reserved 0
//   8    4   metaLen
//   12   M   meta: UTF-8 text, one "key value" per line (session identity:
//            role, config text and exe/config digests, seed, delay, start
//            time; see the session's inlog_open)
//   12+M ... records until end of file
// Record = one tag byte, then the body:
//   tag bits 0x00..0x0F, a FRAME record (one per Advance, in frame order):
//     bit0 0x01  p0 (host) input changed since the previous frame: 16 bytes
//     bit1 0x02  p1 (joiner) input changed: 16 bytes
//     bit2 0x04  the seven sub-hash columns are present
//     bit3 0x08  the folded state-hash total is present
//     body: u32 frame, [p0 16B], [p1 16B], [u64 total], [7 * u64 subs]
//     The first frame's "previous" input is 16 zero bytes (neutral), so a
//     neutral frame costs 5 + 8 bytes. An input is the exact 16 bytes
//     GekkoNet passed (pc_netplay_gekko_input.h): buttons, sticks, triggers,
//     control yaw and the extended-input bytes.
//     total/subs are THIS peer's hash AFTER simulating the frame; a replay
//     compares its own against them.
//   tag 0x40, an EVENT record: u32 frame, u8 kind, u16 len, len bytes.
//     kind 2 note (text), kind 3 end (text: why the session ended);
//     kind 1 is reserved.
// A file cut anywhere (kill, power) is still readable up to its last whole
// record. The writer bounds the file (kMaxFileBytes), then writes one final
// note and stops.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace pc_netplay_inlog {

constexpr uint16_t kVersion       = 1;
constexpr size_t kInputBytes      = 16;
constexpr size_t kMaxMetaBytes    = 16384;
constexpr uint64_t kMaxFileBytes  = 96ull * 1024 * 1024; // about 3 MB per busy hour (45 B/frame worst case: 4.7 MB/h); 96 MB is ~20 hours
constexpr uint32_t kSubsEvery     = 32;                  // frames between sub-hash columns
constexpr uint8_t kTagFrameMask   = 0x0F;
constexpr uint8_t kTagEvent       = 0x40;
constexpr uint8_t kEvNote         = 2;
constexpr uint8_t kEvEnd          = 3;

struct Frame {
	uint32_t frame = 0;
	uint8_t in[2][kInputBytes] = {};
	bool haveTotal = false;
	uint64_t total = 0;
	bool haveSubs = false;
	uint64_t subs[7] = { 0, 0, 0, 0, 0, 0, 0 };
};
struct Event {
	uint32_t frame = 0;
	uint8_t kind = 0;
	std::vector<uint8_t> data;
};
struct Log {
	std::vector<std::pair<std::string, std::string>> meta;
	std::vector<Frame> frames;
	std::vector<Event> events;
	size_t truncatedBytes = 0; // bytes after the last whole record
	std::string meta_get(const std::string& key, const std::string& fallback = std::string()) const;
};

// Pure parse of a whole file's bytes. False (with *err) when the header is bad.
bool parse(const uint8_t* data, size_t len, Log* out, std::string* err);
bool load_file(const std::string& path, Log* out, std::string* err);

// Pure serialisation helpers (also what the writer uses).
void encode_header(const std::string& metaText, std::vector<uint8_t>* out);
void encode_frame(const uint8_t prev[2][kInputBytes], const Frame& f, std::vector<uint8_t>* out);
void encode_event(const Event& e, std::vector<uint8_t>* out);

// Appending writer. Buffered in memory and in stdio; flush() is cheap and is
// called by the session at least every few seconds and on every exit path.
class Writer {
public:
	~Writer();
	bool open(const std::string& path, const std::string& metaText, std::string* err);
	bool is_open() const { return mFile != nullptr; }
	// False once the size cap stopped the log (a final note was written).
	bool append_frame(const Frame& f);
	bool append_event(const Event& e);
	void flush();
	void close();
	uint64_t bytes() const { return mBytes; }
	uint64_t frames() const { return mFrames; }
	bool capped() const { return mCapped; }
	const std::string& path() const { return mPath; }

private:
	bool put(const std::vector<uint8_t>& b);
	FILE* mFile = nullptr;
	std::string mPath;
	uint8_t mPrev[2][kInputBytes] = {};
	uint64_t mBytes  = 0;
	uint64_t mFrames = 0;
	bool mCapped     = false;
};

} // namespace pc_netplay_inlog
