#include "pc_coop_switch.h"
#include "pc_coop.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct CaptainName {
	const char* name;
	int value;
};

// PcCaptain values (pc_coop.h): 0 Olimar, 1 Louie, 2 Red, 3 Yellow, 4 Blue.
const CaptainName kCaptains[] = {
	{ "olimar", 0 },
	{ "louie", 1 },
	{ "pikmin-red", 2 },
	{ "pikmin-yellow", 3 },
	{ "pikmin-blue", 4 },
};

bool parseOne(const char* begin, const char* end, int* out)
{
	// Trim ASCII whitespace; the name itself must match exactly.
	while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
	while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) --end;
	for (const CaptainName& c : kCaptains) {
		const size_t n = std::strlen(c.name);
		if (size_t(end - begin) == n && std::memcmp(begin, c.name, n) == 0) {
			*out = c.value;
			return true;
		}
	}
	return false;
}

bool asciiEqualFold(const char* a, const char* b)
{
	while (*a && *b) {
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z') ca = char(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z') cb = char(cb - 'A' + 'a');
		if (ca != cb) return false;
		++a;
		++b;
	}
	return *a == *b;
}

// Explicit-off words honour PIKMIN_COOP=0 (and friends): a launcher that
// always exports a captain preference must not force co-op on.
bool envIsOffWord(const char* v) { return asciiEqualFold(v, "0") || asciiEqualFold(v, "false") || asciiEqualFold(v, "off") || asciiEqualFold(v, "no") || asciiEqualFold(v, "n"); }

bool envOn(const char* name)
{
	const char* v = std::getenv(name);
	if (!v || !*v) return false;
	return !envIsOffWord(v);
}

void warnBadCaptains(const char* v)
{
	std::printf("[NETPLAY] coop_switch: invalid captains '%s', using olimar,louie\n", v ? v : "");
}

} // namespace

bool pc_coop_switch_parse_captains(const char* text, int* outP1, int* outP2)
{
	if (!text || !outP1 || !outP2) return false;
	const char* comma = std::strchr(text, ',');
	if (!comma || !comma[1] || std::strchr(comma + 1, ',')) return false;
	int p1 = 0, p2 = 0;
	if (!parseOne(text, comma, &p1)) return false;
	if (!parseOne(comma + 1, text + std::strlen(text), &p2)) return false;
	*outP1 = p1;
	*outP2 = p2;
	return true;
}

PcCoopSwitch pc_coop_switch_parse(int argc, char** argv)
{
	PcCoopSwitch sw;
	const char* cliCaptains = nullptr;
	// Only --coop / PIKMIN_COOP arm co-op. Captain values only select
	// captains; PIKMIN_COOP_CAPTAINS alone never arms.
	if (envOn("PIKMIN_COOP")) sw.coop = true;
	if (const char* envCaptains = std::getenv("PIKMIN_COOP_CAPTAINS")) {
		if (*envCaptains) {
			int p1 = sw.captainP1, p2 = sw.captainP2;
			if (pc_coop_switch_parse_captains(envCaptains, &p1, &p2)) {
				sw.captainP1 = p1;
				sw.captainP2 = p2;
			} else if (sw.coop) {
				// Warn only when the switch is otherwise present, so
				// default single-player logs stay unchanged.
				warnBadCaptains(envCaptains);
			}
		}
	}
	for (int i = 1; i < argc; ++i) {
		if (!argv[i]) continue;
		if (std::strcmp(argv[i], "--coop") == 0) {
			sw.coop = true;
		} else if (std::strncmp(argv[i], "--coop-captains=", 16) == 0) {
			cliCaptains = argv[i] + 16;
		}
	}
	if (cliCaptains) {
		// Explicit CLI captains imply co-op and win over the env pair.
		sw.coop = true;
		int p1 = sw.captainP1, p2 = sw.captainP2;
		if (pc_coop_switch_parse_captains(cliCaptains, &p1, &p2)) {
			sw.captainP1 = p1;
			sw.captainP2 = p2;
		} else {
			warnBadCaptains(cliCaptains);
		}
	}
	return sw;
}

namespace {
bool sSwitchActive = false;
} // namespace

void pc_coop_switch_apply(const PcCoopSwitch& sw)
{
	if (!sw.coop) return;
	pc_coop_set_pending(true);
	pc_coop_set_captain(0, sw.captainP1);
	pc_coop_set_captain(1, sw.captainP2);
	sSwitchActive = true;
}

bool pc_coop_switch_active(void) { return sSwitchActive; }

namespace {
int sDirect = 0; // 0 no, 1 requested, 2 on the way to the run, 3 started
} // namespace

void pc_coop_direct_request(void) { sDirect = 1; }
static int sDirectSlot = 0;
void pc_coop_direct_set_slot(int slot) { sDirectSlot = (slot >= 1 && slot <= 3) ? slot : 0; }
int pc_coop_direct_slot(void) { return sDirectSlot; }
bool pc_coop_direct_requested(void) { return sDirect == 1; }

bool pc_coop_direct_take_title(void)
{
	if (sDirect != 1) return false;
	sDirect = 2;
	return true;
}

bool pc_coop_direct_take_setup(void)
{
	if (sDirect != 2) return false;
	sDirect = 3;
	return true;
}

bool pc_coop_direct_active(void) { return sDirect == 3; }
