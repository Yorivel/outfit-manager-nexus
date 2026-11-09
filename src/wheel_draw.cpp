// The Outfits tab's slots: images, badges, key caps, mode dots, the ring, the centre tooltip.
#include "om.h"
#include "wheel_draw.h"
#include "addrs.h"
#include "ui.h"
#include "items.h"
#include "gear.h"
#include "looks.h"
#include "looks_view.h"
#include "settings.h"
#include "wheel_hooks.h"
#include "pads.h"
#include "arc.h"

namespace om {

std::uint8_t g_slot_mode[kOutfitSlots] = {};
ScrollFn g_scroll_prev_orig = nullptr, g_scroll_next_orig = nullptr;
SelectorSelectFn g_selector_select = nullptr;
// The slot's badge (browser mod CSS), on its occupancy-chain element (unused by our slots), above the picture and the
// focus, where the game draws its own slot badge (the lantern's Auto arrows): om-plus-c (the "+" slot's "+"),
// om-cog-c / om-flag-c (settings / conditions mode); om-plus-big / om-cog-big for the centred style. The worn wedge
// is on the disable element, the lowest layer.
constexpr bool kPlusSlotCentred = false; // the "+" slot: true = an "add" button (no silhouette, big centred "+" / cog,
                                          // style D); false = silhouette, "+" / cog in the badges' corner (style A)
U64 g_badge_elem[kOutfitSlots] = {};
U64 g_low_elem[kOutfitSlots] = {};   // the disable element: the worn wedge
bool g_low_shown[kOutfitSlots] = {};
const char* g_badge_class[kOutfitSlots] = {}; // badge shown on slot i (nullptr: none)
std::string g_plus_icon; // the "+" slot's picture: the worn chest while no saved outfit is worn ("" : the silhouette)
std::string g_worn_chest; // the worn chest's icon, whatever is worn (the "+" slot's picture since #21; "": the silhouette)

bool g_amber = false; // the anchor (the last loaded / exactly worn outfit) has been changed by hand: its highlight is amber
// Equipment slots the game refuses to empty ("There are no Items available to be equipped after removal": the main
// hand, the shield, the dagger, the lantern - seen; more are learned from that refusal and kept in the file): an outfit
// that leaves one empty still counts as worn whatever is in it.
std::set<U16> g_keep_slots = {0, 1, 12, 15};
// g_keep_slots and g_placeholder_keys are changed on the UI thread and written into the outfit file by any thread
// (save writes are seen on the game's threads): changes and the file's copy hold this lock.
std::mutex g_keys_mu;
U64 g_mark_elem[kOutfitSlots] = {};  // the Appearance marker's element (the charge-count wrap)
bool g_mark_shown[kOutfitSlots] = {}; // the game re-hides it on focus changes: shown again like the badges

void reset_modes() { std::fill(std::begin(g_slot_mode), std::end(g_slot_mode), std::uint8_t(kModeMain)); }

// --- The game's depth menu (the lantern's Auto / On / Off / Back ring) -----------------------------------------------
// Ours is never opened (the settings and visibility views are on the wheel itself); while the game's own is open (the
// lantern's), its presses and sectors are the game's.

bool in_depth(U64 self)
{
    unsigned char v = 0;
    return kOffRootFocusIdx && rd(self + kOffRootFocusIdx, v) && v;
}
U32 g_depth_outfit = 0; // the saved outfit the conditions' texts are for

// Set while the plugin itself is calling into the game (pump, prompt answers): callbacks the game fires
// synchronously meanwhile (focus, frame getter, cursor) must not start another round of work.
int g_busy = 0;

int g_force_images = 0; // frames left to re-set the chest textures (after focus changes / opening)

// Shows `icon` on slot i's item image ("" = hide it); false if nothing is shown.
bool show_slot_image(U64 self, U64 root, U32 i, const std::string& icon)
{
    U64 img = find_by_class(root, "quickslot-item-img");
    if (!img) return false;
    SlotImage& si = g_slot_image[i];
    if (!icon.empty() && (si.elem != img || si.icon != icon)) {
        int r = set_image_raw(self, img, icon.c_str());
        si.elem = img;
        si.icon = icon;
        si.ok = r == 1;
        LOG("slot %u image %s: %s (%s)", i, icon.c_str(), r == 1 ? "set" : r == 0 ? "no such texture" : "FAULT",
            describe(img).c_str());
    }
    bool on = !icon.empty() && si.ok;
    si.shown = on;
    set_class(img, "!cpp-none", !on);
    return on;
}
void keep_mode_dots(U64 self);

// The game resets a slot's item image when focus moves (it holds no item for our slots): put the shown
// chest images back. `force` also re-sets the texture (the reset may clear it without hiding the element).

void keep_slot_images(U64 self, bool force)
{
    keep_mode_dots(self);
    for (U32 i = 0; i < kOutfitSlots; ++i) {
        SlotImage& si = g_slot_image[i];
        if (!si.shown || !si.elem || !is_element(si.elem)) continue;
        bool hidden = has_class(si.elem, "!cpp-none");
        if (!hidden && !force) continue;
        if (hidden && !g_rehide_logged) {
            g_rehide_logged = true;
            LOG("slot %u image was hidden by the game (%s)", i, describe(si.elem).c_str());
        }
        if (set_image_raw(self, si.elem, si.icon.c_str()) != 1) continue;
        set_class(si.elem, "!cpp-none", false);
        set_class(si.elem, "om-missing", si.missing);
        set_class(si.elem, "om-present", !si.missing);
    }
    for (U32 i = 0; i < kOutfitSlots; ++i) { // badges and the worn wedge, if the game re-hides their elements
        if (g_badge_class[i] && is_element(g_badge_elem[i]) && has_class(g_badge_elem[i], "!cpp-none"))
            set_class(g_badge_elem[i], "!cpp-none", false);
        if (g_low_shown[i] && is_element(g_low_elem[i]) && has_class(g_low_elem[i], "!cpp-none"))
            set_class(g_low_elem[i], "!cpp-none", false);
        if (g_mark_shown[i] && is_element(g_mark_elem[i]) && has_class(g_mark_elem[i], "!cpp-none"))
            set_class(g_mark_elem[i], "!cpp-none", false);
    }
}

// Visible slots share the whole wheel (browser mod CSS, generated by tools/omring_gen.py): om-list-N on
// the tab body turns its radial layout off (it misplaces slots 1+ at 360/N, and turning it back on for 12
// leaves stale positions) and om-sN-i places slot i at (i+1)*360/N. Below 12, om-ring-N gives the ring N
// segments (12 keeps the vanilla ring), and om-fN-i turns slot i's focus into its 360/N wedge (for every N, so
// all counts share the comrades-wheel focus look).
U32 wheel_n() { return visible_slots() < kOutfitSlots ? visible_slots() : 0; }

std::string numbered(const char* fmt, U32 a, U32 b)
{
    char s[32];
    std::snprintf(s, sizeof s, fmt, a, b);
    return s;
}

std::string g_list_class;
std::string g_slot_class[kOutfitSlots];
std::string g_focus_class[kOutfitSlots];
std::string g_worn_class[kOutfitSlots];
std::string g_placeholder_class[kOutfitSlots]; // the Visibility view: an empty slot's icon (the game's own for that slot)

// The saved outfit the character is wearing (-1: none) gets the game's "selected" wedge in its segment (om-wN-i on
// the slot's otherwise unused reserve element, below the focus) and the "Equipped" tag in the centre tooltip.
int g_worn_outfit = -1;
bool g_worn_changed = false; // the centre tooltip's tag follows on the next pump

// Outfits with a saved piece that is gone (no other copy to offer) are greyed out (om-missing, browser mod CSS).

// The select shows the dots in pages of +0xf4 (the dots per page; only the lit dot's page is drawn). It stays at the
// last view's count (closing the wheel keeps it too), so ours is written with the count: one page. (Closing the wheel
// in the settings / Visibility also left the dots of the next opening's changes unshown: close_hook puts the slots back
// to the outfits first.) Flag 1 skips the select's sound; 0 plays it, kept for a scroll, where the lit dot moves.

// Outfits tab: saved outfits, then one empty "+" slot (until all 12 are used); the rest hidden.
bool g_slots_logged = false;
U32 g_applied_visible = 0; // visible slot count the classes were last set for
std::map<U16, std::pair<int, int>> g_item_resist; // key -> {ice, fire}

const char* look_badge(std::uint8_t look)
{
    return look == kLookHidden ? "om-look-hidden-c" : look == kLookCombat ? "om-look-combat-c"
         : look == kLookPeace  ? "om-look-peace-c"  : look == kLookShown  ? "om-look-shown-c" : nullptr;
}
const char* prompt_class()
{
    return !g_pad_in_use ? "om-prompt-c" : g_pad_is_ps ? "om-cross-c" : "om-abtn-c";
}
// (g_badge_class keeps a pointer: into kBadges)
size_t badge_index(const char* c)
{
    for (size_t k = 0; k < std::size(kBadges); ++k)
        if (!std::strcmp(kBadges[k], c)) return k;
    return 0;
}

// Slot i of the Appearance view: a piece (its icon, its look's badge, dimmed when hidden or not in the inventory), the
// page slot (the silhouette of the page chosen) or Back (the label "Back" and the click prompt, as in the settings).

// A setting's value under its name, in the game's gold caption style: a second text in the slot's label wrap
// (OmValueText, added to the game's compiled slot by tools/uibin_tab.py; the name's text holds one colour only). The
// wrap shows with the label. "" hides it; without the element (the patch not mounted) nothing is shown.
// The game's key icon for a key (Common_Icon_Keyguide_PC_<name>), wide: a wide cap (120 px) instead of a square one;
// null: the game has none (the text is shown instead).
const char* key_icon(U32 vk, bool& wide)
{
    static char buf[64];
    wide = false;
    const char* n = nullptr;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        snprintf(buf, sizeof buf, "Common_Icon_Keyguide_PC_%c", char(vk));
        return buf;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        snprintf(buf, sizeof buf, "Common_Icon_Keyguide_PC_F%u", vk - VK_F1 + 1);
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(buf, sizeof buf, "Common_Icon_Keyguide_PC_num%u", vk - VK_NUMPAD0);
        return buf;
    }
    switch (vk) {
    case VK_CONTROL: n = "Ctrl"; wide = true; break;
    case VK_MENU: n = "Alt"; wide = true; break;
    case VK_SHIFT: n = "Shift"; wide = true; break;
    case VK_TAB: n = "Tab"; wide = true; break;
    case VK_CAPITAL: n = "Caps"; wide = true; break;
    case VK_BACK: n = "Backspace"; wide = true; break;
    case VK_INSERT: n = "Insert"; wide = true; break;
    case VK_DELETE: n = "Del"; wide = true; break;
    case VK_HOME: n = "Home"; wide = true; break;
    case VK_END: n = "End"; wide = true; break;
    case VK_PRIOR: n = "Pageup"; wide = true; break;
    case VK_NEXT: n = "Pagedown"; wide = true; break;
    case VK_SPACE: n = "Spacer"; wide = true; break;
    case VK_RETURN: n = "Enter"; break;
    case VK_LEFT: n = "Left"; break;
    case VK_RIGHT: n = "Right"; break;
    case VK_UP: n = "Up"; break;
    case VK_DOWN: n = "Down"; break;
    case VK_OEM_3: n = "Backquote"; break;
    case VK_OEM_MINUS: n = "Hyphen"; break;
    case VK_OEM_PLUS: n = "Plus"; break;
    case VK_OEM_4: n = "Lbracket"; break;
    case VK_OEM_6: n = "Rbracket"; break;
    case VK_OEM_5: n = "Backslash"; break;
    case VK_OEM_1: n = "Semicolon"; break;
    case VK_OEM_7: n = "Doublequotes"; break;
    case VK_OEM_COMMA: n = "Comma"; break;
    case VK_OEM_PERIOD: n = "Dot"; break;
    case VK_OEM_2: n = "Slash"; break;
    case VK_MULTIPLY: n = "num_Asterisk"; break;
    case VK_ADD: n = "num_Plus"; break;
    case VK_SUBTRACT: n = "num_Minus"; break;
    case VK_DECIMAL: n = "num_Dot"; break;
    case VK_DIVIDE: n = "num_Slash"; break;
    case VK_MBUTTON: n = "Mouse_Wheel"; break;
    case VK_XBUTTON1: n = "Mouse_x1"; break;
    case VK_XBUTTON2: n = "Mouse_x2"; break;
    default: return nullptr;
    }
    snprintf(buf, sizeof buf, "Common_Icon_Keyguide_PC_%s", n);
    return buf;
}

// The hotkey as key caps on the slot (OmHkCaps): false if the slot has none or a key has no icon (the text then).
// where: 1 under a plain anchor, 2 under the eye marker.
bool show_caps(U64 self, U64 root, U32 vk, std::uint8_t mods, int where)
{
    U64 box = find_by_class(root, "om-hk-caps");
    if (!box) return false;
    bool wide = false;
    const char* key = key_icon(vk, wide);
    if (!key) return set_class(box, "!cpp-none", true), false;
    const U32 mvk[3] = {VK_CONTROL, VK_MENU, VK_SHIFT};
    const std::uint8_t bit[3] = {1, 2, 4};
    for (int k = 0; k < 3; ++k) {
        U64 cap = find_by_class(box, ("om-hk-c" + std::to_string(2 * k)).c_str(), 1);
        U64 plus = find_by_class(box, ("om-hk-c" + std::to_string(2 * k + 1)).c_str(), 1);
        bool on = (mods & bit[k]) != 0;
        bool w = false;
        if (cap && on) set_image_raw(self, cap, key_icon(mvk[k], w));
        if (cap) set_class(cap, "!cpp-none", !on);
        if (plus) {
            if (on && g_set_text) g_set_text(plus, "+", 0);
            set_class(plus, "!cpp-none", !on);
        }
    }
    U64 cap = find_by_class(box, "om-hk-c6", 1);
    if (!cap) return false;
    set_image_raw(self, cap, key_icon(vk, wide));
    set_class(cap, "om-hk-wide", wide);
    set_class(cap, "!cpp-none", false);
    set_class(box, "om-hk-caps-plain", where == 1);
    set_class(box, "om-hk-caps-eye", where == 2);
    set_class(box, "!cpp-none", false);
    return true;
}

                                                                                       // anchor, 2 under the eye
bool show_value(U64 root, const std::string& text, int hotkey, bool caps)
{
    if (!caps)
        if (U64 box = find_by_class(root, "om-hk-caps")) set_class(box, "!cpp-none", true);
    U64 t = find_by_class(root, "om-value-text");
    if (!t) return true;
    if (!text.empty() && g_set_text) g_set_text(t, text.c_str(), 0);
    set_class(t, "om-hotkey-plain", hotkey == 1); // (an outfit's hotkey on its slot: under the icon)
    set_class(t, "om-hotkey-eye", hotkey == 2);
    return set_class(t, "!cpp-none", text.empty());
}
bool clear_label(U64 mark);
void apply_eye_slot(U64 self, U64 script, U64 root, U32 i, U32 visible, const std::vector<size_t>& pieces)
{
    U32 k = i < visible ? slot_outfit(i) : kOutfitSlots;
    bool piece = k < pieces.size(), page = k == pieces.size(), back = k == pieces.size() + 1;
    const bool under = piece && pieces[k] == kUnderwearPiece; // (no item: a label, like the page slot)
    OutfitItem tmp{};
    const OutfitItem* it = piece ? eye_item(pieces[k], tmp) : nullptr;
    std::string pic = it ? piece_icon(*it) : std::string();
    show_slot_image(self, root, i, pic);
    if (i < visible) show_mode_dots(script, mode_count(i), mode_lit(i));
    U64 badge = find_by_class(root, "quickslot-occupancy-icon-chain"), low = find_by_class(root, "cpp-icon-disable-item");
    // (Back and the page slot: the click prompt, as actions - the page slot's click opens the page its dots chose)
    const char* want = it ? look_badge(it->look) : back || page ? prompt_class() : nullptr;
    g_badge_elem[i] = badge, g_badge_class[i] = want;
    bool ok = true;
    if (badge) {
        ok &= set_class(badge, "self-center", true);
        for (const char* c : kBadges) ok &= set_class(badge, c, want && !std::strcmp(want, c));
        ok &= set_class(badge, "!cpp-none", !want);
    }
    // Back and the page slot: a label (the page slot names the page its dots chose - its silhouette was easy to misread
    // while scrolling through the three, #23)
    static const char* const kPageName[kEyeSteps] = {"page.armour", "page.weapons", "page.accessories"};
    const bool labelled = back || page || under;
    U64 mark = find_parent_of_class(root, "charged-count");
    if (mark) {
        ok &= labelled ? set_class(mark, "self-center", true) && set_class(mark, "om-label-wrap", true) : clear_label(mark);
        ok &= set_class(mark, "om-eye-c", false) && set_class(mark, "om-eye2-c", false);
        ok &= set_class(mark, "!cpp-none", !labelled);
        if (U64 text = labelled ? find_by_class(mark, "charged-count") : 0) {
            const char* label = back    ? lang::t("setting.back") // (Back: the settings' word, translated)
                                : under ? lang::t("piece.underwear")
                                        : lang::t(kPageName[g_eye_pick % kEyeSteps]);
            if (g_set_text) g_set_text(text, label, 0);
            ok &= set_class(text, "om-label", true) && set_class(text, "om-label-value", false);
        }
    }
    g_mark_elem[i] = mark, g_mark_shown[i] = mark && labelled;
    ok &= show_value(root, "");
    bool dim = it && it->look != kLookDefault && it->look != kLookShown; // (a look that hides it at times)
    g_slot_image[i].missing = dim;
    ok &= set_class(root, "!cpp-none", i >= visible);
    if (U64 img = find_by_class(root, "quickslot-item-img"))
        ok &= set_class(img, "om-missing", dim) && set_class(img, "om-present", !dim);
    ok &= swap_class(root, g_slot_class[i], i < visible ? numbered("om-s%u-%u", visible, i) : "");
    U64 icon = rp(script + kOffScriptCategoryIcon);
    if (icon && is_element(icon)) {
        ok &= set_class(icon, "quickslot-item-image-none", false);
        const bool sword = false, necklace = false; // (the page slot: its label now, no silhouette)
        bool holder = piece && !under && pic.empty() && it->slot != kBackShieldSlot; // (an empty slot: the game's placeholder)
        bool armor = piece && pic.empty() && !holder;
        ok &= set_class(icon, "om-empty-armor", armor);
        ok &= set_class(icon, "om-empty-sword", sword);
        ok &= set_class(icon, "om-empty-necklace", necklace);
        ok &= swap_class(icon, g_placeholder_class[i], holder ? "quickslot-item-image-equipment-" + std::to_string(it->slot) : std::string());
        ok &= set_class(icon, "!cpp-none", !armor && !sword && !necklace && !holder);
        ok &= set_class(icon, "om-missing", holder && dim);
    }
    g_low_elem[i] = low, g_low_shown[i] = false;
    if (low) ok &= swap_class(low, g_worn_class[i], "") && set_class(low, "!cpp-none", true);
    if (U64 focus = find_by_class(root, "quickslot-item-focus-bg-10"))
        ok &= swap_class(focus, g_focus_class[i], i < visible ? numbered("om-f%u-%u", visible, i) : "");
    if (!ok) LOG("  appearance slot %u: class change skipped (%s)", i, describe(root).c_str());
}

// The settings view's label on a slot's charge-count wrap: taken off (the outfits and the pieces don't show one).
bool clear_label(U64 mark)
{
    if (!mark) return true;
    bool ok = set_class(mark, "om-label-wrap", false);
    U64 text = find_by_class(mark, "charged-count");
    if (text && (has_class(text, "om-label") || has_class(text, "om-label-value"))) {
        if (g_set_text) g_set_text(text, "", 0);
        ok &= set_class(text, "om-label", false) && set_class(text, "om-label-value", false);
    }
    return ok;
}

extern U64 g_ring_bg; // (the ring's element: sync_ring below)
// The settle-in (a fade and scale when the slot count changes while the wheel is open): the class each element has.
std::string g_settle_list, g_settle_ring;
bool g_settle_b = false;
U32 g_settle_from = 0; // (the count before the change: set by apply_outfit_slots, played by both elements)
void settle(U64 elem, std::string& cls)
{
    if (elem && is_element(elem)) swap_class(elem, cls, g_settle_b ? "om-settle-b" : "om-settle-a");
}

void apply_outfit_slots(U64 self, U32 only)
{
    U32 tab = g_fake_key.load();
    if (tab == 0xFFFF || !layout_ok(self)) return;
    U64 sv = rp(self + kOffRootSlotVecs);
    if (!sv || r32(self + kOffRootSlotVecCount) <= tab) return;
    U64 arr = rp(sv + tab * 0x10ull);
    U32 cnt = r32(sv + tab * 0x10ull + 8);
    U32 visible = visible_slots();
    if (only >= kOutfitSlots) { // (the whole wheel)
        // (the count changed with the wheel open: the new layout settles in instead of snapping; not on opening)
        const bool settles = g_menu_open && g_applied_visible != 0 && g_applied_visible != visible;
        if (settles) g_settle_b = !g_settle_b, g_settle_from = g_applied_visible;
        g_applied_visible = visible;
        // (during a character switch the worn gear is already the new character's while the list is still the old
        // one's: comparing them would read as "a new set" and drop the old character's looks, 2026-11-06)
        U16 now_char = current_character();
        bool same_char = now_char == kNoCharacter || g_character == kNoCharacter || now_char == g_character;
        if (int w = same_char ? find_worn_outfit(self) : g_worn_outfit; w != g_worn_outfit) {
            LOG("worn outfit: %d -> %d", g_worn_outfit, w);
            g_worn_outfit = w;
            g_worn_changed = true;
            // (put together by hand: its looks apply too; none worn keeps the last loaded; let go (#21): not)
            if (w >= 0 && !let_go(w)) mark_worn(w);
        }
        if (same_char) update_plus_icon(self);
        refresh_missing(self);
        U64 body = rp(rp(rp(self + kOffRootBodies) + tab * 8ull) + 8);
        bool list_ok = body && is_element(body) && swap_class(body, g_list_class, numbered("om-list-%u", visible));
        if (settles) settle(body, g_settle_list), settle(g_ring_bg, g_settle_ring);
        if (!g_slots_logged || !list_ok)
            LOG("  outfit list%s: %s", list_ok ? "" : " (class change skipped)", body ? describe(body).c_str() : "null");
    }
    std::vector<size_t> pieces = eye_pieces();
    const int worn_set = g_eye < 0 && g_cfg < 0 ? worn_weapon_set(self) : -1; // (#27: its badge is gold)
    const bool target_is_set = fx_busy_target() >= 0 && size_t(fx_busy_target()) < g_outfits.size() &&
                               is_weapon_set(g_outfits[size_t(fx_busy_target())]);
    g_worn_set = worn_set;
    for (U32 i = 0; arr && i < cnt && i < kOutfitSlots; ++i) {
        if (only < kOutfitSlots && i != only) continue;
        U64 script = rp(arr + i * 8ull);
        U64 root = script ? rp(script + 8) : 0;
        if (!root || !is_element(root)) continue;
        if (g_eye >= 0) {
            apply_eye_slot(self, script, root, i, visible, pieces);
            continue;
        }
        if (g_cfg >= 0) {
            apply_cfg_slot(self, script, root, i, visible);
            continue;
        }
        bool empty = i < visible && slot_outfit(i) >= g_outfit_count;
        bool saved = i < visible && !empty;
        std::uint8_t mode = i < visible ? slot_mode_of(i) : std::uint8_t(kModeMain);
        // (the "+" slot: the worn chest, whatever is worn - what it unequips / what saving would keep)
        const bool set = saved && is_weapon_set(g_outfits[slot_outfit(i)]); // (a weapon set, #27)
        std::string pic = set ? weapon_set_picture(g_outfits[slot_outfit(i)])
                          : saved ? g_outfits[slot_outfit(i)].chest_icon : empty ? g_worn_chest : std::string();
        if (saved && !set && pic.empty())
            if (const OutfitItem* c = outfit_piece(g_outfits[slot_outfit(i)], kChestSlot)) pic = piece_icon(*c);
        bool chest = show_slot_image(self, root, i, pic);
        if (i < visible) show_mode_dots(script, mode_count(i), g_slot_mode[i]); // (the dot, not what it does)
        U64 badge = find_by_class(root, "quickslot-occupancy-icon-chain"), low = find_by_class(root, "cpp-icon-disable-item");
        const char* want = mode == kModeRelease && empty  ? "om-unequip-c" // (Unequip Outfit: the slashed armour)
                           : mode != kModeMain && empty  ? (kPlusSlotCentred ? "om-cog-big" : "om-cog-c") // (settings 1 / 2)
                           : mode == kModeSettings               ? "om-cog-c"
                           : mode == kModeAppearance             ? "om-eye-c"
                           : empty && g_outfit_count < kMaxOutfits ? (kPlusSlotCentred ? "om-plus-big" : "om-plus-c")
                           : set ? (int(slot_outfit(i)) == fx_busy_target() ? "om-wset-busy-c" // (loading: pulses)
                                    : int(slot_outfit(i)) == worn_set ? "om-wset-gold-c" : "om-wset-c") // (#27)
                           : saved ? climate_badge(g_outfits[slot_outfit(i)]) // (its Climate marker, or none)
                                                                   : nullptr; // (at the cap: no "+")
        g_badge_elem[i] = badge, g_badge_class[i] = want;
        bool badge_ok = true;
        if (badge) {
            badge_ok &= set_class(badge, "self-center", true);
            for (const char* c : kBadges) badge_ok &= set_class(badge, c, want && !std::strcmp(want, c));
            badge_ok &= set_class(badge, "!cpp-none", !want);
        }
        // Appearance marker: a saved outfit with looks set shows the eye (main mode), above its climate thermometer when
        // it has one, else on the badge spot; on the charge-count wrap (unused by our slots), a second badge element.
        U64 mark = find_parent_of_class(root, "charged-count");
        badge_ok &= clear_label(mark);
        // (an outfit's hotkey shows under its icon: in the same wrap, an anchor when no eye shows there)
        const Outfit* ho = saved && mode == kModeMain ? &g_outfits[slot_outfit(i)] : nullptr;
        bool hk = ho && ho->hk_vk;
        bool looks = false;
        if (mark) {
            if (saved && !set && mode == kModeMain) // (a weapon set has no looks, #27)
                for (U16 s = 0; s < 32; ++s) looks |= slot_look(g_outfits[slot_outfit(i)], s) != kLookDefault;
            if (saved && !set && mode == kModeMain) looks |= g_outfits[slot_outfit(i)].underwear_look != kLookDefault;
            bool climate = want && (!std::strncmp(want, "om-cold", 7) || !std::strncmp(want, "om-heat", 7) ||
                                    !std::strcmp(want, "om-sleep-c"));
            bool stacked = looks && climate;
            badge_ok &= set_class(mark, "self-center", true);
            badge_ok &= set_class(mark, "om-eye-c", looks && !stacked);
            badge_ok &= set_class(mark, "om-eye2-c", stacked);
            badge_ok &= set_class(mark, "om-label-wrap", hk && !looks);
            badge_ok &= set_class(mark, "!cpp-none", !looks && !hk);
        }
        g_mark_elem[i] = mark, g_mark_shown[i] = mark && saved && mode == kModeMain &&
                                                 (has_class(mark, "om-eye-c") || has_class(mark, "om-eye2-c") || hk);
        // (hk: the hotkey's text lives in this wrap too; the game hides it on focus changes, 2026-11-06)
        bool caps = hk && show_caps(self, root, ho->hk_vk, ho->hk_mods, looks ? 2 : 1); // (the game's key icons)
        show_value(root, hk && !caps ? hotkey_name(ho->hk_vk, ho->hk_mods) : std::string(), hk && !caps ? (looks ? 2 : 1) : 0,
                   caps);
        bool missing = saved && outfit_missing(slot_outfit(i));
        g_slot_image[i].missing = missing;
        bool ok = set_class(root, "!cpp-none", i >= visible) && badge_ok;
        if (U64 img = find_by_class(root, "quickslot-item-img"))
            ok &= set_class(img, "om-missing", missing) && set_class(img, "om-present", !missing);
        ok &= swap_class(root, g_slot_class[i], i < visible ? numbered("om-s%u-%u", visible, i) : "");
        U64 icon = rp(script + kOffScriptCategoryIcon);
        if (icon && is_element(icon)) {
            ok &= set_class(icon, "quickslot-item-image-none", false);
            ok &= swap_class(icon, g_placeholder_class[i], "");
            bool sil = (saved && !chest) || (empty && !chest && !kPlusSlotCentred); // no chest / the "+" slot (style A)
            ok &= set_class(icon, "om-empty-armor", sil);
            ok &= set_class(icon, "!cpp-none", !sil);
            ok &= set_class(icon, "om-missing", missing);
        }
        // (unequipped: the gear may still be that outfit's, but no outfit is applied - only the "+" slot shows it, #21)
        bool worn = saved && int(slot_outfit(i)) == g_worn_outfit && !g_unmanaged.count(g_character);
        // Amber: the last loaded outfit while what is worn has been changed by hand since (not saved); green: the "+" slot
        // while an unsaved set is worn (its chest is previewed there).
        bool drifted = !worn && saved && g_worn_outfit < 0 && g_outfits[slot_outfit(i)].worn && g_amber;
        // (the "+" slot: green on its Save dot while what is worn can be saved - no saved outfit is it; white while no
        // outfit is applied: what is worn is "the current outfit", #21)
        bool ready = empty && mode == kModeMain && g_worn_outfit < 0 && g_outfit_count < kMaxOutfits;
        bool current = empty && !ready && anchor_outfit() < 0;
        // While an outfit change's effect plays, only the slot it is about pulses (the outfit going on; the "+" slot for
        // Unequip Outfit): the wheel's other highlights follow once it has landed.
        // (a weapon set's change: its badge pulses instead, the wedges - the applied outfit's - stay as they are, #27)
        const int target = target_is_set ? -1 : fx_busy_target();
        const bool busy = target != -1 && ((saved && int(slot_outfit(i)) == target) || (empty && target == -2));
        if (target != -1) worn = drifted = ready = current = false;
        g_low_elem[i] = low, g_low_shown[i] = busy || worn || drifted || ready || current;
        if (low) {
            ok &= swap_class(low, g_worn_class[i], busy ? numbered("om-wb%u-%u", visible, i)
                                                   : worn || current ? numbered("om-w%u-%u", visible, i)
                                                   : drifted ? numbered("om-wa%u-%u", visible, i)
                                                   : ready   ? numbered("om-wg%u-%u", visible, i) : "");
            ok &= set_class(low, "!cpp-none", !g_low_shown[i]); // (hidden by the game's markup)
        }
        U64 focus = find_by_class(root, "quickslot-item-focus-bg-10");
        if (focus)
            ok &= swap_class(focus, g_focus_class[i], i < visible ? numbered("om-f%u-%u", visible, i) : "");
        if (!g_slots_logged || !ok) {
            LOG("  outfit slot %u%s: root %s", i, ok ? "" : " (class change skipped)", describe(root).c_str());
            LOG("    icon %s", icon ? describe(icon).c_str() : "null");
            LOG("    badge %s", badge ? describe(badge).c_str() : "null");
            LOG("    mark %s", mark ? describe(mark).c_str() : "null");
            LOG("    worn %s", low ? describe(low).c_str() : "null");
            LOG("    focus %s", focus ? describe(focus).c_str() : "null");
        }
    }
    g_slots_logged = true;
}

// A weapon set's picture (#27): its main weapon (equipment-0, else its first weapon) - saved with it, else from the item.
std::string weapon_set_picture(const Outfit& o)
{
    if (!o.weapon_icon.empty()) return o.weapon_icon;
    const OutfitItem* first = nullptr;
    for (const OutfitItem& it : o.items)
        if (is_weapon_slot(it.slot) && it.key != 0xFFFF) {
            if (it.slot == kHandR) return piece_icon(it);
            if (!first) first = &it;
        }
    return first ? piece_icon(*first) : o.chest_icon;
}
int g_worn_set = -1; // (the weapon set worn when the slots were last drawn)

U64 g_outfits_root_of_slots() { return g_fake_key.load() != 0xFFFF ? g_outfits_root : 0; }

bool outfits_current(U64 self)
{
    U32 tab = g_fake_key.load();
    if (tab == 0xFFFF || !g_layout_bad.empty() || self != g_outfits_root) return false;
    U64 tb = rp(self + kOffRootTabBar);
    return tb && r32(tb + kOffTabCurrent) == tab;
}

// Saved outfit: outfit / settings / conditions / appearance; the "+" slot: + / settings 1 / settings 2 (mode 2 there =
// page 2). The Appearance view: the page slot's three pages, a piece's looks, Back none.

U32 mode_count(U32 i)
{
    if (g_eye >= 0) {
        size_t k = slot_outfit(i), n = eye_pieces().size();
        return k < n ? kLookCount : k == n ? kEyeSteps : 0;
    }
    if (g_cfg >= 0) return cfg_dots(i, nullptr);
    // (an outfit: its action, its settings; the "+" slot once outfits exist: Unequip Outfit, Save, Settings - #21)
    return slot_outfit(i) == g_outfits.size() && !g_outfits.empty() ? 3 : 2;
}
// What slot i's lit dot does (a SlotMode): the dot itself, but the "+" slot's three are Unequip / Save / Settings.
std::uint8_t slot_mode_of(U32 i)
{
    const std::uint8_t dot = i < kOutfitSlots ? g_slot_mode[i] : std::uint8_t(kModeMain);
    if (g_eye >= 0 || g_cfg >= 0 || i >= visible_slots() || slot_outfit(i) != g_outfits.size() || g_outfits.empty())
        return dot;
    static const std::uint8_t kPlus[3] = {kModeRelease, kModeMain, kModeSettings};
    return kPlus[dot % 3];
}
// The dot lit on slot i: its mode; in the Appearance view a piece's look, the page slot's step.
U32 mode_lit(U32 i)
{
    if (g_cfg >= 0) {
        U32 lit = 0;
        return cfg_dots(i, &lit) ? lit : 0xFFFFFFFFu;
    }
    if (g_eye < 0) return g_slot_mode[i];
    std::vector<size_t> pieces = eye_pieces();
    U32 k = slot_outfit(i);
    return k < pieces.size() ? look_pos(eye_look(pieces[k])) : k == pieces.size() ? g_eye_pick : 0xFFFFFFFFu;
}
// The dot lit on slot i.

// The slot's dots: `count` of them, `mode` lit (the game sizes them from the count field when one is lit).
bool g_dots_logged = false;
SelectorSelectHookFn g_selector_select_orig = nullptr;
SlotRefreshFn g_slot_refresh_orig = nullptr;

void __fastcall slot_refresh_hook(U64 script)
{
    U64 root = script ? rp(script + 0x28) : 0;
    if (root && root == g_outfits_root_of_slots() && outfit_slot_index(root, script) < kOutfitSlots) return;
    g_slot_refresh_orig(script);
}

void __fastcall selector_select_hook(U64 sel, U32 index, U64 flag)
{
    U64 root = g_outfits_root_of_slots();
    U32 tab = g_fake_key.load();
    U64 sv = sel && root && g_menu_open && GetCurrentThreadId() == g_ui_thread ? rp(root + kOffRootSlotVecs) : 0;
    U64 arr = sv ? rp(sv + tab * 0x10ull) : 0;
    U32 cnt = arr ? r32(sv + tab * 0x10ull + 8) : 0;
    for (U32 i = 0; i < cnt && i < visible_slots() && i < kOutfitSlots; ++i)
        if (U64 script = rp(arr + i * 8ull); script && rp(script + kOffSlotSelector) == sel) {
            U32 count = mode_count(i), page = count ? count : 1;
            bool resized = r32(sel + kOffSelectorCount) != count || r32(sel + kOffSelectorPage) != page;
            if (resized) wr<U32>(sel + kOffSelectorCount, count), wr<U32>(sel + kOffSelectorPage, page);
            g_selector_select_orig(sel, mode_lit(i), flag);
            return;
        }
    g_selector_select_orig(sel, index, flag);
}

void show_mode_dots(U64 script, U32 count, U32 mode)
{
    if (!g_selector_select) return;
    U64 sel = rp(script + kOffSlotSelector);
    if (!has_vt(sel, kRvaItemSelectorVt)) {
        if (!g_dots_logged) g_dots_logged = true, LOG("slot dots: script %llX has no selector (%llX)", script, sel);
        return;
    }
    // Unchanged: left alone (re-selecting rebuilds the dots: they blinked whenever the wheel was re-applied).
    U32 page = count ? count : 1;
    bool resized = r32(sel + kOffSelectorCount) != count || r32(sel + kOffSelectorPage) != page;
    bool moved = kOffSelectorLit && r32(sel + kOffSelectorLit) != mode;
    if (!resized && !moved) return;
    if (r32(sel + kOffSelectorPage) != page && !wr<U32>(sel + kOffSelectorPage, page)) return;
    if (r32(sel + kOffSelectorCount) != count && !wr<U32>(sel + kOffSelectorCount, count)) return;
    __try {
        g_selector_select(sel, mode, moved && !resized ? 0 : 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_selector_select = nullptr;
    }
    if (!g_dots_logged) g_dots_logged = true, LOG("slot dots: %u, %u lit (selector %llX)", count, mode, sel);
}

// Every frame: the visible slots' dots as they should be (cheap: two reads each; the select only on a difference).
void keep_mode_dots(U64 self)
{
    U32 tab = g_fake_key.load();
    U64 sv = tab != 0xFFFF ? rp(self + kOffRootSlotVecs) : 0;
    if (!sv || !g_selector_select) return;
    U64 arr = rp(sv + tab * 0x10ull);
    U32 cnt = r32(sv + tab * 0x10ull + 8), visible = visible_slots();
    for (U32 i = 0; arr && i < cnt && i < visible && i < kOutfitSlots; ++i)
        if (U64 script = rp(arr + i * 8ull)) show_mode_dots(script, mode_count(i), mode_lit(i));
}

// The 12-slot ring texture (QuickSlotItemListBg) is shared by every tab body: on the Outfits tab it gets the
// N-segment ring, and the radial cursor (whose sector count the tab change sets from the tab's 12 slot
// scripts) gets N sectors so the stick picks slots by the same angles.
U64 g_ring_bg = 0;
std::string g_ring_class;
bool g_ring_logged = false;

void sync_ring(U64 self)
{
    if (g_fake_key.load() == 0xFFFF || self != g_outfits_root || in_depth(self)) return; // (submenu: own sectors)
    U64 menu = rp(self + 8);
    if (!g_ring_bg || !is_element(g_ring_bg)) g_ring_bg = find_by_class(menu, "cpp-item-list-background", 24);
    bool on = outfits_current(self);
    U32 n = on ? wheel_n() : 0;
    bool ok = g_ring_bg && swap_class(g_ring_bg, g_ring_class, n ? numbered("om-ring-%u", n) : "");
    U64 cursor = rp(self + kOffRootCursor);
    U16 sectors = 0;
    if (on && cursor && kOffCursorSectors && rd(cursor + kOffCursorSectors, sectors) && sectors <= kOutfitSlots &&
        sectors != visible_slots())
        ok &= wr<U16>(cursor + kOffCursorSectors, static_cast<U16>(visible_slots()));
    if (!g_ring_logged || !ok) {
        g_ring_logged = true;
        LOG("ring (outfits=%d, n=%u, sectors %u)%s: bg %s", on, n, sectors, ok ? "" : " (change failed)",
            g_ring_bg ? describe(g_ring_bg).c_str() : "not found");
    }
}

// --- Centre tooltip -----------------------------------------------------------------------------
// Focusing a saved outfit reuses the centre tooltip (dark disc) for its name: the item-only parts are
// hidden (only those the game had showing, restored on the next focus / tab change / close). The worn
// outfit keeps (or gets) the game's "Equipped" tag above its name. While one of our submenu pages is open the
// highlighted entry is named there and explained in the item-count caption below the line (g_center_desc).
const char* const kTooltipEquipped = "cpp-item-equipped";
const char* const kTooltipCount = "cpp-item-info-item-count";
const char* const kTooltipItemParts[] = {kTooltipEquipped, "cpp-item-tooltip-stat-set",
                                         "cpp-item-tooltip-durabiltiy-gauge-wrap", "quickslot-buff-stat-wrap",
                                         kTooltipCount};
std::string g_center_desc; // the caption's text ("" : hidden, as for an outfit)
std::vector<U64> g_hidden_parts;
std::vector<U64> g_shown_parts; // parts the game had hidden that we show
std::string g_center_name;
int g_center_outfit = -1;
int g_center_frames = 0; // re-assert the name for a while (a deferred tooltip refresh may overwrite it)
// Restores the engine skipped (element mid-restyle), retried every frame on any tab; otherwise the part (e.g. the
// Equipped tag) stayed hidden in the Weapons / Armor wheels too.
std::vector<U64> g_unhide_retry, g_rehide_retry;

void retry_restore()
{
    auto undo = [](std::vector<U64>& v, bool hide) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [hide](U64 e) { return !is_element(e) || set_class(e, "!cpp-none", hide); }),
                v.end());
    };
    undo(g_unhide_retry, false);
    undo(g_rehide_retry, true);
}

void center_restore()
{
    g_unhide_retry.insert(g_unhide_retry.end(), g_hidden_parts.begin(), g_hidden_parts.end());
    g_rehide_retry.insert(g_rehide_retry.end(), g_shown_parts.begin(), g_shown_parts.end());
    g_hidden_parts.clear();
    g_shown_parts.clear();
    g_center_frames = 0;
    retry_restore();
    if (!g_unhide_retry.empty() || !g_rehide_retry.empty())
        LOG("centre restore deferred (mid-restyle): %zu to show, %zu to hide", g_unhide_retry.size(),
            g_rehide_retry.size());
}

// A part still waiting for its restore that gets hidden (shown) again is ours again instead.
bool take_retry(std::vector<U64>& v, U64 e)
{
    auto it = std::find(v.begin(), v.end(), e);
    if (it == v.end()) return false;
    v.erase(std::remove(v.begin(), v.end(), e), v.end());
    return true;
}

void set_tooltip_state(U64 tip, U64 state_rva)
{
    U32 state = r32(g_base + state_rva);
    if (r32(tip + 0xa8) != state) g_set_state(tip, &state, 0);
}

bool g_center_logged = false;

void center_show(U64 self)
{
    U64 tip = rp(self + kOffRootCenterTip);
    U64 te = tip ? rp(tip + 8) : 0;
    if (!te || !is_element(te)) return;
    U64 name = find_by_class(te, "quickslot-center-tooltip-name", 12);
    if (name) g_set_text(name, g_center_name.c_str(), 0);
    for (const char* part : kTooltipItemParts) {
        U64 e = find_by_class(te, part, 12);
        if (!e) continue;
        bool desc = part == kTooltipCount && !g_center_desc.empty();
        if (part == kTooltipCount) {
            if (desc) g_set_text(e, g_center_desc.c_str(), 0);
            set_class(e, "om-desc", desc);
        }
        // (a weapon set: Equipped while exactly its weapons are worn, #27)
        if ((part == kTooltipEquipped && g_center_outfit >= 0 && (g_center_outfit == g_worn_outfit || g_center_outfit == g_worn_set)) ||
            desc) {
            if (take_retry(g_rehide_retry, e)) {
                g_shown_parts.push_back(e);
            } else if (take_retry(g_unhide_retry, e)) { // the game had it showing anyway
                if (!set_class(e, "!cpp-none", false)) g_unhide_retry.push_back(e);
            } else if (has_class(e, "!cpp-none") && set_class(e, "!cpp-none", false)) {
                g_shown_parts.push_back(e);
            }
            static bool logged = false;
            if (!logged && !desc) logged = true, LOG("Equipped tag shown: %s", describe(e).c_str());
        } else if (take_retry(g_unhide_retry, e)) {
            g_hidden_parts.push_back(e);
        } else if (take_retry(g_rehide_retry, e)) { // the game had it hidden anyway
            if (!set_class(e, "!cpp-none", true)) g_rehide_retry.push_back(e);
        } else if (!has_class(e, "!cpp-none") && set_class(e, "!cpp-none", true)) {
            g_hidden_parts.push_back(e);
        }
    }
    set_tooltip_state(tip, kRvaStateShow);
    if (!g_center_logged) {
        g_center_logged = true;
        LOG("centre tooltip %s; name elem %llX, state %X", describe(te).c_str(), name, r32(tip + 0xa8));
    }
}

void center_hide(U64 self)
{
    center_restore();
    if (U64 tip = rp(self + kOffRootCenterTip)) set_tooltip_state(tip, kRvaStateHide);
}

} // namespace om
