// The gear wheel's hooks: the Outfits tab's record, focus, clicks, scrolling, frames.
#include "om.h"
#include "wheel_hooks.h"
#include "addrs.h"
#include "ui.h"
#include "items.h"
#include "gear.h"
#include "store.h"
#include "looks.h"
#include "looks_view.h"
#include "load.h"
#include "stash.h"
#include "prompts.h"
#include "actions.h"
#include "settings.h"
#include "wheel_draw.h"
#include "pads.h"
#include "selftest.h"
#include "setup.h"

namespace om {

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
        std::uint8_t mode = slot_mode_of(i);
        bool saved = o < g_outfits.size();
        g_center_name = mode == kModeSettings ? std::string(lang::t(saved ? "center.outfit_settings" : "center.settings"))
                        : mode == kModeRelease ? std::string(lang::t(anchor_outfit() >= 0 ? "center.release" : "center.no_outfit"))
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
        std::uint8_t mode = i < visible_slots() ? slot_mode_of(i) : std::uint8_t(kModeMain);
        bool press = click || space;
        if (press && mode == kModeSettings && o <= g_outfits.size()) cfg_enter(self, o); // (the "+" slot: the plugin's)
        else if (press && mode == kModeRelease) { // (the "+" slot's Unequip Outfit, #21; none applied: nothing to do)
            if (anchor_outfit() >= 0) release_outfit(self);
            refresh_wheel(self);
            center_for_focus(self, focused_script(self), true);
        } else if (press && o == g_outfits.size() && o < kOutfitSlots) save_new_outfit(self);
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
    { // an outfit change's effect started / ended: its slot pulses, then the highlights follow
        static int busy_seen = -1;
        if (int b = fx_busy_target(); b != busy_seen) busy_seen = b, apply_outfit_slots(self);
    }
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
        log_unlisted_slots(); // (these two: whenever what they show changed - ammo, the DLC's gear)
        log_weapon_wheel(self);
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

} // namespace om
