// Keyboard and mouse: the game window's messages, raw input, the mouse wheel hook.
#pragma once
#include "om.h"

namespace om {

void watch_game_chars();
bool game_focused();
DWORD WINAPI key_thread(LPVOID);
extern std::atomic<U32> g_wheel_notches;
void wheel_hook(bool on);
void install_raw_input();

} // namespace om
