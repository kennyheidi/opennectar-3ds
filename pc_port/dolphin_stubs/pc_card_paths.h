#pragma once
// Where the emulated memory card keeps a channel's files on disk (the
// folder holding "Pikmin dataFile" and its ".meta_" record). Online sessions
// copy the host's card to the guest so both games load the same save.
#include <string>

std::string pc_card_channel_dir(int channel);
