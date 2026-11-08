// Stash: saved pieces left in the bag after a load go to storage.
#pragma once
#include "om.h"

namespace om {

extern StashCheck g_stash_check;
bool stash_pending(U64 uid);
void stash_verify(U64 self);
std::vector<StashDest> stash_destinations();
void stash_pieces(U64 self, const Outfit* loaded);

} // namespace om
