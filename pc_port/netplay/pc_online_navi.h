#pragma once
// Engine side of online captain replication (pc_netplay_session.cpp, online
// mode): reads and writes one captain's transform. Player 0 is the host's
// captain (P1), player 1 the guest's (P2). Only valid between ticks.

struct PcOnlineCaptain {
	float position[3] = { 0, 0, 0 };
	float velocity[3] = { 0, 0, 0 };
	float faceDirection = 0;
};

// False while that captain does not exist (title, loading, single player).
bool pc_online_read_captain(int player, PcOnlineCaptain* out);
// Replaces position, velocity and facing (remote captain on the guest).
bool pc_online_write_captain(int player, const PcOnlineCaptain& in);
// Moves the captain by an offset (prediction correction); with
// resetVelocity also stops it, for snaps.
bool pc_online_nudge_captain(int player, float dx, float dy, float dz, bool resetVelocity);
// Diagnostics: GameCoreSection's pause flags in the low 16 bits, bit 16 set
// while a screen is drawn over gameplay (pause menu, ship text...).
int pc_online_pause_state(void);
