// Loading an outfit: equip steps, the load pump, verification and the loss check.
#pragma once
#include "om.h"

namespace om {

int note_fault(EXCEPTION_POINTERS* ep);
extern CompanionEquipFn g_companion_equip_orig;
U64 __fastcall companion_equip_hook(U64 target, U64 err, U64 id, U64 bag, U64 pos, U64 slot);
bool companion_pull_ok();
U64 call_inventory(U64 actor);
std::string error_text(int err);
extern AsyncLoad g_async;
std::string slot_list(const Outfit& o, const std::vector<U16>& slots);
std::string piece_label(const Outfit& o, U16 slot);
void finish_load(U64 self, AskedLoad& p);
void start_loading(U64 self, U32 index);
void pump_async(U64 self);
void prepare_outfits();

} // namespace om
