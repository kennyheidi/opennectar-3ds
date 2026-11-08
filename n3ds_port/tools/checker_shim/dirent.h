// CHECKER ONLY (bare newlib has no <dirent.h>; devkitARM does).
#pragma once
struct dirent { char d_name[256]; };
typedef struct DIR_ DIR;
extern "C" { DIR* opendir(const char*); struct dirent* readdir(DIR*); int closedir(DIR*); }
