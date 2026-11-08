#include "netplay/pc_online_menu.h"
#include "netplay/pc_online_relaunch.h"

#include "netplay/pc_netplay_session.h"
#include "gl/pc_gfx.h"
#include "settings/pc_settings_p2d.h"

#include "Colour.h"
#include "Controller.h"
#include "Geometry.h"
#include "Graphics.h"
#include "Matrix4f.h"
#include "SoundID.h"
#include "SoundMgr.h"
#include "system.h"

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

enum Page { kClosed, kMode, kOnline, kHostSave, kJoin, kNotice };

Page sPage    = kClosed;
bool sVs      = false;
int sCursor   = 0;
std::string sMessage; // error or hint under the options

constexpr int kScreenW = 640, kScreenH = 480;

std::vector<const char*> options()
{
	switch (sPage) {
	case kMode: return { "Local", "Online" };
#if PIKI_NECTAR_ONLINE
	case kOnline: return { "Create room", "Join with a code", "Back" };
#else
	case kOnline: return { "Create room", "Join with a code", "Back" };
#endif
	case kHostSave: return { "New game", "Continue file 1", "Continue file 2", "Continue file 3", "Back" };
	case kJoin: return { "Paste code and join", "Back" };
	case kNotice: return { "OK" };
	default: return {};
	}
}

void go(Page page)
{
	sPage   = page;
	sCursor = 0;
	sMessage.clear();
}

// Starts this game again with `extra` and leaves (Android: Java ends this
// process a moment later). False on failure.
bool relaunch(const std::vector<std::string>& extra)
{
	if (!pc_online_relaunch(extra, &sMessage)) return false;
#if defined(__ANDROID__)
	sMessage = "Restarting the game..."; // Java ends this process
#else
	std::_Exit(0);
#endif
	return true;
}

std::vector<std::string> mode_args()
{
	return { "--netplay-mode", sVs ? "vs" : "coop" };
}

#if PIKI_NECTAR_ONLINE
void host(int saveSlot)
{
	// A fixed port, so one firewall rule or router forward serves every session.
	std::vector<std::string> extra = { "--online-host", "39911", "--online-code-clipboard" };
	if (saveSlot != 0) extra.insert(extra.end(), { "--online-save", std::to_string(saveSlot) });
	const std::vector<std::string> mode = mode_args();
	extra.insert(extra.end(), mode.begin(), mode.end());
	relaunch(extra);
}
#endif

#if !PIKI_NECTAR_ONLINE
// The netplay run folder goes beside the game data: the default, beside the
// executable, is read-only on Android (/system/bin).
void add_run_root(std::vector<std::string>& extra)
{
	std::error_code ec;
	std::filesystem::create_directories("online-sessions", ec);
	const std::filesystem::path runRoot = std::filesystem::absolute("online-sessions", ec);
	if (!ec) extra.insert(extra.end(), { "--netplay-run-root", runRoot.string() });
}

// Lockstep host over ICE: the game opens the room, copies its code and asks
// for the friend's answer in a dialog (--netplay-dialogs).
void ice_host()
{
	std::vector<std::string> extra = { "--netplay-host-ice", "--netplay-dialogs", "--netplay-mode", sVs ? "vs" : "coop" };
	add_run_root(extra);
	relaunch(extra);
}
#endif

// The code the guest was sent, from the clipboard: "NECTAR1-" and Crockford
// Base32 groups. Spaces and line breaks around it are dropped.
#if PIKI_NECTAR_ONLINE
constexpr const char* kCodePrefixes[] = { "NECTAR1-" };
constexpr size_t kMaxCode = 400;
#else
// ICE offers (pc_netplay_ice): "NPIX2-" or the older "NPIX1-".
constexpr const char* kCodePrefixes[] = { "NPIX3-", "NPIX2-" };
constexpr size_t kMaxCode = 16384;
#endif

bool clipboard_code(std::string& code)
{
	char* text = SDL_GetClipboardText();
	std::string raw = text != nullptr ? text : "";
	SDL_free(text);
	code.clear();
	for (char c : raw) {
		if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
		if (c < 0x21 || c > 0x7e || code.size() >= kMaxCode) return false;
		code += c;
	}
	bool prefixed = false;
	for (const char* prefix : kCodePrefixes) prefixed = prefixed || code.rfind(prefix, 0) == 0;
	return prefixed && code.size() > 16;
}

void join()
{
	std::string code;
	if (!clipboard_code(code)) {
		sMessage = std::string("No code in the clipboard: copy the code your friend sent you (") + kCodePrefixes[0]
		         + "...), then try again.";
		return;
	}
	// A file only this user can read, removed by the game once read.
	std::error_code ec;
	std::filesystem::create_directories("online-sessions", ec);
#if PIKI_NECTAR_ONLINE
	const std::string path = "online-sessions/join-code.txt";
#else
	const std::string path = "online-sessions/join-offer.txt";
#endif
#ifdef _WIN32
	FILE* f = fopen(path.c_str(), "wb");
#else
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	FILE* f      = fd >= 0 ? fdopen(fd, "wb") : nullptr;
#endif
	const bool ok = f != nullptr && fwrite(code.data(), 1, code.size(), f) == code.size();
	if (f != nullptr) fclose(f);
	if (!ok) {
		sMessage = "Cannot write the code file in online-sessions/.";
		return;
	}
#if PIKI_NECTAR_ONLINE
	std::vector<std::string> extra = { "--online-join", "@" + path, "--online-code-once" };
#else
	// Lockstep over ICE: the game answers the offer (the answer goes to the
	// clipboard) and starts once the host pastes it.
	std::vector<std::string> extra = { "--netplay-join-ice", "@" + path, "--online-code-once", "--netplay-dialogs" };
	add_run_root(extra);
#endif
	const std::vector<std::string> mode = mode_args();
	extra.insert(extra.end(), mode.begin(), mode.end());
	// The new game reads the file and removes it; only a failed restart does here.
	if (!relaunch(extra)) std::remove(path.c_str());
}

void sound(int id)
{
	if (seSystem != nullptr) seSystem->playSysSe(id);
}

// Word wrap for the message line.
std::vector<std::string> wrap(const std::string& text, int maxW, int fw)
{
	std::vector<std::string> out;
	std::string line;
	size_t i = 0;
	while (i < text.size()) {
		size_t j = text.find(' ', i);
		if (j == std::string::npos) j = text.size();
		const std::string word = text.substr(i, j - i);
		const std::string cand = line.empty() ? word : line + " " + word;
		if (!line.empty() && pc_settings_p2d_text_width(cand.c_str(), fw) > maxW) {
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

} // namespace

bool pc_online_menu_available(void)
{
	return !pc_netplay_session_active();
}

void pc_online_menu_open(bool vs)
{
	sVs = vs;
	go(kMode);
}

bool pc_online_menu_active(void)
{
	// A failed join came back to the title: say why first.
	if (sPage == kClosed) {
		if (const char* why = pc_netplay_online_take_error()) {
			go(kNotice);
			sMessage = why;
		}
	}
	return sPage != kClosed;
}

int pc_online_menu_update(Controller* input)
{
	if (sPage == kClosed || input == nullptr) return PC_ONLINE_MENU_CANCEL;
	const int count = (int)options().size();
	if (input->keyClick(KBBTN_DPAD_UP | KBBTN_MSTICK_UP)) {
		sCursor = (sCursor + count - 1) % count;
		sound(SYSSE_MOVE1);
	}
	if (input->keyClick(KBBTN_DPAD_DOWN | KBBTN_MSTICK_DOWN)) {
		sCursor = (sCursor + 1) % count;
		sound(SYSSE_MOVE1);
	}
	const bool back = input->keyClick(KBBTN_B) || (input->keyClick(KBBTN_A) && sCursor == count - 1 && sPage != kMode);
	if (back) {
		sound(SYSSE_CANCEL);
		if (sPage == kMode || sPage == kNotice) {
			sPage = kClosed;
			return PC_ONLINE_MENU_CANCEL;
		}
		go(sPage == kOnline ? kMode : kOnline);
		return PC_ONLINE_MENU_OPEN;
	}
	if (!input->keyClick(KBBTN_A | KBBTN_START)) return PC_ONLINE_MENU_OPEN;
	sound(SYSSE_DECIDE1);
	switch (sPage) {
	case kMode:
		if (sCursor == 0) {
			sPage = kClosed;
			return PC_ONLINE_MENU_LOCAL;
		}
		go(kOnline);
		break;
	case kOnline:
#if PIKI_NECTAR_ONLINE
		if (sCursor == 0) {
			if (sVs) host(0);
			else go(kHostSave);
		} else {
			go(kJoin);
		}
#else
		if (sCursor == 0) ice_host();
		else go(kJoin);
#endif
		break;
#if PIKI_NECTAR_ONLINE
	case kHostSave: host(sCursor); break;
#endif
	case kJoin: join(); break;
	default: break;
	}
	return PC_ONLINE_MENU_OPEN;
}

void pc_online_menu_draw(void)
{
	if (sPage == kClosed || !gsys || !gsys->mDGXGfx) return;
	DGXGraphics* gfx = static_cast<DGXGraphics*>(gsys->mDGXGfx);
	pc_gfx_set_ui_43_no_bars(0);
	struct Unbind {
		~Unbind() { pc_gfx_set_ui_43_no_bars(0); }
	} unbind;
	PcSettingsP2DFrame frame(kScreenW, kScreenH);
	if (!pc_settings_p2d_active()) return;
	pc_gfx_set_ui_43_no_bars(1);

	const std::vector<const char*> rows = options();
	const int w = 400, pad = 28, rowH = 30;
	std::vector<std::string> msg;
	std::string hint;
#if PIKI_NECTAR_ONLINE
	if (sPage == kOnline) hint = "Create a room and send the code to your friend, or join with the code you were sent.";
#else
	if (sPage == kOnline)
		hint = "Create a room and send its code to your friend, or join with the code you were sent. Both need the "
		       "same disc and build.";
#endif
	if (sPage == kHostSave) hint = "Continuing uses a file of your memory card; only your card keeps the progress.";
#if PIKI_NECTAR_ONLINE
	if (sPage == kJoin) hint = "Copy the code your friend sent you (NECTAR1-...) and choose Paste.";
#else
	if (sPage == kJoin)
		hint = "Copy the code your friend sent you (NPIX3-...) and choose Paste. Your answer code is then copied: "
		       "send it back to your friend.";
#endif
	for (const std::string& l : wrap(sMessage.empty() ? hint : sMessage, w - 2 * pad, 10)) msg.push_back(l);
	const int h = 70 + (int)rows.size() * rowH + (msg.empty() ? 0 : 12 + (int)msg.size() * 17) + 40;
	const int x = (kScreenW - w) / 2, y = (kScreenH - h) / 2;
	pc_gfx_blur_gx_rect(x + 6, y + 6, w - 12, h - 12, 3);
	Matrix4f ortho;
	gfx->setOrthogonal(ortho.mMtx, RectArea(0, 0, kScreenW, kScreenH));
	pc_settings_p2d_plate(x, y, w, h, 3);

	const char* title = sPage == kMode     ? (sVs ? "VS" : "Co-op")
	                  : sPage == kOnline   ? (sVs ? "VS online" : "Co-op online")
	                  : sPage == kHostSave ? "Create room"
	                  : sPage == kNotice   ? "Could not join"
	                                       : "Join a room";
	pc_settings_p2d_text_styled(x + pad, y + 22, title, Colour(255, 236, 140, 255), Colour(255, 190, 60, 255), 16, 22);
	int ry = y + 62;
	for (int i = 0; i < (int)rows.size(); ++i) {
		if (i == sCursor) pc_settings_p2d_plate(x + pad - 10, ry - 4, w - 2 * pad + 20, rowH - 2, 2);
		pc_settings_p2d_text(x + pad + 4, ry + 2, rows[i], i == sCursor ? Colour(255, 255, 255, 255) : Colour(200, 210, 225, 255),
		                     12, 18);
		ry += rowH;
	}
	if (!msg.empty()) {
		ry += 12;
		const Colour tone = sMessage.empty() ? Colour(180, 200, 220, 255) : Colour(255, 150, 120, 255);
		for (const std::string& l : msg) {
			pc_settings_p2d_text(x + pad, ry, l.c_str(), tone, 10, 15);
			ry += 17;
		}
	}
	pc_settings_p2d_text(x + pad, y + h - 30, "A: choose    B: back", Colour(160, 170, 190, 255), 10, 15);
}
