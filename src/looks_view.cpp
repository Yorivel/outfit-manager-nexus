// The appearance view: the wheel showing one outfit's pieces.
#include "om.h"
#include "looks_view.h"
#include "items.h"
#include "gear.h"
#include "store.h"
#include "looks.h"
#include "settings.h"
#include "wheel_draw.h"
#include "wheel_hooks.h"
#include <map>
#include "lang.h"
#include "parts.h"
#include "mem.h"
#include "addrs.h"

namespace om {

// --- Appearance view: the wheel shows one saved outfit's pieces ------------------------------------
// Entered from the outfit settings' Visibility entry. Its pieces run clockwise from the top like the outfits, then the
// page slot and Back (left of the top, where the "+" slot is). The page slot's dots choose Armour / Weapons /
// Accessories (scroll it), a click opens the page; Back returns to the settings. Scrolling a piece moves its look on.
// Closing the menu leaves it.
int g_eye = -1;             // the outfit shown (-1: the outfits)
int g_cfg = -1;             // the settings view's outfit (the saved count: the "+" slot's, the plugin's; -1: none)

std::uint8_t g_eye_step = 0; // 0 Armour, 1 Weapons, 2 Accessories
std::uint8_t g_eye_pick = 0; // the page slot's dots: scrolled to choose, a click opens it (g_eye_step follows)
// Visibility Extras (the view in parts mode): one row per part group of a piece the outfit holds, on the page of the
// piece's slot. A piece's groups come from the model while it is worn (parts.cpp), and are kept per item for the
// session, so an outfit whose pieces were worn before shows them too. A piece with one group has nothing extra.
bool g_eye_extras = false;
int g_extra_piece = -1;
std::vector<ExtraRow> g_extra_rows;
std::map<U64, std::vector<std::string>> g_seen_groups; // item uid -> its part groups (as last worn)

std::vector<ExtraRow> extra_rows(const Outfit& o)
{
    std::vector<ExtraRow> rows;
    const U64 inv = player_inventory(), actor = inv ? rp(inv + 8) : 0, model = actor ? look_model(actor) : 0;
    const U64 list = inv ? rp(inv + kOffInvEquipList) : 0;
    std::vector<OutfitItem> items = o.items;
    std::sort(items.begin(), items.end(), [](const OutfitItem& a, const OutfitItem& b) { return a.slot < b.slot; });
    for (const OutfitItem& it : items) {
        if (it.key == 0xFFFF || !it.uid) continue;
        U64 e = list ? equipped_item(list, it.slot) : 0, uid = 0;
        if (e && model && rd(e, uid) && uid == it.uid) { // (worn: its groups as the model has them now)
            std::vector<std::string> g;
            for (const ItemPart& p : item_parts(inv, model, it.slot))
                if (std::find(g.begin(), g.end(), part_group(p.name)) == g.end()) g.push_back(part_group(p.name));
            if (!g.empty()) g_seen_groups[it.uid] = g;
        }
        auto f = g_seen_groups.find(it.uid);
        if (f == g_seen_groups.end() || f->second.size() < 2) continue;
        for (const std::string& g : f->second) rows.push_back({it.slot, it.key, it.uid, g});
    }
    return rows;
}
bool has_extras(const Outfit& o) { return parts_on() && !extra_rows(o).empty(); }
// A part group's name for the player: its line ("part.Upperbody_Acc"), or the group's own words (a modded item's part).
std::string part_label(const std::string& group)
{
    // (a weapon's scabbard / case, and the weapon itself, whatever its kind and side: one line each)
    const bool weapon = !group.compare(0, 11, "MainWeapon_") || !group.compare(0, 14, "TwoHandWeapon_");
    if (weapon && group.find("_IN") != std::string::npos) return lang::t("part.scabbard");
    const std::string k = "part." + group;
    const char* t = lang::t(k.c_str());
    if (t == k.c_str() && weapon) return lang::t("part.weapon");
    if (t != k.c_str()) return t;
    std::string s = group;
    for (char& c : s) if (c == '_') c = ' ';
    return s;
}
std::string extra_key(const ExtraRow& r) { return std::to_string(r.slot) + ":" + r.group; }
// The page a slot's rows go on: 0 Armour, 1 Weapons, 2 Accessories (as the pieces).
std::uint8_t slot_step(U16 slot)
{
    return is_weapon_slot(slot) || is_display_slot(slot) ? 1 : is_accessory_slot(slot) ? 2 : 0;
}

bool is_accessory_slot(U16 slot) { return std::find(std::begin(kAccessorySlots), std::end(kAccessorySlots), slot) != std::end(kAccessorySlots); }

// The outfit the view edits: g_outfits[g_eye], or with g_eye the saved count (the "+" slot's, as g_cfg) the character's
// No Outfit looks (#33).
bool eye_no_outfit() { return g_eye >= 0 && size_t(g_eye) == g_outfits.size(); }
Outfit& eye_outfit() { return eye_no_outfit() ? store_for(g_character).no_outfit : g_outfits[size_t(g_eye)]; }

// The shown page's slots: every slot of its wheel, in slot order, held by the outfit or not (the Weapons page also the
// back shield).
std::vector<size_t> eye_pieces()
{
    std::vector<size_t> v;
    if (g_eye < 0 || size_t(g_eye) > g_outfits.size()) return v;
    if (g_eye_extras) { // (parts mode: the page's pieces with extra parts; a piece opened: its part groups)
        for (size_t r = 0; r < g_extra_rows.size(); ++r) {
            const U16 sl = g_extra_rows[r].slot;
            if (g_extra_piece >= 0) {
                if (sl == g_extra_piece) v.push_back(kExtraBase + r);
            } else if (slot_step(sl) == g_eye_step && std::find(v.begin(), v.end(), kExtraPieceBase + sl) == v.end()) {
                v.push_back(kExtraPieceBase + sl);
            }
        }
        return v;
    }
    if (g_eye_step == 1) {
        for (U32 i = 0; i < g_weapon_slot_count; ++i)
            if (g_weapon_slots[i] != 0xFFFF) v.push_back(g_weapon_slots[i]);
    } else {
        for (U32 i = 0; i < g_armor_slot_count; ++i)
            if (is_accessory_slot(g_armor_slots[i]) == (g_eye_step != 0)) v.push_back(g_armor_slots[i]);
    }
    std::sort(v.begin(), v.end());
    if (g_eye_step == 1) { // (the display slots, after the wheel's: the carried weapon, the back shield, the carried ranged one)
        if (display_item(kCarriedSlotA)) v.push_back(kCarriedSlotA);
        v.push_back(kBackPiece);
        if (display_item(kCarriedSlotB)) v.push_back(kCarriedSlotB);
    }
    if (g_eye_step == 0) v.push_back(kUnderwearPiece); // (the Armour page: the underwear last)
    return v;
}

bool outfit_has_slot(const Outfit& o, U16 slot)
{
    for (const OutfitItem& it : o.items)
        if (it.slot == slot) return true;
    return false;
}

// The look of a slot in an outfit (the slot's look, whatever is worn there).
std::uint8_t slot_look(const Outfit& o, U16 slot)
{
    for (const OutfitItem& it : o.items)
        if (it.slot == slot) return it.look;
    auto f = o.empty_looks.find(slot);
    return f != o.empty_looks.end() ? f->second : std::uint8_t(kLookDefault);
}

// A slot of the view: the outfit's item there; nothing saved there: an empty item with the slot's look; (kBackPiece)
// the back shield as an item: what its slot holds now (for the icon and name) with the outfit's back look.

const OutfitItem* eye_item(size_t idx, OutfitItem& tmp)
{
    Outfit& o = eye_outfit();
    if (is_extra_piece(idx)) // (a piece with extra parts: the piece, no look of its own here)
        for (const ExtraRow& r : g_extra_rows)
            if (r.slot == idx - kExtraPieceBase) return &(tmp = {r.slot, r.key, r.uid, kLookDefault});
    if (is_extra_row(idx) && idx - kExtraBase < g_extra_rows.size()) { // (a part group: its piece, with the group's look)
        const ExtraRow& r = g_extra_rows[idx - kExtraBase];
        auto f = o.part_looks.find(extra_key(r));
        tmp = {r.slot, r.key, r.uid, f != o.part_looks.end() ? f->second : std::uint8_t(kLookDefault)};
        return &tmp;
    }
    if (idx == kUnderwearPiece) {
        tmp = {kUnderwearSlot, 0xFFFF, 0, o.underwear_look};
        return &tmp;
    }
    if (idx != kBackPiece) {
        for (const OutfitItem& it : o.items)
            if (it.slot == idx) return &it;
        if (idx == kCarriedSlotA || idx == kCarriedSlotB || eye_no_outfit()) { // (what the slot holds now, with the
                                                                               // slot's look: no outfit's are all so)
            U16 key = 0xFFFF;
            U64 uid = 0;
            if (U64 e = display_item(static_cast<U16>(idx))) rd(e + 8, key), rd(e, uid);
            tmp = {static_cast<U16>(idx), key, uid, slot_look(o, static_cast<U16>(idx))};
            return &tmp;
        }
        tmp = {static_cast<U16>(idx), 0xFFFF, 0, slot_look(o, static_cast<U16>(idx))};
        return &tmp;
    }
    U64 e = back_shield_item();
    U16 key = 0xFFFF;
    U64 uid = 0;
    if (e) rd(e + 8, key), rd(e, uid);
    tmp = {kBackShieldSlot, key, uid, o.back_look};
    return &tmp;
}
std::uint8_t& eye_look(size_t idx)
{
    Outfit& o = eye_outfit();
    if (is_extra_row(idx) && idx - kExtraBase < g_extra_rows.size()) return o.part_looks[extra_key(g_extra_rows[idx - kExtraBase])];
    static std::uint8_t none = kLookDefault; // (a piece in parts mode: no look to change)
    if (is_extra_piece(idx)) return none = kLookDefault;
    if (idx == kBackPiece) return o.back_look;
    if (idx == kUnderwearPiece) return o.underwear_look;
    for (OutfitItem& it : o.items)
        if (it.slot == idx) return it.look;
    return o.empty_looks[static_cast<U16>(idx)];
}

// --- Outfit pages (#32) ----------------------------------------------------------------------------------------------
// Up to kPagelessMax outfits: one page, as before. More: page p shows outfits p*kPageOutfits.., then the page slot (its
// dots are the pages: scrolled to choose, a click opens it), then the "+" slot. The wheel opens on the character's last
// page, unless the applied outfit changed since it was left (a hotkey, the Arc, sleepwear, a load): then on its page.
U32 g_page = 0, g_page_pick = 0;
std::map<U16, std::pair<U32, int>> g_char_page; // character -> (the page left, the applied outfit then)
bool paged() { return g_outfits.size() > kPagelessMax; }
U32 page_count() { return paged() ? U32((g_outfits.size() + kPageOutfits - 1) / kPageOutfits) : 1; }
U32 page_of(size_t o) { return paged() && !g_outfits.empty() ? U32(std::min(o, g_outfits.size() - 1) / kPageOutfits) : 0; }
void page_clamp()
{
    const U32 n = page_count();
    if (g_page >= n) g_page = n - 1;
    if (g_page_pick >= n) g_page_pick = g_page;
}
U32 page_len()
{
    if (!paged()) return g_outfit_count;
    page_clamp();
    return std::min<U32>(kPageOutfits, U32(g_outfits.size()) - g_page * kPageOutfits);
}
void page_show(U32 p)
{
    g_page = g_page_pick = p;
    page_clamp();
}
void pages_on_open()
{
    const int applied = anchor_outfit();
    auto it = g_char_page.find(g_character);
    U32 p = it != g_char_page.end() ? it->second.first : 0;
    if ((it == g_char_page.end() || it->second.second != applied) && applied >= 0) p = page_of(size_t(applied));
    page_show(p);
    if (paged()) LOG("outfit pages: %u, opening on page %u", page_count(), g_page + 1);
}
void pages_on_close() { g_char_page[g_character] = {g_page, anchor_outfit()}; }

// Saved outfits plus one empty "+" slot, until all 12 are used (with pages: the page's outfits, the page slot and the
// "+" slot); in the Appearance view the pieces, the page slot and Back (the fullest page, Weapons: 8 slots and the back
// shield, 11 in all).
U32 visible_slots()
{
    if (g_eye >= 0) return static_cast<U32>(eye_pieces().size()) + 2;
    if (g_cfg >= 0) return static_cast<U32>(cfg_entries().size());
    if (paged()) return page_len() + 2;
    return g_outfit_count < kOutfitSlots ? g_outfit_count + 1 : kOutfitSlots;
}

// Visible slot i sits at (i+1)*360/N, so slot N-1 is the top one: outfits start there and run clockwise,
// which puts the "+" slot (outfit index = saved count) last, just left of the top. On the outfits' wheel with pages:
// the outfit's own index, kPageSlot for the page slot (before the "+").
U32 slot_outfit(U32 i)
{
    const U32 pos = (i + 1) % visible_slots();
    if (g_eye >= 0 || g_cfg >= 0 || !paged()) return pos;
    const U32 len = page_len();
    return pos < len ? g_page * kPageOutfits + pos : pos == len ? kPageSlot : U32(g_outfits.size());
}

// --- Appearance view: entering, paging, leaving, and the presses in it -----------------------------

void eye_refresh(U64 self)
{
    g_force_images = 3; // (the game resets the focused slot's picture: set again for a few frames)
    if (g_eye >= 0) { // (the page's pieces, for the log)
        std::string s;
        for (size_t k : eye_pieces()) {
            OutfitItem tmp{};
            const OutfitItem& it = *eye_item(k, tmp);
            s += " equipment-" + std::to_string(it.slot) + " \"" + (it.key != 0xFFFF ? item_name(it.key) : std::string()) + "\"";
        }
        LOG("appearance view page %u:%s", g_eye_step, s.empty() ? " (no pieces)" : s.c_str());
    }
    refresh_wheel(self);
    center_for_focus(self, focused_script(self), true);
}

void eye_enter(U64 self, U32 o, bool extras)
{
    g_eye = static_cast<int>(o), g_eye_step = 0, g_eye_pick = 0;
    g_eye_extras = extras && !eye_no_outfit(), g_extra_piece = -1;
    g_extra_rows = g_eye_extras ? extra_rows(g_outfits[o]) : std::vector<ExtraRow>{};
    if (g_eye_extras) { // (open on the first page that has rows)
        for (std::uint8_t st = 0; st < kEyeSteps; ++st) {
            bool any = false;
            for (const ExtraRow& r : g_extra_rows) any |= slot_step(r.slot) == st;
            if (any) { g_eye_step = g_eye_pick = st; break; }
        }
        std::string s;
        for (const ExtraRow& r : g_extra_rows) s += " " + extra_key(r);
        LOG("visibility extras: \"%s\":%s", g_outfits[o].name.c_str(), s.empty() ? " (none)" : s.c_str());
    }
    LOG("appearance view: %s", eye_no_outfit() ? "no outfit (#33)" : ("\"" + g_outfits[o].name + "\"").c_str());
    eye_refresh(self);
}

void eye_leave(U64 self)
{
    LOG("appearance view: back to the settings");
    U32 o = static_cast<U32>(g_eye);
    g_eye = -1, g_eye_step = 0, g_eye_extras = false, g_extra_piece = -1;
    g_extra_rows.clear();
    g_center_desc.clear();
    cfg_enter(self, o); // (entered from there)
}

// A scroll on slot i of the view: a piece's look moves on / back (Default, Hidden, Combat only, Out of combat only,
// Shown: its dots), the page slot chooses Armour / Weapons / Accessories (a click opens it); Back doesn't scroll.
void eye_scroll(U64 self, U32 i, int step)
{
    std::vector<size_t> pieces = eye_pieces();
    U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (k < pieces.size() && is_extra_piece(pieces[k])) return; // (a piece in parts mode: a click opens it)
    if (k < pieces.size()) {
        Outfit& o = eye_outfit();
        std::uint8_t& look = eye_look(pieces[k]);
        look = kLookOrder[(look_pos(look) + kLookCount + step) % kLookCount];
        OutfitItem tmp{};
        const OutfitItem& it = *eye_item(pieces[k], tmp);
        LOG("appearance: \"%s\" equipment-%u %s%s -> look %u", o.name.c_str(), it.slot, (it.key != 0xFFFF ? item_name(it.key) : std::string("(empty)")).c_str(),
            is_extra_row(pieces[k]) ? (" part " + g_extra_rows[pieces[k] - kExtraBase].group).c_str() : "", look);
        save_outfits(); // (publishes the looks)
        look_refresh(); // (the worn outfit's pieces change at once)
        apply_outfit_slots(self, i);
        center_for_focus(self, focused_script(self), true);
    } else if (k == pieces.size()) { // (the page slot: scrolling chooses, a click opens it)
        g_eye_pick = std::uint8_t((g_eye_pick + kEyeSteps + step) % kEyeSteps);
        apply_outfit_slots(self, i);
        center_for_focus(self, focused_script(self), true);
    }
}

// A click / Space on slot i of the view: the page slot opens the page chosen (or the next one), Back returns to the
// settings (pieces: scrolled).
void eye_press(U64 self, U32 i)
{
    std::vector<size_t> pieces = eye_pieces();
    U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (k < pieces.size() && is_extra_piece(pieces[k])) { // (parts mode: the piece's parts open)
        g_extra_piece = int(pieces[k] - kExtraPieceBase);
        LOG("visibility extras: equipment-%d's parts", g_extra_piece);
        eye_refresh(self);
    } else if (k == pieces.size()) {
        const bool same = g_eye_pick == g_eye_step && g_extra_piece < 0;
        g_eye_step = g_eye_pick = same ? std::uint8_t((g_eye_step + 1) % kEyeSteps) : g_eye_pick;
        g_extra_piece = -1;
        LOG("appearance view: step %u", g_eye_step);
        eye_refresh(self);
    } else if (k == pieces.size() + 1) {
        if (g_extra_piece >= 0) g_extra_piece = -1, eye_refresh(self); // (a piece's parts: back to the page's pieces)
        else eye_leave(self);
    }
}

} // namespace om
