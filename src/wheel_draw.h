// The Outfits tab's slots: images, badges, key caps, mode dots, the ring, the centre tooltip.
#pragma once
#include "om.h"

namespace om {

void show_side_badges(U64 root, const char* const kind[2], int where); // (the badge row beside the badge spot)
void badge_tier(U64 root, U32 visible);   // (the badges' size for the slot count)

extern std::uint8_t g_slot_mode[kOutfitSlots];
extern ScrollFn g_scroll_prev_orig, g_scroll_next_orig;
extern SelectorSelectFn g_selector_select;
extern U64 g_badge_elem[kOutfitSlots];
extern U64 g_low_elem[kOutfitSlots];
extern bool g_low_shown[kOutfitSlots];
extern const char* g_badge_class[kOutfitSlots];
extern std::string g_plus_icon;
extern std::string g_worn_chest; // (the worn chest's icon, whatever is worn: the "+" slot's picture)
std::uint8_t slot_mode_of(U32 i);
std::string weapon_set_picture(const Outfit& o); // (#27: the main weapon)
extern int g_worn_set;                            // (#27: the weapon set worn when the slots were last drawn) // (what slot i's lit dot does: the "+" slot's three are Unequip / Save / Settings)
extern bool g_amber;
extern std::set<U16> g_keep_slots;
extern std::mutex g_keys_mu;
extern U64 g_mark_elem[kOutfitSlots];
extern bool g_mark_shown[kOutfitSlots];
void reset_modes();
bool in_depth(U64 self);
extern U32 g_depth_outfit;
extern int g_force_images;
bool show_slot_image(U64 self, U64 root, U32 i, const std::string& icon);
void keep_slot_images(U64 self, bool force);
std::string numbered(const char* fmt, U32 a, U32 b = 0);
extern std::string g_slot_class[kOutfitSlots];
extern std::string g_focus_class[kOutfitSlots];
extern std::string g_worn_class[kOutfitSlots];
extern std::string g_placeholder_class[kOutfitSlots];
extern int g_worn_outfit;
extern bool g_worn_changed;
extern U32 g_applied_visible;
extern std::map<U16, std::pair<int, int>> g_item_resist;
const char* prompt_class();
size_t badge_index(const char* c);
bool show_caps(U64 self, U64 root, U32 vk, std::uint8_t mods, int where);
bool show_value(U64 root, const std::string& text, int hotkey = 0, bool caps = false);
void apply_outfit_slots(U64 self, U32 only = kOutfitSlots);
bool outfits_current(U64 self);
U32 mode_count(U32 i);
U32 mode_lit(U32 i);
extern SelectorSelectHookFn g_selector_select_orig;
extern SlotRefreshFn g_slot_refresh_orig;
void __fastcall slot_refresh_hook(U64 script);
void __fastcall selector_select_hook(U64 sel, U32 index, U64 flag);
void show_mode_dots(U64 script, U32 count, U32 mode);
extern U64 g_ring_bg;
void sync_ring(U64 self);
extern std::string g_center_desc;
extern std::string g_center_name;
extern int g_center_outfit;
extern int g_center_frames;
extern std::vector<U64> g_unhide_retry, g_rehide_retry;
void retry_restore();
void center_restore();
void center_show(U64 self);
void center_hide(U64 self);

} // namespace om
