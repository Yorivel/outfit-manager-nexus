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

namespace om {

std::atomic<U32> g_peek_key{0};           // the Outfit Arc's key (a virtual-key code; 0 = Off)

// The current character's hotkeys for the input filters (another thread): vk | mods << 8 per outfit, 0 = none.
std::atomic<U32> g_hotkeys[16] = {};
std::atomic<bool> g_peek_pad{false};       // the Outfit Arc on controllers: hold L1 / LB, press the d-pad's down

// The input in use (the last one touched: a pad, or the mouse / keyboard), set by key_thread; a pad's family picks its
// confirm icon. The game swaps its own prompts the same way.
std::atomic<bool> g_pad_in_use{false}, g_pad_is_ps{false}, g_prompt_changed{false};
thread_local bool t_own_pad = false;      // our own pad reading (XInput, the PlayStation pad's reports): never filtered

// The game's PlayStation pad handles (opened by path: \\?\hid#vid_054c&pid_....), with their report length.
struct PadHandle {
    std::atomic<HANDLE> h{nullptr};
    DWORD len = 0;
    bool dualsense = false;
};
PadHandle g_pad_handles[8];

void note_pad_handle(LPCWSTR path, HANDLE h)
{
    if (!path || h == INVALID_HANDLE_VALUE || t_own_pad) return;
    std::wstring l = path;
    for (wchar_t& c : l) c = static_cast<wchar_t>(std::towlower(c));
    if (l.find(L"vid_054c") == std::wstring::npos) return;
    PHIDP_PREPARSED_DATA pp = nullptr;
    HIDP_CAPS caps{};
    DWORD len = 0;
    if (HidD_GetPreparsedData(h, &pp)) {
        if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) len = caps.InputReportByteLength;
        HidD_FreePreparsedData(pp);
    }
    if (!len) return;
    bool ds = l.find(L"pid_0ce6") != std::wstring::npos || l.find(L"pid_0df2") != std::wstring::npos;
    for (PadHandle& p : g_pad_handles) {
        HANDLE none = nullptr;
        if (p.h.load() == h) return;
        if (p.h.compare_exchange_strong(none, h)) {
            p.len = len, p.dualsense = ds;
            LOG("Outfit Arc: the game's PlayStation pad handle %p (reports of %lu bytes)", static_cast<void*>(h), len);
            return;
        }
    }
}

const PadHandle* pad_handle(HANDLE h)
{
    if (!h) return nullptr;
    for (const PadHandle& p : g_pad_handles)
        if (p.h.load() == h) return &p;
    return nullptr;
}

U32 crc32_bytes(U32 crc, const unsigned char* d, size_t n)
{
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// A report with the d-pad's up / down taken out (a diagonal keeps its left / right). Bluetooth reports carry a CRC
// (seeded with 0xA1): written again.
void strip_pad_report(unsigned char* r, DWORD n, bool dualsense)
{
    int face = -1;
    if (dualsense) {
        if (r[0] == 0x01 && n >= 64) face = 8;
        else if (r[0] == 0x31 && n >= 11) face = 9;
        else if (r[0] == 0x01 && n >= 7) face = 5;
    } else {
        if (r[0] == 0x01 && n >= 7) face = 5;
        else if (r[0] == 0x11 && n >= 9) face = 7;
    }
    if (face < 0 || DWORD(face) >= n) return;
    static const unsigned char kNoUpDown[9] = {8, 2, 2, 2, 8, 6, 6, 6, 8};
    unsigned char d = r[face] & 0x0F;
    if (d > 8 || kNoUpDown[d] == d) return;
    r[face] = static_cast<unsigned char>((r[face] & 0xF0) | kNoUpDown[d]);
    if ((r[0] == 0x31 || r[0] == 0x11) && n >= 78) {
        unsigned char seed = 0xA1;
        U32 c = crc32_bytes(crc32_bytes(0, &seed, 1) , r, 74);
        // (crc32_bytes chains: the seed byte, then the report's first 74 bytes)
        std::memcpy(r + 74, &c, 4);
    }
}

// Overlapped reads of a pad handle: the buffer is filled later; filtered when the game asks for the result.
struct PendingRead {
    std::atomic<OVERLAPPED*> ov{nullptr};
    unsigned char* buf = nullptr;
    DWORD len = 0;
    bool dualsense = false;
};
PendingRead g_pending_reads[16];

using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using GetOverlappedResultFn = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
using GetOverlappedResultExFn = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
ReadFileFn g_read_file_orig = nullptr;
GetOverlappedResultFn g_ov_result_orig = nullptr;
GetOverlappedResultExFn g_ov_result_ex_orig = nullptr;
bool peek_pad_owned();

BOOL WINAPI read_file_hook(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    BOOL r = g_read_file_orig(h, buf, n, got, ov);
    if (t_own_pad || !buf) return r;
    const PadHandle* p = pad_handle(h);
    if (!p || n < p->len) return r;
    DWORD err = GetLastError();
    if (r && peek_pad_owned()) {
        DWORD k = got ? *got : n;
        __try {
            strip_pad_report(static_cast<unsigned char*>(buf), k ? k : n, p->dualsense);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    } else if (!r && err == ERROR_IO_PENDING && ov) {
        for (PendingRead& q : g_pending_reads) {
            OVERLAPPED* none = nullptr;
            if (q.ov.load() == ov || q.ov.compare_exchange_strong(none, ov)) {
                q.buf = static_cast<unsigned char*>(buf), q.len = n, q.dualsense = p->dualsense;
                q.ov = ov;
                break;
            }
        }
    }
    SetLastError(err);
    return r;
}

void finish_pending_read(LPOVERLAPPED ov, BOOL ok, LPDWORD got)
{
    if (!ov) return;
    for (PendingRead& q : g_pending_reads) {
        if (q.ov.load() != ov) continue;
        if (ok && peek_pad_owned()) {
            __try {
                strip_pad_report(q.buf, got && *got ? *got : q.len, q.dualsense);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (ok) q.ov = nullptr; // (the slot is free; the game's next read takes it again)
        return;
    }
}

BOOL WINAPI ov_result_hook(HANDLE h, LPOVERLAPPED ov, LPDWORD got, BOOL wait)
{
    BOOL r = g_ov_result_orig(h, ov, got, wait);
    if (!t_own_pad) {
        DWORD err = GetLastError();
        finish_pending_read(ov, r, got);
        SetLastError(err);
    }
    return r;
}

BOOL WINAPI ov_result_ex_hook(HANDLE h, LPOVERLAPPED ov, LPDWORD got, DWORD ms, BOOL alertable)
{
    BOOL r = g_ov_result_ex_orig(h, ov, got, ms, alertable);
    if (!t_own_pad) {
        DWORD err = GetLastError();
        finish_pending_read(ov, r, got);
        SetLastError(err);
    }
    return r;
}

// Xbox pads: the game's xinput1_4 (by name or by ordinal 100, the one with the guide button) gets the d-pad's up / down
// taken out of its button word.
using XGetStateFn = DWORD(WINAPI*)(DWORD, void*);
XGetStateFn g_xget_orig = nullptr, g_xget100_orig = nullptr;

DWORD xget_filter(DWORD r, void* state)
{
    if (r == ERROR_SUCCESS && state && !t_own_pad && peek_pad_owned()) {
        WORD& b = *reinterpret_cast<WORD*>(static_cast<unsigned char*>(state) + 4); // (XINPUT_STATE: packet, then buttons)
        if (b & 0x0003) b &= static_cast<WORD>(~0x0003);
    }
    return r;
}
DWORD WINAPI xget_hook(DWORD i, void* s) { return xget_filter(g_xget_orig(i, s), s); }
DWORD WINAPI xget100_hook(DWORD i, void* s) { return xget_filter(g_xget100_orig(i, s), s); }

void install_pad_filters()
{
    struct { const wchar_t* dll; const char* name; void* fn; void** orig; } hooks[] = {
        {L"kernelbase", "ReadFile", reinterpret_cast<void*>(&read_file_hook), reinterpret_cast<void**>(&g_read_file_orig)},
        {L"kernelbase", "GetOverlappedResult", reinterpret_cast<void*>(&ov_result_hook), reinterpret_cast<void**>(&g_ov_result_orig)},
        {L"kernelbase", "GetOverlappedResultEx", reinterpret_cast<void*>(&ov_result_ex_hook), reinterpret_cast<void**>(&g_ov_result_ex_orig)},
    };
    int ok = 0;
    for (auto& h : hooks) {
        void* target = nullptr;
        if (MH_CreateHookApiEx(h.dll, h.name, h.fn, h.orig, &target) == MH_OK && MH_EnableHook(target) == MH_OK) ++ok;
        else LOG("Outfit Arc: %s filter failed", h.name);
    }
    if (HMODULE x = LoadLibraryW(L"xinput1_4.dll")) {
        if (void* t = reinterpret_cast<void*>(GetProcAddress(x, "XInputGetState")))
            if (MH_CreateHook(t, reinterpret_cast<void*>(&xget_hook), reinterpret_cast<void**>(&g_xget_orig)) == MH_OK &&
                MH_EnableHook(t) == MH_OK) ++ok;
        if (void* t = reinterpret_cast<void*>(GetProcAddress(x, MAKEINTRESOURCEA(100))))
            if (MH_CreateHook(t, reinterpret_cast<void*>(&xget100_hook), reinterpret_cast<void**>(&g_xget100_orig)) == MH_OK &&
                MH_EnableHook(t) == MH_OK) ++ok;
    }
    LOG("Outfit Arc: pad filters on (%d of 5)", ok);
}

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
U64 call_inventory(U64 actor);

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

// A companion's piece in storage / the wardrobe: the UI request derives the bag from the item's type (the carried bag),
// but the request under it (0x9e0290) takes a bag id, as Kliff's direct equip does. For our call it gets the piece's
// own bag (hooked; every other call passes through untouched), so the game's own path does the rest.
using CompanionEquipFn = U64(__fastcall*)(U64, U64, U64, U64, U64, U64);
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

// --- Typed characters the prompt drops -------------------------------------------------------------
// The game's text box makes its own characters from the keys: letters (shifted too) and digits, but no Shift / AltGr
// symbols (! + / * ...) or accented letters, though pasting them works. While our prompt is open the characters
// Windows makes for the game window (WM_CHAR) are collected; once typing pauses for two frames, the ones the box didn't
// take are put in at the caret (input +0xd8 / +0xdc: caret and selection end, in characters), which then moves past them
// (+0xf1: the caret-moved flag the game's own keys set, so the caret is drawn there at once).

WNDPROC g_game_wndproc = nullptr;

// The wheel hook is in (the arc open): it alone counts the notches. The window and raw input routes then only keep
// the wheel from the game (2026-11-06: all three counted, 6 steps per notch, and with 3 outfits the arc went round
// twice and seemed not to move).
std::atomic<bool> g_wheel_hooked{false};
extern std::atomic<U64> g_peek_hud_at;

// Is (vk) with the modifiers held now one of the current outfits' hotkeys (in play)?
bool hotkey_held_now(U32 vk)
{
    if (!vk || g_menu_open || g_name_modal || GetTickCount64() - g_peek_hud_at.load() >= 250) return false;
    std::uint8_t mods = static_cast<std::uint8_t>(((GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0) |
                                                  ((GetKeyState(VK_MENU) & 0x8000) ? 2 : 0) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 4 : 0));
    bool bare = false; // (no hotkey with exactly these modifiers: the key alone counts, e.g. while Shift sprints)
    for (const auto& h : g_hotkeys) {
        U32 v = h.load();
        if (!v || (v & 0xFF) != vk) continue;
        if (((v >> 8) & 0xFF) == mods) return true;
        bare |= ((v >> 8) & 0xFF) == 0;
    }
    return bare;
}
extern std::atomic<bool> g_peek_live;
extern std::atomic<int> g_peek_wheel;

LRESULT CALLBACK name_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && hotkey_held_now(U32(wp)))
        return 0; // (an outfit's hotkey: its key does nothing else in play)
    if (m == WM_MBUTTONDOWN || m == WM_MBUTTONUP || m == WM_XBUTTONDOWN || m == WM_XBUTTONUP) {
        U32 vk = m == WM_MBUTTONDOWN || m == WM_MBUTTONUP ? VK_MBUTTON : GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        if (hotkey_held_now(vk)) return m == WM_XBUTTONDOWN || m == WM_XBUTTONUP ? TRUE : 0;
    }
    // The Outfit Arc: while it is open the wheel is its (not the camera's); its key's messages are its own in play.
    if (m == WM_MOUSEWHEEL) {
        if (g_peek_live) {
            if (!g_wheel_hooked) g_peek_wheel += GET_WHEEL_DELTA_WPARAM(wp); // (else the hook counts it)
            return 0;
        }
    }
    if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && g_peek_key.load() &&
        wp == g_peek_key.load() && GetTickCount64() - g_peek_hud_at.load() < 250 && !g_menu_open && !g_name_modal) {
        return 0;
    }
    if (m == WM_CHAR && g_name_modal && wp >= 0x20 && wp != 0x7f && (wp < 0xD800 || wp > 0xDFFF)) {
        std::lock_guard<std::mutex> lock(g_typed_mu);
        if (g_typed.empty()) g_typed_base = g_name_text, g_typed_caret = g_name_caret;
        g_typed += wchar_t(wp);
        g_typed_quiet = 0;
    }
    return CallWindowProcW(g_game_wndproc, w, m, wp, lp);
}

// Once, when our prompt first opens (the game window is in front then); stays in place, idle while no prompt is open.
void watch_game_chars()
{
    if (g_game_wndproc || !game_focused()) return;
    HWND w = GetForegroundWindow();
    g_game_wndproc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&name_wndproc)));
    LOG("rename: watching typed characters (window %p)", static_cast<void*>(w));
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

// Older builds of 5 November stored names; now the key code.
void peek_key_load(const std::string& id)
{
    static const std::pair<const char*, int> old[] = {{"tab", VK_TAB}, {"caps", VK_CAPITAL}, {"backquote", VK_OEM_3},
                                                       {"l", 'L'}, {"mouse3", VK_MBUTTON}, {"mouse4", VK_XBUTTON1},
                                                       {"mouse5", VK_XBUTTON2}};
    for (const auto& [n, vk] : old)
        if (id == n) return void(g_peek_key = vk);
    int vk = std::atoi(id.c_str());
    g_peek_key = vk > 0 && vk < 256 ? vk : 0;
}
const char* peek_key_id()
{
    static char buf[8];
    U32 vk = g_peek_key.load();
    if (!vk) return nullptr;
    snprintf(buf, sizeof buf, "%u", vk);
    return buf;
}

// The key's name as the keyboard layout calls it (the mouse buttons: our lines).
std::string peek_key_name(U32 vk)
{
    if (!vk) return lang::t("value.off");
    if (vk == VK_MBUTTON) return lang::t("key.mouse3");
    if (vk == VK_XBUTTON1) return lang::t("key.mouse4");
    if (vk == VK_XBUTTON2) return lang::t("key.mouse5");
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) { // (keys Windows names by their numpad twin unless flagged extended)
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_RCONTROL: case VK_RMENU: case VK_DIVIDE:
        sc |= 0x100;
    }
    wchar_t w[64] = {};
    int n = sc ? GetKeyNameTextW(static_cast<LONG>(sc << 16), w, 64) : 0;
    if (n <= 0) return "Key " + std::to_string(vk);
    char u[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, n, u, sizeof u - 1, nullptr, nullptr);
    return u;
}

// A hotkey as shown: "Ctrl + Shift + 3" (the modifiers' names as the keyboard layout calls them).
constexpr std::uint8_t kModCtrl = 1, kModAlt = 2, kModShift = 4;
std::string hotkey_name(U32 vk, std::uint8_t mods)
{
    if (!vk) return lang::t("value.no_hotkey");
    std::string s;
    for (auto [bit, mvk] : {std::pair<std::uint8_t, U32>{kModCtrl, VK_CONTROL}, {kModAlt, VK_MENU}, {kModShift, VK_SHIFT}})
        if (mods & bit) s += peek_key_name(mvk) + " + ";
    return s + peek_key_name(vk);
}

// The modifiers held now (exactly these must be held for a hotkey).
std::uint8_t mods_now()
{
    return static_cast<std::uint8_t>(((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? kModCtrl : 0) |
                                     ((GetAsyncKeyState(VK_MENU) & 0x8000) ? kModAlt : 0) |
                                     ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? kModShift : 0));
}

// The mouse button's raw input flags (0: a keyboard key).
USHORT peek_mouse_flags(U32 vk)
{
    if (vk == VK_MBUTTON) return RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_UP;
    if (vk == VK_XBUTTON1) return RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_4_UP;
    if (vk == VK_XBUTTON2) return RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_5_UP;
    return 0;
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
std::wstring g_log_path;

void self_test(U64 self)
{
    static bool model_done = false, bags_done = false, names_done = false;
    ensure_resist_ids();
    if (U64 tb = rp(self + kOffRootTabBar); tb && !g_tab_checked) { // (the current tab: only ever one of the headers)
        U32 cur = r32(tb + kOffTabCurrent), headers = r32(tb + kOffTabHeaderCount);
        g_tab_checked = true;
        LOG("self-test: current tab %u of %u headers: %s", cur, headers, cur < headers ? "ok" : "FAILED");
        if (cur >= headers && g_layout_bad.empty()) g_layout_bad = "tab state";
    }
    if (model_done && bags_done && names_done) return;
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    if (actor && !model_done) { // the character model behind the actor and its part table: Visibility
        U64 model = look_model(actor);
        U64 tab = model ? rp(model + 0x58) : 0;
        U32 n = model ? r32(model + 0x60) : 0;
        if (model) {
            U32 named = 0;
            for (U32 i = 0; tab && i < n && i < 1024; ++i)
                if (U64 part = rp(tab + i * 16ull + 8); part && readable_name(part_name(r32(part + 0x48)))) ++named;
            bool ok = tab && n >= 4 && n <= 1024 && named * 2 >= n;
            LOG("self-test: character model %llX: %u parts, %u named: %s", model, n, named, ok ? "ok" : "FAILED");
            if (!ok && g_vis_set) {
                g_vis_set = nullptr;
                feature_off("feature.visibility_rules");
            }
            model_done = true;
        }
    }
    if (actor && !bags_done) { // the bags and their capacity: Stash
        if (U64 mgr = call_inventory(actor)) {
            U64 bags = rp(mgr + kOffMgrBags);
            U32 n = r32(mgr + 0x20), sized = 0, odd = 0;
            for (U32 i = 0; bags && i < n && i < 64; ++i) {
                U64 bag = rp(bags + i * 8ull);
                short cap = 0;
                if (!bag || !rd(bag + kOffBagCount, cap)) continue;
                if (cap > 0 && cap <= 5000) ++sized;
                else if (cap != 0) ++odd;
            }
            bool ok = n >= 1 && n <= 64 && sized >= 1 && !odd;
            LOG("self-test: %u bags, %u with a capacity, %u odd: %s", n, sized, odd, ok ? "ok" : "FAILED");
            if (!ok && g_item_move) {
                g_item_move = nullptr;
                feature_off("feature.stash");
            }
            bags_done = true;
        }
    }
    if (ctx[0]) release_player(ctx);
    if (!names_done && g_item_info) { // a worn piece's key and name (the item record's layout): the names shown, only
        WornRaw w[kWornMax];
        int n = read_worn(self, w, kWornMax), worn = 0, named = 0;
        for (int i = 0; i < n; ++i)
            if (w[i].key != 0xFFFF) ++worn, named += readable_name(item_name(w[i].key));
        if (worn) {
            LOG("self-test: %d worn pieces, %d named: %s", worn, named, named ? "ok" : "FAILED");
            if (!named) feature_off("feature.item_names");
            names_done = true;
        }
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
bool g_features_toast_done = false;

// At the first menu open: one toast naming whatever had to be turned off.
void features_toast(U64 self)
{
    static bool store_told = false;
    if (g_store_unreadable && !store_told) {
        store_told = true;
        std::string why = g_store_why; // (its English detail is in the log)
        const char* key = !why.compare(0, 7, "damaged") ? "store.damaged" : !why.compare(0, 4, "from") ? "store.newer" : "store.locked";
        notify(self, lang::f("toast.store_bad", {{"why", lang::t(key)}}));
    }
    if (g_features_toast_done || g_features_off.empty()) return;
    g_features_toast_done = true;
    std::string list; // (feature.* keys)
    for (size_t a = 0; a < g_features_off.size();) {
        size_t b = g_features_off.find(',', a);
        if (b == std::string::npos) b = g_features_off.size();
        list += (list.empty() ? std::string() : lang::f("list.separator")) + lang::t(g_features_off.substr(a, b - a).c_str());
        a = b + 1;
    }
    notify(self, lang::f("toast.features_off", {{"list", list}}));
}

bool install()
{
    g_base = game::main_module().base;
    resolve_addresses();
    // The core: the Outfits tab, its wheel, saving (worn gear, icons, centre text) and the menu hooks.
    if (!have({kRvaRootQuickSlotVt, kRvaRootOpen, kRvaRootClose, kRvaRootFrame, kRvaRootFocus, kRvaRootTabChanged,
               kRvaRootCursorSelect, kRvaQuickSlotInfoGet, kRvaStringInfoGet, kRvaQuickSlotInfoMgr, kRvaClassNames,
               kRvaBuildBodies, kRvaGearMode, kRvaSetText, kRvaQsiTableRoot0, kRvaQsiTableRoot1, kRvaIntern,
               kRvaClassVecAdd, kRvaClassVecRemove, kRvaClassVecSort, kRvaRestyle, kRvaPlayerHolder, kRvaActorCtx,
               kRvaCtxRelease, kRvaCtxVt, kRvaItemIconName, kRvaEmptyItem, kRvaEmptyString, kRvaStrAssign,
               kRvaStrNormalize, kRvaStrLower, kRvaStrDestroy, kRvaSetStyleProp, kRvaTextureExists,
               kRvaIconScriptFlag, kRvaSetState, kRvaStateShow, kRvaStateHide, kRvaFocusedSlot,
               kRvaEquip, kRvaUnequip, kRvaInventory})) { // without loading the tab has no purpose: all or nothing
        LOG("gear menu or equip addresses not found; unsupported game version");
        return false;
    }
    void** vt = reinterpret_cast<void**>(g_base + kRvaRootQuickSlotVt);
    g_intern = reinterpret_cast<InternFn>(g_base + kRvaIntern);
    g_vec_add = reinterpret_cast<VecAddFn>(g_base + kRvaClassVecAdd);
    g_vec_remove = reinterpret_cast<VecRemoveFn>(g_base + kRvaClassVecRemove);
    g_vec_sort = reinterpret_cast<VoidFn>(g_base + kRvaClassVecSort);
    g_restyle = reinterpret_cast<VoidFn>(g_base + kRvaRestyle);
    g_qsi_get = reinterpret_cast<RecordGetFn>(g_base + kRvaQuickSlotInfoGet);
    g_str_get = reinterpret_cast<RecordGetFn>(g_base + kRvaStringInfoGet);
    g_set_text = reinterpret_cast<SetTextFn>(g_base + kRvaSetText);
    g_actor_ctx = reinterpret_cast<CtxFn>(g_base + kRvaActorCtx);
    g_ctx_release = reinterpret_cast<CtxReleaseFn>(g_base + kRvaCtxRelease);
    g_icon_name = reinterpret_cast<IconNameFn>(g_base + kRvaItemIconName);
    g_str_assign = reinterpret_cast<StrAssignFn>(g_base + kRvaStrAssign);
    g_str_normalize = reinterpret_cast<StrFn>(g_base + kRvaStrNormalize);
    g_str_lower = reinterpret_cast<StrFn>(g_base + kRvaStrLower);
    g_str_destroy = reinterpret_cast<StrFn>(g_base + kRvaStrDestroy);
    g_set_style_prop = reinterpret_cast<SetStylePropFn>(g_base + kRvaSetStyleProp);
    g_texture_exists = reinterpret_cast<TextureExistsFn>(g_base + kRvaTextureExists);
    g_set_state = reinterpret_cast<SetStateFn>(g_base + kRvaSetState);
    g_focused_slot = reinterpret_cast<FocusedSlotFn>(g_base + kRvaFocusedSlot);
    g_equip = reinterpret_cast<EquipFn>(g_base + kRvaEquip);
    if (have({kRvaEquipRequest, kRvaUnequipRequest})) {
        g_equip_request = reinterpret_cast<EquipRequestFn>(g_base + kRvaEquipRequest);
        g_unequip_request = reinterpret_cast<UnequipRequestFn>(g_base + kRvaUnequipRequest);
    } else {
        feature_off("feature.companions");
    }
    g_unequip = reinterpret_cast<UnequipFn>(g_base + kRvaUnequip);
    g_inventory = reinterpret_cast<InventoryFn>(g_base + kRvaInventory);
    if (have({kRvaErrorMgr, kRvaErrorText})) { // only the text of the game's equip errors, in the log
        g_error_mgr = reinterpret_cast<ErrorMgrFn>(g_base + kRvaErrorMgr);
        g_error_text = reinterpret_cast<ErrorTextFn>(g_base + kRvaErrorText);
    }
    if (have({kRvaToast}))
        g_toast = reinterpret_cast<ToastFn>(g_base + kRvaToast);
    else
        LOG("toast function not found; notifications only go to the log");
    if (MH_STATUS s = MH_Initialize(); s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) return false;
    // Killswitch: from here on any failure undoes every hook, so the mod is either fully on or fully off (and the
    // Outfits header, hidden in the thtml, is only ever shown by a working plugin).
    struct VtPatch { int slot; void* fn; void** orig; } patches[] = {
        {kSlotOpen, reinterpret_cast<void*>(&open_hook), reinterpret_cast<void**>(&g_open_orig)},
        {kSlotTabChanged, reinterpret_cast<void*>(&tab_hook), reinterpret_cast<void**>(&g_tab_orig)},
        {kSlotCursorSelect, reinterpret_cast<void*>(&cursor_select_hook), reinterpret_cast<void**>(&g_select_orig)},
        {kSlotClose, reinterpret_cast<void*>(&close_hook), reinterpret_cast<void**>(&g_close_orig)},
        {kSlotFrame, reinterpret_cast<void*>(&frame_hook), reinterpret_cast<void**>(&g_frame_orig)},
        {kSlotFocus, reinterpret_cast<void*>(&focus_hook), reinterpret_cast<void**>(&g_focus_orig)},
    };
    auto fail = [&](const char* what) {
        MH_DisableHook(MH_ALL_HOOKS);
        for (auto& p : patches)
            if (*p.orig) game::patch_vtable(vt, p.slot, *p.orig);
        LOG("%s; every hook undone", what);
        return false;
    };
    if (!install_save_hooks()) return fail("file hooks failed");
    struct { U64 rva; void* fn; void** orig; } hooks[] = {
        {kRvaQuickSlotInfoGet, reinterpret_cast<void*>(&qsi_get_hook), reinterpret_cast<void**>(&g_qsi_get)},
        {kRvaBuildBodies, reinterpret_cast<void*>(&build_hook), reinterpret_cast<void**>(&g_build_orig)},
        {kRvaGearMode, reinterpret_cast<void*>(&gear_mode_hook), reinterpret_cast<void**>(&g_gear_orig)},
    };
    for (auto& h : hooks) {
        void* target = reinterpret_cast<void*>(g_base + h.rva);
        if (MH_CreateHook(target, h.fn, h.orig) != MH_OK || MH_EnableHook(target) != MH_OK) {
            LOG("hook at +%llX failed", h.rva);
            return fail("gear menu hook failed");
        }
    }
    for (auto& p : patches) {
        *p.orig = vt[p.slot]; // before the patch: the game may call the hook at once
        if (!game::patch_vtable(vt, p.slot, p.fn)) return fail("RootQuickSlot vtable patch failed");
    }
    LOG("hooks: getter, builder, gear mode, open %p, tab %p, select %p, close %p, frame %p, focus %p (base %llX)",
        g_open_orig, g_tab_orig, g_select_orig, g_close_orig, g_frame_orig, g_focus_orig, g_base);
    install_rename_prompt();
    install_ask_dialog();
    if (g_equip_request && have({kRvaCompanionEquip})) { // companions' pieces from storage / the wardrobe
        void* t = reinterpret_cast<void*>(g_base + kRvaCompanionEquip);
        if (MH_CreateHook(t, reinterpret_cast<void*>(&companion_equip_hook),
                          reinterpret_cast<void**>(&g_companion_equip_orig)) != MH_OK ||
            MH_EnableHook(t) != MH_OK)
            g_companion_equip_orig = nullptr;
    }
    if (!g_companion_equip_orig) LOG("companions: storage / wardrobe pieces unavailable (Kliff only)");
    if (have({kRvaVisSet, kRvaVisRelease, kRvaBattleVis, kRvaSlotHandle})) { // Appearance
        void* t = reinterpret_cast<void*>(g_base + kRvaBattleVis);
        g_vis_set = reinterpret_cast<VisSetFn>(g_base + kRvaVisSet);
        g_vis_release = reinterpret_cast<VisReleaseFn>(g_base + kRvaVisRelease);
        g_slot_handle = reinterpret_cast<SlotHandleFn>(g_base + kRvaSlotHandle);
        if (have({kRvaBattleCheck})) g_battle_check = reinterpret_cast<BattleCheckFn>(g_base + kRvaBattleCheck);
        else LOG("appearance: battle check not found; combat looks follow the headgear switch");
        if (MH_CreateHook(t, reinterpret_cast<void*>(&battle_vis_hook), reinterpret_cast<void**>(&g_battle_vis_orig)) !=
                MH_OK ||
            MH_EnableHook(t) != MH_OK)
            g_vis_set = nullptr;
        void* s = reinterpret_cast<void*>(g_base + kRvaModelSwap); // (without it: pieces put on elsewhere wait for a switch)
        if (g_vis_set && have({kRvaModelSwap}) &&
            (MH_CreateHook(s, reinterpret_cast<void*>(&model_swap_hook), reinterpret_cast<void**>(&g_model_swap_orig)) !=
                 MH_OK ||
             MH_EnableHook(s) != MH_OK))
            LOG("appearance: model rebuild hook failed");
        void* vs = reinterpret_cast<void*>(g_base + kRvaVisSet); // flight: the flight cloak's layer (our calls: past it)
        void* vr = reinterpret_cast<void*>(g_base + kRvaVisRelease);
        if (g_vis_set && MH_CreateHook(vs, reinterpret_cast<void*>(&vis_set_hook), reinterpret_cast<void**>(&g_vis_set_orig)) == MH_OK &&
            MH_CreateHook(vr, reinterpret_cast<void*>(&vis_release_hook), reinterpret_cast<void**>(&g_vis_release_orig)) == MH_OK &&
            MH_EnableHook(vs) == MH_OK && MH_EnableHook(vr) == MH_OK)
            g_vis_set = g_vis_set_orig, g_vis_release = g_vis_release_orig;
        else
            LOG("appearance: flight hooks failed (the hat / cloak may show in flight)");
        void* c = reinterpret_cast<void*>(g_base + kRvaEquipVisCheck); // (without it: weapon looks also apply when drawn)
        if (g_vis_set && have({kRvaEquipVisCheck}) &&
            (MH_CreateHook(c, reinterpret_cast<void*>(&equip_vis_check_hook),
                           reinterpret_cast<void**>(&g_equip_vis_check_orig)) != MH_OK ||
             MH_EnableHook(c) != MH_OK))
            LOG("appearance: drawn-weapon hook failed");
    }
    if (!g_vis_set) feature_off("feature.appearance");
    // Slot modes: scrolling an Outfits slot (without them a slot only loads / saves).
    bool scroll_ok = false;
    if (have({kRvaSlotScript181, kRvaSlotScript182})) {
        void* p = reinterpret_cast<void*>(g_base + kRvaSlotScript181);
        void* n = reinterpret_cast<void*>(g_base + kRvaSlotScript182);
        scroll_ok = MH_CreateHook(p, reinterpret_cast<void*>(&scroll_prev_hook),
                                  reinterpret_cast<void**>(&g_scroll_prev_orig)) == MH_OK &&
                    MH_CreateHook(n, reinterpret_cast<void*>(&scroll_next_hook),
                                  reinterpret_cast<void**>(&g_scroll_next_orig)) == MH_OK &&
                    MH_EnableHook(p) == MH_OK && MH_EnableHook(n) == MH_OK;
    }
    if (!scroll_ok) feature_off("feature.settings");
    if (have({kRvaSlotScript164})) { // (without it the Outfits slots blink after equipment changes; nothing else)
        void* t = reinterpret_cast<void*>(g_base + kRvaSlotScript164);
        if (MH_CreateHook(t, reinterpret_cast<void*>(&slot_refresh_hook), reinterpret_cast<void**>(&g_slot_refresh_orig)) !=
                MH_OK ||
            MH_EnableHook(t) != MH_OK)
            LOG("slot refresh: not hooked (the Outfits slots may blink after equipment changes)");
    }
    if (scroll_ok && have({kRvaItemSelectorVt, kRvaSelectorSelect, kOffSlotSelector, kOffSelectorCount}))
    {
        void* t = reinterpret_cast<void*>(g_base + kRvaSelectorSelect);
        if (MH_CreateHook(t, reinterpret_cast<void*>(&selector_select_hook),
                          reinterpret_cast<void**>(&g_selector_select_orig)) == MH_OK &&
            MH_EnableHook(t) == MH_OK)
            g_selector_select = reinterpret_cast<SelectorSelectFn>(g_base + kRvaSelectorSelect); // (-> the hook)
        else
            LOG("slot dots: the select could not be hooked; no dots");
    }
    if (have({kRvaItemInfo, kRvaDefaultItemInfo}))
        g_item_info = reinterpret_cast<ItemInfoFn>(g_base + kRvaItemInfo);
    LOG("item names %s", g_item_info ? "ready" : "unavailable (messages name the slot only)");
    if (have({kRvaItemMove, kRvaBagInfo})) {
        g_item_move = reinterpret_cast<ItemMoveFn>(g_base + kRvaItemMove);
        g_bag_info = reinterpret_cast<BagInfoFn>(g_base + kRvaBagInfo);
    }
    if (g_item_move && have({kRvaHudVt})) { // (the stash check's tick with the wheel closed; else it waits for the wheel)
        void** hvt = reinterpret_cast<void**>(g_base + kRvaHudVt);
        if (hvt[kSlotFrame] == reinterpret_cast<void*>(g_base + kRvaRootFrame)) {
            g_hud_frame_orig = reinterpret_cast<FrameFn>(hvt[kSlotFrame]);
            if (!game::patch_vtable(hvt, kSlotFrame, reinterpret_cast<void*>(&hud_frame_hook))) g_hud_frame_orig = nullptr;
        }
    }
    LOG("stash %s%s", g_item_move ? "ready" : "unavailable (its game functions were not found)",
        g_item_move && !g_hud_frame_orig ? " (its report waits for the wheel: HUD frame not hooked)" : "");
    return true;
}

bool in_game_process()
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe = buf;
    exe = exe.substr(exe.find_last_of(L"/\\") + 1);
    return _wcsicmp(exe.c_str(), L"CrimsonDesert.exe") == 0;
}

bool game_focused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Controllers: the stick picks a slot (the game's own cursor) and L2 / R2 scroll it through the game's own handler once
// the L2 / R2 prompts show (pad_prompts). The wheel has no confirm button of its own, so the pad is read directly and
// Cross / A counts as Space:
//  - Xbox pads (and anything shown as one, e.g. Steam Input's virtual pad): XInput, loaded at run time; a port that
//    reads as disconnected is skipped for 2 s (polling an empty port is slow).
//  - PlayStation pads (DualSense, DualSense Edge, DualShock 4; the game reads them natively, XInput never sees them):
//    their HID input reports, read by a thread that waits for the pad (no polling); the latest buttons are kept here.
constexpr U8 kPadCross = 1;   // the bits pad_buttons() returns: Cross / A held,
constexpr U8 kPadTouched = 2; // any button, trigger or stick in use (the pad is the input in use),
constexpr U8 kPadL1 = 4;      // L1 / LB held (the Outfit Arc's modifier),
constexpr U8 kPadUp = 8;      // the d-pad's up (or a diagonal with it),
constexpr U8 kPadDown = 16;   // and down

struct XPad {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XState {
    DWORD dwPacketNumber;
    XPad Gamepad;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XState*);
constexpr WORD kXInputA = 0x1000; // XINPUT_GAMEPAD_A
constexpr WORD kXInputUp = 0x0001, kXInputDown = 0x0002, kXInputLB = 0x0100; // XINPUT_GAMEPAD_DPAD_UP / _DOWN / LEFT_SHOULDER

U8 xinput_buttons()
{
    static XInputGetStateFn get = [] {
        for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryW(dll))
                if (auto f = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"))) {
                    LOG("controller input (Xbox pads): %ls", dll);
                    return f;
                }
        LOG("controller input: no XInput found (Xbox pads won't work in the Outfits tab)");
        return static_cast<XInputGetStateFn>(nullptr);
    }();
    static U64 skip_until[4] = {};
    if (!get) return 0;
    U64 now = GetTickCount64();
    U8 b = 0;
    struct Own { bool was = t_own_pad; Own() { t_own_pad = true; } ~Own() { t_own_pad = was; } } own; // (unfiltered)
    for (DWORD i = 0; i < 4; ++i) {
        if (now < skip_until[i]) continue;
        XState s{};
        if (get(i, &s) != ERROR_SUCCESS) {
            skip_until[i] = now + 2000;
            continue;
        }
        if (s.Gamepad.wButtons & kXInputA) b |= kPadCross;
        if (s.Gamepad.wButtons & kXInputLB) b |= kPadL1;
        if (s.Gamepad.wButtons & kXInputUp) b |= kPadUp;
        if (s.Gamepad.wButtons & kXInputDown) b |= kPadDown;
        if (s.Gamepad.wButtons || s.Gamepad.bLeftTrigger > 64 || s.Gamepad.bRightTrigger > 64 ||
            std::abs(s.Gamepad.sThumbLX) > 12000 || std::abs(s.Gamepad.sThumbLY) > 12000 ||
            std::abs(s.Gamepad.sThumbRX) > 12000 || std::abs(s.Gamepad.sThumbRY) > 12000)
            b |= kPadTouched;
    }
    return b;
}

// PlayStation pads over HID. The face-button byte (Square 0x10, Cross 0x20, Circle 0x40, Triangle 0x80) by report, as
// documented by the community (SDL, DS4Windows): DualSense USB 0x01 [8], Bluetooth 0x31 (full mode, as Steam sets it)
// [9], Bluetooth simple 0x01 [5]; DualShock 4 USB 0x01 [5], Bluetooth 0x11 [7].
std::atomic<U8> g_hid_buttons{0};
HANDLE g_hid_wake = nullptr; // set when the menu opens: a thread without a pad looks again

struct PadKind {
    USHORT pid;
    bool dualsense;
};
constexpr USHORT kSonyVid = 0x054C;
constexpr PadKind kPads[] = {{0x0CE6, true}, {0x0DF2, true}, {0x05C4, false}, {0x09CC, false}, {0x0BA0, false}};

// The sticks are the four bytes right after the report id (DualShock 4 over Bluetooth: two later), 0x80 = centred; the
// face byte's low nibble is the d-pad (8 = none), the next two bytes hold the shoulder / stick / menu buttons.
U8 hid_parse(const unsigned char* r, DWORD n, bool dualsense)
{
    int face = -1, sticks = 1;
    if (dualsense) {
        if (r[0] == 0x01 && n >= 64) face = 8;                   // USB
        else if (r[0] == 0x31 && n >= 11) face = 9, sticks = 2;  // Bluetooth, full
        else if (r[0] == 0x01 && n >= 7) face = 5;               // Bluetooth, simple
    } else {
        if (r[0] == 0x01 && n >= 7) face = 5;                    // USB (and Bluetooth simple)
        else if (r[0] == 0x11 && n >= 9) face = 7, sticks = 3;   // Bluetooth
    }
    if (face < 0 || DWORD(face + 2) >= n) return 0;
    U8 b = (r[face] & 0x20) ? kPadCross : 0;
    U8 dpad = r[face] & 0x0F; // (0 up, clockwise to 7 up-left; 8 none)
    if (dpad == 0 || dpad == 1 || dpad == 7) b |= kPadUp;
    if (dpad >= 3 && dpad <= 5) b |= kPadDown;
    if (r[face + 1] & 0x01) b |= kPadL1; // (L1: the first bit of the next byte, DualSense and DualShock 4 alike)
    bool touched = (r[face] & 0xF0) || (r[face] & 0x0F) != 8 || r[face + 1] || (r[face + 2] & 0x03);
    for (int s = 0; s < 4 && !touched; ++s) touched = std::abs(int(r[sticks + s]) - 0x80) > 48;
    return b | (touched ? kPadTouched : 0);
}

// The first connected PlayStation pad: an open handle (shared with the game and Steam) and its input report size.
HANDLE hid_open(bool& dualsense, DWORD& report_len)
{
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA itf{sizeof itf};
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &itf); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &itf, nullptr, 0, &need, nullptr);
        std::vector<char> buf(need);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!need || !SetupDiGetDeviceInterfaceDetailW(set, &itf, detail, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{sizeof at};
        bool ours = false;
        if (HidD_GetAttributes(h, &at) && at.VendorID == kSonyVid)
            for (const PadKind& k : kPads)
                if (at.ProductID == k.pid) ours = true, dualsense = k.dualsense;
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps{};
        if (ours && HidD_GetPreparsedData(h, &pp)) {
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength) {
                report_len = caps.InputReportByteLength;
                found = h;
                LOG("controller input (PlayStation pad): %s, product %04X, reports of %lu bytes",
                    dualsense ? "DualSense" : "DualShock 4", at.ProductID, report_len);
            }
            HidD_FreePreparsedData(pp);
        }
        if (found != h) CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// Waits for a PlayStation pad's reports and keeps its buttons in g_hid_buttons. Without a pad (or once it is gone),
// it waits for the menu to open and looks again.
DWORD WINAPI hid_thread(LPVOID)
{
    t_own_pad = true; // (our reading: never filtered, never taken for the game's)
    for (;;) {
        bool dualsense = false;
        DWORD len = 0;
        HANDLE h = hid_open(dualsense, len);
        if (h == INVALID_HANDLE_VALUE) {
            g_hid_buttons = 0;
            WaitForSingleObject(g_hid_wake, INFINITE);
            continue;
        }
        std::vector<unsigned char> r(len);
        DWORD got = 0;
        while (ReadFile(h, r.data(), len, &got, nullptr)) g_hid_buttons = hid_parse(r.data(), got, dualsense);
        LOG("controller input: the PlayStation pad stopped answering (disconnected?)");
        CloseHandle(h);
        g_hid_buttons = 0;
    }
}

// The pads' buttons, and which input is in use: a touched pad switches to it (its family too); the mouse moving or
// clicking, or Space, switches back. A change asks pump to re-show the slots (g_prompt_changed).
U8 pad_buttons_raw() { return xinput_buttons() | g_hid_buttons.load(); }

U8 pad_buttons()
{
    U8 x = xinput_buttons(), h = g_hid_buttons.load();
    static POINT last{};
    POINT p{};
    bool mouse = GetCursorPos(&p) && (std::abs(p.x - last.x) > 2 || std::abs(p.y - last.y) > 2);
    last = p;
    // A PlayStation pad through Steam Input shows up on both readers; the game then sees the virtual Xbox pad (and shows
    // Xbox prompts), so XInput wins the icon family.
    bool pad = (x | h) & kPadTouched, ps = g_pad_is_ps;
    if (h & kPadTouched) ps = true;
    if (x & kPadTouched) ps = false;
    bool keys = (GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
    bool in_use = pad ? true : (mouse || keys) ? false : g_pad_in_use.load();
    if (in_use != g_pad_in_use || (in_use && ps != g_pad_is_ps)) {
        g_pad_in_use = in_use, g_pad_is_ps = ps;
        g_prompt_changed = true;
    }
    return x | h;
}

// Latches Space / Cross / A presses and left clicks (game window focused, menu open) for the UI thread (pump), and
// follows the game's save files (poll_saves). The keys are only read while the menu is open.
DWORD WINAPI key_thread(LPVOID)
{
    bool space = false, lmb = false, open = false;
    for (U32 tick = 0;; ++tick) {
        Sleep(15);
        if (tick % 16 == 0) poll_saves();
        bool was_open = open;
        open = g_menu_open && !g_name_modal && !g_ask_modal; // not while a prompt is open (re-baselined afterwards)
        if (!open) continue;
        if (!was_open && g_hid_wake) SetEvent(g_hid_wake); // (a PlayStation pad plugged in since: found now)
        U8 pb = pad_buttons();
        bool s = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0 || (pb & kPadCross);
        bool m = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (!was_open) space = s, lmb = m; // held while the menu opened: not a press
        if (s && !space && game_focused()) g_space_at = GetTickCount64();
        if (m && !lmb && game_focused()) {
            POINT p;
            HWND w = GetForegroundWindow();
            g_click_pos = GetCursorPos(&p) && ScreenToClient(w, &p) ? (U64(U32(p.x)) << 32) | U32(p.y) : ~0ull;
            g_click_at = GetTickCount64();
        }
        space = s;
        lmb = m;
    }
}

// --- Gear-wheel UI, patched in memory -----------------------------------------------------------
// The game builds its screens from compiled UI files (ui/bin__/*.uibinary). The Outfits tab header and the value text
// are added to the gear wheel's files while the game loads them (uibin_patch.cpp), onto whatever copy it loads: its
// own, or another mod's that DMM chose. The mod ships none of these files, so it doesn't conflict over them, and a game
// update that changes them needs no new version as long as the elements copied are still there.
// The loader reads a file (path from "%#%#%#_%#.uibinary") and wraps the bytes in a stream on its stack
// {vt, .., data +0x20, size u32 +0x28, pos u32 +0x2c} for the parser (its only call). The hook gives the parser a
// patched copy instead (kept for the whole run: what the parser makes may point into it). The path is the loader's
// local: inline, or on the heap when long; where both are relative to the stream is read from the loader's code.
using UiParseFn = U64(__fastcall*)(U64 loader, U64 stream);
UiParseFn g_ui_parse_orig = nullptr;
U64 g_ui_parse_ret = 0;                                // the parser call's return address (the loader's call)
std::int64_t g_ui_path_inline = 0, g_ui_path_heap = 0; // the path buffer / its heap pointer, relative to the stream
std::string g_ui_hook_note;                            // how the hook went in (logged by the init thread)
std::mutex g_ui_mutex;
std::deque<std::vector<U8>> g_ui_patched; // the patched files, alive for the run

bool ui_copy(U64 from, void* to, size_t n)
{
    __try {
        std::memcpy(to, reinterpret_cast<const void*>(from), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string ui_stream_path(U64 stream)
{
    char buf[0x106] = {};
    U64 heap = 0;
    U64 src = rd(stream + g_ui_path_heap, heap) && plausible(heap) ? heap : stream + g_ui_path_inline;
    if (!ui_copy(src, buf, sizeof buf - 1)) return {};
    buf[sizeof buf - 1] = 0;
    for (char* c = buf; *c; ++c)
        if (*c < 0x20 || *c > 0x7e) return {};
    std::string r = buf;
    return r.size() > 9 && _stricmp(r.c_str() + r.size() - 9, ".uibinary") == 0 ? r : std::string();
}

void ui_patch_stream(U64 stream, const std::string& path, unsigned what)
{
    U64 data = 0;
    U32 size = 0, pos = 0;
    if (!rd(stream + 0x20, data) || !rd(stream + 0x28, size) || !rd(stream + 0x2c, pos) || !plausible(data) || !size ||
        size > (64u << 20) || pos) {
        LOG("gear wheel UI: %s left as it is (its stream isn't as expected: %llX, %u bytes, at %u)", path.c_str(), data,
            size, pos);
        return;
    }
    std::vector<U8> in(size), out;
    if (!ui_copy(data, in.data(), size)) {
        LOG("gear wheel UI: %s left as it is (bytes unreadable)", path.c_str());
        return;
    }
    uibin::Report r;
    if (!uibin::patch(in, what, out, r)) {
        LOG("gear wheel UI: %s left as it is: %s", path.c_str(), r.error.c_str());
        return;
    }
    std::lock_guard lock(g_ui_mutex);
    g_ui_patched.push_back(std::move(out));
    const std::vector<U8>& kept = g_ui_patched.back();
    *reinterpret_cast<U64*>(stream + 0x20) = reinterpret_cast<U64>(kept.data());
    *reinterpret_cast<U32*>(stream + 0x28) = U32(kept.size());
    if (r.headers) ++g_ui_tab_added;
    LOG("gear wheel UI: %s: %d tab header(s), %d value text(s), %d peek(s) added (%u -> %zu bytes)", path.c_str(),
        r.headers, r.values, r.peeks, size, kept.size());
}

U64 __fastcall ui_parse_hook(U64 loader, U64 stream)
{
    if (reinterpret_cast<U64>(_ReturnAddress()) == g_ui_parse_ret) {
        try {
            std::string path = ui_stream_path(stream);
            if (unsigned what = path.empty() ? 0 : uibin::wants(path)) ui_patch_stream(stream, path, what);
        } catch (...) { // (out of memory: the file stays as it is)
        }
    }
    return g_ui_parse_orig(loader, stream);
}

// At load (DllMain), before the game builds its UI; again from the init thread if that didn't work.
bool install_ui_patch()
{
    if (g_ui_parse_orig) return true;
    if (!g_base) g_base = game::main_module().base;
    // mov rax,[rsp+70]; mov [rbp+..],rax; mov eax,[rsp+7C]; mov [rbp+..],eax; mov [rbp+..],r12d; lea rdx,[rbp+s];
    // mov rcx,rsi; call parser  (unique in the 10-30, 11-01b and 11-04 exes)
    U64 site = game::find_unique("48 8B 44 24 70 48 89 45 ?? 8B 44 24 7C 89 45 ?? 44 89 65 ?? 48 8D 55 ?? 48 8B CE E8");
    if (!site) return g_ui_hook_note = "the UI loader's parser call not found", false;
    std::int8_t stream_at = 0;
    std::int32_t rel = 0;
    rd(site + 23, stream_at), rd(site + 28, rel);
    // lea r15,[rbp+i]; mov rcx,[rbp+h]; test rcx,rcx; cmovne r15,rcx: the path buffer, inline or on the heap
    const U8 pat[] = {0x4C, 0x8D, 0x7D, 0, 0x48, 0x8B, 0x8D, 0, 0, 0, 0, 0x48, 0x85, 0xC9, 0x4C, 0x0F, 0x45, 0xF9};
    U64 hit = 0;
    int hits = 0;
    for (U64 a = site - 0x600; a < site; ++a) {
        bool ok = true;
        for (size_t k = 0; k < sizeof pat && ok; ++k)
            ok = k == 3 || (k >= 7 && k <= 10) || *reinterpret_cast<const U8*>(a + k) == pat[k];
        if (ok) hit = a, ++hits;
    }
    if (hits != 1) return g_ui_hook_note = "the UI loader's path buffer not found (" + std::to_string(hits) + ")", false;
    std::int8_t inl = 0;
    std::int32_t heap = 0;
    rd(hit + 3, inl), rd(hit + 7, heap);
    g_ui_path_inline = std::int64_t(inl) - stream_at;
    g_ui_path_heap = std::int64_t(heap) - stream_at;
    U64 call = site + 27, parser = call + 5 + rel;
    g_ui_parse_ret = call + 5;
    MH_STATUS init = MH_Initialize();
    void* fn = reinterpret_cast<void*>(parser);
    if ((init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) ||
        MH_CreateHook(fn, reinterpret_cast<void*>(&ui_parse_hook), reinterpret_cast<void**>(&g_ui_parse_orig)) != MH_OK ||
        MH_EnableHook(fn) != MH_OK) {
        g_ui_parse_orig = nullptr;
        return g_ui_hook_note = "the UI parser hook failed", false;
    }
    char t[160];
    std::snprintf(t, sizeof t, "parser +%llX (call +%llX), path at stream%+lld / [stream%+lld]", parser - g_base,
                  call - g_base, g_ui_path_inline, g_ui_path_heap);
    g_ui_hook_note = t;
    return true;
}

// --- Quick peek ----------------------------------------------------------------------------------
// The outfits on an arc beside the character, to switch without the gear wheel. Its elements are added to the stamina
// gauge's tracker (statusgaugeview2) as the game loads it (uibin_patch.cpp: OmPeekWrap > OmPeekArc, OmPeekFrame0-4,
// OmPeekIcon0-4, OmPeekLabel, OmPeekName); tools/peek_gen.py makes the layout. A carousel: the highlighted entry sits
// in the middle (position 2), the others around it, wrapping; it opens on the worn outfit, or on an extra "current
// outfit" entry when what is worn isn't a saved outfit (choosing it changes nothing).
// It follows the character (in the HP tracker, which the game never shows or hides: the plugin shows it while the peek
// is, its own gauge hidden; the stamina tracker flickered, the game hiding it while stamina is full). Everything runs in the screen's own per-frame call (its root script
// UIGamePlayControlRootStatusGauge, vtable slot kSlotFrame, the shared root frame function; self+8 = the root element),
// on the UI thread, whether or not the wheel was ever opened.
// Its distance is fixed (tools/peek_gen.py), as the user preferred after trying distances following the camera zoom,
// the character's size measured from two trackers, and a fixed spot on the screen (2026-11-05).
FrameFn g_gauge_frame_orig = nullptr;
U64 g_peek_root = 0;
struct Peek {
    bool open = false;
    std::vector<int> entries;        // outfit indices in the Outfits tab's order; -1 = the current, unsaved outfit
    size_t sel = 0;                  // the highlighted entry (shown in the middle)
    int worn = -1;
    std::string current_icon;        // the worn chest piece's icon (the unsaved entry)
    U64 closing_at = 0;              // closing: its animation plays, then it hides (GetTickCount64 of the close; 0 = not)
    U16 character = 0xFFFF;          // whose outfits it shows (a switch closes it: they'd be the wrong ones)
    U64 self = 0;                    // the status screen it was opened from (loads and their questions go through it)
    U64 wrap = 0, arc = 0, frame[5] = {}, icon[5] = {}, label = 0, name = 0;
    U64 badge[5] = {}, nb[2] = {};   // each slot's climate badge; the highlighted outfit's badges by its label
    std::string badge_set[5], nb_set[2];
    U64 trk = 0, hp_gauge = 0;       // the HP tracker (the peek's home) and its own gauge (hidden while shown)
    std::string icon_set[5];         // last icon set per position
} g_peek;
bool g_peek_down = false;             // the arc key on the last frame

bool g_peek_by_pad = false;           // the open arc came from the pad (L1 / LB held: it closes when that lets go)
std::atomic<bool> g_peek_pad_open{false}; // (the same, for the pad filters on other threads)
bool peek_pad_owned() { return g_peek_live && g_peek_pad_open; }
U8 g_peek_pad_last = 0;               // the pad's buttons on the last frame (for presses)

std::atomic<bool> g_peek_live{false};  // the arc is open (read by the input filters: the wheel is the arc's)
std::atomic<U64> g_peek_hud_at{0};     // the status screen's last frame with the arc ready (tick): the key is the arc's
std::atomic<int> g_peek_wheel{0};      // wheel movement kept from the game while the arc is open (WHEEL_DELTA units)

bool peek_elements()
{
    Peek& p = g_peek;
    if (p.wrap && is_element(p.wrap) && has_class(p.wrap, "om-peek-wrap")) return true;
    p = Peek{p.open, p.entries, p.sel, p.worn, p.current_icon};
    p.wrap = find_by_class(g_peek_root, "om-peek-wrap", 8);
    if (!p.wrap) return false;
    for (int k = 0; k < 5; ++k) {
        p.frame[k] = find_by_class(p.wrap, ("om-peek-frame-" + std::to_string(k)).c_str(), 1);
        p.icon[k] = find_by_class(p.wrap, ("om-peek-icon-" + std::to_string(k)).c_str(), 1);
    }
    p.label = find_by_class(p.wrap, "om-peek-label", 1), p.name = find_by_class(p.wrap, "om-peek-name", 1);
    p.arc = find_by_class(p.wrap, "om-peek-arc", 1);
    for (int k = 0; k < 5; ++k) p.badge[k] = find_by_class(p.wrap, ("om-peek-badge-" + std::to_string(k)).c_str(), 1);
    for (int k = 0; k < 2; ++k) p.nb[k] = find_by_class(p.wrap, ("om-peek-nb-" + std::to_string(k)).c_str(), 1);
    p.trk = find_by_class(g_peek_root, "cpp-hp-actor-tracker", 4);
    p.hp_gauge = find_by_class(p.trk, "hp-sub-gauge-bar-wrap", 1);
    bool all = p.label && p.name && p.arc && p.trk;
    for (int k = 0; k < 5; ++k) all &= p.frame[k] && p.icon[k];
    LOG("Outfit Arc: elements %s", all ? "found" : "INCOMPLETE");
    return all;
}

// The carousel as it is now: position k shows entry sel + (k - 2), wrapping; with fewer than 5 entries the ends stay
// empty rather than repeat one.
void peek_draw(U64 self)
{
    Peek& p = g_peek;
    size_t n = p.entries.size();
    // the arc fits the shown slots (om-peek-arc-n1..n5: tools/peek_gen.py)
    for (int v = 1; v <= 5; ++v)
        set_class(p.arc, ("om-peek-arc-n" + std::to_string(v)).c_str(), size_t(v) == std::clamp<size_t>(n, 1, 5));
    for (int k = 0; k < 5; ++k) {
        int off = k - 2;
        bool shown = n && (off >= -int((n - 1) / 2) && off <= int(n / 2));
        set_class(p.frame[k], "!cpp-none", !shown);
        std::string pic;
        if (shown) {
            int e = p.entries[(p.sel + n + size_t(off % int(n) + int(n))) % n];
            pic = e < 0 ? p.current_icon : e < int(g_outfits.size()) ? g_outfits[size_t(e)].chest_icon : std::string();
        }
        if (pic != p.icon_set[k]) {
            int r = pic.empty() ? 1 : set_image_raw(self, p.icon[k], pic.c_str());
            if (r != 1) LOG("Outfit Arc: icon \"%s\" at position %d: %s", pic.c_str(), k, r ? "fault" : "no such texture");
            p.icon_set[k] = pic;
        }
        set_class(p.icon[k], "!cpp-none", !shown || pic.empty());
    }
    // the badges: a thermometer on each other slot whose outfit has a climate (as the wheel shows it: manual, or from
    // its resistance); the highlighted outfit's thermometer and eye (custom looks) beside its label, its hotkey under
    // its name as key caps
    auto image = [&](U64 el, std::string& set, const char* want) {
        if (!el) return;
        std::string w = want ? want : "";
        if (w != set && !w.empty()) {
            int r = set_image_raw(self, el, w.c_str());
            if (r != 1) LOG("Outfit Arc: badge \"%s\": %s", w.c_str(), r ? "fault" : "no such texture");
        }
        set = w;
        set_class(el, "!cpp-none", w.empty());
    };
    auto climate_tex = [](const Outfit& o) -> const char* {
        const char* c = climate_badge(o);
        if (!c) return nullptr;
        if (!std::strcmp(c, "om-cold-c")) return "cd_icon_temperature_freeze_00";
        if (!std::strcmp(c, "om-cold2-c")) return "cd_icon_temperature_freeze_phase_00";
        if (!std::strcmp(c, "om-heat-c")) return "cd_icon_temperature_burn_00";
        if (!std::strcmp(c, "om-heat2-c")) return "cd_icon_temperature_burn_phase_00";
        return nullptr;
    };
    for (int k = 0; k < 5; ++k) {
        int off = k - 2;
        bool shown = n && (off >= -int((n - 1) / 2) && off <= int(n / 2)) && k != 2;
        int ek = shown ? p.entries[(p.sel + n + size_t(off % int(n) + int(n))) % n] : -1;
        image(p.badge[k], p.badge_set[k], ek >= 0 && ek < int(g_outfits.size()) ? climate_tex(g_outfits[size_t(ek)]) : nullptr);
    }
    int e = n ? p.entries[p.sel] : 0;
    {
        const Outfit* o = e >= 0 && e < int(g_outfits.size()) ? &g_outfits[size_t(e)] : nullptr;
        const char* clim = o ? climate_tex(*o) : nullptr;
        bool looks = false;
        if (o)
            for (U16 s = 0; s < 32; ++s) looks |= slot_look(*o, s) != kLookDefault;
        // (nb-1 is next to the label, nb-0 before it: one badge takes nb-1)
        image(p.nb[1], p.nb_set[1], looks ? "outfitmanager_badge_eye" : clim);
        image(p.nb[0], p.nb_set[0], looks ? clim : nullptr);
        int count = (clim ? 1 : 0) + (looks ? 1 : 0);
        set_class(p.label, "om-peek-label-s1", count == 1);
        set_class(p.label, "om-peek-label-s2", count == 2);
        bool caps = o && o->hk_vk && show_caps(self, p.wrap, o->hk_vk, o->hk_mods, 0);
        if (!caps)
            if (U64 box = find_by_class(p.wrap, "om-hk-caps", 1)) set_class(box, "!cpp-none", true);
    }
    std::string label = e < 0 ? lang::t("peek.unsaved_label") : lang::f("peek.label", {{"n", std::to_string(e + 1)}});
    std::string name = e < 0 ? lang::t("peek.unsaved") : e < int(g_outfits.size()) ? g_outfits[size_t(e)].name : "";
    if (g_set_text) g_set_text(p.label, label.c_str(), 0), g_set_text(p.name, name.c_str(), 0);
}

// The animations (tools/peek_gen.py): the HUD key guide's unfold. Opening: every part has its own keyframes
// (om-peek-in-arc / -frame-k / -icon-k / -label / -name: the arc unfolds, the slots ride along and pop, the texts fade
// in, the middle frame pulses); closing: the whole peek fades with a slight shrink (om-peek-out-wrap). One set at a
// time; none between, so each plays again from its start.
enum class PeekAnim { None, In, Out };
constexpr U64 kPeekOutMs = 270; // the closing fade (peek_gen.py OUT_TOTAL, 0.25 s) and a little more

void peek_animate(PeekAnim a)
{
    Peek& p = g_peek;
    bool in = a == PeekAnim::In;
    set_class(p.arc, "om-peek-in-arc", in);
    for (int k = 0; k < 5; ++k) {
        set_class(p.frame[k], ("om-peek-in-frame-" + std::to_string(k)).c_str(), in);
        set_class(p.icon[k], ("om-peek-in-icon-" + std::to_string(k)).c_str(), in);
    }
    set_class(p.label, "om-peek-in-label", in);
    set_class(p.name, "om-peek-in-name", in);
    for (int k = 0; k < 5; ++k) // (each slot's badge rides with its frame)
        if (p.badge[k]) set_class(p.badge[k], ("om-peek-in-badge-" + std::to_string(k)).c_str(), in);
    for (U64 b : {p.nb[0], p.nb[1]})
        if (b) set_class(b, "om-peek-in-name", in); // (with the texts)
    if (U64 box = find_by_class(p.wrap, "om-hk-caps", 1)) set_class(box, "om-peek-in-name", in);
    set_class(p.wrap, "om-peek-out-wrap", a == PeekAnim::Out);
}

void peek_open(U64 self)
{
    Peek& p = g_peek;
    p.self = self;
    prepare_outfits(); // (as the wheel does when it opens)
    p.entries.clear();
    p.worn = find_worn_outfit(self);
    p.current_icon.clear();
    if (p.worn < 0) { // what is worn isn't a saved outfit: the "current outfit" entry, first
        WornRaw w[kWornMax];
        int n = read_worn(self, w, kWornMax);
        for (int i = 0; i < n; ++i)
            if (w[i].slot == kChestSlot && w[i].icon) p.current_icon = w[i].icon;
        p.entries.push_back(-1);
    }
    for (size_t i = 0; i < g_outfits.size(); ++i) p.entries.push_back(int(i));
    p.sel = p.worn < 0 ? 0 : size_t(p.worn);
    p.open = true;
    p.character = g_character;
    set_class(p.trk, "!cpp-none", false); // (the HP tracker: shown while the peek is, its own gauge hidden)
    set_class(p.hp_gauge, "!cpp-none", true);
    peek_animate(PeekAnim::None); // (a close still playing: cut short, the opening starts over)
    p.closing_at = 0;
    peek_draw(self);
    peek_animate(PeekAnim::In);
    set_class(p.wrap, "!cpp-none", false);
    LOG("Outfit Arc: open (character %d, %zu outfit(s), worn %d, %zu entries)", int(g_character), g_outfits.size(), p.worn,
        p.entries.size());
}
extern std::atomic<U32> g_wheel_notches;

void peek_close(bool choose)
{
    Peek& p = g_peek;
    int e = p.entries.empty() ? -1 : p.entries[p.sel];
    peek_animate(PeekAnim::None);
    peek_animate(PeekAnim::Out); // (hidden by peek_frame once it has played)
    p.closing_at = GetTickCount64();
    p.open = false;
    bool load = choose && e >= 0 && e != p.worn && static_cast<size_t>(e) < g_outfits.size();
    LOG("Outfit Arc: closed on %s%s (%s, wheel notches %u)", e < 0 ? "the current outfit" : ("outfit " + std::to_string(e + 1)).c_str(),
        !choose ? "" : load ? "; putting it on" : " (nothing to change)", g_peek_by_pad ? "pad" : "key",
        g_wheel_notches.load());
    // as a click on it in the wheel: where each piece is, the questions (pieces on other characters, copies), the load
    if (load) start_loading(p.self, static_cast<U32>(e));
}

// The outfits' hotkeys, in play (not in menus, prompts or with the arc open): a press of exactly the combination loads
// that outfit as a click in the wheel would. The character's own outfits are the ones in use (taken over when it
// changes). The filters (name_wndproc, raw_input_hook) keep the key from the game while its modifiers match.
void peek_hotkeys(U64 self, bool free)
{
    static U16 shown_char = 0xFFFF;
    U16 ch = current_character();
    if (free && ch != kNoCharacter && ch != shown_char) { // (another character: its own outfits and hotkeys)
        shown_char = ch;
        if (ch != g_character) prepare_outfits();
    }
    static bool was[16] = {};
    std::uint8_t mods = mods_now();
    for (size_t i = 0; i < 16; ++i) {
        const Outfit* o = i < g_outfits.size() ? &g_outfits[i] : nullptr;
        U32 packed = o && o->hk_vk ? o->hk_vk | U32(o->hk_mods) << 8 : 0;
        if (g_hotkeys[i].load() != packed) g_hotkeys[i] = packed;
    }
    for (size_t i = 0; i < 16; ++i) {
        const Outfit* o = i < g_outfits.size() ? &g_outfits[i] : nullptr;
        U32 packed = o && o->hk_vk ? o->hk_vk | U32(o->hk_mods) << 8 : 0;
        // exactly its modifiers; a bare key also with others held when no outfit has that exact combination (so "X"
        // still works while Shift sprints, unless "Shift + X" is another outfit's)
        bool exact_taken = false;
        if (packed && !o->hk_mods && mods)
            for (size_t j = 0; j < g_outfits.size(); ++j)
                exact_taken |= g_outfits[j].hk_vk == o->hk_vk && g_outfits[j].hk_mods == mods;
        bool mods_ok = o && (mods == o->hk_mods || (!o->hk_mods && !exact_taken));
        bool down = packed && (GetAsyncKeyState(o->hk_vk) & 0x8000) && mods_ok;
        bool press = down && !was[i];
        was[i] = down;
        if (!press || !free) continue;
        U16 vk = o->hk_vk;
        std::uint8_t m = o->hk_mods;
        prepare_outfits(); // (as the wheel and the arc do first; it may rebuild the list: found again by the combination)
        int idx = -1;
        for (size_t j = 0; j < g_outfits.size(); ++j)
            if (g_outfits[j].hk_vk == vk && g_outfits[j].hk_mods == m) idx = int(j);
        if (idx < 0) continue;
        int worn = find_worn_outfit(self);
        LOG("hotkey %s: \"%s\"%s", hotkey_name(vk, m).c_str(), g_outfits[size_t(idx)].name.c_str(),
            idx == worn ? " (already worn)" : "; putting it on");
        if (idx != worn) start_loading(self, U32(idx));
        break; // (one load per frame)
    }
}
void wheel_hook(bool on);

// The arc opened or closed: the input filters follow (g_peek_live), the wheel hook goes on / off.

void peek_sync_live(bool open)
{
    if (open == g_peek_live.load()) return;
    if (open) g_wheel_notches = 0;
    g_peek_live = open;
    wheel_hook(open);
}

void peek_frame(U64 self)
{
    U64 root = rp(self + 8);
    if (root != g_peek_root) { // (first frame, or the screen was made again: a load)
        g_peek_root = root, g_peek = Peek{};
        if (!root || !is_element(root) || !find_by_class(root, "om-peek-wrap", 4)) {
            if (root) LOG("Outfit Arc: the status screen %llX has no peek (its UI file wasn't patched)", root);
            g_peek_root = ~0ull; // (said once; looked at again only if the screen changes)
            return;
        }
        LOG("Outfit Arc: found in the status screen %llX", root);
    }
    if (g_peek_root == ~0ull || !peek_elements()) return;
    Peek& p = g_peek;
    g_peek_hud_at = GetTickCount64();
    // The key (the "+" page's Quick Peek): held = open, released = the highlighted outfit goes on. Not while the gear
    // wheel, a question or the naming prompt is open.
    if (g_peek_capture && !g_menu_open) g_peek_capture = false; // (the wheel closed while the setting waited)
    watch_game_chars(); // (the window watch: the wheel's messages while the arc is open)
    U32 vk = g_peek_key.load();
    bool down = vk && !g_peek_capture && game_focused() && (GetAsyncKeyState(int(vk)) & 0x8000) != 0;
    bool free = !g_menu_open && !g_ask_modal && !g_name_modal;
    // The pad: hold L1 / LB and press the d-pad's down (2026-11-05: down does nothing while L1 is held outside combat;
    // left raises the lantern, up calls the horse, right the items); up / down then move, letting go of L1 wears it.
    U8 pad = g_peek_pad && game_focused() ? pad_buttons_raw() : 0, pressed = pad & ~g_peek_pad_last;
    g_peek_pad_last = pad;
    if (down && !g_peek_down && !p.open && free) {
        g_peek_wheel = 0;
        g_peek_by_pad = false;
        peek_open(self);
    } else if ((pad & kPadL1) && (pressed & kPadDown) && !p.open && free) {
        g_peek_wheel = 0;
        g_peek_by_pad = true;
        peek_open(self);
        pressed = 0; // (the opening press doesn't also move)
    } else if (p.open && (g_peek_by_pad ? !(pad & kPadL1) : !down)) {
        peek_close(true);
    }
    if (p.open && g_peek_by_pad && (pressed & (kPadUp | kPadDown)))
        g_peek_wheel += (pressed & kPadUp) ? WHEEL_DELTA : -WHEEL_DELTA; // (as a wheel notch: up = up the arc)
    g_peek_down = down;
    g_peek_pad_open = p.open && g_peek_by_pad;
    peek_hotkeys(self, free && !p.open && game_focused());
    { // a PlayStation pad's reader waits for a pad from the first frame on (not only once the gear wheel opened)
        static bool woke = false;
        if (!woke && g_peek_pad && g_hid_wake) woke = true, SetEvent(g_hid_wake);
    }
    peek_sync_live(p.open);
    if (g_ask_next && !g_menu_open && g_ask_self == self) pump_ask(self); // (a question asked from the peek)
    // closed (nothing chosen) when the player switches character (its outfits would be the last one's) or opens the
    // gear wheel (2026-11-05: switching with the peek open kept showing Kliff's outfits on Damiane and Oongka)
    if (p.open) {
        U16 ch = current_character();
        if (g_menu_open || (ch != kNoCharacter && ch != p.character)) {
            LOG("Outfit Arc: closed by %s", g_menu_open ? "the gear wheel opening" : "a character switch");
            peek_close(false);
        }
    }
    if (p.open && !p.entries.empty()) { // the wheel: a notch is one outfit (towards you = down the arc)
        int w = g_peek_wheel.load(), steps = w / WHEEL_DELTA;
        if (steps) {
            g_peek_wheel -= steps * WHEEL_DELTA;
            long long n = static_cast<long long>(p.entries.size());
            long long m = (static_cast<long long>(p.sel) - steps) % n;
            p.sel = static_cast<size_t>(m < 0 ? m + n : m);
            peek_draw(self);
        }
    }
    peek_sync_live(p.open);
    if (p.closing_at && GetTickCount64() - p.closing_at >= kPeekOutMs) { // the closing animation has played
        set_class(p.wrap, "!cpp-none", true);
        peek_animate(PeekAnim::None);
        p.closing_at = 0;
        set_class(p.trk, "!cpp-none", true); // (the HP tracker hidden again, as the game keeps it)
    }
}

float __fastcall gauge_frame_hook(U64 self)
{
    // The UI thread is known from here too, not only once the wheel has opened: a load's later parts (the done check,
    // the stash, the looks held until the model swaps) run on the HUD frame only on that thread (2026-11-05: a load
    // from the quick peek stayed half-shown until the wheel opened).
    if (!g_ui_thread) {
        g_ui_thread = GetCurrentThreadId();
        LOG("ui thread %lu (from the status screen)", g_ui_thread);
    }
    peek_frame(self);
    return g_gauge_frame_orig(self);
}

// The game reads the keyboard and mouse through raw input (GetRawInputData). The peek's key never reaches it while the
// HUD shows the peek (so a key the game also uses, e.g. Tab's Skill 11, does only the peek), and while the peek is open
// the wheel goes to the peek instead of the camera's distance. Nothing of the game's own code is needed for this.
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
GetRawInputDataFn g_raw_input_orig = nullptr;

UINT WINAPI raw_input_hook(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT header)
{
    UINT r = g_raw_input_orig(h, cmd, data, size, header);
    if (cmd != RID_INPUT || !data || r == UINT(-1) || r < sizeof(RAWINPUTHEADER)) return r;
    __try {
        RAWINPUT* in = static_cast<RAWINPUT*>(data);
        U32 vk = g_peek_key.load();
        USHORT mouse = peek_mouse_flags(vk);
        bool hud = GetTickCount64() - g_peek_hud_at.load() < 250 && !g_menu_open && !g_name_modal && !g_ask_modal;
        if (in->header.dwType == RIM_TYPEMOUSE) {
            RAWMOUSE& m = in->data.mouse;
            if (g_peek_live && (m.usButtonFlags & RI_MOUSE_WHEEL)) {
                // (not counted here: the game reads each raw event several times; the hook / window route counts)
                m.usButtonFlags &= ~RI_MOUSE_WHEEL;
                m.usButtonData = 0;
            }
            if (hud && mouse) m.usButtonFlags &= ~mouse;
            for (U32 b : {U32(VK_MBUTTON), U32(VK_XBUTTON1), U32(VK_XBUTTON2)}) // (a hotkey's mouse button)
                if ((m.usButtonFlags & peek_mouse_flags(b)) && hotkey_held_now(b)) m.usButtonFlags &= ~peek_mouse_flags(b);
        } else if (in->header.dwType == RIM_TYPEKEYBOARD) {
            RAWKEYBOARD& k = in->data.keyboard;
            if (hud && vk && !mouse && k.VKey == vk) k.VKey = 0xFF, k.MakeCode = 0, k.Flags &= ~(RI_KEY_E0 | RI_KEY_E1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

// While the arc is open, a low-level mouse hook takes the wheel before anything else sees it (2026-11-06: after using a
// pad, the game in its pad mode passed no wheel to its window, so the window / raw input filters saw none and the arc
// didn't move). Installed when the arc opens, removed when it closes (its own thread: such a hook needs a message loop).
std::atomic<DWORD> g_wheel_thread_id{0};
std::atomic<U32> g_wheel_notches{0}; // notches the hook took this opening (on the close's log line)
HANDLE g_wheel_open_event = nullptr;

LRESULT CALLBACK wheel_ll_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL && g_peek_live && game_focused()) {
        const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        g_peek_wheel += static_cast<SHORT>(HIWORD(m->mouseData));
        ++g_wheel_notches;
        return 1; // (kept from the game: no window message, no raw input)
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI wheel_thread(LPVOID)
{
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE); // (the thread's queue: the close message can be posted to it)
    g_wheel_thread_id = GetCurrentThreadId();
    for (;;) {
        WaitForSingleObject(g_wheel_open_event, INFINITE);
        HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, &wheel_ll_proc, g_self, 0);
        if (!h) LOG("Outfit Arc: wheel hook failed (%lu); the window's wheel messages count instead", GetLastError());
        g_wheel_hooked = h != nullptr;
        // (a close's message can be left over from an opening the hook never saw: it only counts with the arc closed,
        // 2026-11-06: a stale one ended the next opening's hook at once and the wheel did nothing for it)
        while (g_peek_live && GetMessageW(&msg, nullptr, 0, 0) > 0)
            if (msg.message == WM_APP && !g_peek_live) break; // (the arc closed)
        if (h) UnhookWindowsHookEx(h);
        g_wheel_hooked = false;
        while (PeekMessageW(&msg, nullptr, WM_APP, WM_APP, PM_REMOVE)) {} // (none left for the next opening)
    }
}

// The arc opened / closed: the wheel hook goes on / off.
void wheel_hook(bool on)
{
    if (on) {
        if (g_wheel_open_event) SetEvent(g_wheel_open_event);
    } else if (DWORD t = g_wheel_thread_id.load()) {
        PostThreadMessageW(t, WM_APP, 0, 0);
    }
}

void install_raw_input()
{
    g_wheel_open_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, &wheel_thread, nullptr, 0, nullptr)) CloseHandle(t);
    void* target = nullptr;
    if (MH_CreateHookApiEx(L"user32", "GetRawInputData", reinterpret_cast<void*>(&raw_input_hook),
                           reinterpret_cast<void**>(&g_raw_input_orig), &target) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        LOG("Outfit Arc: raw input hook FAILED (the key and the wheel also reach the game)");
        return;
    }
    LOG("Outfit Arc: raw input hook on (key %s)", peek_key_name(g_peek_key.load()).c_str());
}

void install_peek()
{
    void** vt = game::find_vtable(".?AVUIGamePlayControlRootStatusGauge@uiCommonScript@pa@@");
    if (!vt || vt[kSlotFrame] != reinterpret_cast<void*>(g_base + kRvaRootFrame)) { // (the shared root frame function)
        LOG("Outfit Arc: the status screen's frame call not found; no quick peek");
        return;
    }
    g_gauge_frame_orig = reinterpret_cast<FrameFn>(vt[kSlotFrame]); // (before the patch: the game may call it at once)
    void* prev = game::patch_vtable(vt, kSlotFrame, reinterpret_cast<void*>(&gauge_frame_hook));
    LOG("Outfit Arc: status screen frame hook %s (vtable +%llX)", prev ? "on" : "FAILED", reinterpret_cast<U64>(vt) - g_base);
}

DWORD WINAPI init_thread(LPVOID)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(g_self, buf, MAX_PATH);
    std::wstring path = buf;
    path.resize(path.find_last_of(L"/\\") + 1);
    g_log_path = path + L"OutfitManager.log";
    logx::open(g_log_path.c_str());
    {
        char ver[32] = "unknown"; // the version resource (outfit_manager.rc)
        DWORD h = 0, n = GetFileVersionInfoSizeW(buf, &h);
        std::vector<char> vi(n);
        VS_FIXEDFILEINFO* f = nullptr;
        UINT len = 0;
        if (n && GetFileVersionInfoW(buf, 0, n, vi.data()) &&
            VerQueryValueW(vi.data(), L"\\", reinterpret_cast<void**>(&f), &len) && f)
            std::snprintf(ver, sizeof ver, "%u.%u.%u", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS),
                          HIWORD(f->dwFileVersionLS));
        LOG("Outfit Manager %s started", ver);
    }
    {
        bool at_load = g_ui_parse_orig != nullptr; // (put in at load; else now, which may be too late for some files)
        bool ok = install_ui_patch();
        LOG("gear wheel UI hook: %s%s", ok ? (at_load ? "in at load, " : "in (late), ") : "NOT in: ", g_ui_hook_note.c_str());
    }
    g_store_path = path + L"OutfitManager.outfits";
    g_cache_path = path + L"OutfitManager.cache"; // game addresses for this exe (rebuilt when the exe changes)
    load_outfits();
    {
        std::string how;
        g_lang_dir = path + L"Outfit Manager\\lang\\";
        g_detected_language = lang::detect(path, how);
        LOG("game language: %s (from %s)%s", g_detected_language.c_str(), how.c_str(),
            g_language.empty() ? "" : (", setting: " + g_language).c_str());
        lang::load(g_lang_dir, g_language.empty() ? g_detected_language : g_language);
    }
    if (!install()) {
        LOG("setup failed; mod inactive");
        return 0;
    }
    install_peek();
    install_raw_input();
    install_pad_filters();
    if (HANDLE t = CreateThread(nullptr, 0, key_thread, nullptr, 0, nullptr)) CloseHandle(t);
    g_hid_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr); // (auto-reset: one look per menu open without a pad)
    if (g_hid_wake)
        if (HANDLE t = CreateThread(nullptr, 0, hid_thread, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
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
