#pragma once

// Simulation vs cosmetic RNG for deterministic netplay.
//
// Two independent streams sharing one MSL-compatible generator:
//   state = state * 1103515245 + 12345; return (state >> 16) & 0x7fff;
//
// When deterministic mode is OFF, pc_sim_rand() and pc_cosmetic_rand() both
// return rand(), in the same call order, so the libc sequence is unchanged.
// When ON, each stream draws from its own state, reseeded per day by
// pc_netplay_det_reseed_for_new_day(). The sim stream feeds everything the
// simulation reads; the cosmetic stream feeds draw/UI-only effects, so a
// local presentation difference never shifts the sim sequence.

#include <cstdlib>

int pc_sim_rand(void); // sim stream; rand() when not deterministic
void pc_sim_srand(unsigned seed);
unsigned pc_sim_rng_state(void);
void pc_sim_rng_set_state(unsigned state);

int pc_cosmetic_rand(void); // draw/UI-only stream; rand() when not deterministic
void pc_cosmetic_srand(unsigned seed);
unsigned pc_cosmetic_rng_state(void);
void pc_cosmetic_rng_set_state(unsigned state);

// Records the calling thread as the RNG owner (pc_netplay_det_init calls it
// from main()). Deterministic draws from any other thread log once to stderr.
void pc_sim_rng_note_main_thread(void);

// One-draw float shapers mirroring System::getRand: max * (draw / RAND_MAX),
// so the off-mode values are bit-identical to the original call.
inline float pc_sim_randf(float max) { return max * (pc_sim_rand() / float(RAND_MAX)); }
inline float pc_cosmetic_randf(float max) { return max * (pc_cosmetic_rand() / float(RAND_MAX)); }
