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
bool prepare_fake_record();
extern std::set<U16> g_keep_slots;
extern std::mutex g_keys_mu;
extern std::atomic<bool> g_peek_pad;
extern std::map<U16, std::pair<int, int>> g_item_resist;
void note_pad_handle(LPCWSTR path, HANDLE h);
extern std::vector<U16> g_placeholder_keys;
U64 back_shield_item();
void publish_looks(const Store& s);
extern bool g_stash;
extern bool g_take_from_companions;
extern bool g_messages;
extern std::string g_language;
U64 find_copy(U64 uid, U16 key);
void peek_key_load(const std::string& id);
const char* peek_key_id();
void abort_prompts(const char* why);
extern bool g_uninstalled;

} // namespace om
