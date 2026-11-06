// The appearance view: the wheel showing one outfit's pieces.
#include "om.h"
#include "looks_view.h"
#include "items.h"
#include "store.h"
#include "looks.h"
#include "om_rest.h"

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
bool is_accessory_slot(U16 slot) { return std::find(std::begin(kAccessorySlots), std::end(kAccessorySlots), slot) != std::end(kAccessorySlots); }

// The shown page's slots: every slot of its wheel, in slot order, held by the outfit or not (the Weapons page also the
// back shield).
std::vector<size_t> eye_pieces()
{
    std::vector<size_t> v;
    if (g_eye < 0 || size_t(g_eye) >= g_outfits.size()) return v;
    if (g_eye_step == 1) {
        for (U32 i = 0; i < g_weapon_slot_count; ++i)
            if (g_weapon_slots[i] != 0xFFFF) v.push_back(g_weapon_slots[i]);
    } else {
        for (U32 i = 0; i < g_armor_slot_count; ++i)
            if (is_accessory_slot(g_armor_slots[i]) == (g_eye_step != 0)) v.push_back(g_armor_slots[i]);
    }
    std::sort(v.begin(), v.end());
    if (g_eye_step == 1) v.push_back(kBackPiece);
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
    Outfit& o = g_outfits[size_t(g_eye)];
    if (idx != kBackPiece) {
        for (const OutfitItem& it : o.items)
            if (it.slot == idx) return &it;
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
    Outfit& o = g_outfits[size_t(g_eye)];
    if (idx == kBackPiece) return o.back_look;
    for (OutfitItem& it : o.items)
        if (it.slot == idx) return it.look;
    return o.empty_looks[static_cast<U16>(idx)];
}

// Saved outfits plus one empty "+" slot, until all 12 are used; in the Appearance view the pieces, the page slot and
// Back (the fullest page, Weapons: 8 slots and the back shield, 11 in all).
U32 visible_slots()
{
    if (g_eye >= 0) return static_cast<U32>(eye_pieces().size()) + 2;
    if (g_cfg >= 0) return static_cast<U32>(cfg_entries().size());
    return g_outfit_count < kOutfitSlots ? g_outfit_count + 1 : kOutfitSlots;
}

// Visible slot i sits at (i+1)*360/N, so slot N-1 is the top one: outfits start there and run clockwise,
// which puts the "+" slot (outfit index = saved count) last, just left of the top.
U32 slot_outfit(U32 i) { return (i + 1) % visible_slots(); }

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

void eye_enter(U64 self, U32 o)
{
    g_eye = static_cast<int>(o), g_eye_step = 0, g_eye_pick = 0;
    LOG("appearance view: \"%s\"", g_outfits[o].name.c_str());
    eye_refresh(self);
}

void eye_leave(U64 self)
{
    LOG("appearance view: back to the settings");
    U32 o = static_cast<U32>(g_eye);
    g_eye = -1, g_eye_step = 0;
    g_center_desc.clear();
    cfg_enter(self, o); // (entered from there)
}

// A scroll on slot i of the view: a piece's look moves on / back (Default, Hidden, Combat only, Out of combat only,
// Shown: its dots), the page slot chooses Armour / Weapons / Accessories (a click opens it); Back doesn't scroll.
void eye_scroll(U64 self, U32 i, int step)
{
    std::vector<size_t> pieces = eye_pieces();
    U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (k < pieces.size()) {
        Outfit& o = g_outfits[size_t(g_eye)];
        std::uint8_t& look = eye_look(pieces[k]);
        look = kLookOrder[(look_pos(look) + kLookCount + step) % kLookCount];
        OutfitItem tmp{};
        const OutfitItem& it = *eye_item(pieces[k], tmp);
        LOG("appearance: \"%s\" equipment-%u %s -> look %u", o.name.c_str(), it.slot, (it.key != 0xFFFF ? item_name(it.key) : std::string("(empty)")).c_str(), look);
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
    if (k == pieces.size()) {
        g_eye_step = g_eye_pick = g_eye_pick != g_eye_step ? g_eye_pick : std::uint8_t((g_eye_step + 1) % kEyeSteps);
        LOG("appearance view: step %u", g_eye_step);
        eye_refresh(self);
    } else if (k == pieces.size() + 1) {
        eye_leave(self);
    }
}

} // namespace om
