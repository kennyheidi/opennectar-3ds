#pragma once
// Netplay session HUD and end banner.
//
// Netplay builds only. pc_netplay_hud.cpp draws once per presented frame from
// VIWaitForRetrace (vi_stubs.cpp, after the port's VS HUD), through the port's
// preallocated P2D overlay (pc_settings_p2d: no allocation, no GX state left
// behind). Presentation only: it reads the session's numbers below and the
// local keyboard/pad state, and writes nothing the simulation or the netplay
// input reads.
//
//   HUD     a box on the right edge, below the game's "day N" badge:
//           connection quality (colour), ping and jitter, the current input
//           delay and the stalls of the last 10 s (pc_netplay_hud_model.h).
//   Toggle  F4 on the keyboard, or both stick buttons (L3 + R3) together on
//           the pad; PIKMIN_NETPLAY_HUD=0 starts it hidden.
//   Banner  after a desync or a lost connection the game keeps presenting
//           frames for a while (PIKMIN_NETPLAY_BANNER_MS, default 10 s; any
//           key or button closes it) with what happened and where the
//           forensics went.

#include "netplay/pc_netplay_hud_model.h"

#include <cstdint>

constexpr int kBannerMaxLines = 6;

struct PcNetplayHudInfo {
	bool running = false;       // a session is advancing (the HUD may show)
	bool isHost = false;
	int inputKind = 0;          // pc_netplay_input_sel::Kind of this peer
	uint64_t frame = 0;         // last frame both games agreed on
	pc_netplay_hud::Numbers numbers;
	// Online mode: a fourth line with the session state (waiting, catching
	// up, watching, desync...). tone 0 neutral, 1 warning, 2 error.
	bool online = false;
	char status[64] = {};
	int statusTone  = 0;
	// End banner (the session is over; presented before the process exits).
	bool banner = false;
	bool bannerError = false;   // red title (desync / lost) vs neutral
	double bannerLeftMs = 0;    // until the banner closes by itself
	char bannerTitle[96] = {};
	int bannerLines = 0;
	char bannerLine[kBannerMaxLines][256] = {};
};

// Session side (pc_netplay_session.cpp): false when no netplay session exists.
bool pc_netplay_hud_info(PcNetplayHudInfo* out);

// HUD side (pc_netplay_hud.cpp): called from VIWaitForRetrace.
void pc_netplay_hud_draw(void);
// Whether the HUD box is currently shown (the toggle state).
bool pc_netplay_hud_visible(void);
