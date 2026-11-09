// Item data: names, the item table, resistances and climate, key repair.
#include "om.h"
#include "items.h"
#include "addrs.h"
#include "gear.h"
#include "store.h"
#include "wheel_draw.h"

namespace om {

RecordGetFn g_str_get = nullptr;

U32 kPassiveFire = 0x74e, kPassiveIce = 0x74f; // (0x704 / 0x705 before 2026-10-30: Kuku armour Ice 10 = 0x74f, Ziggurat Lightning 1 = 0x750)
// Base stats (cloaks...): a second list of the same {kind, level}: info +0x188 -> {rows*, n}, row (0x80 each) +0x58 entries, +0x60 count
// (Plate Cloak of the Shadows: Fire Lv 7; Wolf Cloak: Ice Lv 5).
U32 kBaseFire = 0x72, kBaseIce = 0x73; // (0x68 / 0x69 before 2026-10-30: Scout Cloak Ice 3 = 0x73, Hungering Fang Lightning 7 = 0x74)

// The game's item table: a key is a row, and a patch that adds items renumbers the rows; the permanent id in each row
// stays. Only the first block is used: the rows from the data file, whose body offsets grow from row to row. After it the
// table has room for rows the game fills while it runs (they come and go, and repeat ids). Checked: ids all different.
U64 item_rows(U32* count = nullptr)
{
    static int state = 0; // 0 not checked, 1 fine, 2 not what it should be
    static U32 block = 0;
    U64 table = kRvaItemTable ? rp(g_base + kRvaItemTable) : 0;
    U64 rows = table ? rp(table + 0x28) : 0;
    U32 n = table ? r32(table + 8) : 0;
    if (!rows || !n) return 0; // (not loaded yet)
    if (!state) {
        std::set<U32> ids;
        U32 last = 0;
        for (U32 i = 0; i < n && i < 60000; ++i) {
            U32 id = r32(rows + i * 8ull), off = r32(rows + i * 8ull + 4);
            if (!id || (i && off <= last)) break;
            ids.insert(id);
            last = off;
            block = i + 1;
        }
        bool ok = n <= 60000 && block >= 100 && block * 2 >= n && ids.size() == block;
        state = ok ? 1 : 2;
        LOG("self-test: item table, %u rows, %u from the data file, %zu different ids: %s", n, block, ids.size(),
            ok ? "ok" : "FAILED (keys can't be repaired)");
    }
    if (state != 1) return 0;
    if (count) *count = block;
    return rows;
}

U32 pid_of(U16 key)
{
    U32 n = 0;
    U64 rows = item_rows(&n);
    return rows && key < n ? r32(rows + key * 8ull) : 0;
}

U16 key_of_pid(U32 pid)
{
    static std::map<U32, U16> keys;
    U32 n = 0;
    U64 rows = item_rows(&n);
    if (!rows) return 0xFFFF;
    if (keys.empty())
        for (U32 i = 0; i < n && i < 0xFFFF; ++i)
            if (U32 id = r32(rows + i * 8ull)) keys[id] = static_cast<U16>(i);
    auto f = keys.find(pid);
    return f != keys.end() ? f->second : U16(0xFFFF);
}

// An item info's passive list (base false) or base stat list (true, from the first refinement row) at `off`.
std::pair<U64, U32> stat_list(U64 info, U64 off, bool base)
{
    if (!info) return {0, 0};
    if (base) {
        U64 stats = rp(info + off), row0 = stats ? rp(stats) : 0;
        return {row0 ? rp(row0 + 0x58) : 0, row0 ? r32(row0 + 0x60) : 0};
    }
    U64 p = rp(info + off);
    return {p ? rp(p) : 0, p ? r32(p + 8) : 0};
}

std::pair<int, int> item_resistance(U16 key)
{
    if (auto f = g_item_resist.find(key); f != g_item_resist.end()) return f->second;
    std::pair<int, int> r{0, 0};
    U16 k = key;
    U64 info = g_item_info ? g_item_info(0, k) : 0;
    auto [list, n] = stat_list(info, kOffItemPassives, false);
    auto [base, nb] = stat_list(info, kOffItemStats, true); // (the resistance is the same in every refinement row)
    auto sum = [&](U64 l, U32 cnt, U32 ice, U32 fire) {
        for (U32 i = 0; l && i < cnt && i < 16; ++i) {
            U32 kind = 0, level = 0;
            if (!rd(l + i * 8ull, kind) || !rd(l + i * 8ull + 4, level) || level > 100) continue;
            if ((kind & 0xFFFF) == ice) r.first += static_cast<int>(level);
            else if ((kind & 0xFFFF) == fire) r.second += static_cast<int>(level);
        }
    };
    sum(list, n, kPassiveIce, kPassiveFire);
    sum(base, nb, kBaseIce, kBaseFire);
    if (g_item_info) g_item_resist[key] = r; // (not cached before the lookup is there)
    if (r.first || r.second) LOG("resistance of item %u \"%s\": ice %d, fire %d", key, item_name(key).c_str(), r.first, r.second);
    return r;
}

// The ids of the resistance stats can renumber on a patch, and the lists can move in the item info: both are read from
// items whose values are known (found by permanent id). First the lists' offsets: for each kind (passive / base), the
// offset where every reference of that kind has an entry with its id and level (one offset only, else the built-in one
// stays). Then the ids: a reference counts when exactly one of its entries has the known level; references must agree;
// without that the built-in id stays.
void calibrate_resist_ids()
{
    struct Ref { U32 pid; U32 level; bool base; U32* id; };
    const Ref refs[] = {{0xF4B2F, 10, false, &kPassiveIce}, {0xF4B2E, 10, false, &kPassiveFire}, {0xF4EC4, 3, true, &kBaseIce},
                        {0xF43FC, 5, true, &kBaseIce},      {0xF43BF, 7, true, &kBaseFire}};
    for (bool base : {false, true}) {
        U64& off = base ? kOffItemStats : kOffItemPassives;
        std::vector<U64> hits;
        for (U64 o = 0x80; o < 0x300; o += 8) {
            int refs_of_kind = 0, matched = 0;
            for (const Ref& r : refs) {
                if (r.base != base) continue;
                ++refs_of_kind;
                U16 key = key_of_pid(r.pid);
                auto [list, n] = stat_list(key != 0xFFFF && g_item_info ? g_item_info(0, key) : 0, o, base);
                bool has = false;
                for (U32 i = 0; list && n <= 64 && i < n; ++i) {
                    U32 kind = 0, level = 0;
                    has |= rd(list + i * 8ull, kind) && rd(list + i * 8ull + 4, level) && level == r.level &&
                           (kind & 0xFFFF) == *r.id;
                }
                matched += has;
            }
            if (refs_of_kind && matched == refs_of_kind) hits.push_back(o);
        }
        if (hits.size() == 1 && hits[0] != off) LOG("self-test: the %s stat list moved: +%llX -> +%llX", base ? "base" : "passive", off, hits[0]);
        if (hits.size() == 1) off = hits[0];
        else LOG("self-test: the %s stat list found at %zu offsets; +%llX kept", base ? "base" : "passive", hits.size(), off);
    }
    std::map<U32*, U32> found;
    std::set<U32*> clash;
    int answered = 0;
    for (const Ref& r : refs) {
        U16 key = key_of_pid(r.pid);
        U64 info = key != 0xFFFF && g_item_info ? g_item_info(0, key) : 0;
        auto [list, n] = stat_list(info, r.base ? kOffItemStats : kOffItemPassives, r.base);
        U32 id = 0;
        int hits = 0;
        for (U32 i = 0; list && i < n && i < 16; ++i) {
            U32 kind = 0, level = 0;
            if (rd(list + i * 8ull, kind) && rd(list + i * 8ull + 4, level) && level == r.level) id = kind & 0xFFFF, ++hits;
        }
        if (hits != 1) continue;
        ++answered;
        if (auto f = found.find(r.id); f != found.end() && f->second != id) clash.insert(r.id);
        found[r.id] = id;
    }
    bool moved = false;
    for (const auto& [var, id] : found)
        if (!clash.count(var) && *var != id) {
            LOG("self-test: a resistance id moved: %X -> %X", *var, id);
            *var = id, moved = true;
        }
    if (moved) g_item_resist.clear();
    LOG("self-test: resistance ids from %d of %zu references (%zu disagree): passive ice %X fire %X, base ice %X fire %X "
        "(lists at +%llX / +%llX)", answered, std::size(refs), clash.size(), kPassiveIce, kPassiveFire, kBaseIce, kBaseFire,
        kOffItemPassives, kOffItemStats);
}

// Abyss gears belong to one COPY of an item: its sockets are at copy+0x60 -> 6-byte entries {u16 gear key (0xFFFF:
// empty), u16, u8 socket, u8}. A gear is an item too, its resistance in its own lists (Flameward II, key 3244: Fire
// Lv 2 as a passive). The copy is found by uid + key: worn, or in a bag (storage / wardrobe too). Re-read at each
// wheel open (gears can be put in / taken out between). Companions' worn copies are not searched (a piece they wear
// counts without its gears).

std::map<U64, std::pair<int, int>> g_gear_resist; // uid -> its gears' {ice, fire}

std::pair<int, int> gear_resistance(U64 uid, U16 key)
{
    if (auto f = g_gear_resist.find(uid); f != g_gear_resist.end()) return f->second;
    std::pair<int, int> r{0, 0};
    U64 copy = find_copy(uid, key);
    U64 sockets = copy ? rp(copy + 0x60) : 0;
    for (U32 i = 0; sockets && i < 5; ++i) {
        U16 gear = 0xFFFF;
        if (!rd(sockets + i * 6ull, gear) || gear == 0xFFFF || gear == 0) continue;
        auto [gi, gf] = item_resistance(gear);
        r.first += gi, r.second += gf;
    }
    g_gear_resist[uid] = r;
    if (r.first || r.second) LOG("abyss gears of %s (uid %llX): ice %d, fire %d", item_name(key).c_str(), uid, r.first, r.second);
    return r;
}

void outfit_resistance(const Outfit& o, int& ice, int& fire)
{
    ice = fire = 0;
    for (const OutfitItem& it : o.items) {
        if (!loads_piece(o, it.slot)) continue; // (weapons it leaves aren't worn by it)
        auto [i, f] = item_resistance(it.key);
        auto [gi, gf] = gear_resistance(it.uid, it.key);
        ice += i + gi, fire += f + gf;
    }
}

// The outfit's Climate badge: Auto from its resistance (Lv. 10 plain, Lv. 15 phase thermometer; the stronger side),
// or the manual Cold / Heat (plain), or none.
const char* climate_badge(const Outfit& o)
{
    if (o.sleepwear) return "om-sleep-c"; // (sleepwear: the bed, in the thermometer's spot)
    if (o.climate == 1) return "om-cold-c";
    if (o.climate == 2) return "om-heat-c";
    if (o.climate == 3) return nullptr;
    int ice = 0, fire = 0;
    outfit_resistance(o, ice, fire);
    bool cold = ice >= fire;
    int lv = cold ? ice : fire;
    // (the plain thermometer is the FULL meter, the "phase" one the half: Lv 10-14 half, Lv 15+ full; user 23:4x)
    const char* badge = lv >= 15 ? (cold ? "om-cold-c" : "om-heat-c") : lv >= 10 ? (cold ? "om-cold2-c" : "om-heat2-c") : nullptr;
    static std::map<std::string, std::string> logged; // (once per outfit and result)
    std::string what = std::to_string(ice) + "/" + std::to_string(fire) + (badge ? badge : "-");
    if (logged[o.name] != what) {
        logged[o.name] = what;
        std::string pieces;
        for (const OutfitItem& it : o.items) {
            auto [i, f] = item_resistance(it.key);
            if (i || f) pieces += " " + item_name(it.key) + " (ice " + std::to_string(i) + ", fire " + std::to_string(f) + ")";
        }
        LOG("climate \"%s\" (auto): ice %d, fire %d -> %s;%s", o.name.c_str(), ice, fire, badge ? badge : "no badge",
            pieces.empty() ? " no resistant pieces" : pieces.c_str());
    }
    return badge;
}

const char* icon_name_of(U64 item)
{
    __try {
        return g_icon_name ? g_icon_name(item) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// A piece's icon texture: from its copy (worn or in a bag), else from a bare item with its key (not in the inventory).
std::string piece_icon(const OutfitItem& it)
{
    U64 copy = it.slot == kBackShieldSlot ? back_shield_item() : find_copy(it.uid, it.key);
    if (!copy && it.key == 0xFFFF) return {}; // (nothing saved there)
    alignas(16) unsigned char bare[200] = {};
    std::memcpy(bare, &it.uid, 8);
    std::memcpy(bare + 8, &it.key, 2);
    long long one = 1;
    std::memcpy(bare + 0x10, &one, 8);
    const char* name = icon_name_of(copy ? copy : reinterpret_cast<U64>(bare));
    return name ? cstr(reinterpret_cast<U64>(name)) : std::string();
}
using LocTextFn = const char*(__fastcall*)(U64, U64);
ItemInfoFn g_item_info = nullptr; // set by install() after a prologue check
std::map<U16, std::string> g_item_names;

const char* item_name_raw(U16 key)
{
    __try {
        U64 info = g_item_info(0, key);
        if (!info || info == *reinterpret_cast<U64*>(g_base + kRvaDefaultItemInfo)) return nullptr;
        U64 text = *reinterpret_cast<U64*>(info + 0x90); // localized name, vfunc 1 (text, 0) -> UTF-8
        if (!text) return nullptr;
        return reinterpret_cast<LocTextFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(text) + 8))(text, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::string item_name(U16 key)
{
    if (!g_item_info || key == 0xFFFF) return {};
    if (auto f = g_item_names.find(key); f != g_item_names.end()) return f->second;
    const char* s = item_name_raw(key);
    std::string n = s ? std::string(s, strnlen(s, 256)) : std::string();
    LOG("item %u: name \"%s\"", key, n.c_str());
    return g_item_names[key] = n;
}

// The saved keys against the permanent ids: a piece whose key no longer names its item goes to the key that does (keys are
// renumbered by patches). A saved piece with no id yet gets one only when that exact uid + key is in the inventory, so an
// entry saved before a renumbering can't record a wrong id. The file is copied to OutfitManager_backups before a repair.
void repair_keys()
{
    if (!item_rows()) return;
    if (!g_placeholder_ids.empty()) {
        std::lock_guard<std::mutex> lock(g_keys_mu);
        for (U32 id : g_placeholder_ids)
            if (U16 k = key_of_pid(id); k != 0xFFFF && std::find(g_placeholder_keys.begin(), g_placeholder_keys.end(), k) == g_placeholder_keys.end())
                g_placeholder_keys.push_back(k);
        g_placeholder_ids.clear();
    }
    static std::map<std::pair<U64, U16>, bool> owned; // (uid, key) looked for this session
    int fixed = 0, stamped = 0, gone = 0;
    std::string repairs;
    for (Outfit& o : g_outfits)
        for (OutfitItem& it : o.items) {
            if (it.pid) {
                if (pid_of(it.key) == it.pid) continue;
                U16 k = key_of_pid(it.pid);
                if (k == 0xFFFF) {
                    ++gone;
                    LOG("key repair: \"%s\" equipment-%u: item id %X is no longer in the game (key %u kept)", o.name.c_str(), it.slot, it.pid, it.key);
                    continue;
                }
                repairs += " " + std::to_string(it.key) + "->" + std::to_string(k);
                it.key = k;
                ++fixed;
            } else if (std::pair<U64, U16> id{it.uid, it.key}; owned.try_emplace(id, false).second) {
                owned[id] = find_copy(it.uid, it.key) != 0;
            }
            if (!it.pid && owned[{it.uid, it.key}]) {
                it.pid = pid_of(it.key);
                stamped += it.pid != 0;
            }
        }
    if (fixed) {
        std::wstring dir = g_store_path.substr(0, g_store_path.find_last_of(L"\\/")) + L"\\OutfitManager_backups";
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[64];
        swprintf_s(name, L"\\OutfitManager.outfits.%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        bool copied = CopyFileW(g_store_path.c_str(), (dir + name).c_str(), FALSE) != 0;
        LOG("key repair: %d pieces:%s (file copied first: %s)", fixed, repairs.c_str(), copied ? "yes" : "NO");
        g_item_resist.clear();
    }
    if (stamped) LOG("key repair: %d pieces got their permanent id", stamped);
    if (fixed || stamped) save_outfits();
}

// --- Self-test ----------------------------------------------------------------------------------------------------
// A patch can move a field no pattern can see. So at the first menu open each one is read on the real thing, read-only:
// what doesn't look right turns its feature off (with the usual toast) instead of working on the wrong memory. A check
// that can't be made yet (no player, nothing worn) stays open for the next menu open.

bool readable_name(const std::string& n)
{
    if (n.empty() || n.size() > 128) return false;
    for (unsigned char c : n)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

// The resistance ids / lists, checked once against items of known values before any climate is worked out (the gear
// wheel's self-test and the Outfit Arc's opening both call it: 2026-11-06, the arc opened first and read every
// outfit's resistance as 0, so its thermometers didn't show).
void ensure_resist_ids()
{
    static bool done = false;
    if (!done && g_item_info && item_rows()) done = true, calibrate_resist_ids();
}

} // namespace om
