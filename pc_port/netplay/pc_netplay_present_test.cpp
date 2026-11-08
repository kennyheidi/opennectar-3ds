// Host test for the two-pass present module: skip-presentation flag, null-GX
// counters and local-player parsing. Engine-free (PC_NETPLAY_PRESENT_HOST=1).

#include <cstdio>

#include "netplay/pc_netplay_present.h"

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

int main()
{
	// Skip flag defaults off, toggles, restores.
	check(pc_netplay_present_skip_presentation() == 0, "skip flag must default off");
	pc_netplay_present_set_skip_presentation(1);
	check(pc_netplay_present_skip_presentation() == 1, "skip flag must read back on");
	pc_netplay_present_set_skip_presentation(0);
	check(pc_netplay_present_skip_presentation() == 0, "skip flag must read back off");

	// Null-GX counters start at zero.
	pc_netplay_present_reset_counters();
	check(pc_netplay_present_null_gl_calls() == 0, "real counter must start at 0");
	check(pc_netplay_present_null_attempted() == 0, "attempted counter must start at 0");
	check(pc_netplay_present_null_active() == 0, "null flag must default off");

	// Attempts accumulate; the real-GL canary increments only via note_real.
	pc_netplay_present_set_null_gx(1);
	check(pc_netplay_present_null_active() == 1, "null flag must read back on");
	pc_netplay_present_note_attempt();
	pc_netplay_present_note_attempt();
	check(pc_netplay_present_null_attempted() == 2, "two attempts must count 2");
	check(pc_netplay_present_null_gl_calls() == 0, "no real GL noted yet");
	pc_netplay_present_note_real();
	check(pc_netplay_present_null_gl_calls() == 1, "note_real must increment the canary");
	check(pc_netplay_present_null_attempted() == 2, "note_real must not touch attempts");
	pc_netplay_present_set_null_gx(0);
	check(pc_netplay_present_null_active() == 0, "null flag must read back off");

	// Reset clears both counters.
	pc_netplay_present_reset_counters();
	check(pc_netplay_present_null_gl_calls() == 0, "reset must clear real");
	check(pc_netplay_present_null_attempted() == 0, "reset must clear attempted");

	// Host builds never run two-pass: single side, never the sim pass.
	check(pc_netplay_present_two_pass_active() == 0, "host: two-pass must be off");
	check(pc_netplay_present_sim_side() == 1, "host: single-pass side must be sim");
	check(pc_netplay_present_sim_pass() == 0, "host: sim pass must be off");

	// Local player parses PIKMIN_NETPLAY_LOCAL_PLAYER=0|1, default 0.
	pc_netplay_present_reset_local_player();
	setEnv("PIKMIN_NETPLAY_LOCAL_PLAYER", nullptr);
	check(pc_netplay_present_local_player() == 0, "local player must default to 0");
	pc_netplay_present_reset_local_player();
	setEnv("PIKMIN_NETPLAY_LOCAL_PLAYER", "1");
	check(pc_netplay_present_local_player() == 1, "local player must parse 1");
	pc_netplay_present_reset_local_player();
	setEnv("PIKMIN_NETPLAY_LOCAL_PLAYER", "0");
	check(pc_netplay_present_local_player() == 0, "local player must parse 0");
	pc_netplay_present_reset_local_player();
	setEnv("PIKMIN_NETPLAY_LOCAL_PLAYER", "2");
	check(pc_netplay_present_local_player() == 0, "local player must reject non-0/1 as 0");
	pc_netplay_present_reset_local_player();
	setEnv("PIKMIN_NETPLAY_LOCAL_PLAYER", nullptr);

	std::printf("PcNetplayPresent: %s\n", failures ? "FAILED" : "all tests passed");
	return failures ? 1 : 0;
}
