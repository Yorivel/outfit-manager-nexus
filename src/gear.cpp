// Worn gear, the player and the characters, the bags and where pieces are.
#include "om.h"
#include "gear.h"
#include "addrs.h"
#include "items.h"
#include "store.h"
#include "looks.h"
#include "looks_view.h"
#include "load.h"
#include "stash.h"
#include "wheel_draw.h"

namespace om {

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

// Equipped items in slots on neither wheel (an outfit never records them): logged by name, to learn which slots ammo
// and the DLC's gear use (bug report 2026-11-08: Damiane's harpoon and bullets aren't switched).
U64 equipment_list();
const char* character_name(U16 key);
bool is_placeholder(U16 key);
void log_unlisted_slots()
{
    U64 list = equipment_list();
    U64 e = list ? rp(list + 8) : 0;
    U32 n = list ? r32(list + 0x10) : 0;
    { // (again only when it changed)
        static U64 last = 0;
        U64 sig = 1469598103934665603ull ^ g_character;
        for (U32 i = 0; e && i < n && i < 64; ++i) {
            U64 uid = 0;
            U16 slot = 0;
            if (!rd(e + i * 0xd0ull, uid) || !rd(e + i * 0xd0ull + kOffEntrySlot, slot)) break;
            sig = (sig ^ uid ^ (U64(slot) << 48)) * 1099511628211ull;
        }
        if (sig == last) return;
        last = sig;
    }
    LOG("equipped, on no wheel (%s):", character_name(g_character));
    for (U32 i = 0; e && i < n && i < 64; ++i) {
        U16 key = 0xFFFF, slot = 0xFFFF, at1 = 0;
        U64 uid = 0;
        U64 x = e + i * 0xd0ull;
        if (!rd(x + 8, key) || !rd(x, uid) || !rd(x + kOffEntrySlot, slot) || !rd(x + 0xca, at1)) break;
        if (key == 0xFFFF || !uid || slot == 0xFFFF || is_weapon_slot(slot)) continue;
        bool armour = false;
        for (U32 k = 0; k < g_armor_slot_count; ++k) armour |= g_armor_slots[k] == slot;
        if (armour) continue;
        LOG("  (not on a wheel) equipment-%u key %u \"%s\" uid %llX, +CA %u%s", slot, key, item_name(key).c_str(), uid, at1,
            is_placeholder(key) ? " (placeholder)" : "");
    }
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

// The Weapons wheel's positions as the menu holds them (its slot scripts, record 0): what each shows, also the ones
// with no equip slot (ammo is one of them - bug report 2026-11-08). For the log only.
void log_weapon_wheel_body(U64 self)
{
    {
        U64 sv = self ? *reinterpret_cast<U64*>(self + kOffRootSlotVecs) : 0;
        if (!sv || *reinterpret_cast<U32*>(self + kOffRootSlotVecCount) == 0) {
            LOG("weapon wheel: no slot scripts");
            return;
        }
        U64 arr = *reinterpret_cast<U64*>(sv + 0);
        U32 cnt = *reinterpret_cast<U32*>(sv + 8);
        U64 empty = *reinterpret_cast<U64*>(g_base + kRvaEmptyItem);
        { // (logged again only when something on it changed: every open would flood the log)
            static U64 last = 0;
            U64 sig = 1469598103934665603ull ^ g_character;
            for (U32 i = 0; arr && i < cnt && i < 16; ++i) {
                U64 script = *reinterpret_cast<U64*>(arr + i * 8ull);
                U64 data = script ? *reinterpret_cast<U64*>(script + kOffScriptData) : 0;
                for (int alt = 0; data && alt < 2; ++alt) {
                    U64 item = data + (alt ? kOffDataItemAlt : kOffDataItem);
                    if (item_valid(item, empty)) sig = (sig ^ *reinterpret_cast<U64*>(item) ^ (U64(*reinterpret_cast<U16*>(item + 8)) << 48)) * 1099511628211ull;
                }
            }
            if (sig == last) return;
            last = sig;
        }
        LOG("weapon wheel (%s): %u positions", character_name(g_character), cnt);
        for (U32 i = 0; arr && i < cnt && i < 16; ++i) {
            U64 script = *reinterpret_cast<U64*>(arr + i * 8ull);
            U64 data = script ? *reinterpret_cast<U64*>(script + kOffScriptData) : 0;
            if (!data) {
                LOG("  position %u: script %llX, no data", i, script);
                continue;
            }
            U16 qslot = *reinterpret_cast<U16*>(script + kOffScriptEquipIdx);
            unsigned type = *reinterpret_cast<std::uint8_t*>(data + kOffDataType);
            for (int alt = 0; alt < 2; ++alt) {
                U64 item = data + (alt ? kOffDataItemAlt : kOffDataItem);
                bool valid = item_valid(item, empty);
                U16 key = valid ? *reinterpret_cast<U16*>(item + 8) : 0xFFFF;
                U64 uid = valid ? *reinterpret_cast<U64*>(item) : 0;
                LOG("  position %u (equip idx %u, type %u) %s: %s key %u \"%s\" uid %llX", i, qslot, type,
                    alt ? "alt item" : "item", valid ? "valid" : "empty", key, key != 0xFFFF ? item_name(key).c_str() : "", uid);
            }
            std::string raw; // (the data's first bytes: where else an ammo choice could sit)
            char b[8];
            for (U32 k = 0; k < 0x60; ++k) {
                snprintf(b, sizeof b, "%02X%s", *reinterpret_cast<std::uint8_t*>(data + k), (k & 7) == 7 ? " " : "");
                raw += b;
            }
            LOG("    data %llX: %s", data, raw.c_str());
        }
    }
}
void log_weapon_wheel(U64 self) // (the body has strings: the fault guard lives here)
{
    __try {
        log_weapon_wheel_body(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("weapon wheel: fault while reading");
    }
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
    { // (the worn chest for the "+" slot's picture, whatever is worn; none: the placeholder)
        g_worn_chest.clear();
        WornRaw wc[kWornMax];
        int nc = read_worn(self, wc, kWornMax);
        for (int k = 0; k < nc; ++k)
            if (wc[k].slot == kChestSlot && wc[k].key != 0xFFFF && wc[k].icon) g_worn_chest = cstr(reinterpret_cast<U64>(wc[k].icon));
    }
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
// Bags loading takes pieces from: the ones on you, storage and the wardrobe (wherever a piece was put away, by Stash or
// by hand, loading finds it). Companions go through the game's UI requests, which find the item in its own bag (from
// the item type): storage / the wardrobe only with the request's bag set (see call_equip_request).
U64 equip_bags(bool companion)
{
    U64 m = bag_bit(kBagCarried) | bag_bit(kBagKeyItems);
    if (!companion || companion_pull_ok()) m |= bag_bit(kBagStorage) | bag_bit(kBagWardrobe);
    return m;
}

// The bag holding uid, among the bags in `allowed` (bag_bit mask; 0 = any). With a key, the item must also be that item:
// uids are handed out again after loading an older save (the counter is saved with it), so a stale uid can name
// another item.
bool find_in_bags(U64 mgr, U64 uid, ItemAt& at, U64 allowed, U16 key)
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

// Bag entries that carry this uid (any item, count > 0), described in `where` ("bag 1 pos 419: key 5887 x3; ...").
// A worn piece is never in a bag: a bag entry with its uid means two things share one id (seen with a stacking mod's
// stacks of gear, #1). Taking such a piece off loses it: the game merges it into the entry with the same id.
int shared_id_in_bags(U64 mgr, U64 uid, std::string* where, long long* copies)
{
    int found = 0;
    if (copies) *copies = 0;
    __try {
        U64 bags = *reinterpret_cast<U64*>(mgr + kOffMgrBags);
        U32 n = *reinterpret_cast<U32*>(mgr + 0x20);
        for (U32 i = 0; bags && i < n && i < 64; ++i) {
            U64 bag = *reinterpret_cast<U64*>(bags + i * 8ull);
            U64 items = bag ? *reinterpret_cast<U64*>(bag) : 0;
            short cap = items ? *reinterpret_cast<short*>(bag + kOffBagCount) : 0;
            for (short p = 0; p < cap && p < 4096; ++p) {
                U64 it = items + p * 200ull;
                U16 k = *reinterpret_cast<U16*>(it + 8);
                long long cnt = *reinterpret_cast<long long*>(it + 0x10);
                if (*reinterpret_cast<U64*>(it) != uid || k == 0xFFFF || cnt <= 0) continue;
                ++found;
                if (copies) *copies += cnt;
                if (where && where->size() < 400) {
                    char b[96];
                    std::snprintf(b, sizeof b, "%sbag %u pos %d: key %u x%lld", where->empty() ? "" : "; ",
                                  *reinterpret_cast<U16*>(bag + 0x10), p, k, cnt);
                    *where += b;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return found;
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

} // namespace om
