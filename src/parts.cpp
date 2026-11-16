// Visibility Extras: the model parts a worn piece brings, by the game's own data (2026-11-16).
//
// An item is built from prefabs (the blade, its scabbard; a chest's body, trim and pauldrons); each prefab puts parts
// on the character model. The game's slot handle function (kRvaSlotHandle) asks for them in this order, and so do we:
//  - the slot's record in the equip component (inv+A0: {.., +8 records*, +10 count}; 0xD0 each, slot u16 +C8, the
//    shown item's key +90, else the equipped one's +8);
//  - the character's body record (BodyFn on [[[inv+8]+68]+20]+30: its +8) and the item's info (ItemInfoFn on the key);
//  - PrefabsFn(info, body, slot, &{0, &vec}): the item's prefab ids (u16) into vec {u16*, u32 count};
//  - per prefab id: PrefabRecFn(&id) -> its record; the map [[[G+D8]+8]+2C0]+40068 (+50), looked up (MapFindFn) by
//    record+10 -> a node -> {handle entries*, u32 count}: 8 bytes each, the part handle in the low dword.
// Every address and offset is read from the slot handle function's own instructions (two patterns), so the chain
// follows the game: one pattern missing and the feature stays off. Handles are those the visibility calls take; a
// part's name is the model's (its part table). The prefab id vector the game allocates isn't freed (a few bytes per
// item seen; cached per item and model state).
#include "om.h"
#include "parts.h"
#include "addrs.h"
#include "looks.h"
#include "log.h"
#include "mem.h"
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
    U64 global = 0;                                           // (the address holding G)
    U32 o_actor = 0, o_a = 0, o_b = 0, o_c = 0, o_body = 0;   // [[[inv+o_actor]+o_a]+o_b]+o_c; the body id at +o_body
    U32 o_g = 0, o_rec_key = 0, o_m1 = 0, o_m2 = 0, o_m3 = 0, o_m4 = 0; // G+o_g, rec+o_rec_key, [[x+o_m1]+o_m2]+o_m3 (+o_m4)
} g_chain;
bool g_ok = false;

// A byte pattern ("48 8B ?? ...") in [from, from+len): its address, or 0.
U64 match(U64 from, size_t len, const char* pat)
{
    std::vector<int> p;
    for (const char* s = pat; *s;) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?') p.push_back(-1), s += 2;
        else p.push_back(static_cast<int>(std::strtoul(std::string(s, 2).c_str(), nullptr, 16))), s += 2;
    }
    const auto* b = reinterpret_cast<const unsigned char*>(from);
    for (size_t i = 0; i + p.size() <= len; ++i) {
        size_t k = 0;
        while (k < p.size() && (p[k] < 0 || b[i + k] == p[k])) ++k;
        if (k == p.size()) return from + i;
    }
    return 0;
}
U64 call_target(U64 at) { return at + 5 + static_cast<std::int64_t>(*reinterpret_cast<const std::int32_t*>(at + 1)); }
U32 d8(U64 at) { return *reinterpret_cast<const unsigned char*>(at); }
U32 d32(U64 at) { return *reinterpret_cast<const U32*>(at); }

struct Vec { U64 ptr; U32 count, cap; };

U32 prefab_ids_raw(U64 inv, U16 slot, U16* out, U32 cap)
{
    U32 got = 0;
    __try {
        U64 rt = rp(inv + 0xA0), arr = rt ? rp(rt + 8) : 0;
        const U32 n = rt ? r32(rt + 0x10) : 0;
        U64 rec = 0;
        for (U32 i = 0; arr && i < n && i < 64; ++i)
            if (*reinterpret_cast<U16*>(arr + i * 0xD0ull + 0xC8) == slot) { rec = arr + i * 0xD0ull; break; }
        if (!rec || *reinterpret_cast<U16*>(rec + 8) == 0xFFFF) return 0;
        U16 key = *reinterpret_cast<U16*>(rec + 0x90);
        if (key == 0xFFFF) key = *reinterpret_cast<U16*>(rec + 8);
        const U64 a = rp(rp(rp(inv + g_chain.o_actor) + g_chain.o_a) + g_chain.o_b);
        if (!a) return 0;
        const U64 body = g_chain.body(a + g_chain.o_c);
        const U32 b = body ? r32(body + g_chain.o_body) : 0;
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
        const U64 g = rec ? rp(g_chain.global) : 0, x = g ? rp(g + g_chain.o_g) : 0;
        U64 m = x ? rp(rp(x + g_chain.o_m1) + g_chain.o_m2) : 0;
        m = m ? rp(m + g_chain.o_m3) : 0;
        if (!m) return 0;
        const U64 node = g_chain.find(m + g_chain.o_m4, rec + g_chain.o_rec_key);
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
    if (!kRvaSlotHandle) return false;
    const U64 fn = g_base + kRvaSlotHandle;
    const U64 a = match(fn, 0x300, "48 8B 41 ?? 48 8B 48 ?? 48 8B 49 ?? 48 83 C1 ?? E8 ?? ?? ?? ?? 8B 58 ?? 48 8D 4C 24 ?? E8"
                                   " ?? ?? ?? ?? 4C 89 74 24 ?? 48 8D 4C 24 ?? 48 89 4C 24 ?? 4C 8D 4C 24 ?? 44 0F B7 C7 8B"
                                   " D3 48 8B C8 E8");
    const U64 b = match(fn, 0x300, "48 8B 05 ?? ?? ?? ?? 48 8B 98 ?? ?? ?? ?? 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 8D 50 ?? 48"
                                   " 8B 43 ?? 48 8B 88 ?? ?? ?? ?? 48 8B 89 ?? ?? ?? ?? 48 83 C1 ?? E8");
    // (the slot records' layout this file reads: inv+A0 {+8 records, +10 count}, 0xD0 each, slot +C8, keys +90 / +8)
    const U64 r = match(fn, 0x100, "48 8B 81 A0 00 00 00 4C 8B 50 08 4D 8B DA 8B 40 10 48 69 C0 D0 00 00 00");
    const U64 k = match(fn, 0x100, "66 41 39 BB C8 00 00 00");
    const U64 v = match(fn, 0x100, "41 0F B7 82 90 00 00 00");
    if (!r || !k || !v) {
        LOG("visibility extras: the slot records' layout changed; extras are off");
        return false;
    }
    if (!a || !b) {
        LOG("visibility extras: the slot handle function's part lookup wasn't found (%s); extras are off", a ? "map" : "prefabs");
        return false;
    }
    Chain& c = g_chain;
    c.o_actor = d8(a + 3), c.o_a = d8(a + 7), c.o_b = d8(a + 11), c.o_c = d8(a + 15);
    c.body = reinterpret_cast<BodyFn>(call_target(a + 16));
    c.o_body = d8(a + 23);
    c.info = reinterpret_cast<ItemInfoFn2>(call_target(a + 29));
    c.prefabs = reinterpret_cast<PrefabsFn>(call_target(a + 63));
    c.global = b + 7 + static_cast<std::int64_t>(static_cast<std::int32_t>(d32(b + 3)));
    c.o_g = d32(b + 10);
    c.rec = reinterpret_cast<PrefabRecFn>(call_target(b + 19));
    c.o_rec_key = d8(b + 27), c.o_m1 = d8(b + 31), c.o_m2 = d32(b + 35), c.o_m3 = d32(b + 42), c.o_m4 = d8(b + 49);
    c.find = reinterpret_cast<MapFindFn>(call_target(b + 50));
    g_ok = true;
    LOG("visibility extras: part lookup at +%llX / +%llX (prefabs +%llX, map +%llX)", a - g_base, b - g_base,
        reinterpret_cast<U64>(c.prefabs) - g_base, reinterpret_cast<U64>(c.find) - g_base);
    return true;
}

bool parts_on() { return g_ok; }

// Every part the slot's shown item puts on the model (in the item's prefab order), with its name; none when the slot
// is empty, the item is off the presentation (its parts aren't built) or the lookup is off.
std::vector<ItemPart> item_parts(U64 inv, U64 model, U16 slot)
{
    std::vector<ItemPart> out;
    if (!g_ok || !inv || !model) return out;
    U64 rt = rp(inv + 0xA0), arr = rt ? rp(rt + 8) : 0;
    U16 key = 0xFFFF;
    for (U32 i = 0, n = rt ? r32(rt + 0x10) : 0; arr && i < n && i < 64; ++i)
        if (*reinterpret_cast<U16*>(arr + i * 0xD0ull + 0xC8) == slot) {
            key = *reinterpret_cast<U16*>(arr + i * 0xD0ull + 0x90);
            if (key == 0xFFFF) key = *reinterpret_cast<U16*>(arr + i * 0xD0ull + 8);
            break;
        }
    if (key == 0xFFFF) return out;
    const U64 tab = rp(model + 0x58);
    const CacheKey ck{inv, slot, key, tab};
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (auto it = g_cache.find(ck); it != g_cache.end()) return it->second;
    }
    U16 ids[32];
    const U32 n = prefab_ids_raw(inv, slot, ids, 32);
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

} // namespace om
