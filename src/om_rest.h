// What outfit_manager.cpp still holds that the modules use (shrinks as the split goes on).
#pragma once
#include "om.h"

namespace om {

extern int kSlotTextModalClick;
extern int kSlotCommonModalClick;
extern std::atomic<int> g_ui_tab_added;
extern RecordGetFn g_qsi_get;
extern OpenFn g_build_orig;
extern VoidFn g_restyle;
extern std::atomic<U32> g_fake_key;
extern U64 g_outfits_root;
U64 qsi_table(U64 root_rva);
void collect_weapon_slots();
bool prepare_fake_record();
extern std::set<U16> g_keep_slots;
extern std::mutex g_keys_mu;
extern std::atomic<bool> g_peek_pad;
extern int g_force_images;
extern int g_worn_outfit;
extern bool g_worn_changed;
extern std::map<U16, std::pair<int, int>> g_item_resist;
void apply_outfit_slots(U64 self, U32 only = kOutfitSlots);
void note_pad_handle(LPCWSTR path, HANDLE h);
U64 equipped_item(U64 list, U16 slot);
extern std::vector<U16> g_placeholder_keys;
int read_worn(U64 self, WornRaw* out, int max);
U64 back_shield_item();
int anchor_outfit();
bool left_anchor(const Outfit& o, const WornRaw* w, int n);
int find_worn_outfit(U64 self);
U64 acquire_player(U64* ctx);
void release_player(U64* ctx);
U16 character_of(U64 actor);
U16 current_character();
extern bool g_stash;
extern bool g_take_from_companions;
extern bool g_messages;
extern std::string g_language;
U64 find_copy(U64 uid, U16 key);
extern AsyncLoad g_async;
extern std::string g_center_desc;
U64 focused_script(U64 self);
void center_for_focus(U64 self, U64 script, bool hide_otherwise);
extern std::atomic<bool> g_menu_open;
extern DWORD g_ui_thread;
void refresh_wheel(U64 self);
extern std::atomic<U64> g_ask_modal;
void peek_key_load(const std::string& id);
const char* peek_key_id();
std::vector<std::uint8_t> cfg_entries();
void cfg_enter(U64 self, U32 o);
extern bool g_ask_next;
void abort_prompts(const char* why);
void pump_async(U64 self);
extern StashCheck g_stash_check;
void stash_verify(U64 self);
extern bool g_uninstalled;

} // namespace om
