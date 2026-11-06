// Looks: how each piece shows (the visibility engine).
#pragma once
#include "om.h"

namespace om {

extern VisSetFn g_vis_set;
extern VisReleaseFn g_vis_release;
extern BattleVisFn g_battle_vis_orig;
extern SlotHandleFn g_slot_handle;
void publish_looks(const Store& s);
U64 look_model(U64 actor);
std::string part_name(U32 index);
extern BattleCheckFn g_battle_check;
void __fastcall battle_vis_hook(U64 inv, std::uint8_t show, std::uint8_t show2);
extern VisSetFn g_vis_set_orig;
extern VisReleaseFn g_vis_release_orig;
std::uint8_t __fastcall vis_set_hook(U64 model, U64 list, std::uint8_t shown, std::uint8_t layer, U64 a, U64 b, std::uint8_t c);
std::uint8_t __fastcall vis_release_hook(U64 model, U64 list, std::uint8_t layer, std::uint8_t a, std::uint8_t b);
extern EquipVisCheckFn g_equip_vis_check_orig;
float __fastcall equip_vis_check_hook(U64 vc, const U32* part, std::uint8_t* rec, std::uint8_t drawn);
extern ModelSwapFn g_model_swap_orig;
extern std::atomic<bool> g_worn_dirty;
void __fastcall model_swap_hook(U64 job, U64 a, U64 b, char c, std::uint8_t* state);
void look_pending();
void look_hold(bool on);
bool look_changing();
void look_settled();
void look_refresh();
void mark_worn(int index);
void anchor_recheck();
extern FrameFn g_hud_frame_orig;
float __fastcall hud_frame_hook(U64 self);

} // namespace om
