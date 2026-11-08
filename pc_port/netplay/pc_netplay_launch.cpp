// Netplay one-command launch: the pre-init stage. See pc_netplay_launch.h.

#include "netplay/pc_netplay_launch.h"

#include "netplay/pc_netplay_exit_codes.h"
#include "netplay/pc_netplay_ice.h"
#include "netplay/pc_netplay_session.h"
#include "settings/pc_settings.h"
#include "pc_coop.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

PcNetplayLaunch sSetup;

[[noreturn]] void die(const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printf("[netplay] launch: %s\n", buf);
	fflush(stdout);
	std::exit(pc_netplay_exit::kUsage);
}

const char* getenv_nonempty(const char* name)
{
	const char* v = std::getenv(name);
	return (v != nullptr && *v != '\0') ? v : nullptr;
}

bool argv_present(int argc, char** argv, const char* flag)
{
	for (int i = 1; i < argc; ++i)
		if (argv[i] != nullptr && std::strcmp(argv[i], flag) == 0) return true;
	return false;
}

// Value after `flag`; dies when the flag is the last argument.
const char* argv_value(int argc, char** argv, const char* flag)
{
	for (int i = 1; i < argc; ++i) {
		if (argv[i] == nullptr || std::strcmp(argv[i], flag) != 0) continue;
		if (i + 1 >= argc || argv[i + 1] == nullptr) die("%s needs a value", flag);
		return argv[i + 1];
	}
	return nullptr;
}

void set_env(const char* name, const std::string& value)
{
#ifdef _WIN32
	_putenv_s(name, value.c_str());
#else
	setenv(name, value.c_str(), 1);
#endif
}

unsigned current_pid()
{
#ifdef _WIN32
	return (unsigned)GetCurrentProcessId();
#else
	return (unsigned)getpid();
#endif
}

std::string trim(const std::string& s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
	return s.substr(b, e - b);
}

fs::path exe_dir()
{
#ifdef _WIN32
	wchar_t path[4096];
	const DWORD n = GetModuleFileNameW(nullptr, path, 4096);
	if (n == 0 || n >= 4096) return fs::path();
	return fs::path(std::wstring(path, n)).parent_path();
#else
	std::error_code ec;
	const fs::path p = fs::read_symlink("/proc/self/exe", ec);
	return ec ? fs::path() : p.parent_path();
#endif
}

bool dir_writable(const fs::path& dir)
{
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) return false;
	const fs::path probe = dir / ".nectar-write-test";
	FILE* f              = fopen(probe.string().c_str(), "wb");
	if (f == nullptr) return false;
	fclose(f);
	fs::remove(probe, ec);
	return true;
}

bool write_file(const fs::path& path, const std::string& data)
{
	FILE* f = fopen(path.string().c_str(), "wb");
	if (f == nullptr) return false;
	bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
	ok      = (fclose(f) == 0) && ok;
	return ok;
}

std::string absolute_new_path(const std::string& path)
{
	std::error_code ec;
	const fs::path p = fs::absolute(path, ec);
	return ec ? path : p.lexically_normal().generic_string();
}

#ifdef _WIN32
// @clipboard before SDL exists: the Win32 clipboard as UTF-8 (codes are
// ASCII; anything else fails the decode with a reason).
bool read_clipboard(std::string* out, std::string* err)
{
	bool opened = false;
	for (int attempt = 0; attempt < 10 && !opened; ++attempt) {
		opened = OpenClipboard(nullptr) != 0;
		if (!opened) Sleep(50);
	}
	if (!opened) {
		*err = "cannot open the clipboard";
		return false;
	}
	bool ok  = false;
	HANDLE h = GetClipboardData(CF_UNICODETEXT);
	if (h != nullptr) {
		const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h));
		if (w != nullptr) {
			const size_t cap = pc_netplay_ice::kMaxOfferV2Chars + 4096;
			size_t wlen      = 0;
			while (w[wlen] != L'\0' && wlen <= cap) ++wlen;
			if (wlen > cap) {
				*err = "the clipboard text is too large to be a connection code";
			} else {
				const int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, nullptr, 0, nullptr, nullptr);
				std::string s((size_t)(n > 0 ? n : 0), '\0');
				if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, &s[0], n, nullptr, nullptr);
				*out = s;
				ok   = true;
			}
			GlobalUnlock(h);
		}
	} else {
		*err = "the clipboard holds no text";
	}
	CloseClipboard();
	return ok;
}
#endif

std::string read_offer_arg(const std::string& arg)
{
	std::string code, err;
	if (trim(arg) == "@clipboard") {
#ifdef _WIN32
		if (!read_clipboard(&code, &err)) die("cannot read the offer code from the clipboard: %s", err.c_str());
#else
		die("@clipboard is only supported on Windows; pass the code itself or @file");
#endif
		code = trim(code);
		if (code.empty()) die("the clipboard is empty; copy the host's offer code first");
		return code;
	}
	if (!pc_netplay_ice::ice_read_code_arg(arg, &code, &err)) die("cannot read the offer code: %s", err.c_str());
	return code;
}

void decode_offer()
{
	std::string err;
	switch (pc_netplay_ice::ice_classify_code(sSetup.offerCode)) {
	case pc_netplay_ice::kCodeEmpty:
		die("the offer code is empty; copy the host's whole offer line");
	case pc_netplay_ice::kCodeGarbage:
		die("that is not a connection code (the host's room code starts with NPIX3-); copy the host's whole code");
	case pc_netplay_ice::kCodeV1: {
		bool isOffer = false;
		std::string sdp;
		if (!pc_netplay_ice::ice_decode_code(sSetup.offerCode, &isOffer, &sdp, &err)) die("%s", err.c_str());
		if (!isOffer)
			die("that is an answer code (NPIX1-): it goes to the host; --netplay-join-ice needs the host's offer "
			    "(NPIX2-...)");
		die("that offer (NPIX1-) carries no session setup; both players need the same build, whose host prints "
		    "an NPIX2- offer");
	}
	case pc_netplay_ice::kCodeV3:
	case pc_netplay_ice::kCodeOfferV2:
		break;
	}
	std::string sdp;
	pc_netplay_ice::SessionBundle bundle;
	// A compact offer carries the settings' values only; this build's own
	// settings text gives the key list.
	pc_netplay_ice::ice_set_local_config_text(pc_settings_netplay_config_text());
	if (!pc_netplay_ice::ice_decode_any_offer(sSetup.offerCode, &sdp, &bundle, &err)) die("%s", err.c_str());
	sSetup.seed       = bundle.seed;
	sSetup.configText = bundle.configText;
	sSetup.descriptor = bundle.descriptor;
	sSetup.haveCaptains =
	    pc_netplay_launch_parse_descriptor(sSetup.descriptor, &sSetup.captainP1, &sSetup.captainP2);
	sSetup.mode = pc_netplay_launch_parse_mode(sSetup.descriptor, &sSetup.vsMapJson);
}

// The descriptor's keys, without the map.json that may end it.
std::string descriptor_head(const std::string& text)
{
	const size_t at = text.find("vsMapJson=");
	return at == std::string::npos ? text : text.substr(0, at);
}

void make_run_dir(int argc, char** argv)
{
	fs::path base;
	if (const char* root = argv_value(argc, argv, "--netplay-run-root")) {
		base = fs::absolute(root).lexically_normal();
		if (!dir_writable(base)) die("--netplay-run-root %s is not a writable folder", root);
	} else {
		const fs::path exeBase = exe_dir() / "netplay";
		const char* local      = getenv_nonempty("LOCALAPPDATA");
		if (!exe_dir().empty() && dir_writable(exeBase)) {
			base = exeBase;
		} else if (local != nullptr && dir_writable(fs::path(local) / "Nectar" / "netplay")) {
			base = fs::path(local) / "Nectar" / "netplay";
			printf("[netplay] launch: %s is not writable; run folders go to %s\n", exeBase.string().c_str(),
			       base.string().c_str());
		} else {
			die("cannot create a run folder: %s is not writable", exeBase.string().c_str());
		}
	}
	const time_t now = time(nullptr);
	struct tm lt;
#ifdef _WIN32
	localtime_s(&lt, &now);
#else
	localtime_r(&now, &lt);
#endif
	char stamp[128];
	snprintf(stamp, sizeof(stamp), "run-%04d%02d%02d-%02d%02d%02d-%s-pid%u", lt.tm_year + 1900, lt.tm_mon + 1,
	         lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, sSetup.isHost ? "host" : "join", current_pid());
	// A run folder is never reused, so nothing stale meets a new session.
	for (int n = 0; n < 100 && sSetup.runDir.empty(); ++n) {
		const fs::path dir = base / (std::string(stamp) + (n == 0 ? std::string() : "-" + std::to_string(n)));
		std::error_code ec;
		if (fs::create_directory(dir, ec)) sSetup.runDir = dir.generic_string();
		else if (ec) die("cannot create the run folder %s", dir.string().c_str());
	}
	if (sSetup.runDir.empty()) die("cannot create a fresh run folder under %s", base.string().c_str());
	sSetup.saveDir = sSetup.runDir + "/save";
	std::error_code ec;
	fs::create_directories(sSetup.saveDir, ec);
	if (ec) die("cannot create %s", sSetup.saveDir.c_str());
}

} // namespace

const PcNetplayLaunch& pc_netplay_launch_setup(void) { return sSetup; }

std::string pc_netplay_launch_descriptor(int captainP1, int captainP2, int mode, const std::string& vsMapJson)
{
	std::string d = "captainP1=" + std::to_string(captainP1) + ";captainP2=" + std::to_string(captainP2) + ";";
	if (mode == 2) {
		d += "mode=vs;";
		if (!vsMapJson.empty() && vsMapJson.size() <= kPcNetplayVsMapJsonMax) d += "vsMapJson=" + vsMapJson;
	}
	return d;
}

int pc_netplay_launch_parse_mode(const std::string& text, std::string* vsMapJson)
{
	const std::string head = descriptor_head(text);
	const bool vs          = head.rfind("mode=vs;", 0) == 0 || head.find(";mode=vs;") != std::string::npos;
	if (vsMapJson != nullptr) {
		const size_t at = text.find("vsMapJson=");
		*vsMapJson      = vs && at != std::string::npos ? text.substr(at + 10) : std::string();
	}
	return vs ? 2 : 1;
}

bool pc_netplay_launch_parse_descriptor(const std::string& full, int* captainP1, int* captainP2)
{
	const std::string text = descriptor_head(full);
	auto value = [&](const char* key, int* out) {
		const std::string k = std::string(key) + "=";
		size_t at           = text.find(k);
		while (at != std::string::npos && at != 0 && text[at - 1] != ';') at = text.find(k, at + 1);
		if (at == std::string::npos) return false;
		char* end     = nullptr;
		const char* v = text.c_str() + at + k.size();
		const long n  = strtol(v, &end, 10);
		if (end == v || (*end != ';' && *end != '\0') || n < 0 || n >= PC_CAPTAIN_COUNT) return false;
		*out = (int)n;
		return true;
	};
	int p1 = 0, p2 = 1;
	if (!value("captainP1", &p1) || !value("captainP2", &p2)) return false;
	*captainP1 = p1;
	*captainP2 = p2;
	return true;
}

void pc_netplay_launch_preinit(int argc, char** argv)
{
	const bool hostIce = argv_present(argc, argv, "--netplay-host-ice");
	const bool joinIce = argv_present(argc, argv, "--netplay-join-ice");
	const char* launchOnly[] = { "--netplay-code-out", "--netplay-answer-in", "--netplay-run-root" };
	if (!hostIce && !joinIce) {
		for (const char* flag : launchOnly)
			if (argv_present(argc, argv, flag)) die("%s needs --netplay-host-ice or --netplay-join-ice", flag);
		return;
	}
	if (hostIce && joinIce) die("--netplay-host-ice and --netplay-join-ice are exclusive");
	const char* direct[] = { "--netplay-host", "--netplay-join" };
	for (const char* flag : direct)
		if (argv_present(argc, argv, flag))
			die("--netplay-host-ice/--netplay-join-ice cannot be combined with %s (the UDP switches)", flag);
	if (getenv_nonempty("PIKMIN_NETPLAY_HOST") != nullptr || getenv_nonempty("PIKMIN_NETPLAY_JOIN") != nullptr)
		die("PIKMIN_NETPLAY_HOST/PIKMIN_NETPLAY_JOIN are set; unset them to use --netplay-host-ice/--netplay-join-ice");
	if (!hostIce && argv_present(argc, argv, "--netplay-answer-in"))
		die("--netplay-answer-in is for the host (the joiner prints its answer)");

	sSetup.active = true;
	sSetup.isHost = hostIce;
	if (const char* v = argv_value(argc, argv, "--netplay-code-out")) sSetup.codeOut = absolute_new_path(v);
	if (const char* v = argv_value(argc, argv, "--netplay-answer-in")) sSetup.answerIn = absolute_new_path(v);

	if (sSetup.isHost) {
		if (const char* seedEnv = getenv_nonempty("PIKMIN_NETPLAY_SEED")) {
			char* end                  = nullptr;
			const unsigned long long v = strtoull(seedEnv, &end, 0);
			if (end != seedEnv && *end == '\0' && v <= 0xFFFFFFFFull) sSetup.seed = (uint32_t)v;
		}
	} else {
		const char* joinArg = argv_value(argc, argv, "--netplay-join-ice");
		sSetup.offerCode    = read_offer_arg(joinArg);
		// The title's Online menu hands the offer over in a file read once.
		if (argv_present(argc, argv, "--online-code-once") && joinArg[0] == '@') std::remove(joinArg + 1);
		decode_offer();
	}

	make_run_dir(argc, argv);
	// The card root is resolved at boot (CARDInit), so it is set before
	// anything can read it.
	if (const char* prev = getenv_nonempty("NECTAR_SAVE_DIR"))
		printf("[netplay] launch: NECTAR_SAVE_DIR was %s; this session uses its private save folder\n", prev);
	set_env("NECTAR_SAVE_DIR", sSetup.saveDir);
	// The deterministic reseed and the session read the seed from the
	// environment: the joiner takes the host's.
	if (!sSetup.isHost) set_env("PIKMIN_NETPLAY_SEED", std::to_string(sSetup.seed));

	std::string rec;
	rec += std::string("role ") + (sSetup.isHost ? "host" : "join") + "\n";
	rec += "save " + sSetup.saveDir + "\n";
	rec += "seed " + std::to_string(sSetup.seed) + "\n";
	if (!sSetup.isHost) {
		rec += "host_config " + sSetup.configText + "\n";
		rec += "host_descriptor " + descriptor_head(sSetup.descriptor) + "\n";
	}
	rec += "started_unix " + std::to_string((long long)time(nullptr)) + "\n";
	write_file(fs::path(sSetup.runDir) / "launch.txt", rec);
	{
		FILE* f = fopen("pikmin_settings.conf", "rb");
		if (f != nullptr) {
			std::string conf;
			char chunk[8192];
			size_t n = 0;
			while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0 && conf.size() < (1u << 20)) conf.append(chunk, n);
			fclose(f);
			write_file(fs::path(sSetup.runDir) / "settings-at-start.conf", conf);
		}
	}
	printf("[netplay] launch: role=%s run folder %s\n", sSetup.isHost ? "host" : "join", sSetup.runDir.c_str());
	printf("[netplay] launch: NECTAR_SAVE_DIR=%s\n", sSetup.saveDir.c_str());
	if (!sSetup.isHost)
		printf("[netplay] launch: offer bundle seed=%u config=%lluB descriptor=%s%s\n", sSetup.seed,
		       (unsigned long long)sSetup.configText.size(),
		       sSetup.descriptor.empty() ? "(none)" : descriptor_head(sSetup.descriptor).c_str(),
		       sSetup.vsMapJson.empty() ? "" : (" + VS map.json " + std::to_string(sSetup.vsMapJson.size()) + "B").c_str());
	fflush(stdout);
}

void pc_netplay_launch_post_settings(void)
{
	// A replay takes the recorded session's sim settings.
	if (const char* replay = pc_netplay_session_replay_settings()) {
		pc_settings_netplay_session_begin(replay);
		return;
	}
	// The online guest plays by the host's rules, received over the link.
	if (const char* rules = pc_netplay_session_online_rules()) {
		pc_settings_netplay_session_begin(rules);
		return;
	}
	if (!sSetup.active) return;
	// The joiner adopts the host's sim settings before anything reads one.
	pc_settings_netplay_session_begin(sSetup.isHost ? nullptr : sSetup.configText.c_str());
}
