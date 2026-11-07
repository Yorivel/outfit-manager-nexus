// The settings pages and their texts; the plugin's settings.
#pragma once
#include "om.h"

namespace om {

extern bool g_stash;
bool stash_of(const Outfit& o);
extern bool g_take_from_companions;
extern bool g_messages;
extern std::uint8_t g_effects; // the outfit effects: 0 off, 1 performance (no glow particles), 2 quality (the default)
extern std::string g_language;
extern std::wstring g_lang_dir;
extern std::string g_detected_language;
const char* character_name(U16 key);
extern std::atomic<bool> g_peek_capture;
std::vector<std::uint8_t> cfg_entries();
std::vector<const char*> cfg_values(std::uint8_t k, U32& cur);
std::string cfg_value_text(std::uint8_t k, const char* v);
std::string cfg_title(std::uint8_t k);
std::string cfg_description(std::uint8_t k);
void cfg_refresh(U64 self);
void cfg_enter(U64 self, U32 o);
void cfg_scroll(U64 self, U32 i, int step);
void cfg_press(U64 self, U32 i);
U32 cfg_dots(U32 i, U32* lit);
void apply_cfg_slot(U64 self, U64 script, U64 root, U32 i, U32 visible);
void peek_capture_frame(U64 self);

} // namespace om
