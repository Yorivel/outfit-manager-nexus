#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Outfit Manager's additions to the gear wheel's compiled UI (ui/bin__/*.uibinary), made in memory while the game loads
// the files, onto whatever copy it loads (the game's own, or another mod's). The C++ form of tools/uibin_tab.py:
//   header: a copy of QuickSlotHeader8's element right after it, as QuickSlotHeaderOutfits (hidden by its "!cpp-none"
//           until the plugin shows it); the tab bar's child count goes up by one
//   value:  in every CoolTimeChargedCountWrap, a copy of its CoolTimeChargedCount text as OmValueText (classes
//           om-value-text, font-text-caption-yellow); the wrap's child count goes up by one
namespace uibin {

struct Report {
    int headers = 0, values = 0;
    std::string error; // why nothing was changed
};

// What a file gets, by its name (any case, any folder): 0 = not ours; bit 1 = header, bit 2 = value text.
unsigned wants(const std::string& path);

// out = in with the additions `what` asks for; false (out empty, r.error set) when the bytes aren't laid out as
// expected, or already have them (another copy of this mod's file).
bool patch(const std::vector<std::uint8_t>& in, unsigned what, std::vector<std::uint8_t>& out, Report& r);

}
