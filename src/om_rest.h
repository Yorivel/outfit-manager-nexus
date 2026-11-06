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
extern std::mutex g_keys_mu;
extern std::map<U16, std::pair<int, int>> g_item_resist;
extern std::vector<U32> g_placeholder_ids;
void save_outfits();
extern std::vector<U16> g_placeholder_keys;
U64 back_shield_item();
extern bool g_messages;
U64 find_copy(U64 uid, U16 key);

} // namespace om
