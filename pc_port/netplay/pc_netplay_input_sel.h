#pragma once
// Netplay per-peer input ownership selection.
//
// --netplay-input keyboard|gamepad[:N]|auto selects which physical device
// feeds this peer's local player through the PcNetplayAccum path:
//   keyboard  this peer plays on keys (every gamepad is ignored)
//   gamepad   this peer plays on a pad (every key is ignored); :N picks the
//             Nth open gamepad (0 = first, the default when omitted)
//   auto      today's behaviour (no change)
//
// Engine-free and pure (no SDL, no game), so the host-run test links only
// this header. pc_window.cpp owns the SDL-side filter flags; the session
// maps the parsed selection onto them with netplay_filter_for_selection().

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pc_netplay_input_sel {

enum Kind {
	kInputAuto     = 0,
	kInputKeyboard = 1,
	kInputGamepad  = 2,
};

// Parses a --netplay-input value (exact, case-sensitive). Returns true on
// success with *kind set; *index is the gamepad index (0 when omitted).
// "keyboard" and "auto" reject any ":N" suffix.
inline bool parse_input_spec(const char* text, Kind* kind, int* index)
{
	if (text == nullptr || kind == nullptr || index == nullptr) return false;
	if (strcmp(text, "auto") == 0) {
		*kind  = kInputAuto;
		*index = 0;
		return true;
	}
	if (strcmp(text, "keyboard") == 0) {
		*kind  = kInputKeyboard;
		*index = 0;
		return true;
	}
	const char* prefix = "gamepad";
	const size_t n     = strlen(prefix);
	if (strncmp(text, prefix, n) != 0) return false;
	if (text[n] == '\0') {
		*kind  = kInputGamepad;
		*index = 0;
		return true;
	}
	if (text[n] != ':') return false;
	const char* num = text + n + 1;
	if (*num == '\0') return false;
	long v = 0;
	for (const char* p = num; *p != '\0'; ++p) {
		if (*p < '0' || *p > '9') return false;
		v = v * 10 + (*p - '0');
		if (v > 16) return false; // more pads than any host will ever open
	}
	*kind  = kInputGamepad;
	*index = (int)v;
	return true;
}

// Which local PADStatus slot the session samples for its local input:
// auto keeps today's behaviour (slot 0 on both peers); an explicit
// selection samples the session role's slot (host = 0, joiner = 1), which
// the device assignment routes the selected device to.
inline int local_pad_index(int sessionRole, Kind kind)
{
	if (kind == kInputAuto) return 0;
	return (sessionRole != 0) ? 1 : 0;
}

// Maps a selection onto the pc_window filter flags (ignoreKeyboard,
// ignoreGamepads). keyboard ignores every gamepad; gamepad ignores every
// key (the mouse is keyboard-family); auto ignores nothing.
inline void filter_for_selection(Kind kind, bool* ignoreKeyboard, bool* ignoreGamepads)
{
	if (ignoreKeyboard != nullptr) *ignoreKeyboard = (kind == kInputGamepad);
	if (ignoreGamepads != nullptr) *ignoreGamepads = (kind == kInputKeyboard);
}

} // namespace pc_netplay_input_sel
