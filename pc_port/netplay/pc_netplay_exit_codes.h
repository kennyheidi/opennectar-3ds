#pragma once
// Process exit codes of a netplay session. A normal quit is 0 (the window
// closed, or PIKMIN_NETPLAY_EXIT_AFTER_TICKS was reached).

namespace pc_netplay_exit {
constexpr int kTransport = 1; // the socket could not be set up
constexpr int kUsage     = 2; // a bad netplay switch or a setting netplay cannot run with
constexpr int kInputFile = 3; // the scripted input file is missing or malformed
constexpr int kRefused   = 4; // the handshake was refused or timed out
constexpr int kDesync    = 5; // the peers' state hashes differ
constexpr int kReplayMismatch = 8; // --netplay-replay-log: a recorded hash did not reproduce
} // namespace pc_netplay_exit
