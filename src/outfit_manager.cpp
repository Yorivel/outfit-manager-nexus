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
#include "ui_hook.h"
#include "setup.h"

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
