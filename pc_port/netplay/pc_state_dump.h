#pragma once
// Netplay desync forensics: per-tick object records of the
// simulation state the seven-column state hash covers, kept in a ring so a
// desync report can name the objects that differ and dump their fields for the
// tick where the peers first disagreed (the desync is only reported several
// frames later).
//
// Read-only. Nothing here writes sim state, calls the sim RNG or changes the
// hash columns: the walkers mirror pc_state_hash.cpp field for field and
// rebuild the same FNV-1a byte stream, so the records reproduce the navi, piki,
// teki, item and world sub-hashes exactly. pc_state_dump_capture() compares its
// rebuilt columns with the real ones; a mismatch is logged once (the dump then
// would not cover what the hash covers). Also computes `xtra`, a separate
// hash over sim fields the seven columns do not mix (see
// pc_netplay_forensics.h); it is not part of the total and GekkoNet never
// sees it.
//
// Netplay build only (the session calls it after pc_state_hash_tick_end()).
// Cost: one extra read-only walk of the live objects per tick and one memcpy
// of ~100 B per object into the ring; off unless enabled.

#include "netplay/pc_netplay_forensics.h"

#include <cstdint>
#include <cstdio>
#include <vector>

constexpr size_t kStateDumpRingTicks = 256; // about 8.5 s at 30 Hz (a desync is reported several frames, or after a long load, later)

void pc_state_dump_set_enabled(bool on);
bool pc_state_dump_enabled(void);

// Call right after pc_state_hash_tick_end() for the same tick, with that tick's
// seven columns (navi piki teki item world rng rand). Fills the ring slot for
// `tick` and the last-xtra value.
void pc_state_dump_capture(uint64_t tick, const uint64_t subs[7]);

// The xtra hash of the last captured tick (0 before the first capture).
uint64_t pc_state_dump_last_xtra(void);

// The records captured for `tick`, or null when it has left the ring (or was
// never captured). The pointer is valid until the next capture.
const std::vector<pc_netplay_forensics::ObjRec>* pc_state_dump_find(uint64_t tick);

// xtra hash captured for `tick`; false when not in the ring.
bool pc_state_dump_xtra_of(uint64_t tick, uint64_t* xtra);

// Oldest tick still in the ring (0 when empty).
uint64_t pc_state_dump_oldest_tick(void);

// Writes the records of `tick` (one pc_netplay_forensics::format_obj line
// each, preceded by a header comment) to `f`. False when the tick is not in
// the ring.
bool pc_state_dump_write_tick(FILE* f, uint64_t tick);

// TEST ONLY (PIKMIN_NETPLAY_TEST_DESYNC_NUDGE in the session, with
// PIKMIN_NETPLAY_TEST_KNOBS=1): adds 1.0 to the x position of the ord-th object of `kind`
// (pc_netplay_forensics::kNavi / kPiki / kTeki). False when there is none.
bool pc_state_dump_test_nudge(int kind, int ord);

// Number of ticks whose rebuilt columns did not match the real hash (0 when the
// dump covers the hash).
unsigned pc_state_dump_coverage_mismatches(void);
