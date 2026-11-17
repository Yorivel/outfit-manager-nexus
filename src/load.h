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
void swim_player_load(int index); // (sleep.cpp: an outfit the player puts on himself - -1 Unequip Outfit - wins over a swim)
void pump_async(U64 self);
extern QuickSlotChangeFn g_quickslot_change; // (#53: a weapon set's weapon selected after its load; null: off)
extern RecordGetFn g_equip_type_info;
extern WeaponGroupSlotsFn g_weapon_group_slots;
void select_set_weapon(U64 self, const Outfit& o);
void weapon_select_install(); // (#53)
void prepare_outfits();

} // namespace om
