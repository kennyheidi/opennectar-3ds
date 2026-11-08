// Builds nectar.pak (format v1, see platform/n3ds_pak.cpp and tools/make_pak.py) from <base>/assets/dataDir - ONLY what the game opens
// through its DVD layer (every DVDOpen path starts with dataDir/). dataDir/SndData is left out: the audio code reads it directly.
// Plain C stdio + dirent, so the same code runs in the 3DS app (tools/pakbuild/main.cpp) and in the PC host test.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct PakProgress {
    unsigned listed;          // files found under assets/ (after name/extension filters)
    unsigned done;            // files processed so far (packed or skipped for size)
    unsigned packed;          // files written into the pak
    unsigned skippedBig;      // files over the size limit (stay loose)
    unsigned skippedExt;      // .stx/.thp/.pak and system junk files (stay loose)
    unsigned skippedErr;      // files that could not be opened/read (stay loose; listed in the result's firstSkipped)
    uint64_t bytes;           // pak size so far
    const char* current;      // file being packed (key form, "assets/...")
};

// Return false to cancel. Called about every file during packing, and every 64 files while listing (done == 0).
typedef bool (*PakProgressFn)(const PakProgress& p, void* user);

struct PakResult {
    bool ok;
    char error[256];
    PakProgress stats;
    char firstSkipped[256];   // first file that could not be read (empty if none)
};

// base: folder that contains assets/ (e.g. "sdmc:/3ds/nectar"). Writes <base>/nectar.pak via <base>/nectar.pak.tmp (renamed at the end; a
// cancelled or failed build leaves the old pak untouched and removes the .tmp). maxFileBytes: bigger files are not packed (default 2 MB).
PakResult pak_build(const char* base, uint64_t maxFileBytes, PakProgressFn fn, void* user);

// Checks <base>/nectar.pak against the loose files: every packed file's size and bytes. ok = no difference found. stats.packed = files
// compared, stats.skippedErr = files that differ (or are missing loose), firstSkipped = the first such file. Same progress/cancel callback.
PakResult pak_verify(const char* base, PakProgressFn fn, void* user);

uint64_t pak_key_hash(const char* key);   // FNV-1a 64 of the normalised key (lower case, '/' separators)
