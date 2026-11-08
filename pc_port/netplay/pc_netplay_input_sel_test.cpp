// Host-run test for --netplay-input
// selection (pc_netplay_input_sel.h, header-only, no SDL/game).
//
// Covers: spec parsing (valid + garbage), the local-pad routing decision
// (auto = slot 0 everywhere; explicit = the session role's slot), and the
// device filter map (a `keyboard` peer ignores every gamepad, a `gamepad`
// peer ignores every key, `auto` ignores nothing).

#include "netplay/pc_netplay_input_sel.h"

#include <cstdio>

namespace {

int sFailures = 0;

void check(bool ok, const char* what, int line)
{
	if (!ok) {
		++sFailures;
		std::printf("FAIL line %d: %s\n", line, what);
	}
}
#define CHECK(ok, what) check((ok), (what), __LINE__)

using pc_netplay_input_sel::Kind;

} // namespace

int main()
{
	using namespace pc_netplay_input_sel;

	// 1. Valid specs.
	{
		Kind k  = kInputAuto;
		int idx = -1;
		CHECK(parse_input_spec("auto", &k, &idx) && k == kInputAuto && idx == 0,
		      "auto parses");
		CHECK(parse_input_spec("keyboard", &k, &idx) && k == kInputKeyboard && idx == 0,
		      "keyboard parses");
		CHECK(parse_input_spec("gamepad", &k, &idx) && k == kInputGamepad && idx == 0,
		      "gamepad defaults to index 0");
		CHECK(parse_input_spec("gamepad:0", &k, &idx) && k == kInputGamepad && idx == 0,
		      "gamepad:0 parses");
		CHECK(parse_input_spec("gamepad:3", &k, &idx) && k == kInputGamepad && idx == 3,
		      "gamepad:3 parses");
	}
	// 2. Garbage is rejected.
	{
		const char* bad[] = {
			"", "Keyboard", "KEYBOARD", "Auto", "gamepad:", "gamepad:x",
			"gamepad:-1", "gamepad:99", "keyboard:0", "auto:1", "pad", "key",
		};
		for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
			Kind k  = kInputAuto;
			int idx = 0;
			char what[96];
			std::snprintf(what, sizeof(what), "garbage rejected: '%s'", bad[i]);
			CHECK(!parse_input_spec(bad[i], &k, &idx), what);
		}
		CHECK(!parse_input_spec(nullptr, nullptr, nullptr), "null rejected");
	}
	// 3. Local-pad routing: auto always samples slot 0 (today's behaviour);
	// an explicit selection samples the session role's slot.
	{
		CHECK(local_pad_index(0, kInputAuto) == 0, "auto host samples pad 0");
		CHECK(local_pad_index(1, kInputAuto) == 0, "auto joiner samples pad 0");
		CHECK(local_pad_index(0, kInputKeyboard) == 0, "keyboard host samples pad 0");
		CHECK(local_pad_index(1, kInputKeyboard) == 1, "keyboard joiner samples pad 1");
		CHECK(local_pad_index(0, kInputGamepad) == 0, "gamepad host samples pad 0");
		CHECK(local_pad_index(1, kInputGamepad) == 1, "gamepad joiner samples pad 1");
	}
	// 4. Device filter map: the `keyboard` peer ignores every gamepad, the
	// `gamepad` peer ignores every key, `auto` ignores nothing.
	{
		bool ik = true, ig = true;
		filter_for_selection(kInputKeyboard, &ik, &ig);
		CHECK(!ik && ig, "keyboard peer: keys kept, pads ignored");
		filter_for_selection(kInputGamepad, &ik, &ig);
		CHECK(ik && !ig, "gamepad peer: keys ignored, pads kept");
		filter_for_selection(kInputAuto, &ik, &ig);
		CHECK(!ik && !ig, "auto peer: nothing ignored");
	}

	if (sFailures == 0) std::printf("pc_netplay_input_sel_test: PASS\n");
	else std::printf("pc_netplay_input_sel_test: %d FAILURES\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
