// Inert definitions of the netplay hooks for the default build.
//
// The engine calls the netplay hooks through weak declarations and skips a
// call when the address is null. An ELF link resolves an undefined weak
// reference to null, which is how the default build linked without these
// definitions. Mach-O rejects an undefined weak reference at link time, so each
// hook is defined here as a no-op, which matches the null path. This file is
// compiled only when PIKMIN_NETPLAY_BUILD is OFF. The netplay build compiles
// the real definitions instead.

#include "pc_netplay_diag.h"
#include "pc_netplay_hud.h"
#include "pc_netplay_launch.h"
#include "pc_netplay_session.h"
#include "pc_online_menu.h"

void pc_netplay_session_notify_argv(int, char**) {}
bool pc_netplay_session_active(void) { return false; }
bool pc_netplay_session_drive(System*, BaseApp*) { return false; }
void pc_netplay_on_stage_load(void) {}
void pc_netplay_load_keepalive(int) {}
void pc_netplay_launch_preinit(int, char**) {}
void pc_netplay_launch_post_settings(void) {}
void pc_netplay_hud_draw(void) {}
void pc_netplay_diag_bad_action_target(const char*, Piki*, Creature*) {}
void pc_netplay_online_request_leave(void) {}
void pc_netplay_online_request_rejoin(void) {}
bool pc_netplay_online_spectating(void) { return false; }
bool pc_netplay_online_is_guest(void) { return false; }

// Title Local/Online choice (pc_online_menu.cpp). available() and active()
// return false, so the title never opens the choice. update() and open() are
// never reached, and draw() does nothing.
bool pc_online_menu_available(void) { return false; }
void pc_online_menu_open(bool) {}
bool pc_online_menu_active(void) { return false; }
int pc_online_menu_update(Controller*) { return PC_ONLINE_MENU_LOCAL; }
void pc_online_menu_draw(void) {}
