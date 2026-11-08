#pragma once
// Netplay lockstep co-op session over GekkoNet and UDP.
//
// Active only when --netplay-host <port> or --netplay-join <ip>:<port> (or
// PIKMIN_NETPLAY_HOST / PIKMIN_NETPLAY_JOIN), --netplay-host-ice /
// --netplay-join-ice (pc_netplay_launch.h) or --netplay-replay-log <file> is
// set. A session forces
// deterministic mode on and boots both peers straight into two-captain story
// co-op with a fresh save (the co-op direct boot, pc_coop_switch.h). Without
// a netplay switch every function here is inert and the game behaves exactly
// as before. The session TU only exists in PIKMIN_NETPLAY_BUILD builds, so
// the engine reaches it through weak references.
//
// Host = player 0 (P1, Olimar's slot), joiner = player 1 (P2).

#include <stdint.h>

class System;
class BaseApp;

// Stores argv and parses the netplay switches. Call once from main(), after
// the other argv parsing and before the game starts. A bad switch exits
// with pc_netplay_exit::kUsage.
void pc_netplay_session_notify_argv(int argc, char** argv);

// True when a netplay switch is set (host or join), even before the session
// starts. False in every non-netplay run.
bool pc_netplay_session_active(void);

// System::run hook. Returns true when netplay owned this loop turn
// (handshake, tick(s), network wait or shutdown).
bool pc_netplay_session_drive(System* sys, BaseApp* app);

// Stage-load hook, called from GameFlow::softReset inside the synchronous
// load (pc_netplay_loadguard.h). Inert outside a session tick.
void pc_netplay_on_stage_load(void);

// Keep-alive entry for long main-thread loops (site =
// pc_netplay_loadguard::Site). Inert outside a session tick.
void pc_netplay_load_keepalive(int site);

// Live session figures for a HUD (presentation only). False outside a
// running session.
struct PcNetplayLiveStats {
	unsigned delay  = 0; // current local input delay (frames)
	int adaptive    = 0; // 1 when the adaptive delay may change it
	float rttLastMs = -1;
	float rttP50Ms  = -1;
	float jitterMs  = 0;
	unsigned stallsLast10s = 0;
	float stallMsLast10s   = 0;
	unsigned long long stallCount = 0;
	float stallTotalMs = 0;
	int stallOpen      = 0; // 1 while the session is waiting right now
};
bool pc_netplay_live_stats(PcNetplayLiveStats* out);

// True on the host of a session.
bool pc_netplay_is_host(void);

// --netplay-replay-log: the recorded sim settings, which the replay adopts at
// settings init (pc_netplay_launch_post_settings). Null otherwise.
const char* pc_netplay_session_replay_settings(void);
// Online guest (--online-join): joins the room and returns the host's sim
// settings to adopt at settings init. Null otherwise.
const char* pc_netplay_session_online_rules(void);
// Online guest: leave co-op (P2 keeps its slot, its input is idle and the
// view follows the host's captain); Start asks to rejoin at a safe tick.
void pc_netplay_online_request_leave(void);
void pc_netplay_online_request_rejoin(void);
bool pc_netplay_online_spectating(void);
bool pc_netplay_online_is_guest(void);
/// Why the last online join failed (--online-error), once; null when none.
const char* pc_netplay_online_take_error(void);
