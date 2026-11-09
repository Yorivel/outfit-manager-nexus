// The settings pages and their texts; the plugin's settings.
#include "om.h"
#include "settings.h"
#include "addrs.h"
#include "ui.h"
#include "items.h"
#include "store.h"
#include "looks.h"
#include "looks_view.h"
#include "load.h"
#include "stash.h"
#include "prompts.h"
#include "actions.h"
#include "wheel_draw.h"
#include "wheel_hooks.h"
#include "input.h"
#include "pads.h"
#include "arc.h"

namespace om {

std::string stash_description(const Outfit& o);

std::pair<std::string, std::string> entry_text(int page, U32 index)
{
    if (index == 2) return {lang::t("setting.back"), lang::t("desc.back")};
    if (page == kPageOutfit) {
        if (index == 0) return {lang::t("setting.rename"), lang::t("desc.rename")};
        if (index == 1) return {lang::t("setting.delete"), lang::t("desc.delete")};
    } else if (page == kPageSettings) {
        if (index == 3) return {"", lang::t(g_stash ? "desc.gstash.on" : "desc.gstash.off")};
        if (index == 0) return {"", lang::t(g_take_from_companions ? "desc.party.take" : "desc.party.leave")};
        if (index == 1) return {"", lang::t(g_messages ? "desc.messages.on" : "desc.messages.off")};
    } else if (page == kPageSettings2) {
        if (index == 0) return {lang::t("setting.clear"), lang::t("desc.clear")};
        if (index == 1) return {lang::t("setting.uninstall"), lang::t("desc.uninstall")};
    } else if (page == kPageConditions && g_depth_outfit < g_outfits.size()) {
        const Outfit& o = g_outfits[g_depth_outfit];
        if (index == 3) {
            if (o.climate == 1) return {"", lang::t("desc.climate.cold")};
            if (o.climate == 2) return {"", lang::t("desc.climate.heat")};
            if (o.climate == 3) return {"", lang::t("desc.climate.off")};
            int ice = 0, fire = 0;
            outfit_resistance(o, ice, fire);
            return {"", lang::f("desc.climate.auto", {{"ice", std::to_string(ice)}, {"fire", std::to_string(fire)}})};
        }
        if (index == 0) {
            if (!o.has_weapons) return {"", lang::t("desc.weapons.none")};
            return {"", lang::t(o.weapons_only ? "desc.weapons.only" : o.load_weapons ? "desc.weapons.load" : "desc.weapons.leave")};
        }
        if (index == 1) return {"", stash_description(o)};
    }
    return {"", ""};
}

// --- Bags and settings --------------------------------------------------------------------------
// One inventory for all three characters (each wears its own gear). Bag ids: 1 carried items, 4 key items (both "on
// you"), 7 private storage, 14 wardrobe, 13 a hidden bag (never used); the others hold no gear.
// User settings (the "+" slot's settings view).
bool g_stash = false;               // after a load, saved pieces left in the bag go to storage (off until turned on)
bool stash_of(const Outfit& o) { return o.stash ? o.stash == 1 : g_stash; } // (its own, or the global)
bool g_take_from_companions = true; // pieces another character wears: take them (asked first)
std::map<U16, int> g_unmanaged;    // (#21: the outfit let go, per character - see om.h)
bool let_go(int outfit)
{
    (void)outfit; // (any outfit: one matched by hand would hide pieces by its looks unasked - user, 2026-11-07)
    return g_unmanaged.count(g_character) != 0;
}
std::uint8_t g_effects = 2;         // the outfit effects (transition.cpp): 0 off, 1 performance, 2 quality
int transition_setting() { return g_effects; }
bool g_messages = true;             // the after-the-fact reports (pieces left, not taken, overwritten); "+" settings page
std::string g_language;             // the mod's language: a lang::kLanguages code, "" = the game's (detected at start)
std::wstring g_lang_dir;            // the language files: <plugin folder>/Outfit Manager/lang/
std::string g_detected_language = "en"; // the game's, read at start (its options file, Steam)

// Loads the chosen language (the setting, else the game's) unless it is loaded already.
void apply_language()
{
    std::string want = g_language.empty() ? g_detected_language : g_language;
    if (want != lang::code()) lang::load(g_lang_dir, want);
}

// Character keys -> names (keys are game data, the same in every save).
const char* character_name(U16 key)
{
    switch (key) {
    case 0: return lang::t("character.kliff");
    case 3: return lang::t("character.damiane");
    case 5: return lang::t("character.oongka");
    }
    return lang::t("character.other");
}

// --- Settings view: an outfit's settings (or the plugin's, from the "+" slot) on the wheel ---------------------------
// The second scroll mode of a slot opens it (no submenu). Its segments: a setting's name and value (its dots are the
// choices: scroll to change) or an action (the click prompt), then Back (left of the top). The centre names the
// highlighted setting, its value and what it does.

bool g_overwrite_reset = false; // the settings view's Overwrite: false keeps the outfit's settings, true resets them

// The Outfit Arc's key (the "+" page's Outfit Arc; Off by default: no key is free in every layout, so the player sets
// one). A click on the setting waits for the next key or mouse button (left / right excepted: they click), read with
// GetAsyncKeyState so it works however the game reads its input; Esc cancels, Backspace / Delete turn it off. Held: the
// arc shows; the wheel moves along it; released: the highlighted outfit goes on. The key and, while the arc is open, the
// wheel are kept from the game (raw_input_hook, name_wndproc).
std::atomic<bool> g_peek_capture{false}; // the setting waits for a key
int g_capture_outfit = -1;               // ... for that outfit's Hotkey (-1: for the Outfit Arc's key)
U32 g_peek_capture_slot = 0;             // its slot (refreshed when the key comes)
bool g_peek_capture_held[256] = {};      // keys already down when it started waiting (not taken as the answer)

std::vector<std::uint8_t> cfg_entries()
{
    if (g_cfg < 0) return {};
    if (size_t(g_cfg) < g_outfits.size()) {
        if (is_weapon_set(g_outfits[size_t(g_cfg)])) // (a weapon set: no looks, climate or stash, #27)
            return {kCfgWeapons, kCfgHotkey, kCfgPosition, kCfgRename, kCfgOverwrite, kCfgDelete, kCfgBack};
        return {kCfgVisibility, kCfgClimate, kCfgSleepwear, kCfgWeapons, kCfgStash, kCfgHotkey, kCfgPosition, kCfgRename,
                kCfgOverwrite, kCfgDelete, kCfgBack};
    }
    // (the Outfit Arc's binding for the input in use: the key, or the pad's; pump re-shows the page when it changes)
    return {kCfgGStash, kCfgParty, kCfgMessages, kCfgEffects, kCfgLanguage, g_pad_in_use ? kCfgPeekPad : kCfgPeekKey,
            kCfgClear, kCfgUninstall, kCfgBack};
}

const char* cfg_name(std::uint8_t k)
{
    static const char* const n[] = {"setting.visibility", "setting.climate", "setting.weapons", "setting.stash",
                                    "setting.position",   "setting.rename",  "setting.overwrite", "setting.delete",
                                    "setting.stash",      "setting.party",   "setting.messages", "setting.clear",
                                    "setting.uninstall",  "setting.back",    "setting.language", "setting.peekkey",
                                    "setting.peekpad",    "setting.hotkey",    "setting.effects",
                                    "setting.sleepwear"};
    return k <= kCfgSleepwear ? lang::t(n[k]) : "";
}

// A text that stays where it is (the settings' values are kept as pointers between refreshes: a reused std::string's
// buffer moves once the text outgrows it, 2026-11-06: "Ctrl + Shift + 1" showed leftover memory). Each distinct text
// is kept once, for the session (a few key names).
const char* stable_text(const std::string& t)
{
    static std::set<std::string> kept;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    return kept.insert(t).first->c_str();
}

// A value setting's choices (their line keys; the Language setting: Auto and each language's own name) and the current
// one (none: an action).
std::vector<const char*> cfg_values(std::uint8_t k, U32& cur)
{
    cur = 0;
    if (size_t(g_cfg) < g_outfits.size()) {
        const Outfit& o = g_outfits[size_t(g_cfg)];
        if (k == kCfgClimate) { // (shown Auto / Off / Cold / Heat; stored 0 Auto, 3 Off, 1 Cold, 2 Heat)
            static const U32 pos[4] = {0, 2, 3, 1};
            cur = pos[o.climate & 3];
            return {"value.auto", "value.off", "value.cold", "value.heat"};
        }
        if (k == kCfgWeapons) {
            cur = is_weapon_set(o) ? 2 : uses_weapons(o) ? 0 : 1;
            return {"value.load", "value.leave", "value.weapons_only"};
        }
        if (k == kCfgStash) {
            cur = o.stash % 3;
            return {"value.default", "value.on", "value.off"};
        }
        if (k == kCfgSleepwear) {
            cur = o.sleepwear ? 1 : 0;
            return {"value.off", "value.on"};
        }
        if (k == kCfgHotkey) { // (one value: the hotkey, or the wait for one)
            bool waiting = g_peek_capture && g_capture_outfit == g_cfg;
            return {waiting ? "value.press_key" : stable_text(hotkey_name(o.hk_vk, o.hk_mods))};
        }
        if (k == kCfgOverwrite) { // (an action with a choice: scrolled, then clicked)
            cur = g_overwrite_reset ? 1 : 0;
            return {"value.keep_settings", "value.reset_all"};
        }
    }
    if (k == kCfgGStash) return cur = g_stash ? 1 : 0, std::vector<const char*>{"value.off", "value.on"};
    if (k == kCfgParty) return cur = g_take_from_companions ? 1 : 0, std::vector<const char*>{"value.leave", "value.take"};
    if (k == kCfgMessages) return cur = g_messages ? 1 : 0, std::vector<const char*>{"value.off", "value.on"};
    if (k == kCfgEffects)
        return cur = g_effects, std::vector<const char*>{"value.off", "value.performance", "value.quality"};
    if (k == kCfgPeekPad) return cur = g_peek_pad ? 1 : 0, std::vector<const char*>{"value.off", "value.peekpad"};
    if (k == kCfgPeekKey) // (one value: the key, or the wait for one)
        return {g_peek_capture && g_capture_outfit < 0 ? "value.press_key" : stable_text(peek_key_name(g_peek_key.load()))};
    if (k == kCfgLanguage) {
        std::vector<const char*> v{"value.auto_language"};
        for (size_t i = 0; i < lang::kLanguageCount; ++i) {
            v.push_back(lang::kLanguages[i].code);
            if (g_language == lang::kLanguages[i].code) cur = U32(i + 1);
        }
        return v;
    }
    return {};
}

// A value as shown (the Language setting's: the language's own name; Auto: with the game's).
std::string cfg_value_text(std::uint8_t k, const char* v)
{
    if (k == kCfgPeekKey || k == kCfgHotkey) // (a line key, or the key's own label)
        return !std::strcmp(v, "value.press_key") || !std::strcmp(v, "value.no_hotkey") ? lang::t(v) : v;
    if (k == kCfgPeekPad) return lang::f(v, {{"button", g_pad_is_ps ? "L1" : "LB"}}); // (the pad in use's own name)
    if (k != kCfgLanguage) return lang::t(v);
    std::string auto_code = std::string(v) == "value.auto_language" ? g_detected_language : v;
    const char* name = "English";
    for (size_t i = 0; i < lang::kLanguageCount; ++i)
        if (auto_code == lang::kLanguages[i].code) name = lang::kLanguages[i].name;
    return std::string(v) == "value.auto_language" ? lang::f("value.auto_language", {{"language", name}}) : name;
}

// The setting's name on its slot and in the centre; Position shows the outfit's place (no dots: there can be many).
std::string cfg_title(std::uint8_t k)
{
    if (k != kCfgPosition) return cfg_name(k);
    return lang::f("setting.position", {{"n", std::to_string(g_cfg + 1)}, {"total", std::to_string(g_outfits.size())}});
}

// What the setting does (the centre's caption).
std::string cfg_description(std::uint8_t k)
{
    std::pair<std::string, std::string> t;
    if (k == kCfgPosition) return lang::t("desc.position");
    if (size_t(g_cfg) < g_outfits.size()) {
        g_depth_outfit = static_cast<U32>(g_cfg); // (entry_text's outfit)
        if (k == kCfgClimate) t = entry_text(kPageConditions, 3);
        else if (k == kCfgWeapons) t = entry_text(kPageConditions, 0);
        else if (k == kCfgStash) t = entry_text(kPageConditions, 1);
        else if (k == kCfgSleepwear) t.second = lang::t(g_outfits[size_t(g_cfg)].sleepwear ? "desc.sleepwear.on" : "desc.sleepwear.off");
        else if (k == kCfgRename) t = entry_text(kPageOutfit, 0);
        else if (k == kCfgOverwrite)
            t.second = lang::t(g_overwrite_reset ? "desc.overwrite.reset" : "desc.overwrite.keep");
        else if (k == kCfgDelete) t = entry_text(kPageOutfit, 1);
        else if (k == kCfgVisibility) t.second = lang::t("desc.visibility");
        else if (k == kCfgHotkey) {
            const Outfit& o = g_outfits[size_t(g_cfg)];
            t.second = g_peek_capture && g_capture_outfit == g_cfg ? lang::t("desc.hotkey.wait")
                       : o.hk_vk ? lang::f("desc.hotkey.on", {{"hotkey", hotkey_name(o.hk_vk, o.hk_mods)}})
                                 : lang::t("desc.hotkey.off");
        }
    } else {
        if (k == kCfgGStash) t = entry_text(kPageSettings, 3);
        else if (k == kCfgParty) t = entry_text(kPageSettings, 0);
        else if (k == kCfgMessages) t = entry_text(kPageSettings, 1);
        else if (k == kCfgEffects)
            t.second = lang::t(g_effects == 0 ? "desc.effects.off" : g_effects == 1 ? "desc.effects.performance"
                                                                                     : "desc.effects.quality");
        else if (k == kCfgClear) t = entry_text(kPageSettings2, 0);
        else if (k == kCfgUninstall) t = entry_text(kPageSettings2, 1);
        else if (k == kCfgLanguage) t.second = lang::t("desc.language");
        else if (k == kCfgPeekPad)
            t.second = lang::f(g_peek_pad ? "desc.peekpad.on" : "desc.peekpad.off", {{"button", g_pad_is_ps ? "L1" : "LB"}});
        else if (k == kCfgPeekKey)
            t.second = lang::t(g_peek_capture ? "desc.peekkey.wait" : g_peek_key.load() ? "desc.peekkey.on" : "desc.peekkey.off");
    }
    if (k == kCfgBack) t.second = lang::t("desc.back");
    return t.second;
}

void cfg_refresh(U64 self)
{
    g_force_images = 3;
    refresh_wheel(self);
    center_for_focus(self, focused_script(self), true);
}

void cfg_enter(U64 self, U32 o)
{
    g_eye = -1, g_cfg = static_cast<int>(o), g_overwrite_reset = false;
    LOG("settings view: %s", o < g_outfits.size() ? ("\"" + g_outfits[o].name + "\"").c_str() : "the plugin's");
    cfg_refresh(self);
}

void cfg_leave(U64 self)
{
    LOG("settings view: back to the outfits");
    g_cfg = -1;
    g_center_desc.clear();
    reset_modes();
    cfg_refresh(self);
}

// Position: the outfit changes places with its neighbour (no wrap at the ends); the view stays on it, the worn outfit's
// index follows. Prompts and a load in progress hold a name they check, but a load's late parts use the index: not then.
void cfg_move(U64 self, U32 i, int step)
{
    int from = g_cfg, to = from + (step > 0 ? 1 : -1);
    if (from < 0 || to < 0 || size_t(to) >= g_outfits.size() || size_t(from) >= g_outfits.size() || g_async.active) return;
    std::swap(g_outfits[size_t(from)], g_outfits[size_t(to)]);
    g_cfg = to;
    if (g_worn_outfit == from) g_worn_outfit = to;
    else if (g_worn_outfit == to) g_worn_outfit = from;
    LOG("settings \"%s\": position %d -> %d", g_outfits[size_t(to)].name.c_str(), from + 1, to + 1);
    save_outfits();
    apply_outfit_slots(self, i);
    center_for_focus(self, focused_script(self), true);
}

// A scroll on a value setting moves it on / back (and saves); on the rest nothing.
void cfg_scroll(U64 self, U32 i, int step)
{
    std::vector<std::uint8_t> e = cfg_entries();
    U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (k >= e.size()) return;
    if (e[k] == kCfgPosition) return cfg_move(self, i, step);
    U32 cur = 0;
    std::vector<const char*> v = cfg_values(e[k], cur);
    if (v.empty()) return;
    U32 to = U32((cur + v.size() + step) % v.size());
    if (e[k] == kCfgOverwrite) { // (how the next Overwrite goes: nothing saved)
        g_overwrite_reset = to == 1;
        LOG("settings view: Overwrite -> %s", v[to]);
        apply_outfit_slots(self, i);
        center_for_focus(self, focused_script(self), true);
        return;
    }
    if (size_t(g_cfg) < g_outfits.size()) {
        Outfit& o = g_outfits[size_t(g_cfg)];
        if (e[k] == kCfgClimate) {
            static const std::uint8_t val[4] = {0, 3, 1, 2};
            o.climate = val[to];
        } else if (e[k] == kCfgWeapons) {
            if (!o.has_weapons) {
                notify(self, lang::f("toast.no_weapons", {{"outfit", o.name}}));
                return;
            }
            o.load_weapons = to != 1;
            o.weapons_only = to == 2;
            if (o.weapons_only && o.worn) mark_worn(-1); // (a weapon set is never the applied outfit)
        } else if (e[k] == kCfgStash) {
            o.stash = static_cast<std::uint8_t>(to);
        } else if (e[k] == kCfgSleepwear) {
            o.sleepwear = to == 1;
        }
        LOG("settings \"%s\": %s -> %s", o.name.c_str(), cfg_name(e[k]), v[to]);
    } else {
        if (e[k] == kCfgGStash) g_stash = to == 1;
        else if (e[k] == kCfgParty) g_take_from_companions = to == 1;
        else if (e[k] == kCfgMessages) g_messages = to == 1;
        else if (e[k] == kCfgEffects) g_effects = static_cast<std::uint8_t>(to);
        else if (e[k] == kCfgPeekPad) g_peek_pad = to == 1;
        else if (e[k] == kCfgLanguage) {
            g_language = to == 0 ? std::string() : std::string(v[to]);
            apply_language();
            if (U64 h = outfits_header(self)) set_label(h); // (the tab's name)
        }
        LOG("settings: %s -> %s", cfg_name(e[k]), v[to]);
    }
    save_outfits();
    if (cfg_entries().size() != e.size()) cfg_refresh(self); // (to / from Weapons only: the page's entries change, #27)
    else apply_outfit_slots(self, i);
    center_for_focus(self, focused_script(self), true);
}

// A click on an action: Visibility opens the outfit's pieces; the others the existing prompts (Delete and the "+" slot's
// clean-ups leave the view first: their answer changes the list).
void cfg_press(U64 self, U32 i)
{
    std::vector<std::uint8_t> e = cfg_entries();
    U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (k >= e.size()) return;
    U32 o = static_cast<U32>(g_cfg);
    LOG("settings view: %s pressed", cfg_name(e[k]));
    switch (e[k]) {
    case kCfgVisibility: g_cfg = -1, eye_enter(self, o); break;
    case kCfgRename: open_rename_prompt(self, o); break;
    case kCfgOverwrite: overwrite_pressed(self, o, g_overwrite_reset); break;
    case kCfgDelete: cfg_leave(self), open_confirm(self, o, kConfirmDelete, 0); break;
    case kCfgClear: cfg_leave(self), open_clean_confirm(self, kConfirmCleanSave); break;
    case kCfgUninstall: cfg_leave(self), open_clean_confirm(self, kConfirmUninstall); break;
    case kCfgBack: cfg_leave(self); break;
    case kCfgPeekKey: // (waits for the key: peek_capture_frame)
    case kCfgHotkey:
        for (int vk = 0; vk < 256; ++vk) g_peek_capture_held[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        g_capture_outfit = e[k] == kCfgHotkey ? int(o) : -1;
        g_peek_capture_slot = i;
        g_peek_capture = true;
        apply_outfit_slots(self, i);
        center_for_focus(self, focused_script(self), true);
        break;
    default: break; // (a value setting: scrolled)
    }
}

// The dots of slot i in the settings view: a value setting's choices (lit: the current one); none for the rest.
U32 cfg_dots(U32 i, U32* lit)
{
    std::vector<std::uint8_t> e = cfg_entries();
    U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (k >= e.size()) return 0;
    U32 cur = 0;
    std::vector<const char*> v = cfg_values(e[k], cur);
    if (lit) *lit = cur;
    return e[k] == kCfgLanguage || e[k] == kCfgPeekKey || e[k] == kCfgHotkey ? 0 : static_cast<U32>(v.size()); // (the value says it)
}

// Slot i of the settings view: the setting's name (and value) on the slot's text, its dots, the click prompt for an
// action; Back: its label and the click prompt, like an action.
void apply_cfg_slot(U64 self, U64 script, U64 root, U32 i, U32 visible)
{
    std::vector<std::uint8_t> e = cfg_entries();
    U32 k = i < visible ? slot_outfit(i) : kOutfitSlots;
    bool entry = k < e.size();
    std::uint8_t kind = entry ? e[k] : kCfgBack;
    U32 cur = 0;
    std::vector<const char*> vals = entry ? cfg_values(kind, cur) : std::vector<const char*>{};
    bool back = entry && kind == kCfgBack, action = entry && !back && vals.empty() && kind != kCfgPosition;
    show_slot_image(self, root, i, std::string());
    if (i < visible) show_mode_dots(script, mode_count(i), mode_lit(i));
    U64 badge = find_by_class(root, "quickslot-occupancy-icon-chain"), low = find_by_class(root, "cpp-icon-disable-item");
    const char* want = action || back ? prompt_class() : nullptr;
    g_badge_elem[i] = badge, g_badge_class[i] = want ? kBadges[badge_index(want)] : nullptr;
    bool ok = true;
    if (badge) {
        ok &= set_class(badge, "self-center", true);
        for (const char* c : kBadges) ok &= set_class(badge, c, want && !std::strcmp(want, c));
        ok &= set_class(badge, "!cpp-none", !want);
    }
    U64 mark = find_parent_of_class(root, "charged-count");
    bool label = entry; // (Back too: "Back")
    if (mark) {
        ok &= set_class(mark, "om-eye-c", false) && set_class(mark, "om-eye2-c", false);
        ok &= set_class(mark, "self-center", true) && set_class(mark, "om-label-wrap", label);
        ok &= set_class(mark, "!cpp-none", !label);
        if (U64 text = find_by_class(mark, "charged-count")) {
            std::string t = label ? cfg_title(kind) : "";
            if (g_set_text) g_set_text(text, t.c_str(), 0);
            ok &= set_class(text, "om-label", label) && set_class(text, "om-label-value", false);
        }
    }
    g_mark_elem[i] = mark, g_mark_shown[i] = mark && label;
    ok &= show_value(root, vals.empty() ? std::string() : cfg_value_text(kind, vals[cur]));
    g_slot_image[i].missing = false;
    ok &= set_class(root, "!cpp-none", i >= visible);
    ok &= swap_class(root, g_slot_class[i], i < visible ? numbered("om-s%u-%u", visible, i) : "");
    U64 icon = rp(script + kOffScriptCategoryIcon);
    if (icon && is_element(icon)) {
        ok &= set_class(icon, "quickslot-item-image-none", false);
        ok &= swap_class(icon, g_placeholder_class[i], "");
        ok &= set_class(icon, "om-empty-armor", false);
        ok &= set_class(icon, "om-empty-sword", false) && set_class(icon, "om-empty-necklace", false);
        ok &= set_class(icon, "!cpp-none", true);
        ok &= set_class(icon, "om-missing", false);
    }
    g_low_elem[i] = low, g_low_shown[i] = false;
    if (low) ok &= swap_class(low, g_worn_class[i], "") && set_class(low, "!cpp-none", true);
    if (U64 focus = find_by_class(root, "quickslot-item-focus-bg-10"))
        ok &= swap_class(focus, g_focus_class[i], i < visible ? numbered("om-f%u-%u", visible, i) : "");
    if (!ok) LOG("  settings slot %u: class change skipped (%s)", i, describe(root).c_str());
}

// --- The submenu pages (entries: 0 right, 1 bottom, 2 left = Back, 3 top) --------------------------------------------
// The outfit's Stash, explained; with the pieces another outfit keeps in the bag (Off wins for a shared piece).
std::string stash_description(const Outfit& o)
{
    bool on = stash_of(o);
    std::string s = lang::t(!o.stash ? (g_stash ? "desc.stash.default_on" : "desc.stash.default_off")
                            : on      ? "desc.stash.on"
                                      : "desc.stash.off");
    if (!on) return s;
    int held = 0;
    std::string by;
    for (const auto& [uid, key, away] : stash_destinations())
        if (!away)
            for (const OutfitItem& it : o.items)
                if (it.uid == uid && it.key == key && loads_piece(o, it.slot)) {
                    ++held;
                    for (const Outfit& other : g_outfits) // (the outfit that keeps it, on this character)
                        if (by.empty() && &other != &o && !stash_of(other))
                            for (const OutfitItem& x : other.items)
                                if (x.uid == uid && x.key == key && loads_piece(other, x.slot)) by = other.name;
                }
    if (held) s += " " + (by.empty() ? lang::n("desc.stash.kept_other", held) : lang::n("desc.stash.kept_by", held, {{"outfit", by}}));
    return s;
}

// A saved outfit's settings, as words.
const char* climate_text(const Outfit& o)
{
    static const char* const t[4] = {"Climate: Auto", "Climate: Cold", "Climate: Heat", "Climate: Off"};
    return t[o.climate & 3];
}
const char* weapons_text(const Outfit& o) { return uses_weapons(o) ? "Weapons: Load" : "Weapons: Leave"; }
const char* outfit_stash_text(const Outfit& o)
{
    static const char* const k[3] = {"Stash: Default", "Stash: On", "Stash: Off"};
    return k[o.stash % 3];
}

const char* stash_text() { return g_stash ? "Stash: On" : "Stash: Off"; }

// Runs on the UI thread from the menu's frequent callbacks.
// The Outfit Arc setting waiting for a key: the next key or mouse button newly down (left / right excepted).
void peek_capture_frame(U64 self)
{
    if (!g_peek_capture) return;
    int got = -1;
    for (int vk = 1; vk < 256 && got < 0; ++vk) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN)
            continue; // (the modifiers: held with a hotkey's key, never the key)
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !g_peek_capture_held[vk]) got = vk;
        g_peek_capture_held[vk] = down;
    }
    if (got < 0 || !game_focused()) return;
    g_peek_capture = false;
    if (g_capture_outfit >= 0) { // an outfit's Hotkey
        size_t o = size_t(g_capture_outfit);
        g_capture_outfit = -1;
        if (o >= g_outfits.size()) return;
        if (got == VK_ESCAPE) {
            LOG("settings \"%s\": hotkey: cancelled", g_outfits[o].name.c_str());
        } else if (got == VK_BACK || got == VK_DELETE) {
            g_outfits[o].hk_vk = 0, g_outfits[o].hk_mods = 0;
            LOG("settings \"%s\": hotkey cleared", g_outfits[o].name.c_str());
            save_outfits();
        } else if (U32(got) == g_peek_key.load()) { // (it opens the Outfit Arc: not also a hotkey)
            notify(self, lang::f("toast.hotkey_arc", {{"hotkey", peek_key_name(U32(got))}}));
        } else {
            std::uint8_t m = mods_now();
            for (size_t j = 0; j < g_outfits.size(); ++j) // (one outfit per hotkey: it moves here)
                if (j != o && g_outfits[j].hk_vk == got && g_outfits[j].hk_mods == m) {
                    g_outfits[j].hk_vk = 0, g_outfits[j].hk_mods = 0;
                    notify(self, lang::f("toast.hotkey_moved", {{"hotkey", hotkey_name(U32(got), m)}, {"outfit", g_outfits[j].name}}));
                }
            g_outfits[o].hk_vk = static_cast<U16>(got), g_outfits[o].hk_mods = m;
            LOG("settings \"%s\": hotkey -> %s (%d, mods %u)", g_outfits[o].name.c_str(), hotkey_name(U32(got), m).c_str(), got, m);
            save_outfits();
        }
        apply_outfit_slots(self, g_peek_capture_slot);
        center_for_focus(self, focused_script(self), true);
        return;
    }
    if (got == VK_ESCAPE) LOG("settings: Outfit Arc key: cancelled");
    else {
        g_peek_key = got == VK_BACK || got == VK_DELETE ? 0 : U32(got);
        LOG("settings: Outfit Arc key -> %s (%d)", peek_key_name(g_peek_key.load()).c_str(), g_peek_key.load());
        save_outfits();
    }
    apply_outfit_slots(self, g_peek_capture_slot);
    center_for_focus(self, focused_script(self), true);
}

} // namespace om
