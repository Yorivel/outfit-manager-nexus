// Item data: names, the item table, resistances and climate, key repair.
#pragma once
#include "om.h"

namespace om {

extern RecordGetFn g_str_get;
U32 pid_of(U16 key);
U16 key_of_pid(U32 pid);
bool key_assigned_at_run(U16 key);
extern std::map<U64, std::pair<int, int>> g_gear_resist;
void outfit_resistance(const Outfit& o, int& ice, int& fire);
const char* climate_badge(const Outfit& o);
const char* climate_only(const Outfit& o);
std::string piece_icon(const OutfitItem& it);
extern ItemInfoFn g_item_info;
std::string item_name(U16 key);
void repair_keys();
bool readable_name(const std::string& n);
void ensure_resist_ids();

} // namespace om
