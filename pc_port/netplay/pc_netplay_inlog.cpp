// Netplay session input log. See pc_netplay_inlog.h.

#include "netplay/pc_netplay_inlog.h"

#include <cstring>

namespace pc_netplay_inlog {

std::string Log::meta_get(const std::string& key, const std::string& fallback) const
{
	for (const auto& kv : meta)
		if (kv.first == key) return kv.second;
	return fallback;
}

namespace {
void put16(std::vector<uint8_t>* o, uint16_t v)
{
	o->push_back((uint8_t)(v & 0xFF));
	o->push_back((uint8_t)((v >> 8) & 0xFF));
}
void put32(std::vector<uint8_t>* o, uint32_t v)
{
	for (int i = 0; i < 4; ++i) o->push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
void put64(std::vector<uint8_t>* o, uint64_t v)
{
	for (int i = 0; i < 8; ++i) o->push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
uint32_t get32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t get64(const uint8_t* p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
	return v;
}
} // namespace

void encode_header(const std::string& metaText, std::vector<uint8_t>* out)
{
	std::string meta = metaText;
	if (meta.size() > kMaxMetaBytes) meta.resize(kMaxMetaBytes);
	out->push_back('P');
	out->push_back('K');
	out->push_back('N');
	out->push_back('L');
	put16(out, kVersion);
	put16(out, 0);
	put32(out, (uint32_t)meta.size());
	out->insert(out->end(), meta.begin(), meta.end());
}

void encode_frame(const uint8_t prev[2][kInputBytes], const Frame& f, std::vector<uint8_t>* out)
{
	uint8_t tag = 0;
	if (std::memcmp(prev[0], f.in[0], kInputBytes) != 0) tag |= 0x01;
	if (std::memcmp(prev[1], f.in[1], kInputBytes) != 0) tag |= 0x02;
	if (f.haveSubs) tag |= 0x04;
	if (f.haveTotal) tag |= 0x08;
	out->push_back(tag);
	put32(out, f.frame);
	if (tag & 0x01) out->insert(out->end(), f.in[0], f.in[0] + kInputBytes);
	if (tag & 0x02) out->insert(out->end(), f.in[1], f.in[1] + kInputBytes);
	if (tag & 0x08) put64(out, f.total);
	if (tag & 0x04)
		for (int i = 0; i < 7; ++i) put64(out, f.subs[i]);
}

void encode_event(const Event& e, std::vector<uint8_t>* out)
{
	out->push_back(kTagEvent);
	put32(out, e.frame);
	out->push_back(e.kind);
	const size_t n = e.data.size() > 0xFFFF ? 0xFFFF : e.data.size();
	put16(out, (uint16_t)n);
	out->insert(out->end(), e.data.begin(), e.data.begin() + (long)n);
}

bool parse(const uint8_t* d, size_t len, Log* out, std::string* err)
{
	auto fail = [&](const char* why) {
		if (err != nullptr) *err = why;
		return false;
	};
	if (d == nullptr || out == nullptr) return fail("null argument");
	if (len < 12) return fail("shorter than the 12-byte header");
	if (d[0] != 'P' || d[1] != 'K' || d[2] != 'N' || d[3] != 'L') return fail("bad magic (not a PKNL input log)");
	if (get16(d + 4) != kVersion) return fail("unsupported version");
	const uint32_t metaLen = get32(d + 8);
	if (metaLen > kMaxMetaBytes || 12 + (size_t)metaLen > len) return fail("bad meta length");
	Log log;
	{
		const std::string text((const char*)d + 12, metaLen);
		size_t at = 0;
		while (at < text.size()) {
			size_t nl = text.find('\n', at);
			if (nl == std::string::npos) nl = text.size();
			std::string line = text.substr(at, nl - at);
			at               = nl + 1;
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty()) continue;
			const size_t sp = line.find(' ');
			if (sp == std::string::npos) log.meta.emplace_back(line, std::string());
			else log.meta.emplace_back(line.substr(0, sp), line.substr(sp + 1));
		}
	}
	uint8_t prev[2][kInputBytes] = {};
	size_t at = 12 + metaLen;
	while (at < len) {
		const size_t start = at;
		const uint8_t tag  = d[at++];
		if ((tag & ~kTagFrameMask) == 0) {
			size_t need = 4;
			if (tag & 0x01) need += kInputBytes;
			if (tag & 0x02) need += kInputBytes;
			if (tag & 0x08) need += 8;
			if (tag & 0x04) need += 56;
			if (len - at < need) {
				at = start;
				break;
			}
			Frame f;
			f.frame = get32(d + at);
			at += 4;
			if (tag & 0x01) {
				std::memcpy(prev[0], d + at, kInputBytes);
				at += kInputBytes;
			}
			if (tag & 0x02) {
				std::memcpy(prev[1], d + at, kInputBytes);
				at += kInputBytes;
			}
			std::memcpy(f.in[0], prev[0], kInputBytes);
			std::memcpy(f.in[1], prev[1], kInputBytes);
			if (tag & 0x08) {
				f.haveTotal = true;
				f.total     = get64(d + at);
				at += 8;
			}
			if (tag & 0x04) {
				f.haveSubs = true;
				for (int i = 0; i < 7; ++i) f.subs[i] = get64(d + at + 8 * i);
				at += 56;
			}
			log.frames.push_back(f);
		} else if (tag == kTagEvent) {
			if (len - at < 7) {
				at = start;
				break;
			}
			Event e;
			e.frame        = get32(d + at);
			e.kind         = d[at + 4];
			const size_t n = get16(d + at + 5);
			at += 7;
			if (len - at < n) {
				at = start;
				break;
			}
			e.data.assign(d + at, d + at + n);
			at += n;
			log.events.push_back(std::move(e));
		} else {
			// An unknown tag: the rest cannot be framed. Report it as a cut.
			at = start;
			break;
		}
	}
	log.truncatedBytes = len - at;
	*out               = std::move(log);
	return true;
}

bool load_file(const std::string& path, Log* out, std::string* err)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (f == nullptr) {
		if (err != nullptr) *err = "cannot open " + path;
		return false;
	}
	std::vector<uint8_t> bytes;
	uint8_t buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		bytes.insert(bytes.end(), buf, buf + n);
		if (bytes.size() > (size_t)kMaxFileBytes + 65536) {
			std::fclose(f);
			if (err != nullptr) *err = "file larger than the log cap";
			return false;
		}
	}
	std::fclose(f);
	return parse(bytes.data(), bytes.size(), out, err);
}

Writer::~Writer() { close(); }

bool Writer::open(const std::string& path, const std::string& metaText, std::string* err)
{
	close();
	mFile = std::fopen(path.c_str(), "wb");
	if (mFile == nullptr) {
		if (err != nullptr) *err = "cannot create " + path;
		return false;
	}
	mPath = path;
	std::memset(mPrev, 0, sizeof(mPrev));
	mBytes = mFrames = 0;
	mCapped          = false;
	std::vector<uint8_t> h;
	encode_header(metaText, &h);
	if (!put(h)) {
		close();
		if (err != nullptr) *err = "cannot write " + path;
		return false;
	}
	std::fflush(mFile);
	return true;
}

bool Writer::put(const std::vector<uint8_t>& b)
{
	if (mFile == nullptr || b.empty()) return mFile != nullptr;
	if (std::fwrite(b.data(), 1, b.size(), mFile) != b.size()) return false;
	mBytes += b.size();
	return true;
}

bool Writer::append_frame(const Frame& f)
{
	if (mFile == nullptr || mCapped) return false;
	if (mBytes >= kMaxFileBytes) {
		Event e;
		e.frame = f.frame;
		e.kind  = kEvNote;
		const char* t = "log size cap reached; later frames are not recorded";
		e.data.assign(t, t + std::strlen(t));
		std::vector<uint8_t> b;
		encode_event(e, &b);
		put(b);
		mCapped = true;
		std::fflush(mFile);
		return false;
	}
	std::vector<uint8_t> b;
	encode_frame(mPrev, f, &b);
	if (!put(b)) return false;
	std::memcpy(mPrev[0], f.in[0], kInputBytes);
	std::memcpy(mPrev[1], f.in[1], kInputBytes);
	++mFrames;
	return true;
}

bool Writer::append_event(const Event& e)
{
	if (mFile == nullptr) return false;
	std::vector<uint8_t> b;
	encode_event(e, &b);
	return put(b);
}

void Writer::flush()
{
	if (mFile != nullptr) std::fflush(mFile);
}

void Writer::close()
{
	if (mFile != nullptr) {
		std::fflush(mFile);
		std::fclose(mFile);
		mFile = nullptr;
	}
}

} // namespace pc_netplay_inlog
