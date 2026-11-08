// Netplay session HUD and end banner. See pc_netplay_hud.h. Netplay builds
// only; drawn from VIWaitForRetrace once per presented frame, after the
// frame's sim work.
//
// Presentation only. It draws through the port's P2D overlay, whose panes,
// font and plate textures pc_settings_p2d_init() allocated once at start-up,
// so a HUD frame allocates nothing (the peers' heaps stay in step whether or
// not a player shows the HUD). It reads the session's numbers
// (pc_netplay_hud_info), the local keyboard state and the local pad's two
// stick buttons, and writes nothing the simulation or the netplay input reads.
//
// Toggle: F4, or L3 + R3 together on the pad. Checked against the bindings:
// no default keyboard action and no port hotkey uses F4 (F1 settings, F2
// control mode, F3 photo mode, F5-F9 debug/VS; the default layout's keys are
// Space, Shift, X, Y, Z, Enter, Q, E, arrows, WASD, TFGH, C, R, V), and F4 is
// ignored while a player has bound it to an action. Every standard pad button
// already has a default binding (A, B, X, Y, the shoulders, Start, the D-pad,
// Back opens F1, L3 first person, R3 lock-on; Guide belongs to the OS), so
// the pad toggle is the two-stick chord: first person and lock-on are
// session-locked sim settings, off unless the host turned them on, so in a
// default session the chord does nothing else. When either is on (or a
// player bound another action to a stick button) the chord is ignored and
// only F4 toggles (stick_buttons_in_use). A keyboard-only peer
// (--netplay-input keyboard) ignores the chord, since pads need no focus and
// the local two-window test gives the pad to the other window.

#include "netplay/pc_netplay_hud.h"

#include "netplay/pc_netplay_input_sel.h"
#include "netplay/pc_netplay_present.h"
#include "settings/pc_glass_menu.h"
#include "settings/pc_settings.h"
#include "settings/pc_settings_p2d.h"
#include "gl/pc_gfx.h"
#include "gl/pc_opengl.h"
#include "pc_window.h"

#include "Colour.h"
#include "Geometry.h"
#include "Graphics.h"
#include "Matrix4f.h"
#include "NaviMgr.h"
#include "NaviState.h"
#include "system.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool sInit       = false;
bool sVisible    = true;
pc_netplay_hud::Toggle sKeyEdge;
pc_netplay_hud::Toggle sPadEdge;
// TEST knobs (PIKMIN_NETPLAY_TEST_KNOBS=1 only): PIKMIN_NETPLAY_TEST_HUD_TOGGLE_FRAME=<f>
// toggles once at frame f (the toggle path in a scripted pair);
// PIKMIN_NETPLAY_TEST_HUD_SHOT=<file.bmp> captures the frame with the HUD at
// PIKMIN_NETPLAY_TEST_HUD_SHOT_FRAME (default 900), and
// PIKMIN_NETPLAY_TEST_BANNER_SHOT=<file.bmp> the end banner's 10th frame.
long long sTestToggleFrame = -1;
bool sTestToggled          = false;
std::string sShotFile, sBannerShotFile;
long long sShotFrame = 900;
bool sShotDone = false, sBannerShotDone = false;
int sBannerFrames = 0;

// The current framebuffer (the viewport) as a 24-bit BMP.
void write_shot(const std::string& path)
{
	pc_gfx_flush_batch(); // the last queued draws (the final text line) first
	GLint vp[4] = { 0, 0, 0, 0 };
	glGetIntegerv(GL_VIEWPORT, vp);
	const int w = vp[2], h = vp[3];
	if (w <= 0 || h <= 0) return;
	std::vector<unsigned char> rgba((size_t)w * h * 4);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(vp[0], vp[1], w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	const int row = (w * 3 + 3) & ~3;
	const unsigned size = 54u + (unsigned)row * (unsigned)h;
	unsigned char hdr[54] = { 'B', 'M' };
	auto put32 = [&](int at, unsigned v) {
		for (int i = 0; i < 4; ++i) hdr[at + i] = (unsigned char)((v >> (8 * i)) & 0xFF);
	};
	put32(2, size);
	put32(10, 54);
	put32(14, 40);
	put32(18, (unsigned)w);
	put32(22, (unsigned)h);
	hdr[26] = 1;
	hdr[28] = 24;
	FILE* f = std::fopen(path.c_str(), "wb");
	if (f == nullptr) return;
	std::fwrite(hdr, 1, sizeof(hdr), f);
	std::vector<unsigned char> line((size_t)row, 0);
	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			const unsigned char* p = &rgba[((size_t)y * w + x) * 4];
			line[x * 3 + 0] = p[2];
			line[x * 3 + 1] = p[1];
			line[x * 3 + 2] = p[0];
		}
		std::fwrite(line.data(), 1, line.size(), f);
	}
	std::fclose(f);
	printf("[netplay] hud: test shot %dx%d written to %s\n", w, h, path.c_str());
	fflush(stdout);
}

void init_once()
{
	if (sInit) return;
	sInit = true;
	const char* hud = std::getenv("PIKMIN_NETPLAY_HUD");
	if (hud != nullptr && hud[0] == '0' && hud[1] == '\0') sVisible = false;
	const char* knobs = std::getenv("PIKMIN_NETPLAY_TEST_KNOBS");
	if (knobs != nullptr && std::strcmp(knobs, "1") == 0) {
		if (const char* t = std::getenv("PIKMIN_NETPLAY_TEST_HUD_TOGGLE_FRAME")) sTestToggleFrame = std::atoll(t);
		if (const char* s = std::getenv("PIKMIN_NETPLAY_TEST_HUD_SHOT")) sShotFile = s;
		if (const char* s = std::getenv("PIKMIN_NETPLAY_TEST_HUD_SHOT_FRAME")) sShotFrame = std::atoll(s);
		if (const char* s = std::getenv("PIKMIN_NETPLAY_TEST_BANNER_SHOT")) sBannerShotFile = s;
	}
	printf("[netplay] hud: %s (F4 or L3+R3 toggles it)\n", sVisible ? "on" : "off");
	fflush(stdout);
}

// Once, when the chord's state first differs from the default: say why only
// F4 toggles now (first person or lock-on is on, or a stick button is bound).
bool sChordNoteDone = false;
void note_chord_state(bool inUse)
{
	if (sChordNoteDone || !inUse) return;
	sChordNoteDone = true;
	printf("[netplay] hud: L3+R3 does not toggle the HUD in this session (a stick button is in use: first person, "
	       "lock-on or a binding); F4 does\n");
	fflush(stdout);
}

bool f4_bound()
{
	for (int a = 0; a < PC_KEY_ACT_COUNT; ++a) {
		if ((int)pc_window_get_key_binding(a) == (int)SDL_SCANCODE_F4) return true;
	}
	return false;
}

// Fix round 1 (review): whether a stick button (L3 or R3) does something in
// the game, so the chord would fire it too: any pad action bound to it,
// except first person and lock-on while those session-locked settings are
// off (then those actions do nothing). The HUD then ignores the chord (F4
// still works), as it ignores F4 while F4 is bound.
bool stick_buttons_in_use()
{
	for (int a = 0; a < PC_KEY_ACT_COUNT; ++a) {
		const int b = pc_window_get_gamepad_binding(a);
		if (b != SDL_CONTROLLER_BUTTON_LEFTSTICK && b != SDL_CONTROLLER_BUTTON_RIGHTSTICK) continue;
		if (a == PC_KEY_ACT_FIRSTPERSON && !pc_settings_get_first_person()) continue;
		if (a == PC_KEY_ACT_LOCKON && !pc_settings_get_lock_on()) continue;
		return true;
	}
	return false;
}

void set_visible(bool on, const char* why)
{
	sVisible = on;
	printf("[netplay] hud: %s (%s)\n", on ? "on" : "off", why);
	fflush(stdout);
}

void poll_toggle(const PcNetplayHudInfo& info)
{
	const Uint8* keys  = SDL_GetKeyboardState(nullptr);
	const bool keyDown = keys != nullptr && keys[SDL_SCANCODE_F4] != 0 && !f4_bound();
	bool chord         = false;
	const bool sticksInUse = info.inputKind != pc_netplay_input_sel::kInputKeyboard && stick_buttons_in_use();
	note_chord_state(sticksInUse);
	if (info.inputKind != pc_netplay_input_sel::kInputKeyboard && !sticksInUse) {
		SDL_GameController* ctl = pc_window_get_controller();
		chord = ctl != nullptr && SDL_GameControllerGetButton(ctl, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0
		     && SDL_GameControllerGetButton(ctl, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0;
	}
	const bool keyEdge = sKeyEdge.update(keyDown);
	const bool padEdge = sPadEdge.update(chord);
	if (keyEdge || padEdge) set_visible(!sVisible, keyEdge ? "F4" : "L3+R3");
	if (sTestToggleFrame >= 0 && !sTestToggled && (long long)info.frame >= sTestToggleFrame) {
		sTestToggled = true;
		set_visible(!sVisible, "test toggle");
	}
}

Colour rgba(const uint8_t c[4]) { return Colour(c[0], c[1], c[2], c[3]); }

// Word wrap to a pixel width at a font size.
std::vector<std::string> wrap(const std::string& text, int maxW, int fontW)
{
	std::vector<std::string> out;
	std::string line;
	size_t i = 0;
	while (i < text.size()) {
		size_t j = text.find(' ', i);
		if (j == std::string::npos) j = text.size();
		const std::string word = text.substr(i, j - i);
		const std::string cand = line.empty() ? word : line + " " + word;
		if (!line.empty() && pc_settings_p2d_text_width(cand.c_str(), fontW) > maxW) {
			out.push_back(line);
			line = word;
		} else {
			line = cand;
		}
		i = j + 1;
	}
	if (!line.empty()) out.push_back(line);
	return out;
}

void draw_box(const PcNetplayHudInfo& info, int W, int H)
{
	(void)W;
	// A small tag in the top-left corner, the one corner the game's HUD leaves
	// free (sun and day at the top right, captain and counters at the bottom):
	// the link quality and the ping in the quality's colour, the stalls only
	// when there are any, and a second line only for a state worth reading.
	const pc_netplay_hud::Quality q = pc_netplay_hud::classify(info.numbers.havePing, info.numbers.pingMs,
	                                                           info.numbers.jitterMs, info.numbers.stalls10s);
	uint8_t qc[4];
	pc_netplay_hud::quality_rgba(q, qc);
	char line[96];
	if (info.numbers.havePing)
		snprintf(line, sizeof line, "%s  %d ms", pc_netplay_hud::quality_name(q), (int)(info.numbers.pingMs + 0.5f));
	else
		snprintf(line, sizeof line, "%s", pc_netplay_hud::quality_name(q));
	if (info.numbers.stalls10s > 0) {
		const size_t len = strlen(line);
		snprintf(line + len, sizeof line - len, "  %u stalls", info.numbers.stalls10s);
	}
	const int fw = 8, fh = 12, pad = 8;
	const bool hasStatus = info.status[0] != '\0';
	int tw = pc_settings_p2d_text_width(line, fw);
	if (hasStatus) tw = std::max(tw, pc_settings_p2d_text_width(info.status, 6));
	const int bw = tw + 2 * pad + 6;
	const int bh = hasStatus ? 38 : 25;
	const int x  = 6;
	const int y  = (int)(H * 0.03f);
	pc_settings_p2d_plate(x, y, bw, bh, 0);
	pc_settings_p2d_text(x + pad + 1, y + 6, line, rgba(qc), fw, fh);
	if (hasStatus) {
		const Colour tone = info.statusTone == 2 ? Colour(255, 90, 80, 255)
		                  : info.statusTone == 1 ? Colour(255, 210, 90, 255)
		                                         : Colour(170, 220, 255, 255);
		pc_settings_p2d_text(x + pad + 1, y + 6 + fh + 3, info.status, tone, 6, 9);
	}
}

void draw_banner(const PcNetplayHudInfo& info, int W, int H)
{
	const int fw = 11, fh = 16, pad = 20;
	const int bw = std::min(W - 32, 620);
	const int textW = bw - 2 * pad;
	std::vector<std::string> body;
	for (int i = 0; i < info.bannerLines; ++i) {
		for (const std::string& l : wrap(info.bannerLine[i], textW, fw)) body.push_back(l);
	}
	if (body.size() > 14) body.resize(14); // at most 374 units tall in all
	char footer[96];
	const int left = info.bannerLeftMs > 0 ? (int)(info.bannerLeftMs / 1000.0 + 0.999) : 0;
	if (info.bannerLeftMs < 0) snprintf(footer, sizeof footer, "F4 hides this. It goes away when your friend joins.");
	else snprintf(footer, sizeof footer, "Closing in %d s - press any key or button to close now.", left);
	const int lineH = fh + 4;
	const int bh    = 16 + 24 + 8 + (int)body.size() * lineH + 10 + lineH + 16;
	const int x     = (W - bw) / 2;
	const int y     = std::max(8, (H - bh) / 2);
	pc_settings_p2d_plate(x, y, bw, bh, 0);
	const Colour title = info.bannerError ? Colour(255, 96, 80, 255) : Colour(255, 214, 90, 255);
	pc_settings_p2d_text(x + pad, y + 16, info.bannerTitle, title, 14, 20);
	int ty = y + 16 + 24 + 8;
	const Colour text(238, 238, 238, 255);
	for (const std::string& l : body) {
		pc_settings_p2d_text(x + pad, ty, l.c_str(), text, fw, fh);
		ty += lineH;
	}
	pc_settings_p2d_text(x + pad, ty + 10, footer, Colour(170, 180, 200, 255), 10, 15);
}

} // namespace

bool pc_netplay_hud_visible(void) { return sVisible; }

void pc_netplay_hud_draw(void)
{
	PcNetplayHudInfo info;
	if (!pc_netplay_hud_info(&info)) return; // no netplay session: nothing, not even the toggle
	init_once();
	poll_toggle(info);
	if (info.banner && info.bannerLeftMs < 0 && !sVisible) return; // the waiting banner hides with F4
	if (!info.banner) {
		if (!info.running || !sVisible) return;
		if (pc_settings_menu_is_open() || pc_glass_menu_active()) return;
		// The local Onion prompt occupies the status box's corner. Keep its
		// opening/closing animation clear too; the other captain's menu does
		// not obscure this peer's view. End-of-session alerts bypass this gate.
		Navi* local = naviMgr ? naviMgr->getNavi(pc_netplay_present_local_player()) : nullptr;
		if (local && local->getCurrState() && local->getCurrState()->getID() == NAVISTATE_Container) return;
	}
	if (gsys == nullptr || gsys->mDGXGfx == nullptr) return;
	DGXGraphics* gfx = static_cast<DGXGraphics*>(gsys->mDGXGfx);
	const int W = gfx->mScreenWidth;
	const int H = gfx->mScreenHeight;
	if (W <= 0 || H <= 0) return;
	{
		PcSettingsP2DFrame frame(W, H);
		if (!pc_settings_p2d_active()) return;
		Matrix4f ortho;
		gfx->setOrthogonal(ortho.mMtx, RectArea(0, 0, W, H));
		if (info.banner) draw_banner(info, W, H);
		else draw_box(info, W, H);
	}
	if (!info.banner && !sShotFile.empty() && !sShotDone && (long long)info.frame >= sShotFrame) {
		sShotDone = true;
		write_shot(sShotFile);
	}
	if (info.banner && !sBannerShotFile.empty() && !sBannerShotDone && ++sBannerFrames >= 10) {
		sBannerShotDone = true;
		write_shot(sBannerShotFile);
	}
}
