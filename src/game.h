#pragma once

#include <cstdint>
#include <span>

// Access to CrimsonDesert.exe: module range and byte-pattern search.
namespace game {

struct Module {
    std::uintptr_t base = 0;
    std::size_t size = 0;
};

Module main_module();

// Pattern like "40 53 48 83 EC 20 ?? ?? 48 8B" ("??" = any byte).
// Returns 0 if the pattern matches zero or several places.
std::uintptr_t find_unique(const char* pattern);

// True if the bytes at addr match the pattern (used to confirm known addresses).
bool matches(std::uintptr_t addr, const char* pattern);

// Vtable of a class by its MSVC RTTI name (".?AVName@ns@@"), or nullptr.
void** find_vtable(const char* mangled_name);

// Replaces one vtable slot; returns the previous function, or nullptr on failure.
void* patch_vtable(void** vtable, int slot, void* fn);

}
