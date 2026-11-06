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
#include "prompts.h"
#include "settings.h"
#include "wheel_draw.h"
#include "wheel_hooks.h"
#include "ui_hook.h"
#include "setup.h"

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
