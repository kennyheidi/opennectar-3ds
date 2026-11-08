// Host test for the deterministic RNG streams.
//
// Covers the MSL sequence for seed 1, the state
// get/set round-trip, independence of the cosmetic stream, and passthrough
// to rand() when deterministic mode is off.

#include "netplay/pc_sim_rng.h"
#include "netplay/pc_netplay_det.h"

#include <cstdio>
#include <cstdlib>

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
	// _putenv updates both the CRT environ (read by getenv) and the process
	// environment; "NAME=" removes the variable.
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
	// Deterministic phase: MSL-compatible sequence for seed 1.
	setEnv("PIKMIN_NETPLAY_DETERMINISTIC", "1");
	setEnv("PIKMIN_NETPLAY_UNTHROTTLED", nullptr);
	pc_netplay_det_init(0, emptyArgv);
	check(pc_netplay_deterministic(), "deterministic switch must be on with env=1");
	check(!pc_netplay_unthrottled(), "unthrottled must be off without env");

	pc_sim_srand(1);
	const int expected[5] = { 16838, 5758, 10113, 17515, 31051 };
	for (int i = 0; i < 5; ++i) {
		const int got = pc_sim_rand();
		if (got != expected[i]) {
			std::fprintf(stderr, "FAIL: seed-1 output %d: got %d want %d\n", i, got, expected[i]);
			++failures;
		}
		if (got < 0 || got > 0x7fff) {
			std::fprintf(stderr, "FAIL: seed-1 output %d out of range: %d\n", i, got);
			++failures;
		}
	}

	// State get/set round-trip.
	pc_sim_srand(0x12345678u);
	check(pc_sim_rng_state() == 0x12345678u, "state getter must return the seed");
	const int before = pc_sim_rand();
	const unsigned mid  = pc_sim_rng_state();
	const int after     = pc_sim_rand();
	check(mid != pc_sim_rng_state(), "state must advance after a draw");
	pc_sim_rng_set_state(mid);
	check(pc_sim_rand() == after, "restored state must reproduce the next draw");
	(void)before;

	// Independence of the cosmetic stream.
	pc_sim_srand(0x11111111u);
	pc_cosmetic_srand(0x22222222u);
	const unsigned simState0 = pc_sim_rng_state();
	const unsigned cosState0 = pc_cosmetic_rng_state();
	for (int i = 0; i < 16; ++i) pc_sim_rand();
	check(pc_cosmetic_rng_state() == cosState0, "sim draws must not advance the cosmetic stream");
	for (int i = 0; i < 16; ++i) pc_cosmetic_rand();
	check(pc_sim_rng_state() != simState0, "sim stream must have advanced");
	const unsigned simState1 = pc_sim_rng_state();
	for (int i = 0; i < 16; ++i) pc_cosmetic_rand();
	check(pc_sim_rng_state() == simState1, "cosmetic draws must not advance the sim stream");
	pc_sim_srand(0xAAAAAAAAu);
	pc_cosmetic_srand(0xAAAAAAAAu);
	check(pc_sim_rand() == pc_cosmetic_rand(), "same seed must give the same first draw on both streams");

	// Passthrough phase: with the switch off both streams return rand().
	setEnv("PIKMIN_NETPLAY_DETERMINISTIC", nullptr);
	pc_netplay_det_init(0, emptyArgv);
	check(!pc_netplay_deterministic(), "deterministic switch must be off without env/flag");

	std::srand(424242);
	const int simExpected = std::rand();
	const int cosExpected = std::rand();
	std::srand(424242);
	check(pc_sim_rand() == simExpected, "sim stream must passthrough to rand() when off");
	check(pc_cosmetic_rand() == cosExpected, "cosmetic stream must passthrough to rand() when off");

	std::printf("PcSimRng: %s\n", failures ? "FAILED" : "all tests passed");
	return failures ? 1 : 0;
}
