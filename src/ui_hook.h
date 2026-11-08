// The gear wheel's compiled UI files, patched as the game loads them.
#pragma once
#include "om.h"

namespace om {

extern UiParseFn g_ui_parse_orig;
extern std::string g_ui_hook_note;
bool install_ui_patch();

} // namespace om
