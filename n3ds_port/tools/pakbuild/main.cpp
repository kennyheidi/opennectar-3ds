// nectar_pakbuild.3dsx: builds sdmc:/3ds/nectar/nectar.pak ON THE CONSOLE (same file tools/make_pak.py makes on a PC).
// Start it from the Homebrew Launcher, press A, wait (a few minutes: it copies every small asset file once), done.
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "pak_builder.h"

static PrintConsole sTop, sBot;
static u64 sStart = 0, sLastDraw = 0;

static bool exists(const char* p) { struct stat st; return stat(p, &st) == 0; }

static unsigned msSince(u64 t) { return (unsigned)((svcGetSystemTick() - t) * 1000ULL / SYSCLOCK_ARM11); }

static bool progress(const PakProgress& p, void*)
{
    // redraw at most ~8 times a second (console text is slow), and keep HOME / B working
    if (msSince(sLastDraw) >= 120 || sLastDraw == 0) {
        sLastDraw = svcGetSystemTick();
        consoleSelect(&sTop);
        const unsigned s = msSince(sStart) / 1000;
        printf("\x1b[8;0H");
        if (p.done == 0) {
            printf("Listing files...  %u found          \n\x1b[K\n\x1b[K\n\x1b[K\n", p.listed);
        } else {
            const unsigned pct = p.listed ? p.done * 100 / p.listed : 0;
            printf("Packing  %u / %u files  (%u%%)      \n", p.done, p.listed, pct);
            printf("Pak size %.1f MB   time %u:%02u        \n", p.bytes / 1048576.0, s / 60, s % 60);
            printf("Kept loose: %u big, %u other, %u unreadable \n", p.skippedBig, p.skippedExt, p.skippedErr);
            printf("\x1b[K%.48s\n", p.current ? p.current : "");
        }
        gfxFlushBuffers(); gfxSwapBuffers();
    }
    if (!aptMainLoop()) return false;
    hidScanInput();
    return !(hidKeysDown() & KEY_B);
}

int main()
{
    gfxInitDefault();
    osSetSpeedupEnable(true);
    consoleInit(GFX_TOP, &sTop);
    consoleInit(GFX_BOTTOM, &sBot);

    // the game keeps its files in sdmc:/3ds/nectar; fall back to the folder this .3dsx was started from
    const char* base = "sdmc:/3ds/nectar";
    static char cwd[256];
    if (!exists("sdmc:/3ds/nectar/assets/dataDir") && getcwd(cwd, sizeof cwd)) {
        size_t n = strlen(cwd); if (n > 1 && cwd[n - 1] == '/') cwd[n - 1] = 0;
        base = cwd;
    }
    char assets[300]; snprintf(assets, sizeof assets, "%s/assets/dataDir", base);
    char pak[300]; snprintf(pak, sizeof pak, "%s/nectar.pak", base);
    const bool haveAssets = exists(assets);

    consoleSelect(&sTop);
    printf("Open Nectar - nectar.pak builder\n\n");
    printf("Folder: %s\n", base);
    printf("assets/dataDir: %s\nnectar.pak: %s\n\n", haveAssets ? "found" : "MISSING", exists(pak) ? "exists (will be replaced)" : "none yet");
    consoleSelect(&sBot);
    printf("Packs assets/dataDir (what the\ngame loads; not SndData) into\none nectar.pak so levels load\nfaster (one SD-card open instead\nof hundreds).\n\n");
    printf("Keep the assets/ folder: big\nfiles and cutscene audio stay\nloose.\n\n");
    printf("Re-run after changing assets.\nnectar.ini: pak = 0 turns it off.\n\n");
    if (haveAssets) printf("A: build   Y: verify the pak\nB: cancel (while working)\nSTART: exit\n");
    else printf("No assets/dataDir folder found.\nSTART: exit\n");

    bool built = false;
    while (aptMainLoop()) {
        hidScanInput();
        const u32 k = hidKeysDown();
        if (k & KEY_START) break;
        if ((k & KEY_Y) && haveAssets && exists(pak)) {
            consoleSelect(&sTop);
            printf("\x1b[6;0HVerifying nectar.pak against the\nloose files... (B cancels)\n");
            sStart = svcGetSystemTick(); sLastDraw = 0;
            const PakResult r = pak_verify(base, progress, nullptr);
            consoleSelect(&sTop);
            printf("\x1b[14;0H\x1b[J");
            const unsigned s = msSince(sStart) / 1000;
            if (r.ok) printf("\x1b[32mPAK OK\x1b[0m: %u packed files identical\nto the loose files (%u:%02u)\n", r.stats.packed, s / 60, s % 60);
            else {
                printf("\x1b[31mPAK BAD\x1b[0m: %s\n", r.error);
                if (r.firstSkipped[0]) printf("first: %.80s\n", r.firstSkipped);
                printf("Rebuild it with A, or use pak = 0.\n");
            }
            printf("\nSTART: exit\n");
        }
        if ((k & KEY_A) && haveAssets && !built) {
            consoleSelect(&sTop);
            printf("\x1b[6;0HBuilding... (B cancels)\n");
            sStart = svcGetSystemTick(); sLastDraw = 0;
            const PakResult r = pak_build(base, 2u * 1024 * 1024, progress, nullptr);
            consoleSelect(&sTop);
            printf("\x1b[14;0H");
            const unsigned s = msSince(sStart) / 1000;
            if (r.ok) {
                printf("\x1b[32mDONE\x1b[0m in %u:%02u\n", s / 60, s % 60);
                printf("%u files packed, %.1f MB\n", r.stats.packed, r.stats.bytes / 1048576.0);
                printf("%u big + %u other files stay loose\n", r.stats.skippedBig, r.stats.skippedExt);
                if (r.stats.skippedErr) printf("%u unreadable, e.g.\n %.60s\n", r.stats.skippedErr, r.firstSkipped);
                printf("\n");
                printf("Start the game: its log shows\n\"[pak] nectar.pak: N files indexed\"\n");
            } else {
                printf("\x1b[31mNOT BUILT\x1b[0m: %s\n", r.error);
                printf("(the old nectar.pak, if any, is unchanged)\n");
            }
            printf("\nSTART: exit\n");
            built = true;
        }
        gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
