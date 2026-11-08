// Netplay-only backtrace helper. See
// pc_netplay_diag.h. Kept in its own TU so <windows.h> never meets the game
// headers (DebugLog.h redefines ERROR).

#include "netplay/pc_netplay_diag.h"

#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

void pc_netplay_diag_backtrace(const char* tag)
{
#if defined(_WIN32)
	void* frames[32];
	USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
	// The loader rewrites the in-memory ImageBase to the ASLR load address, so
	// this prints the load base; rebase with `addr - load + preferred` (the
	// exe's on-disk ImageBase, 0x140000000 for MinGW x64) before `nm -C`.
	HMODULE base = GetModuleHandleA(nullptr);
	std::fprintf(stderr, "[netplay-diag] %s backtrace (%u frames, load base %p)\n", tag, (unsigned)count, (void*)base);
	for (USHORT i = 0; i < count; i++) {
		std::fprintf(stderr, "[netplay-diag] %s bt#%02u %p\n", tag, (unsigned)i, frames[i]);
	}
#else
	std::fprintf(stderr, "[netplay-diag] %s backtrace unavailable on this platform\n", tag);
#endif
	std::fflush(stderr);
}
