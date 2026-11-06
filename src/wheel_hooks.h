// The gear wheel's hooks: the Outfits tab's record, focus, clicks, scrolling, frames.
#pragma once
#include "om.h"

namespace om {

extern std::atomic<int> g_ui_tab_added;
extern RecordGetFn g_qsi_get;
extern OpenFn g_open_orig;
extern TabFn g_tab_orig;
extern OpenFn g_build_orig;
extern OpenFn g_gear_orig;
extern TabFn g_select_orig;
extern TabFn g_close_orig;
extern TabFn g_focus_orig;
extern FloatFn g_frame_orig;
extern VoidFn g_restyle;
extern std::atomic<U32> g_fake_key;
extern U64 g_outfits_root;
U64 qsi_table(U64 root_rva);
U64 __fastcall qsi_get_hook(U64 unused, U32 key);
void collect_weapon_slots();
bool prepare_fake_record();
U32 outfit_slot_index(U64 self, U64 script);
U64 focused_script(U64 self);
void center_for_focus(U64 self, U64 script, bool hide_otherwise);
extern std::atomic<U64> g_space_at;
extern std::atomic<bool> g_menu_open;
extern DWORD g_ui_thread;
void refresh_wheel(U64 self);
extern std::atomic<U64> g_click_at;
extern std::atomic<U64> g_click_pos;
extern float g_prompt_scale;
U64 __fastcall cursor_select_hook(U64 self, U64 cursor, U64 flag, U64 idx, U64 a5, U64 a6);
float __fastcall frame_hook(U64 self);
U64 __fastcall focus_hook(U64 self, U64 script, U64 a, U64 b, U64 c, U64 d);
U64 __fastcall scroll_prev_hook(U64 slot);
U64 __fastcall scroll_next_hook(U64 slot);
U64 __fastcall close_hook(U64 self, U64 a, U64 b, U64 c, U64 d, U64 e);
U64 __fastcall gear_mode_hook(U64 self, U64 a, U64 b, U64 c);
extern bool g_tab_checked;
U64 __fastcall open_hook(U64 self, U64 a, U64 b, U64 c);
U64 __fastcall tab_hook(U64 self, U64 tabbar, U64 flag, U64 old_idx, U64 new_idx, U64 header);

} // namespace om
