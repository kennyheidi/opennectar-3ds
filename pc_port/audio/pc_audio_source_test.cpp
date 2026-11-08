// Host test for the netplay audio ownership gate.
//
// Captain-owned, non-positional sounds must reach the mixer only for the
// captain this PC plays in a deterministic netplay session; everything else
// (single player, local split-screen co-op, unattributed callers) stays
// audible exactly as before.
#include <cstdio>
#include <cstdlib>

#define PIKI_PC_PORT 1
#include "audio/pc_audio_source.h"

static int failures = 0;

static void check(bool condition, const char* message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		++failures;
	}
}

namespace {
int sTwoPass = 0;
int sLocal   = 0;
bool sSession = true;
} // namespace

// Netplay builds define this in the session TU; the gate reads it weakly.
bool pc_netplay_session_active(void) { return sSession; }

// The real definitions live in the engine (pc_netplay_present.cpp); the gate
// only needs these two answers.
extern "C" int pc_netplay_present_two_pass_active(void) { return sTwoPass; }
extern "C" int pc_netplay_present_local_player(void) { return sLocal; }

int main()
{
	// Not in a session: every captain is audible, with or without a scope.
	sTwoPass = 0;
	check(pc_audio_source_audible(), "no session, unattributed");
	{
		PcAudioSource captain(1);
		check(pc_audio_source_audible(), "no session (single player / split screen): captain 1 audible");
	}

	// A deterministic replay (two-pass frame, no host/join switch): one human, both captains, nothing muted.
	sTwoPass = 1;
	sLocal   = 0;
	sSession = false;
	{
		PcAudioSource louie(1);
		check(pc_audio_source_audible(), "replay (no session): captain 1 audible with local player 0");
	}
	sSession = true;

	// In a session, host (local 0): captain 0 audible, captain 1 muted.
	sTwoPass = 1;
	sLocal   = 0;
	check(pc_audio_source_audible(), "host: unattributed audible");
	{
		PcAudioSource olimar(0);
		check(pc_audio_source_audible(), "host: own captain audible");
		{
			PcAudioSource louie(1);
			check(!pc_audio_source_audible(), "host: the other captain is muted");
		}
		check(pc_audio_source_audible(), "host: the scope restored the outer captain");
	}
	check(pc_audio_source_captain_slot() == -1, "host: scopes left no captain behind");

	// Joiner (local 1): the other way round.
	sLocal = 1;
	{
		PcAudioSource olimar(0);
		check(!pc_audio_source_audible(), "joiner: captain 0 (the host's) is muted");
	}
	{
		PcAudioSource louie(1);
		check(pc_audio_source_audible(), "joiner: own captain audible");
	}
	check(pc_audio_source_audible(), "joiner: unattributed audible");

	if (failures != 0) {
		std::fprintf(stderr, "pc_audio_source_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("pc_audio_source_test: ok\n");
	return 0;
}
