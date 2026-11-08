// Start-up: installing the hooks, the init thread.
#pragma once
#include "om.h"

namespace om {

extern std::wstring g_log_path;
void features_toast(U64 self);
bool in_game_process();
DWORD WINAPI init_thread(LPVOID);

} // namespace om
