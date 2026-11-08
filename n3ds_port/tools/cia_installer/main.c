// Nectar CIA installer + setup guide.
//
// Launch it from the HOME menu. The main screen checks the SD card (New 3DS?, game files in sdmc:/3ds/nectar/assets/dataDir,
// nectar.ini, nectar.pak, a game .cia in the SD root, what is installed) and says what to do next; Y opens a step-by-step setup
// guide (guide.h) for people setting the game up for the first time; R builds / checks nectar.pak (the same builder as
// nectar_pakbuild.3dsx, tools/pakbuild/pak_builder.cpp, through pak_c.cpp); A installs the newest *nectar*.cia from the ROOT of the SD
// card (sdmc:/nectar.cia, sdmc:/nectar_test.cia, ... not one with "install" in its name) to the SD title storage with the AM service.
//
// Needs custom firmware (Luma3DS) like every CIA installer: the process talks to am:net, which the exheader (installer.rsf) lists.
// Same technique as FBI: AM_StartCiaInstall -> FSFILE_Write the file in chunks -> AM_FinishCiaInstall.

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "guide.h"
#include "pak_c.h"

#define CHUNK_BIG   (256 * 1024)   /* same block size FBI uses */
#define CHUNK_SMALL (32 * 1024)    /* second attempt if the first one is refused */

#define NECTAR_HOME "sdmc:/3ds/nectar"
#define TOP_W 49                   /* top console is 50 columns; one less so a full line never wraps by itself */
#define BOT_W 39                   /* bottom console is 40 columns */

/* ANSI colours (libctru console) */
#define C_RESET  "\x1b[0m"
#define C_GREEN  "\x1b[32;1m"
#define C_RED    "\x1b[31;1m"
#define C_YELLOW "\x1b[33;1m"
#define C_CYAN   "\x1b[36;1m"
#define C_DIM    "\x1b[37m"

static PrintConsole sTop, sBot;

static const u64 kTitleMain = 0x000400000EC40000ULL;   /* Open Nectar        (UniqueId 0xEC400) */
static const u64 kTitleTest = 0x000400000EC40200ULL;   /* Open Nectar TEST   (UniqueId 0xEC402) */

static void present(void)
{
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static int contains_ci(const char* hay, const char* needle)
{
    const size_t hl = strlen(hay), nl = strlen(needle);
    if (nl > hl) return 0;
    for (size_t i = 0; i + nl <= hl; ++i) {
        size_t k = 0;
        while (k < nl && lower(hay[i + k]) == needle[k]) ++k;
        if (k == nl) return 1;
    }
    return 0;
}

static int ends_with_cia(const char* name)
{
    const size_t n = strlen(name);
    return n > 4 && name[n - 4] == '.' && lower(name[n - 3]) == 'c' && lower(name[n - 2]) == 'i' && lower(name[n - 1]) == 'a';
}

static int is_dir(const char* p)  { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int is_file(const char* p) { struct stat st; return stat(p, &st) == 0 && !S_ISDIR(st.st_mode); }

// Newest nectar*.cia in the root of the SD card. 1 = found (path/size filled), 0 = none, -1 = the SD card root cannot be read.
static int find_cia(char* path, size_t cap, u64* size, int* totalCias)
{
    DIR* d = opendir("sdmc:/");
    if (!d) return -1;
    int found = 0;
    time_t bestTime = 0;
    *totalCias = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        if (n[0] == '.') continue;                       // ".", "..", macOS "._" files
        if (!ends_with_cia(n)) continue;
        ++*totalCias;
        if (contains_ci(n, "install")) continue;         // never pick an installer (this one included)
        if (!contains_ci(n, "nectar")) continue;
        char full[300];
        snprintf(full, sizeof full, "sdmc:/%s", n);
        struct stat st;
        if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) continue;
        if (!found || st.st_mtime > bestTime) {
            found = 1;
            bestTime = st.st_mtime;
            snprintf(path, cap, "%s", full);
            *size = (u64)st.st_size;
        }
    }
    closedir(d);
    return found;
}

// Title ID of the CIA, read from its TMD (needed to remove an installed copy first, like FBI does). 0 = could not parse.
static u64 cia_title_id(FILE* f)
{
    u8 h[0x20];
    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, sizeof h, f) != sizeof h) return 0;
    const u32 hdr   = h[0] | (h[1] << 8) | (h[2] << 16) | ((u32)h[3] << 24);
    const u32 certs = h[8] | (h[9] << 8) | (h[10] << 16) | ((u32)h[11] << 24);
    const u32 tik   = h[12] | (h[13] << 8) | (h[14] << 16) | ((u32)h[15] << 24);
    if (hdr != 0x2020 || certs > 0x10000 || tik > 0x10000) return 0;
    const u32 tmdOff = (((((hdr + 63) & ~63u) + certs + 63) & ~63u) + tik + 63) & ~63u;
    u8 sig[4];
    if (fseek(f, (long)tmdOff, SEEK_SET) != 0 || fread(sig, 1, 4, f) != 4) return 0;
    const u32 type = ((u32)sig[0] << 24) | (sig[1] << 16) | (sig[2] << 8) | sig[3];   // big endian signature type
    u32 sigSize;
    switch (type) {
        case 0x010000: case 0x010003: sigSize = 0x200 + 0x3C; break;   // RSA-4096
        case 0x010001: case 0x010004: sigSize = 0x100 + 0x3C; break;   // RSA-2048
        case 0x010002: case 0x010005: sigSize = 0x3C + 0x40;  break;   // ECDSA
        default: return 0;
    }
    u8 id[8];
    if (fseek(f, (long)(tmdOff + 4 + sigSize + 0x4C), SEEK_SET) != 0 || fread(id, 1, 8, f) != 8) return 0;
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | id[i];   // big endian
    return v;
}

// =====================================================================================================================  SD check
enum AssetState {
    AS_OK,           // sdmc:/3ds/nectar/assets/dataDir with the key files
    AS_INCOMPLETE,   // dataDir is there but a key file is missing (copy stopped early, or the wrong folder)
    AS_MISPLACED,    // a dataDir exists somewhere else on the SD card (assets/files/dataDir, 3ds/nectar/dataDir, ...)
    AS_FLAT,         // the CONTENTS of dataDir were copied straight into assets (assets/parms, assets/SndData)
    AS_EMPTY,        // assets folder exists, nothing usable in it
    AS_MISSING,      // no sdmc:/3ds/nectar/assets at all
};

typedef struct {
    int  sdOk;                 // SD card root readable
    int  isNew3ds;
    int  assets;               // AssetState
    char assetsWhere[160];     // AS_MISPLACED: the folder that HOLDS the stray dataDir
    char assetsMissing[64];    // AS_INCOMPLETE: first missing key file
    int  ini, iniInRoot;
    int  pak;
    int  ciaFound, totalCias;
    char ciaPath[300];
    u64  ciaSize;
    int  amOk;
    int  instMain, instTest;   // installed titles
    u64  sdFreeMB;             // 0 = unknown
} Check;

static const char* const kKeyFiles[] = {          // files every working copy has (also what the PC/Android ports check)
    "parms/gamePrms.bin",
    "SndData/Banks/pikibank.bx",
    "SndData/Seqs/pikiseq.arc",
};

static int title_installed(u64 tid)
{
    AM_TitleEntry e;
    return R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &tid, &e));
}

// Look for a stray dataDir: one folder too deep under assets, or in the other usual wrong places.
static int find_stray_datadir(char* where, size_t cap)
{
    char p[300];
    DIR* d = opendir(NECTAR_HOME "/assets");
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof p, NECTAR_HOME "/assets/%s/dataDir", e->d_name);
            if (is_dir(p)) { snprintf(where, cap, NECTAR_HOME "/assets/%s", e->d_name); closedir(d); return 1; }
        }
        closedir(d);
    }
    static const char* const kPlaces[] = {
        NECTAR_HOME, NECTAR_HOME "/files", NECTAR_HOME "/assets/assets", "sdmc:", "sdmc:/files", "sdmc:/assets",
        "sdmc:/nectar", "sdmc:/nectar/assets", "sdmc:/3ds", "sdmc:/3ds/assets", "sdmc:/3ds/nectar_test/assets",
    };
    for (size_t i = 0; i < sizeof kPlaces / sizeof kPlaces[0]; ++i) {
        snprintf(p, sizeof p, "%s/dataDir", kPlaces[i]);
        if (is_dir(p)) { snprintf(where, cap, "%s", kPlaces[i]); return 1; }
    }
    return 0;
}

static void run_check(Check* c)
{
    memset(c, 0, sizeof *c);
    char p[300];

    bool n3 = false;
    APT_CheckNew3DS(&n3);
    c->isNew3ds = n3 ? 1 : 0;

    c->sdOk = is_dir("sdmc:/");

    // game files
    if (is_dir(NECTAR_HOME "/assets/dataDir")) {
        c->assets = AS_OK;
        for (size_t i = 0; i < sizeof kKeyFiles / sizeof kKeyFiles[0]; ++i) {
            snprintf(p, sizeof p, NECTAR_HOME "/assets/dataDir/%s", kKeyFiles[i]);
            if (!is_file(p)) { c->assets = AS_INCOMPLETE; snprintf(c->assetsMissing, sizeof c->assetsMissing, "%s", kKeyFiles[i]); break; }
        }
    } else if (find_stray_datadir(c->assetsWhere, sizeof c->assetsWhere)) {
        c->assets = AS_MISPLACED;
    } else if (is_dir(NECTAR_HOME "/assets/parms") || is_dir(NECTAR_HOME "/assets/SndData")) {
        c->assets = AS_FLAT;
    } else if (is_dir(NECTAR_HOME "/assets")) {
        c->assets = AS_EMPTY;
    } else {
        c->assets = AS_MISSING;
    }

    c->ini = is_file(NECTAR_HOME "/nectar.ini");
    c->iniInRoot = !c->ini && is_file("sdmc:/nectar.ini");
    c->pak = is_file(NECTAR_HOME "/nectar.pak");

    c->ciaFound = find_cia(c->ciaPath, sizeof c->ciaPath, &c->ciaSize, &c->totalCias) == 1;

    FS_ArchiveResource r;
    if (R_SUCCEEDED(FSUSER_GetArchiveResource(&r, SYSTEM_MEDIATYPE_SD)))
        c->sdFreeMB = (u64)r.freeClusters * r.clusterSize / (1024 * 1024);
}

static void check_titles(Check* c, int amOk)
{
    c->amOk = amOk;
    if (amOk) {
        c->instMain = title_installed(kTitleMain);
        c->instTest = title_installed(kTitleTest);
    }
}

// =====================================================================================================================  drawing
static int sWrapRow;
static void emit_line(const char* line, void* ctx) { (void)ctx; printf("%s\n", line); ++sWrapRow; }
static void print_wrapped(const char* s, int width) { guide_wrap(s, width, emit_line, NULL); }

static void status_line(const char* tag, const char* colour, const char* label, const char* text)
{
    printf(" %s[%s]" C_RESET " %-12s %.28s\n", colour, tag, label, text);
}

static const char* basename_of(const char* path)
{
    const char* b = strrchr(path, '/');
    return b ? b + 1 : path;
}

static void draw_main(const Check* c)
{
    consoleSelect(&sTop);
    consoleClear();
    printf(C_CYAN " Open Nectar for New 3DS" C_RESET "  -  installer + guide\n");
    printf(" ------------------------------------------------\n\n");

    if (c->isNew3ds) status_line(" OK ", C_GREEN, "Console", "New 3DS family");
    else             status_line("STOP", C_RED,   "Console", "not a New 3DS (needed)");

    switch (c->assets) {
        case AS_OK:         status_line(" OK ", C_GREEN,  "Game files", "3ds/nectar/assets/dataDir"); break;
        case AS_INCOMPLETE: status_line("FIX ", C_RED,    "Game files", "incomplete (see below)"); break;
        case AS_MISPLACED:  status_line("FIX ", C_YELLOW, "Game files", "in the wrong folder"); break;
        case AS_FLAT:       status_line("FIX ", C_YELLOW, "Game files", "dataDir folder missing"); break;
        default:            status_line("NEED", C_RED,    "Game files", "not found yet"); break;
    }

    if (c->ini)            status_line(" OK ", C_GREEN,  "nectar.ini", "found");
    else if (c->iniInRoot) status_line("FIX ", C_YELLOW, "nectar.ini", "in the SD root: move it");
    else                   status_line(" -- ", C_DIM,    "nectar.ini", "none (defaults are used)");

    if (c->pak) status_line(" OK ", C_GREEN, "nectar.pak", "found (fast loading)");
    else        status_line(" -- ", C_DIM,   "nectar.pak", "none (optional: R builds it)");

    char txt[64];
    if (c->ciaFound) {
        snprintf(txt, sizeof txt, "%.20s %llu MB", basename_of(c->ciaPath), (unsigned long long)(c->ciaSize >> 20));
        status_line(" OK ", C_GREEN, "Game .cia", txt);
    } else {
        status_line(" -- ", C_DIM, "Game .cia", "none in the SD root");
    }

    if (!c->amOk)                       status_line(" ?? ", C_YELLOW, "Installed", "can't tell (no CFW?)");
    else if (c->instMain && c->instTest) status_line(" OK ", C_GREEN, "Installed", "Open Nectar + TEST");
    else if (c->instMain)               status_line(" OK ", C_GREEN,  "Installed", "Open Nectar");
    else if (c->instTest)               status_line(" OK ", C_GREEN,  "Installed", "Open Nectar TEST");
    else                                status_line(" -- ", C_DIM,    "Installed", "not yet");

    if (c->sdFreeMB) printf("\n        SD card free space: %llu MB\n", (unsigned long long)c->sdFreeMB);
    else             printf("\n");

    printf("\n");
    if (c->ciaFound && c->amOk) printf("  " C_CYAN "A" C_RESET "      Install %.30s\n", basename_of(c->ciaPath));
    else if (c->ciaFound)       printf(C_DIM "  A      Install (needs custom firmware)" C_RESET "\n");
    else                        printf(C_DIM "  A      Install (no game .cia in the SD root)" C_RESET "\n");
    printf("  " C_CYAN "Y" C_RESET "      Step-by-step setup guide\n");
    if (c->assets == AS_OK || c->assets == AS_INCOMPLETE)
        printf("  " C_CYAN "R" C_RESET "      %s nectar.pak (faster loading)\n", c->pak ? "Rebuild / check" : "Build");
    else
        printf(C_DIM "  R      Build nectar.pak (needs the game files)" C_RESET "\n");
    printf("  " C_CYAN "X" C_RESET "      Check the SD card again\n");
    printf("  " C_CYAN "START" C_RESET "  Back to the HOME menu\n");

    // bottom screen: the next thing to do
    consoleSelect(&sBot);
    consoleClear();
    printf(C_CYAN "What to do next" C_RESET "\n---------------------------------------\n");
    char msg[400];
    if (!c->sdOk) {
        print_wrapped("The SD card can't be read. Check that it is inserted properly, then press X.", BOT_W);
    } else if (!c->isNew3ds) {
        print_wrapped("This console is an original 3DS or 2DS. Open Nectar only runs on a New 3DS, New 3DS XL or New 2DS XL "
                      "(it needs their extra memory and speed).", BOT_W);
    } else if (c->assets == AS_MISSING || c->assets == AS_EMPTY) {
        print_wrapped("The game files from your Pikmin disc aren't on the SD card yet.", BOT_W);
        print_wrapped("", BOT_W);
        print_wrapped("New here? Press Y: the guide walks you through it (Steps 1 and 2).", BOT_W);
    } else if (c->assets == AS_MISPLACED) {
        snprintf(msg, sizeof msg, "Found the game files, but in the wrong place: dataDir is inside %s/", c->assetsWhere + 5);
        print_wrapped(msg, BOT_W);
        print_wrapped("", BOT_W);
        snprintf(msg, sizeof msg, "On a computer, move the dataDir folder from %s into 3ds/nectar/assets so it ends up at "
                 "3ds/nectar/assets/dataDir, then press X.", c->assetsWhere + 5);
        print_wrapped(msg, BOT_W);
    } else if (c->assets == AS_FLAT) {
        print_wrapped("The folders that belong INSIDE dataDir (parms, SndData, ...) are straight in 3ds/nectar/assets.", BOT_W);
        print_wrapped("", BOT_W);
        print_wrapped("On a computer, make a folder named dataDir in 3ds/nectar/assets and move all of them into it. Then press X.", BOT_W);
    } else if (c->assets == AS_INCOMPLETE) {
        snprintf(msg, sizeof msg, "dataDir is in the right place, but dataDir/%s is missing.", c->assetsMissing);
        print_wrapped(msg, BOT_W);
        print_wrapped("", BOT_W);
        print_wrapped("The copy may have stopped early, or this is not a full Pikmin (USA) extract. Copy the game files again "
                      "(guide Steps 1 and 2), then press X.", BOT_W);
    } else if (c->ciaFound && c->amOk) {
        print_wrapped("Game files look good. Press A to install the game.", BOT_W);
        if (!c->pak) { print_wrapped("", BOT_W); print_wrapped("Then press R to build nectar.pak for much faster loading.", BOT_W); }
    } else if (!c->amOk) {
        print_wrapped("The install service can't be opened: custom firmware (Luma3DS) is needed to install .cia files. "
                      "See https://3ds.hacks.guide", BOT_W);
    } else if (!c->instMain && !c->instTest) {
        print_wrapped("Game files look good. Now copy the game's .cia (nectar.cia) to the ROOT of the SD card, start this app "
                      "again and press A (guide Step 3).", BOT_W);
    } else {
        printf(C_GREEN "All set!" C_RESET "\n");
        print_wrapped("Start Open Nectar from the HOME menu.", BOT_W);
        if (!c->pak) { print_wrapped("", BOT_W); print_wrapped("Tip: press R to build nectar.pak for much faster loading (a few minutes, guide Step 4).", BOT_W); }
    }
    if (c->iniInRoot) {
        print_wrapped("", BOT_W);
        print_wrapped("Also: nectar.ini is in the SD root. Move it to sdmc:/3ds/nectar/ or the game won't see it.", BOT_W);
    }
    present();
}

static void draw_guide(int page)
{
    const GuidePage* g = &kGuide[page];
    consoleSelect(&sTop);
    consoleClear();
    printf(C_CYAN " %.48s" C_RESET "\n", g->title);
    printf(" ------------------------------------------------\n");
    for (int i = 0; g->lines[i]; ++i) print_wrapped(g->lines[i], TOP_W);
    printf("\x1b[29;0H" C_DIM " < Left          page %d / %d          Right >" C_RESET, page + 1, GUIDE_PAGES);

    consoleSelect(&sBot);
    consoleClear();
    printf(C_CYAN "Setup guide" C_RESET "\n---------------------------------------\n\n");
    for (int i = 0; i < GUIDE_PAGES; ++i)
        printf("%s %d. %.33s" C_RESET "\n", i == page ? C_CYAN ">" : C_DIM " ", i + 1, kGuide[i].title);
    printf("\n\nLeft/Right or L/R   turn the page\n");
    printf("Up/Down             jump to a page\n");
    printf("B                   back to the checks\n");
    printf("START               back to HOME\n");
    present();
}

// =====================================================================================================================  install
typedef struct {
    Result   res;        // 0 = installed
    const char* stage;   // what failed
    u64      failOff;    // for a write error: the offset and size of the refused write
    u32      failSize;
} InstallResult;

// One installation attempt with a given write block size. Removes an installed copy of the title first (as FBI does).
static InstallResult install_once(const char* path, u64 size, u64 titleId, u32 chunk)
{
    InstallResult r = { 0, "", 0, 0 };
    FILE* f = fopen(path, "rb");
    if (!f) { r.res = -1; r.stage = "open the .cia"; return r; }
    u8* buf = (u8*)malloc(chunk);
    if (!buf) { fclose(f); r.res = -1; r.stage = "allocate the write buffer"; return r; }

    if (titleId) {
        AM_DeleteTitle(MEDIATYPE_SD, titleId);   // errors ignored: normally "not installed"
        AM_DeleteTicket(titleId);
        AM_QueryAvailableExternalTitleDatabase(NULL);
    }

    Handle cia = 0;
    Result res = AM_StartCiaInstall(MEDIATYPE_SD, &cia);
    if (R_FAILED(res)) { free(buf); fclose(f); r.res = res; r.stage = "AM_StartCiaInstall"; return r; }

    aptSetHomeAllowed(false);   // no HOME / sleep in the middle of the install
    aptSetSleepAllowed(false);

    fseek(f, 0, SEEK_SET);
    u64 offset = 0;
    unsigned lastPercent = 1000;
    int ok = 1;
    for (;;) {
        const size_t got = fread(buf, 1, chunk, f);
        if (got == 0) break;
        u32 written = 0;
        res = FSFILE_Write(cia, &written, offset, buf, (u32)got, 0);
        if (R_FAILED(res) || written != got) {
            r.res = R_FAILED(res) ? res : -2;
            r.stage = "write to the installer";
            r.failOff = offset;
            r.failSize = (u32)got;
            ok = 0;
            break;
        }
        offset += got;
        const unsigned percent = size ? (unsigned)(offset * 100 / size) : 100;
        if (percent != lastPercent) {
            lastPercent = percent;
            printf("\rInstalling... %3u%%", percent);
        }
        present();
        if (!aptMainLoop()) { r.res = -3; r.stage = "cancelled (system asked to quit)"; ok = 0; break; }
    }
    if (ok && ferror(f)) { r.res = -4; r.stage = "read the SD card"; ok = 0; }
    free(buf);
    fclose(f);

    if (ok) {
        printf("\rFinishing the install...   \n");
        present();
        res = AM_FinishCiaInstall(cia);
        if (R_FAILED(res)) { r.res = res; r.stage = "AM_FinishCiaInstall"; ok = 0; }
    } else {
        AM_CancelCIAInstall(cia);
    }
    aptSetSleepAllowed(true);
    aptSetHomeAllowed(true);
    if (ok) r.res = 0;
    return r;
}

// Wait for one of the keys in mask; returns the key pressed (0 if the system asked us to quit).
static u32 wait_keys(u32 mask)
{
    while (aptMainLoop()) {
        hidScanInput();
        const u32 k = hidKeysDown() & mask;
        if (k) return k;
        present();
    }
    return 0;
}

// Installs the .cia; returns 1 to go back to the menu, 0 to leave for HOME.
static int do_install(const char* path, u64 size)
{
    consoleSelect(&sBot);
    consoleClear();
    printf(C_CYAN "Installing" C_RESET "\n---------------------------------------\n\n");
    print_wrapped("HOME and sleep are off until the install finishes. Don't take out the SD card.", BOT_W);

    consoleSelect(&sTop);
    consoleClear();
    printf(C_CYAN " Installing %.36s" C_RESET "\n", basename_of(path));
    printf(" ------------------------------------------------\n\n");
    printf("Size: %llu MB\n", (unsigned long long)(size >> 20));
    present();

    u64 titleId = 0;
    int bad = 0;
    {
        FILE* f = fopen(path, "rb");
        if (!f) {
            printf(C_RED "Cannot open %s" C_RESET "\n", path);
            bad = 1;
        } else {
            u32 headerSize = 0;
            // A CIA starts with its header size, 0x2020 (little endian). Refuse anything else before touching AM.
            if (fread(&headerSize, 1, sizeof headerSize, f) != sizeof headerSize || headerSize != 0x2020) {
                printf(C_RED "This file is not a valid .cia (header 0x%08lX)." C_RESET "\n", (unsigned long)headerSize);
                printf("Copy it to the SD card again.\n");
                bad = 1;
            } else {
                titleId = cia_title_id(f);
            }
            fclose(f);
        }
    }

    InstallResult r = { -1, "", 0, 0 };
    if (!bad) {
        printf("Title ID: %08lX%08lX\n\n", (unsigned long)(titleId >> 32), (unsigned long)(titleId & 0xFFFFFFFFu));
        static const u32 chunks[2] = { CHUNK_BIG, CHUNK_SMALL };
        for (int attempt = 0; attempt < 2; ++attempt) {
            r = install_once(path, size, titleId, chunks[attempt]);
            if (R_SUCCEEDED(r.res)) break;
            printf(C_RED "\nAttempt %d failed: %s (0x%08lX)" C_RESET "\n", attempt + 1, r.stage, (unsigned long)r.res);
            if (r.failSize) printf("  write of %lu bytes at offset %llu (%llu KB), block size %lu\n", (unsigned long)r.failSize,
                                   (unsigned long long)r.failOff, (unsigned long long)(r.failOff / 1024), (unsigned long)chunks[attempt]);
            if (strcmp(r.stage, "write to the installer") != 0) break;   // only a refused write is worth another try with a smaller block
            if (attempt == 0) printf("Trying again with %lu KB blocks...\n", (unsigned long)(chunks[1] / 1024));
        }
    }

    if (!bad && R_SUCCEEDED(r.res)) {
        printf(C_GREEN "\nDone. Open Nectar is installed." C_RESET "\n\n");
        printf("  " C_CYAN "SELECT" C_RESET " Delete the .cia (frees %llu MB)\n", (unsigned long long)(size >> 20));
        printf("  " C_CYAN "A" C_RESET "      Back to the checks\n");
        printf("  " C_CYAN "START" C_RESET "  Back to the HOME menu\n");
        consoleSelect(&sBot);
        consoleClear();
        printf(C_GREEN "Installed!" C_RESET "\n---------------------------------------\n\n");
        print_wrapped("Open Nectar is now on the HOME menu.", BOT_W);
        print_wrapped("", BOT_W);
        print_wrapped("The .cia on the SD card is not needed any more: SELECT deletes it to free the space.", BOT_W);
        print_wrapped("", BOT_W);
        print_wrapped("No nectar.pak yet? Back on the checks screen, R builds it (much faster loading).", BOT_W);
        consoleSelect(&sTop);
        present();
        for (;;) {
            const u32 k = wait_keys(KEY_A | KEY_B | KEY_START | KEY_SELECT);
            if (k & KEY_SELECT) {
                if (remove(path) == 0) printf(C_GREEN "\nDeleted %.36s" C_RESET "\n", basename_of(path));
                else                   printf(C_RED "\nCould not delete %.30s" C_RESET "\n", basename_of(path));
                present();
                continue;
            }
            return (k & (KEY_A | KEY_B)) ? 1 : 0;
        }
    }

    if (!bad) printf("\nNothing was installed. File size %llu bytes (mod 64 = %u).\n", (unsigned long long)size, (unsigned)(size & 63));
    consoleSelect(&sBot);
    consoleClear();
    printf(C_RED "Install failed" C_RESET "\n---------------------------------------\n\n");
    print_wrapped("Check the free space on the SD card and copy the .cia to it again (the copy may be damaged). "
                  "The guide's Troubleshooting page has more.", BOT_W);
    consoleSelect(&sTop);
    printf("\n  " C_CYAN "A" C_RESET "      Back to the checks\n  " C_CYAN "START" C_RESET "  Back to the HOME menu\n");
    present();
    const u32 k = wait_keys(KEY_A | KEY_B | KEY_START);
    return (k & (KEY_A | KEY_B)) ? 1 : 0;
}

// =====================================================================================================================  nectar.pak
static u64 sPakStart, sPakLastDraw;
static unsigned ms_since(u64 t) { return (unsigned)((svcGetSystemTick() - t) * 1000ULL / SYSCLOCK_ARM11); }

#define PAK_ROW 15   /* first console row of the progress / result area */

static int pak_progress(const NpProgress* p, void* user)
{
    (void)user;
    if (sPakLastDraw == 0 || ms_since(sPakLastDraw) >= 120) {   // console text is slow: ~8 redraws a second
        sPakLastDraw = svcGetSystemTick();
        const unsigned s = ms_since(sPakStart) / 1000;
        consoleSelect(&sTop);
        printf("\x1b[%d;0H", PAK_ROW);
        if (p->done == 0) {
            printf(" Listing files...  %u found\x1b[K\n\x1b[K\n\x1b[K\n\x1b[K\n", p->listed);
        } else {
            const unsigned pct = p->listed ? p->done * 100 / p->listed : 0;
            printf(" %u / %u files  (%u%%)\x1b[K\n", p->done, p->listed, pct);
            printf(" %.1f MB   time %u:%02u\x1b[K\n", p->bytes / 1048576.0, s / 60, s % 60);
            printf(" Kept loose: %u big, %u other, %u unreadable\x1b[K\n", p->skippedBig, p->skippedExt, p->skippedErr);
            printf(" %.47s\x1b[K\n", p->current ? p->current : "");
        }
        present();
    }
    if (!aptMainLoop()) return 0;
    hidScanInput();
    return !(hidKeysDown() & KEY_B);
}

static void draw_pak(const Check* c)
{
    char pak[64];
    snprintf(pak, sizeof pak, NECTAR_HOME "/nectar.pak");
    struct stat st;
    const int have = stat(pak, &st) == 0 && !S_ISDIR(st.st_mode);

    consoleSelect(&sTop);
    consoleClear();
    printf(C_CYAN " nectar.pak" C_RESET "  -  faster loading\n");
    printf(" ------------------------------------------------\n\n");
    if (have) printf("  nectar.pak:  %.1f MB (replaced by a new build)\n", st.st_size / 1048576.0);
    else      printf("  nectar.pak:  none yet\n");
    if (c->sdFreeMB) printf("  SD free:     %llu MB\n", (unsigned long long)c->sdFreeMB);
    printf("\n");
    printf("  " C_CYAN "A" C_RESET "      Build nectar.pak (a few minutes)\n");
    if (have) printf("  " C_CYAN "Y" C_RESET "      Check nectar.pak against the game files\n");
    printf("  " C_CYAN "B" C_RESET "      Back (also cancels a build)\n");
    printf("  " C_CYAN "START" C_RESET "  Back to the HOME menu\n");

    consoleSelect(&sBot);
    consoleClear();
    printf(C_CYAN "About nectar.pak" C_RESET "\n---------------------------------------\n");
    print_wrapped("Packs the game's thousands of small files (assets/dataDir, except SndData) into one file, so a level opens "
                  "one file on the SD card instead of hundreds. Loading gets a lot faster.", BOT_W);
    print_wrapped("", BOT_W);
    print_wrapped("Keep the assets folder: big files and the music stay loose and are still read from there.", BOT_W);
    print_wrapped("", BOT_W);
    print_wrapped("Build it again whenever you replace the game files. nectar.ini: pak = 0 turns it off.", BOT_W);
    print_wrapped("", BOT_W);
    print_wrapped("Leave the SD card in while it works.", BOT_W);
    present();
}

// nectar.pak screen; returns 1 to go back to the checks, 0 to leave for HOME.
static int do_pak(const Check* c)
{
    int redraw = 1;
    while (aptMainLoop()) {
        if (redraw) { draw_pak(c); redraw = 0; }
        hidScanInput();
        const u32 k = hidKeysDown();
        if (k & KEY_START) return 0;
        if (k & KEY_B) return 1;
        const int have = is_file(NECTAR_HOME "/nectar.pak");
        if ((k & KEY_A) || ((k & KEY_Y) && have)) {
            const int build = (k & KEY_A) != 0;
            consoleSelect(&sTop);
            printf("\x1b[%d;0H\x1b[J", PAK_ROW - 2);
            printf(build ? " Building... (B cancels)\n" : " Checking... (B cancels)\n");
            present();
            sPakStart = svcGetSystemTick(); sPakLastDraw = 0;
            static NpResult r;   // ~600 bytes: keep it off the stack
            aptSetSleepAllowed(false);
            if (build) np_build(NECTAR_HOME, 2ull * 1024 * 1024, pak_progress, NULL, &r);
            else       np_verify(NECTAR_HOME, pak_progress, NULL, &r);
            aptSetSleepAllowed(true);
            const unsigned s = ms_since(sPakStart) / 1000;

            consoleSelect(&sTop);
            printf("\x1b[%d;0H\x1b[J", PAK_ROW - 2);
            if (build && r.ok) {
                printf(C_GREEN " Done" C_RESET " in %u:%02u\n", s / 60, s % 60);
                printf(" %u files packed, %.1f MB\n", r.stats.packed, r.stats.bytes / 1048576.0);
                printf(" %u big + %u other files stay loose\n", r.stats.skippedBig, r.stats.skippedExt);
                if (r.stats.skippedErr) printf(" %u unreadable, e.g.\n  %.46s\n", r.stats.skippedErr, basename_of(r.firstSkipped));
            } else if (build) {
                printf(C_RED " Not built:" C_RESET " %.37s\n", r.error);
                if (strlen(r.error) > 37) printf(" %.48s\n", r.error + 37);
                printf(" The old nectar.pak (if any) is unchanged.\n");
            } else if (r.ok) {
                printf(C_GREEN " nectar.pak is OK" C_RESET ": %u packed files match\n the game files (%u:%02u).\n", r.stats.packed, s / 60, s % 60);
            } else {
                printf(C_RED " nectar.pak is out of date" C_RESET "\n %.48s\n", r.error);
                if (r.firstSkipped[0]) printf(" first: %.41s\n", basename_of(r.firstSkipped));
                printf(" Build it again with A.\n");
            }
            printf("\n  " C_CYAN "A" C_RESET "      Back to the checks\n  " C_CYAN "START" C_RESET "  Back to the HOME menu\n");
            present();
            const u32 k2 = wait_keys(KEY_A | KEY_B | KEY_START);
            return (k2 & (KEY_A | KEY_B)) ? 1 : 0;
        }
        present();
    }
    return 0;
}

// =====================================================================================================================  main
int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &sTop);
    consoleInit(GFX_BOTTOM, &sBot);
    osSetSpeedupEnable(true);   /* New 3DS: 804 MHz + L2 cache (the pak build is mostly SD card time, but it helps) */

    const int amOk = R_SUCCEEDED(amInit());

    Check c;
    run_check(&c);
    check_titles(&c, amOk);

    int inGuide = 0, page = 0, redraw = 1;
    while (aptMainLoop()) {
        if (redraw) { if (inGuide) draw_guide(page); else draw_main(&c); redraw = 0; }
        hidScanInput();
        const u32 k = hidKeysDown();
        if (k & KEY_START) break;
        if (inGuide) {
            if (k & (KEY_DRIGHT | KEY_R | KEY_CPAD_RIGHT)) { if (page + 1 < GUIDE_PAGES) { ++page; redraw = 1; } }
            if (k & (KEY_DLEFT | KEY_L | KEY_CPAD_LEFT))   { if (page > 0) { --page; redraw = 1; } }
            if (k & (KEY_DDOWN | KEY_CPAD_DOWN))           { page = (page + 1) % GUIDE_PAGES; redraw = 1; }
            if (k & (KEY_DUP | KEY_CPAD_UP))               { page = (page + GUIDE_PAGES - 1) % GUIDE_PAGES; redraw = 1; }
            if (k & (KEY_B | KEY_Y)) {
                inGuide = 0;
                run_check(&c); check_titles(&c, amOk);   // they may have been following the guide with the SD card in a PC
                redraw = 1;
            }
        } else {
            if (k & KEY_Y) { inGuide = 1; redraw = 1; }
            if (k & KEY_X) {
                consoleSelect(&sBot); consoleClear(); printf("Checking the SD card...\n"); present();
                run_check(&c); check_titles(&c, amOk);
                redraw = 1;
            }
            if ((k & KEY_R) && (c.assets == AS_OK || c.assets == AS_INCOMPLETE)) {
                if (!do_pak(&c)) break;
                run_check(&c); check_titles(&c, amOk);
                redraw = 1;
            }
            if ((k & KEY_A) && c.ciaFound && amOk) {
                if (!do_install(c.ciaPath, c.ciaSize)) break;
                run_check(&c); check_titles(&c, amOk);
                redraw = 1;
            }
        }
        present();
    }

    if (amOk) amExit();
    gfxExit();
    return 0;   // returning from main leaves a CIA application -> HOME menu
}
