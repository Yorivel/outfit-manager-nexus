// Start-up: installing the hooks, the init thread.
#include "om.h"
#include "setup.h"
#include "addrs.h"
#include "ui.h"
#include "items.h"
#include "store.h"
#include "saves.h"
#include "looks.h"
#include "load.h"
#include "prompts.h"
#include "settings.h"
#include "wheel_draw.h"
#include "wheel_hooks.h"
#include "input.h"
#include "pads.h"
#include "arc.h"
#include "ui_hook.h"

namespace om {

void sleep_install(); // (sleep.cpp)
void sound_install(); // (sound.cpp)

std::wstring g_log_path;
bool g_features_toast_done = false;

// A stacking mod that makes equipment stackable (#1, reproduced 2026-11-07: "Desert Link Unlimited Stacks", its mode
// "All Stackable (including equipment)", sets every item's _maxStackCount - armour too): pieces merge into stacks the
// game won't equip, and a piece taken off by the client-side call never reached the bag. Loads are safe against it
// now (requests, take-off first, the shared-id rule, the loss check), but the stacks it makes are still the player's
// problem - so they are told the cause once per session. Found by its module and the mode in its own log.
bool equipment_stacking_mod()
{
    HMODULE m = GetModuleHandleW(L"DesertLinkUnlimitedStacks.asi");
    if (!m) return false;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(m, path, MAX_PATH)) return false;
    std::wstring log = path;
    log = log.substr(0, log.size() - 4) + L".log"; // (DesertLinkUnlimitedStacks.log beside it)
    FILE* f = _wfopen(log.c_str(), L"rb");
    if (!f) return true; // (no log to tell: the only mode its build knows is the one with equipment)
    std::string text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    size_t last = text.rfind("Unlimited Stacks v"); // (its latest start)
    bool with = last != std::string::npos && text.find("including equipment", last) != std::string::npos;
    LOG("stacking mod: Desert Link Unlimited Stacks loaded, %s", with ? "equipment stacks (pieces can get lost)" : "equipment not stacked");
    return with;
}

// At the first menu open: one toast naming whatever had to be turned off.
void features_toast(U64 self)
{
    static bool stacks_told = false;
    if (!stacks_told) {
        stacks_told = true;
        if (equipment_stacking_mod()) notify(self, lang::t("toast.stacking_mod"));
    }
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
    sleep_install(); // sleepwear: the sleep seen in the action charts (sleep.cpp)
    sound_install(); // hidden gear's sounds, the equip sounds of an outfit change (sound.cpp)
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
    // the outfit transition and the hidden weapons' effects (optional: without them outfits change at once)
    if (!(have({kRvaMatUpdate, kRvaMatSend, kRvaMatChange, kRvaPermSet, kRvaMsgInit, kRvaMsgFloatVtable, kRvaMsgUintVtable,
                kRvaMsgBaseVtable, kRvaPermKey, kRvaPermValue, kRvaPermId, kRvaNameId, kRvaMeshCollect,
                kRvaMeshCollectNamed, kRvaMeshColour, kRvaMeshColourArg}) &&
          install_transition()))
        feature_off("feature.effects");
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
