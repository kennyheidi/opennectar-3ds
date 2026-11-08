// Host test for the netplay sim-visibility policy.
//
// Covers the opt-in contract: with the deterministic switch off every helper
// is the identity (byte-for-byte vanilla behaviour); with the switch on the
// sim always sees visible / 16:9 / near LOD regardless of the frustum input.

#include "netplay/pc_netplay_policy.h"
#include "netplay/pc_netplay_det.h"

#include <cstdio>

static int failures = 0;

static void check(bool condition, const char* message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

static void setEnv(const char* name, const char* value)
{
#if defined(_WIN32)
	char entry[128];
	if (value != nullptr) {
		std::snprintf(entry, sizeof(entry), "%s=%s", name, value);
	} else {
		std::snprintf(entry, sizeof(entry), "%s=", name);
	}
	_putenv(entry);
#else
	if (value != nullptr) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
#endif
}

static char* emptyArgv[] = { nullptr };

int main()
{
	// Switch off: identity pass-through.
	setEnv("PIKMIN_NETPLAY_DETERMINISTIC", nullptr);
	pc_netplay_det_init(0, emptyArgv);
	check(!pc_netplay_deterministic(), "switch must be off without env/flag");
	check(pc_netplay_sim_visible(false) == false, "off: false must pass through");
	check(pc_netplay_sim_visible(true) == true, "off: true must pass through");
	check(pc_netplay_sim_aspect(4.0f / 3.0f) == 4.0f / 3.0f, "off: aspect must pass through");
	check(pc_netplay_sim_aspect(21.0f / 9.0f) == 21.0f / 9.0f, "off: wide aspect must pass through");
	check(pc_netplay_sim_lod_distance(5000.0f) == 5000.0f, "off: lod distance must pass through");
	check(pc_netplay_sim_lod_distance(0.0f) == 0.0f, "off: zero lod distance must pass through");

	// Switch on: pinned sim values regardless of the local view.
	setEnv("PIKMIN_NETPLAY_DETERMINISTIC", "1");
	pc_netplay_det_init(0, emptyArgv);
	check(pc_netplay_deterministic(), "switch must be on with env=1");
	check(pc_netplay_sim_visible(false) == true, "det: culled must become visible");
	check(pc_netplay_sim_visible(true) == true, "det: visible must stay visible");
	check(pc_netplay_sim_aspect(4.0f / 3.0f) == 16.0f / 9.0f, "det: 4:3 aspect must pin to 16:9");
	check(pc_netplay_sim_aspect(800.0f / 600.0f) == 16.0f / 9.0f, "det: window aspect must pin to 16:9");
	check(pc_netplay_sim_lod_distance(5000.0f) == 0.0f, "det: far distance must pin to near LOD");
	check(pc_netplay_sim_lod_distance(100.0f) == 0.0f, "det: near distance must pin to near LOD");

	// Toggling back off restores pass-through (opt-in rule).
	setEnv("PIKMIN_NETPLAY_DETERMINISTIC", nullptr);
	pc_netplay_det_init(0, emptyArgv);
	check(pc_netplay_sim_visible(false) == false, "re-off: false must pass through again");

	std::printf("PcNetplayPolicy: %s\n", failures ? "FAILED" : "all tests passed");
	return failures ? 1 : 0;
}
