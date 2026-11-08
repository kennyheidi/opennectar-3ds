// Netplay desync forensics: host test for the session input log
// format (pc_netplay_inlog) and the desync exchange data model
// (pc_netplay_forensics). No game code.

#include "netplay/pc_netplay_forensics.h"
#include "netplay/pc_netplay_inlog.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace pc_netplay_inlog;
namespace fx = pc_netplay_forensics;

static int sFailures = 0;

static void check(bool ok, const char* what)
{
	if (!ok) {
		std::printf("FAIL: %s\n", what);
		++sFailures;
	}
}

static Frame make_frame(uint32_t frame, uint8_t a, uint8_t b, bool total, bool subs)
{
	Frame f;
	f.frame = frame;
	for (size_t i = 0; i < kInputBytes; ++i) {
		f.in[0][i] = (uint8_t)(a + i);
		f.in[1][i] = (uint8_t)(b ^ (uint8_t)i);
	}
	if (a == 0) std::memset(f.in[0], 0, kInputBytes);
	if (b == 0) std::memset(f.in[1], 0, kInputBytes);
	f.haveTotal = total;
	f.total     = 0x1122334455667788ull + frame;
	f.haveSubs  = subs;
	for (int i = 0; i < 7; ++i) f.subs[i] = 0xAB00000000000000ull + (uint64_t)i * 0x10001 + frame;
	return f;
}

static void test_inlog_roundtrip()
{
	std::vector<Frame> in;
	in.push_back(make_frame(0, 0, 0, true, false));  // neutral: delta mask 0
	in.push_back(make_frame(1, 5, 0, true, false));  // p0 changes
	in.push_back(make_frame(2, 5, 0, true, false));  // identical: no input bytes
	in.push_back(make_frame(3, 5, 9, true, true));   // p1 changes, subs present
	in.push_back(make_frame(4, 7, 9, false, false)); // no hash
	std::vector<uint8_t> bytes;
	encode_header("role host\ntoken abc\nseed 13\n", &bytes);
	uint8_t prev[2][kInputBytes] = {};
	for (const Frame& f : in) {
		encode_frame(prev, f, &bytes);
		std::memcpy(prev[0], f.in[0], kInputBytes);
		std::memcpy(prev[1], f.in[1], kInputBytes);
	}
	Event ev;
	ev.frame = 3;
	ev.kind  = kEvNote;
	ev.data.assign(68, 0x5A);
	encode_event(ev, &bytes);

	Log log;
	std::string err;
	check(parse(bytes.data(), bytes.size(), &log, &err), "parse of a whole log");
	check(log.meta_get("role") == "host" && log.meta_get("seed") == "13" && log.meta_get("nope", "x") == "x",
	      "meta read back");
	check(log.frames.size() == in.size(), "frame count");
	bool same = log.frames.size() == in.size();
	for (size_t i = 0; same && i < in.size(); ++i) {
		const Frame& a = in[i];
		const Frame& b = log.frames[i];
		same = a.frame == b.frame && std::memcmp(a.in[0], b.in[0], kInputBytes) == 0
		    && std::memcmp(a.in[1], b.in[1], kInputBytes) == 0 && a.haveTotal == b.haveTotal
		    && (!a.haveTotal || a.total == b.total) && a.haveSubs == b.haveSubs
		    && (!a.haveSubs || std::memcmp(a.subs, b.subs, sizeof(a.subs)) == 0);
	}
	check(same, "every frame round-trips (inputs, hash total, sub-hashes)");
	check(log.events.size() == 1 && log.events[0].kind == kEvNote && log.events[0].data.size() == 68
	          && log.events[0].frame == 3,
	      "event round-trips");
	check(log.truncatedBytes == 0, "no truncation");

	// Delta coding: the identical frame 2 carries no input bytes (tag + frame + total).
	std::vector<uint8_t> one;
	encode_frame(in[1].in, in[2], &one);
	check(one.size() == 1 + 4 + 8, "an unchanged frame is tag+frame+total only");

	// Cut anywhere: still parses to the last whole record.
	bool allCuts = true;
	for (size_t cut = 12 + 33; cut < bytes.size(); ++cut) {
		Log c;
		if (!parse(bytes.data(), cut, &c, &err)) {
			allCuts = false;
			break;
		}
		if (c.frames.size() > in.size()) allCuts = false;
		for (size_t i = 0; i < c.frames.size(); ++i)
			if (std::memcmp(c.frames[i].in[0], in[i].in[0], kInputBytes) != 0) allCuts = false;
	}
	check(allCuts, "every truncation point parses to a clean prefix");
	{
		Log c;
		check(parse(bytes.data(), bytes.size() - 3, &c, &err) && c.truncatedBytes > 0, "a cut event reports truncatedBytes");
	}
	// Garbage is refused.
	{
		Log c;
		std::vector<uint8_t> bad = bytes;
		bad[0] = 'X';
		check(!parse(bad.data(), bad.size(), &c, &err), "bad magic refused");
		check(!parse(bytes.data(), 8, &c, &err), "short header refused");
		bad = bytes;
		bad[8] = 0xFF;
		bad[9] = 0xFF;
		bad[10] = 0xFF;
		bad[11] = 0x7F;
		check(!parse(bad.data(), bad.size(), &c, &err), "absurd meta length refused");
	}
}

static std::string tmp_path(const char* name)
{
	const char* t = std::getenv("TEMP");
	if (t == nullptr) t = std::getenv("TMPDIR");
	std::string p = t != nullptr ? t : ".";
	return p + "/" + name;
}

static void test_inlog_writer()
{
	const std::string path = tmp_path("pc_netplay_forensics_test.pknl");
	Writer w;
	std::string err;
	check(w.open(path, "role join\n", &err), "writer opens");
	for (uint32_t i = 0; i < 100; ++i) {
		Frame f = make_frame(i, (uint8_t)(i / 10), (uint8_t)(i / 7), true, i % kSubsEvery == 0);
		check(w.append_frame(f), "append frame");
	}
	w.flush();
	// Readable while still open (the flush made it durable enough to read).
	Log log;
	check(load_file(path, &log, &err) && log.frames.size() == 100, "log is readable before close, 100 frames");
	Event e;
	e.frame = 100;
	e.kind  = kEvEnd;
	e.data.assign({ 'd', 'o', 'n', 'e' });
	check(w.append_event(e), "append end event");
	w.close();
	Log log2;
	check(load_file(path, &log2, &err) && log2.frames.size() == 100 && log2.events.size() == 1
	          && log2.meta_get("role") == "join",
	      "log after close");
	std::remove(path.c_str());
	Log missing;
	check(!load_file(tmp_path("pc_netplay_forensics_test_missing.pknl"), &missing, &err), "missing file refused");
}

static fx::TickSubs ts(uint64_t tick, uint64_t total, uint64_t xtra, uint64_t piki = 1)
{
	fx::TickSubs t;
	t.tick  = tick;
	t.total = total;
	for (int i = 0; i < 7; ++i) t.subs[i] = 100 + (uint64_t)i;
	t.subs[1] = piki;
	t.xtra    = xtra;
	return t;
}

static void test_ring_wire_and_diff()
{
	fx::RingMsg m;
	m.role           = 1;
	m.frame          = 41535;
	m.localChecksum  = 0xa92664b0u;
	m.remoteChecksum = 0xa059c49du;
	for (uint64_t t = 41500; t < 41540; ++t) m.ticks.push_back(ts(t, 7, 9));
	m.ticks[30] = ts(41530, 8, 9, 2); // differing piki at 41530
	const std::vector<uint8_t> w = fx::encode_ring(m);
	fx::RingMsg back;
	check(fx::decode_ring(w.data(), w.size(), &back), "ring decodes");
	check(back.role == 1 && back.frame == 41535 && back.localChecksum == 0xa92664b0u && back.remoteChecksum == 0xa059c49du
	          && back.ticks.size() == 40 && back.ticks[30].total == 8 && back.ticks[30].subs[1] == 2,
	      "ring fields round-trip");
	check(!fx::decode_ring(w.data(), w.size() - 1, &back), "ring with a missing byte refused");
	std::vector<uint8_t> big = w;
	big[20] = 0xFF;
	big[21] = 0xFF;
	big[22] = 0xFF;
	big[23] = 0x7F;
	check(!fx::decode_ring(big.data(), big.size(), &back), "ring with an absurd count refused");
	std::vector<uint8_t> badMagic = w;
	badMagic[0] = 'Z';
	check(!fx::decode_ring(badMagic.data(), badMagic.size(), &back), "ring bad magic refused");

	std::vector<fx::TickSubs> a, b;
	for (uint64_t t = 41500; t < 41540; ++t) {
		a.push_back(ts(t, 7, 9));
		b.push_back(ts(t, 7, 9));
	}
	uint64_t tick = 0;
	check(!fx::first_diff_total(a, b, &tick), "identical rings: no differing tick");
	b[30] = ts(41530, 8, 9, 2);
	b[31] = ts(41531, 8, 9, 2);
	check(fx::first_diff_total(a, b, &tick) && tick == 41530, "first differing total tick");
	check(fx::subs_mask_text(fx::differing_subs(a[30], b[30])) == "piki", "differing sub is piki");
	check(fx::subs_mask_text(0) == "none", "mask 0 -> none");
	b[10] = ts(41510, 7, 77); // xtra differs earlier than the visible total
	check(fx::first_diff_xtra(a, b, &tick) && tick == 41510, "first differing xtra tick precedes the total");
	// Rings with only a partial overlap diff over the overlap only.
	std::vector<fx::TickSubs> c(b.begin() + 5, b.end());
	check(fx::first_diff_total(a, c, &tick) && tick == 41530, "partial overlap diff");
	check(fx::find_tick(a, 41539) != nullptr && fx::find_tick(a, 5) == nullptr, "find_tick");
}

static fx::ObjRec obj(uint8_t kind, int ord, uint64_t hash)
{
	fx::ObjRec r;
	r.kind  = kind;
	r.ord   = ord;
	r.type  = kind * 10;
	r.state = 3;
	r.health = 1.5f;
	r.f[0]  = 12.25f;
	r.hash  = hash;
	r.xhash = 5;
	return r;
}

static void test_objs()
{
	fx::ObjsMsg m;
	m.role = 0;
	fx::ObjTick t;
	t.tick = 41536;
	for (int i = 0; i < 20; ++i) t.keys.push_back(fx::key_of(obj(fx::kPiki, i, 1000 + (uint64_t)i)));
	for (int i = 0; i < 3; ++i) t.keys.push_back(fx::key_of(obj(fx::kTeki, i, 2000 + (uint64_t)i)));
	t.keys.push_back(fx::key_of(obj(fx::kBoss, 0, 3000)));
	m.ticks.push_back(t);
	t.tick = 41530;
	m.ticks.push_back(t);
	const std::vector<uint8_t> w = fx::encode_objs(m);
	fx::ObjsMsg back;
	check(fx::decode_objs(w.data(), w.size(), &back) && back.ticks.size() == 2 && back.ticks[0].tick == 41536
	          && back.ticks[1].tick == 41530 && back.ticks[0].keys.size() == 24 && back.ticks[0].keys[20].kind == fx::kTeki
	          && back.ticks[0].keys[23].hash == 3000,
	      "objs round-trip");
	check(!fx::decode_objs(w.data(), w.size() - 2, &back), "objs truncated refused");
	std::vector<uint8_t> extra = w;
	extra.push_back(0);
	check(!fx::decode_objs(extra.data(), extra.size(), &back), "objs trailing byte refused");
	std::vector<uint8_t> manyTicks = w;
	manyTicks[6] = 9;
	check(!fx::decode_objs(manyTicks.data(), manyTicks.size(), &back), "objs tick count bound");

	// The injected-desync scenario: one piki differs, one boss only on one side.
	std::vector<fx::ObjKey> local = back.ticks[0].keys, remote = back.ticks[0].keys;
	remote[7].hash ^= 1;
	remote.pop_back(); // boss gone on the remote side
	const fx::ObjDiff d = fx::diff_objs(local, remote);
	check(d.changed.size() == 1 && d.changed[0].local.kind == fx::kPiki && d.changed[0].local.ord == 7,
	      "diff names the changed piki");
	check(d.onlyLocal.size() == 1 && d.onlyLocal[0].kind == fx::kBoss && d.onlyRemote.empty() && d.countMismatch,
	      "diff names the object present on one side only");
	const fx::ObjDiff same = fx::diff_objs(local, local);
	check(same.changed.empty() && same.onlyLocal.empty() && same.onlyRemote.empty() && !same.countMismatch,
	      "identical object sets diff empty");

	const std::string line = fx::format_obj(obj(fx::kPiki, 4, 0xabcdef));
	check(line.find("piki ord=4") == 0 && line.find("hash=0000000000abcdef") != std::string::npos
	          && line.find("pos=(12.25,0,0)") != std::string::npos,
	      "dump line format");
	check(fx::format_subs(ts(5, 6, 7)).find("5 0000000000000006") == 0, "subs line format");
}

int main()
{
	test_inlog_roundtrip();
	test_inlog_writer();
	test_ring_wire_and_diff();
	test_objs();
	if (sFailures != 0) {
		std::printf("pc_netplay_forensics_test: %d failure(s)\n", sFailures);
		return 1;
	}
	std::printf("pc_netplay_forensics_test: ok\n");
	return 0;
}
