// Offline test of the resolver against game exes on disk (no game needed):
//   ResolverTest.exe <cache dir> <exe>...
// Per exe: (1) search only, (2) normal with an empty cache (writes it), (3) normal again (must be all cached).
// Prints one line per address and run; details go to <cache dir>\ResolverTest.log.

#include "log.h"
#include "resolver.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>

using U64 = std::uint64_t;
#define OM_SIGS_NO_VARS
#include "om_sigs.h"

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        std::fwprintf(stderr, L"usage: ResolverTest <cache dir> <exe>...\n");
        return 2;
    }
    std::wstring dir = argv[1];
    logx::open((dir + L"\\ResolverTest.log").c_str());
    constexpr std::size_t n = std::size(kRecipes);
    int bad = 0;
    // --swap: swap RootQuickSlot vtable slots 19 and 20 (known build only) = a patch that inserted a virtual, so the
    // resolver must find kRvaRootClose in slot 20.
    bool swap = std::wstring(argv[2]) == L"--swap";
    for (int a = swap ? 3 : 2; a < argc; ++a) {
        HMODULE h = LoadLibraryExW(argv[a], nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!h) {
            std::fwprintf(stderr, L"cannot map %ls (%lu)\n", argv[a], GetLastError());
            ++bad;
            continue;
        }
        U64 base = reinterpret_cast<U64>(h) & ~U64(3);
        if (swap) {
            auto vt = reinterpret_cast<U64*>(base + 0x56e1968);
            DWORD old;
            if (!VirtualProtect(vt + 19, 16, PAGE_READWRITE, &old)) {
                std::fwprintf(stderr, L"cannot unprotect the vtable (%lu)\n", GetLastError());
                return 2;
            }
            std::swap(vt[19], vt[20]);
        }
        resolver::set_test_image(base);
        std::wstring cache = dir + L"\\test_" + std::to_wstring(a) + L".cache";
        DeleteFileW(cache.c_str());
        resolver::Result search[n], first[n], second[n];
        LOG("==== %ls: search only", argv[a]);
        std::size_t s = resolver::resolve(kRecipes, n, search, nullptr, true);
        LOG("==== %ls: normal, empty cache", argv[a]);
        std::size_t f = resolver::resolve(kRecipes, n, first, cache.c_str(), false);
        LOG("==== %ls: normal, cache from the run before", argv[a]);
        std::size_t c = resolver::resolve(kRecipes, n, second, cache.c_str(), false);
        std::wprintf(L"== %ls: search %zu/%zu, normal %zu, cached run %zu\n", argv[a], s, n, f, c);
        for (std::size_t i = 0; i < n; ++i) {
            bool same = search[i].rva == first[i].rva && first[i].rva == second[i].rva;
            bool all_cached = second[i].how == resolver::How::Cached || second[i].how == resolver::How::Missing;
            if (!same || !all_cached) ++bad;
            bool slot_same = search[i].slot == first[i].slot && first[i].slot == second[i].slot;
            if (!slot_same) ++bad;
            std::printf("  %-26s %-8s %-8s %-8s +%llX%s%s%s\n", kRecipes[i].name, resolver::how_name(search[i].how),
                        resolver::how_name(first[i].how), resolver::how_name(second[i].how), first[i].rva,
                        same ? "" : "  <-- RUNS DISAGREE", all_cached ? "" : "  <-- NOT FROM CACHE",
                        slot_same ? (first[i].slot >= 0 && first[i].slot != kRecipes[i].slot ? "  (slot moved)" : "")
                                  : "  <-- SLOTS DISAGREE");
            if (first[i].slot >= 0 && first[i].slot != kRecipes[i].slot)
                std::printf("      slot %d -> %d\n", kRecipes[i].slot, first[i].slot);
        }
        constexpr std::size_t no = std::size(kOffsets);
        std::uint64_t vals[no];
        std::size_t moved = resolver::resolve_offsets(kOffsets, no, first, vals);
        std::printf("  offsets: %zu moved\n", moved);
        for (std::size_t i = 0; i < no; ++i)
            std::printf("  %-26s +%X -> +%llX%s\n", kOffsets[i].name, kOffsets[i].known, vals[i],
                        vals[i] != kOffsets[i].known ? "  MOVED" : "");
        FreeLibrary(h);
    }
    std::printf("%s (%d problem(s))\n", bad ? "FAIL" : "PASS", bad);
    return bad ? 1 : 0;
}
