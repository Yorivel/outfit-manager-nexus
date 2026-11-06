// The Outfit Arc and per-outfit hotkeys.
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

namespace om {

std::atomic<U32> g_peek_key{0};           // the Outfit Arc's key (a virtual-key code; 0 = Off)

// The current character's hotkeys for the input filters (another thread): vk | mods << 8 per outfit, 0 = none.
std::atomic<U32> g_hotkeys[16] = {};
std::atomic<bool> g_peek_pad{false};       // the Outfit Arc on controllers: hold L1 / LB, press the d-pad's down

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
// in the middle (position 2), the others around it, wrapping; it opens on the worn outfit, or on an extra "current
// outfit" entry when what is worn isn't a saved outfit (choosing it changes nothing).
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
    std::vector<int> entries;        // outfit indices in the Outfits tab's order; -1 = the current, unsaved outfit
    size_t sel = 0;                  // the highlighted entry (shown in the middle)
    int worn = -1;
    std::string current_icon;        // the worn chest piece's icon (the unsaved entry)
    U64 closing_at = 0;              // closing: its animation plays, then it hides (GetTickCount64 of the close; 0 = not)
    U16 character = 0xFFFF;          // whose outfits it shows (a switch closes it: they'd be the wrong ones)
    U64 self = 0;                    // the status screen it was opened from (loads and their questions go through it)
    U64 wrap = 0, arc = 0, frame[5] = {}, icon[5] = {}, label = 0, name = 0;
    U64 badge[5] = {}, nb[2] = {};   // each slot's climate badge; the highlighted outfit's badges by its label
    std::string badge_set[5], nb_set[2];
    U64 trk = 0, hp_gauge = 0;       // the HP tracker (the peek's home) and its own gauge (hidden while shown)
    std::string icon_set[5];         // last icon set per position
} g_peek;
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
    p = Peek{p.open, p.entries, p.sel, p.worn, p.current_icon};
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
            pic = e < 0 ? p.current_icon : e < int(g_outfits.size()) ? g_outfits[size_t(e)].chest_icon : std::string();
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
    }
    std::string label = e < 0 ? lang::t("peek.unsaved_label") : lang::f("peek.label", {{"n", std::to_string(e + 1)}});
    std::string name = e < 0 ? lang::t("peek.unsaved") : e < int(g_outfits.size()) ? g_outfits[size_t(e)].name : "";
    if (g_set_text) g_set_text(p.label, label.c_str(), 0), g_set_text(p.name, name.c_str(), 0);
}

// The animations (tools/peek_gen.py): the HUD key guide's unfold. Opening: every part has its own keyframes
// (om-peek-in-arc / -frame-k / -icon-k / -label / -name: the arc unfolds, the slots ride along and pop, the texts fade
// in, the middle frame pulses); closing: the whole peek fades with a slight shrink (om-peek-out-wrap). One set at a
// time; none between, so each plays again from its start.
enum class PeekAnim { None, In, Out };
constexpr U64 kPeekOutMs = 270; // the closing fade (peek_gen.py OUT_TOTAL, 0.25 s) and a little more

void peek_animate(PeekAnim a)
{
    Peek& p = g_peek;
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
    if (U64 box = find_by_class(p.wrap, "om-hk-caps", 1)) set_class(box, "om-peek-in-name", in);
    set_class(p.wrap, "om-peek-out-wrap", a == PeekAnim::Out);
}

void peek_open(U64 self)
{
    Peek& p = g_peek;
    p.self = self;
    prepare_outfits(); // (as the wheel does when it opens)
    p.entries.clear();
    p.worn = find_worn_outfit(self);
    p.current_icon.clear();
    if (p.worn < 0) { // what is worn isn't a saved outfit: the "current outfit" entry, first
        WornRaw w[kWornMax];
        int n = read_worn(self, w, kWornMax);
        for (int i = 0; i < n; ++i)
            if (w[i].slot == kChestSlot && w[i].icon) p.current_icon = w[i].icon;
        p.entries.push_back(-1);
    }
    for (size_t i = 0; i < g_outfits.size(); ++i) p.entries.push_back(int(i));
    p.sel = p.worn < 0 ? 0 : size_t(p.worn);
    p.open = true;
    p.character = g_character;
    set_class(p.trk, "!cpp-none", false); // (the HP tracker: shown while the peek is, its own gauge hidden)
    set_class(p.hp_gauge, "!cpp-none", true);
    peek_animate(PeekAnim::None); // (a close still playing: cut short, the opening starts over)
    p.closing_at = 0;
    peek_draw(self);
    peek_animate(PeekAnim::In);
    set_class(p.wrap, "!cpp-none", false);
    LOG("Outfit Arc: open (character %d, %zu outfit(s), worn %d, %zu entries)", int(g_character), g_outfits.size(), p.worn,
        p.entries.size());
}

void peek_close(bool choose)
{
    Peek& p = g_peek;
    int e = p.entries.empty() ? -1 : p.entries[p.sel];
    peek_animate(PeekAnim::None);
    peek_animate(PeekAnim::Out); // (hidden by peek_frame once it has played)
    p.closing_at = GetTickCount64();
    p.open = false;
    bool load = choose && e >= 0 && e != p.worn && static_cast<size_t>(e) < g_outfits.size();
    LOG("Outfit Arc: closed on %s%s (%s, wheel notches %u)", e < 0 ? "the current outfit" : ("outfit " + std::to_string(e + 1)).c_str(),
        !choose ? "" : load ? "; putting it on" : " (nothing to change)", g_peek_by_pad ? "pad" : "key",
        g_wheel_notches.load());
    // as a click on it in the wheel: where each piece is, the questions (pieces on other characters, copies), the load
    if (load) start_loading(p.self, static_cast<U32>(e));
}

// The outfits' hotkeys, in play (not in menus, prompts or with the arc open): a press of exactly the combination loads
// that outfit as a click in the wheel would. The character's own outfits are the ones in use (taken over when it
// changes). The filters (name_wndproc, raw_input_hook) keep the key from the game while its modifiers match.
void peek_hotkeys(U64 self, bool free)
{
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
        int worn = find_worn_outfit(self);
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

void peek_frame(U64 self)
{
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
    { // DIAG (pad arc, 2026-11-06): the status screen's frames stopping and resuming (a faded HUD?)
        U64 now = GetTickCount64(), last = g_peek_hud_at.load();
        if (last && now - last > 1000) LOG("DIAG Outfit Arc: status screen frames resumed after %llu ms", now - last);
    }
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
    U8 pad = g_peek_pad && game_focused() ? pad_buttons_raw() : 0, pressed = pad & ~g_peek_pad_last;
    g_peek_pad_last = pad;
    if ((pressed & kPadL1) || ((pad & kPadL1) && (pressed & kPadDown))) // DIAG (pad arc): every LB / LB + down press
        LOG("DIAG Outfit Arc: pad %02X pressed %02X (arc %s; menu %d, question %d, naming %d; focused %d)", pad, pressed,
            p.open ? "open" : "closed", int(g_menu_open.load()), g_ask_modal ? 1 : 0, g_name_modal ? 1 : 0,
            int(game_focused()));
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
            long long n = static_cast<long long>(p.entries.size());
            long long m = (static_cast<long long>(p.sel) - steps) % n;
            p.sel = static_cast<size_t>(m < 0 ? m + n : m);
            peek_draw(self);
        }
    }
    peek_sync_live(p.open);
    if (p.closing_at && GetTickCount64() - p.closing_at >= kPeekOutMs) { // the closing animation has played
        set_class(p.wrap, "!cpp-none", true);
        peek_animate(PeekAnim::None);
        p.closing_at = 0;
        set_class(p.trk, "!cpp-none", true); // (the HP tracker hidden again, as the game keeps it)
    }
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
    peek_frame(self);
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
