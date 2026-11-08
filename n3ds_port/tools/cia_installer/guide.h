// Setup guide pages for the Nectar installer (plain C, no 3DS headers: tools/cia_installer/guide_test.c checks on a PC that
// every page fits the top screen).
//
// Each line is word-wrapped to the screen width when drawn. A line's leading spaces are its indent; a wrapped "- " or "1. " line
// continues under the text, not under the bullet. Lines are kept short enough that the tree drawings never wrap.
#ifndef NECTAR_INSTALLER_GUIDE_H
#define NECTAR_INSTALLER_GUIDE_H

#define GUIDE_MAX_LINES 32

typedef struct {
    const char* title;
    const char* lines[GUIDE_MAX_LINES];   // NULL-terminated
} GuidePage;

static const GuidePage kGuide[] = {
    { "Welcome", {
        "This app installs Open Nectar (the Pikmin PC port) on your 3DS and helps you set up the game files. The main screen checks your SD card for the usual mistakes.",
        "",
        "What you need:",
        "- A New 3DS, New 3DS XL or New 2DS XL. An original 3DS or 2DS does not have enough memory or speed.",
        "- Custom firmware (Luma3DS). If you don't have it yet, follow https://3ds.hacks.guide",
        "- Your own copy of Pikmin for GameCube (USA), dumped from your own disc (.iso, .gcm or .rvz). The game files are NOT included with Open Nectar.",
        "- About 1 GB free on the SD card.",
        "- A computer to copy files to the SD card.",
        "",
        "Turn pages with Left / Right (or L / R).",
        NULL } },

    { "Step 1: Get the game files (on a computer)", {
        "Open Nectar reads the files from your Pikmin disc image. Extract them on a computer with ONE of these:",
        "",
        "A) Dolphin (the GameCube emulator)",
        "  1. Add your Pikmin disc image to Dolphin's game list.",
        "  2. Right-click it > Properties > Filesystem.",
        "  3. Right-click the disc (the top entry) > Extract Entire Disc..., and pick an empty folder.",
        "  4. Inside you'll find a folder (named files) that has a folder named dataDir in it. That is the one you need.",
        "",
        "B) The Open Nectar PC launcher",
        "  Run this (all on one line):",
        "    nectar-launcher --rom Pikmin.iso --install-dir C:\\OpenNectar --extract-only",
        "  The game files end up in C:\\OpenNectar\\assets (dataDir is inside it).",
        NULL } },

    { "Step 2: Copy the game files to the SD card", {
        "Put the SD card in the computer and set it up like this:",
        "",
        "  SD card",
        "  +- nectar.cia        (the game, for Step 3)",
        "  +- 3ds",
        "     +- nectar",
        "        +- nectar.ini  (comes with the game)",
        "        +- assets",
        "           +- dataDir",
        "              +- parms",
        "              +- SndData",
        "              +- ... (all the rest)",
        "",
        "So dataDir must end up at  sdmc:/3ds/nectar/assets/dataDir",
        "",
        "Copy what is INSIDE the folder that holds dataDir (Dolphin's files folder, or the launcher's assets folder) into 3ds/nectar/assets.",
        "Common mistake: one folder too many, like assets/files/dataDir. The check on the main screen finds that and tells you what to move.",
        NULL } },

    { "Step 3: Install the game", {
        "1. Copy the game's .cia (for example nectar.cia) to the ROOT of the SD card, not into a folder.",
        "2. Start this installer and press A on the main screen.",
        "3. Wait for Done. An older Open Nectar install is replaced by itself.",
        "",
        "Your saves are kept: they are in sdmc:/3ds/nectar/save, not inside the installed game. Still, back that folder up before an update.",
        "",
        "After installing you can delete the .cia from the SD card (the installer offers to) to get the space back.",
        "",
        "Prefer FBI? Installing the .cia with FBI works too.",
        NULL } },

    { "Step 4 (optional): faster loading", {
        "Loading is a lot faster with nectar.pak: the game's thousands of small files packed into one big file.",
        "",
        "1. Once the game files are on the SD card, press R on this installer's main screen.",
        "2. Press A and wait a few minutes. It writes sdmc:/3ds/nectar/nectar.pak.",
        "",
        "Y on that screen checks an existing nectar.pak against the game files. Build it again whenever you replace the game files.",
        "",
        "(nectar_pakbuild.3dsx from the Homebrew Launcher does the same thing.)",
        NULL } },

    { "Step 5: Play", {
        "Start Open Nectar from the HOME menu.",
        "",
        "- The first load can take a little while.",
        "- In the game, tap the bottom screen for the settings menu (render distance, lighting and more).",
        "- The same settings live in sdmc:/3ds/nectar/nectar.ini.",
        "- Saves: sdmc:/3ds/nectar/save. Back it up now and then.",
        NULL } },

    { "Troubleshooting", {
        "Goes back to HOME right away, or a black screen:",
        "  Look at sdmc:/3ds/nectar/log.txt on a computer: the last lines say what went wrong. Usually the game files are missing or in the wrong folder. Run the check on the main screen (X).",
        "",
        "\"needs a New 3DS\" or out of memory:",
        "  Only the New 3DS family can run Open Nectar.",
        "",
        "Install fails, \"Cannot open the AM service\":",
        "  Custom firmware (Luma3DS) is needed.",
        "",
        "Install fails with a write error:",
        "  Check the free space, then copy the .cia to the SD card again (the copy may be damaged).",
        "",
        "Slow loading: build nectar.pak (R on the main screen, Step 4).",
        "",
        "Asking for help? Include log.txt.",
        NULL } },
};

#define GUIDE_PAGES ((int)(sizeof kGuide / sizeof kGuide[0]))

// Word wrap: calls emit(line) for every screen line of s at the given width. Returns the number of lines.
static int guide_wrap(const char* s, int width, void (*emit)(const char* line, void* ctx), void* ctx)
{
    char line[128];
    if (width > (int)sizeof line - 1) width = (int)sizeof line - 1;
    int indent = 0;
    while (s[indent] == ' ') ++indent;
    int cont = indent;                                           // indent of continuation lines
    if (s[indent] == '-' && s[indent + 1] == ' ') cont = indent + 2;
    else if (s[indent] >= '0' && s[indent] <= '9' && s[indent + 1] == '.' && s[indent + 2] == ' ') cont = indent + 3;
    else if (s[indent] >= 'A' && s[indent] <= 'Z' && s[indent + 1] == ')' && s[indent + 2] == ' ') cont = indent + 3;
    if (cont > width / 2) cont = 0;

    int n = 0, len = 0;
    for (int i = 0; i < indent && len < width; ++i) line[len++] = ' ';
    const char* p = s + indent;
    if (!*p) { line[len] = 0; emit(line, ctx); return 1; }
    int lineStart = len;                                         // where words start on this line
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        int wl = 0;
        while (p[wl] && p[wl] != ' ') ++wl;
        const int need = (len > lineStart ? 1 : 0) + wl;
        if (len + need > width && len > lineStart) {             // word does not fit: new line
            line[len] = 0; emit(line, ctx); ++n;
            len = 0;
            for (int i = 0; i < cont; ++i) line[len++] = ' ';
            lineStart = len;
        }
        if (len > lineStart) line[len++] = ' ';
        while (wl > 0) {                                         // copy the word (hard break if it is longer than a line)
            if (len >= width) {
                line[len] = 0; emit(line, ctx); ++n;
                len = 0;
                for (int i = 0; i < cont; ++i) line[len++] = ' ';
                lineStart = len;
            }
            line[len++] = *p++; --wl;
        }
    }
    line[len] = 0; emit(line, ctx); ++n;
    return n;
}

#endif
