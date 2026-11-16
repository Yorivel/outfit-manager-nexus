// Visibility Extras: the model parts a worn piece brings, by the game's own data (2026-11-16).
//
// An item is built from prefabs (the blade, its scabbard; a chest's body, trim and pauldrons); each prefab puts parts
// on the character model. The game's slot handle function (kRvaSlotHandle) asks for them in this order, and so do we:
//  - the slot's entry in the equipment list (inv+A0: {.., +8 entries*, +10 count}; 0xD0 each, slot u16 +C8, the
//    shown item's key +90, else the equipped one's +8);
//  - the character's body (kRvaPartsBody on [[[inv+8]+68]+20]+30: its id +8) and the item's info (kRvaItemInfo on &key);
//  - kRvaPrefabIds(info, body id, slot, &{0, &vec}): the item's prefab ids (u16) into vec {u16*, u32 count};
//  - per prefab id: kRvaStringInfoGet(&id) -> its record; the map [[[G+D8]+8]+2C0]+40068 (+50) (G at kRvaPartsGlobal),
//    looked up (kRvaPartsMapFind) by record+10 -> a node -> {handle entries*, u32 count}: 8 bytes each, the part handle
//    in the low dword.
// Every address and offset (values above: game 2.05) comes from the resolver, each offset witnessed at the slot handle
// function's own instruction that uses it, so the chain follows the game; anything not found or not certain and the
// feature stays off (an older call shape - 2.04's body getter took a u16 by value - doesn't match those witnesses).
// Handles are those the visibility calls take; a part's name is the model's (its part table). The prefab id vector the
// game allocates isn't freed (a few bytes per item seen; cached per item and model state).
#include "om.h"
#include "parts.h"
#include "addrs.h"
#include "looks.h"
#include "log.h"
#include "mem.h"
#include "items.h"
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace om {

namespace {

using BodyFn = U64(__fastcall*)(U64);
using ItemInfoFn2 = U64(__fastcall*)(U16*);
using PrefabsFn = void(__fastcall*)(U64 info, U32 body, U32 slot, void* out);
using PrefabRecFn = U64(__fastcall*)(U16*);
using MapFindFn = U64(__fastcall*)(U64 map, U64 key);

struct Chain {
    BodyFn body = nullptr;
    ItemInfoFn2 info = nullptr;
    PrefabsFn prefabs = nullptr;
    PrefabRecFn rec = nullptr;
    MapFindFn find = nullptr;
    U64 global = 0; // (the address holding G)
} g_chain;
bool g_ok = false;

struct Vec { U64 ptr; U32 count, cap; };

U16 r16(U64 at)
{
    U16 v = 0xFFFF;
    rd(at, v);
    return v;
}

// The key of the item the slot shows (its shown item, else the equipped one), as the slot handle function picks it;
// 0xFFFF: nothing equipped there.
U16 shown_key(U64 inv, U16 slot)
{
    const U64 list = rp(inv + kOffInvEquipList), arr = list ? rp(list + kOffEquipListEntries) : 0;
    const U32 n = list ? r32(list + kOffEquipListCount) : 0;
    for (U32 i = 0; arr && i < n && i < 64; ++i) {
        const U64 e = arr + i * kOffEquipEntrySize;
        if (r16(e + kOffEntrySlot) != slot) continue;
        if (r16(e + kOffEntryKey) == 0xFFFF) return 0xFFFF;
        const U16 shown = r16(e + kOffEntryShownKey);
        return shown != 0xFFFF ? shown : r16(e + kOffEntryKey);
    }
    return 0xFFFF;
}

U32 prefab_ids_raw(U64 inv, U16 slot, U16 key, U16* out, U32 cap)
{
    U32 got = 0;
    __try {
        const U64 a = rp(rp(rp(inv + kOffInvActor) + kOffActorComp) + kOffPartsComp);
        if (!a) return 0;
        const U64 body = g_chain.body(a + kOffPartsBodyArg);
        const U32 b = body ? r32(body + kOffPartsBodyId) : 0;
        const U64 info = g_chain.info(&key);
        if (!info) return 0;
        Vec v{};
        struct { U64 zero; Vec* vec; } arg{0, &v};
        g_chain.prefabs(info, b, slot, &arg);
        for (U32 i = 0; v.ptr && i < v.count && got < cap; ++i) out[got++] = reinterpret_cast<U16*>(v.ptr)[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return got;
}

U32 prefab_handles_raw(U16 id, int* out, U32 cap)
{
    U32 got = 0;
    __try {
        const U64 rec = g_chain.rec(&id);
        const U64 g = rec ? rp(g_chain.global) : 0, x = g ? rp(g + kOffPartsG) : 0;
        U64 m = x ? rp(rp(x + kOffPartsG1) + kOffPartsG2) : 0;
        m = m ? rp(m + kOffPartsG3) : 0;
        if (!m) return 0;
        const U64 node = g_chain.find(m + kOffPartsMap, rec + kOffPrefabRecKey);
        const U64 v = node ? rp(node) : 0;
        const U64 p = v ? rp(v) : 0;
        const U32 n = v ? r32(v + 8) : 0;
        for (U32 i = 0; p && i < n && i < 64 && got < cap; ++i) out[got++] = static_cast<int>(r32(p + i * 8ull));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return got;
}

std::mutex g_mu;
struct CacheKey {
    U64 inv;
    U16 slot, key;
    U64 tab;
    bool operator<(const CacheKey& o) const { return std::tie(inv, slot, key, tab) < std::tie(o.inv, o.slot, o.key, o.tab); }
};
std::map<CacheKey, std::vector<ItemPart>> g_cache;

} // namespace

bool parts_install()
{
    // (an offset the resolver isn't sure of is 0: addrs.cpp turns the chain's off rather than read a wrong field)
    const U64 offsets[] = {kOffInvEquipList, kOffEquipListEntries, kOffEquipListCount, kOffEquipEntrySize, kOffEntrySlot,
                           kOffEntryKey, kOffEntryShownKey, kOffInvActor, kOffActorComp, kOffPartsComp, kOffPartsBodyArg,
                           kOffPartsBodyId, kOffPartsG, kOffPartsG1, kOffPartsG2, kOffPartsG3, kOffPartsMap, kOffPrefabRecKey};
    bool offsets_ok = true;
    for (U64 o : offsets) offsets_ok = offsets_ok && o;
    if (!have({kRvaSlotHandle, kRvaPartsBody, kRvaItemInfo, kRvaPrefabIds, kRvaStringInfoGet, kRvaPartsMapFind,
               kRvaPartsGlobal}) || !offsets_ok) {
        LOG("visibility extras: the part lookup's %s not found or not certain; extras are off",
            offsets_ok ? "functions were" : "offsets were");
        return false;
    }
    Chain& c = g_chain;
    c.body = reinterpret_cast<BodyFn>(g_base + kRvaPartsBody);
    c.info = reinterpret_cast<ItemInfoFn2>(g_base + kRvaItemInfo);
    c.prefabs = reinterpret_cast<PrefabsFn>(g_base + kRvaPrefabIds);
    c.rec = reinterpret_cast<PrefabRecFn>(g_base + kRvaStringInfoGet);
    c.find = reinterpret_cast<MapFindFn>(g_base + kRvaPartsMapFind);
    c.global = g_base + kRvaPartsGlobal;
    g_ok = true;
    LOG("visibility extras: part lookup on (prefabs +%llX, map +%llX)", kRvaPrefabIds, kRvaPartsMapFind);
    return true;
}

bool parts_on() { return g_ok; }

// Every part the slot's shown item puts on the model (in the item's prefab order), with its name; none when the slot
// is empty, the item is off the presentation (its parts aren't built) or the lookup is off.
std::vector<ItemPart> item_parts(U64 inv, U64 model, U16 slot)
{
    std::vector<ItemPart> out;
    if (!g_ok || !inv || !model) return out;
    const U16 key = shown_key(inv, slot);
    if (key == 0xFFFF) return out;
    const U64 tab = rp(model + 0x58);
    const CacheKey ck{inv, slot, key, tab};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (auto it = g_cache.find(ck); it != g_cache.end()) return it->second;
    }
    U16 ids[32];
    const U32 n = prefab_ids_raw(inv, slot, key, ids, 32);
    std::map<int, std::string> names; // (the model's parts by handle)
    for (U32 i = 0, c = r32(model + 0x60); tab && i < c && i < 1024; ++i)
        if (U64 part = rp(tab + i * 16ull + 8)) names.emplace(static_cast<int>(r32(tab + i * 16ull)), part_name(r32(part + 0x48)));
    for (U32 i = 0; i < n; ++i) {
        int hs[64];
        const U32 m = prefab_handles_raw(ids[i], hs, 64);
        for (U32 k = 0; k < m; ++k) {
            auto f = names.find(hs[k]);
            if (f == names.end()) continue; // (not on the model: off the presentation, or not built for this body)
            bool dup = false;
            for (const ItemPart& p : out) dup |= p.handle == hs[k];
            if (!dup) out.push_back({hs[k], f->second});
        }
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_cache.size() > 512) g_cache.clear();
    g_cache[ck] = out;
    return out;
}

// A part's group: its name without a trailing number ("Upperbody_Acc_01" -> "Upperbody_Acc").
std::string part_group(const std::string& name)
{
    size_t e = name.size();
    while (e > 0 && name[e - 1] >= '0' && name[e - 1] <= '9') --e;
    if (e < name.size() && e > 0 && name[e - 1] == '_') return name.substr(0, e - 1);
    return name;
}

// --- Working parts (2026-11-16, the Tech Gloves crash) ------------------------------------------------------------
// Some pieces have a part the game builds with a component of its own (the Tech Gloves' All-Purpose Arm; ordinary
// gloves, the Great Thief's Gloves don't - checked in play): opening a chest with them reaches into it (the part's
// scene object, type-cast, its table +228, entry 27). Taken off the presentation, or presented again by our own call,
// that part exists without it (the bare body's Hand, or our build's), and the game crashed reading a null table
// (full dump, 2026-11-16). Such pieces are never taken off: hidden on our layer instead (looks.cpp). Which ones is
// read from the game's own build: the chest path's check (part +28 -> +8, flag +15 == 0, the cast of it -28, the
// table +228), its offsets and cast function from the resolver (witnessed in that code).
// Known per item's permanent id, kept in the outfits file: true = has a working part.
namespace {
using CastFn = U64(__fastcall*)(U64);
struct LiveCheck {
    bool ok = false;
    U32 o_part = 0, o_obj = 0, o_flag = 0, o_table = 0;
    U32 entry = ~0u; // (the table entry the chest path asks for: the part in use - the arm out; ~0: not known)
    U32 o_flags[3] = {}; // (that entry's enabled / unbound / in-use bytes; 0: not known)
    std::int32_t o_adj = 0;
    CastFn cast = nullptr;
} g_live;
std::mutex g_kind_mu;
std::map<U32, bool> g_kind; // permanent id -> has a working part
std::atomic<bool> g_kind_dirty{false};

bool live_check_ready()
{
    if (g_live.ok) return true;
    static bool tried = false;
    if (tried) return false;
    tried = true;
    // (from the resolver: the chest path's function and every offset it reads are witnessed in its code; a doubtful
    // one is 0, and then the check is off for this game version)
    if (!kRvaChestPartCheck || !kRvaPartCast || !kOffPartScene || !kOffSceneInner || !kOffSceneSkip || !kOffSceneCastAdj ||
        !kOffCastTable) {
        LOG("working parts: the game's check isn't known on this game version; pieces are taken off as before");
        return false;
    }
    g_live.o_part = U32(kOffPartScene), g_live.o_obj = U32(kOffSceneInner), g_live.o_flag = U32(kOffSceneSkip);
    g_live.o_adj = -static_cast<std::int32_t>(kOffSceneCastAdj);
    g_live.cast = reinterpret_cast<CastFn>(g_base + kRvaPartCast);
    g_live.o_table = U32(kOffCastTable);
    // (the part in use, from the resolver too: the entry the chest path asks for - witnessed at its "mov edx, N" - and
    // that entry's flags, witnessed in the arm IK's own code; any unknown: hidden pieces don't show while in use)
    if (kOffArmEntry && kOffArmEnabled && kOffArmUnbound && kOffArmInUse) {
        g_live.entry = U32(kOffArmEntry);
        g_live.o_flags[0] = U32(kOffArmEnabled), g_live.o_flags[1] = U32(kOffArmUnbound), g_live.o_flags[2] = U32(kOffArmInUse);
    }
    g_live.ok = true;
    LOG("working parts: the game's check at +%llX (part +%X -> +%X, flag +%X, cast +%llX, table +%X, in use: entry %d +%X +%X +%X)",
        kRvaChestPartCheck, g_live.o_part, g_live.o_obj, g_live.o_flag, kRvaPartCast, g_live.o_table,
        g_live.entry == ~0u ? -1 : int(g_live.entry), g_live.o_flags[0], g_live.o_flags[1], g_live.o_flags[2]);
    return true;
}

// 1: the part has the component, 0: it hasn't, -1: can't tell (no scene object, or the read faulted)
int part_has_component_raw(U64 part)
{
    __try {
        const U64 a = rp(part + g_live.o_part);
        const U64 q = a ? rp(a + g_live.o_obj) : 0;
        if (!q) return -1;
        if (*reinterpret_cast<const unsigned char*>(q + g_live.o_flag) != 0) return 0; // (the chest path skips it)
        const U64 obj = g_live.cast(q + g_live.o_adj);
        return obj && rp(obj + g_live.o_table) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
} // namespace

U64 look_part(U64 model, int handle); // (looks.cpp)

int working_part_known(U16 key)
{
    const U32 pid = key != 0xFFFF ? pid_of(key) : 0;
    if (!pid) return -1;
    {
        std::lock_guard<std::mutex> lock(g_kind_mu);
        if (auto it = g_kind.find(pid); it != g_kind.end()) return it->second ? 1 : 0;
    }
    return live_check_ready() ? -1 : 0; // (no check on this game version: an unknown piece is taken off as before)
}

int working_part_check(U64 inv, U64 model, U16 slot, U16 key)
{
    const U32 pid = key != 0xFFFF ? pid_of(key) : 0;
    if (!pid || !live_check_ready()) return -1;
    const std::vector<ItemPart> parts = item_parts(inv, model, slot);
    if (parts.empty()) return -1; // (not built yet)
    int result = 0;
    for (const ItemPart& p : parts) {
        const int r = part_has_component_raw(look_part(model, p.handle));
        if (r < 0) return -1;
        if (r > 0) result = 1;
    }
    {
        std::lock_guard<std::mutex> lock(g_kind_mu);
        g_kind[pid] = result != 0;
    }
    g_kind_dirty = true;
    LOG("working parts: item %u (id %X) %s (checked in the world)", key, pid,
        result ? "has a working part: hidden on the look layer only, never taken off" : "is plain: can be taken off");
    return result;
}

std::string working_parts_line(bool working)
{
    std::lock_guard<std::mutex> lock(g_kind_mu);
    std::string s;
    char b[16];
    for (const auto& [pid, w] : g_kind)
        if (w == working) std::snprintf(b, sizeof b, "%s%X", s.empty() ? "" : " ", pid), s += b;
    return s;
}

void working_parts_read(const std::string& ids, bool working)
{
    std::lock_guard<std::mutex> lock(g_kind_mu);
    const char* q = ids.c_str();
    char* e = nullptr;
    for (unsigned long v = std::strtoul(q, &e, 16); e != q; v = std::strtoul(q = e, &e, 16))
        if (v) g_kind[U32(v)] = working;
}

bool working_parts_dirty() { return g_kind_dirty.exchange(false); }

namespace {
// The part's in-use entry (0: none) and its flags. The entry is the arm's IK (PoseModifierManipulatorIK, see addrs.cpp):
// enabled (+kOffArmEnabled: set by the chest path on every use), unbound (+kOffArmUnbound: from its construction to its
// first update) and in use (+kOffArmInUse: from the update's first pose to the end of its blend-out). Polled in play,
// 2026-11-17, three uses in a row on one entry: first use unbound, then enabled + in use, then in use alone, then all
// 0; later uses only enabled, about 3 s. Its "state" here: the three ORed, 0 at rest. -1: can't tell.
constexpr unsigned char kAtRest = 0;
int part_in_use_raw(U64 part, U64* entry, unsigned char* state)
{
    __try {
        const U64 a = rp(part + g_live.o_part);
        const U64 q = a ? rp(a + g_live.o_obj) : 0;
        if (!q) return -1;
        const U64 obj = g_live.cast(q + g_live.o_adj);
        const U64 t = obj ? rp(obj + g_live.o_table) : 0;
        if (!t) return -1;
        *entry = rp(t + U64(g_live.entry) * 8);
        *state = kAtRest;
        if (*entry)
            for (const U32 o : g_live.o_flags) *state |= *reinterpret_cast<const unsigned char*>(*entry + o);
        return *entry ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
} // namespace

// The slots whose piece has a working part that is in use now (the Tech Gloves' arm out): its component holds the
// entry the game asks for then (seen appearing as the arm was used, 2026-11-16). A hidden piece shows while it is.
U32 working_parts_in_use(U64 inv, U64 model)
{
    if (!inv || !model || !live_check_ready() || g_live.entry == ~0u) return 0; // (found here too: a known item never checks)
    // per slot: the entry seen last, and whether its use is on (from the entry's first appearance - it starts at rest
    // for a moment as the arm comes out - or from its state leaving rest, until it comes back to rest)
    struct Use { U64 entry = 0; bool on = false, moved = false; };
    static std::map<std::pair<U64, U16>, Use> uses;
    U32 mask = 0;
    const U64 rt = rp(inv + 0xA0), arr = rt ? rp(rt + 8) : 0;
    for (U32 i = 0, n = rt ? r32(rt + 0x10) : 0; arr && i < n && i < 64; ++i) {
        const U16 slot = *reinterpret_cast<U16*>(arr + i * 0xD0ull + 0xC8);
        U16 key = *reinterpret_cast<U16*>(arr + i * 0xD0ull + 0x90);
        if (key == 0xFFFF) key = *reinterpret_cast<U16*>(arr + i * 0xD0ull + 8);
        if (slot >= 32 || key == 0xFFFF || working_part_known(key) != 1) continue;
        for (const ItemPart& p : item_parts(inv, model, slot)) {
            U64 entry = 0;
            unsigned char state = kAtRest;
            if (part_in_use_raw(look_part(model, p.handle), &entry, &state) < 0) continue;
            Use& u = uses[{inv, slot}];
            if (!entry) { u = {}; continue; }
            if (entry != u.entry) u = {entry, true, false}; // (it just appeared: the arm is coming out)
            if (state != kAtRest) u.on = true, u.moved = true;
            else if (u.moved) u.on = false, u.moved = false; // (back at rest after moving: put away)
            if (u.on) { mask |= 1u << slot; break; }
        }
    }
    return mask;
}

} // namespace om
