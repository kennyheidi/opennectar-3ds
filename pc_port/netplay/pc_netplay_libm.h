#pragma once

// Deterministic software libm for netplay builds.
//
// This header only declares the self-test hooks. The math entry points
// themselves (sinf, cosf, sincosf, tanf, atanf, atan2f, asinf, acosf, expf,
// logf, powf, fmodf, hypotf, sqrtf, log10f/log2f/exp2f and the double versions)
// are plain C symbols defined in pc_netplay_libm.c. That TU is linked into
// the game executable only when PIKMIN_NETPLAY_BUILD=ON, so the default
// build keeps using the mingw math library untouched.
//
// Test hooks below let the host-run ctest verify which implementation the
// test binary linked (netplay TU present or not).

#ifdef __cplusplus
extern "C" {
#endif

// 1 when pc_netplay_libm.c is linked into this binary, else 0.
int pc_netplay_libm_present(void);

// ABI version of the deterministic libm (bumped on any bit-exact change).
// The test logs it next to the golden checksum it commits.
unsigned long long pc_netplay_libm_abi_version(void);

#ifdef __cplusplus
}
#endif
