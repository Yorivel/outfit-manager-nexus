#include "game.h"

#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

namespace {

struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<bool> mask; // true = must match
};

std::optional<Pattern> parse(const char* text)
{
    Pattern p;
    for (const char* c = text; *c;) {
        if (*c == ' ') {
            ++c;
            continue;
        }
        if (c[0] == '?' && c[1] == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(false);
            c += 2;
            continue;
        }
        char hex[3] = {c[0], c[1], 0};
        char* end = nullptr;
        unsigned long v = std::strtoul(hex, &end, 16);
        if (end != hex + 2)
            return std::nullopt;
        p.bytes.push_back(static_cast<std::uint8_t>(v));
        p.mask.push_back(true);
        c += 2;
    }
    if (p.bytes.empty())
        return std::nullopt;
    return p;
}

bool match_at(const std::uint8_t* at, const Pattern& p)
{
    for (std::size_t i = 0; i < p.bytes.size(); ++i)
        if (p.mask[i] && at[i] != p.bytes[i])
            return false;
    return true;
}

}

game::Module game::main_module()
{
    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    return {base, nt->OptionalHeader.SizeOfImage};
}

std::uintptr_t game::find_unique(const char* pattern)
{
    auto p = parse(pattern);
    if (!p)
        return 0;
    auto mod = main_module();
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(mod.base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);

    std::uintptr_t found = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        auto begin = reinterpret_cast<const std::uint8_t*>(mod.base + sec->VirtualAddress);
        std::size_t len = sec->Misc.VirtualSize;
        if (len < p->bytes.size())
            continue;
        for (std::size_t off = 0; off + p->bytes.size() <= len; ++off) {
            if (!match_at(begin + off, *p))
                continue;
            if (found)
                return 0; // not unique
            found = reinterpret_cast<std::uintptr_t>(begin + off);
        }
    }
    return found;
}

namespace {

// Calls fn(address) for every occurrence of needle in the image's data sections
// (RTTI names, type descriptors and vtables all live outside the code sections).
template <class Fn>
void for_each_in_image(const void* needle, std::size_t len, Fn fn)
{
    auto mod = game::main_module();
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod.base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(mod.base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ) || (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        auto begin = reinterpret_cast<const std::uint8_t*>(mod.base + sec->VirtualAddress);
        std::size_t size = sec->Misc.VirtualSize;
        for (std::size_t off = 0; off + len <= size; ++off)
            if (begin[off] == *static_cast<const std::uint8_t*>(needle) && std::memcmp(begin + off, needle, len) == 0)
                if (!fn(reinterpret_cast<std::uintptr_t>(begin + off)))
                    return;
    }
}

}

void** game::find_vtable(const char* mangled_name)
{
    auto mod = main_module();
    std::size_t len = std::strlen(mangled_name) + 1;
    void** result = nullptr;
    for_each_in_image(mangled_name, len, [&](std::uintptr_t name) {
        auto td_rva = static_cast<std::uint32_t>(name - 0x10 - mod.base);
        // CompleteObjectLocator: {sig=1, offset, cdOffset, rvaTypeDescriptor, rvaClassDescriptor, rvaSelf}
        for_each_in_image(&td_rva, 4, [&](std::uintptr_t at) {
            auto col = reinterpret_cast<const std::uint32_t*>(at - 12);
            if (col[0] != 1 || col[1] != 0 || col[5] != static_cast<std::uint32_t>(at - 12 - mod.base))
                return true;
            std::uint64_t col_va = at - 12;
            for_each_in_image(&col_va, 8, [&](std::uintptr_t ref) {
                result = reinterpret_cast<void**>(ref + 8);
                return false;
            });
            return result == nullptr;
        });
        return result == nullptr;
    });
    return result;
}

void* game::patch_vtable(void** vtable, int slot, void* fn)
{
    DWORD old;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old))
        return nullptr;
    void* prev = vtable[slot];
    vtable[slot] = fn;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return prev;
}

bool game::matches(std::uintptr_t addr, const char* pattern)
{
    auto p = parse(pattern);
    return p && match_at(reinterpret_cast<const std::uint8_t*>(addr), *p);
}
