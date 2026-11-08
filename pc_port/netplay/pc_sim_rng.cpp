// Simulation vs cosmetic RNG for deterministic netplay. Engine-free.

#include "netplay/pc_sim_rng.h"
#include "netplay/pc_netplay_det.h"

#include <cstdio>
#include <mutex>
#include <thread>

namespace {

std::mutex sMutex;
unsigned sSimState      = 1;
unsigned sCosmeticState = 0xC05E77u;
std::thread::id sOwner;
bool sRecorded = false;
bool sWarned   = false;

unsigned next(unsigned& state)
{
	state = state * 1103515245u + 12345u;
	return (state >> 16) & 0x7fffu;
}

int draw(bool cosmetic)
{
	if (!pc_netplay_deterministic()) return std::rand();
	std::lock_guard<std::mutex> lock(sMutex);
	if (sRecorded && sOwner != std::this_thread::get_id() && !sWarned) {
		// A draw from another thread would make the sequence depend on
		// thread timing. Serialised by the mutex, but still reported.
		sWarned = true;
		std::fprintf(stderr, "[netplay-det] RNG draw from a non-main thread\n");
	}
	return static_cast<int>(next(cosmetic ? sCosmeticState : sSimState));
}

void seed(bool cosmetic, unsigned value)
{
	std::lock_guard<std::mutex> lock(sMutex);
	(cosmetic ? sCosmeticState : sSimState) = value;
}

} // namespace

void pc_sim_rng_note_main_thread(void)
{
	std::lock_guard<std::mutex> lock(sMutex);
	sOwner    = std::this_thread::get_id();
	sRecorded = true;
}

int pc_sim_rand(void) { return draw(false); }
int pc_cosmetic_rand(void) { return draw(true); }
void pc_sim_srand(unsigned value) { seed(false, value); }
void pc_cosmetic_srand(unsigned value) { seed(true, value); }
void pc_sim_rng_set_state(unsigned value) { seed(false, value); }
void pc_cosmetic_rng_set_state(unsigned value) { seed(true, value); }

unsigned pc_sim_rng_state(void)
{
	std::lock_guard<std::mutex> lock(sMutex);
	return sSimState;
}

unsigned pc_cosmetic_rng_state(void)
{
	std::lock_guard<std::mutex> lock(sMutex);
	return sCosmeticState;
}
