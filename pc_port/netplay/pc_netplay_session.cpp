// Netplay lockstep co-op session over GekkoNet.
//
// Each System::run loop turn while a session is active:
//   1. mControllerMgr.update() pumps SDL and samples the local pad (the F1
//      menu's consume path already zeroes the pads while it is open); the
//      sample is folded into the button-OR / latest-sticks accumulator.
//   2. When a submit is due, the local input (the accumulator, or the next
//      record of a scripted input file) goes to gekko_add_local_input.
//   3. gekko_update_session.
//   4. Per Advance: both inputs are injected into pads 0 and 1 with each
//      pad's control yaw, and exactly one tick runs (updateSysClock,
//      pc_netplay_on_tick_begin, app->idle, the state hash).
//   5. Per Save: an 8-byte handle {frame} and the state hash as checksum.
//   6. A Load cannot happen with prediction window 0: logged, then abort.
//   7. Session events: Connected, Started, Disconnected, DesyncDetected.
//   8. No Advance (waiting): no tick; the turn still pumped the network and
//      the window.
//
// Pacing: the session owns the single 30 Hz schedule (the presentation
// limiter in pc_window.cpp stays off). Drift-free deadline, next += 1000/30
// ms per Advance, with bounded catch-up; gekko_frames_ahead() stretches the
// deadline slightly while this peer is ahead, so the pair converges. Waiting
// turns pump the network and wait about 1 ms on a high-resolution timer.
// PIKMIN_NETPLAY_UNTHROTTLED=1 runs as fast as the session allows (tests).
//
// The local input delay is PIKMIN_NETPLAY_DELAY frames (default 2) or "auto"
// (from the median handshake RTT), then adapted to the stalls the peer
// reports (pc_netplay_adaptive.h). It is per peer and not hashed. Long
// blocking loads are covered by the load guard (pc_netplay_loadguard.h).
//
// Handshake: a Hello on the handshake channel with platform-neutral build
// manifest and config digests, the seed and mode (pc_netplay_transfer.h).
// The first mismatching field refuses on both peers (exit 4).
//
// Transports: UDP (--netplay-host/--netplay-join) or ICE (--netplay-host-ice/
// --netplay-join-ice, pc_netplay_launch.h): the copy-paste code exchange runs
// on the first turn, then the session talks to the ICE-selected pair through
// the same channel bytes.
//
// Forensics: every Advance's two inputs and this peer's hash go to the session
// input log (pc_netplay_inlog.h), and per-tick object records to the state
// dump ring (pc_state_dump.h). On a desync the peers swap their sub-hash rings
// and the object hashes of the first differing tick over the handshake
// channel, and each writes desync-report.txt. --netplay-replay-log <file>
// replays a logged session offline through the same per-Advance body.

#include "netplay/pc_netplay_session.h"
#include "netplay/pc_online_relaunch.h"

#include "netplay/pc_coop_switch.h"
#include "netplay/pc_input_log.h"
#include "netplay/pc_netplay_adaptive.h"
#include "netplay/pc_netplay_build_id.h"
#include "netplay/pc_netplay_camlead.h"
#include "netplay/pc_netplay_det.h"
#include "netplay/pc_netplay_exit_codes.h"
#include "netplay/pc_netplay_forensics.h"
#include "netplay/pc_netplay_gekko_input.h"
#include "netplay/pc_netplay_hud.h"
#include "netplay/pc_netplay_ice.h"
#include "netplay/pc_netplay_inlog.h"
#include "netplay/pc_netplay_input_sel.h"
#include "netplay/pc_netplay_launch.h"
#include "netplay/pc_netplay_loadguard.h"
#include "netplay/pc_netplay_pad.h"
#include "netplay/pc_netplay_present.h"
#include "netplay/pc_netplay_sha256.h"
#include "netplay/pc_netplay_transfer.h"
#include "netplay/pc_netplay_udp.h"
#include "netplay/pc_state_dump.h"
#include "netplay/pc_state_hash.h"
#include "pc_coop.h"
#include "pc_permadeath.h"
#include "pc_speedrun.h"
#include "pc_vs.h"
#include "mods/pc_vs_arena.h"
#include "mods/pc_vs_map.h"
#include "Vector.h"
#include "pc_window.h"
#include "randomizer/pc_randomizer.h"
#include "settings/pc_settings.h"
#include "settings/pc_settings_rows.h"

#include "gekkonet.h"

#if PIKI_NECTAR_ONLINE && defined(NECTAR_NET_HAS_GNS)
#define PC_ONLINE 1
#include "netplay/pc_online_navi.h"
#include "dolphin_stubs/pc_card_paths.h"
#include "nectar_net/clock.h"
#include "nectar_net/input.h"
#include "nectar_net/limits.h"
#include "nectar_net/protocol.h"
#include "nectar_net/replication.h"
#include "nectar_net/room.h"
#include "nectar_net/scene.h"
#include <array>
#include <map>
#include <memory>
#else
#define PC_ONLINE 0
#endif
#if PIKI_NETPLAY_PORTMAP
#include "nectar_net/port_publisher.h"
#include <memory>
#endif

#include "system.h"
#include "BaseApp.h"
#include "Dolphin/os.h"
#include "Dolphin/pad.h"
#include "jaudio/interface.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <synchapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

// Ends the process with a session exit code. On Android the game thread
// cannot run static destructors while the UI threads still use them (abort:
// "mutex destroyed"), so it leaves without them there.
[[noreturn]] static void session_exit(int code)
{
	fflush(stdout);
	fflush(stderr);
#if defined(__ANDROID__)
	_exit(code);
#else
	std::exit(code);
#endif
}

static std::string sOnlineError; // --online-error: shown once on the title

namespace {

namespace npx = pc_netplay_exit;
namespace xfer = pc_netplay_xfer;

enum Phase {
	kIdle,      // no netplay switch: drive() returns false at once
	kHandshake,
	kSession,
	kDone,      // ended: keep owning the loop until the process quits
};

constexpr unsigned kMaxLocalDelay = pc_netplay_adaptive::kMaxLocalDelay;
static_assert(pc_netplay_loadguard::kLoadWindowFrames > kMaxLocalDelay + 1,
              "the load window must outlast the largest delay");

struct Config {
	bool isHost        = false;
	bool active        = false;
	uint16_t hostPort  = 0; // --netplay-host <port>
	uint32_t joinIp    = 0; // --netplay-join <ip:port>
	uint16_t joinPort  = 0;
	unsigned localDelay = 2; // PIKMIN_NETPLAY_DELAY
	bool delayAuto     = false;
	uint32_t seed      = 0; // PIKMIN_NETPLAY_SEED
	std::string localInputFile; // --netplay-input <file> / PIKMIN_NETPLAY_LOCAL_INPUT_FILE
	uint64_t exitAfter = 0;     // PIKMIN_NETPLAY_EXIT_AFTER_TICKS
	std::string inputSpec;      // --netplay-input keyboard|gamepad[:N]|auto
	int inputKind      = 0;     // pc_netplay_input_sel::Kind
	int inputGamepad   = 0;
	bool ice           = false; // --netplay-host-ice / --netplay-join-ice
	bool replay        = false; // --netplay-replay-log <file>
	uint8_t mode       = 1;     // xfer::kModeCoopDirect or kModeVs (--netplay-mode, --vs-map, the offer)
	bool vsMapFile     = false; // VS: the map is a file (--vs-map, the offer's map.json), not the menu's
	float testVsMatchSeconds = 0; // TEST ONLY: PIKMIN_NETPLAY_TEST_VS_MATCH_SECONDS
	// Host-authoritative online co-op over nectar_net (--online-host /
	// --online-join): one code, no lockstep. See ONLINE_COOP_IMPLEMENTATION_PLAN 10.1.1.
	bool online            = false;
	uint16_t onlinePort    = 0;  // --online-host <port>; 0 = random high port
	std::string onlineCode;      // --online-join <code | @file>
	std::string onlineCodeOut;   // --online-code-out <file>: the host writes its code there
	bool onlineCodeClipboard = false; // --online-code-clipboard: the host copies its code to the clipboard
	bool onlineFromMenu      = false; // guest started by the in-game menu (--online-code-once)
	bool onlinePredictive  = false; // --online-predictive: Phase 4 captain prediction instead of the exact replica
	int onlineSaveSlot     = 0;     // --online-save <1-3>: the host continues that file of its own card
};

int sArgc     = 0;
char** sArgv  = nullptr;
Config sCfg;
Phase sPhase       = kIdle;
bool sInitialised  = false;
bool sForcedModes  = false;

constexpr uint16_t kProtocolVersion = xfer::kProtocolV3;
constexpr size_t kHsHeaderLen       = xfer::kHsHeaderLen;
constexpr size_t kHsLen             = xfer::kHsLen;
using Hello = xfer::Hello;

Hello sLocal;
std::string sVsMapHexStr;
// A captain this PC cannot load (a Pikmin 2 one without the local install):
// the handshake refuses as soon as the peer is heard. -1 when both load.
int sCaptainMissing = -1;
std::string sBuildHexStr;
std::string sBuildManifest;
std::string sCfgHexStr;
std::string sCfgText;

pc_netplay_transport::UdpSocket* sSock = nullptr;
pc_netplay_transport::GekkoLink* sLink = nullptr;
pc_netplay_transport::LossyLink* sLossy = nullptr;
// ICE: exactly one of the UDP pair (sSock/sLink) or this pair is set.
pc_netplay_ice::IceSocket* sIce   = nullptr;
pc_netplay_ice::IceLink* sIceLink = nullptr;
GekkoNetAdapter* sAdapter = nullptr;
GekkoSession* sGekko      = nullptr;
uint8_t sRemoteAddrBlob[6] = { 0 };
bool sHaveRemote      = false;
uint32_t sRemoteIp    = 0;
uint16_t sRemotePort  = 0;
int sLocalHandle      = 0;
int sLocalRole        = 0; // 0 = host/P1, 1 = joiner/P2
bool sGekkoStarted    = false;

// Handshake progress. Every Hello send carries a fresh nonce with its send
// time recorded; each Ack echoes the Hello nonce it answers, so an RTT
// sample is matched to its own send. Samples are median-filtered.
bool sSentAck       = false;
bool sGotAck        = false;
double sHsStartMs   = 0;
double sHsLastSendMs = 0;
uint64_t sHsNextNonce = 1;
std::vector<std::pair<uint64_t, double>> sHsSendTimes;
std::vector<double> sHsSamples;
// Test hook: PIKMIN_NETPLAY_TEST_DROP_FINAL_ACK=1 drops handshake-phase Acks,
// so the peer must recover through in-session Hello answers.
bool sDropFinalAckInit = false;
bool sDropFinalAck     = false;
// Test hook: PIKMIN_NETPLAY_TEST_LOAD_DELAY_MS sleeps once inside the next
// stage load on that peer only.
bool sLoadDelayDone = false;

// Load guard (pc_netplay_loadguard.h). sInAdvance is true only while
// app->idle() runs inside an Advance, the only place the keep-alive may
// pump, and only on the session's own thread.
std::atomic<bool> sInAdvance{ false };
std::thread::id sLgMainThread;
bool sLgConfigured  = false;
bool sLgKeepAlive   = true;
bool sLgSpeculative = false;
pc_netplay_loadguard::KeepAliveGate sLgGate;
pc_netplay_loadguard::LoadWindow sLgWindow;
pc_netplay_loadguard::StallPlan sLgStall;
bool sLgStallDone     = false;
uint32_t sLgStageLoads = 0;
uint32_t sLgTickFrame  = 0;
double sLgTickStartMs  = 0;
double sLgLastPumpMs   = 0;
double sLgTickMaxGapMs = 0;
uint64_t sLgTickPumps  = 0;
uint64_t sLgPumps      = 0;
uint64_t sLgSitePumps[pc_netplay_loadguard::kSiteCount] = {};
uint64_t sLgWindows    = 0;
uint64_t sLgLongTicks  = 0;
double sLgLongestTickMs = 0;
double sLgWorstGapMs    = 0;
double sLgWindowOpenMs  = 0;
uint32_t sLgWindowFirst = 0;
bool sLgSummaryDone     = false;

// Run stats.
uint64_t sSessionTicks = 0;
uint64_t sAdvances     = 0;
uint64_t sStalls       = 0;
uint64_t sSaves        = 0;
uint64_t sSubmitted    = 0;
// The frame the next local input lands on (GekkoNet's local last-received
// frame + 1). A submit is due when sNextLand == sAdvances + localDelay.
uint64_t sNextLand     = 0;
double sNextTurnMs     = 0;
double sStallMs        = 0;
double sSessionStartMs = 0;
uint32_t sLastAdvanceFrame = 0;
double sLastAdvanceWallMs  = 0;
PcNetplayAccum sPadAccum;
// TEST ONLY: PIKMIN_NETPLAY_TEST_DESYNC_AT_FRAME=<f> flips this peer's
// reported checksum from frame f on, so both peers take the desync path
// without any sim change. Not in the config hash.
int64_t sTestDesyncFrame = -1;
bool sTestDesyncLogged   = false;
bool sNaviPosTrace       = false; // PIKMIN_NETPLAY_DEBUG_NAVI_POS=1

// Ring of per-tick hashes so a desync report can print the desynced frame's
// sub-hashes. GekkoNet frame F is hash tick F+1.
struct HashEntry {
	bool valid      = false;
	uint64_t tick   = 0;
	uint64_t total  = 0;
	uint64_t subs[7] = { 0, 0, 0, 0, 0, 0, 0 };
	uint64_t xtra   = 0; // pc_state_dump's extra hash (not in the total)
};
constexpr size_t kHashRing = 2048;
HashEntry sHashRing[kHashRing];

void hash_ring_store(uint64_t tick, uint64_t total, const uint64_t subs[7], uint64_t xtra)
{
	HashEntry& e = sHashRing[tick % kHashRing];
	e.valid      = true;
	e.tick       = tick;
	e.total      = total;
	for (int i = 0; i < 7; ++i) e.subs[i] = subs[i];
	e.xtra = xtra;
}

const HashEntry* hash_ring_find(uint64_t tick)
{
	const HashEntry& e = sHashRing[tick % kHashRing];
	return (e.valid && e.tick == tick) ? &e : nullptr;
}

// ---- desync forensics state (observes the session, never feeds the sim) ----
//   PIKMIN_NETPLAY_INPUT_LOG=<file>  the session input log (launcher sessions:
//                                    <run folder>/session-inputs.pknl; =0 off)
//   PIKMIN_NETPLAY_FORENSICS=0       no per-tick object records
//   PIKMIN_NETPLAY_FORENSICS_DIR     where desync files go (default: the
//                                    launcher run folder, else the working dir)
//   PIKMIN_NETPLAY_TEST_DESYNC_NUDGE=<frame>[:piki|navi|teki[:<ord>]]
//       TEST ONLY, with PIKMIN_NETPLAY_TEST_KNOBS=1: adds 1.0 to the x
//       position of one object of THIS peer's sim at the start of that
//       frame's tick, so exactly one object diverges. Not in the config hash.
namespace fx = pc_netplay_forensics;
pc_netplay_inlog::Writer sInLog;
double sInLogFlushMs = 0;
bool sInLogCapSaid   = false;
bool sForensicsOn    = false;
struct NudgeKnob {
	int64_t frame = -1;
	int kind      = fx::kPiki;
	int ord       = 0;
	bool done     = false;
};
NudgeKnob sNudge;
struct ReplayState {
	std::string path;
	pc_netplay_inlog::Log log;
	size_t next          = 0;
	uint64_t verified    = 0;
	uint64_t mismatches  = 0;
	uint32_t firstBadFrame = 0;
	bool haveBad         = false;
	bool finished        = false;
	std::string settings; // the recorded sim settings, adopted at settings init
};
ReplayState sReplay;
// End banner (after a desync or a lost connection, between ticks).
bool sBannerOn        = false;
bool sBannerError     = false;
int sBannerExit       = -1; // exit code once it closes (-1: a normal quit)
double sBannerStartMs = 0;
double sBannerMs      = 0;
std::string sBannerTitle;
std::vector<std::string> sBannerLines;

// Scripted local input (pkni v2, pad 0 of each record).
std::vector<uint8_t> sScriptBytes;
size_t sScriptTicks  = 0;
size_t sScriptIdx    = 0;
size_t sScriptRec    = 56;
bool sScriptActive   = false;

double now_ms()
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// One shared high-resolution timer for every short wait. Hidden processes
// on Windows 11 do not get the timeBeginPeriod(1) granularity they ask for
// (Sleep(1) sleeps ~15.6 ms), which would quantise the handshake RTT and the
// 30 Hz slot wait, so the bulk of a wait runs on a waitable timer created
// with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION. Slot waits spin only their
// last 1 ms; poll waits never spin. Without the high-resolution flag (before
// Windows 10 1803) a plain timer with timeBeginPeriod(1) is used.
#ifdef _WIN32
bool sHiresTimerReady   = false;
HANDLE sHiresTimer      = NULL;
bool sHiresTimerHighRes = false;
bool sHiresPeriodBegun  = false;
bool sHiresCoarse       = false;

void hires_timer_teardown()
{
	if (sHiresTimer != NULL) {
		CloseHandle(sHiresTimer);
		sHiresTimer = NULL;
	}
	if (sHiresPeriodBegun) {
		sHiresPeriodBegun = false;
		timeEndPeriod(1);
	}
}

void ensure_hires_timer()
{
	if (sHiresTimerReady) return;
	sHiresTimerReady = true;
	HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (timer != NULL) {
		sHiresTimer        = timer;
		sHiresTimerHighRes = true;
	} else {
		timer = CreateWaitableTimerW(NULL, FALSE, NULL);
		if (timer != NULL) {
			sHiresTimer        = timer;
			sHiresTimerHighRes = false;
			timeBeginPeriod(1);
			sHiresPeriodBegun = true;
		}
	}
	if (sHiresTimer != NULL) {
		printf("[netplay] timer path: %swaitable timer%s\n", sHiresTimerHighRes ? "high-resolution " : "plain ",
		       sHiresTimerHighRes ? "" : " + timeBeginPeriod(1)");
	} else {
		timeBeginPeriod(1);
		sHiresPeriodBegun = true;
		const double t0 = now_ms();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		sHiresCoarse = (now_ms() - t0) > 5.0;
		printf("[netplay] timer path: timeBeginPeriod fallback (%s)\n",
		       sHiresCoarse ? "coarse: spin waits" : "fine: sleep+spin tail");
	}
	fflush(stdout);
	std::atexit(hires_timer_teardown);
}
#else
inline void ensure_hires_timer() {}
#endif

// Waits ms. Everything except the last spinTailMs (at most 1 ms) blocks;
// poll waits pass spinTailMs = 0 and never spin.
void sleep_hires_ms(double ms, double spinTailMs = 1.0)
{
	if (ms <= 0) return;
	if (spinTailMs < 0) spinTailMs = 0;
	if (spinTailMs > 1.0) spinTailMs = 1.0;
	ensure_hires_timer();
	const double start   = now_ms();
	const double blockMs = ms - spinTailMs;
#ifdef _WIN32
	if (sHiresTimer != NULL) {
		if (blockMs > 0.05) {
			LARGE_INTEGER due;
			due.QuadPart = -(LONGLONG)(blockMs * 10000.0); // 100 ns units
			if (SetWaitableTimer(sHiresTimer, &due, 0, NULL, NULL, FALSE)) {
				WaitForSingleObject(sHiresTimer, INFINITE);
			} else {
				std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(blockMs));
			}
		}
	} else if (!sHiresCoarse || spinTailMs == 0) {
		if (blockMs > 0.05) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(blockMs));
	}
#else
	if (blockMs > 0.05) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(blockMs));
#endif
	if (spinTailMs == 0) return;
	const double deadline = start + ms;
	while (now_ms() < deadline - 0.3) std::this_thread::yield();
	while (now_ms() < deadline) {
	}
}

// While a session runs, the process opts out of timer-resolution power
// throttling (its timer-resolution requests are always honoured); at the end
// control returns to the system. SetProcessInformation is resolved at run
// time: its declaration needs _WIN32_WINNT >= 0x0602.
void netplay_timer_power_opt(bool on)
{
#ifdef _WIN32
	static bool resolved = false;
	typedef BOOL(WINAPI * SetProcessInformationFn)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
	static SetProcessInformationFn fn = nullptr;
	static bool active = false;
	if (!resolved) {
		resolved        = true;
		HMODULE kernel  = GetModuleHandleW(L"kernel32.dll");
		if (kernel != nullptr) fn = (SetProcessInformationFn)(void*)GetProcAddress(kernel, "SetProcessInformation");
	}
	if (fn == nullptr || on == active) return;
	PROCESS_POWER_THROTTLING_STATE state;
	state.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	state.ControlMask = on ? PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION : 0;
	state.StateMask   = 0;
	if (fn(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) active = on;
#else
	(void)on;
#endif
}

const char* getenv_nonempty(const char* name)
{
	const char* v = std::getenv(name);
	return (v != nullptr && *v != '\0') ? v : nullptr;
}

unsigned read_unsigned_env(const char* name, unsigned fallback)
{
	const char* v = getenv_nonempty(name);
	if (v == nullptr) return fallback;
	char* end       = nullptr;
	unsigned long n = strtoul(v, &end, 10);
	if (end == v || *end != '\0') return fallback;
	return (unsigned)n;
}

double read_double_env(const char* name, double fallback)
{
	const char* v = getenv_nonempty(name);
	if (v == nullptr) return fallback;
	char* end = nullptr;
	double n  = strtod(v, &end);
	if (end == v || *end != '\0') return fallback;
	return n;
}

uint32_t read_u32_env(const char* name, uint32_t fallback)
{
	const char* v = getenv_nonempty(name);
	if (v == nullptr) return fallback;
	char* end       = nullptr;
	unsigned long n = strtoul(v, &end, 0);
	if (end == v || *end != '\0' || n > 0xFFFFFFFFul) return fallback;
	return (uint32_t)n;
}

bool argv_has(int argc, char** argv, const char* flag)
{
	for (int i = 1; i < argc; ++i)
		if (argv[i] != nullptr && std::strcmp(argv[i], flag) == 0) return true;
	return false;
}

const char* argv_value(int argc, char** argv, const char* flag)
{
	for (int i = 1; i + 1 < argc; ++i) {
		if (argv[i] != nullptr && std::strcmp(argv[i], flag) == 0) return argv[i + 1];
	}
	return nullptr;
}

bool env_is_one(const char* name)
{
	const char* v = getenv_nonempty(name);
	return v != nullptr && std::strcmp(v, "1") == 0;
}

[[noreturn]] void usage_exit(const char* fmt, const char* arg)
{
	printf("[netplay] ");
	printf(fmt, arg != nullptr ? arg : "");
	printf("\n");
	fflush(stdout);
	session_exit(npx::kUsage);
}

// ---- digests ----

// The session config the handshake hashes: every sim-relevant setting
// (pc_settings_netplay_config_text, the list is in pc_settings.cpp), the
// co-op mode and captains, the netplay seed and the protocol version.
// The local input delay is per peer and deliberately not part of it.
std::string build_config_string()
{
	std::string s = "netplay-config-v1;";
	s += pc_settings_netplay_config_text();
	char buf[96];
	snprintf(buf, sizeof(buf), "coop=1;captainP1=%d;captainP2=%d;netplaySeed=%u;protocolVersion=%u;",
	         pc_coop_captain(0), pc_coop_captain(1), (unsigned)sCfg.seed, (unsigned)kProtocolVersion);
	s += buf;
	// VS: the mode and the map are the Hello's mode field and digest (so a
	// mismatch names them); the rules are the vs* keys above.
	if (sCfg.mode == xfer::kModeVs && sCfg.testVsMatchSeconds > 0) {
		snprintf(buf, sizeof(buf), "testVsMatchSeconds=%g;", sCfg.testVsMatchSeconds);
		s += buf;
	}
	return s;
}

void compute_local_hello()
{
	sBuildManifest = pc_netplay_build::manifest();
	pc_netplay_build::digest(sLocal.build);
	sBuildHexStr = pc_netplay_sha::hex(sLocal.build, 32);
	sCfgText   = build_config_string();
	pc_netplay_sha::sha256(sCfgText.data(), sCfgText.size(), sLocal.cfg);
	sCfgHexStr            = pc_netplay_sha::hex(sLocal.cfg, 32);
	sLocal.seed           = sCfg.seed;
	sLocal.mode.mode      = sCfg.mode;
	sLocal.mode.captainP1 = (uint8_t)pc_coop_captain(0);
	sLocal.mode.captainP2 = (uint8_t)pc_coop_captain(1);
	for (int p = 0; p < 2 && sCaptainMissing < 0; ++p) {
		if (!pc_captain_available(pc_coop_captain(p))) sCaptainMissing = pc_coop_captain(p);
	}
	if (sCaptainMissing >= 0) {
		printf("[netplay] captain %d is not available on this PC (it needs a local Pikmin 2 install); the session "
		       "will be refused\n",
		       sCaptainMissing);
	}
	if (sCfg.mode != xfer::kModeVs) return;
	// VS: both PCs must compile the very same map (pc_vs_arena), so the
	// handshake compares the digest of the compiled .mod and .ini.
	if (!sCfg.vsMapFile) pc_vs_map_select(pc_settings_get_vs_map());
	if (const char* why = pc_vs_arena_prepare()) {
		printf("[netplay] refusing session: the VS map cannot be played: %s\n", why);
		fflush(stdout);
		session_exit(npx::kUsage);
	}
	const u8* mod = nullptr;
	const u8* ini = nullptr;
	size_t modLen = 0, iniLen = 0;
	pc_vs_arena_compiled(&mod, &modLen, &ini, &iniLen);
	pc_netplay_sha::Sha256 sha;
	sha.update(mod, modLen);
	sha.update(ini, iniLen);
	// The places the sim spawns things from (bases, Onions, pellets, pieces,
	// enemies, objects...) come from the map, not from the compiled files.
	// Every spot struct is 4-byte fields only, so its bytes are its value.
	{
		auto put = [&sha](const void* p, size_t n) { sha.update(static_cast<const uint8_t*>(p), n); };
		for (int player = 0; player < 2; ++player) {
			Vector3f pos;
			f32 face = 0;
			pc_vs_arena_base(player, pos, face);
			put(&pos, sizeof(pos));
			put(&face, sizeof(face));
			pc_vs_arena_rocket(player, pos, face);
			put(&pos, sizeof(pos));
			put(&face, sizeof(face));
			pc_vs_arena_gate(player, pos, face);
			put(&pos, sizeof(pos));
			put(&face, sizeof(face));
			const PcVsReaction gateReaction = pc_vs_arena_gate_reaction(player);
			put(&gateReaction, sizeof(gateReaction));
			pc_vs_arena_bomb_pile(player, pos);
			put(&pos, sizeof(pos));
			for (int color = 0; color < 3; ++color) {
				pc_vs_arena_onion(player, color, pos);
				put(&pos, sizeof(pos));
			}
		}
		Vector3f big;
		pc_vs_arena_big_piece(big);
		put(&big, sizeof(big));
		PcVsPelletSpot pellets[16];
		put(pellets, sizeof(pellets[0]) * (size_t)pc_vs_arena_pellet_spots(pellets, 16));
		PcVsPieceSpot pieces[16];
		put(pieces, sizeof(pieces[0]) * (size_t)pc_vs_arena_piece_spots(pieces, 16));
		PcVsGuardSpot guards[4];
		put(guards, sizeof(guards[0]) * (size_t)pc_vs_arena_guard_spots(guards, 4));
		PcVsEnemySpot enemies[48];
		put(enemies, sizeof(enemies[0]) * (size_t)pc_vs_arena_enemy_spots(enemies, 48));
		PcVsObjectSpot objects[32];
		put(objects, sizeof(objects[0]) * (size_t)pc_vs_arena_object_spots(objects, 32));
	}
	sha.final(sLocal.vsMapSha);
	sVsMapHexStr       = pc_netplay_sha::hex(sLocal.vsMapSha, 32);
	const PcVsMap* map = pc_vs_map();
	printf("[netplay] vs map: \"%s\" (%s) compiled %llu+%llu B, digest (compiled map and spots) %s\n", map ? map->name.c_str() : "?",
	       sCfg.vsMapFile ? "file" : (pc_settings_get_vs_map()[0] ? pc_settings_get_vs_map() : "built-in arena"),
	       (unsigned long long)modLen, (unsigned long long)iniLen, sVsMapHexStr.c_str());
	fflush(stdout);
}

bool local_ui_open() { return pc_settings_menu_is_open() != 0; }

// ---- input ----

void apply_input_selection()
{
	using namespace pc_netplay_input_sel;
	if (sCfg.inputKind == (int)kInputAuto) return;
	const Kind kind = (Kind)sCfg.inputKind;
	bool ignoreKb = false, ignorePad = false;
	filter_for_selection(kind, &ignoreKb, &ignorePad);
	pc_window_set_netplay_input_filter(ignoreKb, ignorePad);
	const char* who = sLocalRole == 0 ? "host/P1" : "joiner/P2";
	if (kind == kInputKeyboard) {
		pc_window_input_assign(sLocalRole, PC_INPUT_DEV_KEYBOARD, -1);
		printf("[netplay] input: keyboard and mouse feed %s (every gamepad ignored)\n", who);
	} else {
		pc_window_set_netplay_gamepad(sLocalRole, sCfg.inputGamepad);
		printf("[netplay] input: gamepad #%d feeds %s (every key ignored; the pad keeps working while another "
		       "window has focus)\n",
		       sCfg.inputGamepad, who);
		if (pc_window_num_gamepads() <= sCfg.inputGamepad)
			printf("[netplay] input: gamepad #%d is not connected yet; %s stays neutral until it is plugged in\n",
			       sCfg.inputGamepad, who);
	}
	fflush(stdout);
}

void load_scripted_file()
{
	const char* path = sCfg.localInputFile.c_str();
	if (path[0] == '\0') return;
	FILE* f = fopen(path, "rb");
	if (f == nullptr) {
		printf("[netplay] local input file: cannot open %s\n", path);
		fflush(stdout);
		session_exit(npx::kInputFile);
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 10) {
		printf("[netplay] local input file: truncated %s\n", path);
		fflush(stdout);
		fclose(f);
		session_exit(npx::kInputFile);
	}
	sScriptBytes.resize((size_t)size);
	const size_t got = fread(sScriptBytes.data(), 1, (size_t)size, f);
	fclose(f);
	sScriptBytes.resize(got);
	if (got < 10 || memcmp(sScriptBytes.data(), "PKNI", 4) != 0) {
		printf("[netplay] local input file: bad magic %s\n", path);
		fflush(stdout);
		session_exit(npx::kInputFile);
	}
	const uint16_t version = (uint16_t)(sScriptBytes[4] | ((uint16_t)sScriptBytes[5] << 8));
	const uint16_t pads    = (uint16_t)(sScriptBytes[6] | ((uint16_t)sScriptBytes[7] << 8));
	const uint16_t rec     = (uint16_t)(sScriptBytes[8] | ((uint16_t)sScriptBytes[9] << 8));
	if (!((version == 2 && rec == 56) || (version == 3 && rec == 64)) || pads != 4) {
		printf("[netplay] local input file: need pkni v2 4x14 or v3 4x16, got v%u %ux%u (%s)\n",
		       (unsigned)version, (unsigned)pads, (unsigned)rec, path);
		fflush(stdout);
		session_exit(npx::kInputFile);
	}
	sScriptRec    = rec;
	sScriptTicks  = (sScriptBytes.size() - 10) / rec;
	sScriptActive = true;
	printf("[netplay] local input file: %s (%llu ticks)\n", path, (unsigned long long)sScriptTicks);
	fflush(stdout);
}

PcNetplayInput scripted_record(size_t idx)
{
	PcNetplayInput in;
	if (!sScriptActive || idx >= sScriptTicks) return in; // neutral past the end
	const uint8_t* rec = sScriptBytes.data() + 10 + idx * sScriptRec;
	// Pad 0 of the 4-pad record (14 bytes: 11 PADStatus + yaw u16 + flags;
	// v3 16: the flags are the ext bits, then the cursor rotate s16).
	in.buttons    = (uint16_t)(rec[0] | ((uint16_t)rec[1] << 8));
	in.stickX     = (int8_t)rec[2];
	in.stickY     = (int8_t)rec[3];
	in.substickX  = (int8_t)rec[4];
	in.substickY  = (int8_t)rec[5];
	in.triggerL   = rec[6];
	in.triggerR   = rec[7];
	in.controlYaw = (uint16_t)(rec[11] | ((uint16_t)rec[12] << 8));
	if (sScriptRec == 64) {
		in.ext          = rec[13] & pc_input_log::kExtKnown;
		in.cursorRotate = (int16_t)(uint16_t)(rec[14] | ((uint16_t)rec[15] << 8));
	}
	return in;
}

PcNetplayInput build_local_input()
{
	if (sScriptActive) {
		PcNetplayInput in = scripted_record(sScriptIdx++);
		if (local_ui_open()) in = pc_netplay_input_neutral();
		return in;
	}
	if (local_ui_open()) {
		// The F1 menu is local: the local input stays neutral, and the latch
		// is cleared so buttons held before opening do not leak in after it.
		sPadAccum.reset();
		return pc_netplay_input_neutral();
	}
	return sPadAccum.take();
}

// This peer's Bomb, Lock-On and Charge presses, held Swarm button and cursor
// rotation since the last turn. Every local device plays this peer's own
// captain, so both window slots are read, as the camera drag does.
void take_local_ext(uint8_t* ext, int16_t* rotate)
{
	uint8_t e = 0;
	if (pc_window_take_bomb_press(0) | pc_window_take_bomb_press(1)) e |= pc_input_log::kExtBomb;
	if (pc_window_take_lockon_press(0) | pc_window_take_lockon_press(1)) e |= pc_input_log::kExtLockOn;
	if (pc_window_take_swarm_press(0) | pc_window_take_swarm_press(1)) e |= pc_input_log::kExtCharge;
	if (pc_window_swarm_held() || pc_window_swarm_held_p2()) e |= pc_input_log::kExtSwarm;
	*ext    = e;
	*rotate = pc_input_log_local_rotate_take();
}

// Every session turn folds the current physical sample into the
// accumulator, so a press shorter than the submit interval still reaches
// the next submit. The fresh yaw capture keeps the submitted yaw following
// the live camera; inject_input rewrites the slots before every tick.
void accum_add_current()
{
	uint8_t ext    = 0;
	int16_t rotate = 0;
	take_local_ext(&ext, &rotate);
	if (sScriptActive) return;
	if (!local_ui_open()) sPadAccum.add_ext(ext, rotate);
	pc_input_log_capture_yaw_fresh();
	PADStatus* pads  = pc_netplay_pad_status();
	const int padIdx = pc_netplay_input_sel::local_pad_index(sLocalRole, (pc_netplay_input_sel::Kind)sCfg.inputKind);
	const PADStatus s = pads[padIdx];
	const uint16_t yaw = pc_input_log_yaw_valid(sLocalRole) ? pc_input_log_yaw_raw(sLocalRole) : 0;
	sPadAccum.add(s.button, s.stickX, s.stickY, s.substickX, s.substickY, s.triggerLeft, s.triggerRight, yaw);
}

void inject_input(int pad, const PcNetplayInput& in)
{
	PADStatus* pads        = pc_netplay_pad_status();
	pads[pad].button       = in.buttons;
	pads[pad].stickX       = in.stickX;
	pads[pad].stickY       = in.stickY;
	pads[pad].substickX    = in.substickX;
	pads[pad].substickY    = in.substickY;
	pads[pad].triggerLeft  = in.triggerL;
	pads[pad].triggerRight = in.triggerR;
	pads[pad].analogA      = 0;
	pads[pad].analogB      = 0;
	pads[pad].err          = PAD_ERR_NONE;
	pc_input_log_yaw_set(pad, in.controlYaw, pc_input_log::kFlagsNone);
	pc_input_log_ext_set(pad, in.ext & pc_input_log::kExtKnown, in.cursorRotate);
}

void inject_neutral_pad(int pad)
{
	PADStatus* pads = pc_netplay_pad_status();
	memset(&pads[pad], 0, sizeof(PADStatus));
	pads[pad].err = (pad < 2) ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
	pc_input_log_yaw_set(pad, 0, pc_input_log::kFlagsNone);
	pc_input_log_ext_set(pad, 0, 0);
}

uint32_t fold_hash64(uint64_t v) { return (uint32_t)(v ^ (v >> 32)); }

// ---- adaptive input delay (pc_netplay_adaptive.h) ----
//
//   PIKMIN_NETPLAY_ADAPTIVE_DELAY  unset/1: on in real-time sessions (off in
//                                  unthrottled test runs); 0: off; force: on
//                                  even when unthrottled
//   PIKMIN_NETPLAY_TEST_DELAY_SCHEDULE=<frame>:<delay>[,...]  test only:
//                                  forced changes at those frames
//   PIKMIN_NETPLAY_STALL_TRACE=1   one line per stall event and per tick of
//                                  50 ms or more
//
// Wall clock only: nothing here reads or writes sim state, and the only
// GekkoNet calls are the local delay setters and gekko_network_stats. Each
// peer reports its own counted stalls to the other every 250 ms (kHsAdvice
// on the handshake channel, unreliable, cumulative).
constexpr uint64_t kAdaptiveWarmupFrames = 150;
constexpr double kRttSampleMs = 500.0;
constexpr double kAdviceMs    = 250.0;
constexpr uint8_t kHsAdvice   = 0x40; // handshake-channel type: stable header + Advice payload
pc_netplay_adaptive::SessionStats sStats;
pc_netplay_adaptive::DelayController sDelayCtl;
bool sAdaptiveConfigured = false;
bool sAdaptive           = false;
const char* sAdaptiveWhy = "";
std::vector<pc_netplay_adaptive::ScheduleStep> sDelaySched;
size_t sDelaySchedIdx = 0;
unsigned sDelayStart  = 0;
unsigned sDelayLow    = 0;
unsigned sDelayHigh   = 0;
uint64_t sDelayUps    = 0;
uint64_t sDelayDowns  = 0;
std::string sDelayTimeline;
std::string sPendingWhy;
uint64_t sSettleUntil  = 0;
bool sStallOpen        = false;
double sStallStartMs   = 0;
double sStallDurMs     = 0;
bool sStallExcluded    = false;
double sLastFrameEndMs = 0;
double sRttNextMs      = 0;
bool sStatsFinalDone   = false;
double sLateMs         = 0;
uint32_t sLateEvents   = 0;
uint32_t sAdviceSeq    = 0;
double sAdviceNextMs   = 0;
pc_netplay_adaptive::AdviceReceiver sAdviceIn;
double sPeerLateMs     = 0;
double sSelfOverrunMs  = 0;
double sNextDueMs      = 0;
bool sHaveDue          = false;
bool sPrevTurnStalled  = true;
double sOwnLagPrev     = 0;
float sStallAhead      = 0;
int sStallTrace        = -1;

bool stall_trace()
{
	if (sStallTrace < 0) sStallTrace = env_is_one("PIKMIN_NETPLAY_STALL_TRACE") ? 1 : 0;
	return sStallTrace == 1;
}

double session_s(double nowMs) { return sSessionStartMs > 0 ? (nowMs - sSessionStartMs) / 1000.0 : 0.0; }

void adaptive_timeline_add(double nowMs, uint64_t frame, unsigned delay)
{
	char cell[64];
	snprintf(cell, sizeof(cell), "%s+%.1fs@%llu:%u", sDelayTimeline.empty() ? "" : " ", session_s(nowMs),
	         (unsigned long long)frame, delay);
	sDelayTimeline += cell;
	if (delay < sDelayLow) sDelayLow = delay;
	if (delay > sDelayHigh) sDelayHigh = delay;
}

// Session start, once the delay (including DELAY=auto) is final.
void adaptive_configure()
{
	sAdaptiveConfigured = true;
	sStats.reset();
	sDelayStart = sDelayLow = sDelayHigh = sCfg.localDelay;
	sDelayUps = sDelayDowns = 0;
	sDelayTimeline.clear();
	sStallOpen       = false;
	sLastFrameEndMs  = 0;
	sRttNextMs       = 0;
	sStatsFinalDone  = false;
	sLateMs          = 0;
	sLateEvents      = 0;
	sAdviceSeq       = 0;
	sAdviceNextMs    = 0;
	sAdviceIn.reset();
	sPeerLateMs      = 0;
	sSelfOverrunMs   = 0;
	sNextDueMs       = 0;
	sHaveDue         = false;
	sPrevTurnStalled = true;
	sOwnLagPrev      = 0;
	sNextLand        = sCfg.localDelay; // the first add fills frames 0..d-1 with GekkoNet's empty input
	sSettleUntil     = kAdaptiveWarmupFrames;
	pc_netplay_adaptive::Policy pol;
	if (pol.maxDelay > kMaxLocalDelay) pol.maxDelay = kMaxLocalDelay;
	sDelayCtl.configure(pol);
	sDelayCtl.start(now_ms());
	sDelaySched.clear();
	sDelaySchedIdx = 0;
	std::string err;
	const char* sched = getenv_nonempty("PIKMIN_NETPLAY_TEST_DELAY_SCHEDULE");
	if (sched != nullptr && !pc_netplay_adaptive::parse_schedule(sched, pol.minDelay, pol.maxDelay, &sDelaySched, &err)) {
		printf("[netplay] adaptive delay: test schedule ignored (%s)\n", err.c_str());
		sDelaySched.clear();
	}
	const char* mode = getenv_nonempty("PIKMIN_NETPLAY_ADAPTIVE_DELAY");
	const bool off   = mode != nullptr && (std::strcmp(mode, "0") == 0 || std::strcmp(mode, "off") == 0);
	const bool force = mode != nullptr && std::strcmp(mode, "force") == 0;
	if (!sDelaySched.empty()) {
		sAdaptive    = false;
		sAdaptiveWhy = "off (test schedule drives the delay)";
	} else if (off) {
		sAdaptive    = false;
		sAdaptiveWhy = "off (PIKMIN_NETPLAY_ADAPTIVE_DELAY=0)";
	} else if (pc_netplay_unthrottled() && !force) {
		sAdaptive    = false;
		sAdaptiveWhy = "off (unthrottled test run)";
	} else {
		sAdaptive    = true;
		sAdaptiveWhy = force ? "on (forced)" : "on";
	}
	printf("[netplay] adaptive delay: %s start=%u range=%u..%u up=%.0fms stall in %.1fs down=%.0fs clean+rtt "
	       "schedule=%llu steps\n",
	       sAdaptiveWhy, sCfg.localDelay, pol.minDelay, pol.maxDelay, pol.upStallMs, pol.upWindowMs / 1000.0,
	       pol.downHoldMs / 1000.0, (unsigned long long)sDelaySched.size());
	fflush(stdout);
}

void adaptive_log_change(unsigned from, unsigned to, const std::string& why)
{
	const double now = now_ms();
	if (to > from) ++sDelayUps;
	else ++sDelayDowns;
	adaptive_timeline_add(now, sAdvances, to);
	printf("[netplay] delay change: %u -> %u at frame=%llu next-land=%llu t=%.1fs (%s)\n", from, to,
	       (unsigned long long)sAdvances, (unsigned long long)sNextLand, session_s(now), why.c_str());
	fflush(stdout);
}

// Why no change may start now (nullptr: free to change).
const char* adaptive_frozen()
{
	if (sLgWindow.is_open()) return "load window";
	if (!pc_netplay_adaptive::submit_due(sNextLand, sAdvances, sCfg.localDelay)) return "transition";
	if (sAdvances < sSettleUntil) return "settle";
	return nullptr;
}

// Target delay for this submit turn (the current delay: nothing to do).
unsigned adaptive_target()
{
	const unsigned cur = sCfg.localDelay;
	sPendingWhy.clear();
	if (!sDelaySched.empty()) {
		if (sDelaySchedIdx >= sDelaySched.size()) return cur;
		const pc_netplay_adaptive::ScheduleStep& st = sDelaySched[sDelaySchedIdx];
		if (sAdvances < st.frame || sLgWindow.is_open()) return cur;
		// A shrink before the first submit would skip GekkoNet's start fill
		// of frames 0..d-1 and the submit gate would never be due again.
		if (sSubmitted == 0 && st.delay < cur) return cur;
		++sDelaySchedIdx;
		char buf[64];
		snprintf(buf, sizeof(buf), "test schedule step %llu for frame %llu", (unsigned long long)sDelaySchedIdx,
		         (unsigned long long)st.frame);
		sPendingWhy = buf;
		return st.delay;
	}
	if (!sAdaptive) return cur;
	const char* frozen = adaptive_frozen();
	pc_netplay_adaptive::Decision d = sDelayCtl.decide(now_ms(), cur, frozen != nullptr);
	if (!d.changed) {
		if (!d.reason.empty()) {
			printf("[netplay] delay hold at %u: %s\n", cur, d.reason.c_str());
			fflush(stdout);
		}
		return cur;
	}
	sPendingWhy = d.reason;
	return d.target;
}

// A shrink: GekkoNet touches no input; the next (cur - target) turns submit
// nothing, until sNextLand == sAdvances + target.
void adaptive_shrink(unsigned target)
{
	const unsigned from = sCfg.localDelay;
	gekko_set_local_delay_nofill(sGekko, sLocalHandle, (unsigned char)target);
	sCfg.localDelay = target;
	adaptive_log_change(from, target, sPendingWhy);
}

// The submit of this turn: n == 1 normally; n == 1 + k grows the delay by k.
// Each input lands on sNextLand. Scripted inputs consume one record per
// frame; a live pad repeats this turn's sample on the k extra frames.
void submit_local_inputs(unsigned n)
{
	const unsigned from = sCfg.localDelay;
	PcNetplayInput first;
	for (unsigned j = 0; j < n; ++j) {
		if (j > 0) gekko_set_local_delay_nofill(sGekko, sLocalHandle, (unsigned char)(from + j));
		PcNetplayInput local = (j == 0 || sScriptActive) ? build_local_input() : first;
		if (j == 0) {
			first = local;
			// The repeats keep the held state, not the presses or the rotation.
			first.ext &= pc_input_log::kExtSwarm;
			first.cursorRotate = 0;
		}
		uint8_t wire[16];
		pc_netplay_input_encode(local, wire);
		gekko_add_local_input(sGekko, sLocalHandle, wire);
		// The lead camera replays this peer's submitted inputs until the sim
		// applies them, so every add is noted at the frame it lands on.
		pc_netplay_camlead_note_local_input(sNextLand, local);
		++sSubmitted;
		++sNextLand;
	}
	if (n > 1) {
		sCfg.localDelay = from + n - 1;
		adaptive_log_change(from, sCfg.localDelay, sPendingWhy);
	}
}

void adaptive_note_frame()
{
	const double now = now_ms();
	if (sLastFrameEndMs > 0) sStats.add_frame(now - sLastFrameEndMs);
	sLastFrameEndMs = now;
}

void adaptive_close_stall()
{
	if (!sStallOpen) return;
	sStallOpen = false;
	pc_netplay_adaptive::StallEvent e;
	e.startMs  = sStallStartMs;
	e.durMs    = sStallDurMs;
	e.excluded = sStallExcluded;
	sStats.add_stall(e);
	// Reported to the peer (its inputs were late here), never fed to this
	// peer's own controller.
	const bool counted = pc_netplay_adaptive::stall_counted(e, sDelayCtl.policy().hitchMs);
	if (counted) {
		sLateMs += e.durMs;
		++sLateEvents;
	}
	if (stall_trace()) {
		printf("[netplay] stall-trace: stall frame=%llu t=%.3fs dur=%.1fms excluded=%d counted=%d ahead=%.2f "
		       "delay=%u\n",
		       (unsigned long long)sAdvances, session_s(e.startMs), e.durMs, (int)e.excluded, (int)counted,
		       sStallAhead, sCfg.localDelay);
		fflush(stdout);
	}
}

// Own lateness: a turn that starts behind this peer's 30 Hz schedule delays
// its next input, and the peer will report the wait. It counts only where
// it grows, never after a stall turn and not inside a load window.
void adaptive_note_turn_start(double turnStartMs)
{
	if (!sAdaptiveConfigured || !sGekkoStarted || sAdvances == 0 || pc_netplay_unthrottled()) return;
	const double lag = (sHaveDue && turnStartMs > sNextDueMs) ? turnStartMs - sNextDueMs : 0.0;
	if (sPrevTurnStalled || !sHaveDue) {
		sOwnLagPrev = lag;
		return;
	}
	const double grew = lag - sOwnLagPrev;
	sOwnLagPrev       = lag;
	if (grew <= 0 || sLgWindow.is_open()) return;
	sSelfOverrunMs += grew;
	sDelayCtl.add_self_overrun(turnStartMs, grew);
}

void adaptive_note_turn_end(bool advanced, bool haveDue, double nextDueMs)
{
	sPrevTurnStalled = !advanced;
	if (advanced && haveDue) {
		sNextDueMs = nextDueMs;
		sHaveDue   = true;
	}
}

void hs_send(const uint8_t* msg, size_t len);

void adaptive_send_advice(double nowMs)
{
	if (nowMs < sAdviceNextMs) return;
	sAdviceNextMs = nowMs + kAdviceMs;
	pc_netplay_adaptive::Advice a;
	a.seq            = ++sAdviceSeq;
	a.lateMs         = (uint32_t)(sLateMs + 0.5);
	a.lateEvents     = sLateEvents;
	a.delay          = (uint8_t)sCfg.localDelay;
	const double p50 = sStats.rtt_percentile(50);
	a.rttP50         = p50 < 0 ? 0xFFFF : (uint16_t)(p50 > 65534 ? 65534 : p50);
	a.flags          = sAdaptive ? 1 : 0;
	uint8_t msg[kHsHeaderLen + pc_netplay_adaptive::kAdvicePayload];
	memcpy(msg, xfer::kHsMagic, 4);
	msg[4] = kHsAdvice;
	msg[5] = (uint8_t)(kProtocolVersion & 0xFF);
	msg[6] = (uint8_t)((kProtocolVersion >> 8) & 0xFF);
	pc_netplay_adaptive::advice_encode(a, msg + kHsHeaderLen);
	hs_send(msg, sizeof(msg));
}

void adaptive_on_advice(const uint8_t* payload, size_t len)
{
	if (!sAdaptiveConfigured) return;
	pc_netplay_adaptive::Advice a;
	if (!pc_netplay_adaptive::advice_decode(payload, len, &a)) return;
	double delta    = 0;
	uint32_t events = 0;
	if (!sAdviceIn.take(a, &delta, &events)) return; // stale or duplicate
	const double now = now_ms();
	sDelayCtl.add_report(now);
	if (delta > 0) {
		sPeerLateMs += delta;
		sDelayCtl.add_lateness(now, delta, events);
	}
}

void adaptive_note_stall_turn(double startMs, double durMs)
{
	if (sAdvances == 0) return; // session start, not a stall
	if (!sStallOpen) {
		sStallOpen     = true;
		sStallStartMs  = startMs;
		sStallDurMs    = 0;
		sStallExcluded = sLgWindow.is_open() || sAdvances < sSettleUntil;
		sStallAhead    = sGekko != nullptr ? gekko_frames_ahead(sGekko) : 0.0f;
	}
	sStallDurMs += durMs;
}

void adaptive_poll(double nowMs)
{
	if (sGekko == nullptr || !sGekkoStarted) return;
	if (sLgWindow.is_open() || sAdvances < sSettleUntil) sDelayCtl.note_pause(nowMs);
	adaptive_send_advice(nowMs);
	if (nowMs < sRttNextMs) return;
	sRttNextMs = nowMs + kRttSampleMs;
	GekkoNetworkStats st;
	memset(&st, 0, sizeof(st));
	gekko_network_stats(sGekko, 1 - sLocalHandle, &st);
	if (st.avg_ping <= 0.0f && st.last_ping == 0) return; // no sample yet
	sStats.add_rtt((double)st.last_ping);
	pc_netplay_adaptive::RttSample s;
	s.atMs  = nowMs;
	s.rttMs = (double)st.last_ping;
	sDelayCtl.add_rtt(s);
}

void adaptive_stats_line(const char* tag)
{
	const double now = now_ms();
	uint64_t n10     = 0;
	double ms10      = 0;
	sStats.recent(now, 10000.0, &n10, &ms10);
	const pc_netplay_adaptive::FrameTimeHist& f = sStats.frames();
	printf("[netplay] %s: t=%.1fs frame=%llu delay=%u (start %u, range %u..%u, up %llu down %llu) "
	       "stalls=%llu total=%.0fms max=%.0fms excluded=%llu last10s=%llu/%.0fms reported=%.0fms "
	       "own-lag=%.0fms peer: delay=%d late=%.0fms reports=%llu "
	       "rtt last=%.0f p50=%.0f p95=%.0f jitter=%.1f samples=%llu "
	       "frames=%llu p50=%.1f p95=%.1f p99=%.1f max=%.1f >50ms=%llu >100ms=%llu\n",
	       tag, session_s(now), (unsigned long long)sAdvances, sCfg.localDelay, sDelayStart, sDelayLow, sDelayHigh,
	       (unsigned long long)sDelayUps, (unsigned long long)sDelayDowns, (unsigned long long)sStats.stall_count(),
	       sStats.stall_total_ms(), sStats.stall_max_ms(), (unsigned long long)sStats.stall_excluded(),
	       (unsigned long long)n10, ms10, sLateMs, sSelfOverrunMs, sAdviceIn.have() ? (int)sAdviceIn.last().delay : -1,
	       sPeerLateMs, (unsigned long long)sAdviceIn.reports(), sStats.rtt_last(), sStats.rtt_percentile(50),
	       sStats.rtt_percentile(95), sStats.rtt_jitter(), (unsigned long long)sStats.rtt_samples(),
	       (unsigned long long)f.count(), f.percentile(50), f.percentile(95), f.percentile(99), f.max_ms(),
	       (unsigned long long)f.at_least(50.0), (unsigned long long)f.at_least(100.0));
	fflush(stdout);
}

void adaptive_final_stats()
{
	if (!sAdaptiveConfigured || sStatsFinalDone || sSessionStartMs <= 0) return;
	sStatsFinalDone = true;
	adaptive_close_stall();
	adaptive_stats_line("stats final");
	printf("[netplay] delay timeline: +0.0s@0:%u%s%s\n", sDelayStart, sDelayTimeline.empty() ? "" : " ",
	       sDelayTimeline.c_str());
	printf("[netplay] frame-time histogram (ms): %s\n", sStats.frames().summary().c_str());
	fflush(stdout);
}

// ---- load guard (pc_netplay_loadguard.h) ----
//   PIKMIN_NETPLAY_LOAD_GUARD=0        (a) and (b) off
//   PIKMIN_NETPLAY_LOAD_KEEPALIVE=0    (a) off (the keep-alive pump)
//   PIKMIN_NETPLAY_LOAD_WINDOW=0       (b) off (the load-window extension)
//   PIKMIN_NETPLAY_LOAD_DISCONNECT_MS  the timeout inside a load window
//                                      (default 60000; never below the normal one)
// plus the PIKMIN_NETPLAY_TEST_STALL_* injector.
void loadguard_configure(unsigned normalMs)
{
	using namespace pc_netplay_loadguard;
	const bool guard  = read_unsigned_env("PIKMIN_NETPLAY_LOAD_GUARD", 1) != 0;
	sLgKeepAlive      = guard && read_unsigned_env("PIKMIN_NETPLAY_LOAD_KEEPALIVE", 1) != 0;
	const bool window = guard && read_unsigned_env("PIKMIN_NETPLAY_LOAD_WINDOW", 1) != 0;
	sLgWindow.configure(window, normalMs,
	                    read_unsigned_env("PIKMIN_NETPLAY_LOAD_DISCONNECT_MS", kDefaultLoadDisconnectMs));
	sLgGate.reset();
	sLgStallDone  = false;
	sLgStageLoads = 0;
	sLgPumps = sLgWindows = sLgLongTicks = 0;
	for (uint64_t& n : sLgSitePumps) n = 0;
	sLgSpeculative   = false;
	sLgLongestTickMs = sLgWorstGapMs = 0;
	sLgSummaryDone   = false;
	sInAdvance       = false;
	sLgMainThread    = std::this_thread::get_id();
	std::string err;
	if (!parse_stall_plan(getenv_nonempty("PIKMIN_NETPLAY_TEST_STALL_MS"), getenv_nonempty("PIKMIN_NETPLAY_TEST_STALL_AT"),
	                      getenv_nonempty("PIKMIN_NETPLAY_TEST_STALL_ROLE"),
	                      getenv_nonempty("PIKMIN_NETPLAY_TEST_STALL_SLICE_MS"), &sLgStall, &err))
		printf("[netplay] test stall: ignored (%s)\n", err.c_str());
	sLgConfigured = true;
	const char* ka = sLgKeepAlive ? "on (network poll at most every 50 ms inside a tick)" : "off";
	if (window)
		printf("[netplay] load guard: keep-alive=%s load window=on (disconnect timeout %u ms from a stage load until "
		       "%u frames later, %u ms otherwise)\n",
		       ka, sLgWindow.load_ms(), (unsigned)kLoadWindowFrames, normalMs);
	else
		printf("[netplay] load guard: keep-alive=%s load window=off (disconnect timeout %u ms throughout)\n", ka,
		       normalMs);
	if (sLgStall.enabled) {
		const char* at = sLgStall.at == StallAt::Load ? "load" : sLgStall.at == StallAt::Shader ? "shader" : "tick";
		printf("[netplay] test stall: armed at=%s#%llu role=%s total=%ums slice=%ums; this peer %s\n", at,
		       (unsigned long long)sLgStall.index, stall_role_name(sLgStall.roles), sLgStall.totalMs,
		       sLgStall.sliceMs, stall_applies_to(sLgStall, sCfg.isHost) ? "stalls" : "does not stall");
	}
	fflush(stdout);
}

// One keep-alive pump inside a tick: GekkoNet's network layer only (receive,
// ack, resend unacked inputs, NetworkHealth, idle-timer check; no Advance,
// no event consumed). A disconnect found here is queued as a session event
// and handled right after this tick.
void loadguard_pump(double now, int site)
{
	if (sGekko != nullptr) gekko_network_poll(sGekko);
	if (site >= 0 && site < pc_netplay_loadguard::kSiteCount) ++sLgSitePumps[site];
	const double gap = now - sLgLastPumpMs;
	if (gap > sLgTickMaxGapMs) sLgTickMaxGapMs = gap;
	sLgLastPumpMs = now;
	sLgGate.pumped(now);
	++sLgTickPumps;
	++sLgPumps;
}

void loadguard_keepalive(int site)
{
	if (!sInAdvance || !sLgKeepAlive || sPhase != kSession || sGekko == nullptr) return;
	if (std::this_thread::get_id() != sLgMainThread) return;
	const double now = now_ms();
	if (!sLgGate.due(now)) return;
	loadguard_pump(now, site);
}

// Test stall (wall clock only): slices separated by the keep-alive entry
// the real long loops call.
void loadguard_run_stall(const char* where)
{
	using namespace pc_netplay_loadguard;
	sLgStallDone = true;
	printf("[netplay] test stall: begin role=%s at=%s frame=%u total=%ums slice=%ums keep-alive=%s\n",
	       sCfg.isHost ? "host" : "join", where, sLgTickFrame, sLgStall.totalMs, sLgStall.sliceMs,
	       sLgKeepAlive ? "on" : "off");
	fflush(stdout);
	const double t0 = now_ms();
	for (unsigned i = 0;; ++i) {
		const unsigned ms = stall_slice_ms(sLgStall, i);
		if (ms == 0) break;
		if (i > 0) loadguard_keepalive(kSiteStall);
		std::this_thread::sleep_for(std::chrono::milliseconds(ms));
	}
	printf("[netplay] test stall: end after %.0f ms\n", now_ms() - t0);
	fflush(stdout);
}

void loadguard_maybe_stall(pc_netplay_loadguard::StallAt at, uint64_t index, const char* where)
{
	if (sLgStallDone || !sInAdvance || sLgSpeculative || !stall_applies_to(sLgStall, sCfg.isHost)) return;
	if (sLgStall.at != at || sLgStall.index != index) return;
	loadguard_run_stall(where);
}

void loadguard_tick_begin(uint32_t frame, bool speculative)
{
	sLgSpeculative  = speculative;
	sInAdvance      = true;
	sLgTickFrame    = frame;
	sLgTickStartMs  = now_ms();
	sLgLastPumpMs   = sLgTickStartMs;
	sLgTickMaxGapMs = 0;
	sLgTickPumps    = 0;
	// tick:<n> is the hash tick (GekkoNet frame F is hash tick F+1).
	loadguard_maybe_stall(pc_netplay_loadguard::StallAt::Tick, (uint64_t)frame + 1, "tick");
}

void loadguard_tick_end()
{
	sInAdvance       = false;
	const double now = now_ms();
	const double gap = now - sLgLastPumpMs;
	if (gap > sLgTickMaxGapMs) sLgTickMaxGapMs = gap;
	const double tickMs = now - sLgTickStartMs;
	if (tickMs > sLgLongestTickMs) sLgLongestTickMs = tickMs;
	if (tickMs > pc_netplay_loadguard::kLongTickMs) {
		++sLgLongTicks;
		if (sLgTickMaxGapMs > sLgWorstGapMs) sLgWorstGapMs = sLgTickMaxGapMs;
		printf("[netplay] long tick: frame=%u took %.0f ms; keep-alive pumps=%llu, longest stretch without a network "
		       "poll %.0f ms (load window %s)\n",
		       sLgTickFrame, tickMs, (unsigned long long)sLgTickPumps, sLgTickMaxGapMs,
		       sLgWindow.is_open() ? "open" : "closed");
		fflush(stdout);
	}
	if (stall_trace() && tickMs >= 50.0) {
		printf("[netplay] stall-trace: slow tick frame=%u tick=%.1fms window=%d\n", sLgTickFrame, tickMs,
		       (int)sLgWindow.is_open());
		fflush(stdout);
	}
}

// After each completed Advance: close the load window once frame F+30 ran.
void loadguard_after_advance(uint32_t frame, bool speculative)
{
	if (speculative || !sLgWindow.after_advance(frame)) return;
	if (sGekko != nullptr) gekko_set_disconnect_timeout(sGekko, sLgWindow.normal_ms());
	printf("[netplay] load window: closed at frame=%u (opened at frame=%u, last stage load at frame=%u, %.0f ms "
	       "open); disconnect timeout %u ms\n",
	       frame, sLgWindowFirst, sLgWindow.open_frame(), now_ms() - sLgWindowOpenMs, sLgWindow.normal_ms());
	fflush(stdout);
}

void loadguard_on_stage_load()
{
	if (!sInAdvance || sPhase != kSession || sGekko == nullptr) return;
	if (sLgSpeculative) {
		loadguard_keepalive(pc_netplay_loadguard::kSiteStageLoad);
		return;
	}
	++sLgStageLoads;
	const bool wasOpen = sLgWindow.is_open();
	if (sLgWindow.open(sLgTickFrame)) {
		gekko_set_disconnect_timeout(sGekko, sLgWindow.load_ms());
		sLgWindowOpenMs = now_ms();
		sLgWindowFirst  = sLgTickFrame;
		++sLgWindows;
		printf("[netplay] load window: open at frame=%u (stage load %u); disconnect timeout %u ms until frame=%u\n",
		       sLgTickFrame, sLgStageLoads, sLgWindow.load_ms(), sLgWindow.close_frame());
		fflush(stdout);
	} else if (wasOpen) {
		printf("[netplay] load window: stage load %u at frame=%u extends it to frame=%u\n", sLgStageLoads, sLgTickFrame,
		       sLgWindow.close_frame());
		fflush(stdout);
	}
	loadguard_keepalive(pc_netplay_loadguard::kSiteStageLoad);
	loadguard_maybe_stall(pc_netplay_loadguard::StallAt::Load, sLgStageLoads, "load");
}

void loadguard_summary()
{
	if (!sLgConfigured || sLgSummaryDone) return;
	sLgSummaryDone = true;
	using pc_netplay_loadguard::site_name;
	printf("[netplay] load guard summary: keep-alive pumps=%llu (%s=%llu %s=%llu %s=%llu %s=%llu %s=%llu) load "
	       "windows=%llu long ticks=%llu longest tick=%.0f ms longest stretch without a network poll in a long "
	       "tick=%.0f ms\n",
	       (unsigned long long)sLgPumps, site_name(1), (unsigned long long)sLgSitePumps[1], site_name(2),
	       (unsigned long long)sLgSitePumps[2], site_name(3), (unsigned long long)sLgSitePumps[3], site_name(4),
	       (unsigned long long)sLgSitePumps[4], site_name(5), (unsigned long long)sLgSitePumps[5],
	       (unsigned long long)sLgWindows, (unsigned long long)sLgLongTicks, sLgLongestTickMs, sLgWorstGapMs);
	fflush(stdout);
}

// ---- lifecycle ----

void inlog_flush();

#if PC_ONLINE
void online_teardown();
#endif

void stop_session()
{
#if PC_ONLINE
	online_teardown();
#endif
	inlog_flush();
	adaptive_final_stats();
	loadguard_summary();
	pc_netplay_camlead_session_end();
	sInAdvance = false;
	if (sGekko != nullptr) {
		GekkoSession* s = sGekko;
		sGekko          = nullptr;
		gekko_destroy(&s);
	}
	delete sLossy;
	sLossy   = nullptr;
	sAdapter = nullptr;
	delete sLink;
	sLink = nullptr;
	if (sSock != nullptr) {
		sSock->close();
		delete sSock;
		sSock = nullptr;
	}
	// The link goes first: its adapter must not outlive the agent's queue.
	delete sIceLink;
	sIceLink = nullptr;
	if (sIce != nullptr) {
		sIce->close();
		delete sIce;
		sIce = nullptr;
	}
	netplay_timer_power_opt(false);
}

void request_quit()
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = SDL_QUIT;
	SDL_PushEvent(&ev);
}

void replay_load(const char* path);

void parse_config()
{
	sCfg = Config();
	const PcNetplayLaunch& launch = pc_netplay_launch_setup();
	const char* hostCli   = argv_value(sArgc, sArgv, "--netplay-host");
	const char* joinCli   = argv_value(sArgc, sArgv, "--netplay-join");
	const char* hostVal   = hostCli != nullptr ? hostCli : getenv_nonempty("PIKMIN_NETPLAY_HOST");
	const char* joinVal   = joinCli != nullptr ? joinCli : getenv_nonempty("PIKMIN_NETPLAY_JOIN");
	const char* replayCli = argv_value(sArgc, sArgv, "--netplay-replay-log");
	const char* replayVal = replayCli != nullptr ? replayCli : getenv_nonempty("PIKMIN_NETPLAY_REPLAY_LOG");
	if (hostVal != nullptr && joinVal != nullptr) usage_exit("--netplay-host and --netplay-join are exclusive%s", "");
	const char* onlineHost = argv_value(sArgc, sArgv, "--online-host");
	const char* onlineJoin = argv_value(sArgc, sArgv, "--online-join");
	// Why the last online join failed (the in-game menu shows it on the title).
	if (const char* e = argv_value(sArgc, sArgv, "--online-error")) sOnlineError = e;
	if (onlineHost != nullptr || onlineJoin != nullptr) {
		if (onlineHost != nullptr && onlineJoin != nullptr) usage_exit("--online-host and --online-join are exclusive%s", "");
		if (hostVal != nullptr || joinVal != nullptr || replayVal != nullptr || launch.active)
			usage_exit("--online-host/--online-join run alone, without the lockstep switches%s", "");
#if !(PIKI_NECTAR_ONLINE && defined(NECTAR_NET_HAS_GNS))
		usage_exit("this build has no online transport (configure with -DNECTAR_ONLINE=ON -DNECTAR_NET_ENABLE_GNS=ON)%s", "");
#endif
	}
	if (replayVal != nullptr && (hostVal != nullptr || joinVal != nullptr || launch.active))
		usage_exit("--netplay-replay-log runs alone, without a host or join switch%s", "");
	int onlineSave = 0;
	if (const char* slot = argv_value(sArgc, sArgv, "--online-save")) {
		if (onlineHost == nullptr) usage_exit("--online-save is for the host; the guest gets the host's file%s", "");
		if (std::strlen(slot) != 1 || slot[0] < '1' || slot[0] > '3') usage_exit("bad --online-save %s (want 1, 2 or 3)", slot);
		onlineSave = slot[0] - '0';
	}
	// Continuing a file: the host plays on its own card (only the host writes
	// progress) and sends a copy to the guest. Otherwise both peers start a
	// fresh run in a private folder, like the lockstep launcher.
	// Always, even when NECTAR_SAVE_DIR names the player's folder (Android
	// sets it): the guest's copy of the host's card must never land there.
	if ((onlineHost != nullptr && onlineSave == 0) || onlineJoin != nullptr) {
		{
			char stamp[32];
			const time_t now = time(nullptr);
			strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
			const std::string dir = std::string("online-sessions/") + (onlineHost != nullptr ? "host-" : "guest-") + stamp
			                      + "/save";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			if (ec) usage_exit("cannot create the online save folder %s", dir.c_str());
#ifdef _WIN32
			_putenv_s("NECTAR_SAVE_DIR", dir.c_str());
#else
			setenv("NECTAR_SAVE_DIR", dir.c_str(), 1);
#endif
			printf("[online] this session saves to %s\n", dir.c_str());
		}
	}
	if (onlineHost != nullptr) {
		char* end    = nullptr;
		const long p = strtol(onlineHost, &end, 10);
		if (end == onlineHost || *end != '\0' || p < 0 || p > 65535) usage_exit("bad --online-host port %s", onlineHost);
		sCfg.isHost         = true;
		sCfg.active         = true;
		sCfg.online         = true;
		sCfg.onlinePort     = (uint16_t)p;
		sCfg.onlineSaveSlot = onlineSave;
		// The code is a secret and never goes to the log; until the launcher
		// shows it, the host must name a file for it.
		const char* out          = argv_value(sArgc, sArgv, "--online-code-out");
		sCfg.onlineCodeClipboard = argv_has(sArgc, sArgv, "--online-code-clipboard");
		if (out == nullptr && !sCfg.onlineCodeClipboard)
			usage_exit("--online-host needs --online-code-out <file> or --online-code-clipboard to hand over its code%s", "");
		if (out != nullptr) sCfg.onlineCodeOut = out;
	} else if (onlineJoin != nullptr) {
		std::string code = onlineJoin;
		if (!code.empty() && code[0] == '@') {
			FILE* f = fopen(code.c_str() + 1, "rb");
			if (f == nullptr) usage_exit("cannot read the online code file %s", code.c_str() + 1);
			char buf[512];
			const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
			fclose(f);
			buf[n] = '\0';
			code   = buf;
			// The in-game menu hands the code over in a file read only once.
			if (argv_has(sArgc, sArgv, "--online-code-once")) {
				std::remove(onlineJoin + 1);
				sCfg.onlineFromMenu = true;
			}
			while (!code.empty() && (code.back() == '\n' || code.back() == '\r' || code.back() == ' ')) code.pop_back();
		}
		sCfg.active           = true;
		sCfg.online           = true;
		sCfg.onlineCode       = code;
		sCfg.onlinePredictive = argv_has(sArgc, sArgv, "--online-predictive");
	} else if (launch.active) {
		sCfg.isHost = launch.isHost;
		sCfg.active = true;
		sCfg.ice    = true;
	} else if (replayVal != nullptr) {
		// The replay plays the host's role with no network; the sim is the
		// same for both peers.
		replay_load(replayVal);
		sCfg.isHost = true;
		sCfg.active = true;
		sCfg.replay = true;
	} else if (hostVal != nullptr) {
		char* end    = nullptr;
		const long p = strtol(hostVal, &end, 10);
		if (end == hostVal || *end != '\0' || p <= 0 || p > 65535) usage_exit("bad --netplay-host port %s", hostVal);
		sCfg.isHost   = true;
		sCfg.active   = true;
		sCfg.hostPort = (uint16_t)p;
	} else if (joinVal != nullptr) {
		uint32_t ip   = 0;
		uint16_t port = 0;
		if (!pc_netplay_transport::parse_endpoint(joinVal, &ip, &port))
			usage_exit("bad --netplay-join endpoint %s (want ip:port)", joinVal);
		sCfg.active   = true;
		sCfg.joinIp   = ip;
		sCfg.joinPort = port;
	}
	if (!sCfg.active) {
		sPhase = kIdle;
		return;
	}
	if (const char* d = getenv_nonempty("PIKMIN_NETPLAY_DELAY")) {
		if (std::strcmp(d, "auto") == 0) {
			sCfg.delayAuto = true;
		} else {
			sCfg.localDelay = read_unsigned_env("PIKMIN_NETPLAY_DELAY", 2);
			if (sCfg.localDelay < 1) sCfg.localDelay = 1;
			if (sCfg.localDelay > kMaxLocalDelay) sCfg.localDelay = kMaxLocalDelay;
		}
	}
	sCfg.seed = read_u32_env("PIKMIN_NETPLAY_SEED", 0);
	// --netplay-input keyboard|gamepad[:N]|auto picks the device that feeds
	// this peer's captain; any other value is a scripted input file (pkni v2,
	// tools/netplay/gen_inputs.py), like PIKMIN_NETPLAY_LOCAL_INPUT_FILE.
	if (const char* in = argv_value(sArgc, sArgv, "--netplay-input")) {
		pc_netplay_input_sel::Kind kind = pc_netplay_input_sel::kInputAuto;
		int index                       = 0;
		if (pc_netplay_input_sel::parse_input_spec(in, &kind, &index)) {
			sCfg.inputSpec    = in;
			sCfg.inputKind    = (int)kind;
			sCfg.inputGamepad = index;
			// A gamepad peer keeps its pad while another window has focus (a
			// two-window test on one PC). Must be set before SDL_Init.
			if (kind == pc_netplay_input_sel::kInputGamepad)
				SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
		} else {
			sCfg.localInputFile = in;
		}
	}
	if (sCfg.localInputFile.empty()) {
		if (const char* lif = getenv_nonempty("PIKMIN_NETPLAY_LOCAL_INPUT_FILE")) sCfg.localInputFile = lif;
	}
	if (const char* ea = getenv_nonempty("PIKMIN_NETPLAY_EXIT_AFTER_TICKS")) {
		char* end             = nullptr;
		const unsigned long n = strtoul(ea, &end, 10);
		if (end != ea && *end == '\0' && n > 0) sCfg.exitAfter = n;
	}
	// Mode: --netplay-mode coop|vs, --vs-map (VS), the offer (the joiner
	// follows the host) or the recorded session; co-op otherwise.
	int asked = 0;
	if (const char* m = argv_value(sArgc, sArgv, "--netplay-mode")) {
		if (std::strcmp(m, "coop") == 0) asked = xfer::kModeCoopDirect;
		else if (std::strcmp(m, "vs") == 0) asked = xfer::kModeVs;
		else usage_exit("bad --netplay-mode %s (want coop or vs)", m);
	}
	const bool vsMapArg = pc_vs_direct_requested();
	if (vsMapArg && asked == xfer::kModeCoopDirect) usage_exit("--vs-map plays VS; it cannot be used with --netplay-mode coop%s", "");
	if (vsMapArg) asked = xfer::kModeVs;
	std::string vsMapJson;
	if (launch.active && !launch.isHost) {
		const int offered = launch.mode == 2 ? xfer::kModeVs : xfer::kModeCoopDirect;
		if (asked != 0 && asked != offered)
			usage_exit("the host's offer is for %s; join it with the matching mode (Join Co-op or Join VS)",
			           offered == xfer::kModeVs ? "VS" : "co-op");
		asked     = offered;
		vsMapJson = launch.vsMapJson;
	} else if (sCfg.replay) {
		const std::string rec = sReplay.log.meta_get("descriptor");
		if (pc_netplay_launch_parse_mode(rec, &vsMapJson) == 2) asked = xfer::kModeVs;
	}
	sCfg.mode = asked == xfer::kModeVs ? xfer::kModeVs : xfer::kModeCoopDirect;
	if (sCfg.mode == xfer::kModeVs) {
		sCfg.vsMapFile = vsMapArg;
		// Online VS has no map transfer: both peers play the built-in arena.
		if (sCfg.online && !vsMapArg) pc_vs_map_select("");
		if (!vsMapJson.empty()) {
			// The host's map.json, written to this run's folder and played as a file.
			const std::string dir  = launch.active ? launch.runDir : std::string(".");
			const std::string path = dir + "/" + (sCfg.replay ? "replay-vs-map.json" : "vs-map.json");
			FILE* f                = fopen(path.c_str(), "wb");
			const bool written     = f != nullptr && fwrite(vsMapJson.data(), 1, vsMapJson.size(), f) == vsMapJson.size();
			if (f != nullptr) fclose(f);
			if (!written) usage_exit("cannot write the host's VS map to %s", path.c_str());
			if (vsMapArg) printf("[netplay] vs map: --vs-map ignored; playing the host's map from the offer\n");
			pc_vs_map_select_file(path);
			sCfg.vsMapFile = true;
			printf("[netplay] vs map: the host's map.json (%llu B) -> %s\n", (unsigned long long)vsMapJson.size(),
			       path.c_str());
		}
		if (const char* t = getenv_nonempty("PIKMIN_NETPLAY_TEST_VS_MATCH_SECONDS")) {
			if (env_is_one("PIKMIN_NETPLAY_TEST_KNOBS")) {
				sCfg.testVsMatchSeconds = (float)atof(t);
				if (sCfg.testVsMatchSeconds < 0) sCfg.testVsMatchSeconds = 0;
				pc_vs_set_test_match_seconds(sCfg.testVsMatchSeconds);
				printf("[netplay] test: VS match length %g s\n", sCfg.testVsMatchSeconds);
			}
		}
	}
	sLocalRole = sCfg.isHost ? 0 : 1;
	// Each peer presents its own captain full screen: host P1, joiner P2.
	pc_netplay_present_set_local_player_default(sLocalRole);
	pc_netplay_camlead_session_begin(sLocalRole);
	// Both peers boot straight into two-captain co-op with a fresh save, or
	// into a VS match (the --vs-map direct boot).
	PcCoopSwitch sw = pc_coop_switch_parse(sArgc, sArgv);
	sw.coop         = true;
	if (launch.active && !launch.isHost && launch.haveCaptains) {
		// The joiner plays the host's captains (the offer's descriptor).
		sw.captainP1 = launch.captainP1;
		sw.captainP2 = launch.captainP2;
	}
	if (sCfg.replay) {
		int p1 = sw.captainP1, p2 = sw.captainP2;
		if (pc_netplay_launch_parse_descriptor(sReplay.log.meta_get("descriptor"), &p1, &p2)) {
			sw.captainP1 = p1;
			sw.captainP2 = p2;
		}
	}
	pc_coop_switch_apply(sw);
	if (sCfg.mode == xfer::kModeVs) {
		pc_vs_direct_set_online(true);
		if (!vsMapArg) pc_vs_direct_request();
	} else {
		pc_coop_direct_request();
	}
	if (sCfg.online && sCfg.isHost) pc_coop_direct_set_slot(sCfg.onlineSaveSlot);
	sPhase = kHandshake;
}

// Run modes the session cannot keep in lockstep yet. They are chosen on the
// new-game prompts the direct boot skips, so this only fires if something
// arms one anyway; both peers see it on the same tick.
const char* unsupported_run_mode()
{
	if (pc_permadeath_active()) return "Permadeath";
	if (pc_hardmode_active()) return "Hard mode";
	if (pc_randomizer_active()) return "the save randomizer";
	if (pc_speedrun_active()) return "Speedrun mode";
	if (sCfg.mode != xfer::kModeVs && pc_vs_active()) return "VS";
	return nullptr;
}

// ---- handshake ----

void hs_send(const uint8_t* msg, size_t len)
{
	if (sIce != nullptr) {
		sIce->send_payload(pc_netplay_ice::kChannelHandshake, msg, len);
		return;
	}
	if (sSock == nullptr) return;
	if (sCfg.isHost) {
		if (sHaveRemote) sSock->send_to(pc_netplay_transport::kChannelHandshake, msg, len, sRemoteIp, sRemotePort);
	} else {
		sSock->send_payload(pc_netplay_transport::kChannelHandshake, msg, len);
	}
}

std::vector<pc_netplay_transport::UdpSocket::Datagram> hs_drain()
{
	if (sIceLink != nullptr) {
		std::vector<pc_netplay_transport::UdpSocket::Datagram> out;
		for (pc_netplay_ice::IceSocket::Datagram& g : sIceLink->drain_handshake()) {
			pc_netplay_transport::UdpSocket::Datagram d;
			d.channel         = g.channel;
			d.payload         = std::move(g.payload);
			d.fromIpHostOrder = g.fromIpHostOrder;
			d.fromPort        = g.fromPort;
			out.push_back(std::move(d));
		}
		return out;
	}
	if (sLink != nullptr) return sLink->drain_handshake();
	return std::vector<pc_netplay_transport::UdpSocket::Datagram>();
}

void send_hello_msg(uint8_t type, uint8_t refuseField, uint64_t nonce)
{
	uint8_t msg[kHsLen];
	Hello h = sLocal;
	h.nonce = nonce;
	xfer::encode_hello(type, kProtocolVersion, h, refuseField, msg);
	if (type == xfer::kHsAck && sPhase == kHandshake) {
		if (!sDropFinalAckInit) {
			sDropFinalAckInit = true;
			sDropFinalAck     = env_is_one("PIKMIN_NETPLAY_TEST_DROP_FINAL_ACK");
		}
		if (sDropFinalAck) {
			printf("[netplay] test: dropped final handshake Ack (echo=%llu)\n", (unsigned long long)nonce);
			fflush(stdout);
			return;
		}
	}
	hs_send(msg, sizeof(msg));
}

void log_refusal_detail(uint8_t field, const Hello* peer)
{
	if (peer == nullptr) return;
	if (field == xfer::kFieldBuild)
		printf("[netplay] refusal: build local=%s peer=%s; local manifest: %s\n", sBuildHexStr.c_str(),
		       pc_netplay_sha::hex(peer->build, 32).c_str(), sBuildManifest.c_str());
	else if (field == xfer::kFieldConfig)
		printf("[netplay] refusal: config local=%s peer=%s; local config text: %s\n", sCfgHexStr.c_str(),
		       pc_netplay_sha::hex(peer->cfg, 32).c_str(), sCfgText.c_str());
	else if (field == xfer::kFieldSeed)
		printf("[netplay] refusal: seed local=%u peer=%u (PIKMIN_NETPLAY_SEED)\n", sLocal.seed, peer->seed);
	else if (field == xfer::kFieldMode)
		printf("[netplay] refusal: mode local=%s %d,%d peer=%s %d,%d (both must host/join the same mode, co-op "
		       "or VS, with the same captains)\n",
		       xfer::mode_name(sLocal.mode.mode), sLocal.mode.captainP1, sLocal.mode.captainP2,
		       xfer::mode_name(peer->mode.mode), peer->mode.captainP1, peer->mode.captainP2);
	else if (field == xfer::kFieldVsMap)
		printf("[netplay] refusal: vsmap local=%s peer=%s: the two PCs compiled different VS maps. Over ICE the "
		       "offer carries the host's map.json; otherwise both need the same map file. The game files must be "
		       "the same version too (the map takes its textures from them).\n",
		       sVsMapHexStr.c_str(), pc_netplay_sha::hex(peer->vsMapSha, 32).c_str());
}

[[noreturn]] void refuse_and_exit(uint8_t field, const Hello* peer = nullptr)
{
	printf("[netplay] handshake refused: %s\n", xfer::field_name(field));
	log_refusal_detail(field, peer);
	fflush(stdout);
	// Best effort: tell the peer why (the channel is unreliable).
	for (int i = 0; i < 5; ++i) send_hello_msg(xfer::kHsRefuse, field, 0);
	stop_session();
	session_exit(npx::kRefused);
}

[[noreturn]] void peer_refused_exit(uint8_t field)
{
	printf("[netplay] handshake refused: %s (by the peer)\n", xfer::field_name(field == 0 ? 99 : field));
	fflush(stdout);
	stop_session();
	session_exit(npx::kRefused);
}

double handshake_rtt_median()
{
	if (sHsSamples.empty()) return -1;
	std::vector<double> v = sHsSamples;
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

// Returns true once the GekkoNet session may start.
bool handshake_pump()
{
	const double now = now_ms();
	if (sHsStartMs == 0) {
		sHsStartMs    = now;
		sHsLastSendMs = 0;
	}
	const double timeoutMs = (double)read_unsigned_env("PIKMIN_NETPLAY_HANDSHAKE_TIMEOUT_MS", 30000);
	if (now - sHsStartMs > timeoutMs) {
		printf("[netplay] handshake timeout after %.0f ms (sentAck=%d gotAck=%d rttSamples=%llu)\n", now - sHsStartMs,
		       (int)sSentAck, (int)sGotAck, (unsigned long long)sHsSamples.size());
		fflush(stdout);
		stop_session();
		session_exit(npx::kRefused);
	}
	if (now - sHsLastSendMs >= 100) {
		const uint64_t nonce = sHsNextNonce++;
		send_hello_msg(xfer::kHsHello, 0, nonce);
		sHsSendTimes.emplace_back(nonce, now);
		if (sHsSendTimes.size() > 64) sHsSendTimes.erase(sHsSendTimes.begin(), sHsSendTimes.begin() + (sHsSendTimes.size() - 64));
		// Periodic Ack resends echo nonce 0 (ignored for RTT); they exist only
		// so a lost Ack cannot stall the peer.
		if (sSentAck) send_hello_msg(xfer::kHsAck, 0, 0);
		sHsLastSendMs = now;
	}
	for (auto& g : hs_drain()) {
		uint8_t hdrType   = 0;
		uint16_t hdrProto = 0;
		if (!xfer::decode_hello_header(g.payload.data(), g.payload.size(), &hdrType, &hdrProto)) continue;
		if (sCfg.isHost && !sHaveRemote) {
			// Learn the joiner's endpoint from its first header-valid
			// datagram (before any refusal, so a refuse can go back).
			sRemoteIp   = g.fromIpHostOrder;
			sRemotePort = g.fromPort;
			sHaveRemote = true;
			if (sSock != nullptr) sSock->set_peer(sRemoteIp, sRemotePort);
			printf("[netplay] handshake: peer %u.%u.%u.%u:%u\n", (sRemoteIp >> 24) & 0xFF, (sRemoteIp >> 16) & 0xFF,
			       (sRemoteIp >> 8) & 0xFF, sRemoteIp & 0xFF, (unsigned)sRemotePort);
			fflush(stdout);
		}
		if (sCfg.isHost && (g.fromIpHostOrder != sRemoteIp || g.fromPort != sRemotePort)) continue;
		// The stable header first: a protocol mismatch refuses fast even
		// across length changes.
		if (hdrProto != kProtocolVersion) {
			if (hdrType == xfer::kHsRefuse && g.payload.size() > xfer::kHsRefuseOffset)
				peer_refused_exit(g.payload[xfer::kHsRefuseOffset]);
			refuse_and_exit(xfer::kFieldProto);
		}
		if (hdrType == kHsAdvice) continue;
		uint8_t type   = 0;
		uint16_t proto = 0;
		Hello h;
		uint8_t refuse = 0;
		if (!xfer::decode_hello(g.payload.data(), g.payload.size(), &type, &proto, &h, &refuse)) continue;
		if (type == xfer::kHsRefuse) peer_refused_exit(refuse);
		if (type != xfer::kHsHello && type != xfer::kHsAck) continue;
		const uint8_t field = xfer::first_mismatch(sLocal, h);
		if (field != 0) refuse_and_exit(field, &h);
		if (sCaptainMissing >= 0) {
			printf("[netplay] refusal: captain %d needs a local Pikmin 2 install on this PC\n", sCaptainMissing);
			refuse_and_exit(xfer::kFieldCaptain, &h);
		}
		if (type == xfer::kHsHello) {
			// Ack every Hello at once, echoing its nonce.
			send_hello_msg(xfer::kHsAck, 0, h.nonce);
			sSentAck = true;
		} else {
			sGotAck = true;
			if (h.nonce == 0) continue;
			for (auto& st : sHsSendTimes) {
				if (st.first == h.nonce) {
					const double sample = now - st.second;
					if (sample >= 0 && sample < 60000) sHsSamples.push_back(sample);
					break;
				}
			}
		}
	}
	// DELAY=auto needs at least 5 RTT samples for a meaningful median.
	if (sCfg.delayAuto && sSentAck && sGotAck && sHsSamples.size() < 5) return false;
	return sSentAck && sGotAck;
}

// Late or duplicate handshake traffic once the session is up: a peer whose
// last Ack was lost keeps sending Hello, so every Hello gets an Ack (never
// an Ack for an Ack, or they would bounce for the whole session). Also the
// peer's stall advice.
void answer_handshake_in_session()
{
	if (sIceLink == nullptr && (sLink == nullptr || sSock == nullptr)) return;
	for (auto& g : hs_drain()) {
		uint8_t hdrType   = 0;
		uint16_t hdrProto = 0;
		if (!xfer::decode_hello_header(g.payload.data(), g.payload.size(), &hdrType, &hdrProto)) continue;
		if (hdrProto != kProtocolVersion) continue;
		if (sCfg.isHost && sHaveRemote && (g.fromIpHostOrder != sRemoteIp || g.fromPort != sRemotePort)) continue;
		if (hdrType == kHsAdvice) {
			adaptive_on_advice(g.payload.data() + kHsHeaderLen, g.payload.size() - kHsHeaderLen);
			continue;
		}
		uint8_t type   = 0;
		uint16_t proto = 0;
		Hello h;
		uint8_t refuse = 0;
		if (!xfer::decode_hello(g.payload.data(), g.payload.size(), &type, &proto, &h, &refuse)) continue;
		if (type != xfer::kHsHello) continue;
		send_hello_msg(xfer::kHsAck, 0, h.nonce);
		printf("[netplay] answered in-session Hello with Ack (echo=%llu)\n", (unsigned long long)h.nonce);
		fflush(stdout);
	}
}

// ---- end banner ----

// Keeps presenting frames with the banner after the session ended, until it
// times out or a key or button closes it. PIKMIN_NETPLAY_BANNER_MS (default
// 10000, at most 60000, 0 = no banner).
void begin_banner(int exitCode, const char* title, bool error, const std::vector<std::string>& lines)
{
	sBannerMs = (double)read_unsigned_env("PIKMIN_NETPLAY_BANNER_MS", 10000u);
	if (sBannerMs > 60000.0) sBannerMs = 60000.0;
	sBannerExit    = exitCode;
	sBannerStartMs = now_ms();
	sBannerTitle   = title;
	sBannerError   = error;
	sBannerLines   = lines;
	sBannerOn      = sBannerMs > 0;
	if (sBannerOn) {
		pc_window_discard_button_presses();
		printf("[netplay] end banner: shown for up to %.0f ms (any key or button closes it)\n", sBannerMs);
		fflush(stdout);
	}
}

// ---- forensics files ----

std::string forensics_dir()
{
	if (const char* e = getenv_nonempty("PIKMIN_NETPLAY_FORENSICS_DIR")) return e;
	const PcNetplayLaunch& l = pc_netplay_launch_setup();
	if (l.active && !l.runDir.empty()) return l.runDir;
	return ".";
}

std::string forensics_path(const char* name)
{
	std::string d = forensics_dir();
	if (!d.empty() && d.back() != '/' && d.back() != '\\') d += "/";
	return d + name;
}

void forensics_enable()
{
	sForensicsOn = read_unsigned_env("PIKMIN_NETPLAY_FORENSICS", 1) != 0;
	pc_state_dump_set_enabled(sForensicsOn);
	sNudge            = NudgeKnob();
	const char* nudge = getenv_nonempty("PIKMIN_NETPLAY_TEST_DESYNC_NUDGE");
	if (nudge == nullptr || !env_is_one("PIKMIN_NETPLAY_TEST_KNOBS")) return;
	char* end         = nullptr;
	const long long f = strtoll(nudge, &end, 10);
	if (end == nudge || f < 0) return;
	sNudge.frame = f;
	if (*end == ':') {
		++end;
		if (std::strncmp(end, "navi", 4) == 0) sNudge.kind = fx::kNavi;
		else if (std::strncmp(end, "teki", 4) == 0) sNudge.kind = fx::kTeki;
		else sNudge.kind = fx::kPiki;
		const char* colon = std::strchr(end, ':');
		if (colon != nullptr) sNudge.ord = (int)strtol(colon + 1, nullptr, 10);
	}
	printf("[netplay] test: this peer nudges %s#%d at frame=%lld (desync injection)\n",
	       fx::kind_name((uint8_t)sNudge.kind), sNudge.ord, (long long)sNudge.frame);
	fflush(stdout);
}

// Start of a frame's tick, before the sim runs: the test nudge.
void forensics_tick_begin(int frame)
{
	if (sNudge.frame < 0 || sNudge.done || (int64_t)frame != sNudge.frame) return;
	sNudge.done = true;
	const bool ok = pc_state_dump_test_nudge(sNudge.kind, sNudge.ord);
	printf("[netplay] test: nudge %s#%d at frame=%d %s\n", fx::kind_name((uint8_t)sNudge.kind), sNudge.ord, frame,
	       ok ? "applied (x += 1.0)" : "found no such object");
	fflush(stdout);
}

// TEST ONLY, with PIKMIN_NETPLAY_TEST_KNOBS=1:
// PIKMIN_NETPLAY_TEST_SETTINGS_SAVE_AT=<tick> runs the settings menu's save
// path once after that tick, as closing F1 does (the adoption test checks
// that the file keeps this player's own values).
void test_settings_save(uint64_t tick)
{
	static int64_t at = -2;
	if (at == -2) {
		at = -1;
		const char* v = getenv_nonempty("PIKMIN_NETPLAY_TEST_SETTINGS_SAVE_AT");
		if (v != nullptr && env_is_one("PIKMIN_NETPLAY_TEST_KNOBS")) at = strtoll(v, nullptr, 10);
	}
	if (at < 0 || (int64_t)tick != at) return;
	pc_settings_rows_begin();
	pc_settings_rows_end(true);
	printf("[netplay] test: settings saved after tick=%llu (the F1 close path)\n", (unsigned long long)tick);
	fflush(stdout);
}

// ---- session input log ----

void inlog_open()
{
	const PcNetplayLaunch& l = pc_netplay_launch_setup();
	std::string path;
	if (const char* e = getenv_nonempty("PIKMIN_NETPLAY_INPUT_LOG")) {
		if (std::strcmp(e, "0") == 0) return;
		path = e;
	} else {
		if (!l.active || l.runDir.empty()) return;
		path = forensics_path("session-inputs.pknl");
	}
	std::string m;
	// A meta value is one line: the VS map.json in the descriptor has line
	// breaks, which are JSON whitespace, so they become spaces.
	auto kv = [&](const char* k, const std::string& v) {
		m += k;
		m += ' ';
		for (char c : v) m += (c == '\n' || c == '\r') ? ' ' : c;
		m += '\n';
	};
	kv("format", "pknl-1");
	kv("role", sCfg.isHost ? "host" : "join");
	kv("transport", sCfg.ice ? "ice" : "udp");
	kv("build_manifest_sha256", sBuildHexStr);
	kv("build_manifest", sBuildManifest);
	kv("config_sha256", sCfgHexStr);
	kv("config", sCfgText);
	kv("settings", pc_settings_netplay_config_text());
	kv("descriptor", pc_netplay_launch_descriptor(pc_coop_captain(0), pc_coop_captain(1), sCfg.mode,
	                                              sCfg.mode == xfer::kModeVs ? pc_vs_map_source_text() : std::string()));
	kv("netplay_seed", std::to_string(sCfg.seed));
	kv("delay_start", std::to_string(sCfg.localDelay));
	kv("first_frame", "0");
	kv("hash_tick_of_frame", "frame+1");
	kv("started_unix", std::to_string((long long)time(nullptr)));
	if (l.active) kv("run_dir", l.runDir);
	std::string err;
	if (!sInLog.open(path, m, &err)) {
		printf("[netplay] input log: %s\n", err.c_str());
		fflush(stdout);
		return;
	}
	sInLogFlushMs = now_ms();
	printf("[netplay] input log: %s (both inputs and this game's hash per frame; replay with "
	       "tools/netplay/replay_session.py)\n",
	       path.c_str());
	fflush(stdout);
}

void inlog_flush()
{
	if (sInLog.is_open()) sInLog.flush();
}

void inlog_end(const std::string& why)
{
	if (!sInLog.is_open()) return;
	pc_netplay_inlog::Event e;
	e.frame = sLastAdvanceFrame;
	e.kind  = pc_netplay_inlog::kEvEnd;
	e.data.assign(why.begin(), why.end());
	sInLog.append_event(e);
	sInLog.flush();
}

// After the frame's tick and hash: the record, and the periodic flush.
void inlog_frame(int frame, const uint8_t* inputs, uint64_t total, const uint64_t subs[7], bool haveHash)
{
	if (!sInLog.is_open()) return;
	pc_netplay_inlog::Frame f;
	f.frame = (uint32_t)frame;
	std::memcpy(f.in[0], inputs, pc_netplay_inlog::kInputBytes);
	std::memcpy(f.in[1], inputs + 16, pc_netplay_inlog::kInputBytes);
	f.haveTotal = haveHash;
	f.total     = total;
	f.haveSubs  = haveHash && (f.frame % pc_netplay_inlog::kSubsEvery) == 0;
	if (f.haveSubs)
		for (int i = 0; i < 7; ++i) f.subs[i] = subs[i];
	if (!sInLog.append_frame(f) && sInLog.capped() && !sInLogCapSaid) {
		sInLogCapSaid = true;
		printf("[netplay] input log: size cap reached at frame=%d; later frames are not recorded\n", frame);
		fflush(stdout);
	}
	const double now = now_ms();
	if (now - sInLogFlushMs >= 2000.0) {
		sInLog.flush();
		sInLogFlushMs = now;
	}
}

// ---- offline replay ----

bool run_advance(System* sys, BaseApp* app, int frame, const uint8_t* inputs, bool speculative);

// Runs from notify_argv, before the settings load: the recorded seed goes to
// the environment the deterministic reseed reads, and the recorded sim
// settings are adopted at settings init (pc_netplay_session_replay_settings).
void replay_load(const char* path)
{
	std::string err;
	if (!pc_netplay_inlog::load_file(path, &sReplay.log, &err)) {
		printf("[netplay] replay: cannot load %s: %s\n", path, err.c_str());
		fflush(stdout);
		session_exit(npx::kInputFile);
	}
	sReplay.path     = path;
	sReplay.settings = sReplay.log.meta_get("settings");
	if (getenv_nonempty("PIKMIN_NETPLAY_SEED") == nullptr) {
		const std::string seed = sReplay.log.meta_get("netplay_seed", "0");
#ifdef _WIN32
		_putenv_s("PIKMIN_NETPLAY_SEED", seed.c_str());
#else
		setenv("PIKMIN_NETPLAY_SEED", seed.c_str(), 1);
#endif
	}
}

void replay_start()
{
	pc_state_hash_set_netplay_capture(true);
	forensics_enable();
	sSessionStartMs                = now_ms();
	const pc_netplay_inlog::Log& L = sReplay.log;
	const std::string recBuild     = L.meta_get("build_manifest_sha256");
	const std::string recCfg       = L.meta_get("config_sha256");
	printf("[netplay] replay: %s: %llu frames (frame %u..%u), recorded by role=%s%s; replaying with no network\n",
	       sReplay.path.c_str(), (unsigned long long)L.frames.size(), L.frames.empty() ? 0u : L.frames.front().frame,
	       L.frames.empty() ? 0u : L.frames.back().frame, L.meta_get("role", "?").c_str(),
	       L.truncatedBytes != 0 ? " (the log ends in a cut record)" : "");
	printf("[netplay] replay: recorded build=%s %s\n", recBuild.c_str(),
	       recBuild == sBuildHexStr ? "(this build manifest)" : "(differs from this build manifest)");
	printf("[netplay] replay: recorded config=%s %s\n", recCfg.c_str(),
	       recCfg == sCfgHexStr ? "(same session config)" : "(the session config differs; the replay may diverge)");
	if (recCfg != sCfgHexStr)
		printf("[netplay] replay: recorded config text: %s\n[netplay] replay: this run's config text: %s\n",
		       L.meta_get("config").c_str(), sCfgText.c_str());
	fflush(stdout);
	sPhase = kSession;
}

void replay_finish()
{
	if (sReplay.finished) return;
	sReplay.finished = true;
	printf("[netplay] replay: done: %llu frames replayed, %llu hash-verified, %llu mismatch%s%s\n",
	       (unsigned long long)sReplay.next, (unsigned long long)sReplay.verified,
	       (unsigned long long)sReplay.mismatches, sReplay.mismatches == 1 ? "" : "es",
	       sReplay.mismatches == 0 ? " (every recorded hash reproduced)" : "");
	if (sReplay.haveBad)
		printf("[netplay] replay: first hash mismatch at frame=%u (tick %u)\n", sReplay.firstBadFrame,
		       sReplay.firstBadFrame + 1);
	fflush(stdout);
	pc_state_hash_flush();
	stop_session();
	sPhase = kDone;
	// Called from inside the tick loop with the audio and loader threads
	// alive, so leave without running static destructors (all output is
	// flushed above).
	std::fflush(nullptr);
	std::_Exit(sReplay.mismatches == 0 ? 0 : npx::kReplayMismatch);
}

void replay_verify(const pc_netplay_inlog::Frame& f, uint64_t total, const uint64_t subs[7])
{
	if (!f.haveTotal) return;
	++sReplay.verified;
	if (total == f.total) return;
	++sReplay.mismatches;
	if (!sReplay.haveBad) {
		sReplay.haveBad       = true;
		sReplay.firstBadFrame = f.frame;
	}
	if (sReplay.mismatches > 5) return;
	printf("[netplay] replay: hash mismatch at frame=%u (tick %u): recorded total=%016llx replay total=%016llx\n",
	       f.frame, f.frame + 1, (unsigned long long)f.total, (unsigned long long)total);
	if (f.haveSubs) {
		fx::TickSubs a, b;
		for (int i = 0; i < 7; ++i) {
			a.subs[i] = f.subs[i];
			b.subs[i] = subs[i];
		}
		printf("[netplay] replay:   differing sub-hashes: %s\n", fx::subs_mask_text(fx::differing_subs(a, b)).c_str());
	}
	fflush(stdout);
}

// One loop turn of the replay: a few logged frames, no pacing, no network.
bool replay_turn(System* sys, BaseApp* app)
{
	sys->mControllerMgr.update(); // keeps the window and audio pumping
	const std::vector<pc_netplay_inlog::Frame>& frames = sReplay.log.frames;
	int n = 0;
	while (n < 4 && sReplay.next < frames.size() && sPhase == kSession) {
		const pc_netplay_inlog::Frame& f = frames[sReplay.next];
		if (f.frame != (uint32_t)sAdvances) {
			printf("[netplay] replay: the log has no record for frame=%llu (next is frame=%u); a gap cannot be "
			       "replayed\n",
			       (unsigned long long)sAdvances, f.frame);
			fflush(stdout);
			sReplay.mismatches += 1;
			replay_finish();
			return true;
		}
		uint8_t inputs[32];
		std::memcpy(inputs, f.in[0], 16);
		std::memcpy(inputs + 16, f.in[1], 16);
		++sReplay.next;
		++n;
		const bool alive = run_advance(sys, app, (int)f.frame, inputs, false);
		uint64_t total = 0, subs[7] = { 0, 0, 0, 0, 0, 0, 0 }, tick = 0;
		if (pc_state_hash_current(&total, subs, &tick)) replay_verify(f, total, subs);
		if (!alive) {
			replay_finish();
			return true;
		}
	}
	if (sReplay.next >= frames.size() && sPhase == kSession) replay_finish();
	return true;
}

// ---- the desync exchange ----
//
// After a desync each peer sends two messages (pc_netplay_forensics.h): its
// sub-hash ring, then, once it holds the other's ring, the object hashes of
// the first differing tick. They travel in fragments on the handshake channel
// (unreliable): stable header, kHsFxFrag, kind u8, total u32, offset u32,
// bytes. Every fragment of a message is resent every 250 ms until the peer
// acknowledges the whole message (kHsFxAck, kind u8).
constexpr uint8_t kHsFxFrag    = 0x50;
constexpr uint8_t kHsFxAck     = 0x51;
constexpr size_t kFxHeader     = 16;
constexpr size_t kFxChunk      = 3584;
constexpr size_t kFxMaxBytes   = 1u << 22;
constexpr double kFxResendMs   = 250.0;
static_assert(kFxHeader + kFxChunk + 1 <= pc_netplay_transport::kMaxDatagram, "a fragment fits one datagram");

struct FxOut {
	uint8_t kind = 0;
	std::vector<uint8_t> data;
	bool acked        = false;
	double nextSendMs = 0;
};
struct FxIn {
	uint32_t total = 0;
	std::vector<uint8_t> data;
	std::vector<uint8_t> have;
	size_t count  = 0;
	bool complete = false;
};
std::vector<FxOut> sFxOut;
FxIn sFxIn[2]; // ring, objs

int fx_slot(uint8_t kind) { return kind == fx::kBulkDesyncRing ? 0 : kind == fx::kBulkDesyncObjs ? 1 : -1; }

void fx_header(uint8_t* p, uint8_t type)
{
	std::memcpy(p, xfer::kHsMagic, 4);
	p[4] = type;
	xfer::put_u16(p + 5, kProtocolVersion);
}

void fx_send_all(const FxOut& o)
{
	uint8_t msg[kFxHeader + kFxChunk];
	for (size_t off = 0; off < o.data.size(); off += kFxChunk) {
		const size_t len = std::min(kFxChunk, o.data.size() - off);
		fx_header(msg, kHsFxFrag);
		msg[7] = o.kind;
		xfer::put_u32(msg + 8, (uint32_t)o.data.size());
		xfer::put_u32(msg + 12, (uint32_t)off);
		std::memcpy(msg + kFxHeader, o.data.data() + off, len);
		hs_send(msg, kFxHeader + len);
	}
}

void fx_queue(uint8_t kind, std::vector<uint8_t> data)
{
	if (data.empty() || data.size() > kFxMaxBytes) return;
	FxOut o;
	o.kind = kind;
	o.data = std::move(data);
	sFxOut.push_back(std::move(o));
}

void fx_pump(double now)
{
	for (FxOut& o : sFxOut) {
		if (o.acked || now < o.nextSendMs) continue;
		fx_send_all(o);
		o.nextSendMs = now + kFxResendMs;
	}
	for (auto& g : hs_drain()) {
		uint8_t type = 0;
		uint16_t proto = 0;
		if (!xfer::decode_hello_header(g.payload.data(), g.payload.size(), &type, &proto)) continue;
		if (proto != kProtocolVersion) continue;
		if (sCfg.isHost && sHaveRemote && (g.fromIpHostOrder != sRemoteIp || g.fromPort != sRemotePort)) continue;
		const uint8_t* p = g.payload.data();
		const size_t len = g.payload.size();
		if (type == kHsFxAck && len >= 8) {
			for (FxOut& o : sFxOut)
				if (o.kind == p[7]) o.acked = true;
			continue;
		}
		if (type != kHsFxFrag || len < kFxHeader) continue;
		const int slot = fx_slot(p[7]);
		if (slot < 0) continue;
		const uint32_t total = xfer::get_u32(p + 8);
		const uint32_t off   = xfer::get_u32(p + 12);
		const size_t chunk   = len - kFxHeader;
		if (total == 0 || total > kFxMaxBytes || off >= total || off % kFxChunk != 0) continue;
		if (chunk != std::min((size_t)kFxChunk, (size_t)(total - off))) continue;
		FxIn& in = sFxIn[slot];
		if (in.total == 0) {
			in.total = total;
			in.data.assign(total, 0);
			in.have.assign((total + kFxChunk - 1) / kFxChunk, 0);
		}
		if (in.total != total) continue;
		const size_t idx = off / kFxChunk;
		if (!in.have[idx]) {
			in.have[idx] = 1;
			++in.count;
			std::memcpy(in.data.data() + off, p + kFxHeader, chunk);
			if (in.count == in.have.size()) in.complete = true;
		}
		if (in.complete) {
			uint8_t ack[8];
			fx_header(ack, kHsFxAck);
			ack[7] = p[7];
			hs_send(ack, sizeof(ack));
		}
	}
}

bool fx_all_acked()
{
	for (const FxOut& o : sFxOut)
		if (!o.acked) return false;
	return true;
}

// ---- desync report ----

struct DesyncLines {
	std::vector<std::string> lines;
	void add(const char* fmt, ...)
#if defined(__GNUC__)
	    __attribute__((format(printf, 2, 3)))
#endif
	    ;
};
void DesyncLines::add(const char* fmt, ...)
{
	char buf[1536];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	lines.emplace_back(buf);
}

std::vector<fx::TickSubs> desync_local_ring(uint64_t fromTick, bool all)
{
	std::vector<fx::TickSubs> out;
	uint64_t newest = 0, total = 0, subs[7] = { 0, 0, 0, 0, 0, 0, 0 };
	if (!pc_state_hash_current(&total, subs, &newest)) return out;
	uint64_t lo = all ? (newest > (uint64_t)kHashRing ? newest - (uint64_t)kHashRing + 1 : 1) : fromTick;
	if (lo < 1) lo = 1;
	for (uint64_t t = lo; t <= newest; ++t) {
		const HashEntry* e = hash_ring_find(t);
		if (e == nullptr) continue;
		fx::TickSubs s;
		s.tick  = e->tick;
		s.total = e->total;
		for (int i = 0; i < 7; ++i) s.subs[i] = e->subs[i];
		s.xtra = e->xtra;
		out.push_back(s);
	}
	return out;
}

std::string subs_line(const char* who, const fx::TickSubs& t)
{
	char buf[512];
	snprintf(buf, sizeof(buf),
	         "desync subs %s tick=%llu: total=%016llx navi=%016llx piki=%016llx teki=%016llx item=%016llx "
	         "world=%016llx rng=%016llx mode=%016llx xtra=%016llx",
	         who, (unsigned long long)t.tick, (unsigned long long)t.total, (unsigned long long)t.subs[0],
	         (unsigned long long)t.subs[1], (unsigned long long)t.subs[2], (unsigned long long)t.subs[3],
	         (unsigned long long)t.subs[4], (unsigned long long)t.subs[5], (unsigned long long)t.subs[6],
	         (unsigned long long)t.xtra);
	return buf;
}

bool write_text_file(const std::string& path, const std::vector<std::string>& lines)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (f == nullptr) return false;
	for (const std::string& l : lines) std::fprintf(f, "%s\n", l.c_str());
	return std::fclose(f) == 0;
}

// Runs before stop_session (GekkoNet and the transport are still up): swap
// the sub-hash rings, then the per-object hashes of the first differing tick,
// print both sides and write the files. At least 1 s, so GekkoNet's own
// checksum reaches the other game, and at most kDesyncExchangeMs.
constexpr double kDesyncExchangeMs = 4000.0;
void desync_forensics(int frame, uint32_t localCk, uint32_t remoteCk)
{
	const uint64_t wantTick = frame >= 0 ? (uint64_t)frame + 1 : 0;
	const char* myName      = sCfg.isHost ? "host" : "join";
	const char* peerName    = sCfg.isHost ? "join" : "host";

	fx::RingMsg mine;
	mine.role           = sCfg.isHost ? 0 : 1;
	mine.frame          = (uint32_t)frame;
	mine.localChecksum  = localCk;
	mine.remoteChecksum = remoteCk;
	mine.ticks          = desync_local_ring(wantTick > 150 ? wantTick - 150 : 1, false);
	if (mine.ticks.size() > 400) mine.ticks.erase(mine.ticks.begin(), mine.ticks.end() - 400);
	const bool canSend = !mine.ticks.empty();
	if (canSend) fx_queue(fx::kBulkDesyncRing, fx::encode_ring(mine));

	fx::RingMsg peerRing;
	fx::ObjsMsg peerObjs;
	bool haveRing = false, haveObjs = false, sentObjs = false;
	std::vector<uint64_t> objTicks;
	uint64_t diffTick = 0, xtraTick = 0;
	bool haveDiff = false, haveXtra = false;
	const double t0 = now_ms();
	while (sGekko != nullptr && now_ms() - t0 < kDesyncExchangeMs) {
		gekko_network_poll(sGekko);
		fx_pump(now_ms());
		if (!haveRing && sFxIn[0].complete) {
			haveRing = fx::decode_ring(sFxIn[0].data.data(), sFxIn[0].data.size(), &peerRing);
			if (!haveRing) sFxIn[0] = FxIn();
		}
		if (haveRing && !sentObjs && canSend) {
			haveDiff = fx::first_diff_total(mine.ticks, peerRing.ticks, &diffTick);
			haveXtra = fx::first_diff_xtra(mine.ticks, peerRing.ticks, &xtraTick);
			const uint64_t first = haveDiff ? diffTick : wantTick;
			objTicks.push_back(first);
			if (haveXtra && xtraTick != first) objTicks.push_back(xtraTick);
			fx::ObjsMsg om;
			om.role = mine.role;
			for (uint64_t t : objTicks) {
				const std::vector<fx::ObjRec>* recs = pc_state_dump_find(t);
				if (recs == nullptr) continue;
				fx::ObjTick ot;
				ot.tick = t;
				for (const fx::ObjRec& r : *recs) ot.keys.push_back(fx::key_of(r));
				om.ticks.push_back(std::move(ot));
			}
			fx_queue(fx::kBulkDesyncObjs, fx::encode_objs(om));
			sentObjs = true;
		}
		if (!haveObjs && sFxIn[1].complete) {
			haveObjs = fx::decode_objs(sFxIn[1].data.data(), sFxIn[1].data.size(), &peerObjs);
			if (!haveObjs) sFxIn[1] = FxIn();
		}
		const double el = now_ms() - t0;
		if (!canSend && el >= 1000.0) break;
		if (haveRing && haveObjs && sentObjs && fx_all_acked() && el >= 1000.0) break;
		sleep_hires_ms(2.0, 0.0);
	}

	DesyncLines R;
	const fx::TickSubs* myAt = fx::find_tick(mine.ticks, wantTick);
	R.add("desync report: frame=%d this=%s peer=%s local-checksum=%08x remote-checksum=%08x", frame, myName, peerName,
	      localCk, remoteCk);
	if (myAt != nullptr) R.lines.push_back(subs_line(myName, *myAt));
	else R.add("desync subs %s tick=%llu: not in this game's hash ring", myName, (unsigned long long)wantTick);
	uint64_t reportTick = wantTick;
	if (haveRing) {
		const fx::TickSubs* peerAt = fx::find_tick(peerRing.ticks, wantTick);
		if (peerAt != nullptr) R.lines.push_back(subs_line(peerName, *peerAt));
		else R.add("desync subs %s tick=%llu: not in the other game's ring", peerName, (unsigned long long)wantTick);
		if (myAt != nullptr && peerAt != nullptr)
			R.add("desync differing sub-hashes at tick=%llu: %s", (unsigned long long)wantTick,
			      fx::subs_mask_text(fx::differing_subs(*myAt, *peerAt)).c_str());
		if (peerRing.frame != (uint32_t)frame)
			R.add("desync note: the other game reported frame=%u (this game: frame=%d)", peerRing.frame, frame);
		if (haveDiff) {
			const fx::TickSubs* a = fx::find_tick(mine.ticks, diffTick);
			const fx::TickSubs* b = fx::find_tick(peerRing.ticks, diffTick);
			R.add("desync first differing tick in the exchanged window (%llu ticks, %llu..%llu): tick=%llu, "
			      "differing sub-hashes: %s",
			      (unsigned long long)mine.ticks.size(),
			      (unsigned long long)(mine.ticks.empty() ? 0 : mine.ticks.front().tick),
			      (unsigned long long)(mine.ticks.empty() ? 0 : mine.ticks.back().tick), (unsigned long long)diffTick,
			      a != nullptr && b != nullptr ? fx::subs_mask_text(fx::differing_subs(*a, *b)).c_str() : "?");
			reportTick = diffTick;
		} else {
			R.add("desync no tick in the exchanged window (%llu ticks) has different totals",
			      (unsigned long long)mine.ticks.size());
		}
		if (haveXtra)
			R.add("desync first differing xtra hash (state the seven columns do not cover): tick=%llu%s",
			      (unsigned long long)xtraTick,
			      haveDiff && xtraTick < diffTick ? " (earlier than the visible divergence)" : "");
		else R.add("desync xtra hash: identical on both sides over the window");
		for (uint64_t t : objTicks) {
			const std::vector<fx::ObjRec>* recs = pc_state_dump_find(t);
			const fx::ObjTick* pt               = nullptr;
			for (const fx::ObjTick& ot : peerObjs.ticks)
				if (ot.tick == t) pt = &ot;
			if (recs == nullptr || pt == nullptr) {
				R.add("desync objects at tick=%llu: %s", (unsigned long long)t,
				      recs == nullptr ? "this game no longer has them in its object ring"
				                      : "the other game did not send them");
				continue;
			}
			std::vector<fx::ObjKey> lk;
			for (const fx::ObjRec& r : *recs) lk.push_back(fx::key_of(r));
			const fx::ObjDiff d = fx::diff_objs(lk, pt->keys);
			R.add("desync objects at tick=%llu: %llu differ, %llu only on this game, %llu only on the other%s",
			      (unsigned long long)t, (unsigned long long)d.changed.size(), (unsigned long long)d.onlyLocal.size(),
			      (unsigned long long)d.onlyRemote.size(), d.countMismatch ? " (object counts differ)" : "");
			size_t shown = 0;
			for (const fx::ObjDiff::Changed& c : d.changed) {
				if (++shown > 24) {
					R.add("desync   ... %llu more", (unsigned long long)(d.changed.size() - 24));
					break;
				}
				R.add("desync   differs: %s=%s%s%s | %s=%s", myName, fx::format_key(c.local).c_str(),
				      c.local.type != c.remote.type ? " TYPE-DIFFERS" : "",
				      c.local.hash == c.remote.hash ? " (xhash only)" : "", peerName, fx::format_key(c.remote).c_str());
				for (const fx::ObjRec& r : *recs)
					if (r.kind == c.local.kind && r.ord == c.local.ord)
						R.add("desync     %s: %s", myName, fx::format_obj(r).c_str());
			}
			shown = 0;
			for (const fx::ObjKey& k : d.onlyLocal) {
				if (++shown > 12) break;
				R.add("desync   only on %s: %s", myName, fx::format_key(k).c_str());
			}
			shown = 0;
			for (const fx::ObjKey& k : d.onlyRemote) {
				if (++shown > 12) break;
				R.add("desync   only on %s: %s", peerName, fx::format_key(k).c_str());
			}
		}
	} else {
		R.add("desync report: the other game's sub-hashes did not arrive within %.0f s; only this game's side is "
		      "printed. Send both run folders.",
		      (now_ms() - t0) / 1000.0);
	}
	for (const std::string& l : R.lines) printf("[netplay] %s\n", l.c_str());

	{
		std::vector<std::string> subsFile;
		subsFile.push_back(std::string("# netplay desync sub-hash ring (this game=") + myName + ", desync frame "
		                   + std::to_string(frame) + ", hash tick = frame+1)");
		subsFile.push_back(fx::subs_header());
		subsFile.push_back(std::string("# ") + myName + " (this game), every tick in the ring");
		for (const fx::TickSubs& t : desync_local_ring(0, true)) subsFile.push_back(fx::format_subs(t));
		if (haveRing) {
			subsFile.push_back(std::string("# ") + peerName + " (received from the other game)");
			for (const fx::TickSubs& t : peerRing.ticks) {
				const fx::TickSubs* a = fx::find_tick(mine.ticks, t.tick);
				std::string l         = fx::format_subs(t);
				if (a != nullptr && a->total != t.total) l += "  <- total differs from this game's";
				else if (a != nullptr && a->xtra != t.xtra) l += "  <- only xtra differs";
				subsFile.push_back(l);
			}
		}
		write_text_file(forensics_path("desync-subs.txt"), subsFile);
	}
	{
		std::vector<uint64_t> ticks;
		auto addTick = [&](uint64_t t) {
			if (t == 0) return;
			for (uint64_t x : ticks)
				if (x == t) return;
			ticks.push_back(t);
		};
		if (reportTick > 1) addTick(reportTick - 1);
		addTick(reportTick);
		if (haveXtra) addTick(xtraTick);
		addTick(wantTick);
		std::sort(ticks.begin(), ticks.end());
		FILE* f = std::fopen(forensics_path("desync-objects.txt").c_str(), "wb");
		if (f != nullptr) {
			std::fprintf(f,
			             "# netplay desync state dump (this game=%s, desync frame %d). One section per tick; one line\n"
			             "# per object, in manager order: kind ord type state hp pos rot vel drv face aux hash xhash.\n"
			             "# Diff against the other game's file with tools/netplay/diff_desync.py.\n",
			             myName, frame);
			for (uint64_t t : ticks)
				if (!pc_state_dump_write_tick(f, t))
					std::fprintf(f, "# tick %llu: no longer in the object ring (%llu ticks are kept)\n",
					             (unsigned long long)t, (unsigned long long)kStateDumpRingTicks);
			std::fclose(f);
		}
		if (haveRing) {
			std::vector<std::string> pk;
			pk.push_back(std::string("# object hashes the other game (") + peerName + ") sent for the differing tick(s)");
			for (const fx::ObjTick& ot : peerObjs.ticks) {
				pk.push_back("# tick " + std::to_string((unsigned long long)ot.tick));
				for (const fx::ObjKey& k : ot.keys) pk.push_back(fx::format_key(k));
			}
			write_text_file(forensics_path("desync-peer-objects.txt"), pk);
		}
	}
	std::vector<std::string> rep = R.lines;
	rep.push_back(std::string("files: ") + forensics_dir() + " (desync-report.txt desync-subs.txt desync-objects.txt"
	              + (haveRing ? " desync-peer-objects.txt" : "") + (sInLog.is_open() ? " session-inputs.pknl" : "")
	              + ")");
	write_text_file(forensics_path("desync-report.txt"), rep);
	printf("[netplay] desync forensics written to %s: desync-report.txt, desync-subs.txt, desync-objects.txt%s%s\n",
	       forensics_dir().c_str(), haveRing ? ", desync-peer-objects.txt" : "",
	       sInLog.is_open() ? ", session-inputs.pknl (the replayable input log)" : "");
	fflush(stdout);
	inlog_flush();
}

// ---- ICE code exchange ----

// Prints the code (one line after its tag, which the pair tools grep for),
// copies it to the clipboard unless PIKMIN_NETPLAY_NO_CLIPBOARD=1, writes it
// atomically to --netplay-code-out and keeps a copy in the run folder.
void ice_emit_code(const char* kind, const std::string& code)
{
	const PcNetplayLaunch& L = pc_netplay_launch_setup();
	printf("[netplay] ice %s code:\n%s\n", kind, code.c_str());
	if (env_is_one("PIKMIN_NETPLAY_NO_CLIPBOARD")) {
		printf("[netplay] launch: %s code not copied to the clipboard (PIKMIN_NETPLAY_NO_CLIPBOARD)\n", kind);
	} else if (SDL_SetClipboardText(code.c_str()) == 0) {
		printf("[netplay] launch: %s code copied to the clipboard (%llu chars)\n", kind,
		       (unsigned long long)code.size());
		// Android has no console: say it over the (still black) game.
		if (std::strcmp(kind, "answer") == 0 && !argv_has(sArgc, sArgv, "--netplay-dialogs"))
			pc_online_android_toast("Answer code copied. Send it to your friend; the game starts when they paste it.");
	} else {
		printf("[netplay] launch: clipboard copy failed (%s); copy the code above\n", SDL_GetError());
	}
	std::string err;
	if (!L.codeOut.empty()) {
		if (pc_netplay_ice::ice_write_code_file(L.codeOut, code, &err))
			printf("[netplay] launch: %s code written to %s\n", kind, L.codeOut.c_str());
		else
			printf("[netplay] launch: %s\n", err.c_str());
	}
	if (!L.runDir.empty()) pc_netplay_ice::ice_write_code_file(L.runDir + "/" + kind + ".txt", code, &err);
	fflush(stdout);
}

[[noreturn]] void ice_dialog_cancel();
int ice_dialog(const char* title, const std::string& text, std::initializer_list<const char*> buttons);

#if defined(__linux__) && !defined(__ANDROID__)
// ufw on and blocking: its config is world-readable (no root needed). A friend
// whose carrier changes the source port can only come in through a rule.
bool linux_ufw_enabled()
{
	FILE* f = std::fopen("/etc/ufw/ufw.conf", "r");
	if (f == nullptr) return false;
	char line[256];
	bool on = false;
	while (std::fgets(line, sizeof line, f) != nullptr)
		if (std::strncmp(line, "ENABLED=yes", 11) == 0) on = true;
	std::fclose(f);
	return on;
}
#endif

[[noreturn]] void ice_fail(const std::string& err)
{
	printf("[netplay] ice: %s\n", err.c_str());
	fflush(stdout);
	// From the title's Online menu: say why in a dialog and go back to the
	// game in normal mode instead of closing it.
	if (argv_has(sArgc, sArgv, "--netplay-dialogs")) {
		const bool stun = pc_settings_get_online_stun() != 0;
		ice_dialog("Online - could not connect",
		           std::string("The connection with your friend could not be made.\n\n")
		               + (stun ? "If you are on different networks, one of them may block direct connections (this is "
		                         "common when both use mobile data). Try with the other player creating the room, or "
		                         "from the same Wi-Fi. Turning on UPnP in the router of whoever creates the room also "
		                         "helps."
		                       : "F1 > Data > Online over the Internet is off, so only players on the same network (or "
		                         "a VPN) can connect. Turn it on to play with a friend elsewhere.")
#if defined(__linux__) && !defined(__ANDROID__)
		               + (linux_ufw_enabled() ? "\n\nThis PC's firewall (ufw) is on and may be blocking your friend. Run "
		                                        "this once in a terminal, then try again:\n  sudo ufw allow 39911:39919/udp"
		                                      : "")
#endif
		               + "\n\n(" + err + ")",
		           { "OK" });
		ice_dialog_cancel();
	}
	stop_session();
	session_exit(npx::kTransport);
}

// --netplay-dialogs (the title's Online menu): the code exchange in system
// dialogs instead of the console. SDL message boxes are native windows on
// the desktop and AlertDialogs on Android, so they work before the game has
// drawn anything. Returns the pressed button's index, -1 if none.
int ice_dialog(const char* title, const std::string& text, std::initializer_list<const char*> buttons)
{
	std::vector<SDL_MessageBoxButtonData> data;
	int id = 0;
	for (const char* label : buttons) {
		SDL_MessageBoxButtonData b{};
		b.buttonid = id;
		b.text     = label;
		if (id == 0) b.flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
		data.push_back(b);
		++id;
	}
	SDL_MessageBoxData box{};
	box.flags      = SDL_MESSAGEBOX_INFORMATION;
	box.window     = SDL_GL_GetCurrentWindow();
	box.title      = title;
	box.message    = text.c_str();
	box.numbuttons = (int)data.size();
	box.buttons    = data.data();
	int pressed    = -1;
	if (SDL_ShowMessageBox(&box, &pressed) != 0) {
		printf("[netplay] dialog failed: %s\n", SDL_GetError());
		fflush(stdout);
		return -1;
	}
	return pressed;
}

// The clipboard's text without spaces or line breaks.
std::string clipboard_code_text()
{
	char* text = SDL_GetClipboardText();
	std::string out;
	for (const char* c = text; c != nullptr && *c != '\0'; ++c)
		if (*c != ' ' && *c != '\n' && *c != '\r' && *c != '\t') out += *c;
	SDL_free(text);
	return out;
}

#if defined(__linux__) && !defined(__ANDROID__)
// Linux desktops: the message box is another process (zenity/kdialog) that
// holds the focus, and Wayland only lets the focused app read the clipboard,
// so the answer is pasted into that tool's own text field instead. Empty when
// neither tool is there or the player cancels.
// With `prefill` (a code: [A-Za-z0-9_-] only) the field shows it for the
// player to copy, since the game cannot set the clipboard without the focus.
std::string linux_entry_dialog(const char* title, const char* text, const std::string& prefill = std::string())
{
	const std::string zenity = std::string("zenity --entry --width=560 --title='") + title + "' --text='" + text
	                         + "' --entry-text='" + prefill + "' 2>/dev/null";
	const std::string kdialog = std::string("kdialog --title '") + title + "' --inputbox '" + text + "' '" + prefill
	                          + "' 2>/dev/null";
	for (const std::string& cmd : { zenity, kdialog }) {
		const std::string tool = cmd.substr(0, cmd.find(' '));
		if (std::system(("command -v " + tool + " >/dev/null 2>&1").c_str()) != 0) continue;
		FILE* p = popen(cmd.c_str(), "r");
		if (p == nullptr) continue;
		std::string out;
		char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, p)) > 0 && out.size() < 65536) out.append(buf, n);
		pclose(p);
		std::string code;
		for (char c : out)
			if (c != ' ' && c != '\n' && c != '\r' && c != '\t') code += c;
		return code;
	}
	return std::string();
}
#endif

// Cancelled from a dialog: back to the game in normal mode.
[[noreturn]] void ice_dialog_cancel()
{
	printf("[netplay] ice: cancelled from the dialog\n");
	stop_session();
	std::string err;
	if (pc_online_relaunch({}, &err)) {
#if defined(__ANDROID__)
		for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
#else
		std::_Exit(0);
#endif
	}
	session_exit(0);
}

#if PIKI_NETPLAY_PORTMAP
// Port mapping on this side's router (PCP, NAT-PMP or UPnP, nectar_net): a
// friend whose carrier NAT changes the source port per destination (mobile
// data) cannot use the STUN address, but a mapped port takes packets from
// anyone. The mapped address joins the code as one more candidate.
std::unique_ptr<nectar::net::PortPublisher> sPortmap;

void portmap_withdraw()
{
	if (sPortmap) sPortmap->withdraw();
}

void portmap_add_candidate(std::string& sdp, const std::function<void()>& pump)
{
	// The agent's port: the first IPv4 host candidate.
	uint32_t port = 0;
	for (size_t at = sdp.find("a=candidate:"); at != std::string::npos; at = sdp.find("a=candidate:", at + 1)) {
		const size_t eol        = sdp.find('\n', at);
		const std::string line  = sdp.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
		char ip[64]             = {};
		unsigned p              = 0;
		char type[16]           = {};
		if (std::sscanf(line.c_str(), "a=candidate:%*s %*s %*s %*s %63s %u typ %15s", ip, &p, type) == 3
		    && std::strcmp(type, "host") == 0 && std::strchr(ip, ':') == nullptr && p > 0 && p < 65536) {
			port = p;
			break;
		}
	}
	if (port == 0) return;
	if (!sPortmap) {
		// Routers answer PCP/NAT-PMP within milliseconds and SSDP within a few
		// hundred on the LAN; one that answers nothing would hold the code
		// ~12 s with the default schedule (miniupnpc waits the discovery time
		// once per device type). 150+300+600 ms and 600 ms of discovery.
		nectar::net::PublisherBackends backends = nectar::net::PublisherBackends::system();
		backends.retry.initialTimeoutMs        = 150;
		backends.retry.attempts                = 3;
		backends.upnpDiscoveryMs               = 600;
		sPortmap = std::make_unique<nectar::net::PortPublisher>(backends);
		std::atexit(portmap_withdraw);
	}
	printf("[netplay] port mapping: asking the router to open UDP %u\n", (unsigned)port);
	fflush(stdout);
	// Blocking (router discovery takes a few seconds when nothing answers):
	// a worker thread, the window kept alive meanwhile.
	std::atomic<bool> done{ false };
	nectar::net::PublishResult result;
	std::thread worker([&]() {
		result = sPortmap->publish((uint16_t)port, 7200, false);
		done   = true;
	});
	while (!done) {
		if (pump) pump();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	worker.join();
	if (result.reachability != nectar::net::Reachability::Public || result.mapping.externalPort == 0) {
		printf("[netplay] port mapping: the router opened no port (%s; pcp=%s nat-pmp=%s upnp=%s)\n",
		       nectar::net::reachabilityString(result.reachability), nectar::net::mappingErrorString(result.pcpError),
		       nectar::net::mappingErrorString(result.natPmpError), nectar::net::mappingErrorString(result.upnpError));
		fflush(stdout);
		return;
	}
	const auto& a          = result.mapping.externalAddress;
	const std::string addr = std::to_string(a[0]) + "." + std::to_string(a[1]) + "." + std::to_string(a[2]) + "."
	                       + std::to_string(a[3]) + " " + std::to_string(result.mapping.externalPort);
	if (sdp.find(" " + addr + " typ ") != std::string::npos) {
		printf("[netplay] port mapping: %s opened the port (already the STUN address)\n",
		       nectar::net::mappingMethodString(result.method));
		fflush(stdout);
		return;
	}
	const std::string line = "a=candidate:9 1 UDP 1694498815 " + addr + " typ srflx raddr 0.0.0.0 rport 0\r\n";
	const size_t end       = sdp.find("a=end-of-candidates");
	sdp.insert(end == std::string::npos ? sdp.size() : end, line);
	printf("[netplay] port mapping: %s opened the port; its address is in the code\n",
	       nectar::net::mappingMethodString(result.method));
	fflush(stdout);
}
#endif

// The blocking code exchange on the first turn: the host gathers and prints
// its offer (the session bundle: seed, sim settings, captains), waits for the
// answer (--netplay-answer-in, else the console); the joiner gathers and
// prints its answer. Then both wait for ICE to connect. The window keeps
// pumping; closing it ends the game.
void ice_connect(System* sys)
{
	const PcNetplayLaunch& L = pc_netplay_launch_setup();
	// F1 > Data > Online over the Internet: public STUN, unless the
	// environment already chose (PIKMIN_NETPLAY_STUN, tools and tests).
	if (getenv_nonempty("PIKMIN_NETPLAY_STUN") == nullptr && pc_settings_get_online_stun()) {
#ifdef _WIN32
		_putenv_s("PIKMIN_NETPLAY_STUN", "public");
#else
		setenv("PIKMIN_NETPLAY_STUN", "public", 1);
#endif
	}
	// From the title's Online menu the agent takes a port in a small fixed
	// range, so one firewall rule or router forward (UDP 39911-39919) lets a
	// friend's packets in from any port (carrier NATs change the source port
	// per destination). The environment still wins (tools and tests).
	if (argv_has(sArgc, sArgv, "--netplay-dialogs") && getenv_nonempty("PIKMIN_NETPLAY_ICE_PORT_BEGIN") == nullptr) {
#ifdef _WIN32
		_putenv_s("PIKMIN_NETPLAY_ICE_PORT_BEGIN", "39911");
		_putenv_s("PIKMIN_NETPLAY_ICE_PORT_END", "39919");
#else
		setenv("PIKMIN_NETPLAY_ICE_PORT_BEGIN", "39911", 1);
		setenv("PIKMIN_NETPLAY_ICE_PORT_END", "39919", 1);
#endif
	}
	const pc_netplay_ice::IceNetConfig nic = pc_netplay_ice::ice_net_config_from_env();
	printf("[netplay] ice: %s\n", pc_netplay_ice::ice_net_config_text(nic).c_str());
	fflush(stdout);
	std::function<void()> pump = [sys]() {
		sys->mControllerMgr.update();
		if (pc_window_should_close()) {
			printf("[netplay] ice: the window was closed during the code exchange\n");
			std::fflush(nullptr);
			std::_Exit(0);
		}
	};
	sIce = new pc_netplay_ice::IceSocket();
#if PIKI_NETPLAY_PORTMAP
	// Goes with public STUN (F1 > Data > Online over the Internet, or
	// PIKMIN_NETPLAY_STUN=public); tools and tests that pick none skip it.
	const char* stunMode = getenv_nonempty("PIKMIN_NETPLAY_STUN");
	if (stunMode != nullptr && std::strcmp(stunMode, "public") == 0)
		sIce->set_local_sdp_hook([pump](std::string& sdp) { portmap_add_candidate(sdp, pump); });
#endif
	std::string err;
	double completedMs = -1;
	if (sCfg.isHost) {
		pc_netplay_ice::SessionBundle bundle;
		bundle.seed       = sCfg.seed;
		bundle.configText = pc_settings_netplay_config_text();
		// VS: the built-in arena (no map selected, no --vs-map file) is in
		// every build, and the joiner adopts the host's map setting with the
		// sim settings, so only another map travels, minified: whitespace
		// outside strings is most of a hand-written map.json.
		const bool builtInArena = sCfg.mode == xfer::kModeVs && !sCfg.vsMapFile && pc_vs_map_selected().empty();
		std::string vsMapJson;
		if (sCfg.mode == xfer::kModeVs && !builtInArena) {
			bool inString = false, escaped = false;
			for (char c : pc_vs_map_source_text()) {
				if (inString) {
					if (escaped) escaped = false;
					else if (c == '\\') escaped = true;
					else if (c == '"') inString = false;
				} else if (c == '"') {
					inString = true;
				} else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
					continue;
				}
				vsMapJson += c;
			}
		}
		bundle.descriptor = pc_netplay_launch_descriptor(pc_coop_captain(0), pc_coop_captain(1), sCfg.mode, vsMapJson);
		// Co-op with the default captains: the joiner assumes them when the
		// descriptor is empty, so the code leaves it out.
		if (bundle.descriptor == pc_netplay_launch_descriptor(0, 1, xfer::kModeCoopDirect, std::string()))
			bundle.descriptor.clear();
		if (builtInArena) {
			printf("[netplay] vs map: the built-in arena (not in the offer: every build has it)\n");
		} else if (sCfg.mode == xfer::kModeVs) {
			const size_t json = vsMapJson.size();
			if (json > kPcNetplayVsMapJsonMax)
				printf("[netplay] vs map: map.json is %llu B, more than the offer carries (%llu B); the joiner needs "
				       "the same map\n",
				       (unsigned long long)json, (unsigned long long)kPcNetplayVsMapJsonMax);
			else
				printf("[netplay] vs map: the offer carries this map.json (%llu B)\n", (unsigned long long)json);
		}
		std::string offer, answer;
		if (!sIce->host_create_offer(nic, bundle, &offer, &err, pump)) ice_fail(err);
		ice_emit_code("offer", offer);
		bool got = false;
		if (argv_has(sArgc, sArgv, "--netplay-dialogs")) {
			std::string note;
#if defined(__linux__) && !defined(__ANDROID__)
			linux_entry_dialog("Online - your room code",
			                   "Your room code: copy it (Ctrl+A, Ctrl+C) and send it to your friend, then OK.", offer);
#endif
			for (;;) {
				const std::string text =
				    std::string(sCfg.mode == xfer::kModeVs ? "VS" : "Co-op")
				    + " room open.\n\nYour room code (" + offer.substr(0, 16) + "..., " + std::to_string(offer.size())
#if defined(__linux__) && !defined(__ANDROID__)
				    + " characters) goes to your friend, who joins from "
#else
				    + " characters) is in the clipboard: send it to your friend, who joins from "
#endif
				    + (sCfg.mode == xfer::kModeVs ? "VS" : "Co-op") + " > Online > Join with a code.\n\nTheir game then "
				    "gives them an answer code (NPIX3-...). When they send it to you, copy it and press Paste answer."
				    + note;
				const int pressed = ice_dialog("Online - waiting for your friend", text,
				                               { "Paste answer", "Copy my code again", "Cancel" });
				if (pressed == 1) {
#if defined(__linux__) && !defined(__ANDROID__)
					linux_entry_dialog("Online - your room code",
					                   "Your room code: copy it (Ctrl+A, Ctrl+C) and send it to your friend, then OK.", offer);
					note.clear();
#else
					SDL_SetClipboardText(offer.c_str());
					note = "\n\nYour code was copied again.";
#endif
					continue;
				}
				if (pressed != 0) ice_dialog_cancel();
#if defined(__linux__) && !defined(__ANDROID__)
				answer = linux_entry_dialog("Online - your friend's answer",
				                            "Paste your friend's answer code (Ctrl+V), then OK:");
				if (answer.empty()) answer = clipboard_code_text();
#else
				answer = clipboard_code_text();
#endif
				std::string sdp, why;
				if (!pc_netplay_ice::ice_decode_any_answer(answer, &sdp, &why)) {
					note = "\n\nThat is not your friend's answer code (" + why + "). Copy their whole code and try again.";
					continue;
				}
				got = true;
				break;
			}
		} else if (!L.answerIn.empty()) {
			printf("[netplay] launch: waiting for the answer code in %s (up to %.0f min)\n", L.answerIn.c_str(),
			       pc_netplay_ice::ice_connect_timeout_ms() / 60000.0);
			fflush(stdout);
			got = pc_netplay_ice::ice_poll_answer_file(L.answerIn, pc_netplay_ice::ice_connect_timeout_ms(), pump,
			                                           &answer, &err);
		} else {
			got = pc_netplay_ice::ice_read_answer_stdin(pump, &answer, &err);
		}
		if (!got) ice_fail(err);
		if (!sIce->host_apply_answer(answer, &err)) ice_fail(err);
	} else {
		std::string answer;
		if (!sIce->join_create_answer(nic, L.offerCode, &answer, &err, pump, nullptr)) ice_fail(err);
		ice_emit_code("answer", answer);
		if (argv_has(sArgc, sArgv, "--netplay-dialogs")) {
			std::string note;
#if defined(__linux__) && !defined(__ANDROID__)
			// The code in a text field to copy; no second dialog.
			linux_entry_dialog("Online - your answer code",
			                   "Your answer code: copy it (Ctrl+A, Ctrl+C) and send it back to your friend. The game "
			                   "starts once they paste it.",
			                   answer);
			while (false) {
#else
			for (;;) {
#endif
				const int pressed = ice_dialog(
				    "Online - your answer code",
				    "Your answer code is in the clipboard (" + answer.substr(0, 16) + "...). Send it back to your friend, "
				        "who pastes it with Paste answer.\n\nThe game starts by itself once they do." + note,
				    { "OK", "Copy again" });
				if (pressed != 1) break;
				SDL_SetClipboardText(answer.c_str());
				note = "\n\nCopied again.";
			}
		}
	}
	if (!sIce->wait_connected(pc_netplay_ice::ice_connect_timeout_ms(), &completedMs, &err, pump)) ice_fail(err);
	printf("[netplay] ice transport ready (connect %.0fms) local [%s] remote [%s]\n", completedMs,
	       sIce->selected_local().c_str(), sIce->selected_remote().c_str());
	fflush(stdout);
	sIceLink = new pc_netplay_ice::IceLink(sIce);
	// The handshake learns the peer from its first datagram, which over ICE
	// carries the placeholder address.
}

// ---- the GekkoNet session ----

void start_gekko_session()
{
	sGekko = nullptr;
	if (!gekko_create(&sGekko, GekkoGameSession) || sGekko == nullptr) {
		printf("[netplay] gekko_create failed\n");
		fflush(stdout);
		session_exit(npx::kTransport);
	}
	// The lossy test wrapper only when impairments are configured.
	pc_netplay_transport::LossyParams lp;
	lp.latencyMs      = read_double_env("PIKMIN_NETPLAY_TEST_LATENCY_MS", 0.0);
	lp.jitterMs       = read_double_env("PIKMIN_NETPLAY_TEST_JITTER_MS", 0.0);
	lp.lossPct        = read_double_env("PIKMIN_NETPLAY_TEST_LOSS_PCT", 0.0);
	lp.reorderPct     = read_double_env("PIKMIN_NETPLAY_TEST_REORDER_PCT", 0.0);
	lp.reorderExtraMs = read_double_env("PIKMIN_NETPLAY_TEST_REORDER_MS", 0.0);
	lp.seed           = read_u32_env("PIKMIN_NETPLAY_TEST_SEED", 0);
	if (lp.latencyMs < 0) lp.latencyMs = 0;
	if (lp.jitterMs < 0) lp.jitterMs = 0;
	if (lp.lossPct < 0) lp.lossPct = 0;
	if (lp.lossPct > 100) lp.lossPct = 100;
	const bool lossy = lp.latencyMs > 0 || lp.jitterMs > 0 || lp.lossPct > 0 || (lp.reorderPct > 0 && lp.reorderExtraMs > 0);
	GekkoNetAdapter* base = sIceLink != nullptr ? sIceLink->adapter() : sLink->adapter();
	if (lossy) {
		sLossy   = new pc_netplay_transport::LossyLink(base, lp);
		sAdapter = sLossy->adapter();
		printf("[netplay] lossy adapter: latency=%.1fms jitter=%.1fms loss=%.1f%% seed=%u\n", lp.latencyMs, lp.jitterMs,
		       lp.lossPct, lp.seed);
	} else {
		sAdapter = base;
	}
	GekkoConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.num_players             = 2;
	cfg.max_spectators          = 0;
	cfg.input_prediction_window = 0; // lockstep
	cfg.spectator_delay         = 0;
	cfg.input_size              = 16;
	cfg.state_size              = 8; // 8-byte handle {frame}
	cfg.limited_saving          = false;
	cfg.desync_detection        = true;
	cfg.check_distance          = 7;
	gekko_start(sGekko, &cfg);
	// The adapter must be set after gekko_start: Init() clears it.
	gekko_net_adapter_set(sGekko, sAdapter);
	// Role-ordered actors, so handle 0 is the host/P1 and handle 1 the
	// joiner/P2 on both peers.
	// Over ICE the agent is connected 1:1 and every datagram carries the
	// fixed placeholder address 127.0.0.1:1 (pc_netplay_ice.h).
	const uint32_t ip   = sIce != nullptr ? 0x7F000001u : (sCfg.isHost ? sRemoteIp : sCfg.joinIp);
	const uint16_t port = sIce != nullptr ? (uint16_t)1 : (sCfg.isHost ? sRemotePort : sCfg.joinPort);
	sRemoteAddrBlob[0]  = (uint8_t)((ip >> 24) & 0xFF);
	sRemoteAddrBlob[1]  = (uint8_t)((ip >> 16) & 0xFF);
	sRemoteAddrBlob[2]  = (uint8_t)((ip >> 8) & 0xFF);
	sRemoteAddrBlob[3]  = (uint8_t)(ip & 0xFF);
	sRemoteAddrBlob[4]  = (uint8_t)((port >> 8) & 0xFF);
	sRemoteAddrBlob[5]  = (uint8_t)(port & 0xFF);
	GekkoNetAddress raddr;
	raddr.data = sRemoteAddrBlob;
	raddr.size = 6;
	if (sCfg.isHost) {
		sLocalHandle = gekko_add_actor(sGekko, GekkoLocalPlayer, nullptr);
		(void)gekko_add_actor(sGekko, GekkoRemotePlayer, &raddr);
	} else {
		(void)gekko_add_actor(sGekko, GekkoRemotePlayer, &raddr);
		sLocalHandle = gekko_add_actor(sGekko, GekkoLocalPlayer, nullptr);
	}
	unsigned disconnectMs = read_unsigned_env("PIKMIN_NETPLAY_DISCONNECT_MS", 15000);
	if (disconnectMs < 1000) disconnectMs = 1000;
	gekko_set_disconnect_timeout(sGekko, disconnectMs);
	printf("[netplay] disconnect timeout: %ums\n", disconnectMs);
	loadguard_configure(disconnectMs);
	pc_state_hash_set_netplay_capture(true);
	load_scripted_file();
	forensics_enable();
	sNaviPosTrace = env_is_one("PIKMIN_NETPLAY_DEBUG_NAVI_POS");
	// DELAY=auto: ceil((RTT/2)/33.333 ms - 0.05) + 1 from the median
	// handshake RTT, clamped to 1..8. The 0.05-slot tolerance keeps exact
	// boundaries from flipping one frame high on measurement overhead.
	if (sCfg.delayAuto) {
		const double rttMed = handshake_rtt_median();
		unsigned d          = 2;
		if (rttMed >= 0) d = pc_netplay_adaptive::delay_for_rtt(rttMed, 1, kMaxLocalDelay);
		sCfg.localDelay = d;
		printf("[netplay] auto delay: rtt=%.1fms (median of %llu) delay=%u\n", rttMed,
		       (unsigned long long)sHsSamples.size(), d);
	}
	gekko_set_local_delay(sGekko, sLocalHandle, (unsigned char)sCfg.localDelay);
	printf("[netplay] session started: role=%s localHandle=%d delay=%u seed=%u mode=%s captains=%d,%d\n",
	       sCfg.isHost ? "host/P1" : "joiner/P2", sLocalHandle, sCfg.localDelay, sCfg.seed,
	       xfer::mode_name(sLocal.mode.mode), sLocal.mode.captainP1, sLocal.mode.captainP2);
	adaptive_configure();
	printf("[netplay] build=%s manifest=%s\n", sBuildHexStr.c_str(), sBuildManifest.c_str());
	printf("[netplay] config=%s\n", sCfgHexStr.c_str());
	fflush(stdout);
	if (const char* e = getenv_nonempty("PIKMIN_NETPLAY_TEST_DESYNC_AT_FRAME")) {
		char* end         = nullptr;
		const long long v = strtoll(e, &end, 10);
		if (end != e && *end == '\0' && v >= 0) sTestDesyncFrame = v;
	}
	inlog_open();
	sSessionStartMs = now_ms();
	// The drift-free deadline starts here, so the first turn never overruns.
	sNextTurnMs = sSessionStartMs + 1000.0 / 30.0;
	sPhase      = kSession;
}

// GekkoNet reported a desync: print the desynced tick's sub-hashes, swap the
// forensics with the other game (which also keeps polling GekkoNet, so this
// peer's own checksum reaches it and both stop with exit 5), write the
// report, then exit 5, after the end banner when one is shown.
void handle_desync(int frame, uint32_t localCk, uint32_t remoteCk, int remoteHandle)
{
	const uint64_t wantTick = frame >= 0 ? (uint64_t)frame + 1 : 0;
	const HashEntry* e      = hash_ring_find(wantTick);
	uint64_t total = 0, subs[7] = { 0, 0, 0, 0, 0, 0, 0 }, tick = 0;
	if (e != nullptr) {
		total = e->total;
		tick  = e->tick;
		for (int k = 0; k < 7; ++k) subs[k] = e->subs[k];
	} else {
		pc_state_hash_current(&total, subs, &tick);
	}
	printf("[netplay] desync detected: frame=%d local=%08x remote=%08x handle=%d\n", frame, localCk, remoteCk,
	       remoteHandle);
	printf("[netplay] desync subs at tick=%llu%s: total=%016llx (fold %08x) navi=%016llx piki=%016llx teki=%016llx "
	       "item=%016llx world=%016llx rng=%016llx mode=%016llx\n",
	       (unsigned long long)tick, e != nullptr ? "" : " (ring miss: latest)", (unsigned long long)total,
	       fold_hash64(total), (unsigned long long)subs[0], (unsigned long long)subs[1], (unsigned long long)subs[2],
	       (unsigned long long)subs[3], (unsigned long long)subs[4], (unsigned long long)subs[5],
	       (unsigned long long)subs[6]);
	fflush(stdout);
	pc_state_hash_flush();
	desync_forensics(frame, localCk, remoteCk);
	inlog_end("desync frame=" + std::to_string(frame));
	stop_session();
	sPhase = kDone;
	printf("[netplay] session stopped: desync (exit %d)\n", npx::kDesync);
	fflush(stdout);
	std::vector<std::string> lines;
	lines.push_back("The two games stopped agreeing at frame " + std::to_string(frame) + ".");
	std::string folder = forensics_dir();
	const size_t slash = folder.find_last_of("/\\");
	if (slash != std::string::npos && slash + 1 < folder.size()) folder = folder.substr(slash + 1);
	lines.push_back("Both games wrote desync-report.txt and session-inputs.pknl to their netplay folder");
	lines.push_back("(this game: " + folder + ").");
	lines.push_back("Please send both folders with a bug report.");
	begin_banner(npx::kDesync, "DESYNC - SESSION STOPPED", true, lines);
	if (!sBannerOn) session_exit(npx::kDesync);
}

void handle_session_events()
{
	if (sGekko == nullptr) return;
	int n                  = 0;
	GekkoSessionEvent** ev = gekko_session_events(sGekko, &n);
	if (ev == nullptr || n <= 0) return;
	for (int i = 0; i < n; ++i) {
		if (ev[i] == nullptr) continue;
		switch (ev[i]->type) {
		case GekkoPlayerConnected:
			printf("[netplay] connected: handle=%d\n", ev[i]->data.connected.handle);
			break;
		case GekkoSessionStarted:
			printf("[netplay] gekko session started\n");
			sGekkoStarted = true;
			break;
		case GekkoPlayerDisconnected: {
			printf("[netplay] disconnected: handle=%d\n", ev[i]->data.disconnected.handle);
			const double sinceAdvanceMs = sLastAdvanceWallMs > 0 ? now_ms() - sLastAdvanceWallMs : 1e9;
			const bool peerQuit = sinceAdvanceMs < std::min(2500.0, 0.5 * (double)sLgWindow.normal_ms());
			printf("[netplay] session ended: %s at frame=%u\n",
			       peerQuit ? "the other player left" : "connection lost", sLastAdvanceFrame);
			fflush(stdout);
			pc_state_hash_flush();
			inlog_end(std::string(peerQuit ? "peer quit" : "connection lost") + " frame=" + std::to_string(sLastAdvanceFrame));
			stop_session();
			sPhase = kDone;
			std::vector<std::string> lines;
			if (peerQuit) lines.push_back("The other player closed the game.");
			else lines.push_back("No data from the other player for a while: the connection was lost.");
			lines.push_back("Start a new session to play again (Host Co-op / Join Co-op in the launcher).");
			begin_banner(-1, peerQuit ? "THE OTHER PLAYER LEFT" : "CONNECTION LOST", !peerQuit, lines);
			if (!sBannerOn) request_quit();
			return; // the session is gone: stop processing this batch
		}
		case GekkoDesyncDetected:
			handle_desync(ev[i]->data.desynced.frame, ev[i]->data.desynced.local_checksum,
			              ev[i]->data.desynced.remote_checksum, ev[i]->data.desynced.remote_handle);
			return; // the session is gone
		default:
			break;
		}
	}
	fflush(stdout);
}

// A local quit (the window closed mid-session) tells the other game at once
// instead of letting it wait for its disconnect timeout.
void send_quit_notice()
{
	if (sGekko == nullptr || !sGekkoStarted) return;
	gekko_disconnect_actor(sGekko, sLocalHandle);
	const double t0 = now_ms();
	while (now_ms() - t0 < 250.0) {
		gekko_network_poll(sGekko);
		sleep_hires_ms(5.0, 0.0);
	}
	printf("[netplay] quit: this game left the session; the other game was told\n");
	fflush(stdout);
}

// One simulated frame from a pair of 16-byte inputs (host p0, joiner p1).
// `frame` is the GekkoNet frame (hash tick frame + 1). Returns false when the
// session ended inside it.
bool run_advance(System* sys, BaseApp* app, int frame, const uint8_t* inputs, bool speculative)
{
	PcNetplayInput p0, p1;
	if (!pc_netplay_input_decode(inputs, 16, p0) || !pc_netplay_input_decode(inputs + 16, 16, p1)) {
		printf("[netplay] advance decode failed\n");
		fflush(stdout);
		std::abort();
	}
	inject_input(0, p0);
	inject_input(1, p1);
	inject_neutral_pad(2);
	inject_neutral_pad(3);
	// The per-tick work of the normal path, once per Advance: the audio event
	// timers (Jac_Gsync) and the thread liveness check.
	Jac_Gsync();
	(void)OSCheckActiveThreads();
	sys->updateSysClock();
	pc_netplay_on_tick_begin();
	forensics_tick_begin(frame);
	pc_netplay_camlead_check_applied((uint64_t)frame, inputs + 16 * sLocalRole);
	pc_netplay_camlead_begin_frame((uint64_t)frame);
	loadguard_tick_begin((uint32_t)frame, speculative);
	app->idle();
	loadguard_tick_end();
	pc_netplay_det_profile_note_tick();
	pc_input_log_tick_end();
	pc_state_hash_tick_end();
	{
		uint64_t total = 0, subs[7] = { 0, 0, 0, 0, 0, 0, 0 }, tick = 0;
		const bool haveHash = pc_state_hash_current(&total, subs, &tick) && tick > 0;
		if (haveHash) {
			if (sForensicsOn) pc_state_dump_capture(tick, subs);
			hash_ring_store(tick, total, subs, sForensicsOn ? pc_state_dump_last_xtra() : 0);
		}
		inlog_frame(frame, inputs, total, subs, haveHash);
		if (haveHash) test_settings_save(tick);
	}
	sLastAdvanceFrame = (uint32_t)frame;
	loadguard_after_advance((uint32_t)frame, speculative);
	++sSessionTicks;
	++sAdvances;
	adaptive_note_frame();
	// Test evidence (PIKMIN_NETPLAY_DEBUG_NAVI_POS=1): every captain's
	// position, so a pair run can prove both captains really moved.
	if (sNaviPosTrace && sSessionTicks % 150 == 0) pc_state_hash_print_navi_positions(sSessionTicks);
	// Online VS ends where a VS match goes back to the title (both peers on
	// the same tick: the choice is synced input).
	if (sCfg.mode == xfer::kModeVs && pc_vs_left_to_title() > 0) {
		printf("[netplay] VS: back to the title at frame=%d; the session ends\n", frame);
		fflush(stdout);
		pc_state_hash_flush();
		inlog_end("vs left to title frame=" + std::to_string(frame));
		stop_session();
		sPhase = kDone;
		request_quit();
		return false;
	}
	if (const char* what = unsupported_run_mode()) {
		printf("[netplay] %s is not supported online yet; stopping the session at frame=%d\n", what, frame);
		fflush(stdout);
		pc_state_hash_flush();
		stop_session();
		session_exit(npx::kUsage);
	}
	if (sCfg.exitAfter > 0 && sSessionTicks >= sCfg.exitAfter) {
		const double wallS = sSessionStartMs > 0 ? (now_ms() - sSessionStartMs) / 1000.0 : 0.0;
		const double tps   = wallS > 0 ? (double)sAdvances / wallS : 0.0;
		const double stallPct    = wallS > 0 ? 100.0 * sStallMs / (wallS * 1000.0) : 0.0;
		const double slotLossPct = tps >= 30.0 ? 0.0 : 100.0 * (1.0 - tps / 30.0);
		printf("[netplay] exit after %llu ticks\n", (unsigned long long)sSessionTicks);
		printf("[netplay] script records consumed: %llu/%llu\n", (unsigned long long)sScriptIdx,
		       (unsigned long long)sScriptTicks);
		printf("[netplay] wall=%.1fs tps=%.1f stall=%.1f%% slot-loss=%.1f%% vs 30Hz\n", wallS, tps, stallPct,
		       slotLossPct);
		fflush(stdout);
		pc_state_hash_flush();
		inlog_end("exit after " + std::to_string((unsigned long long)sSessionTicks) + " ticks");
		stop_session();
		sPhase = kDone;
		request_quit();
		return false;
	}
	return true;
}

// Runs one tick per Advance event, in event order. Returns the number of
// Advances processed (0 while waiting for remote input).
int handle_game_events(System* sys, BaseApp* app)
{
	int count           = 0;
	GekkoGameEvent** ev = gekko_update_session(sGekko, &count);
	if (ev == nullptr || count <= 0) return 0;
	int advances = 0;
	for (int i = 0; i < count; ++i) {
		if (sPhase != kSession || sGekko == nullptr) break;
		if (ev[i] == nullptr) continue;
		switch (ev[i]->type) {
		case GekkoAdvanceEvent: {
			GekkoGameEvent* e = ev[i];
			if (e->data.adv.input_len != 32 || e->data.adv.inputs == nullptr) {
				printf("[netplay] bad advance: input_len=%u\n", e->data.adv.input_len);
				fflush(stdout);
				std::abort();
			}
			run_advance(sys, app, e->data.adv.frame, e->data.adv.inputs,
			            e->data.adv.rolling_back || e->data.adv.running_ahead);
			++advances;
			break;
		}
		case GekkoSaveEvent: {
			GekkoGameEvent* e = ev[i];
			uint64_t total = 0, subs[7] = { 0, 0, 0, 0, 0, 0, 0 }, tick = 0;
			pc_state_hash_current(&total, subs, &tick);
			if (e->data.save.state != nullptr && e->data.save.state_len != nullptr && *e->data.save.state_len >= 8) {
				const uint64_t frame = (uint64_t)e->data.save.frame;
				for (int b = 0; b < 8; ++b) e->data.save.state[b] = (uint8_t)((frame >> (b * 8)) & 0xFF);
				*e->data.save.state_len = 8;
			}
			if (e->data.save.checksum != nullptr) {
				*e->data.save.checksum = fold_hash64(total);
				if (sTestDesyncFrame >= 0 && (int64_t)e->data.save.frame >= sTestDesyncFrame) {
					*e->data.save.checksum ^= 0x5A5A5A5Au;
					if (!sTestDesyncLogged) {
						sTestDesyncLogged = true;
						printf("[netplay] test: this peer reports altered checksums from frame=%d (the sim is "
						       "unchanged)\n",
						       e->data.save.frame);
						fflush(stdout);
					}
				}
			}
			++sSaves;
			break;
		}
		case GekkoLoadEvent:
			// Window 0 never rolls back: a Load means the lockstep contract broke.
			printf("[netplay] unexpected load event at frame=%d (window 0 forbids loads)\n", ev[i]->data.load.frame);
			fflush(stdout);
			std::abort();
			break;
		default:
			break;
		}
	}
	if (sPhase == kSession && sSessionTicks > 0 && sSessionTicks % 300 == 0 && advances > 0) {
		const double wallS = sSessionStartMs > 0 ? (now_ms() - sSessionStartMs) / 1000.0 : 0.0;
		const double tps   = wallS > 0 ? (double)sAdvances / wallS : 0.0;
		printf("[netplay] tick=%llu adv=%llu stalls=%llu tps=%.1f ahead=%.2f saves=%llu submitted=%llu delay=%u\n",
		       (unsigned long long)sSessionTicks, (unsigned long long)sAdvances, (unsigned long long)sStalls, tps,
		       gekko_frames_ahead(sGekko), (unsigned long long)sSaves, (unsigned long long)sSubmitted,
		       sCfg.localDelay);
		fflush(stdout);
		adaptive_stats_line("stats");
	}
	return advances;
}

// First turn: force the modes, hash the local Hello and bring the socket up.

#if PC_ONLINE
// ---- host-authoritative online co-op (nectar_net) ----
//
// The host ticks at 30 Hz on its own clock and never waits: P1 is its local
// input, P2 the guest's input from an InputReceiver. After every tick it sends
// both captains' authoritative state. The guest runs ahead of the host by the
// input lead, applies its own input at once (prediction), corrects its captain
// from the host's states and shows the host's captain interpolated. Only the
// captains are replicated so far (Phase 4); the rest of the guest's world is
// local. Both peers reuse run_advance, so hashes, traces and exit-after work
// as in lockstep.

namespace nnet = nectar::net;

constexpr std::uint16_t kOnlineProtocolMajor = 1;
constexpr std::uint16_t kOnlineSceneEpoch    = 1;
constexpr double kOnlineSlotMs               = 1000.0 / 30.0;

struct OnlineState {
	std::unique_ptr<nnet::HostRoom> room;
	std::unique_ptr<nnet::GuestJoin> join;
	nnet::GatedTransport* link = nullptr;
	nnet::InputReceiver receiver;
	nnet::ClockEstimator clock;
	nnet::PredictionCorrector corrector;
	nnet::SnapshotInterpolator remote;
	std::vector<nnet::NetInputFrame> sent; // guest: last frames, newest first
	uint64_t nextTick     = 0;  // next tick this peer simulates
	double startMs        = 0;  // host: wall time of tick 0
	int reachability      = 0;  // host: nnet::Reachability of the room
	uint16_t roomPort     = 0;  // host: the room's local port
	bool codeCopied       = false; // host: the code went to the clipboard
	std::string code;              // host, in-game menu: shown while waiting, never logged
	bool copyKeyDown      = false; // host: Ctrl+C held (copy once per press)
	bool guestEverJoined  = false; // host: the code banner goes once someone joins
	bool started          = false;
	bool hostStartSeen    = false;
	uint32_t pingId       = 0;
	double nextPingMs     = 0;
	// Diagnostics (no addresses, no secrets).
	uint64_t live = 0, repeated = 0, neutral = 0;
	double errSum = 0, errMax = 0;
	uint64_t errCount = 0;
	uint32_t lead = 1;
	bool peerGone = false;
	bool guestConnected = false;
	// Host rules (SessionRules): the host's sim settings, adopted by the guest.
	std::string rules;
	bool rulesSeen = false;
	// The host's own input, forwarded so the guest's copy of P1 also opens the
	// pause menu, whistles or throws; its position still comes from the host.
	nnet::InputReceiver hostInput;
	std::vector<nnet::NetInputFrame> hostSent;
	uint64_t hostInputNext = 0;
	int lastPause = -1;
	std::vector<uint64_t> testStartTicks; // TEST ONLY: host Start presses
	// Exact replica (default): the host sends what it simulated; the guest
	// replays exactly those ticks and compares state hashes.
	std::vector<std::vector<uint8_t>> appliedSent;          // host: newest first
	std::map<uint64_t, std::array<uint8_t, 32>> applied;     // guest: ticks to replay
	std::map<uint64_t, uint64_t> hostHashes;                 // guest: hash tick -> host total
	std::map<uint64_t, uint64_t> ownHashes;                  // guest: hash tick -> own total
	double nextSlotMs  = 0;   // guest: replay pacing
	double nextInputMs = 0;   // guest: input sampling
	uint64_t lastInputTick = 0;
	uint64_t stalls    = 0;   // guest: ticks that were due but had not arrived
	uint64_t stalledTick = UINT64_MAX;
	uint64_t hashesChecked = 0;
	bool desync        = false;
	// Host: every applied tick since 0, so a guest joining later (or after a
	// drop) replays the whole session. Bounded to four hours of play.
	std::vector<std::array<uint8_t, 32>> appliedLog;
	bool appliedLogFull  = false;
	uint64_t backfillNext = 0, backfillEnd = 0; // host: ticks still to send to a late guest
	bool catchingUp      = false;               // guest: replaying a backlog fast
	double catchUpStartMs = 0;
	// Guest: the host's card, received before the rules (continuing a file).
	std::map<std::string, std::vector<uint8_t>> cardFiles;
	std::map<std::string, uint64_t> cardReceived;
	uint64_t cardBytes = 0;
	bool cardReady     = false;
	bool cardFailed    = false;
	// Host: the card as it was when the session started. A guest joining
	// later replays from tick 0, so it must get this, not today's card.
	std::map<std::string, std::vector<uint8_t>> cardSnapshot;
	bool cardSnapshotTaken = false;
	bool cardUnshareable   = false;
	uint64_t catchUpStartTick = 0;
	// Spectator: P2 leaves co-op, keeps its slot and watches the host's
	// captain; Start asks to rejoin, which the host grants at a safe tick.
	bool spectator       = false; // both sides: P2 currently spectating
	bool rejoinPending   = false; // host: a rejoin waits for a safe tick
	bool leaveSent       = false; // guest
	bool rejoinSent      = false; // guest
	bool startWasDown    = false; // guest: Start edge while spectating
	bool leaveRequested  = false; // guest: asked by the UI (pc_netplay_online_request_leave)
	bool rejoinRequested = false; // guest: asked by the UI (pc_netplay_online_request_rejoin)
	std::vector<double> stallTimes; // guest: when ticks were late, for the HUD's last 10 s
	uint64_t testLeaveAt = 0, testRejoinAt = 0; // TEST ONLY, guest ticks
};
OnlineState* sOnline = nullptr;

nnet::CompatibilityInfo online_compat()
{
	nnet::CompatibilityInfo info;
	info.protocolMinor = nnet::kProtocolMinor;
	info.game          = static_cast<uint8_t>(nnet::GameId::Pikmin1);
	uint8_t digest[32];
	pc_netplay_build::digest(digest); // source tree, simulation ABI and retail version
	std::memcpy(info.build.data(), digest, 32);
	info.region = digest[0] == 0 ? 1 : digest[0]; // already part of the digest; kept for the UI
	return info;
}

nnet::NetInputFrame to_net_input(const PcNetplayInput& in, uint64_t tick)
{
	nnet::NetInputFrame f;
	f.tick     = tick;
	f.held     = in.buttons;
	f.stickX   = in.stickX;
	f.stickY   = in.stickY;
	f.cstickX  = in.substickX;
	f.cstickY  = in.substickY;
	f.triggerL = in.triggerL;
	f.triggerR = in.triggerR;
	f.flags    = in.ext;
	f.cursorDx = in.cursorRotate;
	f.cursorDy = (int16_t)in.controlYaw;
	return f;
}

PcNetplayInput from_net_input(const nnet::NetInputFrame& f)
{
	PcNetplayInput in;
	in.buttons      = f.held;
	in.stickX       = f.stickX;
	in.stickY       = f.stickY;
	in.substickX    = f.cstickX;
	in.substickY    = f.cstickY;
	in.triggerL     = f.triggerL;
	in.triggerR     = f.triggerR;
	in.ext          = f.flags;
	in.cursorRotate = f.cursorDx;
	in.controlYaw   = (uint16_t)f.cursorDy;
	return in;
}

nnet::InputLimits online_input_limits()
{
	nnet::InputLimits limits;
	limits.flagMask          = pc_input_log::kExtKnown;
	limits.allowStickMinimum = true; // SDL axes may reach -128
	return limits;
}

// The guest's transport changes while GuestJoin tries the code's endpoints,
// so it is looked up every time instead of kept.
nnet::GatedTransport* online_link()
{
	if (sOnline == nullptr) return nullptr;
	if (sOnline->join) return sOnline->join->transport();
	return sOnline->link;
}

void online_send(nnet::MessageType type, uint64_t tick, const std::vector<uint8_t>& payload, nnet::Delivery delivery)
{
	nnet::GatedTransport* link = online_link();
	if (link == nullptr) return;
	nnet::Message message;
	message.header.type           = type;
	message.header.simulationTick = tick;
	message.header.sceneEpoch     = kOnlineSceneEpoch;
	message.payload               = payload;
	std::vector<uint8_t> bytes;
	if (nnet::encodeMessage(message, bytes)) link->send(delivery, bytes.data(), bytes.size());
}

// Closes the room (removes the router mapping, forgets the secret).
void online_teardown()
{
	if (sOnline == nullptr) return;
	// Both sides flush before closing: the host what the guest still has to
	// replay, the guest its goodbye, or the host would keep its slot until
	// the connection times out.
	nnet::GatedTransport* link = online_link();
	if (link != nullptr && link->state() == nnet::TransportState::Connected) {
		link->disconnectPeer(true);
		std::this_thread::sleep_for(std::chrono::milliseconds(sCfg.isHost ? 500 : 300));
	}
	delete sOnline;
	sOnline = nullptr;
}

// The guest could not join. Started from the in-game menu, the game comes
// back in normal mode and the title says why; otherwise it exits (tools).
[[noreturn]] void online_join_failed(const char* why)
{
	printf("[online] %s\n", why);
	fflush(stdout);
#if PIKI_NECTAR_ONLINE
	std::string err;
	if (sCfg.onlineFromMenu && pc_online_relaunch({ "--online-error", why }, &err)) {
#if defined(__ANDROID__)
		// Java restarts the activity and ends this process.
		for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
#else
		std::_Exit(0);
#endif
	}
#endif
	session_exit(npx::kRefused);
}

const char* join_failure_text(nnet::JoinFailure failure)
{
	switch (failure) {
	case nnet::JoinFailure::BadCode: return "The code is wrong or incomplete: copy it again.";
	case nnet::JoinFailure::Expired: return "The code has expired: ask your friend for a new one.";
	case nnet::JoinFailure::WrongVersion:
	case nnet::JoinFailure::Incompatible:
		return "Your friend's game is a different version. Both need the same disc (USA or Europe) and the same "
		       "Open Nectar build.";
	case nnet::JoinFailure::Unreachable:
		return "Could not reach your friend's game. On the same Wi-Fi, check the host PC's firewall (UDP port 39911). "
		       "Over the Internet the host's router must open that port (UPnP).";
	case nnet::JoinFailure::Refused: return "The room refused the code (it was closed or reopened): ask for a new code.";
	case nnet::JoinFailure::RoomFull: return "Someone else is already in that room.";
	default: return "The connection to your friend's game was lost.";
	}
}

void online_setup()
{
	if (sOnline != nullptr) return; // the guest joined early, at settings init
	sOnline = new OnlineState();
	if (sCfg.isHost && env_is_one("PIKMIN_NETPLAY_TEST_KNOBS")) {
		if (const char* list = getenv_nonempty("PIKMIN_ONLINE_TEST_START_AT")) {
			for (const char* at = list; *at != '\0';) {
				char* end = nullptr;
				const unsigned long long v = strtoull(at, &end, 10);
				if (end == at) break;
				sOnline->testStartTicks.push_back(v);
				at = *end == ',' ? end + 1 : end;
			}
			printf("[online] test: host presses Start at %zu tick(s)\n", sOnline->testStartTicks.size());
		}
	}
	if (!sCfg.isHost && env_is_one("PIKMIN_NETPLAY_TEST_KNOBS")) {
		sOnline->testLeaveAt  = read_u32_env("PIKMIN_ONLINE_TEST_LEAVE_AT", 0);
		sOnline->testRejoinAt = read_u32_env("PIKMIN_ONLINE_TEST_REJOIN_AT", 0);
		if (sOnline->testLeaveAt != 0)
			printf("[online] test: guest leaves at %llu, asks to rejoin at %llu\n",
			       (unsigned long long)sOnline->testLeaveAt, (unsigned long long)sOnline->testRejoinAt);
	}
	nnet::InputPolicy policy;
	policy.continuousFlags = pc_input_log::kExtSwarm;
	sOnline->receiver      = nnet::InputReceiver(policy);
	const std::uint64_t nowUnix = (std::uint64_t)time(nullptr);
	if (sCfg.isHost) {
		nnet::RoomConfig room;
		room.protocolMajor = kOnlineProtocolMajor;
		room.compatibility = online_compat();
		room.localPort     = sCfg.onlinePort;
		// TEST ONLY: loopback runs skip the router and advertise 127.0.0.1.
		if (env_is_one("PIKMIN_ONLINE_TEST_LOOPBACK")) {
			room.automaticPortMapping = false;
			room.includeLanEndpoint   = false;
			nnet::DirectEndpoint loopback;
			loopback.address = { 127, 0, 0, 1 };
			loopback.port    = sCfg.onlinePort;
			room.manualEndpoints.push_back(loopback);
		}
		sOnline->room = std::make_unique<nnet::HostRoom>();
		nnet::RoomStatus status;
		const nnet::RoomError error = sOnline->room->open(room, nowUnix, status);
		if (error != nnet::RoomError::None) {
			printf("[online] could not open the room: %s\n", nnet::roomErrorString(error));
			fflush(stdout);
			session_exit(npx::kTransport);
		}
		sOnline->link = sOnline->room->transport();
		// The code is a secret: it goes to the UI or a file, never to the log.
		if (!sCfg.onlineCodeOut.empty()) {
#ifdef _WIN32
			FILE* f = fopen(sCfg.onlineCodeOut.c_str(), "wb");
#else
			// Readable by this user only: the code admits whoever holds it.
			const int fd = open(sCfg.onlineCodeOut.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
			FILE* f      = fd >= 0 ? fdopen(fd, "wb") : nullptr;
#endif
			const bool ok = f != nullptr && fwrite(status.code.data(), 1, status.code.size(), f) == status.code.size();
			if (f != nullptr) fclose(f);
			if (!ok) usage_exit("cannot write the online code to %s", sCfg.onlineCodeOut.c_str());
		}
		sOnline->reachability = (int)status.reachability;
		sOnline->roomPort     = status.localPort;
		if (sCfg.onlineCodeClipboard) {
			sOnline->code       = status.code;
			sOnline->codeCopied = SDL_SetClipboardText(status.code.c_str()) == 0;
			if (!sOnline->codeCopied) printf("[online] clipboard copy failed: %s\n", SDL_GetError());
		}
		printf("[online] room open: port=%u reachability=%s method=%s code=%s\n", (unsigned)status.localPort,
		       nnet::reachabilityString(status.reachability), nnet::mappingMethodString(status.method),
		       sCfg.onlineCodeClipboard ? "(copied to the clipboard)" : "(written to the code file)");
	} else {
		sOnline->join = std::make_unique<nnet::GuestJoin>();
		const nnet::JoinFailure failure = sOnline->join->start(sCfg.onlineCode, online_compat(), kOnlineProtocolMajor,
		                                                       nowUnix, (std::uint64_t)now_ms());
		if (failure != nnet::JoinFailure::None) {
			printf("[online] cannot join: %s\n", nnet::joinFailureString(failure));
			online_join_failed(join_failure_text(failure));
		}
		printf("[online] joining\n");
	}
	fflush(stdout);
}

void online_send_captains(uint64_t tick)
{
	std::vector<nnet::EntityRecord> records;
	for (int player = 0; player < 2; ++player) {
		PcOnlineCaptain c;
		if (!pc_online_read_captain(player, &c)) continue;
		nnet::CaptainState state;
		state.position      = { c.position[0], c.position[1], c.position[2] };
		state.velocity      = { c.velocity[0], c.velocity[1], c.velocity[2] };
		state.faceDirection = c.faceDirection;
		state.player        = (uint8_t)player;
		records.push_back(nnet::captainRecord(kOnlineSceneEpoch, state));
	}
	if (records.empty()) return;
	online_send(nnet::MessageType::WorldDelta, tick, nnet::encodeWorldDelta(0, records), nnet::Delivery::Unreliable);
}

// Decodes one session message; false for anything malformed (dropped).
bool online_decode(const nnet::TransportEvent& event, nnet::Message& message)
{
	return event.type == nnet::TransportEventType::Message
	    && nnet::decodeMessage(event.payload.data(), event.payload.size(), message, nnet::limits::kMaxNormalPayloadBytes)
	           == nnet::DecodeError::None;
}

void online_host_set_spectator(bool on, uint64_t tick);
void online_host_send_welcome();
void online_guest_card_chunk(const std::vector<uint8_t>& p);
void online_guest_card_manifest(const std::vector<uint8_t>& p);

void online_host_events()
{
	nnet::TransportEvent event;
	while (sOnline->link->poll(event)) {
		if (event.type == nnet::TransportEventType::Connected && sOnline->startMs > 0) {
			// A guest joining a running session (or coming back): the rules,
			// then every tick so far; it catches up without drawing.
			if (sOnline->appliedLogFull) {
				printf("[online] a guest joined but the session is too long to replay; it is dropped\n");
				sOnline->link->disconnectPeer(false);
				continue;
			}
			sOnline->receiver      = nnet::InputReceiver(sOnline->receiver.policy());
			sOnline->guestEverJoined = true;
			sOnline->spectator     = false;
			sOnline->rejoinPending = false;
			online_host_send_welcome();
			sOnline->backfillNext = 0;
			sOnline->backfillEnd  = sOnline->nextTick;
			printf("[online] guest joined a running session at tick=%llu; sending its history\n",
			       (unsigned long long)sOnline->nextTick);
			fflush(stdout);
			continue;
		}
		if (event.type == nnet::TransportEventType::Disconnected || event.type == nnet::TransportEventType::Error) {
			// The room stays open; P2 gets neutral input until someone joins.
			printf("[online] guest left at tick=%llu; P2 is neutral\n", (unsigned long long)sOnline->nextTick);
			fflush(stdout);
			continue;
		}
		nnet::Message message;
		if (!online_decode(event, message)) continue;
		if (message.header.type == nnet::MessageType::InputBundle) {
			std::vector<nnet::NetInputFrame> frames;
			if (nnet::decodeInputBundle(message.payload.data(), message.payload.size(), online_input_limits(), frames))
				sOnline->receiver.receive(frames, sOnline->nextTick);
		} else if (message.header.type == nnet::MessageType::LeaveCoopRequest && message.payload.empty()) {
			if (!sOnline->spectator) online_host_set_spectator(true, sOnline->nextTick);
		} else if (message.header.type == nnet::MessageType::RejoinRequest && message.payload.empty()) {
			if (sOnline->spectator) sOnline->rejoinPending = true;
		} else if (message.header.type == nnet::MessageType::Ping) {
			nnet::PingPayload ping;
			if (!nnet::decodePing(message.payload.data(), message.payload.size(), ping)) continue;
			const double sinceTick = now_ms() - (sOnline->startMs + (double)sOnline->nextTick * kOnlineSlotMs);
			nnet::PongPayload pong;
			pong.id              = ping.id;
			pong.echoTimeMs      = ping.senderTimeMs;
			pong.hostTick        = sOnline->nextTick;
			pong.hostTickPhaseMs = sinceTick < 0 ? 0 : sinceTick > 33 ? 33 : (uint32_t)sinceTick;
			online_send(nnet::MessageType::Pong, sOnline->nextTick, nnet::encodePong(pong), nnet::Delivery::Unreliable);
		}
	}
}

void online_guest_events()
{
	nnet::TransportEvent event;
	bool hasEvent = false;
	for (;;) {
		const nnet::JoinProgress progress = sOnline->join->poll((std::uint64_t)now_ms(), event, hasEvent);
		sOnline->guestConnected = progress == nnet::JoinProgress::Connected;
		if (progress == nnet::JoinProgress::Failed) {
			if (!sOnline->peerGone) {
				printf("[online] connection ended: %s\n", nnet::joinFailureString(sOnline->join->failure()));
				fflush(stdout);
			}
			sOnline->peerGone = true;
			return;
		}
		if (!hasEvent) return;
		nnet::Message message;
		if (!online_decode(event, message)) continue;
		if (message.header.type == nnet::MessageType::Pong) {
			nnet::PongPayload pong;
			if (nnet::decodePong(message.payload.data(), message.payload.size(), pong))
				sOnline->clock.addSample(pong, (std::uint64_t)now_ms());
		} else if (message.header.type == nnet::MessageType::SceneStart) {
			sOnline->hostStartSeen = true;
		} else if (message.header.type == nnet::MessageType::AppliedInputs) {
			uint64_t newest = 0;
			std::vector<std::vector<uint8_t>> blobs;
			if (!nnet::decodeAppliedInputs(message.payload.data(), message.payload.size(), 32, newest, blobs)) continue;
			for (size_t i = 0; i < blobs.size(); ++i) {
				const uint64_t tick = newest - i;
				// Only ticks still to replay, and never an unbounded backlog.
				if (tick < sOnline->nextTick || tick > sOnline->nextTick + 500000) continue;
				std::array<uint8_t, 32> entry;
				std::memcpy(entry.data(), blobs[i].data(), 32);
				sOnline->applied.emplace(tick, entry);
			}
		} else if (message.header.type == nnet::MessageType::StateHash) {
			nnet::StateHashPayload hash;
			if (nnet::decodeStateHash(message.payload.data(), message.payload.size(), hash) && sOnline->hostHashes.size() < 4096)
				sOnline->hostHashes[hash.tick] = hash.hash;
		} else if (message.header.type == nnet::MessageType::ContentChunk) {
			online_guest_card_chunk(message.payload);
		} else if (message.header.type == nnet::MessageType::ContentManifest) {
			online_guest_card_manifest(message.payload);
		} else if (message.header.type == nnet::MessageType::PlayerControlChanged) {
			if (message.payload.size() != 10 || message.payload[0] != 1 || message.payload[1] > 1) continue;
			const bool on      = message.payload[1] == 1;
			sOnline->spectator = on;
			sOnline->leaveSent = sOnline->rejoinSent = false;
			// The view follows the host's captain while spectating.
			pc_netplay_present_set_local_player_default(on ? 0 : sLocalRole);
			printf("[online] %s at host tick=%llu\n", on ? "spectating" : "back in co-op",
			       (unsigned long long)message.header.simulationTick);
			fflush(stdout);
		} else if (message.header.type == nnet::MessageType::SessionRules) {
			// Printable "key=value;" text, bounded; anything else is refused.
			const std::vector<uint8_t>& text = message.payload;
			bool printable = !text.empty() && text.size() <= 8192;
			for (uint8_t ch : text) printable = printable && ((ch >= 0x20 && ch < 0x7f) || ch == '\n');
			const std::string all(text.begin(), text.end());
			const size_t newline = all.find('\n');
			const size_t newline2 = newline == std::string::npos ? std::string::npos : all.find('\n', newline + 1);
			const size_t newline3 = newline2 == std::string::npos ? std::string::npos : all.find('\n', newline2 + 1);
			if (printable && !sOnline->rulesSeen && all.rfind("seed=", 0) == 0 && newline3 != std::string::npos
			    && all.compare(newline + 1, 5, "save=") == 0 && all.compare(newline2 + 1, 5, "mode=") == 0) {
				char* end            = nullptr;
				const unsigned long seed = strtoul(all.c_str() + 5, &end, 10);
				if (end != all.c_str() + newline || seed > 0xffffffffUL) continue;
				const unsigned long slot = strtoul(all.c_str() + newline + 6, &end, 10);
				if (end != all.c_str() + newline2 || slot > 3) continue;
				// Both peers boot the mode before the rules arrive: the guest
				// must have joined from the same menu (Co-op or VS).
				const std::string mode = all.substr(newline2 + 6, newline3 - newline2 - 6);
				const char* mine       = sCfg.mode == xfer::kModeVs ? "vs" : "coop";
				if (mode != mine) {
					online_join_failed(mode == "vs" ? "Your friend is playing VS: join from the VS menu."
					                                : "Your friend is playing Co-op: join from the Co-op menu.");
				}
				// A continued file needs the whole card first.
				if (slot != 0 && !sOnline->cardReady) {
					printf("[online] the host continues file %lu but its card did not arrive intact\n", slot);
					fflush(stdout);
					session_exit(npx::kRefused);
				}
				pc_coop_direct_set_slot((int)slot);
				// The day reseed reads the seed from the environment.
				sCfg.seed = (uint32_t)seed;
#ifdef _WIN32
				_putenv_s("PIKMIN_NETPLAY_SEED", std::to_string(seed).c_str());
#else
				setenv("PIKMIN_NETPLAY_SEED", std::to_string(seed).c_str(), 1);
#endif
				sOnline->rules     = all.substr(newline3 + 1);
				sOnline->rulesSeen = true;
			}
		} else if (message.header.type == nnet::MessageType::InputBundle) {
			std::vector<nnet::NetInputFrame> frames;
			if (nnet::decodeInputBundle(message.payload.data(), message.payload.size(), online_input_limits(), frames))
				sOnline->hostInput.receive(frames, sOnline->hostInputNext);
		} else if (message.header.type == nnet::MessageType::WorldDelta) {
			uint8_t flags = 0;
			std::vector<nnet::EntityRecord> records;
			if (!nnet::decodeWorldDelta(message.payload.data(), message.payload.size(), kOnlineSceneEpoch,
			                            nnet::WorldDeltaLimits{}, flags, records))
				continue;
			const uint64_t tick = message.header.simulationTick;
			for (const nnet::EntityRecord& record : records) {
				nnet::CaptainState state;
				if (record.schema != nnet::kCaptainSchema
				    || !nnet::decodeCaptainState(record.data.data(), record.data.size(), state))
					continue;
				if (state.player == (uint8_t)sLocalRole) {
					const float err = sOnline->corrector.reconcile(tick, state.position);
					if (err > 0) {
						sOnline->errSum += err;
						sOnline->errMax = std::max(sOnline->errMax, (double)err);
						++sOnline->errCount;
					}
				} else {
					sOnline->remote.push(tick, state);
				}
			}
		}
	}
}

// Connection phase. True once the session can start ticking.
bool online_connect_pump()
{
	const double now = now_ms();
	if (sCfg.isHost) {
		online_host_events();
		if (sOnline->link->state() != nnet::TransportState::Connected) {
			// From the in-game menu the game does not wait in a black window:
			// it starts, and the guest joins the running session (it gets the
			// rules and every tick so far, and catches up).
			if (!sCfg.onlineCodeClipboard) return false;
			sOnline->startMs  = now;
			sOnline->nextTick = 0;
			printf("[online] room open; the game starts and the guest can join at any time\n");
			fflush(stdout);
			return true;
		}
		sOnline->guestEverJoined = true;
		sOnline->startMs  = now;
		sOnline->nextTick = 0;
		// The session plays by the host's rules (sim settings).
		online_host_send_welcome();
		printf("[online] guest joined; host ticking\n");
		fflush(stdout);
		return true;
	}
	online_guest_events();
	if (sOnline->peerGone) {
		session_exit(npx::kRefused);
	}
	if (sOnline->guestConnected && now >= sOnline->nextPingMs) {
		nnet::PingPayload ping;
		ping.id           = ++sOnline->pingId;
		ping.senderTimeMs = (std::uint64_t)now;
		online_send(nnet::MessageType::Ping, 0, nnet::encodePing(ping), nnet::Delivery::Unreliable);
		sOnline->nextPingMs = now + 100;
	}
	// Start once the host has started and the clock has a few samples.
	if (!sOnline->hostStartSeen || !sOnline->clock.ready() || sOnline->pingId < 4) return false;
	sOnline->nextTick = 0; // catches up to the host quickly (the boot is mostly loading)
	printf("[online] joined; rtt=%u ms jitter=%u ms\n", sOnline->clock.rttMs(), sOnline->clock.jitterMs());
	fflush(stdout);
	return true;
}

std::vector<uint8_t> online_control_payload(bool spectator, uint64_t tick)
{
	std::vector<uint8_t> out = { 1, (uint8_t)(spectator ? 1 : 0) };
	for (int i = 7; i >= 0; --i) out.push_back((uint8_t)(tick >> (8 * i)));
	return out;
}

void online_host_set_spectator(bool on, uint64_t tick)
{
	sOnline->spectator     = on;
	sOnline->rejoinPending = false;
	online_send(nnet::MessageType::PlayerControlChanged, tick, online_control_payload(on, tick),
	            nnet::Delivery::ReliableOrdered);
	printf("[online] P2 %s at tick=%llu\n", on ? "is spectating" : "rejoined", (unsigned long long)tick);
	fflush(stdout);
}

// Exact replica: what the host just simulated, with 8 ticks of redundancy
// on the unreliable channel plus a reliable copy, so the guest never misses
// a tick; and the state hash twice a second for desync detection.
void online_send_applied(uint64_t tick, const PcNetplayInput& p0, const PcNetplayInput& p1)
{
	std::vector<uint8_t> blob(32);
	pc_netplay_input_encode(p0, blob.data());
	pc_netplay_input_encode(p1, blob.data() + 16);
	sOnline->appliedSent.insert(sOnline->appliedSent.begin(), blob);
	if (sOnline->appliedSent.size() > nnet::kMaxAppliedTicks) sOnline->appliedSent.pop_back();
	online_send(nnet::MessageType::AppliedInputs, tick, nnet::encodeAppliedInputs(tick, sOnline->appliedSent),
	            nnet::Delivery::Unreliable);
	online_send(nnet::MessageType::AppliedInputs, tick, nnet::encodeAppliedInputs(tick, { blob }),
	            nnet::Delivery::ReliableOrdered);
	constexpr size_t kMaxLogTicks = 4u * 60u * 60u * 30u;
	if (sOnline->appliedLog.size() == tick && sOnline->appliedLog.size() < kMaxLogTicks) {
		std::array<uint8_t, 32> entry;
		std::memcpy(entry.data(), blob.data(), 32);
		sOnline->appliedLog.push_back(entry);
	} else if (!sOnline->appliedLogFull && sOnline->appliedLog.size() >= kMaxLogTicks) {
		sOnline->appliedLogFull = true;
		printf("[online] session longer than four hours: a guest can no longer join late\n");
		fflush(stdout);
	}
}


// ---- the host's memory card, copied to the guest (continuing a file) ----
//
// ContentChunk:    kind u8 (1 = card file) | name length u8 | name | file size u32 | offset u32 | bytes
// ContentManifest: kind u8 (1) | file count u16 | total bytes u32 | SHA-256 of every (name, size, bytes)
constexpr uint8_t kCardKind        = 1;
constexpr size_t kCardMaxFiles     = 128;
constexpr size_t kCardMaxFileBytes = 1u << 20;
constexpr size_t kCardMaxTotal     = 4u << 20;
constexpr size_t kCardChunkBytes   = 32u * 1024u;

bool card_name_ok(const std::string& name)
{
	if (name.empty() || name.size() > 64 || name == "." || name == "..") return false;
	for (unsigned char ch : name)
		if (ch < 0x20 || ch > 0x7e || ch == '/' || ch == '\\' || ch == ':') return false;
	return true;
}

void card_digest_add(std::vector<uint8_t>& acc, const std::string& name, const std::vector<uint8_t>& data)
{
	acc.push_back((uint8_t)name.size());
	acc.insert(acc.end(), name.begin(), name.end());
	for (int i = 3; i >= 0; --i) acc.push_back((uint8_t)(data.size() >> (8 * i)));
	acc.insert(acc.end(), data.begin(), data.end());
}

void put_u32(std::vector<uint8_t>& out, uint32_t v)
{
	for (int i = 3; i >= 0; --i) out.push_back((uint8_t)(v >> (8 * i)));
}

uint32_t get_u32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

// Host: every regular file of card 0, in name order, as it was when the
// session started (read once, then reused for every guest).
bool online_host_send_card()
{
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path dir = pc_card_channel_dir(0);
	std::map<std::string, std::vector<uint8_t>>& files = sOnline->cardSnapshot;
	size_t total = 0;
	for (const auto& [name, data] : files) total += data.size();
	if (sOnline->cardUnshareable) return false;
	if (!sOnline->cardSnapshotTaken && fs::is_directory(dir, ec)) {
		sOnline->cardSnapshotTaken = true;
		auto refuse = [&]() {
			files.clear();
			sOnline->cardUnshareable = true;
			return false;
		};
		for (const auto& entry : fs::directory_iterator(dir, ec)) {
			if (!entry.is_regular_file(ec)) continue;
			const std::string name = entry.path().filename().string();
			const uint64_t size    = entry.file_size(ec);
			if (!card_name_ok(name) || size > kCardMaxFileBytes || files.size() >= kCardMaxFiles) return refuse();
			std::vector<uint8_t> data((size_t)size);
			FILE* f = fopen(entry.path().string().c_str(), "rb");
			const bool ok = f != nullptr && fread(data.data(), 1, data.size(), f) == data.size();
			if (f != nullptr) fclose(f);
			if (!ok) return refuse();
			total += data.size();
			if (total > kCardMaxTotal) return refuse();
			files.emplace(name, std::move(data));
		}
	}
	std::vector<uint8_t> digestInput;
	for (const auto& [name, data] : files) {
		size_t offset = 0;
		do {
			const size_t n = std::min(kCardChunkBytes, data.size() - offset);
			std::vector<uint8_t> chunk = { kCardKind, (uint8_t)name.size() };
			chunk.insert(chunk.end(), name.begin(), name.end());
			put_u32(chunk, (uint32_t)data.size());
			put_u32(chunk, (uint32_t)offset);
			chunk.insert(chunk.end(), data.begin() + (ptrdiff_t)offset, data.begin() + (ptrdiff_t)(offset + n));
			online_send(nnet::MessageType::ContentChunk, 0, chunk, nnet::Delivery::ReliableOrdered);
			offset += n;
		} while (offset < data.size());
		card_digest_add(digestInput, name, data);
	}
	uint8_t digest[32];
	pc_netplay_sha::sha256(digestInput.data(), digestInput.size(), digest);
	std::vector<uint8_t> manifest = { kCardKind, (uint8_t)(files.size() >> 8), (uint8_t)files.size() };
	put_u32(manifest, (uint32_t)total);
	manifest.insert(manifest.end(), digest, digest + 32);
	online_send(nnet::MessageType::ContentManifest, 0, manifest, nnet::Delivery::ReliableOrdered);
	printf("[online] sent the host's card to the guest (%zu file(s), %zu bytes)\n", files.size(), total);
	fflush(stdout);
	return true;
}

// Guest: one chunk; anything out of bounds fails the whole transfer.
void online_guest_card_chunk(const std::vector<uint8_t>& p)
{
	if (sOnline->cardFailed || sOnline->cardReady) return;
	auto fail = [](const char* why) {
		sOnline->cardFailed = true;
		printf("[online] refused the host's card: %s\n", why);
		fflush(stdout);
	};
	if (p.size() < 2 || p[0] != kCardKind) return fail("bad chunk");
	const size_t nameLen = p[1];
	if (p.size() < 2 + nameLen + 8) return fail("short chunk");
	const std::string name(p.begin() + 2, p.begin() + 2 + (ptrdiff_t)nameLen);
	const uint32_t size   = get_u32(p.data() + 2 + nameLen);
	const uint32_t offset = get_u32(p.data() + 6 + nameLen);
	const size_t n        = p.size() - (10 + nameLen);
	if (!card_name_ok(name)) return fail("bad file name");
	if (size > kCardMaxFileBytes || offset > size || n > size - offset) return fail("chunk out of bounds");
	auto it = sOnline->cardFiles.find(name);
	if (it == sOnline->cardFiles.end()) {
		if (sOnline->cardFiles.size() >= kCardMaxFiles || sOnline->cardBytes + size > kCardMaxTotal)
			return fail("card too large");
		it = sOnline->cardFiles.emplace(name, std::vector<uint8_t>(size)).first;
		sOnline->cardBytes += size;
	} else if (it->second.size() != size) {
		return fail("file size changed");
	}
	std::memcpy(it->second.data() + offset, p.data() + 10 + nameLen, n);
	sOnline->cardReceived[name] += n;
}

// Guest: the manifest closes the transfer; verified, then written to this
// session's private card folder before the game opens the card.
void online_guest_card_manifest(const std::vector<uint8_t>& p)
{
	if (sOnline->cardFailed || sOnline->cardReady) return;
	if (p.size() != 1 + 2 + 4 + 32 || p[0] != kCardKind) {
		sOnline->cardFailed = true;
		return;
	}
	const size_t count = ((size_t)p[1] << 8) | p[2];
	const uint32_t total = get_u32(p.data() + 3);
	std::vector<uint8_t> digestInput;
	bool complete = count == sOnline->cardFiles.size() && total == sOnline->cardBytes;
	for (const auto& [name, data] : sOnline->cardFiles) {
		complete = complete && sOnline->cardReceived[name] == data.size();
		card_digest_add(digestInput, name, data);
	}
	uint8_t digest[32];
	pc_netplay_sha::sha256(digestInput.data(), digestInput.size(), digest);
	if (!complete || std::memcmp(digest, p.data() + 7, 32) != 0) {
		sOnline->cardFailed = true;
		printf("[online] refused the host's card: incomplete or corrupted\n");
		fflush(stdout);
		return;
	}
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path dir = pc_card_channel_dir(0);
	fs::create_directories(dir, ec);
	for (const auto& [name, data] : sOnline->cardFiles) {
		FILE* f = fopen((dir / name).string().c_str(), "wb");
		const bool ok = f != nullptr && fwrite(data.data(), 1, data.size(), f) == data.size();
		if (f != nullptr) fclose(f);
		if (!ok) {
			sOnline->cardFailed = true;
			printf("[online] could not write the host's card\n");
			fflush(stdout);
			return;
		}
	}
	sOnline->cardReady = true;
	printf("[online] received the host's card (%zu file(s), %llu bytes)\n", count, (unsigned long long)total);
	fflush(stdout);
}

// A guest that joined after tick 0 gets the session so far, a slice per turn
// so neither GNS's send buffer nor the frame time explodes.
void online_host_backfill()
{
	if (sOnline->backfillNext >= sOnline->backfillEnd) return;
	const uint64_t stop = std::min<uint64_t>(sOnline->backfillEnd, sOnline->backfillNext + 2000);
	while (sOnline->backfillNext < stop) {
		const uint64_t first = sOnline->backfillNext;
		const uint64_t last  = std::min<uint64_t>(stop, first + nnet::kMaxAppliedTicks) - 1;
		std::vector<std::vector<uint8_t>> blobs;
		for (uint64_t t = last + 1; t-- > first;)
			blobs.emplace_back(sOnline->appliedLog[t].begin(), sOnline->appliedLog[t].end());
		online_send(nnet::MessageType::AppliedInputs, last, nnet::encodeAppliedInputs(last, blobs),
		            nnet::Delivery::ReliableOrdered);
		sOnline->backfillNext = last + 1;
	}
	if (sOnline->backfillNext >= sOnline->backfillEnd) {
		printf("[online] late guest: session history sent (%llu ticks)\n", (unsigned long long)sOnline->backfillEnd);
		fflush(stdout);
	}
}

void online_host_send_welcome()
{
	// Continuing a file: the card goes first, so the rules (reliable and
	// ordered) can only arrive once the guest has all of it.
	if (sCfg.onlineSaveSlot != 0 && !online_host_send_card()) {
		printf("[online] the host's card cannot be shared (too large or unreadable); the guest is dropped\n");
		fflush(stdout);
		sOnline->link->disconnectPeer(false);
		return;
	}
	// "seed=<n>", "save=<slot>" and "mode=coop|vs" lines, then the sim settings text.
	const std::string rules = "seed=" + std::to_string(sCfg.seed) + "\nsave=" + std::to_string(sCfg.onlineSaveSlot)
	                        + "\nmode=" + (sCfg.mode == xfer::kModeVs ? "vs" : "coop") + "\n"
	                        + pc_settings_netplay_config_text();
	online_send(nnet::MessageType::SessionRules, 0, std::vector<uint8_t>(rules.begin(), rules.end()),
	            nnet::Delivery::ReliableOrdered);
	online_send(nnet::MessageType::SceneStart, 0, nnet::encodeSceneStart({ kOnlineSceneEpoch, 0 }),
	            nnet::Delivery::ReliableOrdered);
}

void online_send_hash(uint64_t tick)
{
	uint64_t total = 0, hashTick = 0;
	if (tick % 15 == 14 && pc_state_hash_current(&total, nullptr, &hashTick) && hashTick > 0) {
		nnet::StateHashPayload hash;
		hash.tick = hashTick;
		hash.hash = total;
		online_send(nnet::MessageType::StateHash, tick, nnet::encodeStateHash(hash), nnet::Delivery::ReliableOrdered);
	}
}

void online_trace_pause(uint64_t tick)
{
	const int state = pc_online_pause_state();
	if (state == sOnline->lastPause) return;
	sOnline->lastPause = state;
	printf("[online] pause tick=%llu state=0x%x\n", (unsigned long long)tick, (unsigned)state);
	fflush(stdout);
}

void online_start()
{
	pc_state_hash_set_netplay_capture(true);
	load_scripted_file();
	sNaviPosTrace   = env_is_one("PIKMIN_NETPLAY_DEBUG_NAVI_POS");
	sSessionStartMs = now_ms();
	sPhase          = kSession;
	printf("[netplay] session started: role=%s online=host-authoritative\n", sCfg.isHost ? "host/P1" : "joiner/P2");
	fflush(stdout);
}

void online_log(uint64_t tick)
{
	if (sCfg.isHost) {
		printf("[online] tick=%llu guest-input live=%llu repeated=%llu neutral=%llu ignored=%llu\n",
		       (unsigned long long)tick, (unsigned long long)sOnline->live, (unsigned long long)sOnline->repeated,
		       (unsigned long long)sOnline->neutral, (unsigned long long)sOnline->receiver.ignoredFrames());
	} else {
		printf("[online] tick=%llu rtt=%u jitter=%u lead=%u corrections=%llu snaps=%llu err-avg=%.2f err-max=%.2f\n",
		       (unsigned long long)tick, sOnline->clock.rttMs(), sOnline->clock.jitterMs(), sOnline->lead,
		       (unsigned long long)sOnline->corrector.corrections(), (unsigned long long)sOnline->corrector.snaps(),
		       sOnline->errCount ? sOnline->errSum / (double)sOnline->errCount : 0.0, sOnline->errMax);
	}
	fflush(stdout);
}

bool online_advance(System* sys, BaseApp* app, uint64_t tick, const PcNetplayInput& p0, const PcNetplayInput& p1)
{
	uint8_t inputs[32];
	pc_netplay_input_encode(p0, inputs);
	pc_netplay_input_encode(p1, inputs + 16);
	return run_advance(sys, app, (int)tick, inputs, false);
}


// Compares every host hash the guest can match; a mismatch means the replica
// diverged, which cannot be repaired yet (option B): stop with a report.
void online_check_hashes()
{
	for (auto it = sOnline->hostHashes.begin(); it != sOnline->hostHashes.end();) {
		const auto own = sOnline->ownHashes.find(it->first);
		if (own == sOnline->ownHashes.end()) {
			if (!sOnline->ownHashes.empty() && it->first < sOnline->ownHashes.begin()->first) {
				it = sOnline->hostHashes.erase(it); // too old to compare
				continue;
			}
			++it;
			continue;
		}
		++sOnline->hashesChecked;
		if (own->second != it->second && !sOnline->desync) {
			sOnline->desync = true;
			printf("[online] desync: host and guest differ at hash tick=%llu (host %016llx, guest %016llx)\n",
			       (unsigned long long)it->first, (unsigned long long)it->second, (unsigned long long)own->second);
			fflush(stdout);
		}
		it = sOnline->hostHashes.erase(it);
	}
	while (sOnline->ownHashes.size() > 600) sOnline->ownHashes.erase(sOnline->ownHashes.begin());
}

// Guest input for the exact replica: sampled on the wall clock (not on the
// replay), tagged with the host tick it should land on.
void online_guest_send_input(double now)
{
	if (sOnline->nextInputMs == 0) sOnline->nextInputMs = now;
	int sampled = 0;
	while (now >= sOnline->nextInputMs && sampled < 4) {
		sOnline->nextInputMs += kOnlineSlotMs;
		++sampled;
		PcNetplayInput p1 = build_local_input();
		if (sOnline->catchingUp) p1 = pc_netplay_input_neutral(); // not on screen yet: do not play blind
		uint64_t tick     = sOnline->clock.hostTickAt((std::uint64_t)now) + sOnline->lead;
		if (tick <= sOnline->lastInputTick) tick = sOnline->lastInputTick + 1;
		// Test triggers fire once, like a real request.
		const bool testLeave = sOnline->testLeaveAt != 0 && sOnline->nextTick >= sOnline->testLeaveAt;
		if ((sOnline->leaveRequested || testLeave) && !sOnline->spectator && !sOnline->leaveSent) {
			online_send(nnet::MessageType::LeaveCoopRequest, tick, {}, nnet::Delivery::ReliableOrdered);
			sOnline->leaveSent      = true;
			sOnline->leaveRequested = false;
			if (testLeave) sOnline->testLeaveAt = 0;
		}
		if (sOnline->spectator) {
			const bool startDown  = (p1.buttons & PAD_BUTTON_START) != 0;
			const bool testRejoin = (sOnline->testRejoinAt != 0 && sOnline->nextTick >= sOnline->testRejoinAt)
			                     || sOnline->rejoinRequested;
			if (testRejoin) {
				sOnline->testRejoinAt    = 0;
				sOnline->rejoinRequested = false;
			}
			if (((startDown && !sOnline->startWasDown) || testRejoin) && !sOnline->rejoinSent) {
				online_send(nnet::MessageType::RejoinRequest, tick, {}, nnet::Delivery::ReliableOrdered);
				sOnline->rejoinSent = true;
			}
			sOnline->startWasDown = startDown;
			p1                    = pc_netplay_input_neutral();
		}
		if (!sOnline->sent.empty() && sOnline->sent.front().tick + 1 != tick) sOnline->sent.clear();
		sOnline->sent.insert(sOnline->sent.begin(), to_net_input(p1, tick));
		if (sOnline->sent.size() > nnet::kMaxInputFrames) sOnline->sent.pop_back();
		online_send(nnet::MessageType::InputBundle, tick, nnet::encodeInputBundle(sOnline->sent),
		            nnet::Delivery::Unreliable);
		sOnline->lastInputTick = tick;
	}
	if (now - sOnline->nextInputMs > 500) sOnline->nextInputMs = now; // after a long load
}

// Exact replica turn: replays the host's ticks at 30 Hz, faster while behind.
int online_guest_replica_ticks(System* sys, BaseApp* app, double now)
{
	int advances = 0;
	if (sOnline->nextSlotMs == 0) sOnline->nextSlotMs = now;
	for (;;) {
		auto it = sOnline->applied.find(sOnline->nextTick);
		if (it == sOnline->applied.end()) {
			// The host's tick is late: counted once per tick, not per turn.
			if (now >= sOnline->nextSlotMs + kOnlineSlotMs && sOnline->stalledTick != sOnline->nextTick) {
				++sOnline->stalls;
				sOnline->stalledTick = sOnline->nextTick;
				sOnline->stallTimes.push_back(now);
				while (!sOnline->stallTimes.empty() && sOnline->stallTimes.front() < now - 10000.0)
					sOnline->stallTimes.erase(sOnline->stallTimes.begin());
			}
			break;
		}
		// Count the ticks already here: far behind (joining, a hitch) means catch up.
		uint64_t ready = 0;
		for (auto r = it; r != sOnline->applied.end() && r->first == sOnline->nextTick + ready && ready <= 91; ++r)
			++ready; // only "how far behind" matters, never the whole history
		// Keep only the buffer the measured jitter needs (1-4 ticks): every
		// extra tick held back is input delay for the guest's own captain.
		const uint64_t keep = std::min<uint64_t>(4, 1 + (2 * sOnline->clock.jitterMs()) / 33);
		const bool behind   = ready > keep;
		// Far behind (joined late): many ticks per turn until close. Every
		// tick is still drawn: skipping the presentation pass is NOT neutral
		// for the sim (item state diverged from tick 12 in the rejoin test).
		const bool fastCatchUp = ready > 90;
		if (fastCatchUp != sOnline->catchingUp) {
			sOnline->catchingUp = fastCatchUp;
			if (fastCatchUp) {
				sOnline->catchUpStartMs   = now;
				sOnline->catchUpStartTick = sOnline->nextTick;
				printf("[online] catching up at tick=%llu\n", (unsigned long long)sOnline->nextTick);
			} else {
				const double seconds = (now - sOnline->catchUpStartMs) / 1000.0;
				const double ticks   = (double)(sOnline->nextTick - sOnline->catchUpStartTick);
				printf("[online] caught up at tick=%llu: %.0f ticks in %.1f s (%.0f ticks/s)\n",
				       (unsigned long long)sOnline->nextTick, ticks, seconds, seconds > 0 ? ticks / seconds : 0.0);
			}
			fflush(stdout);
		}
		if (!behind && now < sOnline->nextSlotMs) break;
		if (advances >= (fastCatchUp ? 120 : behind ? 8 : 2)) break;
		PcNetplayInput p0, p1;
		pc_netplay_input_decode(it->second.data(), 16, p0);
		pc_netplay_input_decode(it->second.data() + 16, 16, p1);
		const uint64_t tick = sOnline->nextTick;
		sOnline->applied.erase(it);
		if (!online_advance(sys, app, tick, p0, p1)) return -1;
		uint64_t total = 0, hashTick = 0;
		if (pc_state_hash_current(&total, nullptr, &hashTick) && hashTick > 0) sOnline->ownHashes[hashTick] = total;
		online_trace_pause(tick);
		sOnline->nextTick = tick + 1;
		sOnline->nextSlotMs += kOnlineSlotMs;
		if (now - sOnline->nextSlotMs > 200) sOnline->nextSlotMs = now;
		++advances;
		if (tick % 150 == 149) {
			// Press-to-screen for the guest: the lead to the host, the trip
			// back and the replay buffer.
			const double latencyMs = (double)(sOnline->lead + sOnline->applied.size()) * kOnlineSlotMs
			                       + sOnline->clock.rttMs() / 2.0;
			printf("[online] tick=%llu replica backlog=%zu stalls=%llu rtt=%u lead=%u input-latency~%.0fms "
			       "hashes-checked=%llu%s\n",
			       (unsigned long long)(tick + 1), sOnline->applied.size(), (unsigned long long)sOnline->stalls,
			       sOnline->clock.rttMs(), sOnline->lead, latencyMs, (unsigned long long)sOnline->hashesChecked,
			       sOnline->desync ? " DESYNC" : "");
			fflush(stdout);
		}
	}
	online_check_hashes();
	return advances;
}

bool online_turn(System* sys, BaseApp* app)
{
	sys->mControllerMgr.update();
	accum_add_current();
	const double now = now_ms();
	int advances     = 0;
	if (sCfg.isHost) {
		online_host_events();
		online_host_backfill();
		if (!sOnline->guestEverJoined && !sOnline->code.empty()) {
			// Waiting for the guest: Ctrl+C copies the code again.
			const Uint8* keys = SDL_GetKeyboardState(nullptr);
			const bool copy   = keys != nullptr && keys[SDL_SCANCODE_C] && (SDL_GetModState() & KMOD_CTRL);
			if (copy && !sOnline->copyKeyDown) sOnline->codeCopied = SDL_SetClipboardText(sOnline->code.c_str()) == 0;
			sOnline->copyKeyDown = copy;
		}
		const uint64_t due = (uint64_t)((now - sOnline->startMs) / kOnlineSlotMs);
		while (sOnline->nextTick <= due && advances < 2) {
			const uint64_t tick = sOnline->nextTick;
			PcNetplayInput p0   = build_local_input();
			for (uint64_t at : sOnline->testStartTicks)
				if (tick == at || tick == at + 1) p0.buttons |= PAD_BUTTON_START;
			// A rejoin waits for gameplay: no pause menu, cutscene or overlay.
			if (sOnline->rejoinPending && pc_online_pause_state() == 0) online_host_set_spectator(false, tick);
			nnet::InputHealth health;
			nnet::NetInputFrame remote = sOnline->receiver.consume(tick, health);
			if (sOnline->spectator) remote = nnet::NetInputFrame{}; // P2 keeps its slot, idle
			if (health == nnet::InputHealth::Live) ++sOnline->live;
			else if (health == nnet::InputHealth::Repeated) ++sOnline->repeated;
			else ++sOnline->neutral;
			const PcNetplayInput p1 = from_net_input(remote);
			// Sent before simulating: the inputs are final, and the last tick
			// of a session must reach the guest even if the session ends in it.
			online_send_applied(tick, p0, p1);
			if (!online_advance(sys, app, tick, p0, p1)) return true;
			online_send_hash(tick);
			online_send_captains(tick);
			if (!sOnline->hostSent.empty() && sOnline->hostSent.front().tick + 1 != tick) sOnline->hostSent.clear();
			sOnline->hostSent.insert(sOnline->hostSent.begin(), to_net_input(p0, tick));
			if (sOnline->hostSent.size() > nnet::kMaxInputFrames) sOnline->hostSent.pop_back();
			online_send(nnet::MessageType::InputBundle, tick, nnet::encodeInputBundle(sOnline->hostSent),
			            nnet::Delivery::Unreliable);
			online_trace_pause(tick);
			sOnline->nextTick = tick + 1;
			++advances;
			if (tick % 150 == 149) online_log(tick + 1);
		}
		// After a long load the host does not race to catch up: it resumes.
		if (due > sOnline->nextTick + 5) sOnline->startMs = now - (double)sOnline->nextTick * kOnlineSlotMs;
	} else {
		online_guest_events();
		// After the host leaves, an exact replica still replays what it holds.
		if (sOnline->peerGone && (sCfg.onlinePredictive || sOnline->applied.empty())) {
			pc_state_hash_flush();
			stop_session();
			sPhase = kDone;
			session_exit(npx::kTransport);
		}
		if (now >= sOnline->nextPingMs) {
			nnet::PingPayload ping;
			ping.id           = ++sOnline->pingId;
			ping.senderTimeMs = (std::uint64_t)now;
			online_send(nnet::MessageType::Ping, sOnline->nextTick, nnet::encodePing(ping), nnet::Delivery::Unreliable);
			sOnline->nextPingMs = now + 250;
		}
		sOnline->lead = nnet::inputLeadTicks(sOnline->clock.rttMs(), sOnline->clock.jitterMs());
		if (!sCfg.onlinePredictive) {
			online_guest_send_input(now);
			const int done = online_guest_replica_ticks(sys, app, now);
			if (done < 0) return true;
			if (sOnline->desync && env_is_one("PIKMIN_ONLINE_EXIT_ON_DESYNC")) {
				pc_state_hash_flush();
				stop_session();
				session_exit(npx::kDesync);
			}
			if (done == 0) sleep_hires_ms(1.0, 0.0);
			return true;
		}
		uint32_t phaseMs    = 0;
		const uint64_t host = sOnline->clock.hostTickAt((std::uint64_t)now, &phaseMs);
		const uint64_t target = host + sOnline->lead;
		const int cap = sOnline->nextTick + 30 < target ? 8 : 2; // catch up fast after joining
		while (sOnline->nextTick <= target && advances < cap) {
			const uint64_t tick = sOnline->nextTick;
			PcNetplayInput p1   = build_local_input();
			if ((sOnline->leaveRequested || (sOnline->testLeaveAt != 0 && tick == sOnline->testLeaveAt))
			    && !sOnline->spectator && !sOnline->leaveSent) {
				online_send(nnet::MessageType::LeaveCoopRequest, tick, {}, nnet::Delivery::ReliableOrdered);
				sOnline->leaveSent      = true;
				sOnline->leaveRequested = false;
			}
			if (sOnline->spectator) {
				const bool startDown = (p1.buttons & PAD_BUTTON_START) != 0;
				const bool testRejoin = sOnline->testRejoinAt != 0 && tick == sOnline->testRejoinAt;
				if (((startDown && !sOnline->startWasDown) || testRejoin) && !sOnline->rejoinSent) {
					online_send(nnet::MessageType::RejoinRequest, tick, {}, nnet::Delivery::ReliableOrdered);
					sOnline->rejoinSent = true;
				}
				sOnline->startWasDown = startDown;
				p1                    = pc_netplay_input_neutral(); // a spectator does not play
			}
			// Redundant input: this tick and up to five before it, consecutive.
			if (!sOnline->sent.empty() && sOnline->sent.front().tick + 1 != tick) sOnline->sent.clear();
			sOnline->sent.insert(sOnline->sent.begin(), to_net_input(p1, tick));
			if (sOnline->sent.size() > nnet::kMaxInputFrames) sOnline->sent.pop_back();
			online_send(nnet::MessageType::InputBundle, tick, nnet::encodeInputBundle(sOnline->sent),
			            nnet::Delivery::Unreliable);
			// P1 plays the host's forwarded input, late by about one way plus
			// the lead; consumed in order even when the delay changes.
			PcNetplayInput p0      = pc_netplay_input_neutral();
			const uint64_t delayTicks = sOnline->lead + (sOnline->clock.rttMs() / 2 + 32) / 33 + 1;
			if (tick >= delayTicks) {
				const uint64_t want = std::max(tick - delayTicks, sOnline->hostInputNext);
				if (want <= tick) {
					nnet::InputHealth hostHealth;
					const nnet::NetInputFrame hostFrame = sOnline->hostInput.consume(want, hostHealth);
					sOnline->hostInputNext             = want + 1;
					if (hostHealth != nnet::InputHealth::Neutral) p0 = from_net_input(hostFrame);
				}
			}
			if (!online_advance(sys, app, tick, p0, p1)) return true;
			online_trace_pause(tick);
			// Own captain: apply any pending correction, then remember the prediction.
			bool snapped           = false;
			const nnet::Vec3 offset = sOnline->corrector.step(snapped);
			if (offset.x != 0 || offset.y != 0 || offset.z != 0 || snapped)
				pc_online_nudge_captain(sLocalRole, offset.x, offset.y, offset.z, snapped);
			PcOnlineCaptain own;
			if (pc_online_read_captain(sLocalRole, &own))
				sOnline->corrector.recordPredicted(tick, { own.position[0], own.position[1], own.position[2] });
			// Host's captain: shown a little in the past, interpolated.
			const uint64_t nowThousandths = host * 1000 + (uint64_t)phaseMs * 30;
			const uint64_t delay          = (uint64_t)sOnline->clock.interpolationDelayMs() * 30;
			nnet::CaptainState shown;
			if (nowThousandths > delay && sOnline->remote.sample(nowThousandths - delay, shown)) {
				PcOnlineCaptain c;
				c.position[0]   = shown.position.x;
				c.position[1]   = shown.position.y;
				c.position[2]   = shown.position.z;
				c.velocity[0]   = shown.velocity.x;
				c.velocity[1]   = shown.velocity.y;
				c.velocity[2]   = shown.velocity.z;
				c.faceDirection = shown.faceDirection;
				pc_online_write_captain(1 - sLocalRole, c);
			}
			sOnline->nextTick = tick + 1;
			++advances;
			if (tick % 150 == 149) online_log(tick + 1);
		}
	}
	if (advances == 0) sleep_hires_ms(1.0, 0.0);
	return true;
}

#endif // PC_ONLINE

void session_setup(System* sys)
{
	pc_netplay_det_force_on();
	pc_settings_netplay_session_begin(nullptr);
	apply_input_selection();
	compute_local_hello();
	if (sCfg.replay) {
		replay_start();
		return;
	}
#if PC_ONLINE
	if (sCfg.online) {
		online_setup();
		netplay_timer_power_opt(true);
		return;
	}
#endif
	if (sCfg.ice) {
		printf("[netplay] mode=%s (ice) delay=%s seed=%u\n", sCfg.isHost ? "host" : "join",
		       sCfg.delayAuto ? "auto" : std::to_string(sCfg.localDelay).c_str(), sCfg.seed);
		printf("[netplay] config text: %s\n", sCfgText.c_str());
		fflush(stdout);
		ice_connect(sys);
		netplay_timer_power_opt(true);
		return;
	}
	printf("[netplay] mode=%s delay=%s seed=%u bind=%s\n", sCfg.isHost ? "host" : "join",
	       sCfg.delayAuto ? "auto" : std::to_string(sCfg.localDelay).c_str(), sCfg.seed,
	       getenv_nonempty("PIKMIN_NETPLAY_UDP_BIND") != nullptr ? getenv_nonempty("PIKMIN_NETPLAY_UDP_BIND")
	                                                             : "127.0.0.1");
	printf("[netplay] config text: %s\n", sCfgText.c_str());
	fflush(stdout);
	sSock = new pc_netplay_transport::UdpSocket();
	if (!sSock->bind(sCfg.isHost ? sCfg.hostPort : 0)) {
		printf("[netplay] bind %s failed\n", sCfg.isHost ? "host port" : "ephemeral port");
		fflush(stdout);
		session_exit(npx::kTransport);
	}
	if (!sCfg.isHost) sSock->set_peer(sCfg.joinIp, sCfg.joinPort);
	if (getenv_nonempty("PIKMIN_NETPLAY_UDP_BIND") == nullptr && (sCfg.isHost || (sCfg.joinIp >> 24) != 127)) {
		printf("[netplay] note: bound to 127.0.0.1 only; for a game over the network set "
		       "PIKMIN_NETPLAY_UDP_BIND=0.0.0.0 on both PCs\n");
	}
	printf("[netplay] udp bound on port %u\n", (unsigned)sSock->local_port());
	fflush(stdout);
	sLink = new pc_netplay_transport::GekkoLink(sSock);
	netplay_timer_power_opt(true);
	(void)sys;
}

} // namespace

bool pc_netplay_is_host(void) { return sCfg.active && sCfg.isHost; }

const char* pc_netplay_session_replay_settings(void)
{
	return sCfg.replay && !sReplay.settings.empty() ? sReplay.settings.c_str() : nullptr;
}

bool pc_netplay_hud_info(PcNetplayHudInfo* out)
{
	if (out == nullptr || !sInitialised || !sCfg.active || sCfg.replay) return false;
	*out           = PcNetplayHudInfo();
	out->isHost    = sCfg.isHost;
	out->inputKind = sCfg.inputKind;
	out->frame     = sLastAdvanceFrame;
#if PC_ONLINE
	if (sCfg.online) {
		if (sOnline == nullptr) return true;
		const double now = now_ms();
		out->running = true;
		out->online  = true;
		if (sCfg.isHost && sCfg.onlineCodeClipboard && !sOnline->guestEverJoined) {
			// The room is open and nothing runs until the guest joins: a
			// banner instead of a black window (no countdown: -1).
			out->banner       = true;
			out->bannerLeftMs = -1;
			snprintf(out->bannerTitle, sizeof out->bannerTitle, "Room open - waiting for your friend");
			auto line = [&](const char* text) {
				if (out->bannerLines < kBannerMaxLines)
					snprintf(out->bannerLine[out->bannerLines++], sizeof out->bannerLine[0], "%s", text);
			};
			line(sOnline->codeCopied ? "Your code (copied; Ctrl+C copies it again). Send it to your friend, who joins "
			                           "from the same menu with Join with a code:"
			                         : "Your code (Ctrl+C copies it). Send it to your friend, who joins from the same "
			                           "menu with Join with a code:");
			// The code has no spaces: break it at its dashes for the banner.
			std::string row;
			size_t at = 0;
			const std::string& code = sOnline->code;
			while (at < code.size() && out->bannerLines < kBannerMaxLines) {
				size_t dash        = code.find('-', at);
				const size_t end   = dash == std::string::npos ? code.size() : dash + 1;
				const std::string group = code.substr(at, end - at);
				if (!row.empty() && row.size() + group.size() > 44) {
					line(row.c_str());
					row.clear();
				}
				row += group;
				at = end;
			}
			if (!row.empty()) line(row.c_str());
			const auto reach = (nnet::Reachability)sOnline->reachability;
			char buf[256];
			if (reach == nnet::Reachability::NoAutomaticMapping) {
				snprintf(buf, sizeof buf,
				         "On the same network it works as is. Over the Internet your router did not open the port by "
				         "itself: forward UDP port %u to this PC.",
				         (unsigned)sOnline->roomPort);
				line(buf);
			} else if (reach == nnet::Reachability::BehindCgnat) {
				line("Your connection is behind CGNAT: a friend on another network may not reach you. On the same "
				     "network it works as is.");
			}
			line("The game is already running: press Start to pause it while you wait.");
		}
		nnet::TransportStats stats;
		nnet::GatedTransport* link = online_link();
		const bool linked = link != nullptr && link->state() == nnet::TransportState::Connected;
		if (!sCfg.isHost && sOnline->clock.ready()) {
			out->numbers.havePing = true;
			out->numbers.pingMs   = (float)sOnline->clock.rttMs();
			out->numbers.jitterMs = (float)sOnline->clock.jitterMs();
			// What the player feels: the lead to the host, the replay buffer
			// and the trip back, in ticks (the HUD shows ticks and ms).
			const double feltMs = (double)(sOnline->lead + sOnline->applied.size()) * kOnlineSlotMs
			                    + sOnline->clock.rttMs() / 2.0;
			out->numbers.delay = (unsigned)((feltMs + kOnlineSlotMs / 2) / kOnlineSlotMs);
		} else if (linked && link->stats(stats) && stats.rttMs >= 0) {
			out->numbers.havePing = true;
			out->numbers.pingMs   = (float)stats.rttMs;
		}
		unsigned recent = 0;
		for (double t : sOnline->stallTimes)
			if (t >= now - 10000.0) ++recent;
		out->numbers.stalls10s = recent;
		auto status = [&](int tone, const char* text) {
			out->statusTone = tone;
			snprintf(out->status, sizeof(out->status), "%s", text);
		};
		if (sCfg.isHost) {
			if (sOnline->startMs == 0) {
				char wait[64];
				const auto reach = (nnet::Reachability)sOnline->reachability;
				const char* copied = sOnline->codeCopied ? "code copied" : "waiting";
				if (reach == nnet::Reachability::NoAutomaticMapping)
					snprintf(wait, sizeof wait, "%s; open UDP port %u on the router", copied, (unsigned)sOnline->roomPort);
				else if (reach == nnet::Reachability::BehindCgnat)
					snprintf(wait, sizeof wait, "%s; CGNAT: the guest may not reach you", copied);
				else
					snprintf(wait, sizeof wait, "%s - waiting for the guest", copied);
				status(reach == nnet::Reachability::NoAutomaticMapping || reach == nnet::Reachability::BehindCgnat ? 1 : 0,
				       wait);
			}
			else if (!linked) status(1, "guest left - P2 idle");
			else if (sOnline->backfillNext < sOnline->backfillEnd) status(0, "guest is catching up");
			else if (sOnline->spectator) status(0, "guest is watching");
		} else {
			if (sOnline->desync) status(2, "desync: the games differ");
			else if (sOnline->peerGone) status(2, "the host left");
			else if (sOnline->catchingUp) status(0, "catching up with the host");
			else if (sOnline->spectator) status(0, "watching - Start to rejoin");
		}
		return true;
	}
#endif
	PcNetplayLiveStats st;
	if (sGekkoStarted && sAdvances > 0 && pc_netplay_live_stats(&st)) {
		out->running             = true;
		out->numbers.havePing    = st.rttP50Ms >= 0;
		out->numbers.pingMs      = st.rttP50Ms;
		out->numbers.jitterMs    = st.jitterMs;
		out->numbers.delay       = st.delay;
		out->numbers.stalls10s   = st.stallsLast10s;
		out->numbers.stallMs10s  = st.stallMsLast10s;
	}
	if (sBannerOn) {
		out->banner       = true;
		out->bannerError  = sBannerError;
		out->bannerLeftMs = sBannerMs - (now_ms() - sBannerStartMs);
		snprintf(out->bannerTitle, sizeof(out->bannerTitle), "%s", sBannerTitle.c_str());
		for (const std::string& l : sBannerLines) {
			if (out->bannerLines >= kBannerMaxLines) break;
			snprintf(out->bannerLine[out->bannerLines++], sizeof(out->bannerLine[0]), "%s", l.c_str());
		}
	}
	return true;
}

bool pc_netplay_live_stats(PcNetplayLiveStats* out)
{
	if (out == nullptr || sPhase != kSession || !sAdaptiveConfigured) return false;
	uint64_t n10     = 0;
	double ms10      = 0;
	const double now = now_ms();
	sStats.recent(now, 10000.0, &n10, &ms10);
	out->delay          = sCfg.localDelay;
	out->adaptive       = sAdaptive ? 1 : 0;
	out->rttLastMs      = (float)sStats.rtt_last();
	out->rttP50Ms       = (float)sStats.rtt_percentile(50);
	out->jitterMs       = (float)sStats.rtt_jitter();
	out->stallsLast10s  = (unsigned)n10;
	out->stallMsLast10s = (float)ms10;
	out->stallCount     = (unsigned long long)sStats.stall_count();
	out->stallTotalMs   = (float)sStats.stall_total_ms();
	out->stallOpen      = sStallOpen ? 1 : 0;
	return true;
}

void pc_netplay_session_notify_argv(int argc, char** argv)
{
	sArgc = argc;
	sArgv = argv;
	parse_config();
	sInitialised = true;
}

// Stage-load hook (GameFlow::softReset, inside the synchronous load that
// runs within app->idle() during one Advance). PIKMIN_NETPLAY_TEST_LOAD_DELAY_MS
// sleeps once, unpumped, to exercise the load window alone.
void pc_netplay_on_stage_load(void)
{
	if (!sInitialised || !sCfg.active) return;
	loadguard_on_stage_load();
	if (sLoadDelayDone) return;
	const unsigned n = read_unsigned_env("PIKMIN_NETPLAY_TEST_LOAD_DELAY_MS", 0);
	if (n == 0) return;
	sLoadDelayDone = true;
	printf("[netplay] test load delay: sleeping %ums inside stage load\n", n);
	fflush(stdout);
	std::this_thread::sleep_for(std::chrono::milliseconds(n));
	printf("[netplay] test load delay: done\n");
	fflush(stdout);
}

void pc_netplay_load_keepalive(int site)
{
	if (!sInAdvance) return;
	if (std::this_thread::get_id() != sLgMainThread) return; // the DVD, audio and loading threads
	if (site == pc_netplay_loadguard::kSiteShader) loadguard_maybe_stall(pc_netplay_loadguard::StallAt::Shader, 1, "shader");
	loadguard_keepalive(site);
}

bool pc_netplay_session_active(void) { return sInitialised && sCfg.active; }

bool pc_netplay_session_drive(System* sys, BaseApp* app)
{
	if (!pc_netplay_session_active()) return false;
	if (sys == nullptr || app == nullptr) return false;

	if (!sForcedModes) {
		sForcedModes = true;
		session_setup(sys);
	}

	if (pc_window_should_close()) {
		// Relinquish the loop so System::run breaks at its should_close
		// check and shuts down normally.
		if (sPhase != kDone) {
			const bool inSession = sPhase == kSession && sGekko != nullptr && sGekkoStarted;
			if (inSession) send_quit_notice();
			pc_state_hash_flush();
			if (inSession) inlog_end("local quit frame=" + std::to_string(sLastAdvanceFrame));
			stop_session();
			sPhase = kDone;
		}
		if (sBannerOn && sBannerExit >= 0) {
			fflush(stdout);
			session_exit(sBannerExit); // closed during a desync banner: keep its exit code
		}
		return false;
	}

	if (sPhase == kDone) {
		if (sBannerOn) {
			// The end banner, between ticks (no sim runs): one presented
			// frame per turn at about 30 Hz until it times out or any key or
			// button is pressed.
			int kind = 0, pad = 0;
			const bool dismissed = pc_window_take_button_press(&kind, &pad);
			const double shown   = now_ms() - sBannerStartMs;
			if (dismissed || shown >= sBannerMs) {
				sBannerOn = false;
				printf("[netplay] end banner: closed after %.0f ms (%s)\n", shown, dismissed ? "key" : "timeout");
				fflush(stdout);
				if (sBannerExit >= 0) session_exit(sBannerExit);
				request_quit();
				return true;
			}
			sys->beginRender();
			sys->doneRender();
			sys->waitRetrace(); // overlays (the banner), present, swap, window events
			sleep_hires_ms(33.0);
			return true;
		}
		// Drain the quit event requested above (nothing else polls while the
		// driver owns the loop), then relinquish so the loop breaks.
		pc_window_poll_events(nullptr);
		if (pc_window_should_close()) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return true;
	}

#if PC_ONLINE
	if (sCfg.online) {
		if (sPhase == kHandshake) {
			sys->mControllerMgr.update();
			if (online_connect_pump()) online_start();
			else sleep_hires_ms(1.0, 0.0);
			return true;
		}
		return online_turn(sys, app);
	}
#endif
	if (sPhase == kHandshake) {
		// mControllerMgr.update() pumps the window through PADRead. Poll
		// about every 1 ms on the waitable timer, so the RTT samples measure
		// the wire, not the poll phase, and a waiting host idles.
		sys->mControllerMgr.update();
		if (handshake_pump()) start_gekko_session();
		if (sPhase == kHandshake) sleep_hires_ms(1.0, 0.0);
		return true;
	}

	// kSession.
	if (sCfg.replay) return replay_turn(sys, app);
	const bool unthrottled   = pc_netplay_unthrottled();
	const double turnStartMs = now_ms();
	adaptive_note_turn_start(turnStartMs);
	sys->mControllerMgr.update();
	int advances = 0;
	if (sGekko != nullptr) {
		answer_handshake_in_session();
		accum_add_current();
		// GekkoNet accepts exactly one local input per frame, so a submit is
		// due only when the next local input lands on sAdvances + delay. On
		// a due turn the adaptive delay may shrink the delay (skipping this
		// and the next submits) or grow it (one input per new frame now).
		if (sGekkoStarted) {
			unsigned grow = 0;
			if (pc_netplay_adaptive::submit_due(sNextLand, sAdvances, sCfg.localDelay)) {
				const unsigned target = adaptive_target();
				if (target < sCfg.localDelay) adaptive_shrink(target);
				else grow = target - sCfg.localDelay;
			}
			if (pc_netplay_adaptive::submit_due(sNextLand, sAdvances, sCfg.localDelay)) submit_local_inputs(1 + grow);
		}
		advances = handle_game_events(sys, app);
		handle_session_events();
		if (sPhase != kSession) return true; // stopped inside the handlers
	}
	// Pacing (see the top of the file). At most 2 slots of catch-up per turn,
	// and a snap forward when more than 5 slots behind (a long load).
	double nextDueMs = 0;
	bool haveDue     = false;
	if (!unthrottled) {
		constexpr double kSlotMs          = 1000.0 / 30.0;
		constexpr double kMaxCatchupSlots = 5.0;
		constexpr double kMaxAdvanceSlots = 2.0;
		if (advances > 0) {
			const float aheadNow = (sGekko != nullptr) ? gekko_frames_ahead(sGekko) : 0.0f;
			double extraMs       = 0.0;
			if (aheadNow > 0.75f) {
				extraMs          = (double)(aheadNow - 0.75f) * kSlotMs * 0.05;
				const double cap = kSlotMs * 0.10;
				if (extraMs > cap) extraMs = cap;
			}
			double effAdv = (double)advances;
			if (effAdv > kMaxAdvanceSlots) effAdv = kMaxAdvanceSlots;
			const double now = now_ms();
			if (sNextTurnMs == 0) sNextTurnMs = now + kSlotMs;
			nextDueMs = sNextTurnMs;
			haveDue   = true;
			if (now < sNextTurnMs) {
				const double wait = sNextTurnMs - now;
				if (wait > 0.05) sleep_hires_ms(wait);
				sNextTurnMs += kSlotMs * effAdv + extraMs;
			} else if (now - sNextTurnMs > kMaxCatchupSlots * kSlotMs) {
				sNextTurnMs = now + kSlotMs;
				nextDueMs   = now;
			} else {
				sNextTurnMs += kSlotMs * effAdv + extraMs;
			}
		} else {
			// A waiting turn keeps pumping without spending a whole slot.
			sleep_hires_ms(1.0, 0.0);
		}
	}
	if (advances > 0) {
		sLastAdvanceWallMs = now_ms();
		adaptive_close_stall();
	} else {
		++sStalls;
		const double turnMs = now_ms() - turnStartMs;
		sStallMs += turnMs;
		adaptive_note_stall_turn(turnStartMs, turnMs);
	}
	adaptive_note_turn_end(advances > 0, haveDue, nextDueMs);
	adaptive_poll(now_ms());
	return true;
}

// Online guest: joins before the settings are read and returns the host's
// sim settings to adopt (pc_netplay_launch_post_settings). Null otherwise.
const char* pc_netplay_session_online_rules(void)
{
#if PC_ONLINE
	if (!sInitialised || !sCfg.online || sCfg.isHost) return nullptr;
	online_setup();
	const double deadline = now_ms() + 30000.0;
	while (!sOnline->rulesSeen) {
		online_guest_events();
		// Coming back after a crash, the host may still hold the old
		// connection until it times out: keep knocking for a while.
		if (sOnline->peerGone && sOnline->join->failure() == nnet::JoinFailure::RoomFull && now_ms() < deadline) {
			printf("[online] the room is still full; retrying\n");
			fflush(stdout);
			std::this_thread::sleep_for(std::chrono::milliseconds(2000));
			sOnline->peerGone = false;
			if (sOnline->join->start(sCfg.onlineCode, online_compat(), kOnlineProtocolMajor, (std::uint64_t)time(nullptr),
			                         (std::uint64_t)now_ms())
			    != nnet::JoinFailure::None)
				online_join_failed(join_failure_text(sOnline->join->failure()));
			continue;
		}
		if (sOnline->peerGone) online_join_failed(join_failure_text(sOnline->join->failure()));
		if (now_ms() > deadline) online_join_failed("Your friend's game did not answer in time: try again.");
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	printf("[online] adopting the host's rules (%zu B)\n", sOnline->rules.size());
	fflush(stdout);
	return sOnline->rules.c_str();
#else
	return nullptr;
#endif
}

// UI entry points for the online guest: leave co-op (keep the slot and
// watch the host) and query the state. Start asks to rejoin.
void pc_netplay_online_request_leave(void)
{
#if PC_ONLINE
	if (sOnline != nullptr && !sCfg.isHost) sOnline->leaveRequested = true;
#endif
}

bool pc_netplay_online_spectating(void)
{
#if PC_ONLINE
	return sOnline != nullptr && sOnline->spectator;
#else
	return false;
#endif
}

const char* pc_netplay_online_take_error(void)
{
	static std::string taken;
	taken = sOnlineError;
	sOnlineError.clear();
	return taken.empty() ? nullptr : taken.c_str();
}

bool pc_netplay_online_is_guest(void)
{
#if PC_ONLINE
	return sInitialised && sCfg.online && !sCfg.isHost && sOnline != nullptr;
#else
	return false;
#endif
}

void pc_netplay_online_request_rejoin(void)
{
#if PC_ONLINE
	if (sOnline != nullptr && !sCfg.isHost) sOnline->rejoinRequested = true;
#endif
}
