// Save binding: watching the game's save files and which save is loaded.
#pragma once
#include "om.h"

namespace om {

void poll_saves();
void settle_load_now();
bool install_save_hooks();

} // namespace om
