// Save binding: watching the game's save files and which save is loaded.
#pragma once
#include "om.h"

namespace om {

void poll_saves();
void settle_load_now();
bool install_save_hooks();
bool read_save_key(const std::wstring& path, U64& clock, U32& place); // (savekey.cpp)

} // namespace om
