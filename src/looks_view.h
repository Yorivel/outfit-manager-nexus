// The appearance view: the wheel showing one outfit's pieces.
#pragma once
#include "om.h"

namespace om {

extern int g_eye;
// Visibility Extras: the view's rows in parts mode (eye_pieces gives kExtraBase + the row's index)
struct ExtraRow {
    U16 slot, key;
    U64 uid;
    std::string group; // (a part name without its number: "Upperbody_Acc")
};
constexpr size_t kExtraBase = 0x10000;      // (a part group row: + its index in g_extra_rows)
constexpr size_t kExtraPieceBase = 0x20000; // (a piece with extra parts, on a page: + its slot; a click opens its parts)
inline bool is_extra_row(size_t idx) { return idx >= kExtraBase && idx < kExtraPieceBase; }
inline bool is_extra_piece(size_t idx) { return idx >= kExtraPieceBase && idx < kExtraPieceBase + 0x100; }
extern int g_extra_piece; // (the piece whose parts are shown: its slot; -1 the page's pieces)
extern bool g_eye_extras;
extern std::vector<ExtraRow> g_extra_rows;
bool has_extras(const Outfit& o);
std::string part_label(const std::string& group);
extern int g_cfg;
extern std::uint8_t g_eye_step;
extern std::uint8_t g_eye_pick;
bool is_accessory_slot(U16 slot);
bool eye_no_outfit();
Outfit& eye_outfit();
std::vector<size_t> eye_pieces();
bool outfit_has_slot(const Outfit& o, U16 slot);
std::uint8_t slot_look(const Outfit& o, U16 slot);
const OutfitItem* eye_item(size_t idx, OutfitItem& tmp);
std::uint8_t& eye_look(size_t idx);
U32 visible_slots();
U32 slot_outfit(U32 i);
extern U32 g_page, g_page_pick;
bool paged();
U32 page_count();
U32 page_of(size_t o);
void page_show(U32 p);
void pages_on_open();
void pages_on_close();
void eye_enter(U64 self, U32 o, bool extras = false);
void eye_scroll(U64 self, U32 i, int step);
void eye_press(U64 self, U32 i);

} // namespace om
