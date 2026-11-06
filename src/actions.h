// Saving, overwriting, deleting, cleaning up and uninstalling outfits.
#pragma once
#include "om.h"

namespace om {

void save_new_outfit(U64 self);
void clean_outfits(U64 self);
extern bool g_uninstalled;
void uninstall(U64 self);
void open_clean_confirm(U64 self, int what);
void open_confirm(U64 self, U32 index, int what, U32 other = 0);
void confirmed(U64 self, U32 index, int what, U32 other);
void overwrite_pressed(U64 self, U32 index, bool reset);

} // namespace om
