// What outfit_manager.cpp still holds that the modules use (shrinks as the split goes on).
#pragma once
#include "om.h"

namespace om {

extern std::atomic<int> g_ui_tab_added;
extern RecordGetFn g_qsi_get;
extern OpenFn g_build_orig;
extern VoidFn g_restyle;
extern std::atomic<U32> g_fake_key;
extern U64 g_outfits_root;
U64 qsi_table(U64 root_rva);
void collect_weapon_slots();
bool prepare_fake_record();
extern U64 g_badge_elem[kOutfitSlots];
extern U64 g_low_elem[kOutfitSlots];
extern bool g_low_shown[kOutfitSlots];
extern const char* g_badge_class[kOutfitSlots];
extern std::set<U16> g_keep_slots;
extern std::mutex g_keys_mu;
extern U64 g_mark_elem[kOutfitSlots];
extern bool g_mark_shown[kOutfitSlots];
void reset_modes();
extern std::atomic<U32> g_peek_key;
extern std::atomic<bool> g_peek_pad;
extern U32 g_depth_outfit;
extern int g_force_images;
bool show_slot_image(U64 self, U64 root, U32 i, const std::string& icon);
std::string numbered(const char* fmt, U32 a, U32 b = 0);
extern std::string g_slot_class[kOutfitSlots];
extern std::string g_focus_class[kOutfitSlots];
extern std::string g_worn_class[kOutfitSlots];
extern std::string g_placeholder_class[kOutfitSlots];
extern int g_worn_outfit;
extern bool g_worn_changed;
extern std::map<U16, std::pair<int, int>> g_item_resist;
extern std::atomic<bool> g_pad_in_use, g_pad_is_ps, g_prompt_changed;
const char* prompt_class();
size_t badge_index(const char* c);
bool show_value(U64 root, const std::string& text, int hotkey = 0, bool caps = false);
void apply_outfit_slots(U64 self, U32 only = kOutfitSlots);
bool outfits_current(U64 self);
U32 mode_count(U32 i);
U32 mode_lit(U32 i);
void show_mode_dots(U64 script, U32 count, U32 mode);
void note_pad_handle(LPCWSTR path, HANDLE h);
U64 equipped_item(U64 list, U16 slot);
extern std::vector<U16> g_placeholder_keys;
int read_worn(U64 self, WornRaw* out, int max);
U64 back_shield_item();
int anchor_outfit();
bool left_anchor(const Outfit& o, const WornRaw* w, int n);
void carry_looks(const Outfit& from, Outfit& to);
bool snapshot(U64 self, Outfit& o);
bool same_saved(const Outfit& a, const Outfit& b);
int find_worn_outfit(U64 self);
U64 acquire_player(U64* ctx);
void release_player(U64* ctx);
U16 character_of(U64 actor);
U16 current_character();
U64 find_copy(U64 uid, U16 key);
extern AsyncLoad g_async;
std::string slot_list(const Outfit& o, const std::vector<U16>& slots);
extern std::string g_center_desc;
void center_restore();
U64 focused_script(U64 self);
void center_for_focus(U64 self, U64 script, bool hide_otherwise);
extern std::atomic<bool> g_menu_open;
extern DWORD g_ui_thread;
void refresh_wheel(U64 self);
void watch_game_chars();
std::string piece_label(const Outfit& o, U16 slot);
void finish_load(U64 self, AskedLoad& p);
void peek_key_load(const std::string& id);
const char* peek_key_id();
std::string peek_key_name(U32 vk);
std::string hotkey_name(U32 vk, std::uint8_t mods);
std::uint8_t mods_now();
void pump_async(U64 self);
extern StashCheck g_stash_check;
void stash_verify(U64 self);
std::vector<StashDest> stash_destinations();
extern std::wstring g_log_path;
bool game_focused();

} // namespace om
