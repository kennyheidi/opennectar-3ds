#pragma once
// Netplay one-command launch: --netplay-host-ice / --netplay-join-ice.
//
// Netplay builds only. Resolved before engine init:
//   1. pc_netplay_launch_preinit(), right after the settings CLI in main():
//      - parses the launch switches and refuses bad combinations;
//      - joiner: reads the host's offer code exactly once (the code itself,
//        @file, or @clipboard) and decodes its session bundle: the netplay
//        seed, the host's sim settings and the session descriptor (captains);
//      - creates this run's private folder (per run and per peer) under
//        --netplay-run-root, <exe dir>/netplay, or %LOCALAPPDATA%/Nectar/netplay
//        when the exe folder is read-only, and points NECTAR_SAVE_DIR at its
//        save/ folder, so the session's card never touches the player's own;
//      - joiner: PIKMIN_NETPLAY_SEED becomes the bundle's seed.
//   2. pc_netplay_launch_post_settings(), right after pc_settings_init(): the
//      joiner adopts the host's sim settings for this session only (never
//      written to its settings file), so a different preference on the
//      joiner's PC cannot refuse the session.
//   3. pc_netplay_session_drive(): the code exchange over ICE, then the
//      lockstep session as with --netplay-host/--netplay-join.
//
// Switches:
//   --netplay-host-ice                    host; prints the offer code
//   --netplay-join-ice <code|@file|@clipboard>
//                                         joiner; prints the answer code
//   --netplay-code-out <file>             also write this side's code there
//   --netplay-answer-in <file>            host: poll this file for the answer
//                                         (otherwise the console, stdin)
//   --netplay-run-root <dir>              where run folders are created
// Each code is also copied to the clipboard unless PIKMIN_NETPLAY_NO_CLIPBOARD=1.
// --netplay-input works as in every netplay mode.

#include <cstdint>
#include <string>

struct PcNetplayLaunch {
	bool active = false; // --netplay-host-ice or --netplay-join-ice
	bool isHost = false;
	std::string runDir;   // absolute; private to this run and this peer
	std::string saveDir;  // <runDir>/save (NECTAR_SAVE_DIR)
	std::string codeOut;  // --netplay-code-out
	std::string answerIn; // --netplay-answer-in (host)
	std::string offerCode; // joiner: the offer text, read exactly once
	uint32_t seed = 0;     // host: PIKMIN_NETPLAY_SEED or 0; joiner: the bundle's
	std::string configText; // joiner: the host's sim settings text
	std::string descriptor; // joiner: the host's session descriptor
	bool haveCaptains = false; // joiner: captains carried in the descriptor
	int captainP1 = 0;
	int captainP2 = 1;
	int mode = 0;            // joiner: the descriptor's mode (1 co-op, 2 VS; 0 = none named, co-op)
	std::string vsMapJson;   // joiner, VS: the host's map.json (empty when it did not fit)
};

void pc_netplay_launch_preinit(int argc, char** argv);
void pc_netplay_launch_post_settings(void);
const PcNetplayLaunch& pc_netplay_launch_setup(void);

// The session descriptor the host puts in its offer: "key=value;" text,
//   captainP1=<n>;captainP2=<n>;            every session
//   mode=vs;                                VS (no mode key: co-op)
//   vsMapJson=<the map.json, to the end>    VS, when it fits the offer
// A co-op descriptor is the same as before VS existed.
std::string pc_netplay_launch_descriptor(int captainP1, int captainP2, int mode = 1,
                                         const std::string& vsMapJson = std::string());
// Parses a descriptor's captains. False when it names none (both stay put).
bool pc_netplay_launch_parse_descriptor(const std::string& text, int* captainP1, int* captainP2);
// The descriptor's mode: 2 for VS, 1 otherwise; *json gets its map.json (or "").
int pc_netplay_launch_parse_mode(const std::string& text, std::string* vsMapJson);
// Largest map.json the offer carries (bytes).
constexpr size_t kPcNetplayVsMapJsonMax = 15000;
