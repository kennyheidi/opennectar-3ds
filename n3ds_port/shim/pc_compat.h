// n3ds_port/shim/pc_compat.h
// Stand-in for macros that upstream's pc_port provides but that are missing from this tree.
// Force-included into every game source by n3ds_port/CMakeLists.txt (see -include below).
#pragma once

// aiBridge.cpp / aiBreakWall.cpp use this as "current in-game minute (0-59)".
// WorldClock::mCurrentGameMinute is exactly that (include/WorldClock.h, _28).
#ifndef PC_WORK_MINUTE
#define PC_WORK_MINUTE() (gameflow.mWorldClock.mCurrentGameMinute)
#endif
