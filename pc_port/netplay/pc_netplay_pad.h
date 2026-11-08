#pragma once
// Minimal accessor for the file-static pad array in
// src/sysDolphin/controllerMgr.cpp. The netplay input record/replay layer
// reads and overwrites the polled pads through here; the existing update
// logic is untouched.

#include "Dolphin/pad.h"

// Returns the 4 polled PADStatus records (index 0..3). Never null.
PADStatus* pc_netplay_pad_status(void);
