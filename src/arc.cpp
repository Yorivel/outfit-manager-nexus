// The Outfit Arc and per-outfit hotkeys.
#include "charlook.h"
#include "om.h"
#include "arc.h"
#include "addrs.h"
#include "ui.h"
#include "items.h"
#include "gear.h"
#include "looks_view.h"
#include "load.h"
#include "prompts.h"
#include "settings.h"
#include "wheel_draw.h"
#include "wheel_hooks.h"
#include "input.h"
#include "pads.h"

#include "looks.h"
#include "anim.h"
#include "store.h"
#include <atomic>
#include <algorithm>
#include <set>

namespace om {

std::atomic<U32> g_peek_key{0};           // the Outfit Arc's key (a virtual-key code; 0 = Off)

// The current character's hotkeys for the input filters (another thread): vk | mods << 8 per outfit, 0 = none.
std::atomic<U32> g_hotkeys[16] = {};
std::atomic<bool> g_peek_pad{false};       // the Outfit Arc on controllers: hold L1 / LB, press the d-pad's down
std::atomic<bool> g_peek_weapons{false};   // the Arc's mode: the weapon sets (a press on No Outfit), else the outfits

// Older builds of 5 November stored names; now the key code.
void peek_key_load(const std::string& id)
{
    static const std::pair<const char*, int> old[] = {{"tab", VK_TAB}, {"caps", VK_CAPITAL}, {"backquote", VK_OEM_3},
                                                       {"l", 'L'}, {"mouse3", VK_MBUTTON}, {"mouse4", VK_XBUTTON1},
                                                       {"mouse5", VK_XBUTTON2}};
    for (const auto& [n, vk] : old)
        if (id == n) return void(g_peek_key = vk);
    int vk = std::atoi(id.c_str());
    g_peek_key = vk > 0 && vk < 256 ? vk : 0;
}
const char* peek_key_id()
{
    static char buf[8];
    U32 vk = g_peek_key.load();
    if (!vk) return nullptr;
    snprintf(buf, sizeof buf, "%u", vk);
    return buf;
}

// The key's name as the keyboard layout calls it (the mouse buttons: our lines).
std::string peek_key_name(U32 vk)
{
    if (!vk) return lang::t("value.off");
    if (vk == VK_MBUTTON) return lang::t("key.mouse3");
    if (vk == VK_XBUTTON1) return lang::t("key.mouse4");
    if (vk == VK_XBUTTON2) return lang::t("key.mouse5");
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) { // (keys Windows names by their numpad twin unless flagged extended)
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE:
        sc |= 0x100;
    }
    wchar_t w[64] = {};
    int n = sc ? GetKeyNameTextW(static_cast<LONG>(sc << 16), w, 64) : 0;
    if (n <= 0) return "Key " + std::to_string(vk);
    char u[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, n, u, sizeof u - 1, nullptr, nullptr);
    return u;
}

// A hotkey as shown: "Ctrl + Shift + 3" (the modifiers' names as the keyboard layout calls them).
constexpr std::uint8_t kModCtrl = 1, kModAlt = 2, kModShift = 4;
std::string hotkey_name(U32 vk, std::uint8_t mods)
{
    if (!vk) return lang::t("value.no_hotkey");
    std::string s;
    for (auto [bit, mvk] : {std::pair<std::uint8_t, U32>{kModCtrl, VK_CONTROL}, {kModAlt, VK_MENU}, {kModShift, VK_SHIFT}})
        if (mods & bit) s += peek_key_name(mvk) + " + ";
    return s + peek_key_name(vk);
}

// The modifiers held now (exactly these must be held for a hotkey).
std::uint8_t mods_now()
{
    return static_cast<std::uint8_t>(((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? kModCtrl : 0) |
                                     ((GetAsyncKeyState(VK_MENU) & 0x8000) ? kModAlt : 0) |
                                     ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? kModShift : 0));
}

// The mouse button's raw input flags (0: a keyboard key).
USHORT peek_mouse_flags(U32 vk)
{
    if (vk == VK_MBUTTON) return RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_UP;
    if (vk == VK_XBUTTON1) return RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP;
    if (vk == VK_XBUTTON2) return RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_5_UP;
    return 0;
}

// --- Quick peek ----------------------------------------------------------------------------------
// The outfits on an arc beside the character, to switch without the gear wheel. Its elements are added to the stamina
// gauge's tracker (statusgaugeview2) as the game loads it (uibin_patch.cpp: OmPeekWrap > OmPeekArc, OmPeekFrame0-4,
// OmPeekIcon0-4, OmPeekLabel, OmPeekName); tools/peek_gen.py makes the layout. A carousel: the highlighted entry sits
// in the middle (position 2), the others around it, wrapping. A state picker (#21): "No outfit" first, then the outfits;
// it opens on the state there is now (the outfit applied, or No outfit) and only acts when let go on another entry.
// It follows the character (in the HP tracker, which the game never shows or hides: the plugin shows it while the peek
// is, its own gauge hidden; the stamina tracker flickered, the game hiding it while stamina is full). Everything runs in the screen's own per-frame call (its root script
// UIGamePlayControlRootStatusGauge, vtable slot kSlotFrame, the shared root frame function; self+8 = the root element),
// on the UI thread, whether or not the wheel was ever opened.
// Its distance is fixed (tools/peek_gen.py), as the user preferred after trying distances following the camera zoom,
// the character's size measured from two trackers, and a fixed spot on the screen (2026-11-05).
FrameFn g_gauge_frame_orig = nullptr;
U64 g_peek_root = 0;
struct Peek {
    bool open = false;
    std::vector<int> entries;        // -1 = No outfit, then outfit indices in the Outfits tab's order
    size_t sel = 0;                  // the highlighted entry (shown in the middle)
    int worn = -1;
    bool applied = false;   // (an outfit applied when it opened: No outfit unequips it, #21)
    size_t sel0 = 0;        // (the entry it opened on: let go there, nothing changes)
    U64 closing_at = 0;              // closing: its animation plays, then it hides (GetTickCount64 of the close; 0 = not)
    U16 character = 0xFFFF;          // whose outfits it shows (a switch closes it: they'd be the wrong ones)
    U64 self = 0;                    // the status screen it was opened from (loads and their questions go through it)
    U64 wrap = 0, arc = 0, frame[5] = {}, icon[5] = {}, label = 0, name = 0;
    U64 badge[5] = {}, nb[2] = {};   // each slot's climate badge; the highlighted outfit's badges by its label
    std::string badge_set[5], nb_set[2];
    U64 trk = 0, hp_gauge = 0;       // the HP tracker (the peek's home) and its own gauge (hidden while shown)
    U32 trk_mount = 0;               // on a mount: the mount the HP tracker follows (0: it follows its own actor)
    U32 trk_own = 0;                 // ... the actor it followed before (put back after)
    float trk_own_ratio = 0;         // ... and its height ratio
    U64 reveal_at = 0;               // opened on a mount: invisible until the tracker is at the mount (0 = shown)
    int reveal_frames = 0;           // ... frames with a settled tracker height so far
    float reveal_last = 0;           // ... the last of those heights
    std::string icon_set[5];         // last icon set per position
    bool slide_b = false;            // the scroll's slide: which class set (a / b) the last step used
    std::string slide_set[25];       // its class on each element (frames, icons, badges, the texts, then the marks)
    // #32 pins: No outfit, the pinned outfits, the recent ones (group per entry: 0 / 1 / 2); a press (A / Cross / left
    // click) toggles the highlighted outfit's pin, shown at once (its corner mark, the hint), applied when it closes
    std::vector<std::uint8_t> group;
    std::set<int> toggled;
    // Editing pins (the user's call, 2026-11-13): the first entry (kPeekEdit) is "Edit Pins"; pressed, the arc edits:
    // the outfits show their hints and marks, a press pins / unpins, letting go only saves the pins, and the first entry
    // is Back (pressed: the picker again, on the entry it opened on). Otherwise a press does nothing.
    bool edit = false;
    int opened_on = -1;              // (the entry it opened on, by value: letting go there changes nothing)
    bool landed = false;             // (its mode just switched, not scrolled since: letting go changes nothing)
    U64 mark[5] = {}, hint = 0, hint_icon = 0, ripple = 0;
    std::string mark_set[5], hint_icon_set;
    U64 lift = 0, hold = 0; // (the picked-up glow, the hold disc)
    // Moving a pinned outfit (edit mode): held on it, it is picked up - scrolling moves it among the pinned ones, a tap
    // drops it. The presses: a down starts one (press_at), its up ends it - a tap unless the hold already acted.
    bool moving = false, order_changed = false;
    U64 press_at = 0;
    bool press_used = false;
    bool fx_b = false;               // the press animations' class set (a / b alternate: a press right after restarts)
    // this opening's mode: the remembered one (g_peek_weapons), but the outfits for a character without weapon sets -
    // Damiane / Oongka opened on an empty weapon arc after Kliff's (2026-11-13); the remembered mode stays
    bool weapons = false;
} g_peek;
std::atomic<int> g_peek_clicks{0};    // left clicks taken from the game while the arc is open (input.cpp's mouse hook)
constexpr size_t kPeekRecent = 3;     // the recent outfits on the arc
constexpr int kPeekEdit = -2;         // (the first entry: Edit Pins / Back while editing)
constexpr int kPeekSwitch = -3;       // (the weapon sets' second entry: back to the outfits - No Outfit's place)
constexpr U64 kPeekHoldMs = 450;      // a hold this long on a pinned outfit picks it up (tools/peek_gen.py HOLD_MS)
std::atomic<int> g_peek_ups{0};       // left button releases taken while it is open (input.cpp)
bool g_peek_down = false;             // the arc key on the last frame

bool g_peek_by_pad = false;           // the open arc came from the pad (L1 / LB held: it closes when that lets go)
std::atomic<bool> g_peek_pad_open{false}; // (the same, for the pad filters on other threads)
bool peek_pad_owned() { return g_peek_live && g_peek_pad_open; }
U8 g_peek_pad_last = 0;               // the pad's buttons on the last frame (for presses)

std::atomic<bool> g_peek_live{false};  // the arc is open (read by the input filters: the wheel is the arc's)
std::atomic<U64> g_peek_hud_at{0};     // the status screen's last frame with the arc ready (tick): the key is the arc's
std::atomic<int> g_peek_wheel{0};      // wheel movement kept from the game while the arc is open (WHEEL_DELTA units)

bool peek_elements()
{
    Peek& p = g_peek;
    if (p.wrap && is_element(p.wrap) && has_class(p.wrap, "om-peek-wrap")) return true;
    {
        std::vector<std::uint8_t> group = p.group;
        std::set<int> toggled = p.toggled;
        const bool editing = p.edit, landed = p.landed, weapons = p.weapons;
        const int opened_on = p.opened_on;
        p = Peek{p.open, p.entries, p.sel, p.worn, p.applied, p.sel0};
        p.group = std::move(group), p.toggled = std::move(toggled), p.edit = editing;
        p.opened_on = opened_on, p.landed = landed, p.weapons = weapons;
    }
    p.wrap = find_by_class(g_peek_root, "om-peek-wrap", 8);
    if (!p.wrap) return false;
    for (int k = 0; k < 5; ++k) {
        p.frame[k] = find_by_class(p.wrap, ("om-peek-frame-" + std::to_string(k)).c_str(), 1);
        p.icon[k] = find_by_class(p.wrap, ("om-peek-icon-" + std::to_string(k)).c_str(), 1);
    }
    p.label = find_by_class(p.wrap, "om-peek-label", 1), p.name = find_by_class(p.wrap, "om-peek-name", 1);
    p.arc = find_by_class(p.wrap, "om-peek-arc", 1);
    for (int k = 0; k < 5; ++k) p.badge[k] = find_by_class(p.wrap, ("om-peek-badge-" + std::to_string(k)).c_str(), 1);
    for (int k = 0; k < 2; ++k) p.nb[k] = find_by_class(p.wrap, ("om-peek-nb-" + std::to_string(k)).c_str(), 1);
    for (int k = 0; k < 5; ++k) p.mark[k] = find_by_class(p.wrap, ("om-peek-mark-" + std::to_string(k)).c_str(), 1);
    p.lift = find_by_class(p.wrap, "om-peek-lift", 1), p.hold = find_by_class(p.wrap, "om-peek-hold", 1);
    p.hint = find_by_class(p.wrap, "om-peek-hint", 1), p.hint_icon = find_by_class(p.wrap, "om-peek-hint-icon", 1);
    p.ripple = find_by_class(p.wrap, "om-peek-ripple", 1);
    p.trk = find_by_class(g_peek_root, "cpp-hp-actor-tracker", 4);
    p.hp_gauge = find_by_class(p.trk, "hp-sub-gauge-bar-wrap", 1);
    bool all = p.label && p.name && p.arc && p.trk;
    for (int k = 0; k < 5; ++k) all &= p.frame[k] && p.icon[k];
    LOG("Outfit Arc: elements %s", all ? "found" : "INCOMPLETE");
    return all;
}

// The carousel as it is now: position k shows entry sel + (k - 2), wrapping; with fewer than 5 entries the ends stay
// empty rather than repeat one.
void peek_draw(U64 self)
{
    Peek& p = g_peek;
    size_t n = p.entries.size();
    // the arc fits the shown slots (om-peek-arc-n1..n5: tools/peek_gen.py)
    for (int v = 1; v <= 5; ++v)
        set_class(p.arc, ("om-peek-arc-n" + std::to_string(v)).c_str(), size_t(v) == std::clamp<size_t>(n, 1, 5));
    for (int k = 0; k < 5; ++k) {
        int off = k - 2;
        bool shown = n && (off >= -int((n - 1) / 2) && off <= int(n / 2));
        set_class(p.frame[k], "!cpp-none", !shown);
        std::string pic;
        if (shown) {
            int e = p.entries[(p.sel + n + size_t(off % int(n) + int(n))) % n];
            pic = e == kPeekEdit ? std::string(p.edit ? "outfitmanager_peek_back" : "outfitmanager_peek_edit")
                  : e == kPeekSwitch ? std::string("outfitmanager_peek_outfits") // (the armour badge)
                  : e < 0 ? std::string("outfitmanager_peek_none") // (No outfit: the wheel's Unequip Outfit badge)
                  : e >= int(g_outfits.size()) ? std::string()
                  : is_weapon_set(g_outfits[size_t(e)]) ? weapon_set_picture(g_outfits[size_t(e)]) // (#27: its main weapon)
                                                        : g_outfits[size_t(e)].chest_icon;
        }
        if (pic != p.icon_set[k]) {
            int r = pic.empty() ? 1 : set_image_raw(self, p.icon[k], pic.c_str());
            if (r != 1) LOG("Outfit Arc: icon \"%s\" at position %d: %s", pic.c_str(), k, r ? "fault" : "no such texture");
            p.icon_set[k] = pic;
        }
        set_class(p.icon[k], "!cpp-none", !shown || pic.empty());
    }
    // the badges: a thermometer on each other slot whose outfit has a climate (as the wheel shows it: manual, or from
    // its resistance); the highlighted outfit's thermometer and eye (custom looks) beside its label, its hotkey under
    // its name as key caps
    auto image = [&](U64 el, std::string& set, const char* want) {
        if (!el) return;
        std::string w = want ? want : "";
        if (w != set && !w.empty()) {
            int r = set_image_raw(self, el, w.c_str());
            if (r != 1) LOG("Outfit Arc: badge \"%s\": %s", w.c_str(), r ? "fault" : "no such texture");
        }
        set = w;
        set_class(el, "!cpp-none", w.empty());
    };
    auto climate_tex = [](const Outfit& o) -> const char* {
        const char* c = climate_badge(o);
        if (!c) return nullptr;
        if (!std::strcmp(c, "om-cold-c")) return "cd_icon_temperature_freeze_00";
        if (!std::strcmp(c, "om-cold2-c")) return "cd_icon_temperature_freeze_phase_00";
        if (!std::strcmp(c, "om-heat-c")) return "cd_icon_temperature_burn_00";
        if (!std::strcmp(c, "om-heat2-c")) return "cd_icon_temperature_burn_phase_00";
        if (!std::strcmp(c, "om-sleep-c")) return "cd_uitexture_tab_icon_housing_Bed";
        return nullptr;
    };
    for (int k = 0; k < 5; ++k) {
        int off = k - 2;
        bool shown = n && (off >= -int((n - 1) / 2) && off <= int(n / 2)) && k != 2;
        int ek = shown ? p.entries[(p.sel + n + size_t(off % int(n) + int(n))) % n] : -1;
        image(p.badge[k], p.badge_set[k], ek >= 0 && ek < int(g_outfits.size()) ? climate_tex(g_outfits[size_t(ek)]) : nullptr);
    }
    int e = n ? p.entries[p.sel] : 0;
    {
        const Outfit* o = e >= 0 && e < int(g_outfits.size()) ? &g_outfits[size_t(e)] : nullptr;
        const char* clim = o ? climate_tex(*o) : nullptr;
        bool looks = false;
        if (o)
            for (U16 s = 0; s < 32; ++s) looks |= slot_look(*o, s) != kLookDefault;
        // (nb-1 is next to the label, nb-0 before it: one badge takes nb-1)
        image(p.nb[1], p.nb_set[1], looks ? "outfitmanager_badge_eye" : clim);
        image(p.nb[0], p.nb_set[0], looks ? clim : nullptr);
        int count = (clim ? 1 : 0) + (looks ? 1 : 0);
        set_class(p.label, "om-peek-label-s1", count == 1);
        set_class(p.label, "om-peek-label-s2", count == 2);
        bool caps = o && o->hk_vk && show_caps(self, p.wrap, o->hk_vk, o->hk_mods, 0);
        if (!caps)
            if (U64 box = find_by_class(p.wrap, "om-hk-caps", 1)) set_class(box, "!cpp-none", true);
        // #32: the hint (what a press does to it: pin / unpin), under the hotkey or in its place
        const bool pinned_now = o && ((o->pin > 0) != (p.toggled.count(e) != 0));
        const bool hotkey = o && o->hk_vk; // (its caps row is there: the hint goes under it)
        // (a hint only where a press does something: the first entry, and the outfits while editing)
        // (No Outfit / Outfits: a press switches the arc between the outfits and the weapon sets)
        const bool sw = !p.edit && (e == -1 || e == kPeekSwitch);
        const bool first = e == kPeekEdit, hinted = first || sw || (o && p.edit) || p.moving;
        if (p.hint) {
            if (hinted && g_set_text)
                g_set_text(p.hint, lang::t(p.moving ? "peek.drop"
                                           : sw ? (e == -1 ? "peek.to_weapons" : "peek.to_outfits")
                                           : first ? (p.edit ? "peek.done" : "peek.edit")
                                           : !pinned_now ? "peek.pin"
                                           : p.sel < p.group.size() && p.group[p.sel] == 1 ? "peek.unpin_move" : "peek.unpin"), 0);
            set_class(p.hint, "!cpp-none", !hinted);
            set_class(p.hint, "om-peek-hint-hi", !hotkey);
        }
        if (p.hint_icon) {
            image(p.hint_icon, p.hint_icon_set, !hinted ? nullptr
                                                : g_peek_by_pad ? (g_pad_is_ps ? "outfitmanager_peek_key_cross" : "outfitmanager_peek_key_a")
                                                                : "Common_Icon_Keyguide_PC_Mouse_left");
            set_class(p.hint_icon, "om-peek-hicon-hi", !hotkey);
            if (p.ripple) set_class(p.ripple, "om-peek-ripple-hi", !hotkey);
            if (p.hold) set_class(p.hold, "om-peek-hold-hi", !hotkey);
        }
    }
    // #32: a pin / unpin to come on each shown outfit (its corner); a divider where the groups meet
    auto entry_at = [&](int k) -> int { // (the entry index at position k, or -1 when not shown)
        int off = k - 2;
        bool shown = n && (off >= -int((n - 1) / 2) && off <= int(n / 2));
        return shown ? int((p.sel + n + size_t(off % int(n) + int(n))) % n) : -1;
    };
    for (int k = 0; k < 5; ++k) {
        const int ei = entry_at(k);
        const int ek = ei >= 0 ? p.entries[size_t(ei)] : -1;
        const char* tex = nullptr;
        if (p.edit && ek >= 0 && ek < int(g_outfits.size()) && p.toggled.count(ek))
            tex = g_outfits[size_t(ek)].pin > 0 ? "outfitmanager_peek_unpin" : "outfitmanager_peek_pin";
        image(p.mark[k], p.mark_set[k], tex);
    }
    if (p.lift) set_class(p.lift, "om-peek-lift-on", p.moving);
    // (the first entry: No outfit; the others: the saved outfit's number and name)
    // (#32: the group's name - pinned / recent)
    const std::uint8_t grp = n && p.sel < p.group.size() ? p.group[p.sel] : 0;
    std::string label = p.moving ? lang::t("peek.moving")
                        : e == kPeekEdit ? lang::t(p.edit ? "peek.editing" : "peek.pins_label")
                        : e == kPeekSwitch ? lang::t("peek.weapons_label")
                        : e < 0 ? lang::t("peek.none_label") : lang::t(grp == 2 ? "peek.group.recent" : "peek.group.pinned");
    std::string name = e == kPeekEdit ? lang::t(p.edit ? "setting.back" : "peek.edit_pins")
                       : e == kPeekSwitch ? lang::t("peek.outfits")
                       : e < 0 ? lang::t("peek.none") : e < int(g_outfits.size()) ? g_outfits[size_t(e)].name : "";
    if (g_set_text) g_set_text(p.label, label.c_str(), 0), g_set_text(p.name, name.c_str(), 0);
}

// The animations (tools/peek_gen.py): the HUD key guide's unfold. Opening: every part has its own keyframes
// (om-peek-in-arc / -frame-k / -icon-k / -label / -name: the arc unfolds, the slots ride along and pop, the texts fade
// in, the middle frame pulses); closing: the whole peek fades with a slight shrink (om-peek-out-wrap). One set at a
// time; none between, so each plays again from its start.
enum class PeekAnim { None, In, Out };
constexpr U64 kPeekOutMs = 270; // the closing fade (peek_gen.py OUT_TOTAL, 0.25 s) and a little more
void peek_slide_clear();

void peek_animate(PeekAnim a)
{
    Peek& p = g_peek;
    peek_slide_clear(); // (the opening's classes play again; a slide's would win over them)
    bool in = a == PeekAnim::In;
    set_class(p.arc, "om-peek-in-arc", in);
    for (int k = 0; k < 5; ++k) {
        set_class(p.frame[k], ("om-peek-in-frame-" + std::to_string(k)).c_str(), in);
        set_class(p.icon[k], ("om-peek-in-icon-" + std::to_string(k)).c_str(), in);
    }
    set_class(p.label, "om-peek-in-label", in);
    set_class(p.name, "om-peek-in-name", in);
    for (int k = 0; k < 5; ++k) // (each slot's badge rides with its frame)
        if (p.badge[k]) set_class(p.badge[k], ("om-peek-in-badge-" + std::to_string(k)).c_str(), in);
    for (U64 b : {p.nb[0], p.nb[1]})
        if (b) set_class(b, "om-peek-in-name", in); // (with the texts)
    for (int k = 0; k < 5; ++k)
        if (p.mark[k]) set_class(p.mark[k], ("om-peek-in-mark-" + std::to_string(k)).c_str(), in);
    for (U64 b : {p.hint, p.hint_icon})
        if (b) set_class(b, "om-peek-in-name", in);
    if (U64 box = find_by_class(p.wrap, "om-hk-caps", 1)) set_class(box, "om-peek-in-name", in);
    set_class(p.wrap, "om-peek-out-wrap", a == PeekAnim::Out);
}

// The scroll's slide (2026-11-06, the user's pick of the mockups: without it a step showed no direction). Each slot's
// frame, icon and badge glides from where its new outfit was, or comes in from beyond the arc's end; the texts rise or
// drop in. The a / b class sets alternate, so a step right after another starts its own slide (tools/peek_gen.py).
// dir: +1 the outfits moved up (the next outfit), -1 down.
U64 peek_slide_element(int i)
{
    Peek& p = g_peek;
    if (i < 5) return p.frame[i];
    if (i < 10) return p.icon[i - 5];
    if (i < 15) return p.badge[i - 10];
    if (i == 15) return p.label;
    if (i == 16) return p.name;
    if (i < 19) return p.nb[i - 17];
    if (i == 19) return find_by_class(p.wrap, "om-hk-caps", 1);
    return p.mark[i - 20]; // (20-24: the marks, #32)
}

void peek_slide_clear()
{
    Peek& p = g_peek;
    for (int i = 0; i < 25; ++i) {
        if (p.slide_set[i].empty()) continue;
        if (U64 el = peek_slide_element(i)) set_class(el, p.slide_set[i].c_str(), false);
        p.slide_set[i].clear();
    }
}

void peek_slide(int dir)
{
    Peek& p = g_peek;
    int n = int(p.entries.size());
    if (n < 2) return; // (one outfit: nothing moves)
    p.slide_b = !p.slide_b;
    char ab = p.slide_b ? 'b' : 'a', ud = dir > 0 ? 'u' : 'd';
    int lo = -std::min(2, (n - 1) / 2), hi = std::min(2, n / 2); // (the positions shown, as peek_draw)
    char cls[48];
    for (int i = 0; i < 25; ++i) {
        U64 el = peek_slide_element(i);
        if (!el) continue;
        if (i < 15 || i >= 20) {
            int k = i % 5, from = k + dir - 2;
            const char* part = i < 5 ? "frame" : i < 10 ? "icon" : i < 15 ? "badge" : "mark";
            std::snprintf(cls, sizeof cls, "om-peek-sl%c-%c%c-%s-%d", ab, from >= lo && from <= hi ? 'm' : 'e', ud,
                          part, k);
        } else {
            std::snprintf(cls, sizeof cls, "om-peek-sl%c-t%c", ab, ud);
        }
        if (!p.slide_set[i].empty() && p.slide_set[i] != cls) set_class(el, p.slide_set[i].c_str(), false);
        set_class(el, cls, true);
        p.slide_set[i] = cls;
    }
}

U32 riding_mount(); // (below: the mount being ridden, 0 on foot)

// #32: the arc's entries - Edit Pins (Back while editing) and No outfit (not while editing: nothing to pin there) in one
// group, then the pinned outfits in pin order, then the last applied ones not pinned. In its weapon sets mode the same
// with the weapon sets (#27), and Outfits (back to the outfits) in No outfit's place.
void peek_build(Peek& p)
{
    const bool weapons = p.weapons;
    p.entries.assign(1, kPeekEdit), p.group.assign(1, 0);
    if (!p.edit) p.entries.push_back(weapons ? kPeekSwitch : -1), p.group.push_back(0);
    normalize_pins(g_outfits);
    std::vector<int> pinned, recent;
    for (size_t i = 0; i < g_outfits.size(); ++i) {
        if (is_weapon_set(g_outfits[i]) != weapons) continue;
        if (g_outfits[i].pin > 0) pinned.push_back(int(i));
        else if (g_outfits[i].used) recent.push_back(int(i));
    }
    std::sort(pinned.begin(), pinned.end(), [](int a, int b) { return g_outfits[size_t(a)].pin < g_outfits[size_t(b)].pin; });
    std::sort(recent.begin(), recent.end(), [](int a, int b) { return g_outfits[size_t(a)].used > g_outfits[size_t(b)].used; });
    if (recent.size() > kPeekRecent) recent.resize(kPeekRecent);
    for (int i : pinned) p.entries.push_back(i), p.group.push_back(1);
    for (int i : recent) p.entries.push_back(i), p.group.push_back(2);
}
size_t peek_find(const Peek& p, int e, size_t fallback) // (the entry showing e)
{
    for (size_t k = 0; k < p.entries.size(); ++k)
        if (p.entries[k] == e) return k;
    return fallback < p.entries.size() ? fallback : 0;
}
// #32: the pins the presses changed, saved (when editing ends or the arc closes). Unpinned: the first recent one.
void peek_apply_pins(Peek& p)
{
    p.moving = false;
    if (p.order_changed && p.toggled.empty() && g_character == p.character) save_outfits(); // (moved, nothing toggled)
    p.order_changed = false;
    if (p.toggled.empty() || g_character != p.character) return;
    int top = 0;
    U32 last = 0;
    for (const Outfit& o : g_outfits) top = std::max(top, o.pin), last = std::max(last, o.used);
    for (int t : p.toggled)
        if (t >= 0 && size_t(t) < g_outfits.size()) {
            Outfit& o = g_outfits[size_t(t)];
            o.pin = o.pin > 0 ? 0 : ++top;
            if (!o.pin) o.used = ++last; // (a press away from pinned again)
            LOG("Outfit Arc: \"%s\" %s", o.name.c_str(), o.pin ? "pinned" : "no longer pinned");
        }
    p.toggled.clear();
    save_outfits();
}

// The arc on the state there is now (opening, or its mode switched): letting go there changes nothing.
void peek_land(Peek& p)
{
    if (p.weapons) {
        const int set = worn_weapon_set(p.self);
        p.opened_on = set >= 0 ? set : kPeekSwitch;
    } else {
        p.opened_on = p.applied ? anchor_outfit() : -1;
    }
    p.sel = p.sel0 = peek_find(p, p.opened_on, 1);
}

void peek_open(U64 self)
{
    Peek& p = g_peek;
    p.self = self;
    prepare_outfits(); // (as the wheel does when it opens)
    p.entries.clear();
    p.worn = find_worn_outfit(self);
    // (the entries: see the top of this section)
    p.toggled.clear();
    p.edit = p.moving = p.order_changed = p.landed = false;
    p.press_at = 0;
    g_peek_clicks = 0, g_peek_ups = 0;
    const int anchor = anchor_outfit();
    p.applied = anchor >= 0;
    p.weapons = false;
    if (g_peek_weapons)
        for (const Outfit& o : g_outfits) p.weapons |= is_weapon_set(o);
    peek_build(p);
    // (on the outfit applied - also when some of its pieces were changed since -, else on No outfit; the weapon sets:
    // on the one worn, else on Outfits)
    peek_land(p);
    p.open = true;
    p.character = g_character;
    set_class(p.trk, "!cpp-none", false); // (the HP tracker: shown while the peek is, its own gauge hidden)
    set_class(p.hp_gauge, "!cpp-none", true);
    peek_animate(PeekAnim::None); // (a close still playing: cut short, the opening starts over)
    p.closing_at = 0;
    peek_draw(self);
    peek_animate(PeekAnim::In);
    // On a mount the arc's height comes from the mount's tracker, which only moves while shown: right after opening it
    // still holds an old height, and the arc showed for a frame up at the rider's head (2026-11-07). So on a mount the
    // arc appears once that height is known (peek_ride_place: a frame or two, at most 150 ms).
    // Waiting, the arc is laid out but invisible (om-peek-veil: opacity 0), not hidden: an element unhidden is laid
    // out once from the css, without the height the plugin writes - the frame at the rider's head (2026-11-07).
    p.reveal_at = riding_mount() ? GetTickCount64() : 0;
    p.reveal_frames = 0;
    set_class(p.wrap, "om-peek-veil", p.reveal_at != 0);
    set_class(p.wrap, "!cpp-none", false);
    LOG("Outfit Arc: open (character %d, %zu outfit(s), worn %d, %zu entries%s)", int(g_character), g_outfits.size(), p.worn,
        p.entries.size(), p.weapons ? ", weapon sets" : g_peek_weapons ? ", outfits (no weapon sets)" : "");
}

// #32: a picked-up outfit moved one place among the pinned ones (dir +1: down the arc); the arc follows it.
void peek_move(U64 self, int dir)
{
    Peek& p = g_peek;
    const size_t k = p.sel;
    if ((dir < 0 && k == 0) || k + dir >= p.entries.size()) return;
    const size_t k2 = k + dir;
    if (p.group[k] != 1 || p.group[k2] != 1) return; // (only among the pinned outfits)
    const int a = p.entries[k], b = p.entries[k2];
    std::swap(g_outfits[size_t(a)].pin, g_outfits[size_t(b)].pin);
    std::swap(p.entries[k], p.entries[k2]);
    p.sel = k2;
    p.order_changed = true;
    peek_draw(self);
    peek_slide(dir);
}

// #32: A / Cross / a left click on the highlighted outfit: its pin changes when the arc closes (again: as it was). The
// mark and the hint show it at once; the button dips with the game's press ripple behind it, a new mark pops.
void peek_toggle(U64 self)
{
    Peek& p = g_peek;
    if (!p.open || p.entries.empty()) return;
    if (p.moving) { // (a tap while one is picked up: it is dropped there)
        p.moving = false;
        LOG("Outfit Arc: dropped \"%s\" at place %zu", g_outfits[size_t(p.entries[p.sel])].name.c_str(), p.sel);
        peek_draw(self);
        return;
    }
    const int e = p.entries[p.sel];
    auto press_fx = [&] { // (the button dips, the game's press ripple behind it)
        p.fx_b = !p.fx_b;
        const char* on = p.fx_b ? "b" : "a";
        const char* off = p.fx_b ? "a" : "b";
        for (auto [el, base] : {std::pair<U64, const char*>{p.hint_icon, "om-peek-dip-"}, {p.ripple, "om-peek-ripple-"}})
            if (el) set_class(el, (std::string(base) + off).c_str(), false), set_class(el, (std::string(base) + on).c_str(), true);
    };
    if (!p.edit && (e == -1 || e == kPeekSwitch)) { // (No Outfit: the weapon sets; Outfits: the outfits again)
        g_peek_weapons = e == -1;
        p.weapons = g_peek_weapons;
        peek_build(p);
        peek_land(p); // (what is worn in this mode: letting go there changes nothing)
        p.sel = p.sel0 = 1, p.landed = true; // (on the other mode's switch: Outfits / No Outfit, the user's call)
        LOG("Outfit Arc: %s", p.weapons ? "the weapon sets" : "the outfits");
        save_outfits(); // (the mode is kept)
        peek_draw(self);
        peek_slide(e == -1 ? 1 : -1);
        press_fx();
        return;
    }
    if (e == kPeekEdit) { // (Edit Pins: editing; Back: the pins applied, the picker again - still on Edit Pins)
        p.edit = !p.edit;
        if (!p.edit) peek_apply_pins(p);
        peek_build(p);
        p.sel = 0;
        if (!p.edit) p.sel0 = 0; // (letting go right away: nothing changes)
        LOG("Outfit Arc: %s", p.edit ? "editing pins (letting go wears nothing)" : "done editing pins");
        peek_draw(self);
        press_fx();
        return;
    }
    if (!p.edit || e < 0 || size_t(e) >= g_outfits.size()) return; // (only while editing; No outfit: nothing to pin)
    const bool now = p.toggled.insert(e).second;
    if (!now) p.toggled.erase(e);
    LOG("Outfit Arc: \"%s\" %s when it closes", g_outfits[size_t(e)].name.c_str(),
        now ? (g_outfits[size_t(e)].pin > 0 ? "unpinned" : "pinned") : "left as it was");
    peek_draw(self);
    p.fx_b = !p.fx_b;
    const char* on = p.fx_b ? "b" : "a";
    const char* off = p.fx_b ? "a" : "b";
    auto replay = [&](U64 el, const char* base) {
        if (!el) return;
        set_class(el, (std::string(base) + off).c_str(), false);
        set_class(el, (std::string(base) + on).c_str(), true);
    };
    replay(p.hint_icon, "om-peek-dip-");
    replay(p.ripple, "om-peek-ripple-");
    if (now) replay(p.mark[2], "om-peek-pop-");
}

void peek_close(bool choose)
{
    Peek& p = g_peek;
    int e = p.entries.empty() ? -1 : p.entries[p.sel];
    peek_animate(PeekAnim::None);
    peek_animate(PeekAnim::Out); // (hidden by peek_frame once it has played)
    p.closing_at = GetTickCount64();
    p.open = false;
    // (let go on another entry than the one it opened on: an outfit is put on and applied - also one whose pieces are
    // already worn after an unequip -; No outfit unequips the one applied)
    const bool editing = p.edit; // (editing pins: letting go only saves them)
    p.edit = false;
    if (editing) choose = false;
    if (p.landed) choose = false; // (the mode just switched, nothing picked)
    bool load = choose && e >= 0 && e != p.opened_on && static_cast<size_t>(e) < g_outfits.size();
    peek_apply_pins(p); // (#32: the outfits' indices don't change: a load below is unaffected)
    LOG("Outfit Arc: closed on %s%s (%s, wheel notches %u)", e == kPeekEdit ? "Edit Pins" : e == kPeekSwitch ? "Outfits" : e < 0 ? "No outfit" : ("outfit " + std::to_string(e + 1)).c_str(),
        !choose ? "" : load ? "; putting it on" : " (nothing to change)", g_peek_by_pad ? "pad" : "key",
        g_wheel_notches.load());
    // as a click on it in the wheel: where each piece is, the questions (pieces on other characters, copies), the load
    if (load) start_loading(p.self, static_cast<U32>(e));
    else if (choose && e == -1 && p.applied && e != p.opened_on) release_outfit(p.self); // (#21: No outfit)
}

// The outfits' hotkeys, in play (not in menus, prompts or with the arc open): a press of exactly the combination loads
// that outfit as a click in the wheel would. The character's own outfits are the ones in use (taken over when it
// changes). The filters (name_wndproc, raw_input_hook) keep the key from the game while its modifiers match.
void peek_hotkeys(U64 self, bool free)
{
    { // the helmet key (#34): exactly its modifiers. A press that shows or hides the worn helmet plays the character's
      // take-off / take-on animation (on foot) and the change comes when the hand is at the helmet (or when the
      // animation ends before that; anim.cpp); otherwise at once.
        static bool was_helmet = false;
        static U16 pend_char = kNoCharacter;
        static std::uint8_t pend_pin = 0;
        auto apply = [](U16 c, std::uint8_t pin, const char* how) {
            prepare_outfits(); // (a save loaded since: taken over first, else the change is dropped and not saved)
            helmet_set(c, pin);
            LOG("helmet key: character %u: %s%s", c,
                pin == 1 ? "helmet hidden" : pin == 2 ? "helmet shown" : "the outfit decides again", how);
            save_outfits();
            look_refresh();
        };
        static bool pend_done = false; // (applied at the animation's moment, before its end)
        anim_tick();
        if (pend_char != kNoCharacter) {
            if (!pend_done && anim_cue()) apply(pend_char, pend_pin, " (the hand is at it)"), pend_done = true;
            const AnimOutcome o = anim_outcome();
            if (o != AnimOutcome::Running) {
                if (!pend_done)
                    apply(pend_char, pend_pin, o == AnimOutcome::Played ? " (its animation is over)" : " (no animation)");
                pend_char = kNoCharacter;
                pend_done = false;
            }
        }
        const U32 hv = g_helmet_vk.load();
        const bool down = hv && (GetAsyncKeyState(int(hv)) & 0x8000) && mods_now() == g_helmet_mods.load();
        if (down && !was_helmet && free && pend_char == kNoCharacter) {
            const U16 ch = current_character();
            if (ch != kNoCharacter) {
                prepare_outfits(); // (as the outfit hotkeys: a save just loaded is taken over, its pin decides)
                const HelmetPress plan = helmet_press_plan(ch);
                U16 key = 0xFFFF;
                if (const U64 e = display_item(kHelmetSlot)) rd(e + 8, key);
                // (mounted: none - the animation is on foot only, the game would get him off the horse for it)
                if (plan.shown_before != plan.shown_after && key != 0xFFFF && !riding_mount() &&
                    anim_helmet_start(!plan.shown_after, key))
                    pend_char = ch, pend_pin = plan.pin;
                else
                    apply(ch, plan.pin, "");
            }
        }
        was_helmet = down;
    }
    static U16 shown_char = 0xFFFF;
    U16 ch = current_character();
    if (free && ch != kNoCharacter && ch != shown_char) { // (another character: its own outfits and hotkeys)
        shown_char = ch;
        if (ch != g_character) prepare_outfits();
    }
    static bool was[16] = {};
    std::uint8_t mods = mods_now();
    for (size_t i = 0; i < 16; ++i) {
        const Outfit* o = i < g_outfits.size() ? &g_outfits[i] : nullptr;
        U32 packed = o && o->hk_vk ? o->hk_vk | U32(o->hk_mods) << 8 : 0;
        if (g_hotkeys[i].load() != packed) g_hotkeys[i] = packed;
    }
    for (size_t i = 0; i < 16; ++i) {
        const Outfit* o = i < g_outfits.size() ? &g_outfits[i] : nullptr;
        U32 packed = o && o->hk_vk ? o->hk_vk | U32(o->hk_mods) << 8 : 0;
        // exactly its modifiers; a bare key also with others held when no outfit has that exact combination (so "X"
        // still works while Shift sprints, unless "Shift + X" is another outfit's)
        bool exact_taken = false;
        if (packed && !o->hk_mods && mods)
            for (size_t j = 0; j < g_outfits.size(); ++j)
                exact_taken |= g_outfits[j].hk_vk == o->hk_vk && g_outfits[j].hk_mods == mods;
        bool mods_ok = o && (mods == o->hk_mods || (!o->hk_mods && !exact_taken));
        bool down = packed && (GetAsyncKeyState(o->hk_vk) & 0x8000) && mods_ok;
        bool press = down && !was[i];
        was[i] = down;
        if (!press || !free) continue;
        U16 vk = o->hk_vk;
        std::uint8_t m = o->hk_mods;
        prepare_outfits(); // (as the wheel and the arc do first; it may rebuild the list: found again by the combination)
        int idx = -1;
        for (size_t j = 0; j < g_outfits.size(); ++j)
            if (g_outfits[j].hk_vk == vk && g_outfits[j].hk_mods == m) idx = int(j);
        if (idx < 0) continue;
        int worn = is_weapon_set(g_outfits[size_t(idx)]) ? worn_weapon_set(self) : find_worn_outfit(self); // (#27)
        LOG("hotkey %s: \"%s\"%s", hotkey_name(vk, m).c_str(), g_outfits[size_t(idx)].name.c_str(),
            idx == worn ? " (already worn)" : "; putting it on");
        if (idx != worn) start_loading(self, U32(idx));
        break; // (one load per frame)
    }
}

// The arc opened or closed: the input filters follow (g_peek_live), the wheel hook goes on / off.

void peek_sync_live(bool open)
{
    if (open == g_peek_live.load()) return;
    if (open) g_wheel_notches = 0;
    g_peek_live = open;
    wheel_hook(open);
}

// The rider's state byte at [comp+0x1A0]+0x91: 0x0B while getting off a mount (0x10 riding and once off; 0 unknown)
U8 rider_state()
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U8 st = 0;
    if (U64 comp = actor ? rp(actor + kOffActorComp) : 0)
        if (U64 m = rp(comp + kOffCompRidden)) rd(m + kOffRiderState, st);
    if (ctx[0]) release_player(ctx);
    return st;
}

U32 riding_mount()
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U8 flag = 0xFF;
    U64 id = 0;
    if (U64 comp = actor ? rp(actor + kOffActorComp) : 0) {
        if (U64 r = rp(comp + kOffCompRide)) rd(r + kOffRideFlag, flag);
        if (U64 m = rp(comp + kOffCompRidden)) rd(m + kOffRiddenId, id);
    }
    if (ctx[0]) release_player(ctx);
    if (flag != 1 || id == ~0ull || id > 0xFFFFFFFFull || id < 0x10000000ull) return 0;
    return static_cast<U32>(id);
}

// On a mount the HP tracker (the arc's home) sits 60% up the rider and mount together: at the rider's head, the arc
// above Kliff (2026-11-07). So while riding it follows the mount at the mount stamina spiral's ratio (0.75) - the
// spiral's spot on every mount, placed by the game itself; put back to its own actor and ratio once the arc hides.
// A dismount: the riding flag drops as it starts, so the mount is followed until the rider is off (the rider's state
// leaves 0x0B: about 0.9 - 1.3 s; at most 2.5 s, and 0.3 s if that state never shows).
void peek_ride_place()
{
    Peek& p = g_peek;
    bool shown = (p.open || p.closing_at) && p.trk;
    U64 f = p.trk ? rp(p.trk + kOffTrkFollow) : 0;
    U32 mount = shown ? riding_mount() : 0;
    U64 now = GetTickCount64();
    static U64 rode_at = 0;
    if (mount) rode_at = now;
    static bool getting_off = false;
    if (mount) getting_off = false;
    bool hold = false;
    if (shown && !mount && p.trk_mount) {
        U8 st = rider_state();
        if (st == kRiderGettingOff) getting_off = true;
        hold = getting_off ? st == kRiderGettingOff && now - rode_at < 2500 : now - rode_at < 300;
    }
    U32 follow = !shown ? 0 : mount ? mount : hold ? p.trk_mount : 0;
    if (f && follow) {
        if (!p.trk_mount) {
            p.trk_own = 0, p.trk_own_ratio = 0;
            rd(f + kOffFollowActor, p.trk_own), rd(f + kOffFollowRatio, p.trk_own_ratio);
            if (p.trk_own == follow) p.trk_own = 0; // (already the mount's: never put back as "its own")
            if (!(p.trk_own_ratio > 0.0f && p.trk_own_ratio <= 1.5f)) p.trk_own_ratio = kTrkRatio;
        }
        wr<U32>(f + kOffFollowActor, follow);
        wr<float>(f + kOffFollowRatio, kMountRatio);
        p.trk_mount = follow;
    } else if (p.trk_mount) {
        U32 cur = 0;
        if (f && rd(f + kOffFollowActor, cur) && cur == p.trk_mount) { // (the game hasn't re-aimed it itself)
            if (p.trk_own) wr<U32>(f + kOffFollowActor, p.trk_own);
            wr<float>(f + kOffFollowRatio, p.trk_own_ratio);
        }
        if (shown) LOG("Outfit Arc: back on the rider %llu ms after the dismount began", (unsigned long long)(now - rode_at));
        p.trk_mount = 0;
    }
    if (!p.reveal_at) return;
    // Opened on a mount: the tracker only moves while shown, so it can hold an old place for a frame or two; the arc
    // stays invisible (om-peek-veil) until its height has settled with the mount followed (at most 150 ms).
    float ty = 0;
    bool have = p.trk_mount && rd(p.trk + 0x74, ty) && ty > 0;
    bool settled = have && std::fabs(ty - p.reveal_last) < 1.0f;
    p.reveal_frames = settled ? p.reveal_frames + 1 : 0;
    p.reveal_last = ty;
    if (!p.open || p.reveal_frames >= 2 || !p.trk_mount || now - p.reveal_at > 150) {
        set_class(p.wrap, "om-peek-veil", false);
        p.reveal_at = 0;
    }
}

void sleep_tick(U64 self); // (sleep.cpp)
void sound_tick();         // (sound.cpp)

void peek_frame(U64 self)
{
    sleep_tick(self); // (a sleep begun or ended: the sleepwear goes on / what was worn comes back)
    sound_tick();     // (the armour's sounds follow its looks)
    U64 root = rp(self + 8);
    if (root != g_peek_root) { // (first frame, or the screen was made again: a load)
        g_peek_root = root, g_peek = Peek{};
        if (!root || !is_element(root) || !find_by_class(root, "om-peek-wrap", 4)) {
            if (root) LOG("Outfit Arc: the status screen %llX has no peek (its UI file wasn't patched)", root);
            g_peek_root = ~0ull; // (said once; looked at again only if the screen changes)
            return;
        }
        LOG("Outfit Arc: found in the status screen %llX", root);
    }
    if (g_peek_root == ~0ull || !peek_elements()) return;
    Peek& p = g_peek;
    g_peek_hud_at = GetTickCount64();
    // The key (the "+" page's Quick Peek): held = open, released = the highlighted outfit goes on. Not while the gear
    // wheel, a question or the naming prompt is open.
    if (g_peek_capture && !g_menu_open) g_peek_capture = false; // (the wheel closed while the setting waited)
    watch_game_chars(); // (the window watch: the wheel's messages while the arc is open)
    U32 vk = g_peek_key.load();
    bool down = vk && !g_peek_capture && game_focused() && (GetAsyncKeyState(int(vk)) & 0x8000) != 0;
    bool free = !g_menu_open && !g_ask_modal && !g_name_modal;
    // The pad: hold L1 / LB and press the d-pad's down (2026-11-05: down does nothing while L1 is held outside combat;
    // left raises the lantern, up calls the horse, right the items); up / down then move, letting go of L1 wears it.
    U8 pad = g_peek_pad && game_focused() ? pad_buttons_raw() : 0, pressed = pad & ~g_peek_pad_last,
       released = g_peek_pad_last & ~pad;
    g_peek_pad_last = pad;
    if (down && !g_peek_down && !p.open && free) {
        g_peek_wheel = 0;
        g_peek_by_pad = false;
        peek_open(self);
    } else if ((pad & kPadL1) && (pressed & kPadDown) && !p.open && free) {
        g_peek_wheel = 0;
        g_peek_by_pad = true;
        peek_open(self);
        pressed = 0; // (the opening press doesn't also move)
    } else if (p.open && (g_peek_by_pad ? !(pad & kPadL1) : !down)) {
        peek_close(true);
    }
    { // #32: A / Cross / left click (the mouse hook takes them from the game while it is open): a tap acts on release;
      // held on a pinned outfit while editing, it is picked up once the hold has lasted (the game's hold disc closing in)
        const int downs = g_peek_clicks.exchange(0), ups = g_peek_ups.exchange(0);
        const bool pd = p.open && g_peek_by_pad && (pressed & kPadCross), pu = p.open && g_peek_by_pad && (released & kPadCross);
        const U64 now = GetTickCount64();
        auto hold_disc = [&](bool on) {
            if (p.hold) set_class(p.hold, "om-peek-hold-on", on);
        };
        if (p.open && (downs || pd) && !p.press_at) {
            p.press_at = now, p.press_used = false;
            const bool can_lift = p.edit && !p.moving && p.sel < p.group.size() && p.group[p.sel] == 1;
            hold_disc(can_lift);
        }
        if (p.open && p.press_at && !p.press_used && now - p.press_at >= kPeekHoldMs && p.edit && !p.moving &&
            p.sel < p.group.size() && p.group[p.sel] == 1) {
            p.press_used = true, p.moving = true;
            hold_disc(false);
            LOG("Outfit Arc: picked up \"%s\"", g_outfits[size_t(p.entries[p.sel])].name.c_str());
            peek_draw(self);
            p.fx_b = !p.fx_b;
            for (auto [el, base] : {std::pair<U64, const char*>{p.ripple, "om-peek-ripple-"}})
                if (el) set_class(el, (std::string(base) + (p.fx_b ? "a" : "b")).c_str(), false),
                        set_class(el, (std::string(base) + (p.fx_b ? "b" : "a")).c_str(), true);
        }
        if ((ups || pu) && p.press_at) {
            hold_disc(false);
            if (p.open && !p.press_used) peek_toggle(self);
            p.press_at = 0;
        }
        if (!p.open) p.press_at = 0;
    }
    if (p.open && g_peek_by_pad && (pressed & (kPadUp | kPadDown)))
        g_peek_wheel += (pressed & kPadUp) ? WHEEL_DELTA : -WHEEL_DELTA; // (as a wheel notch: up = up the arc)
    g_peek_down = down;
    g_peek_pad_open = p.open && g_peek_by_pad;
    peek_hotkeys(self, free && !p.open && game_focused());
    { // a PlayStation pad's reader waits for a pad from the first frame on (not only once the gear wheel opened)
        static bool woke = false;
        if (!woke && g_peek_pad && g_hid_wake) woke = true, SetEvent(g_hid_wake);
    }
    peek_sync_live(p.open);
    if (g_ask_next && !g_menu_open && g_ask_self == self) pump_ask(self); // (a question asked from the peek)
    // closed (nothing chosen) when the player switches character (its outfits would be the last one's) or opens the
    // gear wheel (2026-11-05: switching with the peek open kept showing Kliff's outfits on Damiane and Oongka)
    if (p.open) {
        U16 ch = current_character();
        if (g_menu_open || (ch != kNoCharacter && ch != p.character)) {
            LOG("Outfit Arc: closed by %s", g_menu_open ? "the gear wheel opening" : "a character switch");
            peek_close(false);
        }
    }
    if (p.open && !p.entries.empty()) { // the wheel: a notch is one outfit (towards you = down the arc)
        int w = g_peek_wheel.load(), steps = w / WHEEL_DELTA;
        if (steps) {
            g_peek_wheel -= steps * WHEEL_DELTA;
            if (p.moving) { // (#32: the picked-up outfit moves, the arc follows it)
                for (int q = 0; q < std::abs(steps); ++q) peek_move(self, steps < 0 ? 1 : -1);
                steps = 0;
            }
        }
        if (steps) {
            long long n = static_cast<long long>(p.entries.size());
            long long m = (static_cast<long long>(p.sel) - steps) % n;
            p.sel = static_cast<size_t>(m < 0 ? m + n : m);
            p.landed = false;
            peek_draw(self);
            peek_slide(steps < 0 ? 1 : -1); // (towards you: the next outfit, the arc's outfits move up)
        }
    }
    peek_sync_live(p.open);
    if (p.closing_at && GetTickCount64() - p.closing_at >= kPeekOutMs) { // the closing animation has played
        set_class(p.wrap, "!cpp-none", true);
        peek_animate(PeekAnim::None);
        p.closing_at = 0;
        set_class(p.trk, "!cpp-none", true); // (the HP tracker hidden again, as the game keeps it)
    }
    peek_ride_place(); // (on a mount: the arc at the mount's back; else back where it was)
}

float __fastcall gauge_frame_hook(U64 self)
{
    // The UI thread is known from here too, not only once the wheel has opened: a load's later parts (the done check,
    // the stash, the looks held until the model swaps) run on the HUD frame only on that thread (2026-11-05: a load
    // from the quick peek stayed half-shown until the wheel opened).
    if (!g_ui_thread) {
        g_ui_thread = GetCurrentThreadId();
        LOG("ui thread %lu (from the status screen)", g_ui_thread);
    }
    g_hud_root = self; // (#35: the barber question opens from here)
    peek_frame(self);
    fx_tick(self); // (the outfit effect's timeline)
    return g_gauge_frame_orig(self);
}

void install_peek()
{
    void** vt = game::find_vtable(".?AVUIGamePlayControlRootStatusGauge@uiCommonScript@pa@@");
    if (!vt || vt[kSlotFrame] != reinterpret_cast<void*>(g_base + kRvaRootFrame)) { // (the shared root frame function)
        LOG("Outfit Arc: the status screen's frame call not found; no quick peek");
        return;
    }
    g_gauge_frame_orig = reinterpret_cast<FrameFn>(vt[kSlotFrame]); // (before the patch: the game may call it at once)
    void* prev = game::patch_vtable(vt, kSlotFrame, reinterpret_cast<void*>(&gauge_frame_hook));
    LOG("Outfit Arc: status screen frame hook %s (vtable +%llX)", prev ? "on" : "FAILED", reinterpret_cast<U64>(vt) - g_base);
}

} // namespace om
