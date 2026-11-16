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

// (the working-parts check below still reads its offsets from the code with these; the resolver has them as
// kRvaPartCast / kOffPartScene.. kOffCastTable)
U64 call_target(U64 at) { return at + 5 + static_cast<std::int64_t>(*reinterpret_cast<const std::int32_t*>(at + 1)); }
U32 d8(U64 at) { return *reinterpret_cast<const unsigned char*>(at); }
U32 d32(U64 at) { return *reinterpret_cast<const U32*>(at); }

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
// table +228), its offsets and cast function taken from that code's bytes; found once the game has unpacked it.
// Known per item's permanent id, kept in the outfits file: true = has a working part.
namespace {
using CastFn = U64(__fastcall*)(U64);
struct LiveCheck {
    bool ok = false;
    U32 o_part = 0, o_obj = 0, o_flag = 0, o_table = 0;
    std::int32_t o_adj = 0;
    CastFn cast = nullptr;
} g_live;
std::mutex g_kind_mu;
std::map<U32, bool> g_kind; // permanent id -> has a working part
std::atomic<bool> g_kind_dirty{false};

// the pattern in [p, p+n) (wildcards -1); no C++ objects here: read under __try
U64 scan_raw(const int* pat, size_t len, const unsigned char* p, U64 n)
{
    __try {
        for (U64 i = 0; i + len <= n; ++i) {
            if (p[i] != pat[0]) continue;
            size_t k = 1;
            while (k < len && (pat[k] < 0 || p[i + k] == pat[k])) ++k;
            if (k == len) return reinterpret_cast<U64>(p + i);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

bool live_check_ready()
{
    if (g_live.ok) return true;
    static U64 next_try = 0; // (the code is packed until the game unpacks it: tried again now and then, not each frame)
    const U64 now = GetTickCount64();
    if (now < next_try) return false;
    next_try = now + 5000;
    static const char* const kPat = "48 8B 4F ?? 48 85 C9 74 ?? 48 8B 49 ?? 48 85 C9 74 ?? 80 79 ?? 00 75 ?? 48 83 C1 ?? E8 "
                                    "?? ?? ?? ?? 48 85 C0 74 ?? C5 F8 28 DE 44 0F B6 C5 49 8B D6 48 8B 88";
    int pat[64];
    size_t len = 0;
    for (const char* s = kPat; *s && len < 64;) {
        while (*s == ' ') ++s;
        if (!*s) break;
        pat[len++] = s[0] == '?' ? -1 : static_cast<int>(std::strtoul(std::string(s, 2).c_str(), nullptr, 16));
        s += 2;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    U64 at = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections && !at; ++i)
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            at = scan_raw(pat, len, reinterpret_cast<const unsigned char*>(g_base + sec[i].VirtualAddress), sec[i].Misc.VirtualSize);
    if (!at) return false;
    g_live.o_part = d8(at + 3), g_live.o_obj = d8(at + 12), g_live.o_flag = d8(at + 20);
    g_live.o_adj = static_cast<std::int8_t>(d8(at + 27));
    g_live.cast = reinterpret_cast<CastFn>(call_target(at + 28));
    g_live.o_table = d32(at + 52);
    g_live.ok = true;
    LOG("working parts: the game's check found at +%llX (part +%X -> +%X, flag +%X, cast +%llX, table +%X)", at - g_base,
        g_live.o_part, g_live.o_obj, g_live.o_flag, reinterpret_cast<U64>(g_live.cast) - g_base, g_live.o_table);
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
    std::lock_guard<std::mutex> lock(g_kind_mu);
    auto it = g_kind.find(pid);
    return it == g_kind.end() ? -1 : it->second ? 1 : 0;
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

} // namespace om
