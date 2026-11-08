// Netplay desync forensics. See pc_netplay_forensics.h.

#include "netplay/pc_netplay_forensics.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pc_netplay_forensics {

const char* const kSubNames[kSubCount] = { "navi", "piki", "teki", "item", "world", "rng", "mode" };

const char* kind_name(uint8_t kind)
{
	switch (kind) {
	case kNavi: return "navi";
	case kPiki: return "piki";
	case kTeki: return "teki";
	case kItem: return "item";
	case kPellet: return "pellet";
	case kBoss: return "boss";
	case kOnion: return "onion";
	case kWorld: return "world";
	case kRng: return "rng";
	case kMode: return "mode";
	default: return "?";
	}
}

namespace {
void put16(std::vector<uint8_t>& o, uint16_t v)
{
	o.push_back((uint8_t)(v & 0xFF));
	o.push_back((uint8_t)((v >> 8) & 0xFF));
}
void put32(std::vector<uint8_t>& o, uint32_t v)
{
	for (int i = 0; i < 4; ++i) o.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}
void put64(std::vector<uint8_t>& o, uint64_t v)
{
	for (int i = 0; i < 8; ++i) o.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
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

std::vector<uint8_t> encode_ring(const RingMsg& m)
{
	std::vector<uint8_t> o;
	const size_t n = std::min(m.ticks.size(), kMaxRingEntries);
	o.reserve(24 + n * kTickSubsBytes);
	o.push_back('D');
	o.push_back('S');
	o.push_back('R');
	o.push_back('1');
	o.push_back(1);
	o.push_back(m.role);
	put16(o, 0);
	put32(o, m.frame);
	put32(o, m.localChecksum);
	put32(o, m.remoteChecksum);
	put32(o, (uint32_t)n);
	for (size_t i = 0; i < n; ++i) {
		const TickSubs& t = m.ticks[i];
		put64(o, t.tick);
		put64(o, t.total);
		for (int k = 0; k < kSubCount; ++k) put64(o, t.subs[k]);
		put64(o, t.xtra);
	}
	return o;
}

bool decode_ring(const uint8_t* d, size_t len, RingMsg* out)
{
	if (d == nullptr || out == nullptr || len < 24) return false;
	if (d[0] != 'D' || d[1] != 'S' || d[2] != 'R' || d[3] != '1' || d[4] != 1) return false;
	RingMsg m;
	m.role           = d[5];
	m.frame          = get32(d + 8);
	m.localChecksum  = get32(d + 12);
	m.remoteChecksum = get32(d + 16);
	const uint32_t n = get32(d + 20);
	if (n > kMaxRingEntries) return false;
	if (len != 24 + (size_t)n * kTickSubsBytes) return false;
	m.ticks.resize(n);
	const uint8_t* p = d + 24;
	for (uint32_t i = 0; i < n; ++i, p += kTickSubsBytes) {
		TickSubs& t = m.ticks[i];
		t.tick      = get64(p);
		t.total     = get64(p + 8);
		for (int k = 0; k < kSubCount; ++k) t.subs[k] = get64(p + 16 + 8 * k);
		t.xtra = get64(p + 72);
	}
	*out = std::move(m);
	return true;
}

std::vector<uint8_t> encode_objs(const ObjsMsg& m)
{
	std::vector<uint8_t> o;
	const size_t nt = std::min(m.ticks.size(), kMaxObjTicks);
	o.push_back('D');
	o.push_back('S');
	o.push_back('O');
	o.push_back('1');
	o.push_back(1);
	o.push_back(m.role);
	put16(o, (uint16_t)nt);
	for (size_t t = 0; t < nt; ++t) {
		const ObjTick& ot = m.ticks[t];
		const size_t n    = std::min(ot.keys.size(), kMaxObjsPerTick);
		put64(o, ot.tick);
		put32(o, (uint32_t)n);
		for (size_t i = 0; i < n; ++i) {
			const ObjKey& k = ot.keys[i];
			o.push_back(k.kind);
			put32(o, (uint32_t)k.ord);
			put32(o, (uint32_t)k.type);
			put64(o, k.hash);
			put64(o, k.xhash);
		}
	}
	return o;
}

bool decode_objs(const uint8_t* d, size_t len, ObjsMsg* out)
{
	if (d == nullptr || out == nullptr || len < 8) return false;
	if (d[0] != 'D' || d[1] != 'S' || d[2] != 'O' || d[3] != '1' || d[4] != 1) return false;
	ObjsMsg m;
	m.role         = d[5];
	const size_t nt = get16(d + 6);
	if (nt > kMaxObjTicks) return false;
	size_t at = 8;
	for (size_t t = 0; t < nt; ++t) {
		if (len - at < 12) return false;
		ObjTick ot;
		ot.tick          = get64(d + at);
		const uint32_t n = get32(d + at + 8);
		at += 12;
		if (n > kMaxObjsPerTick) return false;
		if (len - at < (size_t)n * kObjKeyBytes) return false;
		ot.keys.resize(n);
		for (uint32_t i = 0; i < n; ++i, at += kObjKeyBytes) {
			ObjKey& k = ot.keys[i];
			k.kind    = d[at];
			k.ord     = (int32_t)get32(d + at + 1);
			k.type    = (int32_t)get32(d + at + 5);
			k.hash    = get64(d + at + 9);
			k.xhash   = get64(d + at + 17);
		}
		m.ticks.push_back(std::move(ot));
	}
	if (at != len) return false;
	*out = std::move(m);
	return true;
}

ObjKey key_of(const ObjRec& r)
{
	ObjKey k;
	k.kind  = r.kind;
	k.ord   = r.ord;
	k.type  = r.type;
	k.hash  = r.hash;
	k.xhash = r.xhash;
	return k;
}

unsigned differing_subs(const TickSubs& a, const TickSubs& b)
{
	unsigned mask = 0;
	for (int i = 0; i < kSubCount; ++i)
		if (a.subs[i] != b.subs[i]) mask |= 1u << i;
	return mask;
}

std::string subs_mask_text(unsigned mask)
{
	std::string s;
	for (int i = 0; i < kSubCount; ++i) {
		if (!(mask & (1u << i))) continue;
		if (!s.empty()) s += ",";
		s += kSubNames[i];
	}
	return s.empty() ? std::string("none") : s;
}

const TickSubs* find_tick(const std::vector<TickSubs>& v, uint64_t tick)
{
	for (const TickSubs& t : v)
		if (t.tick == tick) return &t;
	return nullptr;
}

namespace {
bool first_diff(const std::vector<TickSubs>& a, const std::vector<TickSubs>& b, bool xtra, uint64_t* tick)
{
	bool found = false;
	uint64_t best = 0;
	for (const TickSubs& ta : a) {
		const TickSubs* tb = find_tick(b, ta.tick);
		if (tb == nullptr) continue;
		const bool differs = xtra ? ta.xtra != tb->xtra : ta.total != tb->total;
		if (!differs) continue;
		if (!found || ta.tick < best) {
			best  = ta.tick;
			found = true;
		}
	}
	if (found && tick != nullptr) *tick = best;
	return found;
}
} // namespace

bool first_diff_total(const std::vector<TickSubs>& a, const std::vector<TickSubs>& b, uint64_t* tick)
{
	return first_diff(a, b, false, tick);
}
bool first_diff_xtra(const std::vector<TickSubs>& a, const std::vector<TickSubs>& b, uint64_t* tick)
{
	return first_diff(a, b, true, tick);
}

ObjDiff diff_objs(const std::vector<ObjKey>& local, const std::vector<ObjKey>& remote)
{
	ObjDiff d;
	for (int kind = 0; kind < kKindCount; ++kind) {
		std::vector<const ObjKey*> l, r;
		for (const ObjKey& k : local)
			if (k.kind == kind) l.push_back(&k);
		for (const ObjKey& k : remote)
			if (k.kind == kind) r.push_back(&k);
		if (l.size() != r.size()) d.countMismatch = true;
		const size_t common = std::min(l.size(), r.size());
		for (size_t i = 0; i < common; ++i) {
			if (l[i]->hash != r[i]->hash || l[i]->xhash != r[i]->xhash || l[i]->type != r[i]->type) {
				ObjDiff::Changed c;
				c.local  = *l[i];
				c.remote = *r[i];
				d.changed.push_back(c);
			}
		}
		for (size_t i = common; i < l.size(); ++i) d.onlyLocal.push_back(*l[i]);
		for (size_t i = common; i < r.size(); ++i) d.onlyRemote.push_back(*r[i]);
	}
	return d;
}

std::string format_obj(const ObjRec& r)
{
	char buf[640];
	snprintf(buf, sizeof(buf),
	         "%s ord=%d type=%d state=%d hp=%.9g pos=(%.9g,%.9g,%.9g) rot=(%.9g,%.9g,%.9g) vel=(%.9g,%.9g,%.9g) "
	         "drv=(%.9g,%.9g,%.9g) face=%.9g aux=[%d,%d,%d,%d] hash=%016llx xhash=%016llx",
	         kind_name(r.kind), r.ord, r.type, r.state, (double)r.health, (double)r.f[0], (double)r.f[1],
	         (double)r.f[2], (double)r.f[3], (double)r.f[4], (double)r.f[5], (double)r.f[6], (double)r.f[7],
	         (double)r.f[8], (double)r.f[9], (double)r.f[10], (double)r.f[11], (double)r.f[12], r.aux[0], r.aux[1],
	         r.aux[2], r.aux[3], (unsigned long long)r.hash, (unsigned long long)r.xhash);
	return buf;
}

std::string format_subs(const TickSubs& t)
{
	char buf[320];
	snprintf(buf, sizeof(buf), "%llu %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx",
	         (unsigned long long)t.tick, (unsigned long long)t.total, (unsigned long long)t.subs[0],
	         (unsigned long long)t.subs[1], (unsigned long long)t.subs[2], (unsigned long long)t.subs[3],
	         (unsigned long long)t.subs[4], (unsigned long long)t.subs[5], (unsigned long long)t.subs[6],
	         (unsigned long long)t.xtra);
	return buf;
}

std::string format_key(const ObjKey& k)
{
	char buf[160];
	snprintf(buf, sizeof(buf), "%s#%d(type=%d hash=%016llx xhash=%016llx)", kind_name(k.kind), k.ord, k.type,
	         (unsigned long long)k.hash, (unsigned long long)k.xhash);
	return buf;
}

const char* subs_header()
{
	return "# tick total navi piki teki item world rng mode xtra";
}

} // namespace pc_netplay_forensics
