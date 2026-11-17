// The Outfit Arc and per-outfit hotkeys.
#pragma once
#include "om.h"

namespace om {

extern std::atomic<U32> g_peek_key;
extern std::atomic<U32> g_hotkeys[16];
extern std::atomic<bool> g_peek_pad;
extern std::atomic<bool> g_peek_weapons; // (the Outfit Arc shows the weapon sets, not the outfits: kept, #32)
void peek_key_load(const std::string& id);
const char* peek_key_id();
std::string peek_key_name(U32 vk);
std::string hotkey_name(U32 vk, std::uint8_t mods);
std::uint8_t mods_now();
USHORT peek_mouse_flags(U32 vk);
bool peek_pad_owned();
extern std::atomic<bool> g_peek_live;
extern std::atomic<int> g_peek_clicks; // (#32: left clicks taken while the arc is open)
extern std::atomic<int> g_peek_ups;    // (... and their releases)
extern std::atomic<U64> g_peek_hud_at;
extern std::atomic<int> g_peek_wheel;
void install_peek();

} // namespace om
