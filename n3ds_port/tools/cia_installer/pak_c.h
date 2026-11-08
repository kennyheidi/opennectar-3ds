// C view of tools/pakbuild/pak_builder.h for the installer (main.c is C, the pak builder is C++). Implemented in pak_c.cpp.
#ifndef NECTAR_INSTALLER_PAK_C_H
#define NECTAR_INSTALLER_PAK_C_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned listed, done, packed, skippedBig, skippedExt, skippedErr;
    unsigned long long bytes;
    const char* current;
} NpProgress;

typedef int (*NpProgressFn)(const NpProgress* p, void* user);   // return 0 to cancel

typedef struct {
    int ok;
    char error[256];
    NpProgress stats;
    char firstSkipped[256];
} NpResult;

// Builds <base>/nectar.pak from <base>/assets/dataDir (files over maxFileBytes stay loose). See pak_build() in pak_builder.h.
void np_build(const char* base, unsigned long long maxFileBytes, NpProgressFn fn, void* user, NpResult* out);
// Compares <base>/nectar.pak with the loose files. See pak_verify() in pak_builder.h.
void np_verify(const char* base, NpProgressFn fn, void* user, NpResult* out);

#ifdef __cplusplus
}
#endif
#endif
