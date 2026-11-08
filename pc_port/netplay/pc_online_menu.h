#pragma once
// Title-screen choice between local and online play for Co-op and VS
// (netplay builds with the online transport only; the title reaches it
// through weak references, so other builds go straight to local play).
//
//   Co-op / VS -> Local | Online
//   Online     -> Create room (Co-op: new game or continue file 1-3) | Join
//
// Online restarts the game with the online switches: the host copies its
// code to the clipboard (--online-code-clipboard), the guest pastes the code
// it was sent and passes it in a private file read once (--online-code-once).

class Controller;

enum PcOnlineMenuResult {
	PC_ONLINE_MENU_OPEN   = 0, ///< still showing
	PC_ONLINE_MENU_LOCAL  = 1, ///< play locally, as before
	PC_ONLINE_MENU_CANCEL = 2, ///< back to the title menu
};

/// True when this build and platform can start an online session from here.
bool pc_online_menu_available(void);
void pc_online_menu_open(bool vs);
bool pc_online_menu_active(void);
/// Title input while open. Choosing an online option restarts the game.
int pc_online_menu_update(Controller* input);
/// Drawn over the game before the blit (vi_stubs).
void pc_online_menu_draw(void);
