// The appearance view: the wheel showing one outfit's pieces.
#pragma once
#include "om.h"

namespace om {

extern int g_eye;
extern int g_cfg;
extern std::uint8_t g_eye_step;
extern std::uint8_t g_eye_pick;
bool is_accessory_slot(U16 slot);
std::vector<size_t> eye_pieces();
bool outfit_has_slot(const Outfit& o, U16 slot);
std::uint8_t slot_look(const Outfit& o, U16 slot);
const OutfitItem* eye_item(size_t idx, OutfitItem& tmp);
std::uint8_t& eye_look(size_t idx);
U32 visible_slots();
U32 slot_outfit(U32 i);
void eye_enter(U64 self, U32 o);
void eye_scroll(U64 self, U32 i, int step);
void eye_press(U64 self, U32 i);

} // namespace om
