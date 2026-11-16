// Outfit Manager's shared types, constants and state.
#include "om.h"

namespace om {

U64 g_base = 0;
HMODULE g_self = nullptr;
std::uint8_t look_pos(std::uint8_t look)
{
    for (std::uint8_t k = 0; k < kLookCount; ++k)
        if (kLookOrder[k] == look) return k;
    return 0;
}
std::vector<Outfit> g_outfits; // the current character's outfits (what the wheel shows)
std::wstring g_store_path;
U16 g_armor_slots[kOutfitSlots]; // equipment-N category per Armor wheel slot (template record order)
U32 g_armor_slot_count = 0;
U16 g_weapon_slots[kMaxWeaponSlots]; // the Weapons wheel's (record 0); 0xFFFF = a wheel slot with no equip slot
std::atomic<U32> g_weapon_slot_count{0};

bool is_weapon_slot(U16 slot)
{
    for (U32 i = 0; i < g_weapon_slot_count; ++i)
        if (g_weapon_slots[i] == slot) return true;
    return false;
}
// Runs `pre` before every direct call (E8 rel32) of `target` in the game's executable sections: the calls go to a stub
// near the game that saves the four argument registers, calls pre(rcx, rdx, r8, r9), puts them back and JUMPS to
// `target` - so the game's own return address is still on the stack and the function returns straight to the game.
// The target's bytes stay as they are, so another mod that finds it by its first bytes still finds and hooks it, and
// sees the game as its caller (Combat Evolved's presentation rebuild: 0 matches under our entry hook, then no
// rebuilds counted with us as the caller - its jetpack never saw the rocket pack, 2026-11-16). The calls are found
// by scanning, so a game update that moves them is followed. Returns how many calls now go through `pre`; 0: none
// found, nothing changed. (pre gets only the register arguments; stack ones stay where the target expects them.)
namespace {
// (the direct calls of `target` in [p, p + n) into out[]; -1 past `cap`. No C++ objects here: it reads under __try)
int find_calls(std::uint8_t* p, U64 n, U64 target, std::uint8_t** out, int have, int cap)
{
    __try {
        for (U64 i = 0; i + 5 <= n; ++i) {
            if (p[i] != 0xE8) continue;
            std::int32_t rel;
            std::memcpy(&rel, p + i + 1, 4);
            if (reinterpret_cast<U64>(p + i + 5) + static_cast<std::int64_t>(rel) != target) continue;
            if (have >= cap) return -1;
            out[have++] = p + i;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return have;
}
} // namespace

int redirect_calls(U64 target, void* pre)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    std::uint8_t* found[64];
    int count = 0;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections && count >= 0; ++s)
        if (sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)
            count = find_calls(reinterpret_cast<std::uint8_t*>(g_base + sec[s].VirtualAddress), sec[s].Misc.VirtualSize,
                               target, found, count, 64);
    if (count <= 0) return 0; // (none, or more than a function's calls should be: left alone)
    std::vector<std::uint8_t*> sites(found, found + count);
    std::uint8_t* stub = nullptr; // (within reach of a rel32 from every site: below the game's image)
    for (U64 a = (g_base - 0x10000) & ~0xFFFFull; !stub && a > 0x10000 && g_base - a < 0x7FF00000; a -= 0x10000)
        stub = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_RESERVE | MEM_COMMIT,
                                                       PAGE_EXECUTE_READWRITE));
    if (!stub) return 0;
    // On entry rsp is 8 past a 16-byte boundary (the call pushed the return address). Four pushes and 0x28 more leave
    // it on a boundary with the 32-byte shadow space for pre; then everything back and on to the target.
    const U64 pre_at = reinterpret_cast<U64>(pre);
    std::uint8_t code[] = {
        0x51,                         // push rcx
        0x52,                         // push rdx
        0x41, 0x50,                   // push r8
        0x41, 0x51,                   // push r9
        0x48, 0x83, 0xEC, 0x28,       // sub rsp, 0x28
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, pre
        0xFF, 0xD0,                   // call rax   (rcx, rdx, r8, r9 still hold the call's arguments)
        0x48, 0x83, 0xC4, 0x28,       // add rsp, 0x28
        0x41, 0x59,                   // pop r9
        0x41, 0x58,                   // pop r8
        0x5A,                         // pop rdx
        0x59,                         // pop rcx
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, // mov rax, target
        0xFF, 0xE0,                   // jmp rax    (the game's return address still on the stack)
    };
    std::memcpy(code + 12, &pre_at, 8);
    std::memcpy(code + 34, &target, 8);
    std::memcpy(stub, code, sizeof code);
    DWORD old = 0;
    VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old);
    int done = 0;
    for (std::uint8_t* site : sites) {
        const std::int64_t rel = reinterpret_cast<std::int64_t>(stub) - reinterpret_cast<std::int64_t>(site + 5);
        if (rel < INT32_MIN || rel > INT32_MAX) continue;
        const std::int32_t r = static_cast<std::int32_t>(rel);
        if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        std::memcpy(site + 1, &r, 4);
        VirtualProtect(site + 1, 4, old, &old);
        ++done;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return done;
}
bool uses_weapons(const Outfit& o) { return o.has_weapons && (o.load_weapons || o.weapons_only); }
bool is_weapon_set(const Outfit& o) { return o.has_weapons && o.weapons_only; }
// The outfit's pieces loading puts on (the weapon ones only when it loads weapons).
bool loads_piece(const Outfit& o, U16 slot)
{
    if (is_weapon_set(o)) return is_weapon_slot(slot); // (a weapon set: its weapons only, the armour left alone, #27)
    return uses_weapons(o) || !is_weapon_slot(slot);
}
Store g_store;
U16 g_character = kNoCharacter;

std::atomic<U32> g_outfit_count{0}; // == g_outfits.size()

} // namespace om
