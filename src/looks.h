// Looks: how each piece shows (the visibility engine).
#pragma once
#include <vector>
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
bool model_loading(U64 model);
void look_settled();
void look_refresh();
extern U64 g_weapon_hide_after;
extern U64 g_sheathe_refresh_at; // (GetTickCount64: a sheathed hidden weapon's glow peak is over - the looks again)
// the sneak peek glow on one hidden weapon's parts (part name ids) as it is drawn (false) or sheathed (true) - transition.cpp
void fx_weapon_glow(const std::vector<U32>& names, bool sheathe);
// the hologram on a scabbard's model part(s) (handles) as its hidden weapon is drawn / sheathed - transition.cpp
void fx_scabbard_holo(const std::vector<U32>& handles, bool sheathe);
bool fx_scab_applied_after(U64 t);
bool fx_transition_busy(); // (an outfit transition runs)
bool transition_on();      // (the Effects setting isn't Off, and they work)
 // (the scabbard hologram went out after tick t)
extern U64 g_case_wait_since; // (GetTickCount64: until then, weapons about to be hidden stay - the transition's glow)
void mark_worn(int index);
extern std::atomic<U32> g_player_hidden_slots; // (bit per equip slot the played character's looks hide)
U32 look_hidden_slots();
U64 player_inventory();
// The helmet key (#34): per character, a pin over the helmet's look (0 none, 1 hidden, 2 shown), kept in the outfit file
constexpr U16 kHelmetSlot = 3; // (equipment-3: the helmet, for Kliff, Damiane and Oongka)
extern std::atomic<U32> g_helmet_vk;
extern std::atomic<std::uint8_t> g_helmet_mods;
std::uint8_t helmet_pin(U16 character);
// What a press would do, not applied: the pin it sets, the helmet shown before and after
struct HelmetPress {
    std::uint8_t pin = 0;
    bool shown_before = true, shown_after = true;
};
HelmetPress helmet_press_plan(U16 character);
void helmet_set(U16 character, std::uint8_t pin);
bool lantern_lit(); // (sound.cpp: the played character's lantern burns)
// present.cpp: a slot's piece taken off the game's presentation (model, physics, part rules) while it stays equipped
bool present_on();
void present_request(U64 inv, U16 slot, bool take_off);
void present_game_thread();
void present_seen(U64 inv, U16 slot);
void present_install();
void anchor_recheck();
extern FrameFn g_hud_frame_orig;
float __fastcall hud_frame_hook(U64 self);

} // namespace om
