#pragma once
// 3DS-only game-thread timers (reported as [prof] lines by n3ds_port/platform/n3ds_window.cpp). No-ops elsewhere.
#if defined(__3DS__)
extern "C" void n3ds_prof_begin(const char* group, const char* name);
extern "C" void n3ds_prof_end(void);
struct N3dsProfScope {
	N3dsProfScope(const char* g, const char* n) { n3ds_prof_begin(g, n); }
	~N3dsProfScope() { n3ds_prof_end(); }
};
#define N3DS_PROF_CAT2(a, b) a##b
#define N3DS_PROF_CAT(a, b)  N3DS_PROF_CAT2(a, b)
#define N3DS_PROF(group, name) N3dsProfScope N3DS_PROF_CAT(n3dsProf_, __LINE__)(group, name)
#else
#define N3DS_PROF(group, name) ((void)0)
#endif
