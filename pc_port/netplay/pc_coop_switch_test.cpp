// Host test for the direct-boot co-op switch: the pure
// "<p1>,<p2>" captain parse (valid, invalid, default) plus the CLI/env
// parse and the apply step. No game, no window, no assets.

#include "pc_coop_switch.h"
#include "pc_coop.h"

#include <cstdio>
#include <cstdlib>

// The co-op state the switch arms (pc_coop.cpp needs the game's model code).
namespace {
bool sFakePending = false;
int sFakeCaptain[2] = { PC_CAPTAIN_OLIMAR, PC_CAPTAIN_LOUIE };
} // namespace
void pc_coop_set_pending(bool on) { sFakePending = on; }
bool pc_coop_pending(void) { return sFakePending; }
void pc_coop_set_captain(int player, int captain)
{
	if (player >= 0 && player < 2) sFakeCaptain[player] = captain;
}
int pc_coop_captain(int player) { return (player >= 0 && player < 2) ? sFakeCaptain[player] : PC_CAPTAIN_OLIMAR; }

namespace {

int sFailures = 0;

void check(bool condition, const char* what)
{
	if (!condition) {
		std::printf("FAIL: %s\n", what);
		++sFailures;
	}
}

// setenv/unsetenv are not visible under this toolchain's C++ headers, so go
// through _putenv_s on Windows (empty value reads back as unset for the
// switch parse) and setenv/unsetenv elsewhere.
void setCoopEnv(const char* name, const char* value)
{
#if defined(_WIN32) || defined(__MINGW32__)
	_putenv_s(name, value ? value : "");
#else
	if (value)
		setenv(name, value, 1);
	else
		unsetenv(name);
#endif
}

void clearCoopEnv()
{
	setCoopEnv("PIKMIN_COOP", nullptr);
	setCoopEnv("PIKMIN_COOP_CAPTAINS", nullptr);
}

} // namespace

int main()
{
	int p1 = -1, p2 = -1;

	// Valid captain strings.
	check(pc_coop_switch_parse_captains("olimar,louie", &p1, &p2) && p1 == 0 && p2 == 1,
	      "olimar,louie parses to 0,1");
	check(pc_coop_switch_parse_captains("pikmin-red,pikmin-blue", &p1, &p2) && p1 == 2 && p2 == 4,
	      "pikmin-red,pikmin-blue parses to 2,4");
	check(pc_coop_switch_parse_captains("pikmin-yellow,louie", &p1, &p2) && p1 == 3 && p2 == 1,
	      "pikmin-yellow,louie parses to 3,1");
	check(pc_coop_switch_parse_captains("louie,louie", &p1, &p2) && p1 == 1 && p2 == 1,
	      "same captain twice is allowed");
	check(pc_coop_switch_parse_captains("  olimar , louie ", &p1, &p2) && p1 == 0 && p2 == 1,
	      "surrounding spaces are trimmed");

	// Invalid captain strings: false, outputs untouched.
	p1 = 0;
	p2 = 1;
	check(!pc_coop_switch_parse_captains("olimar,luigi", &p1, &p2) && p1 == 0 && p2 == 1,
	      "unknown captain refused, outputs kept");
	check(!pc_coop_switch_parse_captains("olimar", &p1, &p2) && p1 == 0 && p2 == 1,
	      "missing comma refused");
	check(!pc_coop_switch_parse_captains("olimar,", &p1, &p2) && p1 == 0 && p2 == 1,
	      "empty second captain refused");
	check(!pc_coop_switch_parse_captains(",louie", &p1, &p2) && p1 == 0 && p2 == 1,
	      "empty first captain refused");
	check(!pc_coop_switch_parse_captains("olimar,louie,louie", &p1, &p2) && p1 == 0 && p2 == 1,
	      "extra comma refused");
	check(!pc_coop_switch_parse_captains("", &p1, &p2) && p1 == 0 && p2 == 1,
	      "empty string refused");
	check(!pc_coop_switch_parse_captains(nullptr, &p1, &p2) && p1 == 0 && p2 == 1,
	      "null string refused");
	check(!pc_coop_switch_parse_captains("Olimar,Louie", &p1, &p2) && p1 == 0 && p2 == 1,
	      "wrong case refused");

	// Default: no flags, no env.
	clearCoopEnv();
	{
		char* argv[] = { (char*)"nectar" };
		PcCoopSwitch sw = pc_coop_switch_parse(1, argv);
		check(!sw.coop && sw.captainP1 == 0 && sw.captainP2 == 1, "default is single-player olimar,louie");
	}

	// CLI --coop alone.
	{
		char prog[] = "nectar", flag[] = "--coop";
		char* argv[] = { prog, flag };
		PcCoopSwitch sw = pc_coop_switch_parse(2, argv);
		check(sw.coop && sw.captainP1 == 0 && sw.captainP2 == 1, "--coop keeps default captains");
	}

	// CLI --coop-captains implies co-op.
	{
		char prog[] = "nectar", flag[] = "--coop-captains=pikmin-blue,olimar";
		char* argv[] = { prog, flag };
		PcCoopSwitch sw = pc_coop_switch_parse(2, argv);
		check(sw.coop && sw.captainP1 == 4 && sw.captainP2 == 0, "--coop-captains implies co-op");
	}

	// CLI --coop-captains with a bad value: co-op on, default captains.
	{
		char prog[] = "nectar", flag[] = "--coop-captains=bob,olimar";
		char* argv[] = { prog, flag };
		PcCoopSwitch sw = pc_coop_switch_parse(2, argv);
		check(sw.coop && sw.captainP1 == 0 && sw.captainP2 == 1, "bad CLI captains fall back to default");
	}

	// Env PIKMIN_COOP=1.
	setCoopEnv("PIKMIN_COOP", "1");
	{
		char* argv[] = { (char*)"nectar" };
		PcCoopSwitch sw = pc_coop_switch_parse(1, argv);
		check(sw.coop && sw.captainP1 == 0 && sw.captainP2 == 1, "PIKMIN_COOP=1 arms co-op");
	}
	clearCoopEnv();

	// Env captains pair alone selects captains but does not arm co-op.
	setCoopEnv("PIKMIN_COOP_CAPTAINS", "louie,pikmin-yellow");
	{
		char* argv[] = { (char*)"nectar" };
		PcCoopSwitch sw = pc_coop_switch_parse(1, argv);
		check(!sw.coop && sw.captainP1 == 1 && sw.captainP2 == 3, "PIKMIN_COOP_CAPTAINS alone selects pair without arming");
	}
	clearCoopEnv();

	// Env captains invalid alone: single-player, defaults kept, silent.
	setCoopEnv("PIKMIN_COOP_CAPTAINS", "bob");
	{
		char* argv[] = { (char*)"nectar" };
		PcCoopSwitch sw = pc_coop_switch_parse(1, argv);
		check(!sw.coop && sw.captainP1 == 0 && sw.captainP2 == 1, "bad env captains alone keep defaults without arming");
	}
	clearCoopEnv();

	// Env PIKMIN_COOP=1 plus a captains pair: armed with the pair.
	setCoopEnv("PIKMIN_COOP", "1");
	setCoopEnv("PIKMIN_COOP_CAPTAINS", "louie,pikmin-yellow");
	{
		char* argv[] = { (char*)"nectar" };
		PcCoopSwitch sw = pc_coop_switch_parse(1, argv);
		check(sw.coop && sw.captainP1 == 1 && sw.captainP2 == 3, "PIKMIN_COOP=1 with env pair arms with pair");
	}
	clearCoopEnv();

	// Explicit off wins over a captain preference.
	setCoopEnv("PIKMIN_COOP", "0");
	setCoopEnv("PIKMIN_COOP_CAPTAINS", "louie,louie");
	{
		char* argv[] = { (char*)"nectar" };
		PcCoopSwitch sw = pc_coop_switch_parse(1, argv);
		check(!sw.coop, "PIKMIN_COOP=0 honours explicit off despite captains");
	}
	clearCoopEnv();
	setCoopEnv("PIKMIN_COOP", "false");
	{
		char* argv[] = { (char*)"nectar" };
		PcCoopSwitch sw = pc_coop_switch_parse(1, argv);
		check(!sw.coop, "PIKMIN_COOP=false is off");
	}
	clearCoopEnv();
	setCoopEnv("PIKMIN_COOP", "off");
	{
		char* argv[] = { (char*)"nectar" };
		PcCoopSwitch sw = pc_coop_switch_parse(1, argv);
		check(!sw.coop, "PIKMIN_COOP=off is off");
	}
	clearCoopEnv();

	// Explicit CLI captains win over the env pair.
	setCoopEnv("PIKMIN_COOP_CAPTAINS", "louie,louie");
	{
		char prog[] = "nectar", flag[] = "--coop-captains=olimar,pikmin-red";
		char* argv[] = { prog, flag };
		PcCoopSwitch sw = pc_coop_switch_parse(2, argv);
		check(sw.coop && sw.captainP1 == 0 && sw.captainP2 == 2, "CLI captains win over env");
	}
	clearCoopEnv();

	// Unrelated flags are ignored.
	{
		char prog[] = "nectar", a[] = "--netplay-host", b[] = "7000", c[] = "--coop";
		char* argv[] = { prog, a, b, c };
		PcCoopSwitch sw = pc_coop_switch_parse(4, argv);
		check(sw.coop, "other flags do not disturb the parse");
	}

	// Apply with no switch: nothing armed.
	{
		pc_coop_set_pending(false);
		char* argv[] = { (char*)"nectar" };
		PcCoopSwitch sw = pc_coop_switch_parse(1, argv);
		pc_coop_switch_apply(sw);
		check(!pc_coop_pending(), "apply without switch leaves pending off");
		check(!pc_coop_switch_active(), "inactive without switch");
	}

	// Apply with the switch: pending + captains.
	{
		char prog[] = "nectar", flag[] = "--coop-captains=pikmin-blue,pikmin-red";
		char* argv[] = { prog, flag };
		PcCoopSwitch sw = pc_coop_switch_parse(2, argv);
		pc_coop_switch_apply(sw);
		check(pc_coop_pending(), "apply arms pending");
		check(pc_coop_captain(0) == 4 && pc_coop_captain(1) == 2, "apply sets both captains");
		check(pc_coop_switch_active(), "active once applied");
		pc_coop_set_pending(false); // leave no state for other tests (own process anyway)
	}

	if (sFailures == 0) {
		std::printf("pc_coop_switch_test: all checks passed\n");
		return 0;
	}
	std::printf("pc_coop_switch_test: %d failure(s)\n", sFailures);
	return 1;
}
