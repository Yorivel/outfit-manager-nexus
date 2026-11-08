// Outfit Manager's shared types, constants and state.
#include "om.h"

namespace om {

U64 g_base = 0;
HMODULE g_self = nullptr;
std::uint8_t look_pos(std::uint8_t look)
{
    for (std::uint8_t k = 0; k < kLookCount; ++k)
        if (kLookOrder[k] == look) return k;
    return 0;
}
std::vector<Outfit> g_outfits; // the current character's outfits (what the wheel shows)
std::wstring g_store_path;
U16 g_armor_slots[kOutfitSlots]; // equipment-N category per Armor wheel slot (template record order)
U32 g_armor_slot_count = 0;
U16 g_weapon_slots[kMaxWeaponSlots]; // the Weapons wheel's (record 0); 0xFFFF = a wheel slot with no equip slot
std::atomic<U32> g_weapon_slot_count{0};

bool is_weapon_slot(U16 slot)
{
    for (U32 i = 0; i < g_weapon_slot_count; ++i)
        if (g_weapon_slots[i] == slot) return true;
    return false;
}
bool uses_weapons(const Outfit& o) { return o.has_weapons && (o.load_weapons || o.weapons_only); }
bool is_weapon_set(const Outfit& o) { return o.has_weapons && o.weapons_only; }
// The outfit's pieces loading puts on (the weapon ones only when it loads weapons).
bool loads_piece(const Outfit& o, U16 slot)
{
    if (is_weapon_set(o)) return is_weapon_slot(slot); // (a weapon set: its weapons only, the armour left alone, #27)
    return uses_weapons(o) || !is_weapon_slot(slot);
}
Store g_store;
U16 g_character = kNoCharacter;

std::atomic<U32> g_outfit_count{0}; // == g_outfits.size()

} // namespace om
