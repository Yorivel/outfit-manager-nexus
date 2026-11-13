// Character look (#35): an outfit brings a snapshot of the character's barber look along while it is applied.
#pragma once
#include <atomic>
#include <string>
#include "om.h"

namespace om {

// The played character's look now (raw: 16 mesh options + 250 values; empty when it can't be read).
std::string charlook_read();
// The applied outfit changed (mark_worn): its snapshot goes on, or the character's own look comes back (on a HUD frame,
// once the outfit's gear has settled).
void charlook_follow();
// Once per HUD frame (game thread): a pending change is applied.
void charlook_tick();
// The barber question's answer (to_outfit: the change becomes the outfit's look; else the character's own).
void charlook_barber_answer(bool to_outfit);
// Hooks the barber's reply (setup).
void charlook_install();
extern std::atomic<U64> g_hud_root;

// The settings' Character look: Snapshot clicked (takes / retakes it; the outfit brings it from now on) or Off clicked
// (the outfit stops bringing it; the snapshot is kept). `applied`: the outfit is the one applied now. False: not done.
bool charlook_take(Outfit& o, bool applied);
void charlook_off(Outfit& o, bool applied);
// The outfit brings a snapshot (its setting is Snapshot and it has one).
bool charlook_on(const Outfit& o);

} // namespace om
