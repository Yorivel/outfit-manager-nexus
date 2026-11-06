// Outfit Manager (step 1 of the slots: the Outfits tab layout). The gear menu (RootQuickSlot) builds
// one tab body per QuickSlotInfo record and pairs it with the tab-bar headers from quickslotitem.thtml.
// The "Outfit Manager" browser mod adds a 5th header (visible like Weapons/Armor); while the menu builds
// its bodies this plugin reports a 5th record (the 12-slot Armor layout with every slot's category set
// to "none", so no item binds), labels the header on open, and shows it again when the menu leaves
// wheel mode (the wheels hide every header). Slots show the saved outfits plus one empty "+" slot
// (chest silhouette + plus, CSS from the browser mod's quickslotitem.css.merge); the rest are hidden.
// The visible slots share the whole wheel: they are spaced 360/N apart (first outfit on top, "+" last)
// and, below 12, drawn over an N-segment ring with matching focus wedges; the cursor gets N sectors
// (one slot = the whole ring) and the stick cannot focus the hidden slots.
// A click (or Space) on the "+" slot saves the worn armor as a new outfit (up to 11; not twice); saved slots show the
// outfit's chest icon and focusing one shows its name in the centre tooltip. A click on a saved slot loads it: the
// pieces it does not wear come off, then the missing ones go on (Kliff: the game's own equip calls; companions: its UI
// requests), from storage / the wardrobe / other characters per the settings. Scrolling a slot cycles its mode (dots,
// a badge): a saved outfit's settings page (Overwrite / Rename / Delete) and conditions page, the "+" slot's settings
// page (Pull / Party / Messages), shown in the game's own submenu ring. Outfits and settings persist in
// OutfitManager.outfits next to the plugin, one list per character (and per save slot); every game address comes from
// the resolver (resolver.h), so a game update heals itself or switches the affected feature off.

#include "om.h"
#include "om_rest.h"
#include "addrs.h"
#include "ui.h"
#include "items.h"
#include "store.h"
#include "saves.h"
#include "looks.h"
#include "looks_view.h"

namespace om {

// Vtable slots: the known build's numbers; adopt_slots() takes the slot each function is in now (resolver).
int kSlotTextModalClick = 115;   // (modal, ?, clicked element, char): +0xd8 confirm, +0xe0 cancel
int kSlotCommonModalClick = 115; // same handler role in the CommonModal
constexpr U32 kParamsSize = 0x1428; // (0x1310 before the 2026-10-30 update: one more label buffer at the end)
constexpr U32 kParamsMessage = 0x118, kParamsMessageCap = 0x801, kParamsMessageLen = 0x92c;
constexpr int kNameMaxChars = 24;
constexpr U32 kParamsTitle = 0x0, kParamsTitleLen = 0x114, kParamsConfirm = 0xfc0, kParamsCancel = 0x10d8;
constexpr U32 kParamsLabelCap = 0x100; // inline buffers of 0x108 bytes
constexpr U32 kSlotNameHolder = 0x08; // -> {char* category, ...}
constexpr U32 kSlotHash = 0x1c;
constexpr U32 kSlotEquipIdx = 0x20;

// Category holder for the Outfits slots: the slot binder compares the name with its category table.
struct CategoryHolder {
    const char* name;
    U64 pad[3];
};
const CategoryHolder g_none_category{"none", {}};
std::atomic<int> g_ui_tab_added{0}; // gear-wheel UI files that got the Outfits header (patched in memory at load)
RecordGetFn g_qsi_get = nullptr; // original (trampoline once hooked)
OpenFn g_open_orig = nullptr;
TabFn g_tab_orig = nullptr;
OpenFn g_build_orig = nullptr;
OpenFn g_gear_orig = nullptr;
TabFn g_select_orig = nullptr;
TabFn g_close_orig = nullptr;
TabFn g_focus_orig = nullptr;
FloatFn g_frame_orig = nullptr;
VoidFn g_hide_script = nullptr;
ModalClickFn g_text_click_orig = nullptr;
ModalClickFn g_ask_click_orig = nullptr;
VoidFn g_restyle = nullptr;
std::atomic<U32> g_fake_key{0xFFFF}; // record index served from g_fake_rec (0xFFFF = none)
U64 g_outfits_root = 0; // the menu the Outfits tab was injected into (build_hook)

// --- Outfits tab --------------------------------------------------------------------------------
// The count the body builder iterates: (***root + 8) -> +0x460 -> +8 (root depends on a TLS flag,
// so both candidates are adjusted).
U64 qsi_table(U64 root_rva)
{
    U64 p = rp(g_base + root_rva);
    p = p ? rp(p) : 0;
    p = p ? rp(p + 8) : 0;
    return p ? rp(p + 0x460) : 0;
}

U64 __fastcall qsi_get_hook(U64 unused, U32 key)
{
    if (U16(key) == g_fake_key.load()) return reinterpret_cast<U64>(g_fake_rec);
    return g_qsi_get(unused, key);
}

// Equip slot ids of the template (Armor) record's slots, read before the fake slots are cleared.
void collect_armor_slots()
{
    g_armor_slot_count = 0;
    std::string ids;
    for (U32 i = 0; i < kOutfitSlots; ++i) {
        U16 s = *reinterpret_cast<U16*>(g_fake_slots + i * kSlotSize + kSlotEquipIdx);
        g_armor_slots[g_armor_slot_count++] = s; // index-aligned with the Armor tab's slot scripts
        ids += ' ' + std::to_string(s);
    }
    LOG("armor equip slots:%s", ids.c_str());
}

// Equip slot ids of the Weapons wheel (record 0, read like the Armor one): an outfit holds both wheels.
constexpr U16 kWeaponRecord = 0;

void collect_weapon_slots()
{
    U16 key = kWeaponRecord;
    U64 rec = g_qsi_get(0, key);
    U64 slots = rec ? rp(rec + kRecSlots) : 0;
    U32 n = rec ? r32(rec + kRecSlotCount) : 0;
    // (read on the UI thread and, at the loading screen, the game's: filled whole, then counted; the same values)
    U16 got[kMaxWeaponSlots];
    U32 count = 0;
    std::string ids;
    for (U32 i = 0; slots && i < n && i < kMaxWeaponSlots; ++i) {
        U16 s = 0xFFFF;
        if (!rd(slots + i * kSlotSize + kSlotEquipIdx, s)) break;
        got[count++] = s;
        ids += ' ' + std::to_string(s);
    }
    for (U32 i = 0; i < count; ++i) g_weapon_slots[i] = got[i];
    g_weapon_slot_count = count;
    static bool logged_none = false; // (not there yet: once)
    if (g_weapon_slot_count || !logged_none) LOG("weapon equip slots (record %u, %u slots):%s", key, n, ids.c_str());
    logged_none |= !g_weapon_slot_count;
}

// Copies the template record (Armor) into g_fake_rec / g_fake_slots; the slots get category "none"
// (the binder then attaches no item and the refresh leaves them alone).
bool prepare_fake_record()
{
    U16 key = kTemplateRecord;
    U64 rec = g_qsi_get(0, key);
    U64 slots = rec ? rp(rec + kRecSlots) : 0;
    if (!slots || r32(rec + kRecSlotCount) != kOutfitSlots) {
        LOG("template record %u unusable (rec %llX slots %u)", key, rec, rec ? r32(rec + kRecSlotCount) : 0);
        return false;
    }
    __try {
        std::memcpy(g_fake_rec, reinterpret_cast<void*>(rec), kRecordSize);
        std::memcpy(g_fake_slots, reinterpret_cast<void*>(slots), sizeof g_fake_slots);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("template record copy faulted");
        return false;
    }
    // The Armor and Weapons wheels' equip slots decide which worn items an outfit stores.
    collect_armor_slots();
    collect_weapon_slots();
    U64 own = reinterpret_cast<U64>(g_fake_slots);
    std::memcpy(g_fake_rec + kRecSlots, &own, sizeof own);
    U64 none = reinterpret_cast<U64>(&g_none_category);
    for (U32 i = 0; i < kOutfitSlots; ++i) {
        std::uint8_t* s = g_fake_slots + i * kSlotSize;
        std::memcpy(s + kSlotNameHolder, &none, sizeof none);
        *reinterpret_cast<U32*>(s + kSlotHash) = 0;
        *reinterpret_cast<U16*>(s + kSlotEquipIdx) = 0xFFFF;
    }
    return true;
}
// --- Slot modes: scrolling cycles what an Outfits slot does --------------------------------------
// Like vanilla's item cycling (the dots under a slot): a slot cycles its action (a saved outfit: click loads; the "+"
// slot: click saves) / settings (click opens the settings view). The game's scroll handlers (slot vtable 181 / 182) are hooked; on our
// slots the mode changes instead. Modes stay per slot while the wheel is open and all reset when it closes.
enum SlotMode : std::uint8_t { kModeMain, kModeSettings, kModeConditions, kModeAppearance }; // (eye: saved outfits)
std::uint8_t g_slot_mode[kOutfitSlots] = {};
using ScrollFn = U64(__fastcall*)(U64);
ScrollFn g_scroll_prev_orig = nullptr, g_scroll_next_orig = nullptr;
using SelectorSelectFn = void(__fastcall*)(U64, U32, U64);
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

bool g_amber = false; // the anchor (the last loaded / exactly worn outfit) has been changed by hand: its highlight is amber
// Equipment slots the game refuses to empty ("There are no Items available to be equipped after removal": the main
// hand, the shield, the dagger, the lantern - seen; more are learned from that refusal and kept in the file): an outfit
// that leaves one empty still counts as worn whatever is in it.
std::set<U16> g_keep_slots = {0, 1, 12, 15};
constexpr int kErrNoItemAfterRemoval = 0x69BEDE43;
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

std::atomic<U32> g_peek_key{0};           // the Outfit Arc's key (a virtual-key code; 0 = Off)

// The current character's hotkeys for the input filters (another thread): vk | mods << 8 per outfit, 0 = none.
std::atomic<U32> g_hotkeys[16] = {};
std::atomic<bool> g_peek_pad{false};       // the Outfit Arc on controllers: hold L1 / LB, press the d-pad's down

// The settings' descriptions (the settings view's centre caption), by group and entry: {title, description}.
enum Page { kPageNone, kPageSettings, kPageOutfit, kPageConditions, kPageSettings2 };
U32 g_depth_outfit = 0; // the saved outfit the conditions' texts are for
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
            return {"", lang::t(o.load_weapons ? "desc.weapons.load" : "desc.weapons.leave")};
        }
        if (index == 1) return {"", stash_description(o)};
    }
    return {"", ""};
}

// Set while the plugin itself is calling into the game (pump, prompt answers): callbacks the game fires
// synchronously meanwhile (focus, frame getter, cursor) must not start another round of work.
int g_busy = 0;
struct Busy {
    Busy() { ++g_busy; }
    ~Busy() { --g_busy; }
};

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

std::string numbered(const char* fmt, U32 a, U32 b = 0)
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

// The input in use (the last one touched: a pad, or the mouse / keyboard), set by key_thread; a pad's family picks its
// confirm icon. The game swaps its own prompts the same way.
std::atomic<bool> g_pad_in_use{false}, g_pad_is_ps{false}, g_prompt_changed{false};
const char* prompt_class()
{
    return !g_pad_in_use ? "om-prompt-c" : g_pad_is_ps ? "om-cross-c" : "om-abtn-c";
}

const char* const kBadges[] = {"om-plus-big", "om-cog-big", "om-plus-c", "om-cog-c", "om-flag-c", "om-cold-c", "om-heat-c",
                               "om-cold2-c", "om-heat2-c", "om-eye-c", "om-look-shown-c", "om-look-hidden-c",
                               "om-look-combat-c", "om-look-peace-c", "om-return-c", "om-prompt-c", "om-cross-c",
                               "om-abtn-c"};
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
std::string where(U64 a);

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
bool show_value(U64 root, const std::string& text, int hotkey = 0, bool caps = false);

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
U32 mode_count(U32 i);
U32 mode_lit(U32 i);
void show_mode_dots(U64 script, U32 count, U32 mode);
void apply_eye_slot(U64 self, U64 script, U64 root, U32 i, U32 visible, const std::vector<size_t>& pieces)
{
    U32 k = i < visible ? slot_outfit(i) : kOutfitSlots;
    bool piece = k < pieces.size(), page = k == pieces.size(), back = k == pieces.size() + 1;
    OutfitItem tmp{};
    const OutfitItem* it = piece ? eye_item(pieces[k], tmp) : nullptr;
    std::string pic = it ? piece_icon(*it) : std::string();
    show_slot_image(self, root, i, pic);
    if (i < visible) show_mode_dots(script, mode_count(i), mode_lit(i));
    U64 badge = find_by_class(root, "quickslot-occupancy-icon-chain"), low = find_by_class(root, "cpp-icon-disable-item");
    const char* want = it ? look_badge(it->look) : back ? prompt_class() : nullptr; // (Back: the click prompt, as actions)
    g_badge_elem[i] = badge, g_badge_class[i] = want;
    bool ok = true;
    if (badge) {
        ok &= set_class(badge, "self-center", true);
        for (const char* c : kBadges) ok &= set_class(badge, c, want && !std::strcmp(want, c));
        ok &= set_class(badge, "!cpp-none", !want);
    }
    U64 mark = find_parent_of_class(root, "charged-count");
    if (mark) {
        ok &= back ? set_class(mark, "self-center", true) && set_class(mark, "om-label-wrap", true) : clear_label(mark);
        ok &= set_class(mark, "om-eye-c", false) && set_class(mark, "om-eye2-c", false);
        ok &= set_class(mark, "!cpp-none", !back);
        if (U64 text = back ? find_by_class(mark, "charged-count") : 0) {
            if (g_set_text) g_set_text(text, "Back", 0); // (as the settings' Back)
            ok &= set_class(text, "om-label", true) && set_class(text, "om-label-value", false);
        }
    }
    g_mark_elem[i] = mark, g_mark_shown[i] = mark && back;
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
        bool sword = page && g_eye_pick == 1, necklace = page && g_eye_pick == 2;
        bool holder = piece && pic.empty() && it->slot != kBackShieldSlot; // (an empty slot: the game's placeholder)
        bool armor = (page && g_eye_pick == 0) || (piece && pic.empty() && !holder);
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
void apply_cfg_slot(U64 self, U64 script, U64 root, U32 i, U32 visible);
std::string hotkey_name(U32 vk, std::uint8_t mods);
bool outfit_missing(size_t o);
const OutfitItem* outfit_piece(const Outfit& o, U16 slot);
bool refresh_missing(U64 self);
void update_plus_icon(U64 self);

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
        g_applied_visible = visible;
        // (during a character switch the worn gear is already the new character's while the list is still the old
        // one's: comparing them would read as "a new set" and drop the old character's looks, 2026-11-06)
        U16 now_char = current_character();
        bool same_char = now_char == kNoCharacter || g_character == kNoCharacter || now_char == g_character;
        if (int w = same_char ? find_worn_outfit(self) : g_worn_outfit; w != g_worn_outfit) {
            LOG("worn outfit: %d -> %d", g_worn_outfit, w);
            g_worn_outfit = w;
            g_worn_changed = true;
            if (w >= 0) mark_worn(w); // (put together by hand: its looks apply too; none worn keeps the last loaded)
        }
        if (same_char) update_plus_icon(self);
        refresh_missing(self);
        U64 body = rp(rp(rp(self + kOffRootBodies) + tab * 8ull) + 8);
        bool list_ok = body && is_element(body) && swap_class(body, g_list_class, numbered("om-list-%u", visible));
        if (!g_slots_logged || !list_ok)
            LOG("  outfit list%s: %s", list_ok ? "" : " (class change skipped)", body ? describe(body).c_str() : "null");
    }
    std::vector<size_t> pieces = eye_pieces();
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
        std::uint8_t mode = i < visible ? g_slot_mode[i] : std::uint8_t(kModeMain);
        // (the "+" slot: the worn chest when what is worn is not a saved outfit - what saving would keep)
        std::string pic = saved ? g_outfits[slot_outfit(i)].chest_icon : empty ? g_plus_icon : std::string();
        if (saved && pic.empty())
            if (const OutfitItem* c = outfit_piece(g_outfits[slot_outfit(i)], kChestSlot)) pic = piece_icon(*c);
        bool chest = show_slot_image(self, root, i, pic);
        if (i < visible) show_mode_dots(script, mode_count(i), mode);
        U64 badge = find_by_class(root, "quickslot-occupancy-icon-chain"), low = find_by_class(root, "cpp-icon-disable-item");
        const char* want = mode != kModeMain && empty  ? (kPlusSlotCentred ? "om-cog-big" : "om-cog-c") // (settings 1 / 2)
                           : mode == kModeSettings               ? "om-cog-c"
                           : mode == kModeConditions             ? "om-flag-c"
                           : mode == kModeAppearance             ? "om-eye-c"
                           : empty && g_outfit_count < kMaxOutfits ? (kPlusSlotCentred ? "om-plus-big" : "om-plus-c")
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
            if (saved && mode == kModeMain)
                for (U16 s = 0; s < 32; ++s) looks |= slot_look(g_outfits[slot_outfit(i)], s) != kLookDefault;
            bool climate = want && (!std::strncmp(want, "om-cold", 7) || !std::strncmp(want, "om-heat", 7));
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
        bool worn = saved && int(slot_outfit(i)) == g_worn_outfit;
        // Amber: the last loaded outfit while what is worn has been changed by hand since (not saved); green: the "+" slot
        // while an unsaved set is worn (its chest is previewed there).
        bool drifted = !worn && saved && g_worn_outfit < 0 && g_outfits[slot_outfit(i)].worn && g_amber;
        bool ready = empty && !g_plus_icon.empty(); // (green, the game's "positive" colour: an unsaved set to save)
        g_low_elem[i] = low, g_low_shown[i] = worn || drifted || ready;
        if (low) {
            ok &= swap_class(low, g_worn_class[i], worn      ? numbered("om-w%u-%u", visible, i)
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

U64 g_outfits_root_of_slots() { return g_fake_key.load() != 0xFFFF ? g_outfits_root : 0; }

bool outfits_current(U64 self)
{
    U32 tab = g_fake_key.load();
    if (tab == 0xFFFF || !g_layout_bad.empty() || self != g_outfits_root) return false;
    U64 tb = rp(self + kOffRootTabBar);
    return tb && r32(tb + kOffTabCurrent) == tab;
}
U32 cfg_dots(U32 i, U32* lit);

// Saved outfit: outfit / settings / conditions / appearance; the "+" slot: + / settings 1 / settings 2 (mode 2 there =
// page 2). The Appearance view: the page slot's three pages, a piece's looks, Back none.

U32 mode_count(U32 i)
{
    if (g_eye >= 0) {
        size_t k = slot_outfit(i), n = eye_pieces().size();
        return k < n ? kLookCount : k == n ? kEyeSteps : 0;
    }
    if (g_cfg >= 0) return cfg_dots(i, nullptr);
    return 2; // (the slot's action, its settings)
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
// The game's slot refresh (after every equipment change) re-selects every slot's dots with its own count (0 for ours:
// no items) and -1: the select is hooked, and for our slots' selectors it gets our count and mode instead, so the dots
// never go blank. Our own calls pass through it as well. Our slots are looked up live (the Outfits menu's slot list).
using SelectorSelectHookFn = void(__fastcall*)(U64, U32, U64);
SelectorSelectHookFn g_selector_select_orig = nullptr;

// The gear slots' refresh (EquipSlotVt slot 164, 0xef5900: after every equipment change) rebuilds a slot's item list
// from the inventory and shows its current item (0xef9b80); ours hold none, so it blanked them and the plugin drew them
// again (they flickered). Everything our slots show is ours: their refresh is skipped; every other slot's runs.
using SlotRefreshFn = void(__fastcall*)(U64);
SlotRefreshFn g_slot_refresh_orig = nullptr;
U32 outfit_slot_index(U64 self, U64 script);

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
thread_local bool t_own_pad = false;      // our own pad reading (XInput, the PlayStation pad's reports): never filtered

// The game's PlayStation pad handles (opened by path: \\?\hid#vid_054c&pid_....), with their report length.
struct PadHandle {
    std::atomic<HANDLE> h{nullptr};
    DWORD len = 0;
    bool dualsense = false;
};
PadHandle g_pad_handles[8];

void note_pad_handle(LPCWSTR path, HANDLE h)
{
    if (!path || h == INVALID_HANDLE_VALUE || t_own_pad) return;
    std::wstring l = path;
    for (wchar_t& c : l) c = static_cast<wchar_t>(std::towlower(c));
    if (l.find(L"vid_054c") == std::wstring::npos) return;
    PHIDP_PREPARSED_DATA pp = nullptr;
    HIDP_CAPS caps{};
    DWORD len = 0;
    if (HidD_GetPreparsedData(h, &pp)) {
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) len = caps.InputReportByteLength;
        HidD_FreePreparsedData(pp);
    }
    if (!len) return;
    bool ds = l.find(L"pid_0ce6") != std::wstring::npos || l.find(L"pid_0df2") != std::wstring::npos;
    for (PadHandle& p : g_pad_handles) {
        HANDLE none = nullptr;
        if (p.h.load() == h) return;
        if (p.h.compare_exchange_strong(none, h)) {
            p.len = len, p.dualsense = ds;
            LOG("Outfit Arc: the game's PlayStation pad handle %p (reports of %lu bytes)", static_cast<void*>(h), len);
            return;
        }
    }
}

const PadHandle* pad_handle(HANDLE h)
{
    if (!h) return nullptr;
    for (const PadHandle& p : g_pad_handles)
        if (p.h.load() == h) return &p;
    return nullptr;
}

U32 crc32_bytes(U32 crc, const unsigned char* d, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// A report with the d-pad's up / down taken out (a diagonal keeps its left / right). Bluetooth reports carry a CRC
// (seeded with 0xA1): written again.
void strip_pad_report(unsigned char* r, DWORD n, bool dualsense)
{
    int face = -1;
    if (dualsense) {
        if (r[0] == 0x01 && n >= 64) face = 8;
        else if (r[0] == 0x31 && n >= 11) face = 9;
        else if (r[0] == 0x01 && n >= 7) face = 5;
    } else {
        if (r[0] == 0x01 && n >= 7) face = 5;
        else if (r[0] == 0x11 && n >= 9) face = 7;
    }
    if (face < 0 || DWORD(face) >= n) return;
    static const unsigned char kNoUpDown[9] = {8, 2, 2, 2, 8, 6, 6, 6, 8};
    unsigned char d = r[face] & 0x0F;
    if (d > 8 || kNoUpDown[d] == d) return;
    r[face] = static_cast<unsigned char>((r[face] & 0xF0) | kNoUpDown[d]);
    if ((r[0] == 0x31 || r[0] == 0x11) && n >= 78) {
        unsigned char seed = 0xA1;
        U32 c = crc32_bytes(crc32_bytes(0, &seed, 1) , r, 74);
        // (crc32_bytes chains: the seed byte, then the report's first 74 bytes)
        std::memcpy(r + 74, &c, 4);
    }
}

// Overlapped reads of a pad handle: the buffer is filled later; filtered when the game asks for the result.
struct PendingRead {
    std::atomic<OVERLAPPED*> ov{nullptr};
    unsigned char* buf = nullptr;
    DWORD len = 0;
    bool dualsense = false;
};
PendingRead g_pending_reads[16];

using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using GetOverlappedResultFn = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
using GetOverlappedResultExFn = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
ReadFileFn g_read_file_orig = nullptr;
GetOverlappedResultFn g_ov_result_orig = nullptr;
GetOverlappedResultExFn g_ov_result_ex_orig = nullptr;
bool peek_pad_owned();

BOOL WINAPI read_file_hook(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    BOOL r = g_read_file_orig(h, buf, n, got, ov);
    if (t_own_pad || !buf) return r;
    const PadHandle* p = pad_handle(h);
    if (!p || n < p->len) return r;
    DWORD err = GetLastError();
    if (r && peek_pad_owned()) {
        DWORD k = got ? *got : n;
        __try {
            strip_pad_report(static_cast<unsigned char*>(buf), k ? k : n, p->dualsense);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    } else if (!r && err == ERROR_IO_PENDING && ov) {
        for (PendingRead& q : g_pending_reads) {
            OVERLAPPED* none = nullptr;
            if (q.ov.load() == ov || q.ov.compare_exchange_strong(none, ov)) {
                q.buf = static_cast<unsigned char*>(buf), q.len = n, q.dualsense = p->dualsense;
                q.ov = ov;
                break;
            }
        }
    }
    SetLastError(err);
    return r;
}

void finish_pending_read(LPOVERLAPPED ov, BOOL ok, LPDWORD got)
{
    if (!ov) return;
    for (PendingRead& q : g_pending_reads) {
        if (q.ov.load() != ov) continue;
        if (ok && peek_pad_owned()) {
            __try {
                strip_pad_report(q.buf, got && *got ? *got : q.len, q.dualsense);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (ok) q.ov = nullptr; // (the slot is free; the game's next read takes it again)
        return;
    }
}

BOOL WINAPI ov_result_hook(HANDLE h, LPOVERLAPPED ov, LPDWORD got, BOOL wait)
{
    BOOL r = g_ov_result_orig(h, ov, got, wait);
    if (!t_own_pad) {
        DWORD err = GetLastError();
        finish_pending_read(ov, r, got);
        SetLastError(err);
    }
    return r;
}

BOOL WINAPI ov_result_ex_hook(HANDLE h, LPOVERLAPPED ov, LPDWORD got, DWORD ms, BOOL alertable)
{
    BOOL r = g_ov_result_ex_orig(h, ov, got, ms, alertable);
    if (!t_own_pad) {
        DWORD err = GetLastError();
        finish_pending_read(ov, r, got);
        SetLastError(err);
    }
    return r;
}

// Xbox pads: the game's xinput1_4 (by name or by ordinal 100, the one with the guide button) gets the d-pad's up / down
// taken out of its button word.
using XGetStateFn = DWORD(WINAPI*)(DWORD, void*);
XGetStateFn g_xget_orig = nullptr, g_xget100_orig = nullptr;

DWORD xget_filter(DWORD r, void* state)
{
    if (r == ERROR_SUCCESS && state && !t_own_pad && peek_pad_owned()) {
        WORD& b = *reinterpret_cast<WORD*>(static_cast<unsigned char*>(state) + 4); // (XINPUT_STATE: packet, then buttons)
        if (b & 0x0003) b &= static_cast<WORD>(~0x0003);
    }
    return r;
}
DWORD WINAPI xget_hook(DWORD i, void* s) { return xget_filter(g_xget_orig(i, s), s); }
DWORD WINAPI xget100_hook(DWORD i, void* s) { return xget_filter(g_xget100_orig(i, s), s); }

void install_pad_filters()
{
    struct { const wchar_t* dll; const char* name; void* fn; void** orig; } hooks[] = {
        {L"kernelbase", "ReadFile", reinterpret_cast<void*>(&read_file_hook), reinterpret_cast<void**>(&g_read_file_orig)},
        {L"kernelbase", "GetOverlappedResult", reinterpret_cast<void*>(&ov_result_hook), reinterpret_cast<void**>(&g_ov_result_orig)},
        {L"kernelbase", "GetOverlappedResultEx", reinterpret_cast<void*>(&ov_result_ex_hook), reinterpret_cast<void**>(&g_ov_result_ex_orig)},
    };
    int ok = 0;
    for (auto& h : hooks) {
        void* target = nullptr;
        if (MH_CreateHookApiEx(h.dll, h.name, h.fn, h.orig, &target) == MH_OK && MH_EnableHook(target) == MH_OK) ++ok;
        else LOG("Outfit Arc: %s filter failed", h.name);
    }
    if (HMODULE x = LoadLibraryW(L"xinput1_4.dll")) {
        if (void* t = reinterpret_cast<void*>(GetProcAddress(x, "XInputGetState")))
            if (MH_CreateHook(t, reinterpret_cast<void*>(&xget_hook), reinterpret_cast<void**>(&g_xget_orig)) == MH_OK &&
                MH_EnableHook(t) == MH_OK) ++ok;
        if (void* t = reinterpret_cast<void*>(GetProcAddress(x, MAKEINTRESOURCEA(100))))
            if (MH_CreateHook(t, reinterpret_cast<void*>(&xget100_hook), reinterpret_cast<void**>(&g_xget100_orig)) == MH_OK &&
                MH_EnableHook(t) == MH_OK) ++ok;
    }
    LOG("Outfit Arc: pad filters on (%d of 5)", ok);
}

// The worn armor, in the template record's slot order. The items come from the game's equipment list
// (live); the vanilla Armor wheel's slot scripts (tab kTemplateRecord) only refresh when the menu opens, so
// they are the fallback: their item is in data = script+0x148, at data+0x860 when that one is valid, else
// data+0x5d8 (as the game's own slot handlers pick it). Item = {u64 uid, u16 key (0xFFFF = none), .., i64
// count @+0x10}. Count or < 0 on failure.
bool item_valid(U64 item, U64 empty)
{
    return item && item != empty && *reinterpret_cast<U16*>(item + 8) != 0xFFFF &&
           *reinterpret_cast<long long*>(item + 0x10) > 0;
}

// Equipment list = *(inv+0x90): {.., entry[] @+8, u32 count @+0x10}; entry = 0xd0 bytes, the item (200 bytes)
// then its u16 equip slot @+0xc8 (0x20c5600 looks worn pieces up like this).

U64 equipped_item(U64 list, U16 slot)
{
    U64 e = *reinterpret_cast<U64*>(list + 8);
    U32 n = *reinterpret_cast<U32*>(list + 0x10);
    for (U32 i = 0; e && i < n && i < 64; ++i, e += 0xd0)
        if (*reinterpret_cast<U16*>(e + kOffEntrySlot) == slot) return e;
    return 0;
}

U64 equipment_list()
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 c = actor ? rp(actor + kOffActorComp) : 0;
    U64 inv = c ? rp(c + kOffCompInventory) : 0;
    U64 list = inv ? rp(inv + kOffInvEquipList) : 0;
    if (ctx[0]) release_player(ctx);
    static bool logged = false;
    if (list && !logged) {
        logged = true;
        U64 e = rp(list + 8);
        U32 n = r32(list + 0x10);
        LOG("equipment list %llX: %u entries", list, n);
        for (U32 i = 0; e && i < n && i < 64; ++i) {
            U16 key = 0xFFFF, s0 = 0, s1 = 0;
            rd(e + i * 0xd0ull + 8, key), rd(e + i * 0xd0ull + 0xc8, s0), rd(e + i * 0xd0ull + 0xca, s1);
            LOG("  entry %u: key %u uid %llX, +C8 %u, +CA %u", i, key, rp(e + i * 0xd0ull), s0, s1);
        }
    }
    return list;
}

// The game's placeholders: what it puts in a slot that can't be empty (bare fists, "Ordinary Gloves", in equipment-13:
// taking that slot off says "There are no Items available to be equipped after removal"). They live in a hidden bag
// (13) while not worn: one per character (keys 6280 / 6281 / 6282). An outfit treats them as an empty slot: never
// saved, loaded or reported missing. The one worn is not in the bag, so a key seen there once stays known (kept in the
// outfit file, for a launch with the fists on), and an item named like a known one counts too.
constexpr U16 kBagHidden = 13;

std::vector<U16> g_placeholder_keys;   // seen in the hidden bag (only ever added to)
std::vector<std::string> g_placeholder_names;
bool g_placeholders_new = false; // a key was added: refresh_missing saves the file (not here: callers may hold its lock)
U64 g_placeholder_read = 0; // tick (re-read every few seconds: cheap, one bag)
U64 call_inventory(U64 actor);

void read_placeholder_keys()
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 mgr = actor ? call_inventory(actor) : 0;
    if (ctx[0]) release_player(ctx);
    U64 bags = mgr ? rp(mgr + kOffMgrBags) : 0;
    U32 n = mgr ? r32(mgr + 0x20) : 0;
    if (!bags) return; // (keeps the last answer)
    std::vector<U16> keys = g_placeholder_keys;
    for (U32 i = 0; i < n && i < 64; ++i) {
        U64 bag = rp(bags + i * 8ull);
        U16 id = 0xFFFF;
        short cap = 0;
        if (!bag || !rd(bag + 0x10, id) || id != kBagHidden || !rd(bag + kOffBagCount, cap)) continue;
        U64 items = rp(bag);
        for (short p = 0; items && p < cap && p < 4096; ++p) {
            U16 k = 0xFFFF;
            long long c = 0;
            if (rd(items + p * 200ull + 8, k) && rd(items + p * 200ull + 0x10, c) && k != 0xFFFF && c > 0 &&
                std::find(keys.begin(), keys.end(), k) == keys.end())
                keys.push_back(k);
        }
    }
    bool added = keys != g_placeholder_keys;
    if (added) {
        std::lock_guard<std::mutex> lock(g_keys_mu);
        g_placeholder_keys = std::move(keys);
    }
    for (U16 k : g_placeholder_keys) {
        std::string name = item_name(k);
        if (!name.empty() && std::find(g_placeholder_names.begin(), g_placeholder_names.end(), name) == g_placeholder_names.end())
            g_placeholder_names.push_back(name);
    }
    if (added) {
        std::string list;
        for (U16 k : g_placeholder_keys) list += " " + std::to_string(k) + " \"" + item_name(k) + "\"";
        LOG("placeholders (hidden bag %u, and seen before):%s", kBagHidden, list.c_str());
        g_placeholders_new = true; // (remembered for the next launch)
    }
}

bool is_placeholder(U16 key)
{
    U64 now = GetTickCount64();
    if (!g_placeholder_read || now - g_placeholder_read > 5000) g_placeholder_read = now, read_placeholder_keys();
    if (key == 0xFFFF) return false;
    if (std::find(g_placeholder_keys.begin(), g_placeholder_keys.end(), key) != g_placeholder_keys.end()) return true;
    if (g_placeholder_names.empty()) return false;
    std::string n = item_name(key); // (another character's fists, never seen in the bag)
    return !n.empty() && std::find(g_placeholder_names.begin(), g_placeholder_names.end(), n) != g_placeholder_names.end();
}

int read_worn_from(U64 self, WornRaw* out, int max, U64 list, int& found)
{
    int n = -2;
    __try {
        // The live equipment list needs only the slot ids; the Armor wheel's slot scripts (the menu's) only for the
        // fallback (so a check from the HUD, with the wheel closed, reads the list).
        U64 sv = list ? 0 : *reinterpret_cast<U64*>(self + kOffRootSlotVecs);
        if (!list && (!sv || *reinterpret_cast<U32*>(self + kOffRootSlotVecCount) <= kTemplateRecord)) return -1;
        U64 arr = sv ? *reinterpret_cast<U64*>(sv + kTemplateRecord * 0x10ull) : 0;
        U32 cnt = sv ? *reinterpret_cast<U32*>(sv + kTemplateRecord * 0x10ull + 8) : g_armor_slot_count;
        U64 empty = *reinterpret_cast<U64*>(g_base + kRvaEmptyItem);
        n = 0;
        for (U32 i = 0; (arr || list) && i < cnt && i < g_armor_slot_count && n < max; ++i) {
            WornRaw& w = out[n++];
            w = {g_armor_slots[i], 0xFFFF, ~0ull, nullptr, 0, 0, {0xFFFF, 0xFFFF}};
            U64 script = arr ? *reinterpret_cast<U64*>(arr + i * 8ull) : 0;
            U64 data = script ? *reinterpret_cast<U64*>(script + kOffScriptData) : 0;
            if (data) {
                w.qslot = *reinterpret_cast<U16*>(script + kOffScriptEquipIdx);
                w.type = *reinterpret_cast<std::uint8_t*>(data + kOffDataType);
            }
            U64 item = 0;
            if (list) item = equipped_item(list, w.slot);
            else if (data) item = item_valid(data + kOffDataItem, empty) ? data + kOffDataItem : data + kOffDataItemAlt;
            if (!item_valid(item, empty) || !*reinterpret_cast<U64*>(item) || is_placeholder(*reinterpret_cast<U16*>(item + 8)))
                continue; // (uid 0: the game's default; a placeholder: bare fists)
            w.key = *reinterpret_cast<U16*>(item + 8);
            w.uid = *reinterpret_cast<U64*>(item);
            w.icon = g_icon_name(item);
            w.at[0] = *reinterpret_cast<U16*>(item + kOffEntrySlot);
            w.at[1] = *reinterpret_cast<U16*>(item + 0xca);
            ++found;
        }
        // The Weapons wheel's slots (live list only: the slot-script fallback covers the Armor wheel).
        for (U32 i = 0; list && i < g_weapon_slot_count && n < max; ++i) {
            if (g_weapon_slots[i] == 0xFFFF) continue;
            WornRaw& w = out[n++];
            w = {g_weapon_slots[i], 0xFFFF, ~0ull, nullptr, 0, 0, {0xFFFF, 0xFFFF}};
            U64 item = equipped_item(list, w.slot);
            if (!item_valid(item, empty) || !*reinterpret_cast<U64*>(item) || is_placeholder(*reinterpret_cast<U16*>(item + 8)))
                continue;
            w.key = *reinterpret_cast<U16*>(item + 8);
            w.uid = *reinterpret_cast<U64*>(item);
            w.icon = g_icon_name(item);
            w.at[0] = *reinterpret_cast<U16*>(item + kOffEntrySlot);
            w.at[1] = *reinterpret_cast<U16*>(item + 0xca);
            ++found;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = -3;
    }
    return n;
}

int read_worn(U64 self, WornRaw* out, int max)
{
    U64 list = equipment_list();
    int found = 0;
    int n = list ? read_worn_from(self, out, max, list, found) : -4;
    // (No armor piece found is a valid answer: unequipped pieces stay in the list with slot 0xFFFF.)
    if (n < 0) {
        static bool logged = false;
        if (!logged) logged = true, LOG("equipment list %llX unusable (%d); reading the Armor wheel", list, n);
        n = read_worn_from(self, out, max, 0, found);
    }
    return n;
}

U64 back_shield_item()
{
    U64 list = equipment_list();
    U64 e = list ? equipped_item(list, kBackShieldSlot) : 0;
    U16 key = 0xFFFF;
    return e && rd(e + 8, key) && key != 0xFFFF ? e : 0;
}

int anchor_outfit()
{
    for (size_t i = 0; i < g_outfits.size(); ++i)
        if (g_outfits[i].worn) return static_cast<int>(i);
    return -1;
}

// What is worn has left the anchor: under half of its armour pieces (head, chest, hands, feet, cloak) are still worn.
// Weapons and accessories don't count; nothing else is compared (outfits sharing pieces change nothing).
bool left_anchor(const Outfit& o, const WornRaw* w, int n)
{
    int total = 0, kept = 0;
    for (const OutfitItem& it : o.items) {
        if (is_weapon_slot(it.slot) || is_accessory_slot(it.slot) || it.slot == kBackShieldSlot) continue;
        ++total;
        for (int k = 0; k < n; ++k)
            if (w[k].slot == it.slot && w[k].uid == it.uid && w[k].key == it.key) ++kept;
    }
    return total > 0 && kept * 2 < total;
}

// One state at a time: an outfit worn exactly / the anchor changed by hand (amber) / a new set (no anchor: only the "+"
// slot's green, with the worn chest: what saving would keep). A new set stays one until an outfit is loaded or worn exactly.
void update_plus_icon(U64 self)
{
    g_plus_icon.clear();
    g_amber = false;
    if (g_worn_outfit >= 0) return;
    if (U16 ch = current_character(); ch != kNoCharacter && g_character != kNoCharacter && ch != g_character) return; // (switching)
    WornRaw w[kWornMax];
    int n = read_worn(self, w, kWornMax);
    if (int a = anchor_outfit(); a >= 0) {
        if (n <= 0 || look_changing() || !left_anchor(g_outfits[size_t(a)], w, n)) {
            g_amber = true;
            return;
        }
        LOG("\"%s\": too much changed, what is worn is a new set", g_outfits[size_t(a)].name.c_str());
        mark_worn(-1);
    }
    for (int k = 0; k < n; ++k)
        if (w[k].slot == kChestSlot && w[k].key != 0xFFFF && w[k].icon) g_plus_icon = cstr(reinterpret_cast<U64>(w[k].icon));
}

// Every slot's look of `from` onto `to` (what `to` holds there or not).
void carry_looks(const Outfit& from, Outfit& to)
{
    to.back_look = from.back_look;
    to.empty_looks.clear();
    for (OutfitItem& it : to.items) it.look = slot_look(from, it.slot);
    auto carry = [&](U16 slot, std::uint8_t look) {
        if (look != kLookDefault && !outfit_has_slot(to, slot)) to.empty_looks[slot] = look;
    };
    for (const OutfitItem& it : from.items) carry(it.slot, it.look);
    for (const auto& [slot, look] : from.empty_looks) carry(slot, look);
}

// The worn armor as an outfit (unnamed); false if the equipment could not be read.
bool snapshot(U64 self, Outfit& o)
{
    WornRaw w[kWornMax];
    int n = read_worn(self, w, kWornMax);
    if (n <= 0) {
        LOG("worn gear unreadable (%d)", n);
        return false;
    }
    o = {};
    for (int i = 0; i < n; ++i) {
        std::string icon = cstr(reinterpret_cast<U64>(w[i].icon));
        bool weapon = is_weapon_slot(w[i].slot);
        o.has_weapons |= weapon; // (read: its empty slots count too)
        LOG("  %s slot %d (equipment-%u, quickslot %u, type %u): key %u uid %llX at %u/%u icon %s",
            weapon ? "weapon" : "armor", i, w[i].slot, w[i].qslot, w[i].type, w[i].key, w[i].uid, w[i].at[0],
            w[i].at[1], icon.c_str());
        if (w[i].key == 0xFFFF) continue;
        o.items.push_back({w[i].slot, w[i].key, w[i].uid, 0, pid_of(w[i].key)});
        if (w[i].slot == kChestSlot) o.chest_icon = icon;
    }
    std::sort(o.items.begin(), o.items.end(), [](const OutfitItem& a, const OutfitItem& b) { return a.slot < b.slot; });
    o.has_back = back_shield_item() ? 1 : 2;
    return true;
}

// Are slot's (a hand) and the other hand's worn weapons the outfit's, crossed?
bool hands_crossed(const Outfit& o, const WornRaw* w, int n, U16 slot)
{
    if (slot != kHandR && slot != kHandL) return false;
    U16 other = slot == kHandR ? kHandL : kHandR;
    const WornRaw *ws = nullptr, *wo = nullptr;
    for (int k = 0; k < n; ++k) {
        if (w[k].slot == slot) ws = &w[k];
        if (w[k].slot == other) wo = &w[k];
    }
    const OutfitItem *ts = outfit_piece(o, slot), *to = outfit_piece(o, other);
    return ws && wo && ts && to && ws->key != 0xFFFF && wo->key != 0xFFFF && ts->uid != to->uid &&
           ws->uid == to->uid && ws->key == to->key && wo->uid == ts->uid && wo->key == ts->key;
}

// The same pieces in the same slots (by uid: a duplicate of a piece is another piece). The weapon slots count only
// when both load weapons (an outfit set to Leave, or saved before weapons, matches on its armour). The two hands'
// weapons also match crossed (hands_crossed).
bool same_pieces(const Outfit& a, const Outfit& b)
{
    bool weapons = uses_weapons(a) && uses_weapons(b);
    auto pieces = [&](const Outfit& o, bool swap) {
        std::vector<std::pair<U16, U64>> v;
        for (const OutfitItem& it : o.items) {
            if (!weapons && is_weapon_slot(it.slot)) continue;
            U16 sl = swap && it.slot == kHandR ? kHandL : swap && it.slot == kHandL ? kHandR : it.slot;
            v.push_back({sl, it.uid});
        }
        std::sort(v.begin(), v.end());
        return v;
    };
    return pieces(a, false) == pieces(b, false) || (weapons && pieces(a, false) == pieces(b, true));
}

// Saving (+, Overwrite): is the worn gear (b) already saved as a? Also agreeing on the weapons: an outfit that leaves
// them (Weapons: Leave, or saved before weapons) is not the same as one that loads them (2026-11-06, a player: with
// only the weapon changed, + found the armour "already saved" and nothing was saved; the weapon never loaded).
bool same_saved(const Outfit& a, const Outfit& b) { return uses_weapons(a) == uses_weapons(b) && same_pieces(a, b); }

// The saved outfit whose pieces are the worn ones, or -1.
int find_worn_outfit(U64 self)
{
    WornRaw w[kWornMax];
    int n = read_worn(self, w, kWornMax);
    if (n <= 0) return -1;
    Outfit now;
    for (int i = 0; i < n; ++i) {
        now.has_weapons |= is_weapon_slot(w[i].slot);
        if (w[i].key != 0xFFFF) now.items.push_back({w[i].slot, w[i].key, w[i].uid});
    }
    std::sort(now.items.begin(), now.items.end(), [](const OutfitItem& a, const OutfitItem& b) { return a.slot < b.slot; });
    auto matches = [&](size_t o) {
        Outfit seen = now; // (what this outfit cares about: not the slots it leaves empty that can't be emptied)
        seen.items.erase(std::remove_if(seen.items.begin(), seen.items.end(),
                                        [&](const OutfitItem& it) {
                                            return g_keep_slots.count(it.slot) && !outfit_piece(g_outfits[o], it.slot);
                                        }),
                         seen.items.end());
        return same_pieces(g_outfits[o], seen);
    };
    // (the anchor first: two outfits can match, e.g. the same armour with "Weapons: Leave", and the loaded one stays)
    if (int a = anchor_outfit(); a >= 0 && matches(size_t(a))) return a;
    for (size_t o = 0; o < g_outfits.size(); ++o)
        if (matches(o)) return static_cast<int>(o);
    return -1;
}

std::string default_name()
{
    for (U32 n = 1;; ++n) {
        std::string s = lang::f("outfit.default_name", {{"n", std::to_string(n)}});
        bool used = false;
        for (const Outfit& o : g_outfits) used |= o.name == s;
        if (!used) return s;
    }
}

// --- Player actor and character -----------------------------------------------------------------
// The context getter fills {vt, actor, u8 valid, ..}; it is released the way the game does it.
U64 acquire_player(U64* ctx)
{
    U64 holder = rp(rp(g_base + kRvaPlayerHolder));
    if (!holder) return 0;
    __try {
        g_actor_ctx(holder, ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ctx[0] = 0;
        return 0;
    }
    return (ctx[2] & 0xFF) && plausible(ctx[1]) ? ctx[1] : 0;
}

void release_player(U64* ctx)
{
    ctx[0] = g_base + kRvaCtxVt;
    __try {
        g_ctx_release(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// The character key the inventory getter uses: u16 at rp(rp(actor+0x68)+0x20)+0x30.
U16 character_of(U64 actor)
{
    U64 p = rp(actor + kOffActorComp);
    p = p ? rp(p + 0x20) : 0;
    U16 k = kNoCharacter;
    if (p) rd(p + 0x30, k);
    return k;
}

std::map<U16, U64> g_known_actors; // character key -> actor, noted at menu open (see party_members)

U16 current_character()
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U16 k = actor ? character_of(actor) : kNoCharacter;
    if (k != kNoCharacter) g_known_actors[k] = actor; // (party_members: another source for the companions)
    static U16 logged = kNoCharacter;
    if (actor && k != logged) {
        logged = k;
        // Outfit Loadouts tells characters apart by these ids (logged for comparison).
        U64 c = rp(actor + kOffActorComp);
        U64 a = c ? rp(c + 0x90) : 0, b = c ? rp(c + 0x290) : 0;
        U64 kind = rp(actor + kOffActorKind);
        LOG("player actor %llX: character %u, own id %08X, in control %08X / %08X, kind %u", actor, k,
            r32(actor + 0x60), a ? r32(a + 0x198) : 0, b ? r32(b + 0x198) : 0, kind ? r32(kind + 1) & 0xFF : 0xFF);
    }
    if (ctx[0]) release_player(ctx);
    return k;
}

// --- Loading an outfit --------------------------------------------------------------------------
// All at once on Space, with the client-side requests the inventory screen uses (they apply immediately):
// first the worn pieces that must come off (slots empty in the outfit, pieces the outfit wears in another
// slot), then every slot whose worn piece differs. The equip request names the piece by bag id + position,
// looked up right before each request (positions change as pieces come off).
struct LoadStep {
    bool equip;
    U16 slot; // equip slot
    U16 key;
    U64 uid;
    bool swap = false; // (a take-off ahead of the outfit's piece: refused, the put-on still swaps it)
};
constexpr int kFault = INT32_MIN;

// What the last guarded equip / unequip call faulted on (filled by the exception filter, logged by error_text).
struct Fault {
    U32 code;
    U64 at, info[2];
    U64 rip, rsp, rax, rcx, rdx, r8;
    U64 frames[12]; // values on the stack that point into the game image (return addresses)
    int frame_count;
};
Fault g_fault;

U64 image_size()
{
    static U64 size = 0;
    if (!size && g_base) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
        size = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew)->OptionalHeader.SizeOfImage;
    }
    return size;
}

int note_fault(EXCEPTION_POINTERS* ep)
{
    g_fault = {};
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* cx = ep->ContextRecord;
    g_fault.code = er->ExceptionCode;
    g_fault.at = reinterpret_cast<U64>(er->ExceptionAddress);
    for (DWORD i = 0; i < 2 && i < er->NumberParameters; ++i) g_fault.info[i] = er->ExceptionInformation[i];
    g_fault.rip = cx->Rip, g_fault.rsp = cx->Rsp, g_fault.rax = cx->Rax, g_fault.rcx = cx->Rcx;
    g_fault.rdx = cx->Rdx, g_fault.r8 = cx->R8;
    U64 end = g_base + image_size();
    for (int i = 0; i < 96 && g_fault.frame_count < 12; ++i) {
        U64 v = 0;
        if (rd(cx->Rsp + i * 8ull, v) && v > g_base && v < end) g_fault.frames[g_fault.frame_count++] = v;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// "+RVA" for game addresses, "module+offset" for others.
std::string where(U64 a)
{
    char b[96];
    if (g_base && a >= g_base && a < g_base + image_size()) {
        std::snprintf(b, sizeof b, "+%llX", a - g_base);
        return b;
    }
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(a), &m) &&
        m) {
        GetModuleFileNameA(m, name, sizeof name);
        const char* base = std::strrchr(name, '\\');
        std::snprintf(b, sizeof b, "%s+%llX", base ? base + 1 : name, a - reinterpret_cast<U64>(m));
    } else {
        std::snprintf(b, sizeof b, "%llX", a);
    }
    return b;
}

std::string fault_text()
{
    char b[256];
    std::snprintf(b, sizeof b, "faulted: code %08X at %s (info %llX %llX) rsp %llX rax %llX rcx %llX rdx %llX r8 %llX",
                  g_fault.code, where(g_fault.at).c_str(), g_fault.info[0], g_fault.info[1], g_fault.rsp,
                  g_fault.rax, g_fault.rcx, g_fault.rdx, g_fault.r8);
    std::string s = b;
    if (g_fault.frame_count) s += "\n      stack:";
    for (int i = 0; i < g_fault.frame_count; ++i) s += " " + where(g_fault.frames[i]);
    return s;
}

int call_unequip(U64 inv, U16 slot)
{
    int err = kFault;
    if (!g_unequip) return err;
    __try {
        g_unequip(inv, &err, static_cast<short>(slot));
    } __except (note_fault(GetExceptionInformation())) {
        err = kFault;
    }
    return err;
}

constexpr U16 kBagCarried = 1, kBagKeyItems = 4, kBagStorage = 7, kBagWardrobe = 14;

// A companion's piece in storage / the wardrobe: the UI request derives the bag from the item's type (the carried bag),
// but the request under it (0x9e0290) takes a bag id, as Kliff's direct equip does. For our call it gets the piece's
// own bag (hooked; every other call passes through untouched), so the game's own path does the rest.
using CompanionEquipFn = U64(__fastcall*)(U64, U64, U64, U64, U64, U64);
CompanionEquipFn g_companion_equip_orig = nullptr;
int g_bag_override = -1; // the bag for our running call (UI thread only; -1: none)

U64 __fastcall companion_equip_hook(U64 target, U64 err, U64 id, U64 bag, U64 pos, U64 slot)
{
    if (g_bag_override >= 0 && GetCurrentThreadId() == g_ui_thread) {
        LOG("  (companion request: bag %u -> %d)", U32(bag & 0xFFFF), g_bag_override);
        bag = (bag & ~0xFFFFull) | U16(g_bag_override);
    }
    return g_companion_equip_orig(target, err, id, bag, pos, slot);
}

bool companion_pull_ok() { return g_companion_equip_orig != nullptr; }

int call_equip_request_raw(U64 actor, U64 item, short pos, U16 slot)
{
    int err = kFault;
    if (!g_equip_request) return err;
    alignas(16) unsigned char copy[0xd0];
    __try {
        std::memcpy(copy, reinterpret_cast<const void*>(item), 200);
        *reinterpret_cast<U16*>(copy + 0xc8) = 0xFFFF;
        *reinterpret_cast<short*>(copy + 0xca) = pos;
        g_equip_request(actor, &err, copy, static_cast<short>(slot));
    } __except (note_fault(GetExceptionInformation())) {
        err = kFault;
    }
    return err;
}

// bag: where the piece is; storage / the wardrobe go through the bag override (carried bags need none).
int call_equip_request(U64 actor, U64 item, short pos, U16 slot, U16 bag)
{
    bool pull = (bag == kBagStorage || bag == kBagWardrobe) && g_companion_equip_orig;
    if (pull) g_bag_override = bag;
    int err = call_equip_request_raw(actor, item, pos, slot);
    g_bag_override = -1;
    return err;
}

int call_unequip_request(U64 actor, U16 slot)
{
    int err = kFault;
    if (!g_unequip_request) return err;
    __try {
        g_unequip_request(actor, &err, static_cast<short>(slot));
    } __except (note_fault(GetExceptionInformation())) {
        err = kFault;
    }
    return err;
}

int call_equip(U64 inv, U16 bag, short pos, U16 slot)
{
    int err = kFault;
    if (!g_equip) return err;
    __try {
        g_equip(inv, &err, bag, pos, static_cast<short>(slot), 0);
    } __except (note_fault(GetExceptionInformation())) {
        err = kFault;
    }
    return err;
}

U64 call_inventory(U64 actor)
{
    if (!g_inventory) return 0;
    __try {
        return g_inventory(actor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

U64 error_entry(int err)
{
    if (!g_error_text || !g_error_mgr) return 0;
    __try {
        return g_error_text(g_error_mgr(), &err);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::string error_text(int err)
{
    if (err == 0) return "ok";
    char b[48];
    if (err == kFault) return fault_text();
    std::snprintf(b, sizeof b, "error %X ", static_cast<U32>(err));
    U64 e = error_entry(err);
    return b + (e ? cstr(rp(rp(e))) : std::string("(no text)"));
}

// Bag = {item[] (200 bytes each) @0, short capacity @+0xc, u16 id @+0x10}; the inventory manager lists them.
struct ItemAt {
    U16 bag;
    short pos;
    U64 item; // its 200 bytes in the bag
};

// --- Bags and settings --------------------------------------------------------------------------
// One inventory for all three characters (each wears its own gear). Bag ids: 1 carried items, 4 key items (both "on
// you"), 7 private storage, 14 wardrobe, 13 a hidden bag (never used); the others hold no gear.
// User settings (the "+" slot's settings view).
bool g_stash = false;               // after a load, saved pieces left in the bag go to storage (off until turned on)
bool stash_of(const Outfit& o) { return o.stash ? o.stash == 1 : g_stash; } // (its own, or the global)
bool g_take_from_companions = true; // pieces another character wears: take them (asked first)
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

constexpr U64 bag_bit(U16 bag) { return bag < 64 ? 1ull << bag : 0; }
// Bags loading takes pieces from: the ones on you, storage and the wardrobe (wherever a piece was put away, by Stash or
// by hand, loading finds it). Companions go through the game's UI requests, which find the item in its own bag (from
// the item type): storage / the wardrobe only with the request's bag set (see call_equip_request).
U64 equip_bags(bool companion)
{
    U64 m = bag_bit(kBagCarried) | bag_bit(kBagKeyItems);
    if (!companion || companion_pull_ok()) m |= bag_bit(kBagStorage) | bag_bit(kBagWardrobe);
    return m;
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

// The bag holding uid, among the bags in `allowed` (bag_bit mask; 0 = any). With a key, the item must also be that item:
// uids are handed out again after loading an older save (the counter is saved with it), so a stale uid can name
// another item.
bool find_in_bags(U64 mgr, U64 uid, ItemAt& at, U64 allowed = 0, U16 key = 0xFFFF)
{
    __try {
        U64 bags = *reinterpret_cast<U64*>(mgr + kOffMgrBags);
        U32 n = *reinterpret_cast<U32*>(mgr + 0x20);
        for (U32 i = 0; bags && i < n && i < 64; ++i) {
            U64 bag = *reinterpret_cast<U64*>(bags + i * 8ull);
            U64 items = bag ? *reinterpret_cast<U64*>(bag) : 0;
            if (items && allowed && !(allowed & bag_bit(*reinterpret_cast<U16*>(bag + 0x10)))) continue;
            short cap = items ? *reinterpret_cast<short*>(bag + kOffBagCount) : 0;
            for (short p = 0; p < cap && p < 4096; ++p) {
                U64 it = items + p * 200ull;
                U16 k = *reinterpret_cast<U16*>(it + 8);
                if (*reinterpret_cast<U64*>(it) == uid && k != 0xFFFF && (key == 0xFFFF || k == key) &&
                    *reinterpret_cast<long long*>(it + 0x10) > 0) {
                    at = {*reinterpret_cast<U16*>(bag + 0x10), p, it};
                    return true;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// An item copy by uid + key (abyss gears are per copy): worn, else in any bag.
U64 find_copy(U64 uid, U16 key)
{
    if (U64 list = equipment_list()) { // (worn: the equipment list's entries start with the item copy)
        U64 e = rp(list + 8);
        U32 n = r32(list + 0x10);
        for (U32 i = 0; e && i < n && i < 64; ++i)
            if (U64 id = 0; rd(e + i * 0xd0ull, id) && id == uid && (r32(e + i * 0xd0ull + 8) & 0xFFFF) == key) return e + i * 0xd0ull; // (rd: starting gear has uids too small for rp)
    }
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 mgr = actor ? call_inventory(actor) : 0;
    if (ctx[0]) release_player(ctx);
    ItemAt at{};
    return mgr && find_in_bags(mgr, uid, at, 0, key) ? at.item : 0;
}

void log_bags(U64 mgr)
{
    U64 bags = rp(mgr + kOffMgrBags);
    U32 n = r32(mgr + 0x20);
    LOG("inventory %llX: %u bags", mgr, n);
    for (U32 i = 0; bags && i < n && i < 64; ++i) {
        U64 bag = rp(bags + i * 8ull);
        U64 items = bag ? rp(bag) : 0;
        short cap = 0;
        U16 id = 0xFFFF;
        if (bag) rd(bag + kOffBagCount, cap), rd(bag + 0x10, id);
        int used = 0;
        for (short p = 0; items && p < cap && p < 4096; ++p) {
            U16 k = 0xFFFF;
            long long c = 0;
            if (rd(items + p * 200ull + 8, k) && rd(items + p * 200ull + 0x10, c) && k != 0xFFFF && c > 0) ++used;
        }
        LOG("  bag %u: id %u, capacity %d, %d items", i, id, cap, used);
    }
}

// DIAG (lost pieces, 2026-11-06): everywhere a piece copy is, read raw: every equipment-list entry (taken-off ones too)
// and every bag position, ignoring the count and key checks the normal scans make. For the log only.
void diag_where(U64 mgr, const char* when, U64 uid, U16 key)
{
    std::string out;
    char b[160];
    if (U64 list = equipment_list()) {
        U64 e = rp(list + 8);
        U32 n = r32(list + 0x10);
        for (U32 i = 0; e && i < n && i < 64; ++i) {
            U64 it = e + i * 0xd0ull, id = 0;
            if (!rd(it, id) || id != uid) continue;
            U16 k = 0xFFFF, sl = 0xFFFF, x = 0;
            long long cnt = 0;
            rd(it + 8, k), rd(it + 0x10, cnt), rd(it + kOffEntrySlot, sl), rd(it + kOffEntrySlot + 2, x);
            std::snprintf(b, sizeof b, " equipment list [%u]: key %u count %lld slot %u (+CA %u);", i, k, cnt, sl, x);
            out += b;
        }
    }
    U64 bags = mgr ? rp(mgr + kOffMgrBags) : 0;
    U32 nb = mgr ? r32(mgr + 0x20) : 0;
    for (U32 i = 0; bags && i < nb && i < 64; ++i) {
        U64 bag = rp(bags + i * 8ull);
        U64 items = bag ? rp(bag) : 0;
        short cap = 0;
        U16 bid = 0xFFFF;
        if (bag) rd(bag + kOffBagCount, cap), rd(bag + 0x10, bid);
        for (short q = 0; items && q < cap && q < 4096; ++q) {
            U64 it = items + q * 200ull, id = 0;
            if (!rd(it, id) || id != uid) continue;
            U16 k = 0xFFFF;
            long long cnt = 0;
            rd(it + 8, k), rd(it + 0x10, cnt);
            std::snprintf(b, sizeof b, " bag %u (id %u) pos %d: key %u count %lld;", i, bid, q, k, cnt);
            out += b;
        }
    }
    LOG("  DIAG %s: key %u \"%s\" uid %llX is at:%s", when, key, item_name(key).c_str(), uid,
        out.empty() ? " nowhere" : out.c_str());
}

// DIAG (lost pieces): who is in a bag position now (bag index into the inventory's list, as find_in_bags reports it).
std::string diag_pos(U64 mgr, U16 bag_id, short pos)
{
    U64 bags = mgr ? rp(mgr + kOffMgrBags) : 0;
    U32 nb = mgr ? r32(mgr + 0x20) : 0;
    for (U32 i = 0; bags && i < nb && i < 64; ++i) {
        U64 bag = rp(bags + i * 8ull);
        U16 bid = 0xFFFF;
        short cap = 0;
        if (!bag || !rd(bag + 0x10, bid) || bid != bag_id || !rd(bag + kOffBagCount, cap) || pos >= cap) continue;
        U64 it = rp(bag) + pos * 200ull, id = 0;
        U16 k = 0xFFFF;
        long long cnt = 0;
        rd(it, id), rd(it + 8, k), rd(it + 0x10, cnt);
        char b[128];
        std::snprintf(b, sizeof b, "key %u \"%s\" uid %llX count %lld", k, k != 0xFFFF ? item_name(k).c_str() : "", id, cnt);
        return b;
    }
    return "unreadable";
}

// The equipment list's per-slot occupancy, as the equip check reads it (0x20ce9a0): +0x18 bucket count, +0x1c
// entries, +0x28 buckets (0x100 bytes each: u32 n, then n x {u32 slot, u32 index}), +0x30 entry pointers (entry+4:
// the slot it holds). A piece can hold a second slot (the wheel greys it out); a piece for a held slot is refused
// ("Cannot be equipped because of another gear") until the game lets go of it. Returns the holding entry, 0 if free.
U64 slot_claim(U16 slot)
{
    U64 list = equipment_list();
    if (!list) return 0;
    __try {
        U32 nb = *reinterpret_cast<U32*>(list + 0x18);
        if (!nb || !*reinterpret_cast<U32*>(list + 0x1c)) return 0;
        U32* b = reinterpret_cast<U32*>((slot % nb) * 0x100ull + *reinterpret_cast<U64*>(list + 0x28));
        for (U32 i = 0; i < b[0] && i < 31; ++i)
            if (b[i * 2 + 2] == slot) {
                U64 e = *reinterpret_cast<U64*>(*reinterpret_cast<U64*>(list + 0x30) + b[i * 2 + 3] * 8ull);
                return *reinterpret_cast<short*>(e + 4) == static_cast<short>(slot) ? e : 0;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

const OutfitItem* outfit_piece(const Outfit& o, U16 slot)
{
    for (const OutfitItem& it : o.items)
        if (it.slot == slot) return &it;
    return nullptr;
}

// Runs one request; false if there is no character to run it for. Kliff (kind 1): the client-side functions (they
// apply at once); companions: the game's UI requests. g_step_err: the last put-on's answer (0 = ok).

int g_step_err = 0;
int g_steps_ok = 0; // requests that went through (counted per load)

bool run_step(const LoadStep& s, size_t number)
{
    g_step_err = 0;
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 c = actor ? rp(actor + kOffActorComp) : 0;
    U64 inv = c ? rp(c + kOffCompInventory) : 0;
    U64 kind = actor ? rp(actor + kOffActorKind) : 0;
    bool player = inv && kind && (r32(kind + 1) & 0xFF) == 1;
    bool companion = actor && kind && !player && g_equip_request && g_unequip_request;
    bool ok = player || companion;
    if (!ok) {
        LOG("  step %zu: no character to change (actor %llX, kind %u, inventory %llX)", number, actor,
            kind ? r32(kind + 1) & 0xFF : 0xFF, inv);
    } else if (companion && !s.equip) {
        int err = call_unequip_request(actor, s.slot);
        g_steps_ok += err == 0; // (a change went through: a model swap follows)
        LOG("  step %zu: (companion) take off equipment-%u (key %u uid %llX): %s", number, s.slot, s.key, s.uid,
            error_text(err).c_str());
    } else if (companion) {
        U64 mgr = call_inventory(actor);
        ItemAt at{};
        if (!mgr || !find_in_bags(mgr, s.uid, at, equip_bags(true), s.key)) {
            LOG("  step %zu: (companion) put on equipment-%u: key %u uid %llX is not in the bags; skipped", number,
                s.slot, s.key, s.uid);
        } else {
            int err = call_equip_request(actor, at.item, at.pos, s.slot, at.bag);
            g_steps_ok += err == 0; // (a change went through: a model swap follows)
            g_step_err = err;
            LOG("  step %zu: (companion) put on equipment-%u: key %u \"%s\" uid %llX from bag %u pos %d: %s", number,
                s.slot, s.key, item_name(s.key).c_str(), s.uid, at.bag, at.pos, error_text(err).c_str());
        }
    } else if (!s.equip) {
        int err = call_unequip(inv, s.slot);
        g_steps_ok += err == 0; // (a change went through: a model swap follows)
        LOG("  step %zu: take off equipment-%u (key %u uid %llX): %s", number, s.slot, s.key, s.uid,
            error_text(err).c_str());
        diag_where(call_inventory(actor), "right after its take-off", s.uid, s.key); // DIAG (lost pieces)
        bool learned = false;
        if (err == kErrNoItemAfterRemoval) {
            std::lock_guard<std::mutex> keys(g_keys_mu);
            learned = g_keep_slots.insert(s.slot).second;
        }
        if (learned) {
            LOG("  (equipment-%u can't be left empty: outfits that leave it empty match whatever is in it)", s.slot);
            save_outfits();
        }
    } else {
        static bool bags_logged = false;
        U64 mgr = call_inventory(actor);
        if (mgr && !bags_logged) {
            bags_logged = true;
            log_bags(mgr);
        }
        ItemAt at{};
        if (!mgr || !find_in_bags(mgr, s.uid, at, equip_bags(false), s.key)) {
            LOG("  step %zu: put on equipment-%u: key %u uid %llX is not in the bags (inventory %llX); skipped",
                number, s.slot, s.key, s.uid, mgr);
        } else {
            int err = call_equip(inv, at.bag, at.pos, s.slot);
            g_steps_ok += err == 0; // (a change went through: a model swap follows)
            g_step_err = err;
            LOG("  step %zu: put on equipment-%u: key %u \"%s\" uid %llX from bag %u pos %d: %s", number, s.slot,
                s.key, item_name(s.key).c_str(), s.uid, at.bag, at.pos, error_text(err).c_str());
            LOG("  DIAG bag %u pos %d after the put-on: %s", at.bag, at.pos, diag_pos(mgr, at.bag, at.pos).c_str()); // DIAG
        }
    }
    if (ctx[0]) release_player(ctx);
    return ok;
}

// Compares the worn armor with the outfit after loading it (slots left as they are don't count). Logs the result when
// everything matches or `final`; true if everything matches.
bool verify_loading(U64 self, const Outfit& o, const std::vector<U16>& leave, bool final)
{
    WornRaw w[kWornMax];
    int n = read_worn(self, w, kWornMax);
    if (n <= 0) {
        if (final) LOG("load \"%s\": done (not checked: %d)", o.name.c_str(), n);
        return final;
    }
    int match = 0, counted = 0;
    std::string differs;
    for (int i = 0; i < n; ++i) {
        if (std::find(leave.begin(), leave.end(), w[i].slot) != leave.end() || !loads_piece(o, w[i].slot)) continue;
        ++counted;
        const OutfitItem* t = outfit_piece(o, w[i].slot);
        bool ok = t ? w[i].key == t->key && w[i].uid == t->uid : w[i].key == 0xFFFF;
        ok |= hands_crossed(o, w, n, w[i].slot); // (its weapons in each other's hands: as worn)
        match += ok;
        if (!ok) {
            char b[128];
            std::snprintf(b, sizeof b, "\n  differs: equipment-%u wears key %u uid %llX, outfit key %u uid %llX",
                          w[i].slot, w[i].key, w[i].key != 0xFFFF ? w[i].uid : 0, t ? t->key : 0xFFFF, t ? t->uid : 0);
            differs += b;
        }
    }
    if (match == counted || final)
        LOG("load \"%s\": done, %d of %d slots match%s", o.name.c_str(), match, counted, differs.c_str());
    return match == counted;
}

// Asynchronous parts of a load: companions' requests apply a moment later, and a piece taken from another character
// lands in the bags only once their unequip went through. pump() finishes them on the first frame they can be: the
// taken piece is put on as soon as it is in a bag, and the load is done on the first frame the worn gear matches the
// outfit (it can't match early: until the changes apply, it differs). kAsyncMs is only the give-up for a piece the
// game refuses.
constexpr U64 kAsyncMs = 3000;

AsyncLoad g_async;

// --- Where the pieces are -----------------------------------------------------------------------
// A saved piece is somewhere while its uid is: worn by this character, in a bag (carried, storage, wardrobe, other) or
// worn by another character. One that is nowhere (sold, dropped, dismantled) can be replaced by another copy of the
// same item (key) that the outfit does not use yet. (Moving an item into / out of storage or the wardrobe gives it a
// new uid: adopt_returned follows it.)
enum class Where : std::uint8_t { Worn, Carried, Storage, Wardrobe, Other, Companion };
struct Owned {
    U64 uid;
    U16 key;
    Where where;
    U16 who = kNoCharacter; // Companion: whose
    U64 actor = 0;          // Companion: their actor
    U16 slot = 0xFFFF;      // Companion: the equip slot they wear it in
};

Where bag_where(U16 bag)
{
    if (bag == kBagCarried || bag == kBagKeyItems) return Where::Carried;
    if (bag == kBagStorage) return Where::Storage;
    if (bag == kBagWardrobe) return Where::Wardrobe;
    return Where::Other;
}

// The other playable characters (their actors): from the player holder's party arrays (four of them; which ones list
// whom depends on who is controlled, and their count fields don't cover every entry, so 8 entries of each are read)
// and from the actor each character had when the menu last opened for it. Every candidate must be an actor of the
// controlled one's class with a character key (anything else read there is rejected).
struct PartyMember {
    U16 key;
    U64 actor;
};

std::vector<PartyMember> party_members(U64 self_actor)
{
    std::vector<PartyMember> out;
    U64 vt = self_actor ? rp(self_actor) : 0;
    if (!vt) return out;
    U16 self_key = character_of(self_actor);
    auto add = [&](U64 a, U16 want) {
        if (!a || a == self_actor || rp(a) != vt) return;
        U16 k = character_of(a);
        if (k == kNoCharacter || k == self_key || (want != kNoCharacter && k != want)) return;
        if (std::none_of(out.begin(), out.end(), [k](const PartyMember& m) { return m.key == k; })) out.push_back({k, a});
    };
    U64 holder = rp(rp(g_base + kRvaPlayerHolder));
    // (the arrays around this field: their order varies and it can move; every entry is checked to be a character's actor)
    for (U64 off = kOffHolderParty - 0x60; off <= kOffHolderParty + 0x60; off += 8) {
        U64 arr = holder ? rp(holder + off) : 0;
        for (U32 i = 0; arr && i < 8; ++i) add(rp(arr + i * 8ull), kNoCharacter);
    }
    for (const auto& [k, a] : g_known_actors) add(a, k);
    return out;
}

// The gear another character wears (its equipment list; entries taken off keep slot 0xFFFF).
void read_member_worn(const PartyMember& m, std::vector<Owned>& out)
{
    U64 c = rp(m.actor + kOffActorComp);
    U64 inv = c ? rp(c + kOffCompInventory) : 0;
    U64 list = inv ? rp(inv + kOffInvEquipList) : 0;
    U64 e = list ? rp(list + 8) : 0;
    U32 n = list ? r32(list + 0x10) : 0;
    for (U32 i = 0; e && i < n && i < 64; ++i) {
        U64 it = e + i * 0xd0ull, uid = 0;
        U16 key = 0xFFFF, slot = 0xFFFF;
        long long count = 0;
        if (!rd(it, uid) || !rd(it + 8, key) || !rd(it + 0x10, count) || !rd(it + kOffEntrySlot, slot)) continue;
        if (key != 0xFFFF && count > 0 && slot != 0xFFFF) out.push_back({uid, key, Where::Companion, m.key, m.actor, slot});
    }
}

// Object-free: the items in the inventory's bags (count; -1 if unreadable).
int read_bags_raw(U64 mgr, Owned* out, int max)
{
    int n = 0;
    __try {
        U64 bags = *reinterpret_cast<U64*>(mgr + kOffMgrBags);
        U32 nb = *reinterpret_cast<U32*>(mgr + 0x20);
        for (U32 i = 0; bags && i < nb && i < 64; ++i) {
            U64 bag = *reinterpret_cast<U64*>(bags + i * 8ull);
            U64 items = bag ? *reinterpret_cast<U64*>(bag) : 0;
            short cap = items ? *reinterpret_cast<short*>(bag + kOffBagCount) : 0;
            for (short p = 0; p < cap && p < 4096 && n < max; ++p) {
                U64 it = items + p * 200ull;
                U16 key = *reinterpret_cast<U16*>(it + 8);
                if (key != 0xFFFF && *reinterpret_cast<long long*>(it + 0x10) > 0)
                    out[n++] = {*reinterpret_cast<U64*>(it), key, bag_where(*reinterpret_cast<U16*>(bag + 0x10))};
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

// The worn armor, every bag item and what the other characters wear; false if the first two can't be read.
bool read_owned(U64 self, std::vector<Owned>& out)
{
    WornRaw w[kWornMax];
    int n = read_worn(self, w, kWornMax);
    if (n <= 0) return false;
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 mgr = actor ? call_inventory(actor) : 0;
    out.assign(4096, Owned{0, 0xFFFF, Where::Other});
    int b = mgr ? read_bags_raw(mgr, out.data(), static_cast<int>(out.size())) : -1;
    std::vector<PartyMember> party = b >= 0 ? party_members(actor) : std::vector<PartyMember>{};
    if (ctx[0]) release_player(ctx);
    if (b < 0) {
        out.clear();
        return false;
    }
    out.resize(b);
    for (int i = 0; i < n; ++i)
        if (w[i].key != 0xFFFF) out.push_back({w[i].uid, w[i].key, Where::Worn});
    for (const PartyMember& m : party) read_member_worn(m, out);
    return true;
}

const Owned* find_owned(const std::vector<Owned>& owned, U64 uid, U16 key)
{
    for (const Owned& x : owned)
        if (x.uid == uid && x.key == key) return &x;
    return nullptr;
}

// True if the character the menu is for is Kliff (kind 1: direct equip calls) rather than a companion.
bool playing_main_character()
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 kind = actor ? rp(actor + kOffActorKind) : 0;
    bool main = kind && (r32(kind + 1) & 0xFF) == 1;
    if (ctx[0]) release_player(ctx);
    return main;
}

// then searches 3 pointer hops out from the controlled actor and the player holder for those actors (pointers) or
// their u32 ids (actor+0x60).

struct PieceCheck {
    std::vector<std::pair<size_t, U64>> copies; // outfit item index -> uid of the copy offered for it
    std::vector<size_t> gone;                   // item indices with no copy
};
bool stash_pending(U64 uid);

PieceCheck check_pieces(const Outfit& o, const std::vector<Owned>& owned)
{
    PieceCheck c;
    std::vector<U64> taken;
    for (const OutfitItem& it : o.items) taken.push_back(it.uid);
    for (size_t k = 0; k < o.items.size(); ++k) {
        const OutfitItem& it = o.items[k];
        if (!loads_piece(o, it.slot)) continue; // (a weapon it doesn't load can't be missing from it)
        bool have = stash_pending(it.uid); // (on its way to storage: followed in a moment)
        for (const Owned& x : owned) have |= x.uid == it.uid && x.key == it.key;
        if (have) continue;
        U64 copy = 0;
        for (Where w : {Where::Carried, Where::Worn, Where::Storage, Where::Wardrobe}) { // copies in the bags first
            for (const Owned& x : owned)
                if (x.key == it.key && x.where == w && std::find(taken.begin(), taken.end(), x.uid) == taken.end()) {
                    copy = x.uid;
                    break;
                }
            if (copy) break;
        }
        if (copy) taken.push_back(copy), c.copies.push_back({k, copy});
        else c.gone.push_back(k);
    }
    return c;
}

std::vector<char> g_outfit_missing; // per outfit
U64 g_missing_checked = 0;          // tick

// A gone piece whose item came back (picked up again, bought back: a copy with a uid above o.seen) is the outfit's
// piece again, no question asked. Copies owned alongside the original (uid below) still go through the question.
// Also moves o.seen up while the outfit is complete. True if the outfit changed.

bool adopt_returned(Outfit& o, const std::vector<Owned>& owned)
{
    bool changed = false;
    if (o.seen) {
        std::vector<U64> taken;
        for (const OutfitItem& it : o.items) taken.push_back(it.uid);
        for (OutfitItem& it : o.items) {
            bool have = false;
            for (const Owned& x : owned) have |= x.uid == it.uid && x.key == it.key;
            if (have || stash_pending(it.uid)) continue;
            for (const Owned& x : owned)
                if (x.key == it.key && x.uid > o.seen && std::find(taken.begin(), taken.end(), x.uid) == taken.end()) {
                    LOG("outfit \"%s\": equipment-%u key %u came back (uid %llX -> %llX)", o.name.c_str(), it.slot,
                        it.key, it.uid, x.uid);
                    it.uid = x.uid;
                    taken.push_back(x.uid);
                    changed = true;
                    break;
                }
        }
    }
    PieceCheck c = check_pieces(o, owned);
    if (c.copies.empty() && c.gone.empty()) {
        U64 top = 0;
        for (const Owned& x : owned) top = x.uid > top && x.uid != ~0ull ? x.uid : top; // (~0: an empty entry, not an item)
        if (top > o.seen) o.seen = top, changed = true;
    }
    return changed;
}

bool refresh_missing(U64 self)
{
    g_missing_checked = GetTickCount64();
    std::vector<Owned> owned;
    if (!read_owned(self, owned)) return false; // keeps the last answer
    bool adopted = g_placeholders_new;
    g_placeholders_new = false;
    for (Outfit& o : g_outfits) { // (saved before placeholders were known: the slot is empty)
        auto ph = std::remove_if(o.items.begin(), o.items.end(), [](const OutfitItem& it) { return is_placeholder(it.key); });
        if (ph == o.items.end()) continue;
        LOG("outfit \"%s\": %zu placeholder piece(s) dropped (an empty slot)", o.name.c_str(), size_t(o.items.end() - ph));
        o.items.erase(ph, o.items.end());
        adopted = true;
    }
    for (Outfit& o : g_outfits) adopted |= adopt_returned(o, owned);
    if (adopted) save_outfits();
    std::vector<char> m(g_outfits.size());
    for (size_t o = 0; o < g_outfits.size(); ++o) m[o] = !check_pieces(g_outfits[o], owned).gone.empty();
    if (m == g_outfit_missing) return adopted;
    for (size_t o = 0; o < m.size(); ++o)
        if (o >= g_outfit_missing.size() || m[o] != g_outfit_missing[o])
            LOG("outfit \"%s\": %s", g_outfits[o].name.c_str(), m[o] ? "a piece is gone (greyed out)" : "complete");
    g_outfit_missing = m;
    return true;
}

bool outfit_missing(size_t o) { return o < g_outfit_missing.size() && g_outfit_missing[o]; }

const char* slot_name(U16 slot)
{
    switch (slot) {
    case 3: return lang::t("slot.helmet");
    case 4: return lang::t("slot.chest");
    case 5: return lang::t("slot.gloves");
    case 6: return lang::t("slot.shoes");
    case 7: return lang::t("slot.earring1");
    case 8: return lang::t("slot.earring2");
    case 9: return lang::t("slot.necklace");
    case 10: return lang::t("slot.ring1");
    case 11: return lang::t("slot.ring2");
    case 16: return lang::t("slot.cloak");
    case 17: return lang::t("slot.glasses");
    case 18: return lang::t("slot.mask");
    }
    return lang::t("slot.piece");
}

// "Cloak (Leather Cloak), Ring 1 (Silver Ring)": the outfit's pieces in these slots.
std::string slot_list(const Outfit& o, const std::vector<U16>& slots)
{
    std::string s;
    for (U16 slot : slots) {
        std::string one = slot_name(slot);
        for (const OutfitItem& it : o.items)
            if (it.slot == slot) {
                std::string n = item_name(it.key);
                if (!n.empty()) one = lang::f("piece.with_item", {{"slot", one}, {"item", n}});
                break;
            }
        s += (s.empty() ? std::string() : lang::f("list.separator")) + one;
    }
    return s;
}

// leave: the outfit's piece can't be put on there (a worn piece that isn't the outfit's still comes off);
// keep: untouched (a piece taken from another character is on its way there).

void load_outfit(U64 self, U32 index, const std::vector<U16>& leave = {}, const std::vector<U16>& keep = {})
{
    const Outfit& o = g_outfits[index];
    // (one outfit at a time: the last one is still going on the model; only while that load runs, so a part the game
    // leaves "loading" can never block loading for good)
    if (g_async.active && look_changing()) {
        LOG("load \"%s\": ignored, the last outfit is still being put on", o.name.c_str());
        report(self, lang::t("report.still_loading"));
        return;
    }
    WornRaw w[kWornMax];
    int n = read_worn(self, w, kWornMax);
    if (n <= 0) {
        LOG("load \"%s\": worn gear unreadable (%d)", o.name.c_str(), n);
        return;
    }
    look_hold(true); // (until the model swaps: the old pieces keep their looks, the new ones hide at once)
    mark_worn(static_cast<int>(index)); // (its looks: each piece as soon as it is on)
    std::vector<LoadStep> off, on;
    std::vector<LeavingPiece> leaving; // the worn pieces this load replaces or takes off (must still be owned after)
    std::set<U16> keep_slots; // (slots the game refuses to empty: asking only brings its error message and a wait)
    {
        std::lock_guard<std::mutex> keys(g_keys_mu);
        keep_slots = g_keep_slots;
    }
    for (int i = 0; i < n; ++i) {
        if (!loads_piece(o, w[i].slot)) continue; // (Weapons: Leave / saved before weapons)
        const OutfitItem* t = outfit_piece(o, w[i].slot);
        bool worn = w[i].key != 0xFFFF;
        bool left = std::find(leave.begin(), leave.end(), w[i].slot) != leave.end();
        bool kept = std::find(keep.begin(), keep.end(), w[i].slot) != keep.end();
        LOG("  slot %d (equipment-%u): worn key %u \"%s\" uid %llX at %u/%u, outfit key %u \"%s\" uid %llX%s", i,
            w[i].slot, w[i].key, worn ? item_name(w[i].key).c_str() : "", worn ? w[i].uid : 0, w[i].at[0], w[i].at[1],
            t ? t->key : 0xFFFF, t ? item_name(t->key).c_str() : "", t ? t->uid : 0,
            kept ? " (a taken piece comes here)" : left ? " (outfit piece unavailable)" : "");
        if (kept || (worn && t && w[i].uid == t->uid && w[i].key == t->key)) continue;
        if (hands_crossed(o, w, n, w[i].slot)) { // (its weapons, in each other's hands: left as they are)
            LOG("  equipment-%u: the outfit's weapon is in the other hand; left as it is", w[i].slot);
            continue;
        }
        if (left) {
            if (worn && !keep_slots.count(w[i].slot)) off.push_back({false, w[i].slot, w[i].key, w[i].uid}); // not the outfit's: off
            continue;
        }
        bool needed = false; // the worn piece goes to another slot of the outfit
        for (const OutfitItem& it : o.items) needed |= worn && it.uid == w[i].uid && it.key == w[i].key;
        // A worn piece is taken off before the outfit's goes on (not swapped by the equip): the game's swap sends the
        // old piece back to a bag position it remembers, and a piece remembering a position that doesn't exist was
        // lost on the way (2026-11-06, a player's log: every lost piece had been put on from one bag position and was
        // swapped out; pieces taken off never got lost). Slots the game won't leave empty keep the swap.
        bool swap_off = worn && t && !keep_slots.count(w[i].slot);
        if (worn && (needed || swap_off || (!t && !keep_slots.count(w[i].slot))))
            off.push_back({false, w[i].slot, w[i].key, w[i].uid, swap_off && !needed});
        if (t) on.push_back({true, t->slot, t->key, t->uid});
        if (worn && (t || !keep_slots.count(w[i].slot))) leaving.push_back({w[i].uid, w[i].key, w[i].slot}); // (checked after)
    }
    if (off.empty() && on.empty() && !g_async.active) {
        LOG("load \"%s\": already worn", o.name.c_str());
        look_hold(false);
        look_refresh();
        return;
    }
    LOG("load \"%s\": %zu to take off, %zu to put on", o.name.c_str(), off.size(), on.size());
    off.insert(off.end(), on.begin(), on.end());
    std::vector<DeferredEquip> held;
    g_steps_ok = 0;
    std::set<U16> refused; // (slots whose take-off the game refused: nothing goes on there in this load)
    for (size_t i = 0; i < off.size(); ++i) {
        if (off[i].equip && refused.count(off[i].slot)) {
            LOG("  step %zu: put on equipment-%u skipped: its take-off was refused", i + 1, off[i].slot);
            continue;
        }
        if (!run_step(off[i], i + 1)) break;
        if (!off[i].equip && g_step_err && off[i].swap)
            LOG("  (equipment-%u: the outfit's piece swaps it instead)", off[i].slot);
        else if (!off[i].equip && g_step_err)
            refused.insert(off[i].slot);
        if (!off[i].equip || !g_step_err || g_step_err == kFault) continue;
        U64 e = slot_claim(off[i].slot);
        if (!e) continue; // (refused for another reason: verify reports it)
        LOG("  equipment-%u is held by entry %llX (%s); put on once the game lets go of it", off[i].slot, e,
            hex(e, 16).c_str());
        held.push_back({off[i].uid, off[i].slot, off[i].key, slot_list(o, {off[i].slot}), "", true});
    }
    // The new pieces reach the inventory over the next frames (pump_async syncs each one; the model swaps after).
    // Nothing went through (every step refused): no swap is coming, so the looks apply in full.
    if (!g_steps_ok) look_hold(false);
    look_refresh();
    // Checked by pump once it settled: companions' requests apply a moment later, and even Kliff's direct changes
    // reach the worn list late when there are many (11 take-offs checked at once: "1 of 11").
    g_async = {o.name, leave, std::move(g_async.equips), GetTickCount64(), true};
    for (DeferredEquip& d : held) g_async.equips.push_back(std::move(d));
    g_async.leaving = std::move(leaving);
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
        if ((part == kTooltipEquipped && g_center_outfit >= 0 && g_center_outfit == g_worn_outfit) || desc) {
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

// --- Focus and keys -----------------------------------------------------------------------------
// Index of a slot script in the Outfits tab (kOutfitSlots if not there).
U32 outfit_slot_index(U64 self, U64 script)
{
    U32 tab = g_fake_key.load();
    U64 sv = rp(self + kOffRootSlotVecs);
    if (!script || tab == 0xFFFF || !sv) return kOutfitSlots;
    U64 arr = rp(sv + tab * 0x10ull);
    U32 cnt = r32(sv + tab * 0x10ull + 8);
    for (U32 i = 0; arr && i < cnt && i < kOutfitSlots; ++i)
        if (rp(arr + i * 8ull) == script) return i;
    return kOutfitSlots;
}

U64 focused_script(U64 self)
{
    std::uint8_t mode = 0;
    if (!rd(self + kOffRootFocusIdx, mode)) return 0;
    __try {
        return g_focused_slot(self, mode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
std::string cfg_description(std::uint8_t k);
std::string cfg_title(std::uint8_t k);
std::string cfg_value_text(std::uint8_t k, const char* v);
std::vector<const char*> cfg_values(std::uint8_t k, U32& cur);

// Shows the focused saved outfit's name in the centre, or what the "+" slot does.
void center_for_focus(U64 self, U64 script, bool hide_otherwise)
{
    U32 i = outfit_slot_index(self, script);
    if (g_cfg >= 0) { // the settings view: the setting, its value, what it does
        std::vector<std::uint8_t> e = cfg_entries();
        U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
        if (k >= e.size()) {
            if (hide_otherwise) center_hide(self);
            return;
        }
        U32 cur = 0;
        std::vector<const char*> v = cfg_values(e[k], cur);
        g_center_name = cfg_title(e[k]);
        g_center_desc = (v.empty() ? std::string() : cfg_value_text(e[k], v[cur]) + ": ") + cfg_description(e[k]);
        g_center_outfit = -1;
        g_center_frames = 30;
        center_show(self);
        return;
    }
    if (g_eye >= 0 && size_t(g_eye) < g_outfits.size()) { // the Appearance view: the piece and its look / the page step
        std::vector<size_t> pieces = eye_pieces();
        U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
        static const char* const kLookText[kLookCount] = { // (by Look value)
            "look.default", "look.hidden", "look.combat", "look.peace", "look.shown"};
        static const char* const kStepName[kEyeSteps] = {"page.armour", "page.weapons", "page.accessories"};
        if (k < pieces.size()) {
            OutfitItem tmp{};
            const OutfitItem& it = *eye_item(pieces[k], tmp);
            std::string nm = it.key != 0xFFFF ? item_name(it.key) : std::string();
            if (pieces[k] == kBackPiece)
                nm = nm.empty() ? std::string(lang::t("piece.back_shield")) : lang::f("piece.back_shield_item", {{"item", nm}});
            g_center_name = nm.empty() ? std::string(lang::t("piece.empty")) : nm;
            g_center_desc = lang::t(kLookText[it.look < kLookCount ? it.look : 0]);
        } else if (k == pieces.size()) {
            g_center_name = lang::t(kStepName[g_eye_pick]);
            g_center_desc = lang::t(g_eye_pick == g_eye_step ? "page.this" : "page.open");
        } else if (k == pieces.size() + 1) {
            g_center_name = lang::t("center.back_settings");
            g_center_desc = lang::t("desc.back_settings");
        } else {
            if (hide_otherwise) center_hide(self);
            return;
        }
        g_center_outfit = -1;
        g_center_frames = 30;
        center_show(self);
        return;
    }
    U32 o = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (o < kOutfitSlots) {
        std::uint8_t mode = g_slot_mode[i];
        bool saved = o < g_outfits.size();
        g_center_name = mode == kModeSettings ? std::string(lang::t(saved ? "center.outfit_settings" : "center.settings"))
                        : saved && g_outfits[o].worn && g_worn_outfit != int(o) ? lang::f("center.modified", {{"outfit", g_outfits[o].name}})
                        : saved                              ? g_outfits[o].name
                        : g_outfits.size() >= kMaxOutfits    ? std::string(lang::t("center.limit"))
                                                             : std::string(lang::t("center.save"));
        g_center_outfit = saved && mode == kModeMain ? int(o) : -1;
        g_center_frames = 30;
        center_show(self);
    } else {
        g_center_outfit = -1;
        if (hide_otherwise) center_hide(self);
    }
}

// Key presses are latched by the key thread (tick of the press) and handled on the UI thread.
std::atomic<U64> g_space_at{0};
std::atomic<bool> g_menu_open{false};
DWORD g_ui_thread = 0;

bool take_press(std::atomic<U64>& at)
{
    U64 t = at.exchange(0);
    return t && GetTickCount64() - t < 500;
}

void refresh_wheel(U64 self)
{
    // (a load from the quick peek passes the status screen: no wheel to refresh, and its layout check would read the
    // status screen as a broken wheel and turn the mod off)
    if (self != g_outfits_root) return;
    if (g_applied_visible != visible_slots()) reset_modes(); // (the slots moved)
    apply_outfit_slots(self);
    sync_ring(self);
}

// --- Rename prompt: the game's own TextEditModalMessage (as its naming prompts open it) ----------
// Confirm with text renames the outfit just saved; an empty confirm, cancel, Esc or closing the menu keep "Outfit N".
std::atomic<U64> g_name_modal{0}; // the open prompt (0: none)
U64 g_name_self = 0;       // the gear menu it was opened from
std::string g_name_default;
size_t g_name_index = 0;   // the outfit being named (checked against g_name_default)
U64 g_name_opened = 0;     // tick
U64 g_modal_ended = 0;      // tick: presses made while typing are dropped for a moment after the prompt closes
unsigned char g_name_shown = 0; // modal +0xbd at open (the shown flag; the hide clears it)
alignas(16) unsigned char g_modal_params[0x1800];
static_assert(sizeof(g_modal_params) >= kParamsSize, "the modal parameter block does not fit");

// Object-free (for __try): the opened modal or 0.
U64 open_text_modal_raw(U64 self, const char* message)
{
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        U64 view = ui ? g_find_view(ui, "ModalMessageView") : 0;
        if (!view) return 0;
        g_params_init(reinterpret_cast<U64>(g_modal_params));
        strncpy_s(reinterpret_cast<char*>(g_modal_params + kParamsMessage), kParamsMessageCap, message, _TRUNCATE);
        *reinterpret_cast<U32*>(g_modal_params + kParamsMessageLen) = 0xFFFFFFFF; // length: recompute
        U64 modal = g_open_text_modal(0, view, g_modal_params, kNameMaxChars);
        g_params_free(reinterpret_cast<U64>(g_modal_params));
        if (modal && !has_vt(modal, kRvaTextModalVt)) return 0; // not the modal we know: leave it alone
        if (modal) *reinterpret_cast<U64*>(modal + kOffModalOwner) = 0; // no owner callback (a stale one from earlier use)
        return modal;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Object-free: the prompt's current text (empty if it can't be read).
void read_modal_text_raw(U64 modal, char* out, int size)
{
    out[0] = 0;
    __try {
        U64 input = *reinterpret_cast<U64*>(modal + kOffModalInput);
        if (input && *reinterpret_cast<unsigned char*>(input + kOffInputBusy) == 0)
            g_get_text(*reinterpret_cast<U64*>(input + kOffInputText), out, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
    out[size - 1] = 0;
}

// Object-free: closes the prompt the way its own buttons do.
void hide_modal_raw(U64 modal)
{
    __try {
        g_hide_script(modal);
        if (kOffModalD0)
            if (U64 d0 = *reinterpret_cast<U64*>(modal + kOffModalD0)) *reinterpret_cast<unsigned char*>(d0 + 0xf2) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("rename: hide faulted");
    }
}

U64 call_click_raw(U64 modal, U64 a, U64 elem, U64 flag)
{
    __try {
        return g_text_click_orig(modal, a, elem, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("rename: game click handler faulted");
        return 0;
    }
}

bool name_invalid_raw(const char* s)
{
    __try {
        return g_name_invalid(s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string clean_name(const std::string& in)
{
    std::string s;
    for (char c : in)
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) s += c; // no tabs / newlines (store format)
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// A prompt takes the input while it is open, so the gear menu misses the release of the button that holds it open
// and stays up afterwards. Once a prompt is gone, do what the game's release handler (0x1098f10, event 1) does.
using QueueHideFn = void(__fastcall*)(U64, U64);
QueueHideFn g_queue_hide = nullptr;
bool g_release_pending = false; // set when a prompt closes while the menu is open; done by pump on the next frame

// The input side of the same problem: the HUD key binding that holds the menu (0xcc40c0's binding: +0x232 "pressed")
// never sees its key go up either, so the next press only counts as the missing release. The hook remembers the binding
// whose down / held event left it pressed (with a copy of that event); after a prompt the game is handed that event
// again as an up (type 7 at +0x38) and runs its own release: binding state, hold progress, the HUD's release handler
// (which closes the menu).
using BindingInputFn = void(__fastcall*)(U64, U64, U64);
BindingInputFn g_binding_input_orig = nullptr;
constexpr U32 kInputEventCopy = 0x40; // the fields the state machine reads end at +0x3a
struct HeldBinding {
    U64 dispatcher = 0, binding = 0;
    alignas(16) unsigned char event[kInputEventCopy] = {};
};
std::map<U64, HeldBinding> g_pressed; // bindings whose key is down, by binding
U64 g_last_pressed = 0;               // the binding pressed last (at menu open: the one that opened it)
HeldBinding g_held;                   // the menu's binding, taken at menu open
std::mutex g_held_mu; // input may arrive on another thread than the menu's

// At menu open: the binding pressed last is the menu's button.
void note_menu_binding()
{
    std::lock_guard lock(g_held_mu);
    auto it = g_pressed.find(g_last_pressed);
    g_held = it != g_pressed.end() ? it->second : HeldBinding{};
}

// 1 = pressed by a down / held event, 2 = released (up), 0 = neither or unreadable.
int binding_change(U64 event, U64 binding, unsigned char* copy)
{
    __try {
        unsigned char type = *reinterpret_cast<unsigned char*>(event + kOffEventType);
        if ((type == 3 || type == 6) && *reinterpret_cast<unsigned char*>(binding + kOffBindingPressed)) {
            std::memcpy(copy, reinterpret_cast<const void*>(event), kInputEventCopy);
            return 1;
        }
        return type == 7 ? 2 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void __fastcall binding_input_hook(U64 dispatcher, U64 event, U64 binding)
{
    g_binding_input_orig(dispatcher, event, binding);
    if (!kOffEventType || !kOffBindingPressed) return; // (offsets doubtful after a game update)
    alignas(16) unsigned char copy[kInputEventCopy];
    int change = binding_change(event, binding, copy);
    if (!change) return;
    std::lock_guard lock(g_held_mu);
    if (change == 1) {
        HeldBinding& h = g_pressed[binding];
        h.dispatcher = dispatcher, h.binding = binding;
        std::memcpy(h.event, copy, kInputEventCopy);
        g_last_pressed = binding;
    } else {
        g_pressed.erase(binding);
        if (binding == g_held.binding) g_held = {};
    }
}

// 1 = released through the game's input, 0 = its up did arrive, -1 = fault.
int send_up(HeldBinding& h)
{
    __try {
        if (!*reinterpret_cast<unsigned char*>(h.binding + kOffBindingPressed)) return 0;
        if (!kOffEventType || kOffEventType >= kInputEventCopy) return -1; // layout doubtful / beyond our copy
        h.event[kOffEventType] = 7; // key up
        h.event[0x10] = 0; // "handled" (the state machine sets it)
        g_binding_input_orig(h.dispatcher, reinterpret_cast<U64>(h.event), h.binding);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// True if a held binding was released through the game's own input path.
bool release_held_binding()
{
    HeldBinding h;
    {
        std::lock_guard lock(g_held_mu);
        h = g_held;
        g_held = {};
        g_pressed.erase(h.binding);
    }
    if (!h.binding || !g_binding_input_orig) return false;
    int r = send_up(h);
    LOG("prompt closed: %s (binding %llX)", r > 0 ? "menu button released through the game's input" :
        r == 0 ? "menu button was already up" : "input release faulted", h.binding);
    return r > 0;
}

void release_menu_button(U64 self)
{
    if (release_held_binding()) return;
    if (!kOffPanelFlags || !kOffPanelHoldClose || !kOffPanelElem) return; // (doubtful after a game update)
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        U64 view = ui ? g_find_view(ui, "ItemQuickSlotView2") : 0;
        if (!view || (*reinterpret_cast<unsigned char*>(view + kOffViewFlags) & 0x60) != 0x40) return; // not shown
        U64 inner = *reinterpret_cast<U64*>(view + kOffViewInner);
        U64 panel = inner ? *reinterpret_cast<U64*>(inner + 0x10) : 0;
        if (!panel) return;
        auto flags = reinterpret_cast<unsigned char*>(panel + kOffPanelFlags);
        *flags &= 0xfe; // the button is up
        bool held_open = (*flags & 2) && *reinterpret_cast<unsigned char*>(panel + kOffPanelHoldClose); // opened by holding
        LOG("prompt closed: menu button released (%s)", held_open ? "menu closes" : "menu was opened by a tap: stays");
        if (held_open) {
            U64 elem = *reinterpret_cast<U64*>(panel + kOffPanelElem);
            if (!is_element(elem)) {
                LOG("prompt closed: the menu's view element is not where expected; not closing it");
                return;
            }
            g_queue_hide(*reinterpret_cast<U64*>(elem + 0x58), elem);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("prompt closed: menu button release faulted");
    }
}

void finish_naming(const std::string& text, const char* why)
{
    if (!g_name_modal) return;
    Busy busy;
    g_name_modal = 0;
    g_modal_ended = GetTickCount64();
    g_release_pending = g_menu_open;
    U64 self = g_name_self;
    std::string name = clean_name(text);
    bool here = g_name_index < g_outfits.size() && g_outfits[g_name_index].name == g_name_default;
    LOG("rename: %s, text \"%s\" -> \"%s\"%s", why, text.c_str(), name.c_str(), here ? "" : " (outfit gone)");
    if (!here || name.empty() || name == g_name_default) return;
    for (const Outfit& o : g_outfits)
        if (o.name == name) {
            if (g_menu_open) notify(self, lang::f("toast.name_taken", {{"outfit", name}}));
            return;
        }
    g_outfits[g_name_index].name = name;
    save_outfits();
    if (g_menu_open && outfits_current(self)) {
        center_restore();
        center_for_focus(self, focused_script(self), true);
    }
}

// Slot 113 of the TextEdit modal (its button / key handler): only our prompt is handled here.
U64 __fastcall text_click_hook(U64 modal, U64 a, U64 elem, U64 flag)
{
    if (!g_name_modal || modal != g_name_modal || (flag & 0xFF)) return g_text_click_orig(modal, a, elem, flag);
    if (elem && elem == rp(modal + kOffModalConfirm)) {
        char buf[0x105] = {};
        read_modal_text_raw(modal, buf, sizeof buf);
        std::string text = buf;
        if (clean_name(text).empty()) { // the game ignores an empty confirm; here it keeps the default name
            hide_modal_raw(modal);
            finish_naming("", "confirmed empty");
            return 0;
        }
        if (name_invalid_raw(buf)) {
            LOG("rename: \"%s\" rejected by the game's name filter", buf);
            return call_click_raw(modal, a, elem, flag); // shows the game's error and keeps the prompt open
        }
        U64 r = call_click_raw(modal, a, elem, flag); // validates again and closes
        finish_naming(text, "confirmed");
        return r;
    }
    if (elem && elem == rp(modal + kOffModalCancel)) {
        U64 r = call_click_raw(modal, a, elem, flag);
        finish_naming("", "cancelled");
        return r;
    }
    return g_text_click_orig(modal, a, elem, flag);
}

// --- Typed characters the prompt drops -------------------------------------------------------------
// The game's text box makes its own characters from the keys: letters (shifted too) and digits, but no Shift / AltGr
// symbols (! + / * ...) or accented letters, though pasting them works. While our prompt is open the characters
// Windows makes for the game window (WM_CHAR) are collected; once typing pauses for two frames, the ones the box didn't
// take are put in at the caret (input +0xd8 / +0xdc: caret and selection end, in characters), which then moves past them
// (+0xf1: the caret-moved flag the game's own keys set, so the caret is drawn there at once).

WNDPROC g_game_wndproc = nullptr;
std::mutex g_typed_mu;
std::wstring g_typed;       // characters since the batch began
std::string g_typed_base;   // the box text on the frame before the batch
U32 g_typed_caret = 0;      // and its caret
int g_typed_quiet = 0;      // frames since the last character
std::string g_name_text;    // the box text on the last frame
U32 g_name_caret = 0;       // its caret

// The wheel hook is in (the arc open): it alone counts the notches. The window and raw input routes then only keep
// the wheel from the game (2026-11-06: all three counted, 6 steps per notch, and with 3 outfits the arc went round
// twice and seemed not to move).
std::atomic<bool> g_wheel_hooked{false};
extern std::atomic<U64> g_peek_hud_at;

// Is (vk) with the modifiers held now one of the current outfits' hotkeys (in play)?
bool hotkey_held_now(U32 vk)
{
    if (!vk || g_menu_open || g_name_modal || GetTickCount64() - g_peek_hud_at.load() >= 250) return false;
    std::uint8_t mods = static_cast<std::uint8_t>(((GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0) |
                                                  ((GetKeyState(VK_MENU) & 0x8000) ? 2 : 0) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 4 : 0));
    bool bare = false; // (no hotkey with exactly these modifiers: the key alone counts, e.g. while Shift sprints)
    for (const auto& h : g_hotkeys) {
        U32 v = h.load();
        if (!v || (v & 0xFF) != vk) continue;
        if (((v >> 8) & 0xFF) == mods) return true;
        bare |= ((v >> 8) & 0xFF) == 0;
    }
    return bare;
}
extern std::atomic<bool> g_peek_live;
extern std::atomic<int> g_peek_wheel;

LRESULT CALLBACK name_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && hotkey_held_now(U32(wp)))
        return 0; // (an outfit's hotkey: its key does nothing else in play)
    if (m == WM_MBUTTONDOWN || m == WM_MBUTTONUP || m == WM_XBUTTONDOWN || m == WM_XBUTTONUP) {
        U32 vk = m == WM_MBUTTONDOWN || m == WM_MBUTTONUP ? VK_MBUTTON : GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        if (hotkey_held_now(vk)) return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;
    }
    // The Outfit Arc: while it is open the wheel is its (not the camera's); its key's messages are its own in play.
    if (m == WM_MOUSEWHEEL) {
        if (g_peek_live) {
            if (!g_wheel_hooked) g_peek_wheel += GET_WHEEL_DELTA_WPARAM(wp); // (else the hook counts it)
            return 0;
        }
    }
    if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && g_peek_key.load() &&
        wp == g_peek_key.load() && GetTickCount64() - g_peek_hud_at.load() < 250 && !g_menu_open && !g_name_modal) {
        return 0;
    }
    if (m == WM_CHAR && g_name_modal && wp >= 0x20 && wp != 0x7f && (wp < 0xD800 || wp > 0xDFFF)) {
        std::lock_guard<std::mutex> lock(g_typed_mu);
        if (g_typed.empty()) g_typed_base = g_name_text, g_typed_caret = g_name_caret;
        g_typed += wchar_t(wp);
        g_typed_quiet = 0;
    }
    return CallWindowProcW(g_game_wndproc, w, m, wp, lp);
}
bool game_focused();

// Once, when our prompt first opens (the game window is in front then); stays in place, idle while no prompt is open.
void watch_game_chars()
{
    if (g_game_wndproc || !game_focused()) return;
    HWND w = GetForegroundWindow();
    g_game_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&name_wndproc)));
    LOG("rename: watching typed characters (window %p)", static_cast<void*>(w));
}

// Object-free: the prompt's caret (false if it can't be read, or a selection is open).
bool read_modal_caret_raw(U64 modal, U32& caret)
{
    __try {
        U64 input = *reinterpret_cast<U64*>(modal + kOffModalInput);
        if (!input) return false;
        caret = *reinterpret_cast<U32*>(input + kOffInputCaret);
        return caret == *reinterpret_cast<U32*>(input + kOffInputCaretEnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Object-free: sets the prompt's text and caret (false if it can't be reached).
bool write_modal_text_raw(U64 modal, const char* text, U32 caret)
{
    __try {
        U64 input = *reinterpret_cast<U64*>(modal + kOffModalInput);
        if (!input || *reinterpret_cast<unsigned char*>(input + kOffInputBusy) != 0) return false;
        g_set_text(*reinterpret_cast<U64*>(input + kOffInputText), text, 0);
        *reinterpret_cast<U32*>(input + kOffInputCaret) = caret;
        *reinterpret_cast<U32*>(input + kOffInputCaretEnd) = caret;
        *reinterpret_cast<unsigned char*>(input + kOffInputCaretMoved) = 1; // as the game's own caret keys: redrawn now
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

size_t utf8_chars(const std::string& s)
{
    size_t n = 0;
    for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

// The byte offset of character `chars` (the end if past it).
size_t utf8_offset(const std::string& s, size_t chars)
{
    size_t i = 0;
    for (; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && chars-- == 0) break;
    return i;
}

void type_dropped_chars()
{
    char buf[0x105] = {};
    read_modal_text_raw(g_name_modal, buf, sizeof buf);
    U32 caret = 0;
    bool caret_ok = read_modal_caret_raw(g_name_modal, caret);
    std::string now = buf, base;
    std::wstring typed;
    U32 base_caret = 0;
    {
        std::lock_guard<std::mutex> lock(g_typed_mu);
        g_name_text = now;
        g_name_caret = caret_ok ? caret : 0xFFFFFFFF;
        if (g_typed.empty() || ++g_typed_quiet < 2) return;
        typed.swap(g_typed);
        base = g_typed_base;
        base_caret = g_typed_caret;
    }
    // What the game typed went in at the caret: the text around it must be as before (else: deleted / a selection /
    // edited in the same moment, and it is left as it is).
    size_t pos = base_caret == 0xFFFFFFFF ? std::string::npos : utf8_offset(base, base_caret);
    size_t tail = pos == std::string::npos ? 0 : base.size() - pos;
    if (pos == std::string::npos || now.size() < base.size() || now.compare(0, pos, base, 0, pos) != 0 ||
        now.compare(now.size() - tail, tail, base, pos, tail) != 0) {
        LOG("rename: typed characters skipped (text \"%s\" was \"%s\", caret %u)", now.c_str(), base.c_str(), base_caret);
        return;
    }
    std::string out = now;
    bool added = false;
    for (wchar_t c : typed) {
        char u[8] = {};
        int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, u, sizeof u, nullptr, nullptr);
        if (n <= 0) continue;
        if (out.compare(pos, size_t(n), u, size_t(n)) == 0) { pos += size_t(n); continue; } // the game took it
        if (utf8_chars(out) >= size_t(kNameMaxChars)) break;
        out.insert(pos, u, size_t(n));
        pos += size_t(n);
        added = true;
    }
    if (!added) return;
    U32 at = U32(utf8_chars(out.substr(0, pos)));
    if (write_modal_text_raw(g_name_modal, out.c_str(), at)) {
        LOG("rename: typed characters added: \"%s\" -> \"%s\"", now.c_str(), out.c_str());
        std::lock_guard<std::mutex> lock(g_typed_mu);
        g_name_text = out;
        g_name_caret = at;
    }
}

void open_rename_prompt(U64 self, size_t index)
{
    if (!g_open_text_modal || g_name_modal) return;
    std::string msg = lang::f("prompt.name", {{"outfit", g_outfits[index].name}});
    U64 modal = open_text_modal_raw(self, msg.c_str());
    if (!modal) {
        LOG("rename: prompt did not open; name stays \"%s\"", g_outfits[index].name.c_str());
        return;
    }
    g_name_modal = modal;
    g_name_self = self;
    g_name_index = index;
    g_name_default = g_outfits[index].name;
    g_name_opened = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_typed_mu);
        g_typed.clear();
        g_name_text.clear();
        g_name_caret = 0;
    }
    watch_game_chars();
    rd(modal + kOffModalShown, g_name_shown);
    unsigned char hiding = 0;
    rd(modal + kOffModalHidden, hiding);
    LOG("rename: prompt %llX open for \"%s\" (+0xbd %u, +0xbe %u, input %llX)", modal, g_name_default.c_str(),
        g_name_shown, hiding, rp(modal + kOffModalInput));
}

// While the prompt is open: notices it closing some other way (the shown flag drops, or a 3-minute safety stop).
void watch_rename_prompt()
{
    if (!g_name_modal) return;
    type_dropped_chars();
    unsigned char shown = g_name_shown;
    rd(g_name_modal + kOffModalShown, shown);
    if (g_name_shown && !shown) finish_naming("", "prompt closed elsewhere");
    else if (GetTickCount64() - g_name_opened > 180000) {
        hide_modal_raw(g_name_modal);
        finish_naming("", "timed out");
    }
}

// --- Questions before loading: the game's own yes / no CommonModalMessage --------------------------
// 1. Pieces another character wears (setting on): "... is worn by Oongka. Take it?" - Take: their unequip request, then
//    the piece is put on once it is in the bags (pump); Leave as is: those slots stay.
// 2. Pieces that are gone while another copy is owned: "Wear copy" (saved into the outfit) / "Leave as is".
// Slots left as they are get one notification saying why. Esc / closing the menu loads nothing.
struct Take {
    size_t k;     // outfit item index
    U16 who;      // character key
    U64 actor;    // their actor
    U16 worn_slot; // the slot they wear it in
};
struct Left {
    U16 slot;
    const char* why; // its line: left.storage, ... ({piece}, {who})
    U16 who = 0;     // left.stays_with: whose
};
struct AskedLoad {
    U32 index = 0;
    std::string name; // checked: the list may change while the question is open
    std::vector<std::pair<size_t, U64>> copies;
    std::vector<Take> takes;
    std::vector<Left> left;
    int stage = 0; // the open question: 1 takes, 2 copies
    int confirm = 0; // instead a yes / no from the outfit page: kConfirmOverwrite, kConfirmDelete, kConfirmSwap
    U32 other = 0;          // kConfirmSwap: the outfit worn now (checked by name, as index is)
    std::string other_name;
    bool takes_answered = false, take = false, copies_answered = false, use_copies = false;
};
std::atomic<U64> g_ask_modal{0}; // the open question (0: none)
U64 g_ask_self = 0;
U64 g_ask_opened = 0;
unsigned char g_ask_shown = 0;
AskedLoad g_pending;
enum AskAnswer { kAskAbort, kAskYes, kAskNo };

U64 open_common_modal_raw(U64 self, const char* title, const char* message, const char* yes, const char* no)
{
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        U64 view = ui ? g_find_view(ui, "ModalMessageView") : 0;
        if (!view) return 0;
        unsigned char* p = g_modal_params;
        g_params_init(reinterpret_cast<U64>(p));
        strncpy_s(reinterpret_cast<char*>(p + kParamsTitle), kParamsLabelCap, title, _TRUNCATE);
        *reinterpret_cast<U32*>(p + kParamsTitleLen) = 0xFFFFFFFF;
        strncpy_s(reinterpret_cast<char*>(p + kParamsMessage), kParamsMessageCap, message, _TRUNCATE);
        *reinterpret_cast<U32*>(p + kParamsMessageLen) = 0xFFFFFFFF;
        // (an empty label hides its button)
        strncpy_s(reinterpret_cast<char*>(p + kParamsConfirm), kParamsLabelCap, yes, _TRUNCATE);
        strncpy_s(reinterpret_cast<char*>(p + kParamsCancel), kParamsLabelCap, no, _TRUNCATE);
        U64 modal = g_open_common_modal(0, view, p, reinterpret_cast<const char*>(g_base + kRvaCommonModalName));
        g_params_free(reinterpret_cast<U64>(p));
        if (modal && !has_vt(modal, kRvaCommonModalVt)) return 0; // not the modal we know: leave it alone
        if (modal) *reinterpret_cast<U64*>(modal + kOffModalOwner) = 0; // no owner callback
        return modal;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

U64 call_ask_click_raw(U64 modal, U64 a, U64 elem, U64 flag)
{
    __try {
        return g_ask_click_orig(modal, a, elem, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("missing pieces: game click handler faulted");
        return 0;
    }
}

// "Helmet (Wooden Mask...)": one piece of the outfit.
std::string piece_label(const Outfit& o, U16 slot) { return slot_list(o, {slot}); }

// A piece's new uid, in every outfit of every character (the game gives an item a new uid when it moves to storage).
void follow_piece(U64 from, U64 to, U16 key)
{
    auto follow = [&](std::vector<Outfit>& list) {
        for (Outfit& o : list)
            for (OutfitItem& it : o.items)
                if (it.uid == from && it.key == key) it.uid = to;
    };
    follow(g_outfits);
    std::lock_guard<std::mutex> lock(g_store_mu);
    for (CharacterOutfits& ch : g_store)
        if (ch.character != g_character) follow(ch.outfits);
}

// Loads the outfit as the answers say: copies saved into it, pieces taken from other characters (unequipped from
// them now, put on by pump once they are in the bags), the rest of the left slots untouched; one notification.
void finish_load(U64 self, AskedLoad& p)
{
    if (p.index >= g_outfits.size()) return;
    Busy busy;
    Outfit& o = g_outfits[p.index];
    for (const auto& [k, uid] : p.copies) {
        if (k >= o.items.size()) continue;
        OutfitItem& it = o.items[k];
        if (!p.use_copies) {
            p.left.push_back({it.slot, "left.gone"});
            continue;
        }
        LOG("  equipment-%u: uid %llX -> copy %llX", it.slot, it.uid, uid);
        follow_piece(it.uid, uid, it.key);
    }
    if (p.use_copies && !p.copies.empty()) save_outfits();
    std::vector<U16> keep; // for load_outfit: untouched (a taken piece comes there)
    std::vector<DeferredEquip> equips;
    for (const Take& t : p.takes) {
        if (t.k >= o.items.size()) continue;
        const OutfitItem& it = o.items[t.k];
        if (!p.take) {
            p.left.push_back({it.slot, "left.stays_with", t.who});
            continue;
        }
        keep.push_back(it.slot);
        int err = call_unequip_request(t.actor, t.worn_slot);
        LOG("load \"%s\": taking equipment-%u key %u uid %llX from %s (their slot %u): %s", o.name.c_str(), it.slot,
            it.key, it.uid, character_name(t.who), t.worn_slot, error_text(err).c_str());
        equips.push_back({it.uid, it.slot, it.key, piece_label(o, it.slot), character_name(t.who)});
    }
    std::vector<U16> leave; // the outfit's piece can't come there
    for (const Left& l : p.left) leave.push_back(l.slot);
    g_async = {};
    if (!equips.empty()) g_async = {o.name, {}, std::move(equips), GetTickCount64(), true};
    load_outfit(self, p.index, leave, keep);
    if (g_async.active) {
        g_async.leave.clear();
        for (const Left& l : p.left) g_async.leave.push_back(l.slot); // (taken slots are checked)
    }
    if (!p.left.empty()) {
        std::string msg;
        for (const Left& l : p.left)
            msg += (msg.empty() ? std::string() : lang::f("list.separator2")) +
                   lang::f(l.why, {{"piece", piece_label(o, l.slot)}, {"who", character_name(l.who)}});
        report(self, lang::f("report.left", {{"outfit", o.name}, {"list", msg}}));
    }
    refresh_wheel(self);
}

enum Confirm { kConfirmNone, kConfirmOverwrite, kConfirmDelete, kConfirmSwap, kConfirmCleanSave, kConfirmUninstall };

// --- Settings view: an outfit's settings (or the plugin's, from the "+" slot) on the wheel ---------------------------
// The second scroll mode of a slot opens it (no submenu). Its segments: a setting's name and value (its dots are the
// choices: scroll to change) or an action (the click prompt), then Back (left of the top). The centre names the
// highlighted setting, its value and what it does.

bool g_overwrite_reset = false; // the settings view's Overwrite: false keeps the outfit's settings, true resets them

enum CfgKind : std::uint8_t {
    kCfgVisibility, kCfgClimate, kCfgWeapons, kCfgStash, kCfgPosition, kCfgRename, kCfgOverwrite, kCfgDelete, // a saved outfit's
    kCfgGStash, kCfgParty, kCfgMessages, kCfgClear, kCfgUninstall,                               // the "+" slot's
    kCfgBack, kCfgLanguage, kCfgPeekKey, kCfgPeekPad, kCfgHotkey
};

// The Outfit Arc's key (the "+" page's Outfit Arc; Off by default: no key is free in every layout, so the player sets
// one). A click on the setting waits for the next key or mouse button (left / right excepted: they click), read with
// GetAsyncKeyState so it works however the game reads its input; Esc cancels, Backspace / Delete turn it off. Held: the
// arc shows; the wheel moves along it; released: the highlighted outfit goes on. The key and, while the arc is open, the
// wheel are kept from the game (raw_input_hook, name_wndproc).
std::atomic<bool> g_peek_capture{false}; // the setting waits for a key
int g_capture_outfit = -1;               // ... for that outfit's Hotkey (-1: for the Outfit Arc's key)
U32 g_peek_capture_slot = 0;             // its slot (refreshed when the key comes)
bool g_peek_capture_held[256] = {};      // keys already down when it started waiting (not taken as the answer)

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

std::vector<std::uint8_t> cfg_entries()
{
    if (g_cfg < 0) return {};
    if (size_t(g_cfg) < g_outfits.size())
        return {kCfgVisibility, kCfgClimate, kCfgWeapons, kCfgStash, kCfgHotkey, kCfgPosition, kCfgRename, kCfgOverwrite,
                kCfgDelete, kCfgBack};
    // (the Outfit Arc's binding for the input in use: the key, or the pad's; pump re-shows the page when it changes)
    return {kCfgGStash, kCfgParty, kCfgMessages, kCfgLanguage, g_pad_in_use ? kCfgPeekPad : kCfgPeekKey, kCfgClear,
            kCfgUninstall, kCfgBack};
}

const char* cfg_name(std::uint8_t k)
{
    static const char* const n[] = {"setting.visibility", "setting.climate", "setting.weapons", "setting.stash",
                                    "setting.position",   "setting.rename",  "setting.overwrite", "setting.delete",
                                    "setting.stash",      "setting.party",   "setting.messages", "setting.clear",
                                    "setting.uninstall",  "setting.back",    "setting.language", "setting.peekkey",
                                    "setting.peekpad",    "setting.hotkey"};
    return k <= kCfgHotkey ? lang::t(n[k]) : "";
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
            cur = uses_weapons(o) ? 0 : 1;
            return {"value.load", "value.leave"};
        }
        if (k == kCfgStash) {
            cur = o.stash % 3;
            return {"value.default", "value.on", "value.off"};
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
            o.load_weapons = to == 0;
        } else if (e[k] == kCfgStash) {
            o.stash = static_cast<std::uint8_t>(to);
        }
        LOG("settings \"%s\": %s -> %s", o.name.c_str(), cfg_name(e[k]), v[to]);
    } else {
        if (e[k] == kCfgGStash) g_stash = to == 1;
        else if (e[k] == kCfgParty) g_take_from_companions = to == 1;
        else if (e[k] == kCfgMessages) g_messages = to == 1;
        else if (e[k] == kCfgPeekPad) g_peek_pad = to == 1;
        else if (e[k] == kCfgLanguage) {
            g_language = to == 0 ? std::string() : std::string(v[to]);
            apply_language();
            if (U64 h = outfits_header(self)) set_label(h); // (the tab's name)
        }
        LOG("settings: %s -> %s", cfg_name(e[k]), v[to]);
    }
    save_outfits();
    apply_outfit_slots(self, i);
    center_for_focus(self, focused_script(self), true);
}
void open_clean_confirm(U64 self, int what);
void open_confirm(U64 self, U32 index, int what, U32 other = 0);
void overwrite_pressed(U64 self, U32 index, bool reset);

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

bool g_ask_next = false; // the next question of g_pending opens on the next pump (not inside a modal's click)
void clean_outfits(U64 self);
void confirmed(U64 self, U32 index, int what, U32 other);
void uninstall(U64 self);

void finish_ask(int answer, const char* why)
{
    if (!g_ask_modal) return;
    g_ask_modal = 0;
    g_modal_ended = GetTickCount64();
    g_release_pending = g_menu_open;
    AskedLoad p = std::move(g_pending);
    g_pending = {};
    bool here = p.index < g_outfits.size() && g_outfits[p.index].name == p.name;
    LOG("question %d: %s (\"%s\"%s)", p.stage, why, p.name.c_str(), here ? "" : ", outfit gone");
    if (p.confirm == kConfirmCleanSave || p.confirm == kConfirmUninstall) { // (about no one outfit)
        if (answer != kAskYes || !g_menu_open) return;
        if (p.confirm == kConfirmCleanSave) clean_outfits(g_ask_self);
        else uninstall(g_ask_self);
        return;
    }
    bool from_wheel = g_ask_self == g_outfits_root; // (or the quick peek: asked in the game, no wheel to close)
    if (!here || answer == kAskAbort || (from_wheel && !g_menu_open)) return;
    if (p.confirm) {
        bool other_here = p.confirm != kConfirmSwap ||
                          (p.other < g_outfits.size() && g_outfits[p.other].name == p.other_name);
        if (answer == kAskYes && other_here) confirmed(g_ask_self, p.index, p.confirm, p.other);
        return;
    }
    if (p.stage == 1) p.takes_answered = true, p.take = answer == kAskYes;
    else p.copies_answered = true, p.use_copies = answer == kAskYes;
    if (p.stage == 1 && !p.copies.empty()) { // the copies question next (opened by pump)
        g_release_pending = false;
        g_pending = std::move(p);
        g_ask_next = true;
        return;
    }
    finish_load(g_ask_self, p);
}

void abort_prompts(const char* why)
{
    if (g_name_modal) {
        hide_modal_raw(g_name_modal);
        finish_naming("", why);
    }
    if (g_ask_modal) {
        hide_modal_raw(g_ask_modal);
        finish_ask(kAskAbort, why);
    }
    if (g_ask_next) {
        g_ask_next = false;
        g_pending = {};
        LOG("questions: %s; nothing loaded", why);
    }
}

// Slot 113 of the CommonModal (its button / key handler): only our question is handled here. Without an owner the game
// stores no answer, so the clicked button tells it.
U64 __fastcall ask_click_hook(U64 modal, U64 a, U64 elem, U64 flag)
{
    if (!g_ask_modal || modal != g_ask_modal || (flag & 0xFF)) return g_ask_click_orig(modal, a, elem, flag);
    bool yes = elem && elem == rp(modal + kOffModalConfirm), no = elem && elem == rp(modal + kOffModalCancel);
    bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0; // Esc may arrive as the cancel button: it loads nothing
    LOG("question: click elem %llX (confirm %d, cancel %d, Esc %d)", elem, yes, no, esc);
    U64 r = call_ask_click_raw(modal, a, elem, flag);
    if (esc) finish_ask(kAskAbort, "Esc; nothing loaded");
    else if (yes || no) finish_ask(yes ? kAskYes : kAskNo, yes ? "yes" : "leave as is");
    return r;
}

// Opens the next question of a load (pieces on other characters, then copies), or loads.
void ask_next(U64 self, AskedLoad p)
{
    if (p.index >= g_outfits.size()) return;
    const Outfit& o = g_outfits[p.index];
    std::string msg, yes;
    if (!p.takes.empty() && !p.takes_answered) {
        p.stage = 1;
        for (const Take& t : p.takes)
            msg += (msg.empty() ? std::string() : lang::f("list.separator")) +
                   lang::f("ask.take_item", {{"piece", piece_label(o, o.items[t.k].slot)}, {"who", character_name(t.who)}});
        long long n = static_cast<long long>(p.takes.size());
        msg = lang::n("ask.take", n, {{"piece", piece_label(o, o.items[p.takes[0].k].slot)},
                                      {"who", character_name(p.takes[0].who)}, {"list", msg}});
        yes = lang::n("ask.take_yes", n);
    } else if (!p.copies.empty() && !p.copies_answered) {
        p.stage = 2;
        std::vector<U16> copy_slots;
        for (const auto& [k, uid] : p.copies) copy_slots.push_back(o.items[k].slot);
        long long n = static_cast<long long>(p.copies.size());
        msg = lang::n("ask.copy", n, {{"list", slot_list(o, copy_slots)}});
        yes = lang::n("ask.copy_yes", n);
    } else {
        finish_load(self, p);
        return;
    }
    U64 modal = g_open_common_modal ? open_common_modal_raw(self, o.name.c_str(), msg.c_str(), yes.c_str(), lang::t("ask.leave")) : 0;
    if (!modal) {
        LOG("question %d did not open; those slots stay as they are", p.stage);
        if (p.stage == 1) p.takes_answered = true;
        else p.copies_answered = true;
        ask_next(self, std::move(p));
        return;
    }
    g_pending = std::move(p);
    g_ask_self = self;
    g_ask_opened = GetTickCount64();
    rd(modal + kOffModalShown, g_ask_shown);
    g_ask_modal = modal;
    LOG("question %d %llX open (+0xbd %u): %s", g_pending.stage, modal, g_ask_shown, msg.c_str());
}

// Space / click on a saved outfit: where is each piece? Then asks (pieces on other characters, copies of gone ones) or
// loads.
void start_loading(U64 self, U32 index)
{
    if (g_ask_modal || g_ask_next) return;
    const Outfit& o = g_outfits[index];
    std::vector<Owned> owned;
    if (!read_owned(self, owned)) {
        LOG("load \"%s\": inventory unreadable; loading without the checks", o.name.c_str());
        load_outfit(self, index);
        return;
    }
    if (adopt_returned(g_outfits[index], owned)) save_outfits();
    AskedLoad p;
    p.index = index;
    p.name = o.name;
    bool main = playing_main_character();
    for (size_t k = 0; k < o.items.size(); ++k) {
        const OutfitItem& it = o.items[k];
        if (!loads_piece(o, it.slot)) continue; // (Weapons: Leave)
        const Owned* x = find_owned(owned, it.uid, it.key);
        if (!x) continue; // gone: copies / not in the inventory (below)
        const char* why = nullptr;
        switch (x->where) {
        case Where::Worn: break;
        case Where::Carried: break;
        case Where::Storage:
            if (!(equip_bags(!main) & bag_bit(kBagStorage))) why = "left.storage";
            break;
        case Where::Wardrobe:
            if (!(equip_bags(!main) & bag_bit(kBagWardrobe))) why = "left.wardrobe";
            break;
        case Where::Other: why = "left.gone"; break;
        case Where::Companion: p.takes.push_back({k, x->who, x->actor, x->slot}); break;
        }
        if (why) p.left.push_back({it.slot, why});
        LOG("load \"%s\": equipment-%u key %u uid %llX: %s%s%s", o.name.c_str(), it.slot, it.key, it.uid,
            x->where == Where::Worn ? "worn" : x->where == Where::Carried ? "carried" : x->where == Where::Storage ? "storage"
            : x->where == Where::Wardrobe ? "wardrobe" : x->where == Where::Other ? "other bag" : "worn by ",
            x->where == Where::Companion ? character_name(x->who) : "", why ? " (left as is)" : "");
    }
    PieceCheck c = check_pieces(o, owned);
    bool followed = false;
    for (const auto& [k, uid] : c.copies) {
        int same = 0; // (the only one owned: the same item under a new uid, nothing to ask)
        for (const Owned& x : owned) same += x.key == o.items[k].key;
        LOG("load \"%s\": equipment-%u key %u uid %llX is gone; copy %llX owned%s", o.name.c_str(), o.items[k].slot,
            o.items[k].key, o.items[k].uid, uid, same == 1 ? ": followed" : "");
        if (same == 1) follow_piece(o.items[k].uid, uid, o.items[k].key), followed = true;
        else p.copies.push_back({k, uid});
    }
    if (followed) save_outfits();
    for (size_t k : c.gone) {
        p.left.push_back({o.items[k].slot, "left.gone"});
        LOG("load \"%s\": equipment-%u key %u uid %llX is gone; no copy", o.name.c_str(), o.items[k].slot,
            o.items[k].key, o.items[k].uid);
    }
    // Most of the outfit can't be had (sold, or item data that no longer fits this save): loading would only strip the
    // character. A few gone pieces load as before: the rest of the outfit goes on, those slots are left.
    size_t pieces = 0;
    for (const OutfitItem& it : o.items) pieces += loads_piece(o, it.slot) && it.key != 0xFFFF;
    if (pieces && c.gone.size() * 2 > pieces) {
        LOG("load \"%s\": %zu of %zu pieces are gone; not loaded", o.name.c_str(), c.gone.size(), pieces);
        notify(self, lang::f("toast.not_loaded_missing", {{"outfit", o.name}, {"n", std::to_string(c.gone.size())},
                                                          {"total", std::to_string(pieces)}}));
        return;
    }
    if (!p.takes.empty() && !g_take_from_companions) { // rule: the outfit does not load; say who has the pieces
        std::string msg;
        for (const Take& t : p.takes)
            msg += (msg.empty() ? std::string() : lang::f("list.separator2")) +
                   lang::f("left.worn_by", {{"piece", piece_label(o, o.items[t.k].slot)}, {"who", character_name(t.who)}});
        LOG("load \"%s\": pieces on other characters and taking them is off; not loaded", o.name.c_str());
        notify(self, lang::f("toast.not_loaded", {{"outfit", o.name}, {"list", msg}}));
        return;
    }
    ask_next(self, std::move(p));
}

// pump: the next question of a load, and the asynchronous parts of the last one.

// The next question of a load (opened a moment after the last one closed).
void pump_ask(U64 self)
{
    if (g_ask_next && !g_ask_modal && !g_name_modal && GetTickCount64() - g_modal_ended > 150) {
        g_ask_next = false;
        AskedLoad p = std::move(g_pending);
        g_pending = {};
        if (p.index < g_outfits.size() && g_outfits[p.index].name == p.name) ask_next(self, std::move(p));
    }
}
void lost_check(U64 self, const std::string& outfit, const std::vector<LeavingPiece>& leaving);
void stash_pieces(U64 self, const Outfit* loaded);

void pump_async(U64 self)
{
    pump_ask(self);
    if (!g_async.active) return;
    // (the new pieces reach the inventory a few frames after the requests, their model parts a little later: a look
    // set in between is queued on the still-loading part and lands with it, so nothing is drawn before it is hidden)
    look_refresh();
    U64 now = GetTickCount64();
    auto it = std::find_if(g_outfits.begin(), g_outfits.end(), [](const Outfit& o) { return o.name == g_async.outfit; });
    if (it == g_outfits.end()) {
        g_async = {};
        return;
    }
    bool late = now - g_async.started > kAsyncMs;
    bool acted = false; // a late put-on ran this frame: checked once the game applied it (a later frame)
    for (size_t i = 0; i < g_async.equips.size();) {
        DeferredEquip& d = g_async.equips[i];
        if (d.held) { // (the slot's holder is the game's state, read every frame: on as soon as it is free)
            if (slot_claim(d.slot) && !late) {
                ++i;
                continue;
            }
            if (!late) run_step({true, d.slot, d.key, d.uid}, 0), acted = true;
            if (late || g_step_err) {
                LOG("load \"%s\": %s still refused (%s)", g_async.outfit.c_str(), d.what.c_str(),
                    late ? "its slot stayed held" : error_text(g_step_err).c_str());
                report(self, lang::f("report.not_put_on", {{"outfit", g_async.outfit}, {"piece", d.what}}));
            } else {
                LOG("load \"%s\": %s went on once its slot was free", g_async.outfit.c_str(), d.what.c_str());
            }
            g_async.started = now;
            g_async.equips.erase(g_async.equips.begin() + i);
            continue;
        }
        U64 ctx[4] = {};
        U64 actor = acquire_player(ctx);
        U64 mgr = actor ? call_inventory(actor) : 0;
        ItemAt at{};
        bool there = mgr && find_in_bags(mgr, d.uid, at, bag_bit(kBagCarried) | bag_bit(kBagKeyItems), d.key);
        if (ctx[0]) release_player(ctx);
        if (there) {
            LOG("load \"%s\": %s from %s is in the bags now", g_async.outfit.c_str(), d.what.c_str(), d.from.c_str());
            run_step({true, d.slot, d.key, d.uid}, 0);
            acted = true;
            g_async.started = now; // (the check gets its own window: a late arrival must not be checked at once)
            g_async.equips.erase(g_async.equips.begin() + i);
        } else if (late) {
            LOG("load \"%s\": %s did not arrive from %s", g_async.outfit.c_str(), d.what.c_str(), d.from.c_str());
            report(self, lang::f("report.not_taken", {{"outfit", g_async.outfit}, {"piece", d.what}, {"who", d.from}}));
            g_async.equips.erase(g_async.equips.begin() + i);
        } else {
            ++i;
        }
    }
    if (!g_async.equips.empty() || acted) return;
    if (verify_loading(self, *it, g_async.leave, late) || late) {
        lost_check(self, g_async.outfit, g_async.leaving);
        g_async = {};
        look_settled();
        stash_pieces(self, &*it);
    }
}

// After a load: every piece it took off or replaced must still be somewhere (worn, a bag, a companion). One that is
// nowhere was lost by the game on the way to the bag (2026-11-06, a player's log: a piece swapped out by a load
// vanished; it couldn't be spawned again and came back after a restart, so the game still had it, in no bag). Named
// in the log, and the player is told: restarting without saving brings it back.
void lost_check(U64 self, const std::string& outfit, const std::vector<LeavingPiece>& leaving)
{
    if (leaving.empty()) return;
    std::vector<Owned> owned;
    if (!read_owned(self, owned)) {
        LOG("load \"%s\": loss check skipped (inventory unreadable)", outfit.c_str());
        return;
    }
    int lost = 0;
    U64 dctx[4] = {}; // DIAG (lost pieces): where each piece that left ended up
    U64 dactor = acquire_player(dctx);
    U64 dmgr = dactor ? call_inventory(dactor) : 0;
    if (dctx[0]) release_player(dctx);
    for (const LeavingPiece& p : leaving) diag_where(dmgr, "after the load", p.uid, p.key);
    for (const LeavingPiece& p : leaving) {
        bool found = false;
        for (const Owned& x : owned) found |= x.uid == p.uid && x.key == p.key;
        if (found) continue;
        ++lost;
        LOG("load \"%s\": LOST: key %u \"%s\" uid %llX left equipment-%u and is in no bag; the game still holds it "
            "(restart without saving)", outfit.c_str(), p.key, item_name(p.key).c_str(), p.uid, p.slot);
        notify(self, lang::f("toast.piece_lost", {{"piece", item_name(p.key)}}));
    }
    LOG("load \"%s\": loss check: %zu piece(s) left their slots, %d lost", outfit.c_str(), leaving.size(), lost);
}

// --- Stash: after a load, every saved piece (any outfit, any character) left in the carried bag goes to storage,
// through the storage screen's own move (as when the player moves it there; works anywhere). The piece gets a new uid
// there; stash_verify follows it exactly. Key items (bag 4) stay: the game keeps them on you.
int stash_rule(U16 from, U16 to) // the source bag's transfer rule to `to` (-1: none, e.g. no wardrobe yet)
{
    __try {
        U64 info = g_bag_info(0, from);
        U64 rules = info ? *reinterpret_cast<U64*>(info + 0x38) : 0;
        U32 n = info ? *reinterpret_cast<U32*>(info + 0x40) : 0;
        for (U32 i = 0; rules && i < n && i < 256; ++i) {
            U64 r = rules + i * 0xa0ull;
            if (*reinterpret_cast<char*>(r) != 3 && *reinterpret_cast<U16*>(r + 2) == from &&
                *reinterpret_cast<U16*>(r + 4) == to)
                return static_cast<int>(i);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

int call_item_move(U64 mgr, U32 owner, const ItemAt& at, U32 rule)
{
    int err = kFault;
    __try {
        short key = *reinterpret_cast<short*>(at.item + 8);
        U16 extra = *reinterpret_cast<U16*>(at.item + 0xa);
        g_item_move(mgr, &err, owner, owner, key, extra, 1, at.bag, static_cast<U16>(at.pos), rule);
    } __except (note_fault(GetExceptionInformation())) {
        err = kFault;
    }
    return err;
}
StashCheck g_stash_check;
constexpr U64 kStashCheckMs = 1500; // (only the give-up: a move counts when its piece is found in storage)

bool stash_pending(U64 uid)
{
    if (!g_stash_check.at) return false;
    for (const StashSent& s : g_stash_check.sent)
        if (s.uid == uid) return true;
    return false;
}

std::string pieces_text(int n) { return lang::n("count.pieces", n); }

// Every frame after the requests (from the HUD's frame too, so also with the wheel closed) until each piece is in
// storage (a few frames). A piece still in the bag at the give-up was refused (the game says why itself).
void stash_verify(U64 self)
{
    if (!g_stash_check.at) return;
    bool late = GetTickCount64() - g_stash_check.at >= kStashCheckMs;
    const StashCheck& c = g_stash_check;
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 mgr = actor ? call_inventory(actor) : 0;
    std::vector<Owned> now(4096, Owned{0, 0xFFFF, Where::Other});
    int n = mgr ? read_bags_raw(mgr, now.data(), static_cast<int>(now.size())) : -1;
    now.resize(n > 0 ? n : 0);
    if (ctx[0]) release_player(ctx);
    int moved = 0, left = 0, pending = 0;
    std::vector<std::pair<U64, U64>> renamed; // old uid -> new uid
    std::vector<U64> claimed;
    for (const StashSent& s : c.sent) {
        bool found = false;
        for (const Owned& x : now)
            if (x.key == s.key && x.where == Where::Storage && x.uid != s.uid &&
                std::find(c.before.begin(), c.before.end(), x.uid) == c.before.end() &&
                std::find(claimed.begin(), claimed.end(), x.uid) == claimed.end()) {
                claimed.push_back(x.uid);
                renamed.push_back({s.uid, x.uid});
                found = true;
                break;
            }
        if (found) ++moved;
        else if (late) ++left;
        else ++pending;
    }
    if (pending) return; // (still on the way: next frame)
    g_stash_check.at = 0; // (done: stash_pending is off from here)
    if (!mgr) return;
    if (!renamed.empty()) { // every outfit using the piece, every character
        int uses = 0;
        auto follow = [&](std::vector<Outfit>& list) {
            for (Outfit& o : list)
                for (OutfitItem& it : o.items)
                    for (const auto& [from, to] : renamed)
                        if (it.uid == from) it.uid = to, ++uses;
        };
        follow(g_outfits);
        {
            std::lock_guard<std::mutex> lock(g_store_mu);
            for (CharacterOutfits& ch : g_store)
                if (ch.character != g_character) follow(ch.outfits);
        }
        for (const auto& [from, to] : renamed) LOG("stash: uid %llX is now %llX", from, to);
        LOG("stash: %zu pieces followed (%d outfit slots updated)", renamed.size(), uses);
        save_outfits();
    }
    LOG("stash: %d put in storage, %d not", moved, left);
    if (moved && left) report(self, lang::f("report.stash.moved_left", {{"moved", pieces_text(moved)}, {"left", pieces_text(left)}}));
    else if (moved) report(self, lang::f("report.stash.moved", {{"moved", pieces_text(moved)}}));
    else if (left) report(self, lang::f("report.stash.left", {{"left", pieces_text(left)}}));
}

// Each saved piece and whether it goes away: only when every outfit using it has Stash on (one with it off keeps the
// piece in the bag: Off wins, so a piece shared with an outfit kept at hand stays at hand).
struct StashDest {
    U64 uid;
    U16 key;
    bool on; // goes away (every outfit using it has Stash on)
};
std::vector<StashDest> stash_destinations()
{
    std::vector<StashDest> out;
    auto add = [&](const std::vector<Outfit>& list) {
        for (const Outfit& o : list)
            for (const OutfitItem& it : o.items) {
                if (!loads_piece(o, it.slot)) continue; // (a weapon it leaves: not its business)
                bool on = stash_of(o);
                auto f = std::find_if(out.begin(), out.end(),
                                      [&](const StashDest& e) { return e.uid == it.uid && e.key == it.key; });
                if (f == out.end()) out.push_back({it.uid, it.key, on});
                else f->on = f->on && on;
            }
    };
    add(g_outfits);
    std::lock_guard<std::mutex> lock(g_store_mu);
    for (const CharacterOutfits& c : g_store)
        if (c.character != g_character) add(c.outfits);
    return out;
}

// After a load: saved pieces left in the carried bag go to storage, through the storage screen's own move (it works
// anywhere). Loading takes pieces from the bag, storage and the wardrobe alike. (The wardrobe was tried and dropped: away
// from the house the game counts only its base 10 slots, checked twice, once on its own side - 2026-10-27 tests.)
void stash_pieces(U64 self, const Outfit* loaded)
{
    if (!g_item_move || !g_bag_info) return;
    std::vector<StashDest> dest = stash_destinations();
    if (loaded) // (the outfit just loaded: its pieces are meant to be worn, even one still on its way to its slot)
        for (StashDest& d : dest)
            for (const OutfitItem& it : loaded->items)
                if (it.uid == d.uid && it.key == d.key) d.on = false;
    if (std::none_of(dest.begin(), dest.end(), [](const StashDest& e) { return e.on; })) return;
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 mgr = actor ? call_inventory(actor) : 0;
    U32 owner = actor ? r32(actor + 0x60) : 0; // (the storage screen passes the controlled character for both bags)
    int rule = mgr ? stash_rule(kBagCarried, kBagStorage) : -1;
    int failed = 0, err = 0;
    StashCheck check;
    {
        std::vector<Owned> held(4096, Owned{0, 0xFFFF, Where::Other});
        int n = mgr ? read_bags_raw(mgr, held.data(), static_cast<int>(held.size())) : -1;
        for (int i = 0; i < n; ++i)
            if (held[i].where == Where::Storage) check.before.push_back(held[i].uid);
    }
    for (const auto& [uid, key, on] : dest) {
        ItemAt at{};
        if (!on || rule < 0 || !find_in_bags(mgr, uid, at, bag_bit(kBagCarried), key)) continue;
        int e = call_item_move(mgr, owner, at, static_cast<U32>(rule));
        LOG("stash: uid %llX (key %u \"%s\") bag %u pos %d -> storage (rule %d, owner %08X): %s", uid, key,
            item_name(key).c_str(), at.bag, at.pos, rule, owner, error_text(e).c_str());
        if (!e) check.sent.push_back({uid, key});
        else ++failed, err = e;
    }
    if (ctx[0]) release_player(ctx);
    if (rule < 0 && mgr) {
        LOG("stash: no transfer rule from the bag to storage");
        return;
    }
    if (failed) // (refused at once; the ones sent are checked by stash_verify)
        report(self, lang::f("report.stash.failed", {{"pieces", pieces_text(failed)}, {"error", error_text(err)}}));
    if (!check.sent.empty()) {
        check.at = GetTickCount64();
        g_stash_check = std::move(check);
    }
}

// While the question is open: notices it closing some other way (Esc, a click outside) or a 3-minute safety stop.
void watch_ask()
{
    if (!g_ask_modal) return;
    unsigned char shown = g_ask_shown;
    rd(g_ask_modal + kOffModalShown, shown);
    if (g_ask_shown && !shown) finish_ask(kAskAbort, "closed without an answer; nothing loaded");
    else if (GetTickCount64() - g_ask_opened > 180000) {
        hide_modal_raw(g_ask_modal);
        finish_ask(kAskAbort, "timed out; nothing loaded");
    }
}

// The "+" slot pressed: save the worn armor as a new outfit, unless an identical one exists (then names it).
void save_new_outfit(U64 self)
{
    if (g_uninstalled) {
        notify(self, lang::t("toast.uninstalled"));
        return;
    }
    if (g_outfits.size() >= kMaxOutfits) {
        notify(self, lang::f("toast.limit", {{"n", std::to_string(kMaxOutfits)}}));
        return;
    }
    Outfit o;
    if (!snapshot(self, o)) return;
    for (const Outfit& s : g_outfits)
        if (same_saved(s, o)) {
            notify(self, lang::f("toast.already_saved", {{"outfit", s.name}}));
            return;
        }
    o.name = default_name();
    if (int a = anchor_outfit(); a >= 0) carry_looks(g_outfits[size_t(a)], o); // (a changed outfit saved as new looks the same)
    g_outfits.push_back(o);
    g_outfit_count = static_cast<U32>(g_outfits.size());
    LOG("saved \"%s\" (%zu items, chest %s)", o.name.c_str(), o.items.size(),
        o.chest_icon.empty() ? "none" : o.chest_icon.c_str());
    save_outfits();
    refresh_wheel(self);
    center_for_focus(self, focused_script(self), true);
    open_rename_prompt(self, g_outfits.size() - 1);
}

// Delete (the outfit page, confirmed): the later outfits move up and the wheel shrinks.
void delete_outfit(U64 self, U32 o)
{
    LOG("deleted \"%s\"", g_outfits[o].name.c_str());
    g_outfits.erase(g_outfits.begin() + o);
    g_outfit_count = static_cast<U32>(g_outfits.size());
    save_outfits();
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
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

// Clean up: this save's outfits, i.e. the current outfits of every character and the copies of the save loaded last
// (so loading it again does not bring them back; the other saves keep theirs, as with any game state).
void clean_outfits(U64 self)
{
    size_t gone = 0, saves = 0;
    {
        std::lock_guard<std::mutex> lock(g_store_mu);
        if (g_generation.load() != g_seen_generation) {
            LOG("clean up dropped: a save was loaded meanwhile");
            return;
        }
        store_for(g_character).outfits = g_outfits;
        gone = outfit_total(g_store);
        g_store.clear();
        g_published.clear();
        for (auto& [slot, b] : g_bound)
            if (g_loaded_known && b.id.same(g_loaded_id)) {
                if (!b.store.empty()) ++saves;
                b.store.clear();
            }
        bool ok = write_store_file();
        LOG("clean up: %zu current outfits and the copies of %zu saves deleted%s", gone, saves,
            ok ? "" : " (writing FAILED)");
    }
    g_outfits.clear();
    g_outfit_count = 0;
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
    notify(self, lang::t("toast.cleared"));
}

// Uninstall: every file the mod keeps (the outfits of every save and the settings, the address cache, the log, a
// write's leftover) is deleted, so removing the mod in DMM leaves nothing behind. The mod never writes into game saves.
// It stays loaded until the game closes, inert: nothing it does writes a file again.
bool g_uninstalled = false;
std::wstring g_log_path;

void uninstall(U64 self)
{
    {
        std::lock_guard<std::mutex> lock(g_store_mu);
        g_uninstalled = true;
        g_store.clear(), g_published.clear(), g_bound.clear(), g_pending_writes.clear(), g_unfinished.clear();
    }
    g_outfits.clear();
    g_outfit_count = 0;
    LOG("uninstall: deleting the outfit file, the cache and this log; the mod stays inert until the game closes");
    logx::close();
    int failed = 0;
    for (const std::wstring& f : {g_store_path, g_store_path + L".tmp", g_cache_path, g_log_path})
        if (!f.empty() && !DeleteFileW(f.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) ++failed;
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
    notify(self, lang::t(failed ? "toast.remove_failed" : "toast.removed"));
}

// The Clean up and Uninstall questions: a deleting button and Cancel (the dialog's second button is Esc's: Esc always
// cancels).
void open_clean_confirm(U64 self, int what)
{
    if (g_ask_modal || g_ask_next) return;
    std::string msg, yes;
    if (what == kConfirmCleanSave) {
        size_t total = 0;
        std::string who;
        store_for(g_character); // (the current character counts from its working list, g_outfits)
        for (const CharacterOutfits& c : g_store) {
            size_t n = c.character == g_character ? g_outfits.size() : c.outfits.size();
            if (!n) continue;
            total += n;
            who += (who.empty() ? std::string() : lang::f("list.separator")) +
                   lang::f("ask.clear_who", {{"character", c.character == kNoCharacter ? lang::t("character.unassigned")
                                                                                        : character_name(c.character)},
                                             {"n", std::to_string(n)}});
        }
        if (!total) {
            notify(self, lang::t("toast.nothing_to_clear"));
            return;
        }
        msg = lang::f("ask.clear", {{"n", std::to_string(total)}, {"who", who}});
        yes = lang::t("ask.yes.delete");
    } else {
        msg = lang::t("ask.uninstall");
        yes = lang::t("ask.yes.remove");
    }
    const char* title = lang::t(what == kConfirmCleanSave ? "setting.clear" : "setting.uninstall");
    U64 modal = g_open_common_modal ? open_common_modal_raw(self, title, msg.c_str(), yes.c_str(), lang::t("ask.cancel")) : 0;
    if (!modal) {
        LOG("clean up: the dialog did not open; nothing deleted");
        return;
    }
    AskedLoad p;
    p.confirm = what;
    g_pending = std::move(p);
    g_ask_self = self;
    g_ask_opened = GetTickCount64();
    rd(modal + kOffModalShown, g_ask_shown);
    g_ask_modal = modal;
    LOG("clean up question %d %llX open: %s", what, modal, msg.c_str());
}

// Overwrite / Delete / Swap ask first, in the game's yes / no dialog (the loading questions' machinery: finish_ask
// answers). other: kConfirmSwap's second outfit; kConfirmOverwrite: 1 resets the outfit's settings, 0 keeps them.
void open_confirm(U64 self, U32 index, int what, U32 other)
{
    if (g_ask_modal || g_ask_next || index >= g_outfits.size()) return;
    if (what == kConfirmSwap && other >= g_outfits.size()) return;
    const std::string& name = g_outfits[index].name;
    std::string msg = what == kConfirmOverwrite ? lang::f(other ? "ask.overwrite.reset" : "ask.overwrite.keep", {{"outfit", name}})
                      : what == kConfirmDelete  ? lang::f("ask.delete", {{"outfit", name}})
                                                : lang::f("ask.swap", {{"outfit", name}, {"worn", g_outfits[other].name}});
    const char* yes = lang::t(what == kConfirmOverwrite ? "ask.yes.overwrite" : what == kConfirmDelete ? "ask.yes.delete" : "ask.yes.swap");
    U64 modal = g_open_common_modal ? open_common_modal_raw(self, name.c_str(), msg.c_str(), yes, lang::t("ask.cancel")) : 0;
    if (!modal) {
        LOG("confirm %d for \"%s\": the dialog did not open; nothing done", what, name.c_str());
        return;
    }
    AskedLoad p;
    p.index = index, p.name = name, p.confirm = what;
    p.other = other; // (Swap: the other outfit; Overwrite: reset or keep)
    if (what == kConfirmSwap) p.other_name = g_outfits[other].name;
    g_pending = std::move(p);
    g_ask_self = self;
    g_ask_opened = GetTickCount64();
    rd(modal + kOffModalShown, g_ask_shown);
    g_ask_modal = modal;
    LOG("confirm %d %llX open: %s", what, modal, msg.c_str());
}

void confirmed(U64 self, U32 index, int what, U32 other)
{
    if (index >= g_outfits.size()) return;
    if (what == kConfirmDelete) {
        delete_outfit(self, index);
        return;
    }
    if (what == kConfirmSwap) {
        if (other >= g_outfits.size() || other == index) return;
        std::swap(g_outfits[index], g_outfits[other]);
        LOG("Swap: \"%s\" and \"%s\" changed slots", g_outfits[other].name.c_str(), g_outfits[index].name.c_str());
        save_outfits();
        refresh_wheel(self);
        center_restore();
        center_for_focus(self, focused_script(self), true);
        report(self, lang::f("report.swapped", {{"a", g_outfits[other].name}, {"b", g_outfits[index].name}}));
        return;
    }
    Outfit o;
    if (!snapshot(self, o)) return;
    for (size_t j = 0; j < g_outfits.size(); ++j)
        if (same_saved(g_outfits[j], o)) {
            notify(self, lang::f(j == index ? "toast.already_holds" : "toast.already_saved", {{"outfit", g_outfits[j].name}}));
            return;
        }
    const Outfit& was = g_outfits[index];
    o.name = was.name;
    o.worn = was.worn; // (whether it is the last loaded one: not a setting)
    bool keep = other == 0;
    if (keep) { // (its settings and looks stay; reset: as newly saved)
        if (was.has_weapons) o.load_weapons = was.load_weapons;
        o.climate = was.climate, o.stash = was.stash;
        o.hk_vk = was.hk_vk, o.hk_mods = was.hk_mods;
        carry_looks(was, o);
    }
    g_outfits[index] = o;
    LOG("Overwrite (%s): \"%s\" now holds %zu items (chest %s)", keep ? "settings kept" : "settings reset",
        o.name.c_str(), o.items.size(), o.chest_icon.empty() ? "none" : o.chest_icon.c_str());
    save_outfits();
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
    report(self, lang::f("report.now_holds", {{"outfit", o.name}}));
}

// Overwrite: what is worn decides the question. Already this outfit: said so; another saved outfit: offer to swap
// their slots; anything else: confirm the overwrite.
void overwrite_pressed(U64 self, U32 index, bool reset)
{
    Outfit o;
    if (index >= g_outfits.size() || !snapshot(self, o)) return;
    for (size_t j = 0; j < g_outfits.size(); ++j)
        if (same_saved(g_outfits[j], o)) {
            if (j == index) notify(self, lang::f("toast.already_holds", {{"outfit", g_outfits[j].name}}));
            else open_confirm(self, index, kConfirmSwap, static_cast<U32>(j));
            return;
        }
    open_confirm(self, index, kConfirmOverwrite, reset ? 1 : 0);
}

// A left click anywhere acts like Space on the highlighted (focused) slot, except on the tab headers (switching tabs
// must not also load the highlighted outfit; the tab changes only after the press).
std::atomic<U64> g_click_at{0};
std::atomic<U64> g_click_pos{~0ull}; // client px of the press: x << 32 | y (~0: unknown)

bool click_on_header(U64 self)
{
    U64 pos = g_click_pos.load();
    if (pos == ~0ull) return false;
    float x = float(int(U32(pos >> 32))), y = float(int(U32(pos)));
    U64 tb = rp(self + kOffRootTabBar);
    U64 hv = tb ? rp(tb + kOffTabHeaders) : 0;
    U32 n = tb ? r32(tb + kOffTabHeaderCount) : 0;
    std::string seen;
    for (U32 i = 0; hv && i < n && i < 8; ++i) {
        U64 h = rp(hv + i * 8ull);
        U64 e = h ? rp(h + 8) : 0;
        float l, t, r, b; // element layout rect (client px), as tooltip.cpp reads it
        if (!e || !is_element(e) || has_class(e, "!cpp-none") || !rd(e + kOffElemRect, l) ||
            !rd(e + kOffElemRect + 4, t) || !rd(e + kOffElemRect + 8, r) || !rd(e + kOffElemRect + 0xc, b) || !(r > l && b > t))
            continue;
        char buf[64];
        std::snprintf(buf, sizeof buf, " %u:%.0f,%.0f-%.0f,%.0f", i, l, t, r, b);
        seen += buf;
        constexpr float pad = 6;
        if (x >= l - pad && x <= r + pad && y >= t - pad && y <= b + pad) {
            LOG("click at %.0f,%.0f on tab header %u (%.0f,%.0f-%.0f,%.0f): ignored", x, y, i, l, t, r, b);
            return true;
        }
    }
    LOG("click at %.0f,%.0f; headers%s", x, y, seen.empty() ? " (no rects)" : seen.c_str());
    return false;
}

// The L2 / R2 prompts by the focused slot (the view's QuickSlotSelectPrev / NextKeyGuide, shown for a gamepad only):
// the game hides them on our slots (nothing to cycle, as it sees them), so on the Outfits tab they are shown again,
// which also switches its own L2 / R2 back on there. Their group (#QuickSlotItemKeyguideGroup) is placed by the game's
// code: its style's left (+0) and top (+8), in layout units, are the focused slot's centre; on the Outfits tab the mod
// does the same. Layout units per screen pixel: learnt on the game's tabs (its own left / the focused slot's centre),
// else layout height 1080 over the window's height. On its own tabs the game keeps managing all of it.
float g_prompt_scale = 0;
U64 g_hover_script = 0; // the slot the game's focus call last named (0: none)

void pad_prompts(U64 self)
{
    static U64 prev = 0, next = 0, group = 0;
    U64 menu = rp(self + 8);
    if (!prev || !is_element(prev)) prev = find_by_class(menu, "quickslot-item-focus-keyguide-prev", 24);
    if (!next || !is_element(next)) next = find_by_class(menu, "quickslot-item-focus-keyguide-next", 24);
    if (!group || !is_element(group)) group = find_parent_of_class(menu, "quickslot-keyguide-wrap", 24);
    U64 gh = group ? rp(group + 0x20) : 0, style = gh ? rp(gh + 0x20) : 0;
    U64 script = focused_script(self), fe = script ? rp(script + 8) : 0;
    auto box = [](U64 e, float& cx, float& cy) {
        float l = 0, t = 0, r = 0, b = 0;
        if (!e || !rd(e + kOffElemRect, l) || !rd(e + kOffElemRect + 4, t) || !rd(e + kOffElemRect + 8, r) ||
            !rd(e + kOffElemRect + 0xc, b) || r <= l || b <= t) return false;
        cx = (l + r) / 2, cy = (t + b) / 2;
        return true;
    };
    // The prompts flank the slot's item picture when it is shown. A hidden picture (the "+" slot's, every settings
    // entry's) keeps a stale box, so then the spot is computed: the wheel's centre (the ring background's) plus the
    // slot's offset from it, as the mod's own css places the slots (om-sN-i: margins + half the 50px slot; in layout
    // units, the unit the prompts' style uses).
    static constexpr float kSlotAt[12][12][2] = {
#include "om_slot_offsets.inc"
    };
    float px = 0, py = 0, left = 0;
    if (!outfits_current(self)) { // the game's tab: learn the scale from where it put the group
        U64 img = fe ? find_by_class(fe, "quickslot-item-img", 8) : 0;
        if (img && !has_class(img, "!cpp-none") && box(img, px, py) && style && rd(style, left) && left > 0 &&
            left < 100000)
            g_prompt_scale = left / px;
        return;
    }
    // The slot really hovered: the one the game's focus call last named (focus_hook). After a view or page change none
    // may be hovered (the focus value still names the last one): then no prompts.
    // A view / page change re-lays the wheel without a new focus call: the last one names a slot of the old layout, so
    // it is forgotten until the game's next focus call.
    static int view_was = -1;
    int view = g_eye >= 0 ? 0x100 | g_eye_step : g_cfg >= 0 ? 0x200 : 0; // (the settings view: one layout, whichever
    if (view != view_was) {                                                // outfit, as Position moves it)
        if (view_was != -1) g_hover_script = 0;
        view_was = view;
    }
    U32 n = visible_slots(), i = outfit_slot_index(self, g_hover_script);
    if (i < n) script = g_hover_script, fe = rp(script + 8);
    if (in_depth(self) || n > 12) return;
    if (i >= n) { // nothing hovered
        for (U64 e : {prev, next})
            if (e && !has_class(e, "!cpp-none")) set_class(e, "!cpp-none", true);
        return;
    }
    // Entries with nothing to scroll, as Rename / Delete: no L2 / R2, as vanilla. Position and Language have no dots but
    // scroll (Position moves the outfit on the wheel; Language has a value per language).
    bool scrolls = mode_count(i) > 1;
    if (!scrolls && g_cfg >= 0) {
        std::vector<std::uint8_t> e = cfg_entries();
        U32 k = slot_outfit(i);
        scrolls = k < e.size() && (e[k] == kCfgPosition || e[k] == kCfgLanguage);
    }
    for (U64 e : {prev, next})
        if (e && has_class(e, "!cpp-none") == scrolls) set_class(e, "!cpp-none", !scrolls);
    if (!scrolls) return;
    float k = g_prompt_scale;
    RECT cr{};
    HWND w = GetForegroundWindow();
    bool client = w && GetClientRect(w, &cr) && cr.bottom > 0;
    if (k <= 0) {
        if (!client) return;
        k = 1080.0f / float(cr.bottom);
    }
    if (!style) return;
    float x = 0, y = 0, cur_x = 0, cur_y = 0;
    // Only a saved outfit's chest picture is trusted (the settings / appearance entries' pictures keep stale boxes).
    bool real_picture = g_cfg < 0 && (g_eye >= 0 ? slot_outfit(i) < eye_pieces().size() : slot_outfit(i) < g_outfits.size());
    U64 img = fe && real_picture ? find_by_class(fe, "quickslot-item-img", 8) : 0;
    if (img && !has_class(img, "!cpp-none") && box(img, px, py)) { // a visible picture: centred on it
        x = px * k, y = py * k;
    } else { // hidden: the computed spot
        if (!(g_ring_bg && is_element(g_ring_bg) && box(g_ring_bg, px, py))) { // (else the screen's centre, as the wheel is)
            if (!client) return;
            px = cr.right / 2.0f, py = cr.bottom / 2.0f;
        }
        // the layout the slot really has: its om-sN-i class (the view's slot count can change before it is re-applied)
        unsigned cn = 0, ci = 0;
        if (sscanf_s(g_slot_class[i].c_str(), "om-s%u-%u", &cn, &ci) != 2 || cn < 1 || cn > 12 || ci >= cn) return;
        x = px * k + kSlotAt[cn - 1][ci][0], y = py * k + kSlotAt[cn - 1][ci][1];
    }
    if (rd(style, cur_x) && rd(style + 8, cur_y) && (std::fabs(cur_x - x) > 0.5f || std::fabs(cur_y - y) > 0.5f))
        wr<float>(style, x), wr<float>(style + 8, y); // (the game lays it out from these on its next pass)
}

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

void pump(U64 self)
{
    if (GetCurrentThreadId() != g_ui_thread || !g_menu_open || g_busy) return;
    peek_capture_frame(self);
    { // the "+" page shows the Outfit Arc binding of the input in use: re-shown when the input changes
        static int shown = -1;
        int now = g_pad_in_use ? (g_pad_is_ps ? 2 : 1) : 0;
        if (g_cfg >= 0 && size_t(g_cfg) >= g_outfits.size() && shown >= 0 && now != shown) {
            if (!now) g_peek_capture = false;
            cfg_refresh(self); // (a full redraw: the slot's name, value and images)
        }
        shown = now;
    }
    Busy busy;
    pull_active();
    pump_async(self); // (on any tab: a load's asynchronous parts finish even after switching tabs)
    stash_verify(self);
    if (!outfits_current(self)) {
        pad_prompts(self); // (learns where the game puts the prompts)
        return;
    }
    if (g_applied_visible != visible_slots()) refresh_wheel(self);
    // The game's slot refresh no longer clears our pictures (slot_refresh_hook): they are only put back if hidden,
    // and re-set outright just after the menu opens / the Outfits tab shows (g_force_images). Re-setting them on every
    // focus change, prompt and load (as before) made them flicker.
    keep_slot_images(self, g_force_images > 0);
    if (g_force_images > 0) --g_force_images;
    watch_rename_prompt();
    watch_ask();
    if (g_release_pending && !g_name_modal && !g_ask_modal) {
        g_release_pending = false;
        if (g_queue_hide) release_menu_button(self);
    }
    if (g_name_modal || g_ask_modal || GetTickCount64() - g_modal_ended < 300) { // a prompt (and the key that closed it)
        g_click_at = 0, g_space_at = 0;
    }
    bool click = take_press(g_click_at) && !click_on_header(self);
    bool space = take_press(g_space_at);
    if (in_depth(self)) click = space = false; // (the game's own submenu, the lantern's: its presses are the game's)
    pad_prompts(self); // (shown, they also switch the game's own L2 / R2 back on: it scrolls our slots through its handler)
    if (g_prompt_changed.exchange(false)) apply_outfit_slots(self); // (the confirm icons: mouse / Cross / A)
    if ((click || space) && g_eye >= 0) {
        U32 i = outfit_slot_index(self, focused_script(self));
        LOG("%s in the appearance view: slot %u", click ? "click" : "Space", i);
        if (i < visible_slots()) eye_press(self, i);
        click = space = false;
    }
    if ((click || space) && g_cfg >= 0) {
        U32 i = outfit_slot_index(self, focused_script(self));
        LOG("%s in the settings view: slot %u", click ? "click" : "Space", i);
        if (i < visible_slots()) cfg_press(self, i);
        click = space = false;
    }
    if (click || space) {
        U64 script = focused_script(self);
        U32 i = outfit_slot_index(self, script);
        U32 o = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
        LOG("%s: focused slot %u (script %llX) -> outfit %u of %zu", click ? "click" : "Space", i,
            script, o, g_outfits.size());
        std::uint8_t mode = i < visible_slots() ? g_slot_mode[i] : std::uint8_t(kModeMain);
        bool press = click || space;
        if (press && mode == kModeSettings && o <= g_outfits.size()) cfg_enter(self, o); // (the "+" slot: the plugin's)
        else if (press && o == g_outfits.size() && o < kOutfitSlots) save_new_outfit(self);
        else if (press && o < g_outfits.size()) start_loading(self, o), refresh_wheel(self);
    }
    // The worn armor can also change outside our loader (Armor tab, game events): rechecked when a model swaps (without
    // that hook: a few times a second).
    static U64 worn_checked = 0;
    bool due = g_model_swap_orig ? g_worn_dirty.exchange(false) : GetTickCount64() - worn_checked > 250;
    if (due) {
        worn_checked = GetTickCount64();
        if (find_worn_outfit(self) != g_worn_outfit) apply_outfit_slots(self);
    }
    // Pieces can leave the inventory while the menu is open (dropped, sold): the greyed-out outfits follow.
    if (U64 now = GetTickCount64(); now - g_missing_checked > 1000 && refresh_missing(self)) apply_outfit_slots(self);
    if (g_worn_changed) {
        g_worn_changed = false;
        center_restore(); // the Equipped tag follows
        center_for_focus(self, focused_script(self), false);
    }
    if (g_center_frames > 0) {
        --g_center_frames;
        center_show(self);
    }
}

// Call counters (logged) tell which callbacks run every frame while the menu is open.
struct CallStat {
    const char* name;
    U32 calls = 0;
    U64 since = 0;
    void hit()
    {
        if (calls++ == 0) {
            since = GetTickCount64();
            LOG("%s: first call (thread %lu, ui thread %lu)", name, GetCurrentThreadId(), g_ui_thread);
        } else if (calls == 600) {
            LOG("%s: 600 calls in %llu ms", name, GetTickCount64() - since);
        }
    }
};
CallStat g_frame_stat{"frame getter"};
CallStat g_select_stat{"cursor select"};

// Stick selection: the cursor maps the stick angle to one of the tab's sectors; on the Outfits tab a
// sector without a visible slot keeps the current focus (as a centered stick does).
U64 __fastcall cursor_select_hook(U64 self, U64 cursor, U64 flag, U64 idx, U64 a5, U64 a6)
{
    if (g_menu_open) g_select_stat.hit();
    pump(self);
    short i = static_cast<short>(idx);
    if (i >= 0 && U32(i) >= visible_slots() && outfits_current(self) && !in_depth(self))
        idx = (idx & ~0xFFFFull) | 0xFFFF;
    return g_select_orig(self, cursor, flag, idx, a5, a6);
}

float __fastcall frame_hook(U64 self)
{
    if (g_menu_open) g_frame_stat.hit();
    if ((!g_unhide_retry.empty() || !g_rehide_retry.empty()) && GetCurrentThreadId() == g_ui_thread) {
        retry_restore();
        if (g_unhide_retry.empty() && g_rehide_retry.empty()) LOG("centre restore done");
    }
    pump(self);
    if (GetCurrentThreadId() == g_ui_thread) look_pending(), anchor_recheck();
    return g_frame_orig(self);
}

// Slot focus (stick or mouse): the game fills the centre tooltip from the slot's item, or hides it.
U64 __fastcall focus_hook(U64 self, U64 script, U64 a, U64 b, U64 c, U64 d)
{
    center_restore();
    U64 r = g_focus_orig(self, script, a, b, c, d);
    g_hover_script = script; // (0 or another tab's: nothing of ours hovered; the L2 / R2 prompts follow this)
    if (outfits_current(self)) {
        center_for_focus(self, script, false);
        keep_slot_images(self, false);
        pump(self);
    }
    return r;
}
void scroll_slot(U64 self, U64 slot, U32 i, int step);

// Logs class changes on the menu's root element, SetState on the menu script, and any show/hide-like class change
// anywhere while the menu is open (capped per open).

// A scroll on a slot: on ours (Outfits tab, outside a submenu) the slot's mode moves on; the rest is the game's.
// The slot script's root is at +0x28 (the game's scroll handlers read it there).

bool scroll_mode(U64 slot, int step)
{
    if (!g_menu_open || g_busy || GetCurrentThreadId() != g_ui_thread) return false;
    U64 self = rp(slot + 0x28);
    if (!self || !outfits_current(self) || in_depth(self)) return false;
    U32 i = outfit_slot_index(self, slot);
    if (i >= visible_slots()) return false;
    Busy busy;
    scroll_slot(self, slot, i, step);
    return true;
}

// The scroll itself (mouse wheel through the game's handler above, or a pad's L2 / R2 from pump).
void scroll_slot(U64 self, U64 slot, U32 i, int step)
{
    if (g_eye >= 0) { // the view: a piece's look moves on / back; the page slot steps (never leaves)
        eye_scroll(self, i, step);
        return;
    }
    if (g_cfg >= 0) { // the settings view: a value setting moves on / back
        cfg_scroll(self, i, step);
        return;
    }
    U32 n = mode_count(i);
    std::uint8_t before = g_slot_mode[i];
    g_slot_mode[i] = std::uint8_t((before + n + step) % n);
    LOG("scroll %s on slot %u: mode %u -> %u", step > 0 ? "next" : "prev", i, before, g_slot_mode[i]);
    apply_outfit_slots(self, i);
    center_for_focus(self, slot, false);
}

U64 __fastcall scroll_prev_hook(U64 slot) { return scroll_mode(slot, -1) ? 1 : g_scroll_prev_orig(slot); }
U64 __fastcall scroll_next_hook(U64 slot) { return scroll_mode(slot, 1) ? 1 : g_scroll_next_orig(slot); }

U64 __fastcall close_hook(U64 self, U64 a, U64 b, U64 c, U64 d, U64 e)
{
    bool sub = g_eye >= 0 || g_cfg >= 0;
    reset_modes();
    g_eye = -1, g_eye_step = 0, g_cfg = -1; // (the views end with the menu)
    // Closed in the settings / Visibility: the slots are put back to the outfits as Back does, while the wheel still
    // shows. Left with that view's dots, the next opening's dot changes stayed unshown (the old count's dots) until a
    // hover; closing from the outfits never did that.
    if (sub && GetCurrentThreadId() == g_ui_thread && outfits_current(self)) {
        Busy busy;
        refresh_wheel(self);
    }
    g_menu_open = false;
    g_center_desc.clear();
    abort_prompts("menu closed");
    center_restore();
    return g_close_orig(self, a, b, c, d, e);
}

// Runs when the menu returns from a wheel (which hid every header): show the Outfits header again.
U64 __fastcall gear_mode_hook(U64 self, U64 a, U64 b, U64 c)
{
    U64 r = g_gear_orig(self, a, b, c);
    if (U64 h = outfits_header(self)) {
        show_header(h); // (as the gear mode does for 0 and 1)
        set_label(h);
        apply_outfit_slots(self);
    }
    sync_ring(self);
    return r;
}

// --- Diagnostics hooks --------------------------------------------------------------------------
bool g_dumped_qsi = false;

bool g_tab_checked = false;

void self_test(U64 self)
{
    static bool model_done = false, bags_done = false, names_done = false;
    ensure_resist_ids();
    if (U64 tb = rp(self + kOffRootTabBar); tb && !g_tab_checked) { // (the current tab: only ever one of the headers)
        U32 cur = r32(tb + kOffTabCurrent), headers = r32(tb + kOffTabHeaderCount);
        g_tab_checked = true;
        LOG("self-test: current tab %u of %u headers: %s", cur, headers, cur < headers ? "ok" : "FAILED");
        if (cur >= headers && g_layout_bad.empty()) g_layout_bad = "tab state";
    }
    if (model_done && bags_done && names_done) return;
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    if (actor && !model_done) { // the character model behind the actor and its part table: Visibility
        U64 model = look_model(actor);
        U64 tab = model ? rp(model + 0x58) : 0;
        U32 n = model ? r32(model + 0x60) : 0;
        if (model) {
            U32 named = 0;
            for (U32 i = 0; tab && i < n && i < 1024; ++i)
                if (U64 part = rp(tab + i * 16ull + 8); part && readable_name(part_name(r32(part + 0x48)))) ++named;
            bool ok = tab && n >= 4 && n <= 1024 && named * 2 >= n;
            LOG("self-test: character model %llX: %u parts, %u named: %s", model, n, named, ok ? "ok" : "FAILED");
            if (!ok && g_vis_set) {
                g_vis_set = nullptr;
                feature_off("feature.visibility_rules");
            }
            model_done = true;
        }
    }
    if (actor && !bags_done) { // the bags and their capacity: Stash
        if (U64 mgr = call_inventory(actor)) {
            U64 bags = rp(mgr + kOffMgrBags);
            U32 n = r32(mgr + 0x20), sized = 0, odd = 0;
            for (U32 i = 0; bags && i < n && i < 64; ++i) {
                U64 bag = rp(bags + i * 8ull);
                short cap = 0;
                if (!bag || !rd(bag + kOffBagCount, cap)) continue;
                if (cap > 0 && cap <= 5000) ++sized;
                else if (cap != 0) ++odd;
            }
            bool ok = n >= 1 && n <= 64 && sized >= 1 && !odd;
            LOG("self-test: %u bags, %u with a capacity, %u odd: %s", n, sized, odd, ok ? "ok" : "FAILED");
            if (!ok && g_item_move) {
                g_item_move = nullptr;
                feature_off("feature.stash");
            }
            bags_done = true;
        }
    }
    if (ctx[0]) release_player(ctx);
    if (!names_done && g_item_info) { // a worn piece's key and name (the item record's layout): the names shown, only
        WornRaw w[kWornMax];
        int n = read_worn(self, w, kWornMax), worn = 0, named = 0;
        for (int i = 0; i < n; ++i)
            if (w[i].key != 0xFFFF) ++worn, named += readable_name(item_name(w[i].key));
        if (worn) {
            LOG("self-test: %d worn pieces, %d named: %s", worn, named, named ? "ok" : "FAILED");
            if (!named) feature_off("feature.item_names");
            names_done = true;
        }
    }
}

// Before the outfits are shown or loaded (the wheel opening, the quick peek opening): a save just loaded is taken over,
// the character's own outfits are the ones in use, and their keys are checked against the permanent ids (a save
// written before a game patch brings back the old keys: a load before this repair fetched the wrong items, 2026-11-05).

void prepare_outfits()
{
    ensure_resist_ids();
    settle_load_now();
    pull_active();
    if (U16 ch = current_character(); ch != kNoCharacter) switch_character(ch);
    repair_keys();
}
void features_toast(U64 self);

U64 __fastcall open_hook(U64 self, U64 a, U64 b, U64 c)
{
    if (g_ui_thread && g_ui_thread != GetCurrentThreadId())
        LOG("ui thread: the wheel opens on %lu, the status screen gave %lu", GetCurrentThreadId(), g_ui_thread);
    g_ui_thread = GetCurrentThreadId();
    center_restore();
    prepare_outfits(); // (each character has its own outfits: the wheel shows those of the one it opens for)
    U64 r = g_open_orig(self, a, b, c);
    g_menu_open = true;
    g_gear_resist.clear(); // (abyss gears can change between opens: read again)
    { // what each character wears, once per session (names and uids: for reading logs and making test outfits)
        static std::vector<U16> logged;
        U16 ch = g_character;
        if (ch != kNoCharacter && std::find(logged.begin(), logged.end(), ch) == logged.end()) {
            logged.push_back(ch);
            WornRaw w[kWornMax];
            int n = read_worn(self, w, kWornMax);
            LOG("worn by %s (character %u):", character_name(ch), ch);
            for (int i = 0; i < n; ++i)
                if (w[i].key != 0xFFFF)
                    LOG("  equipment-%u key %u \"%s\" uid %llX", w[i].slot, w[i].key, item_name(w[i].key).c_str(), w[i].uid);
        }
    }
    reset_modes();
    g_force_images = 3;
    note_menu_binding();
    bool layout_bad = g_fake_key.load() != 0xFFFF ? !layout_ok(self) : !g_layout_bad.empty(); // (build-time failure)
    if (layout_bad && !g_layout_toast_done && g_layout_bad != "menu") {
        g_layout_toast_done = true;
        notify(self, lang::f("toast.layout_changed", {{"what", g_layout_bad}}));
    }
    self_test(self);
    features_toast(self);
    if (U64 h = outfits_header(self)) {
        // The thtml hides it: only a working plugin shows it, and only beside a visible Weapons header (gear mode, not
        // the comrades / items wheels). Visible = no hiding class: the other wheels hide it with "cpp-hide", the thtml
        // with "!cpp-none" (a fresh header has neither, nor "cpp-show").
        U64 weapons = rp(rp(rp(self + kOffRootTabBar) + kOffTabHeaders));
        U64 we = weapons ? rp(weapons + 8) : 0;
        if (we && !has_class(we, "cpp-hide") && !has_class(we, "!cpp-none")) show_header(h);
        set_label(h);
        apply_outfit_slots(self);
    }
    sync_ring(self);
    if (!g_dumped_qsi) {
        g_dumped_qsi = true;
        dump_quickslotinfo();
        LOG("icon script flag %u, ui thread %lu", r32(g_base + kRvaIconScriptFlag) & 0xFF, g_ui_thread);
    }
    if (self == g_outfits_root) dump_menu(self, "open");
    return r;
}

U64 __fastcall tab_hook(U64 self, U64 tabbar, U64 flag, U64 old_idx, U64 new_idx, U64 header)
{
    center_restore();
    U64 r = g_tab_orig(self, tabbar, flag, old_idx, new_idx, header);
    if (tabbar == rp(self + kOffRootTabBar)) {
        if (U32(new_idx) == g_fake_key.load()) {
            apply_outfit_slots(self);
            g_force_images = 3;
        }
        sync_ring(self);
    }
    return r;
}
bool g_features_toast_done = false;

// At the first menu open: one toast naming whatever had to be turned off.
void features_toast(U64 self)
{
    static bool store_told = false;
    if (g_store_unreadable && !store_told) {
        store_told = true;
        std::string why = g_store_why; // (its English detail is in the log)
        const char* key = !why.compare(0, 7, "damaged") ? "store.damaged" : !why.compare(0, 4, "from") ? "store.newer" : "store.locked";
        notify(self, lang::f("toast.store_bad", {{"why", lang::t(key)}}));
    }
    if (g_features_toast_done || g_features_off.empty()) return;
    g_features_toast_done = true;
    std::string list; // (feature.* keys)
    for (size_t a = 0; a < g_features_off.size();) {
        size_t b = g_features_off.find(',', a);
        if (b == std::string::npos) b = g_features_off.size();
        list += (list.empty() ? std::string() : lang::f("list.separator")) + lang::t(g_features_off.substr(a, b - a).c_str());
        a = b + 1;
    }
    notify(self, lang::f("toast.features_off", {{"list", list}}));
}

// Optional: without it saving works as before and names stay "Outfit N".
void install_rename_prompt()
{
    if (!have({kRvaTextModalVt, kRvaTextModalClick, kRvaOpenTextModal, kRvaModalParamsInit, kRvaModalParamsFree,
               kRvaFindView, kRvaGetText, kRvaNameInvalid, kRvaHideScript, kOffModalOwner, kOffModalD0})) {
        feature_off("feature.renaming");
        return;
    }
    void** vt = reinterpret_cast<void**>(g_base + kRvaTextModalVt);
    g_find_view = reinterpret_cast<FindViewFn>(g_base + kRvaFindView);
    g_params_init = reinterpret_cast<VoidFn>(g_base + kRvaModalParamsInit);
    g_params_free = reinterpret_cast<VoidFn>(g_base + kRvaModalParamsFree);
    g_get_text = reinterpret_cast<GetTextFn>(g_base + kRvaGetText);
    g_name_invalid = reinterpret_cast<NameInvalidFn>(g_base + kRvaNameInvalid);
    g_hide_script = reinterpret_cast<VoidFn>(g_base + kRvaHideScript);
    g_text_click_orig = reinterpret_cast<ModalClickFn>(vt[kSlotTextModalClick]);
    if (!game::patch_vtable(vt, kSlotTextModalClick, reinterpret_cast<void*>(&text_click_hook))) {
        LOG("rename prompt: vtable patch failed; outfits keep their default names");
        feature_off("feature.renaming");
        return;
    }
    if (have({kRvaQueueHide})) // closing the held menu after a prompt (else it stays up until the button is pressed)
        g_queue_hide = reinterpret_cast<QueueHideFn>(g_base + kRvaQueueHide);
    if (have({kRvaBindingInput})) { // ... and releasing its key binding (else the next press only releases it)
        void* t = reinterpret_cast<void*>(g_base + kRvaBindingInput);
        if (MH_CreateHook(t, reinterpret_cast<void*>(&binding_input_hook),
                          reinterpret_cast<void**>(&g_binding_input_orig)) != MH_OK ||
            MH_EnableHook(t) != MH_OK) {
            g_binding_input_orig = nullptr;
            LOG("rename prompt: key binding hook failed; the menu button may need an extra press after a prompt");
        }
    }
    g_open_text_modal = reinterpret_cast<OpenTextModalFn>(g_base + kRvaOpenTextModal); // last: enables the prompt
    LOG("rename prompt ready (TextEdit modal slot %d hooked)", kSlotTextModalClick);
}

// Optional too (needs the rename prompt's helpers): without it a gone piece's slot is simply left as is.
void install_ask_dialog()
{
    if (!g_find_view ||
        !have({kRvaCommonModalVt, kRvaCommonModalClick, kRvaOpenCommonModal, kRvaCommonModalName, kOffModalOwner}) ||
        memcmp(reinterpret_cast<const void*>(g_base + kRvaCommonModalName), "CommonModalMessage", 19) != 0) {
        feature_off("feature.missing_pieces");
        return;
    }
    void** vt = reinterpret_cast<void**>(g_base + kRvaCommonModalVt);
    g_ask_click_orig = reinterpret_cast<ModalClickFn>(vt[kSlotCommonModalClick]);
    if (!game::patch_vtable(vt, kSlotCommonModalClick, reinterpret_cast<void*>(&ask_click_hook))) {
        LOG("missing-pieces question: vtable patch failed; gone pieces are left as is");
        feature_off("feature.missing_pieces");
        return;
    }
    g_open_common_modal = reinterpret_cast<OpenCommonModalFn>(g_base + kRvaOpenCommonModal); // last: enables it
    LOG("missing-pieces question ready (CommonModal slot %d hooked)", kSlotCommonModalClick);
}

bool install()
{
    g_base = game::main_module().base;
    resolve_addresses();
    // The core: the Outfits tab, its wheel, saving (worn gear, icons, centre text) and the menu hooks.
    if (!have({kRvaRootQuickSlotVt, kRvaRootOpen, kRvaRootClose, kRvaRootFrame, kRvaRootFocus, kRvaRootTabChanged,
               kRvaRootCursorSelect, kRvaQuickSlotInfoGet, kRvaStringInfoGet, kRvaQuickSlotInfoMgr, kRvaClassNames,
               kRvaBuildBodies, kRvaGearMode, kRvaSetText, kRvaQsiTableRoot0, kRvaQsiTableRoot1, kRvaIntern,
               kRvaClassVecAdd, kRvaClassVecRemove, kRvaClassVecSort, kRvaRestyle, kRvaPlayerHolder, kRvaActorCtx,
               kRvaCtxRelease, kRvaCtxVt, kRvaItemIconName, kRvaEmptyItem, kRvaEmptyString, kRvaStrAssign,
               kRvaStrNormalize, kRvaStrLower, kRvaStrDestroy, kRvaSetStyleProp, kRvaTextureExists,
               kRvaIconScriptFlag, kRvaSetState, kRvaStateShow, kRvaStateHide, kRvaFocusedSlot,
               kRvaEquip, kRvaUnequip, kRvaInventory})) { // without loading the tab has no purpose: all or nothing
        LOG("gear menu or equip addresses not found; unsupported game version");
        return false;
    }
    void** vt = reinterpret_cast<void**>(g_base + kRvaRootQuickSlotVt);
    g_intern = reinterpret_cast<InternFn>(g_base + kRvaIntern);
    g_vec_add = reinterpret_cast<VecAddFn>(g_base + kRvaClassVecAdd);
    g_vec_remove = reinterpret_cast<VecRemoveFn>(g_base + kRvaClassVecRemove);
    g_vec_sort = reinterpret_cast<VoidFn>(g_base + kRvaClassVecSort);
    g_restyle = reinterpret_cast<VoidFn>(g_base + kRvaRestyle);
    g_qsi_get = reinterpret_cast<RecordGetFn>(g_base + kRvaQuickSlotInfoGet);
    g_str_get = reinterpret_cast<RecordGetFn>(g_base + kRvaStringInfoGet);
    g_set_text = reinterpret_cast<SetTextFn>(g_base + kRvaSetText);
    g_actor_ctx = reinterpret_cast<CtxFn>(g_base + kRvaActorCtx);
    g_ctx_release = reinterpret_cast<CtxReleaseFn>(g_base + kRvaCtxRelease);
    g_icon_name = reinterpret_cast<IconNameFn>(g_base + kRvaItemIconName);
    g_str_assign = reinterpret_cast<StrAssignFn>(g_base + kRvaStrAssign);
    g_str_normalize = reinterpret_cast<StrFn>(g_base + kRvaStrNormalize);
    g_str_lower = reinterpret_cast<StrFn>(g_base + kRvaStrLower);
    g_str_destroy = reinterpret_cast<StrFn>(g_base + kRvaStrDestroy);
    g_set_style_prop = reinterpret_cast<SetStylePropFn>(g_base + kRvaSetStyleProp);
    g_texture_exists = reinterpret_cast<TextureExistsFn>(g_base + kRvaTextureExists);
    g_set_state = reinterpret_cast<SetStateFn>(g_base + kRvaSetState);
    g_focused_slot = reinterpret_cast<FocusedSlotFn>(g_base + kRvaFocusedSlot);
    g_equip = reinterpret_cast<EquipFn>(g_base + kRvaEquip);
    if (have({kRvaEquipRequest, kRvaUnequipRequest})) {
        g_equip_request = reinterpret_cast<EquipRequestFn>(g_base + kRvaEquipRequest);
        g_unequip_request = reinterpret_cast<UnequipRequestFn>(g_base + kRvaUnequipRequest);
    } else {
        feature_off("feature.companions");
    }
    g_unequip = reinterpret_cast<UnequipFn>(g_base + kRvaUnequip);
    g_inventory = reinterpret_cast<InventoryFn>(g_base + kRvaInventory);
    if (have({kRvaErrorMgr, kRvaErrorText})) { // only the text of the game's equip errors, in the log
        g_error_mgr = reinterpret_cast<ErrorMgrFn>(g_base + kRvaErrorMgr);
        g_error_text = reinterpret_cast<ErrorTextFn>(g_base + kRvaErrorText);
    }
    if (have({kRvaToast}))
        g_toast = reinterpret_cast<ToastFn>(g_base + kRvaToast);
    else
        LOG("toast function not found; notifications only go to the log");
    if (MH_STATUS s = MH_Initialize(); s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) return false;
    // Killswitch: from here on any failure undoes every hook, so the mod is either fully on or fully off (and the
    // Outfits header, hidden in the thtml, is only ever shown by a working plugin).
    struct VtPatch { int slot; void* fn; void** orig; } patches[] = {
        {kSlotOpen, reinterpret_cast<void*>(&open_hook), reinterpret_cast<void**>(&g_open_orig)},
        {kSlotTabChanged, reinterpret_cast<void*>(&tab_hook), reinterpret_cast<void**>(&g_tab_orig)},
        {kSlotCursorSelect, reinterpret_cast<void*>(&cursor_select_hook), reinterpret_cast<void**>(&g_select_orig)},
        {kSlotClose, reinterpret_cast<void*>(&close_hook), reinterpret_cast<void**>(&g_close_orig)},
        {kSlotFrame, reinterpret_cast<void*>(&frame_hook), reinterpret_cast<void**>(&g_frame_orig)},
        {kSlotFocus, reinterpret_cast<void*>(&focus_hook), reinterpret_cast<void**>(&g_focus_orig)},
    };
    auto fail = [&](const char* what) {
        MH_DisableHook(MH_ALL_HOOKS);
        for (auto& p : patches)
            if (*p.orig) game::patch_vtable(vt, p.slot, *p.orig);
        LOG("%s; every hook undone", what);
        return false;
    };
    if (!install_save_hooks()) return fail("file hooks failed");
    struct { U64 rva; void* fn; void** orig; } hooks[] = {
        {kRvaQuickSlotInfoGet, reinterpret_cast<void*>(&qsi_get_hook), reinterpret_cast<void**>(&g_qsi_get)},
        {kRvaBuildBodies, reinterpret_cast<void*>(&build_hook), reinterpret_cast<void**>(&g_build_orig)},
        {kRvaGearMode, reinterpret_cast<void*>(&gear_mode_hook), reinterpret_cast<void**>(&g_gear_orig)},
    };
    for (auto& h : hooks) {
        void* target = reinterpret_cast<void*>(g_base + h.rva);
        if (MH_CreateHook(target, h.fn, h.orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
            LOG("hook at +%llX failed", h.rva);
            return fail("gear menu hook failed");
        }
    }
    for (auto& p : patches) {
        *p.orig = vt[p.slot]; // before the patch: the game may call the hook at once
        if (!game::patch_vtable(vt, p.slot, p.fn)) return fail("RootQuickSlot vtable patch failed");
    }
    LOG("hooks: getter, builder, gear mode, open %p, tab %p, select %p, close %p, frame %p, focus %p (base %llX)",
        g_open_orig, g_tab_orig, g_select_orig, g_close_orig, g_frame_orig, g_focus_orig, g_base);
    install_rename_prompt();
    install_ask_dialog();
    if (g_equip_request && have({kRvaCompanionEquip})) { // companions' pieces from storage / the wardrobe
        void* t = reinterpret_cast<void*>(g_base + kRvaCompanionEquip);
        if (MH_CreateHook(t, reinterpret_cast<void*>(&companion_equip_hook),
                          reinterpret_cast<void**>(&g_companion_equip_orig)) != MH_OK ||
            MH_EnableHook(t) != MH_OK)
            g_companion_equip_orig = nullptr;
    }
    if (!g_companion_equip_orig) LOG("companions: storage / wardrobe pieces unavailable (Kliff only)");
    if (have({kRvaVisSet, kRvaVisRelease, kRvaBattleVis, kRvaSlotHandle})) { // Appearance
        void* t = reinterpret_cast<void*>(g_base + kRvaBattleVis);
        g_vis_set = reinterpret_cast<VisSetFn>(g_base + kRvaVisSet);
        g_vis_release = reinterpret_cast<VisReleaseFn>(g_base + kRvaVisRelease);
        g_slot_handle = reinterpret_cast<SlotHandleFn>(g_base + kRvaSlotHandle);
        if (have({kRvaBattleCheck})) g_battle_check = reinterpret_cast<BattleCheckFn>(g_base + kRvaBattleCheck);
        else LOG("appearance: battle check not found; combat looks follow the headgear switch");
        if (MH_CreateHook(t, reinterpret_cast<void*>(&battle_vis_hook), reinterpret_cast<void**>(&g_battle_vis_orig)) !=
                MH_OK ||
            MH_EnableHook(t) != MH_OK)
            g_vis_set = nullptr;
        void* s = reinterpret_cast<void*>(g_base + kRvaModelSwap); // (without it: pieces put on elsewhere wait for a switch)
        if (g_vis_set && have({kRvaModelSwap}) &&
            (MH_CreateHook(s, reinterpret_cast<void*>(&model_swap_hook), reinterpret_cast<void**>(&g_model_swap_orig)) !=
                 MH_OK ||
             MH_EnableHook(s) != MH_OK))
            LOG("appearance: model rebuild hook failed");
        void* vs = reinterpret_cast<void*>(g_base + kRvaVisSet); // flight: the flight cloak's layer (our calls: past it)
        void* vr = reinterpret_cast<void*>(g_base + kRvaVisRelease);
        if (g_vis_set && MH_CreateHook(vs, reinterpret_cast<void*>(&vis_set_hook), reinterpret_cast<void**>(&g_vis_set_orig)) == MH_OK &&
            MH_CreateHook(vr, reinterpret_cast<void*>(&vis_release_hook), reinterpret_cast<void**>(&g_vis_release_orig)) == MH_OK &&
            MH_EnableHook(vs) == MH_OK && MH_EnableHook(vr) == MH_OK)
            g_vis_set = g_vis_set_orig, g_vis_release = g_vis_release_orig;
        else
            LOG("appearance: flight hooks failed (the hat / cloak may show in flight)");
        void* c = reinterpret_cast<void*>(g_base + kRvaEquipVisCheck); // (without it: weapon looks also apply when drawn)
        if (g_vis_set && have({kRvaEquipVisCheck}) &&
            (MH_CreateHook(c, reinterpret_cast<void*>(&equip_vis_check_hook),
                           reinterpret_cast<void**>(&g_equip_vis_check_orig)) != MH_OK ||
             MH_EnableHook(c) != MH_OK))
            LOG("appearance: drawn-weapon hook failed");
    }
    if (!g_vis_set) feature_off("feature.appearance");
    // Slot modes: scrolling an Outfits slot (without them a slot only loads / saves).
    bool scroll_ok = false;
    if (have({kRvaSlotScript181, kRvaSlotScript182})) {
        void* p = reinterpret_cast<void*>(g_base + kRvaSlotScript181);
        void* n = reinterpret_cast<void*>(g_base + kRvaSlotScript182);
        scroll_ok = MH_CreateHook(p, reinterpret_cast<void*>(&scroll_prev_hook),
                                  reinterpret_cast<void**>(&g_scroll_prev_orig)) == MH_OK &&
                    MH_CreateHook(n, reinterpret_cast<void*>(&scroll_next_hook),
                                  reinterpret_cast<void**>(&g_scroll_next_orig)) == MH_OK &&
                    MH_EnableHook(p) == MH_OK && MH_EnableHook(n) == MH_OK;
    }
    if (!scroll_ok) feature_off("feature.settings");
    if (have({kRvaSlotScript164})) { // (without it the Outfits slots blink after equipment changes; nothing else)
        void* t = reinterpret_cast<void*>(g_base + kRvaSlotScript164);
        if (MH_CreateHook(t, reinterpret_cast<void*>(&slot_refresh_hook), reinterpret_cast<void**>(&g_slot_refresh_orig)) !=
                MH_OK ||
            MH_EnableHook(t) != MH_OK)
            LOG("slot refresh: not hooked (the Outfits slots may blink after equipment changes)");
    }
    if (scroll_ok && have({kRvaItemSelectorVt, kRvaSelectorSelect, kOffSlotSelector, kOffSelectorCount}))
    {
        void* t = reinterpret_cast<void*>(g_base + kRvaSelectorSelect);
        if (MH_CreateHook(t, reinterpret_cast<void*>(&selector_select_hook),
                          reinterpret_cast<void**>(&g_selector_select_orig)) == MH_OK &&
            MH_EnableHook(t) == MH_OK)
            g_selector_select = reinterpret_cast<SelectorSelectFn>(g_base + kRvaSelectorSelect); // (-> the hook)
        else
            LOG("slot dots: the select could not be hooked; no dots");
    }
    if (have({kRvaItemInfo, kRvaDefaultItemInfo}))
        g_item_info = reinterpret_cast<ItemInfoFn>(g_base + kRvaItemInfo);
    LOG("item names %s", g_item_info ? "ready" : "unavailable (messages name the slot only)");
    if (have({kRvaItemMove, kRvaBagInfo})) {
        g_item_move = reinterpret_cast<ItemMoveFn>(g_base + kRvaItemMove);
        g_bag_info = reinterpret_cast<BagInfoFn>(g_base + kRvaBagInfo);
    }
    if (g_item_move && have({kRvaHudVt})) { // (the stash check's tick with the wheel closed; else it waits for the wheel)
        void** hvt = reinterpret_cast<void**>(g_base + kRvaHudVt);
        if (hvt[kSlotFrame] == reinterpret_cast<void*>(g_base + kRvaRootFrame)) {
            g_hud_frame_orig = reinterpret_cast<FrameFn>(hvt[kSlotFrame]);
            if (!game::patch_vtable(hvt, kSlotFrame, reinterpret_cast<void*>(&hud_frame_hook))) g_hud_frame_orig = nullptr;
        }
    }
    LOG("stash %s%s", g_item_move ? "ready" : "unavailable (its game functions were not found)",
        g_item_move && !g_hud_frame_orig ? " (its report waits for the wheel: HUD frame not hooked)" : "");
    return true;
}

bool in_game_process()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe = buf;
    exe = exe.substr(exe.find_last_of(L"/\\") + 1);
    return _wcsicmp(exe.c_str(), L"CrimsonDesert.exe") == 0;
}

bool game_focused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Controllers: the stick picks a slot (the game's own cursor) and L2 / R2 scroll it through the game's own handler once
// the L2 / R2 prompts show (pad_prompts). The wheel has no confirm button of its own, so the pad is read directly and
// Cross / A counts as Space:
//  - Xbox pads (and anything shown as one, e.g. Steam Input's virtual pad): XInput, loaded at run time; a port that
//    reads as disconnected is skipped for 2 s (polling an empty port is slow).
//  - PlayStation pads (DualSense, DualSense Edge, DualShock 4; the game reads them natively, XInput never sees them):
//    their HID input reports, read by a thread that waits for the pad (no polling); the latest buttons are kept here.
constexpr U8 kPadCross = 1;   // the bits pad_buttons() returns: Cross / A held,
constexpr U8 kPadTouched = 2; // any button, trigger or stick in use (the pad is the input in use),
constexpr U8 kPadL1 = 4;      // L1 / LB held (the Outfit Arc's modifier),
constexpr U8 kPadUp = 8;      // the d-pad's up (or a diagonal with it),
constexpr U8 kPadDown = 16;   // and down

struct XPad {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XState {
    DWORD dwPacketNumber;
    XPad Gamepad;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XState*);
constexpr WORD kXInputA = 0x1000; // XINPUT_GAMEPAD_A
constexpr WORD kXInputUp = 0x0001, kXInputDown = 0x0002, kXInputLB = 0x0100; // XINPUT_GAMEPAD_DPAD_UP / _DOWN / LEFT_SHOULDER

U8 xinput_buttons()
{
    static XInputGetStateFn get = [] {
        for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryW(dll))
                if (auto f = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"))) {
                    LOG("controller input (Xbox pads): %ls", dll);
                    return f;
                }
        LOG("controller input: no XInput found (Xbox pads won't work in the Outfits tab)");
        return static_cast<XInputGetStateFn>(nullptr);
    }();
    static U64 skip_until[4] = {};
    if (!get) return 0;
    U64 now = GetTickCount64();
    U8 b = 0;
    struct Own { bool was = t_own_pad; Own() { t_own_pad = true; } ~Own() { t_own_pad = was; } } own; // (unfiltered)
    for (DWORD i = 0; i < 4; ++i) {
        if (now < skip_until[i]) continue;
        XState s{};
        if (get(i, &s) != ERROR_SUCCESS) {
            skip_until[i] = now + 2000;
            continue;
        }
        if (s.Gamepad.wButtons & kXInputA) b |= kPadCross;
        if (s.Gamepad.wButtons & kXInputLB) b |= kPadL1;
        if (s.Gamepad.wButtons & kXInputUp) b |= kPadUp;
        if (s.Gamepad.wButtons & kXInputDown) b |= kPadDown;
        if (s.Gamepad.wButtons || s.Gamepad.bLeftTrigger > 64 || s.Gamepad.bRightTrigger > 64 ||
            std::abs(s.Gamepad.sThumbLX) > 12000 || std::abs(s.Gamepad.sThumbLY) > 12000 ||
            std::abs(s.Gamepad.sThumbRX) > 12000 || std::abs(s.Gamepad.sThumbRY) > 12000)
            b |= kPadTouched;
    }
    return b;
}

// PlayStation pads over HID. The face-button byte (Square 0x10, Cross 0x20, Circle 0x40, Triangle 0x80) by report, as
// documented by the community (SDL, DS4Windows): DualSense USB 0x01 [8], Bluetooth 0x31 (full mode, as Steam sets it)
// [9], Bluetooth simple 0x01 [5]; DualShock 4 USB 0x01 [5], Bluetooth 0x11 [7].
std::atomic<U8> g_hid_buttons{0};
HANDLE g_hid_wake = nullptr; // set when the menu opens: a thread without a pad looks again

struct PadKind {
    USHORT pid;
    bool dualsense;
};
constexpr USHORT kSonyVid = 0x054C;
constexpr PadKind kPads[] = {{0x0CE6, true}, {0x0DF2, true}, {0x05C4, false}, {0x09CC, false}, {0x0BA0, false}};

// The sticks are the four bytes right after the report id (DualShock 4 over Bluetooth: two later), 0x80 = centred; the
// face byte's low nibble is the d-pad (8 = none), the next two bytes hold the shoulder / stick / menu buttons.
U8 hid_parse(const unsigned char* r, DWORD n, bool dualsense)
{
    int face = -1, sticks = 1;
    if (dualsense) {
        if (r[0] == 0x01 && n >= 64) face = 8;                   // USB
        else if (r[0] == 0x31 && n >= 11) face = 9, sticks = 2;  // Bluetooth, full
        else if (r[0] == 0x01 && n >= 7) face = 5;               // Bluetooth, simple
    } else {
        if (r[0] == 0x01 && n >= 7) face = 5;                    // USB (and Bluetooth simple)
        else if (r[0] == 0x11 && n >= 9) face = 7, sticks = 3;   // Bluetooth
    }
    if (face < 0 || DWORD(face + 2) >= n) return 0;
    U8 b = (r[face] & 0x20) ? kPadCross : 0;
    U8 dpad = r[face] & 0x0F; // (0 up, clockwise to 7 up-left; 8 none)
    if (dpad == 0 || dpad == 1 || dpad == 7) b |= kPadUp;
    if (dpad >= 3 && dpad <= 5) b |= kPadDown;
    if (r[face + 1] & 0x01) b |= kPadL1; // (L1: the first bit of the next byte, DualSense and DualShock 4 alike)
    bool touched = (r[face] & 0xF0) || (r[face] & 0x0F) != 8 || r[face + 1] || (r[face + 2] & 0x03);
    for (int s = 0; s < 4 && !touched; ++s) touched = std::abs(int(r[sticks + s]) - 0x80) > 48;
    return b | (touched ? kPadTouched : 0);
}

// The first connected PlayStation pad: an open handle (shared with the game and Steam) and its input report size.
HANDLE hid_open(bool& dualsense, DWORD& report_len)
{
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA itf{sizeof itf};
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &itf); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &itf, nullptr, 0, &need, nullptr);
        std::vector<char> buf(need);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!need || !SetupDiGetDeviceInterfaceDetailW(set, &itf, detail, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{sizeof at};
        bool ours = false;
        if (HidD_GetAttributes(h, &at) && at.VendorID == kSonyVid)
            for (const PadKind& k : kPads)
                if (at.ProductID == k.pid) ours = true, dualsense = k.dualsense;
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps{};
        if (ours && HidD_GetPreparsedData(h, &pp)) {
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength) {
                report_len = caps.InputReportByteLength;
                found = h;
                LOG("controller input (PlayStation pad): %s, product %04X, reports of %lu bytes",
                    dualsense ? "DualSense" : "DualShock 4", at.ProductID, report_len);
            }
            HidD_FreePreparsedData(pp);
        }
        if (found != h) CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// Waits for a PlayStation pad's reports and keeps its buttons in g_hid_buttons. Without a pad (or once it is gone),
// it waits for the menu to open and looks again.
DWORD WINAPI hid_thread(LPVOID)
{
    t_own_pad = true; // (our reading: never filtered, never taken for the game's)
    for (;;) {
        bool dualsense = false;
        DWORD len = 0;
        HANDLE h = hid_open(dualsense, len);
        if (h == INVALID_HANDLE_VALUE) {
            g_hid_buttons = 0;
            WaitForSingleObject(g_hid_wake, INFINITE);
            continue;
        }
        std::vector<unsigned char> r(len);
        DWORD got = 0;
        while (ReadFile(h, r.data(), len, &got, nullptr)) g_hid_buttons = hid_parse(r.data(), got, dualsense);
        LOG("controller input: the PlayStation pad stopped answering (disconnected?)");
        CloseHandle(h);
        g_hid_buttons = 0;
    }
}

// The pads' buttons, and which input is in use: a touched pad switches to it (its family too); the mouse moving or
// clicking, or Space, switches back. A change asks pump to re-show the slots (g_prompt_changed).
U8 pad_buttons_raw() { return xinput_buttons() | g_hid_buttons.load(); }

U8 pad_buttons()
{
    U8 x = xinput_buttons(), h = g_hid_buttons.load();
    static POINT last{};
    POINT p{};
    bool mouse = GetCursorPos(&p) && (std::abs(p.x - last.x) > 2 || std::abs(p.y - last.y) > 2);
    last = p;
    // A PlayStation pad through Steam Input shows up on both readers; the game then sees the virtual Xbox pad (and shows
    // Xbox prompts), so XInput wins the icon family.
    bool pad = (x | h) & kPadTouched, ps = g_pad_is_ps;
    if (h & kPadTouched) ps = true;
    if (x & kPadTouched) ps = false;
    bool keys = (GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
    bool in_use = pad ? true : (mouse || keys) ? false : g_pad_in_use.load();
    if (in_use != g_pad_in_use || (in_use && ps != g_pad_is_ps)) {
        g_pad_in_use = in_use, g_pad_is_ps = ps;
        g_prompt_changed = true;
    }
    return x | h;
}

// Latches Space / Cross / A presses and left clicks (game window focused, menu open) for the UI thread (pump), and
// follows the game's save files (poll_saves). The keys are only read while the menu is open.
DWORD WINAPI key_thread(LPVOID)
{
    bool space = false, lmb = false, open = false;
    for (U32 tick = 0;; ++tick) {
        Sleep(15);
        if (tick % 16 == 0) poll_saves();
        bool was_open = open;
        open = g_menu_open && !g_name_modal && !g_ask_modal; // not while a prompt is open (re-baselined afterwards)
        if (!open) continue;
        if (!was_open && g_hid_wake) SetEvent(g_hid_wake); // (a PlayStation pad plugged in since: found now)
        U8 pb = pad_buttons();
        bool s = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 || (pb & kPadCross);
        bool m = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (!was_open) space = s, lmb = m; // held while the menu opened: not a press
        if (s && !space && game_focused()) g_space_at = GetTickCount64();
        if (m && !lmb && game_focused()) {
            POINT p;
            HWND w = GetForegroundWindow();
            g_click_pos = GetCursorPos(&p) && ScreenToClient(w, &p) ? (U64(U32(p.x)) << 32) | U32(p.y) : ~0ull;
            g_click_at = GetTickCount64();
        }
        space = s;
        lmb = m;
    }
}

// --- Gear-wheel UI, patched in memory -----------------------------------------------------------
// The game builds its screens from compiled UI files (ui/bin__/*.uibinary). The Outfits tab header and the value text
// are added to the gear wheel's files while the game loads them (uibin_patch.cpp), onto whatever copy it loads: its
// own, or another mod's that DMM chose. The mod ships none of these files, so it doesn't conflict over them, and a game
// update that changes them needs no new version as long as the elements copied are still there.
// The loader reads a file (path from "%#%#%#_%#.uibinary") and wraps the bytes in a stream on its stack
// {vt, .., data +0x20, size u32 +0x28, pos u32 +0x2c} for the parser (its only call). The hook gives the parser a
// patched copy instead (kept for the whole run: what the parser makes may point into it). The path is the loader's
// local: inline, or on the heap when long; where both are relative to the stream is read from the loader's code.
using UiParseFn = U64(__fastcall*)(U64 loader, U64 stream);
UiParseFn g_ui_parse_orig = nullptr;
U64 g_ui_parse_ret = 0;                                // the parser call's return address (the loader's call)
std::int64_t g_ui_path_inline = 0, g_ui_path_heap = 0; // the path buffer / its heap pointer, relative to the stream
std::string g_ui_hook_note;                            // how the hook went in (logged by the init thread)
std::mutex g_ui_mutex;
std::deque<std::vector<U8>> g_ui_patched; // the patched files, alive for the run

bool ui_copy(U64 from, void* to, size_t n)
{
    __try {
        std::memcpy(to, reinterpret_cast<const void*>(from), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string ui_stream_path(U64 stream)
{
    char buf[0x106] = {};
    U64 heap = 0;
    U64 src = rd(stream + g_ui_path_heap, heap) && plausible(heap) ? heap : stream + g_ui_path_inline;
    if (!ui_copy(src, buf, sizeof buf - 1)) return {};
    buf[sizeof buf - 1] = 0;
    for (char* c = buf; *c; ++c)
        if (*c < 0x20 || *c > 0x7e) return {};
    std::string r = buf;
    return r.size() > 9 && _stricmp(r.c_str() + r.size() - 9, ".uibinary") == 0 ? r : std::string();
}

void ui_patch_stream(U64 stream, const std::string& path, unsigned what)
{
    U64 data = 0;
    U32 size = 0, pos = 0;
    if (!rd(stream + 0x20, data) || !rd(stream + 0x28, size) || !rd(stream + 0x2c, pos) || !plausible(data) || !size ||
        size > (64u << 20) || pos) {
        LOG("gear wheel UI: %s left as it is (its stream isn't as expected: %llX, %u bytes, at %u)", path.c_str(), data,
            size, pos);
        return;
    }
    std::vector<U8> in(size), out;
    if (!ui_copy(data, in.data(), size)) {
        LOG("gear wheel UI: %s left as it is (bytes unreadable)", path.c_str());
        return;
    }
    uibin::Report r;
    if (!uibin::patch(in, what, out, r)) {
        LOG("gear wheel UI: %s left as it is: %s", path.c_str(), r.error.c_str());
        return;
    }
    std::lock_guard lock(g_ui_mutex);
    g_ui_patched.push_back(std::move(out));
    const std::vector<U8>& kept = g_ui_patched.back();
    *reinterpret_cast<U64*>(stream + 0x20) = reinterpret_cast<U64>(kept.data());
    *reinterpret_cast<U32*>(stream + 0x28) = U32(kept.size());
    if (r.headers) ++g_ui_tab_added;
    LOG("gear wheel UI: %s: %d tab header(s), %d value text(s), %d peek(s) added (%u -> %zu bytes)", path.c_str(),
        r.headers, r.values, r.peeks, size, kept.size());
}

U64 __fastcall ui_parse_hook(U64 loader, U64 stream)
{
    if (reinterpret_cast<U64>(_ReturnAddress()) == g_ui_parse_ret) {
        try {
            std::string path = ui_stream_path(stream);
            if (unsigned what = path.empty() ? 0 : uibin::wants(path)) ui_patch_stream(stream, path, what);
        } catch (...) { // (out of memory: the file stays as it is)
        }
    }
    return g_ui_parse_orig(loader, stream);
}

// At load (DllMain), before the game builds its UI; again from the init thread if that didn't work.
bool install_ui_patch()
{
    if (g_ui_parse_orig) return true;
    if (!g_base) g_base = game::main_module().base;
    // mov rax,[rsp+70]; mov [rbp+..],rax; mov eax,[rsp+7C]; mov [rbp+..],eax; mov [rbp+..],r12d; lea rdx,[rbp+s];
    // mov rcx,rsi; call parser  (unique in the 10-30, 11-01b and 11-04 exes)
    U64 site = game::find_unique("48 8B 44 24 70 48 89 45 ?? 8B 44 24 7C 89 45 ?? 44 89 65 ?? 48 8D 55 ?? 48 8B CE E8");
    if (!site) return g_ui_hook_note = "the UI loader's parser call not found", false;
    std::int8_t stream_at = 0;
    std::int32_t rel = 0;
    rd(site + 23, stream_at), rd(site + 28, rel);
    // lea r15,[rbp+i]; mov rcx,[rbp+h]; test rcx,rcx; cmovne r15,rcx: the path buffer, inline or on the heap
    const U8 pat[] = {0x4C, 0x8D, 0x7D, 0, 0x48, 0x8B, 0x8D, 0, 0, 0, 0, 0x48, 0x85, 0xC9, 0x4C, 0x0F, 0x45, 0xF9};
    U64 hit = 0;
    int hits = 0;
    for (U64 a = site - 0x600; a < site; ++a) {
        bool ok = true;
        for (size_t k = 0; k < sizeof pat && ok; ++k)
            ok = k == 3 || (k >= 7 && k <= 10) || *reinterpret_cast<const U8*>(a + k) == pat[k];
        if (ok) hit = a, ++hits;
    }
    if (hits != 1) return g_ui_hook_note = "the UI loader's path buffer not found (" + std::to_string(hits) + ")", false;
    std::int8_t inl = 0;
    std::int32_t heap = 0;
    rd(hit + 3, inl), rd(hit + 7, heap);
    g_ui_path_inline = std::int64_t(inl) - stream_at;
    g_ui_path_heap = std::int64_t(heap) - stream_at;
    U64 call = site + 27, parser = call + 5 + rel;
    g_ui_parse_ret = call + 5;
    MH_STATUS init = MH_Initialize();
    void* fn = reinterpret_cast<void*>(parser);
    if ((init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) ||
        MH_CreateHook(fn, reinterpret_cast<void*>(&ui_parse_hook), reinterpret_cast<void**>(&g_ui_parse_orig)) != MH_OK ||
        MH_EnableHook(fn) != MH_OK) {
        g_ui_parse_orig = nullptr;
        return g_ui_hook_note = "the UI parser hook failed", false;
    }
    char t[160];
    std::snprintf(t, sizeof t, "parser +%llX (call +%llX), path at stream%+lld / [stream%+lld]", parser - g_base,
                  call - g_base, g_ui_path_inline, g_ui_path_heap);
    g_ui_hook_note = t;
    return true;
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
extern std::atomic<U32> g_wheel_notches;

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
void wheel_hook(bool on);

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

// The game reads the keyboard and mouse through raw input (GetRawInputData). The peek's key never reaches it while the
// HUD shows the peek (so a key the game also uses, e.g. Tab's Skill 11, does only the peek), and while the peek is open
// the wheel goes to the peek instead of the camera's distance. Nothing of the game's own code is needed for this.
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
GetRawInputDataFn g_raw_input_orig = nullptr;

UINT WINAPI raw_input_hook(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    UINT r = g_raw_input_orig(h, cmd, data, size, header);
    if (cmd != RID_INPUT || !data || r == UINT(-1) || r < sizeof(RAWINPUTHEADER)) return r;
    __try {
        RAWINPUT* in = static_cast<RAWINPUT*>(data);
        U32 vk = g_peek_key.load();
        USHORT mouse = peek_mouse_flags(vk);
        bool hud = GetTickCount64() - g_peek_hud_at.load() < 250 && !g_menu_open && !g_name_modal && !g_ask_modal;
        if (in->header.dwType == RIM_TYPEMOUSE) {
            RAWMOUSE& m = in->data.mouse;
            if (g_peek_live && (m.usButtonFlags & RI_MOUSE_WHEEL)) {
                // (not counted here: the game reads each raw event several times; the hook / window route counts)
                m.usButtonFlags &= ~RI_MOUSE_WHEEL;
                m.usButtonData = 0;
            }
            if (hud && mouse) m.usButtonFlags &= ~mouse;
            for (U32 b : {U32(VK_MBUTTON), U32(VK_XBUTTON1), U32(VK_XBUTTON2)}) // (a hotkey's mouse button)
                if ((m.usButtonFlags & peek_mouse_flags(b)) && hotkey_held_now(b)) m.usButtonFlags &= ~peek_mouse_flags(b);
        } else if (in->header.dwType == RIM_TYPEKEYBOARD) {
            RAWKEYBOARD& k = in->data.keyboard;
            if (hud && vk && !mouse && k.VKey == vk) k.VKey = 0xFF, k.MakeCode = 0, k.Flags &= ~(RI_KEY_E0 | RI_KEY_E1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

// While the arc is open, a low-level mouse hook takes the wheel before anything else sees it (2026-11-06: after using a
// pad, the game in its pad mode passed no wheel to its window, so the window / raw input filters saw none and the arc
// didn't move). Installed when the arc opens, removed when it closes (its own thread: such a hook needs a message loop).
std::atomic<DWORD> g_wheel_thread_id{0};
std::atomic<U32> g_wheel_notches{0}; // notches the hook took this opening (on the close's log line)
HANDLE g_wheel_open_event = nullptr;

LRESULT CALLBACK wheel_ll_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL && g_peek_live && game_focused()) {
        const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        g_peek_wheel += static_cast<SHORT>(HIWORD(m->mouseData));
        ++g_wheel_notches;
        return 1; // (kept from the game: no window message, no raw input)
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI wheel_thread(LPVOID)
{
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE); // (the thread's queue: the close message can be posted to it)
    g_wheel_thread_id = GetCurrentThreadId();
    for (;;) {
        WaitForSingleObject(g_wheel_open_event, INFINITE);
        HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, &wheel_ll_proc, g_self, 0);
        if (!h) LOG("Outfit Arc: wheel hook failed (%lu); the window's wheel messages count instead", GetLastError());
        g_wheel_hooked = h != nullptr;
        // (a close's message can be left over from an opening the hook never saw: it only counts with the arc closed,
        // 2026-11-06: a stale one ended the next opening's hook at once and the wheel did nothing for it)
        while (g_peek_live && GetMessageW(&msg, nullptr, 0, 0) > 0)
            if (msg.message == WM_APP && !g_peek_live) break; // (the arc closed)
        if (h) UnhookWindowsHookEx(h);
        g_wheel_hooked = false;
        while (PeekMessageW(&msg, nullptr, WM_APP, WM_APP, PM_REMOVE)) {} // (none left for the next opening)
    }
}

// The arc opened / closed: the wheel hook goes on / off.
void wheel_hook(bool on)
{
    if (on) {
        if (g_wheel_open_event) SetEvent(g_wheel_open_event);
    } else if (DWORD t = g_wheel_thread_id.load()) {
        PostThreadMessageW(t, WM_APP, 0, 0);
    }
}

void install_raw_input()
{
    g_wheel_open_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, &wheel_thread, nullptr, 0, nullptr)) CloseHandle(t);
    void* target = nullptr;
    if (MH_CreateHookApiEx(L"user32", "GetRawInputData", reinterpret_cast<void*>(&raw_input_hook),
                           reinterpret_cast<void**>(&g_raw_input_orig), &target) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("Outfit Arc: raw input hook FAILED (the key and the wheel also reach the game)");
        return;
    }
    LOG("Outfit Arc: raw input hook on (key %s)", peek_key_name(g_peek_key.load()).c_str());
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

DWORD WINAPI init_thread(LPVOID)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_self, buf, MAX_PATH);
    std::wstring path = buf;
    path.resize(path.find_last_of(L"/\\") + 1);
    g_log_path = path + L"OutfitManager.log";
    logx::open(g_log_path.c_str());
    {
        char ver[32] = "unknown"; // the version resource (outfit_manager.rc)
        DWORD h = 0, n = GetFileVersionInfoSizeW(buf, &h);
        std::vector<char> vi(n);
        VS_FIXEDFILEINFO* f = nullptr;
        UINT len = 0;
        if (n && GetFileVersionInfoW(buf, 0, n, vi.data()) &&
            VerQueryValueW(vi.data(), L"\\", reinterpret_cast<void**>(&f), &len) && f)
            std::snprintf(ver, sizeof ver, "%u.%u.%u", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS),
                          HIWORD(f->dwFileVersionLS));
        LOG("Outfit Manager %s started", ver);
    }
    {
        bool at_load = g_ui_parse_orig != nullptr; // (put in at load; else now, which may be too late for some files)
        bool ok = install_ui_patch();
        LOG("gear wheel UI hook: %s%s", ok ? (at_load ? "in at load, " : "in (late), ") : "NOT in: ", g_ui_hook_note.c_str());
    }
    g_store_path = path + L"OutfitManager.outfits";
    g_cache_path = path + L"OutfitManager.cache"; // game addresses for this exe (rebuilt when the exe changes)
    load_outfits();
    {
        std::string how;
        g_lang_dir = path + L"Outfit Manager\\lang\\";
        g_detected_language = lang::detect(path, how);
        LOG("game language: %s (from %s)%s", g_detected_language.c_str(), how.c_str(),
            g_language.empty() ? "" : (", setting: " + g_language).c_str());
        lang::load(g_lang_dir, g_language.empty() ? g_detected_language : g_language);
    }
    if (!install()) {
        LOG("setup failed; mod inactive");
        return 0;
    }
    install_peek();
    install_raw_input();
    install_pad_filters();
    if (HANDLE t = CreateThread(nullptr, 0, key_thread, nullptr, 0, nullptr)) CloseHandle(t);
    g_hid_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr); // (auto-reset: one look per menu open without a pad)
    if (g_hid_wake)
        if (HANDLE t = CreateThread(nullptr, 0, hid_thread, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}

} // namespace om

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        om::g_self = module;
        DisableThreadLibraryCalls(module);
        if (om::in_game_process()) {
            wchar_t buf[MAX_PATH]; // (the log open now: the gear wheel's files can load before the init thread runs)
            GetModuleFileNameW(module, buf, MAX_PATH);
            std::wstring dir = buf;
            dir.resize(dir.find_last_of(L"/\\") + 1);
            logx::open((dir + L"OutfitManager.log").c_str());
            om::install_ui_patch(); // (before the game builds its UI: the files load once, at start)
        }
        if (om::in_game_process())
            if (HANDLE t = CreateThread(nullptr, 0, om::init_thread, nullptr, 0, nullptr))
                CloseHandle(t);
    }
    return TRUE;
}
