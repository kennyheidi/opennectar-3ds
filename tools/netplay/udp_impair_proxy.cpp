// Loopback UDP impairment proxy for netplay tests.
//
// Sits between the two netplay peers: the joiner connects to --listen, the
// proxy forwards every datagram to the host at --upstream and back, applying
// the pc_netplay_impair.h model (latency or a latency schedule, uniform
// jitter, loss, periodic link blocks) to both directions. The model lives
// outside the game, so any executable sees the same link. Test tool only: it is
// built in netplay builds as netplay_impair_proxy and never ships.
//
// Usage:
//   netplay_impair_proxy --listen 48811 --upstream 48810
//     [--bind 127.0.0.1] [--lat MS] [--jit MS] [--loss PCT]
//     [--sched <sec>:<ms>,...] [--spikes <everyMs>:<minMs>:<maxMs>]
//     [--seed N] [--run-seconds N]
// Prints "[impair] listening ..." once ready, one "[impair] block" line per
// link block, a status line every 10 s and a summary at exit. Exits 0 on
// --run-seconds expiry (default 3600).

#include "netplay/pc_netplay_impair.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <chrono>
#include <map>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <synchapi.h>
#endif

namespace {

const char* flag_value(int argc, char** argv, const char* flag)
{
	for (int i = 1; i + 1 < argc; ++i) {
		if (argv[i] != nullptr && strcmp(argv[i], flag) == 0) return argv[i + 1];
	}
	return nullptr;
}

double flag_double(int argc, char** argv, const char* flag, double fallback)
{
	const char* v = flag_value(argc, argv, flag);
	if (v == nullptr) return fallback;
	char* end = nullptr;
	const double d = strtod(v, &end);
	return (end != v && *end == '\0') ? d : fallback;
}

double now_ms()
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct Pkt {
	int dir = 0; // 0: client -> upstream, 1: upstream -> client
	std::vector<char> bytes;
};

} // namespace

int main(int argc, char** argv)
{
#ifndef _WIN32
	fprintf(stderr, "[impair] Windows only\n");
	return 2;
#else
	const char* listenStr = flag_value(argc, argv, "--listen");
	const char* upStr = flag_value(argc, argv, "--upstream");
	if (listenStr == nullptr || upStr == nullptr) {
		fprintf(stderr, "usage: netplay_impair_proxy --listen PORT --upstream PORT [--bind IP] [--lat MS] "
		                "[--jit MS] [--loss PCT] [--sched s:ms,...] [--spikes every:min:max] [--seed N] "
		                "[--run-seconds N]\n");
		return 2;
	}
	const unsigned listenPort = (unsigned)strtoul(listenStr, nullptr, 10);
	const unsigned upPort = (unsigned)strtoul(upStr, nullptr, 10);
	const char* bindIp = flag_value(argc, argv, "--bind");
	if (bindIp == nullptr) bindIp = "127.0.0.1";
	const double latMs = flag_double(argc, argv, "--lat", 0.0);
	const double jitMs = flag_double(argc, argv, "--jit", 0.0);
	const double lossPct = flag_double(argc, argv, "--loss", 0.0);
	const double runS = flag_double(argc, argv, "--run-seconds", 3600.0);
	const uint32_t seed = (uint32_t)flag_double(argc, argv, "--seed", 1.0);
	std::vector<pc_netplay_impair::Point> sched;
	pc_netplay_impair::SpikeSpec spikes;
	std::string err;
	if (!pc_netplay_impair::parse_schedule(flag_value(argc, argv, "--sched"), &sched, &err)) {
		fprintf(stderr, "[impair] bad --sched: %s\n", err.c_str());
		return 2;
	}
	if (!pc_netplay_impair::parse_spikes(flag_value(argc, argv, "--spikes"), &spikes, &err)) {
		fprintf(stderr, "[impair] bad --spikes: %s\n", err.c_str());
		return 2;
	}

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		fprintf(stderr, "[impair] WSAStartup failed\n");
		return 1;
	}
	SOCKET ls = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	SOCKET us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (ls == INVALID_SOCKET || us == INVALID_SOCKET) {
		fprintf(stderr, "[impair] socket failed\n");
		return 1;
	}
	sockaddr_in la;
	memset(&la, 0, sizeof(la));
	la.sin_family = AF_INET;
	la.sin_port = htons((u_short)listenPort);
	inet_pton(AF_INET, bindIp, &la.sin_addr);
	if (bind(ls, (sockaddr*)&la, sizeof(la)) != 0) {
		fprintf(stderr, "[impair] bind %s:%u failed (%d)\n", bindIp, listenPort, WSAGetLastError());
		return 1;
	}
	sockaddr_in ua;
	memset(&ua, 0, sizeof(ua));
	ua.sin_family = AF_INET;
	ua.sin_port = 0;
	inet_pton(AF_INET, bindIp, &ua.sin_addr);
	if (bind(us, (sockaddr*)&ua, sizeof(ua)) != 0) {
		fprintf(stderr, "[impair] bind upstream side failed (%d)\n", WSAGetLastError());
		return 1;
	}
	sockaddr_in host;
	memset(&host, 0, sizeof(host));
	host.sin_family = AF_INET;
	host.sin_port = htons((u_short)upPort);
	inet_pton(AF_INET, bindIp, &host.sin_addr);
	// Large buffers: a link block holds every datagram of its span.
	int buf = 4 << 20;
	setsockopt(ls, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof(buf));
	setsockopt(us, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof(buf));
	setsockopt(ls, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof(buf));
	setsockopt(us, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof(buf));
	WSAEVENT evs[3];
	evs[0] = WSACreateEvent();
	evs[1] = WSACreateEvent();
	WSAEventSelect(ls, evs[0], FD_READ);
	WSAEventSelect(us, evs[1], FD_READ);
	HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	if (timer == NULL) {
		timer = CreateWaitableTimerW(NULL, FALSE, NULL);
		timeBeginPeriod(1);
	}
	evs[2] = timer;

	pc_netplay_impair::Blocks blocks;
	blocks.configure(spikes, seed);
	std::mt19937 rng(seed);
	std::uniform_real_distribution<double> uni(0.0, 1.0);
	std::multimap<double, Pkt> queue; // deliverAt -> datagram (FIFO for equal keys)
	sockaddr_in client;
	memset(&client, 0, sizeof(client));
	bool haveClient = false;
	uint64_t in[2] = { 0, 0 }, out[2] = { 0, 0 }, dropped[2] = { 0, 0 };
	double maxHold = 0;
	const double t0 = now_ms();
	double nextStatus = t0 + 10000.0;
	printf("[impair] listening %s:%u -> upstream %s:%u lat=%.1f jit=%.1f loss=%.2f%% sched=%zu points "
	       "spikes=%.0f:%.0f:%.0f seed=%u run=%.0fs\n",
	       bindIp, listenPort, bindIp, upPort, latMs, jitMs, lossPct, sched.size(), spikes.everyMs, spikes.minMs,
	       spikes.maxMs, seed, runS);
	fflush(stdout);

	char data[65536];
	for (;;) {
		const double now = now_ms();
		const double t = now - t0;
		if (t >= runS * 1000.0) break;
		// Receive everything pending on both sides.
		for (int side = 0; side < 2; ++side) {
			SOCKET s = side == 0 ? ls : us;
			for (;;) {
				sockaddr_in from;
				int fromLen = sizeof(from);
				const int n = recvfrom(s, data, sizeof(data), 0, (sockaddr*)&from, &fromLen);
				if (n <= 0) break;
				if (side == 0) {
					client = from;
					haveClient = true;
				} else if (from.sin_port != host.sin_port || from.sin_addr.s_addr != host.sin_addr.s_addr) {
					continue; // only the host talks to the upstream side
				}
				++in[side];
				if (lossPct > 0 && uni(rng) * 100.0 < lossPct) {
					++dropped[side];
					continue;
				}
				double hold = pc_netplay_impair::schedule_ms(sched, t / 1000.0, latMs);
				if (jitMs > 0) hold += uni(rng) * jitMs;
				Pkt p;
				p.dir = side;
				p.bytes.assign(data, data + n);
				queue.emplace(now + hold, std::move(p));
			}
		}
		// Deliver what is due, unless the link is blocked.
		double blockEnd = 0;
		bool entered = false;
		const bool isBlocked = blocks.blocked(t, &blockEnd, &entered);
		if (entered) {
			printf("[impair] block #%llu at t=%.1fs for %.0fms\n", (unsigned long long)blocks.count(), t / 1000.0,
			       blocks.end_ms() - blocks.start_ms());
			fflush(stdout);
		}
		if (!isBlocked) {
			while (!queue.empty() && queue.begin()->first <= now) {
				Pkt& p = queue.begin()->second;
				if (p.dir == 0) {
					sendto(us, p.bytes.data(), (int)p.bytes.size(), 0, (const sockaddr*)&host, sizeof(host));
					++out[0];
				} else if (haveClient) {
					sendto(ls, p.bytes.data(), (int)p.bytes.size(), 0, (const sockaddr*)&client, sizeof(client));
					++out[1];
				}
				queue.erase(queue.begin());
			}
		} else if (!queue.empty()) {
			const double late = (t0 + blockEnd) - queue.begin()->first; // queue keys are absolute
			if (late > maxHold) maxHold = late;
		}
		if (now >= nextStatus) {
			nextStatus += 10000.0;
			printf("[impair] t=%.1fs base=%.1fms c2h in=%llu out=%llu drop=%llu h2c in=%llu out=%llu drop=%llu "
			       "blocks=%llu queued=%zu\n",
			       t / 1000.0, pc_netplay_impair::schedule_ms(sched, t / 1000.0, latMs), (unsigned long long)in[0],
			       (unsigned long long)out[0], (unsigned long long)dropped[0], (unsigned long long)in[1],
			       (unsigned long long)out[1], (unsigned long long)dropped[1], (unsigned long long)blocks.count(),
			       queue.size());
			fflush(stdout);
		}
		// Wait for a datagram or the next due / block-end time (at most 5 ms).
		double waitMs = 5.0;
		if (isBlocked) waitMs = blockEnd - t;
		else if (!queue.empty()) waitMs = queue.begin()->first - now_ms();
		if (waitMs > 5.0) waitMs = 5.0;
		if (waitMs < 0.05) continue;
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)(waitMs * 10000.0);
		SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
		// The socket events are manual-reset: clear the one that woke us (the
		// receive loop at the top drains everything that is pending anyway).
		const DWORD r = WaitForMultipleObjects(3, evs, FALSE, 50);
		if (r == WAIT_OBJECT_0) WSAResetEvent(evs[0]);
		else if (r == WAIT_OBJECT_0 + 1) WSAResetEvent(evs[1]);
	}
	printf("[impair] done t=%.1fs c2h in=%llu out=%llu drop=%llu h2c in=%llu out=%llu drop=%llu blocks=%llu "
	       "max-block-hold=%.0fms\n",
	       (now_ms() - t0) / 1000.0, (unsigned long long)in[0], (unsigned long long)out[0],
	       (unsigned long long)dropped[0], (unsigned long long)in[1], (unsigned long long)out[1],
	       (unsigned long long)dropped[1], (unsigned long long)blocks.count(), maxHold);
	fflush(stdout);
	closesocket(ls);
	closesocket(us);
	WSACleanup();
	return 0;
#endif
}
