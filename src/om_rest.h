// What outfit_manager.cpp still holds that the modules use (shrinks as the split goes on).
#pragma once
#include "om.h"

namespace om {

extern std::atomic<U32> g_peek_key;
extern std::atomic<bool> g_peek_pad;
extern std::atomic<bool> g_pad_in_use, g_pad_is_ps, g_prompt_changed;
void note_pad_handle(LPCWSTR path, HANDLE h);
U64 equipped_item(U64 list, U16 slot);
extern std::vector<U16> g_placeholder_keys;
int read_worn(U64 self, WornRaw* out, int max);
U64 back_shield_item();
int anchor_outfit();
bool left_anchor(const Outfit& o, const WornRaw* w, int n);
void update_plus_icon(U64 self);
void carry_looks(const Outfit& from, Outfit& to);
bool snapshot(U64 self, Outfit& o);
bool same_saved(const Outfit& a, const Outfit& b);
int find_worn_outfit(U64 self);
U64 acquire_player(U64* ctx);
void release_player(U64* ctx);
U16 character_of(U64 actor);
U16 current_character();
std::string where(U64 a);
U64 find_copy(U64 uid, U16 key);
const OutfitItem* outfit_piece(const Outfit& o, U16 slot);
extern AsyncLoad g_async;
extern U64 g_missing_checked;
bool refresh_missing(U64 self);
bool outfit_missing(size_t o);
std::string slot_list(const Outfit& o, const std::vector<U16>& slots);
void watch_game_chars();
std::string piece_label(const Outfit& o, U16 slot);
void finish_load(U64 self, AskedLoad& p);
void peek_key_load(const std::string& id);
const char* peek_key_id();
std::string peek_key_name(U32 vk);
std::string hotkey_name(U32 vk, std::uint8_t mods);
std::uint8_t mods_now();
void start_loading(U64 self, U32 index);
void pump_async(U64 self);
extern StashCheck g_stash_check;
void stash_verify(U64 self);
std::vector<StashDest> stash_destinations();
extern std::wstring g_log_path;
void self_test(U64 self);
void prepare_outfits();
void features_toast(U64 self);
bool game_focused();

} // namespace om
