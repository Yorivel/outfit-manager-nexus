// A piece taken off the game's presentation while it stays equipped (2026-11-12).
//
// A look used to hide a piece on our visibility layer only: the game still built it, and everything it decides from
// what is built stayed as with the piece on - the body under a hidden chest, the chest's physics part, the rule that
// drops a pack's straps over heavy armour. The game itself separates what is equipped from what is presented: its
// equip slot component (our "inventory") has one central presentation change (kRvaPresentChange: component, int*
// error, {put-on list*, 0}*, u8 0, take-off list*), which equipping, unequipping and its "visual item" functions all
// go through, and a slot refresh (kRvaPresentRefresh: component, slot, the shown item's key, an item) that - with no
// visual item on the slot - puts on nothing and takes off the item made from the key.
//  - taken off: the slot refresh, as the game takes a visual item off an empty slot;
//  - presented again: the presentation change with the equipped item put on and nothing taken off, as the game
//    presents a slot from the equipment (its +A94F60).
// Lists: {entry* ptr, u32 count, u32 cap}; an entry is 0xE0 bytes: u16 slot, u32 (the item info's +8), u64 (the
// item's +0), u16 (the item's +0x40), the item object at +0x18 (kRvaItemInit / kRvaItemCopy / kRvaItemFree).
// The item's stats, durability and place in the inventory are untouched (checked in play: still equipped, same stats).
// What it removes with the piece: its model, its physics part (a plate chest's clinks) and the game's part rules.
// Sounds the game decides from what is EQUIPPED do not follow by themselves:
//  - the footwear's material (kRvaFootwearSound: actor, ?, slot -> a material, 0 with none - its callers then take the
//    character's default): for a character whose boots are off the presentation it is answered "none" here. With that,
//    bumping into a companion whose plate is hidden no longer clashes like metal (confirmed in play, 2026-11-12: gone
//    with this answer, back without it);
//  - a shield's step sound and the played character's own metal knock at a start / stop or a landing: sound.cpp stops
//    those (the knock's source is not found: it stays with every drawn piece off the presentation and with the chest
//    and gloves unequipped - GitHub issue).
// Weapons, tools and the display slots are not taken off (looks.cpp): presenting a weapon again when it is drawn
// rebuilds its part in the sheathed position, and the game draws it a second time (2026-11-12, logged).
// Asked from the looks (looks.cpp, any thread); done on the game thread (a character's material update).
#include "om.h"
#include "addrs.h"
#include "gear.h"
#include "looks.h"
#include "log.h"
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace om {

namespace {

struct PresList { U64 ptr; U32 count, cap; unsigned char store[0xE0 * 2]; };
using PresentFn = int*(__fastcall*)(U64 comp, int* err, void* puton, U8 flag, void* takeoff);
using RefreshFn = void(__fastcall*)(U64 comp, U16 slot, U16 key, void* item);
using ItemFn = void*(__fastcall*)(void*);
using ItemCopyFn = void*(__fastcall*)(void* dst, U64 src);

constexpr U16 kFootwearSlot = 6; // (equipment-6: the boots, for Kliff, Damiane and Oongka)
struct Req { U64 inv; U16 slot; bool take_off; };
// the slots that are off the presentation now, per inventory (set where the change is made, read by the sound hook)
struct OffSlots { std::atomic<U64> inv{0}; std::atomic<U32> mask{0}; };
OffSlots g_off[16];
U32 off_mask(U64 inv)
{
    for (const OffSlots& e : g_off)
        if (e.inv.load() == inv) return e.mask.load();
    return 0;
}
void mark(U64 inv, U16 slot, bool off)
{
    if (slot >= 32) return;
    OffSlots* e = nullptr;
    for (OffSlots& x : g_off)
        if (x.inv.load() == inv) e = &x;
    for (OffSlots& x : g_off)
        if (!e && !x.mask.load()) e = &x, x.inv = inv;
    if (!e) return;
    if (off) e->mask |= 1u << slot;
    else e->mask &= ~(1u << slot);
}
using FootwearFn = U32(__fastcall*)(U64 actor, U64 a2, U64 slot);
FootwearFn g_footwear_orig = nullptr;
U32 __fastcall footwear_hook(U64 actor, U64 a2, U64 slot)
{
    const U32 r = g_footwear_orig(actor, a2, slot);
    const U16 sl = static_cast<U16>(slot) == 0xFFFF ? kFootwearSlot : static_cast<U16>(slot);
    const U64 inv = actor && r ? rp(rp(actor + kOffActorComp) + kOffCompInventory) : 0;
    return inv && sl == kFootwearSlot && ((off_mask(inv) >> sl) & 1) ? 0 : r;
}
std::mutex g_mu;
std::vector<Req> g_reqs;

void fill_entry(unsigned char* e, U16 slot, U64 item) // (an entry for the equipped item, as the game's +A94F60 fills it)
{
    std::memset(e, 0, 0xE0);
    *reinterpret_cast<U16*>(e) = slot;
    reinterpret_cast<ItemFn>(g_base + kRvaItemInit)(e + 0x18);
    U16 key = 0xFFFF, alt = 0xFFFF, w40 = 0;
    rd(item + 8, key), rd(item + 0x90, alt), rd(item + 0x40, w40);
    const U64 info = by_key(reinterpret_cast<RecordGetFn>(g_base + kRvaItemInfo), alt != 0xFFFF ? alt : key);
    U32 v = 0;
    if (info) rd(info + 8, v);
    *reinterpret_cast<U32*>(e + 4) = v;
    *reinterpret_cast<U64*>(e + 8) = rp(item);
    *reinterpret_cast<U16*>(e + 0x10) = w40;
    reinterpret_cast<ItemCopyFn>(g_base + kRvaItemCopy)(e + 0x18, item);
}
int change(U64 comp, U16 slot, U64 item, bool take_off) // -> the game's error code; -1: it faulted
{
    PresList on{}, off{};
    on.ptr = reinterpret_cast<U64>(on.store), on.cap = 2;
    off.ptr = reinterpret_cast<U64>(off.store), off.cap = 2;
    int err = 0;
    __try {
        if (take_off) {
            U16 key = 0xFFFF;
            rd(item + 8, key);
            alignas(16) unsigned char none[0xD0];
            reinterpret_cast<ItemFn>(g_base + kRvaItemInit)(none);
            reinterpret_cast<RefreshFn>(g_base + kRvaPresentRefresh)(comp, slot, key, none);
            reinterpret_cast<ItemFn>(g_base + kRvaItemFree)(none);
        } else {
            fill_entry(on.store, slot, item), on.count = 1;
            struct { PresList* list; U64 zero; } arg{&on, 0};
            reinterpret_cast<PresentFn>(g_base + kRvaPresentChange)(comp, &err, &arg, 0, &off);
            reinterpret_cast<ItemFn>(g_base + kRvaItemFree)(on.store + 0x18);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return err;
}

} // namespace

// (the game's functions found by the resolver: without them a look hides on our layer only, as before)
bool present_on()
{
    static const bool ok = have({kRvaPresentChange, kRvaPresentRefresh, kRvaItemInit, kRvaItemFree, kRvaItemCopy, kRvaItemInfo});
    static bool told = false;
    if (!ok && !told) told = true, LOG("presentation: game functions not found; hidden pieces are hidden on the visibility layer only");
    return ok;
}

// (a piece seen presented again without our asking - the game built it anew: it sounds as worn until taken off again)
void present_seen(U64 inv, U16 slot) { mark(inv, slot, false); }

void present_install()
{
    if (!present_on() || !have({kRvaFootwearSound})) return;
    void* t = reinterpret_cast<void*>(g_base + kRvaFootwearSound);
    const bool ok = MH_CreateHook(t, reinterpret_cast<void*>(&footwear_hook), reinterpret_cast<void**>(&g_footwear_orig)) == MH_OK &&
                    MH_EnableHook(t) == MH_OK;
    if (!ok) LOG("presentation: the footwear sound lookup could not be watched (hidden boots keep their collision sound)");
}

// A piece something keeps presenting again (2026-11-13: Damiane's Visione after a helmet animation, ~25 times a second
// for seconds - its hair flickered): more than kFightMax take-offs of a slot within kFightMs and its take-offs pause
// for kFightPauseMs; the look still hides it on our layer meanwhile.
constexpr int kFightMax = 6;
constexpr U64 kFightMs = 2000, kFightPauseMs = 10000;
struct Fight { U64 since = 0, paused_until = 0; int count = 0; };
std::map<std::pair<U64, U16>, Fight> g_fights;

void present_request(U64 inv, U16 slot, bool take_off)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (take_off) {
        const U64 now = GetTickCount64();
        Fight& f = g_fights[{inv, slot}];
        if (now < f.paused_until) return;
        if (now - f.since > kFightMs) f.since = now, f.count = 0;
        if (++f.count > kFightMax) {
            f.paused_until = now + kFightPauseMs, f.count = 0;
            LOG("presentation: equipment-%u keeps coming back; not taken off for %llu s (hidden on the look layer)", slot,
                kFightPauseMs / 1000);
            return;
        }
    }
    for (Req& r : g_reqs)
        if (r.inv == inv && r.slot == slot) { r.take_off = take_off; return; }
    g_reqs.push_back({inv, slot, take_off});
}

void present_game_thread()
{
    std::vector<Req> reqs;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_reqs.empty()) return;
        reqs.swap(g_reqs);
    }
    static bool faulted = false;
    for (const Req& r : reqs) {
        if (faulted || !present_on() || !rp(r.inv + 8)) continue;
        const U64 list = rp(r.inv + kOffInvEquipList);
        const U64 item = list ? equipped_item(list, r.slot) : 0;
        U16 key = 0xFFFF;
        if (!item || !rd(item + 8, key) || key == 0xFFFF) continue;
        const int err = change(r.inv, r.slot, item, r.take_off);
        if (err != -1) mark(r.inv, r.slot, r.take_off);
        if (err == -1) faulted = true; // (once: no more changes this session, the looks keep hiding on their layer)
        LOG("presentation: equipment-%u (item %u) %s%s", r.slot, key, r.take_off ? "taken off" : "presented again",
            err == -1 ? ": the game's function FAULTED, no more changes this session" : err ? ": the game answered an error" : "");
    }
}

} // namespace om
