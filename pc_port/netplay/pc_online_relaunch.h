#pragma once
#include <string>
#include <vector>

// Starts this executable again with its original arguments (minus any online
// or mode switch) plus `extra`. False with a message in *err on failure; the
// caller exits on success.
bool pc_online_relaunch(const std::vector<std::string>& extra, std::string* err);

// Android: a system toast over the game (works before the game draws
// anything). Elsewhere it does nothing.
void pc_online_android_toast(const char* text);
