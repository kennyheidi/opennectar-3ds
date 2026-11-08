// Host check for guide.h: every guide page fits the top screen (49 columns, 26 text rows) and no line is wider than the screen.
//   cc -std=c99 -o guide_test guide_test.c && ./guide_test        (add -v to print the pages)
#include <stdio.h>
#include <string.h>
#include "guide.h"
static int sMaxW, sVerbose;
static void emit(const char* l, void* ctx) { (void)ctx; int w = (int)strlen(l); if (w > sMaxW) sMaxW = w; if (sVerbose) printf("|%-49s|\n", l); }
int main(int argc, char** argv)
{
    sVerbose = argc > 1;
    int fails = 0;
    for (int p = 0; p < GUIDE_PAGES; ++p) {
        int rows = 0; sMaxW = 0;
        if (sVerbose) printf("== %s\n", kGuide[p].title);
        for (int i = 0; kGuide[p].lines[i]; ++i) rows += guide_wrap(kGuide[p].lines[i], 49, emit, NULL);
        const int ok = rows <= 26 && sMaxW <= 49 && strlen(kGuide[p].title) <= 48;
        printf("page %d  %-44s rows %2d  widest %2d  %s\n", p + 1, kGuide[p].title, rows, sMaxW, ok ? "ok" : "TOO BIG");
        fails += !ok;
    }
    return fails;
}
