#pragma once
// Netplay-only failure-path diagnostics.
//
// Compiled into the netplay exe only (PIKMIN_NETPLAY_BUILD=ON, which defines
// PIKI_NETPLAY_BUILD for the game target). Every entry point runs only on a
// path that is about to halt the game, so it never changes a running sim:
// it reads state and writes to stderr, nothing else. The default build does
// not compile or link any of this.
//
//   pc_netplay_diag_bad_action_target  a Piki action's init() got a target it
//                                      cannot use (the "karl caught a cold !"
//                                      halts in ActEnter/ActBou::init): logs
//                                      the site, the target and its object
//                                      type, the Pikmin, the captain it
//                                      follows, each captain's goal item and
//                                      a backtrace.
//   pc_netplay_diag_backtrace          the calling thread's return addresses
//                                      and the exe's load base; rebase them to
//                                      the on-disk image base (0x140000000)
//                                      and `nm -C nectar.exe` names them.
//
// The game TUs (src/, built into the pikmin_legacy library, which never sees
// PIKI_NETPLAY_BUILD) reach pc_netplay_diag_bad_action_target through a weak
// declaration, as System::run reaches pc_netplay_session_drive. The default
// build defines it as a no-op (pc_netplay_stubs.cpp).

class Creature;
class Piki;

void pc_netplay_diag_bad_action_target(const char* site, Piki* piki, Creature* target);
void pc_netplay_diag_backtrace(const char* tag);
