// Game addresses, struct offsets and game functions (found again by the resolver after a game update).
#include "om.h"
#include "addrs.h"
#include "om_rest.h"

namespace om {

// --- Game addresses (CrimsonDesert 1.0.0.2976) ---------------------------------------------------
U64 kRvaRootQuickSlotVt = 0x5a74e70;
U64 kRvaQuickSlotInfoGet = 0x120d640;  // record* (unused, u32 key)
U64 kRvaStringInfoGet = 0x3dd7e0;      // stringinfo* (unused, u32 id); +0x10 -> char**
U64 kRvaQuickSlotInfoMgr = 0x71d3790;  // ptr; +8 count, +0x58 record pointers
U64 kRvaClassNames = 0x70d2d20;        // ptr; +0x58 -> {char* name, ...}[id] (stride 0x10)
U64 kRvaBuildBodies = 0x120a290;       // RootQuickSlot: one tab body per QuickSlotInfo record
U64 kRvaGearMode = 0x120ac10;          // RootQuickSlot: show Weapons/Armor headers, select tab 0
U64 kRvaSetText = 0x41d39a0;           // (element, const char* utf8, flag)
U64 kRvaQsiTableRoot0 = 0x71b6fe8; // ***root +8 -> +0x460: table the builder counts
U64 kRvaQsiTableRoot1 = 0x71b6ff0;
U64 kRvaIntern = 0x149de20;         // (u64* out id, const char* class, 1, 0x2ffff)
U64 kRvaClassVecAdd = 0x3896a70;    // bool (vec*, const u32* id): appends if missing
U64 kRvaClassVecRemove = 0x3896b10; // u32 index or -1 (vec*, const u32* id)
U64 kRvaClassVecSort = 0x38974a0;   // (vec*)
U64 kRvaRestyle = 0x38933c0;        // (class holder*)

// Worn gear: actor context {vt, actor, u8 valid, ..} of the player (released like a ref-holder).
U64 kRvaPlayerHolder = 0x71b6ca8;   // ptr -> ptr -> argument of the context getter
U64 kRvaActorCtx = 0x99d660;        // (U64 holder, U64 ctx[4])
U64 kRvaCtxRelease = 0x1616930;     // (U64 ctx[4]) after ctx[0] = kRvaCtxVt
U64 kRvaCtxVt = 0x5abd778;
U64 kRvaItemIconName = 0x25fd2c0;   // const char* (item*): persistent icon texture name
U64 kRvaEmptyItem = 0x7147ed8;      // ptr -> empty-item sentinel
// Dynamic background image, as the item icons set theirs: string var {strobj*, u8 type 2}.
U64 kRvaEmptyString = 0x6e9cd20;    // empty string object (var default)
U64 kRvaStrAssign = 0x4455f0;       // (var*, const char*)
U64 kRvaStrNormalize = 0x1601f80;   // (var*)
U64 kRvaStrLower = 0x1602360;       // (var*)
U64 kRvaStrDestroy = 0x3e56e0;      // (var*)
U64 kRvaSetStyleProp = 0x7124c0;    // (class holder*, const u16* prop, var*, const u16* flag)
U64 kRvaItemTable = 0x71b7bd0;       // global -> the item table (+0x8 u32 rows, +0x28 rows: {u32 permanent id, u32 body offset}, by key)
U64 kRvaItemSelectorVt = 0x5a1bf08; // UIGamePlayControlCommon_ItemSelector (a slot's dots, Selection.CommonItemSelector)
U64 kRvaSelectorSelect = 0x100d180;  // (selector, u32 index, u8): lights dot `index` (index >= count: none; shows count)
U64 kRvaTextureExists = 0x422f7e0;  // bool (*(uiMgr + kOffUiTextures), var*)
U64 kRvaIconWitness0 = 0xe008e0;     // only offset witnesses for kOffUiTextures: the game's own "texture exists?" callers
U64 kRvaIconWitness1 = 0xe012e0;
U64 kRvaTextCaretWitness0 = 0x4181e90; // only offset witnesses for the text box fields (kOffInput*): the game's own
U64 kRvaTextCaretWitness1 = 0x4183320; // text box code (caret keys, selection, typing)
U64 kRvaTextCaretWitness2 = 0x4187e30;
U64 kRvaElemRectWitness0 = 0x49fb9f0;  // only offset witnesses for kOffElemRect: the game's element overlap test
U64 kRvaElemRectWitness1 = 0x4ab9dd0;  // and a layout setter
U64 kRvaIconScriptFlag = 0x71760b8; // u8: game uses the image-script path instead (logged only)
// Centre tooltip (RootQuickSlot+0x188, UIGamePlayControlCommonInfoDescription) show/hide state machine.
U64 kRvaSetState = 0xdeeb60;        // (script*, const u32* state, 0)
U64 kRvaStateShow = 0x7113ca8;      // u32 state ids (current state at script+0xa8)
U64 kRvaStateHide = 0x7113e94;
U64 kRvaFocusedSlot = 0x120cbc0;    // slot script* (RootQuickSlot*, u8 *(self+0x28d))
// Equipping: the player's client-side equip / unequip, as the inventory screen uses them on the UI thread
// (0x507d20 / 0x508090 call these for the player; inv = rp(rp(actor+0x68)+0x38), actor at inv+8). The request
// handler cores (0x2ad34c0 / 0x2ad7790) need a worker thread's TLS context and fault on the UI thread.
U64 kRvaEquip = 0xa8ccb0;           // int* (inv, int* err, u16 bag id, short pos, short slot, u8 scratch)
U64 kRvaUnequip = 0xa904d0;         // int* (inv, int* err, short slot)
U64 kRvaInventory = 0x22ee4f0;      // mgr* (actor): +0x18 bag*[], u32 count +0x20
U64 kRvaErrorMgr = 0x1609970;       // mgr* ()
U64 kRvaErrorText = 0x1609ec0;      // entry* (mgr, const int* err); text = **entry
U64 kRvaToast = 0x1171760;
// Native text-input modal (TextEditModalMessage), opened as the game's own naming prompts open it.
U64 kRvaFindView = 0x38a8c30;       // view* (ui, const char* name), e.g. "ModalMessageView"
U64 kRvaModalParamsInit = 0xe909f0; // (params*): fixed char buffers {char[cap] .. char* heap, u32 cap, u32 len}
U64 kRvaModalParamsFree = 0xe90d70; // (params*)
U64 kRvaOpenTextModal = 0xfd3700;   // modal* (parent or 0, view, params*, int max length)
U64 kRvaGetText = 0x41d3840;        // (element, char* out, int size): strncat of the element's utf8 text
U64 kRvaNameInvalid = 0x14a2820;    // bool (const char* utf8): the game's name filter (true = rejected)
U64 kRvaHideScript = 0xdec8a0;      // (script*): hide; sets +0xbe
U64 kRvaQueueHide = 0x38abcc0;      // (ui manager, view element): queues the view's close (the menu button's release does)
U64 kRvaBindingInput = 0xe49730;    // (dispatcher, input event, key binding): a HUD key binding's input state machine
U64 kRvaTextModalVt = 0x5a0f1e0;
U64 kRvaTextModalClick = 0xfd3e20;            // TextEdit modal slot 113
// Native yes / no modal (CommonModalMessage), same view and params block: title, message, both button labels.
U64 kRvaOpenCommonModal = 0xfcd590; // modal* (parent or 0, view, params*, const char* modal name)
U64 kRvaCommonModalVt = 0x5a0eca0;
U64 kRvaCommonModalClick = 0xfcd140; // slot 113 (thunk): confirm / cancel -> owner callback, then hide
U64 kRvaCommonModalName = 0x59eb968; // "CommonModalMessage" (the game passes this string)

int kSlotOpen = 17;        // kRvaRootOpen
int kSlotClose = 20;       // kRvaRootClose
int kSlotFrame = 47;       // kRvaRootFrame, per-frame float getter (shared base)
int kSlotFocus = 123;      // kRvaRootFocus (slot focus -> tooltips, vfunc +0x3d8: self, slot script)
int kSlotTabChanged = 114; // kRvaRootTabChanged (TabBar owner callback, vfunc +0x390)
int kSlotCursorSelect = 139; // kRvaRootCursorSelect (RadialCursor owner callback, vfunc +0x458: slot index or -1)
// What sits in those RootQuickSlot slots (checked by the resolver; a moved slot = feature off).
U64 kRvaRootOpen = 0x1209170;          // thunk
U64 kRvaRootClose = 0x12093d0;
U64 kRvaRootFrame = 0x6ff970;
U64 kRvaRootFocus = 0x1208520;
U64 kRvaRootTabChanged = 0x1208000;
U64 kRvaRootCursorSelect = 0x1208ae0;

// --- Struct offsets (1.0.0.2976) ----------------------------------------------------------------
// Where the game's own code uses a field, its offset is read from that instruction at start-up (tools/siggen.py
// OFFSETS lists the witness functions); a changed offset is only taken when two witnesses agree. Fields without a
// witness keep their value. Every object reached through them is type-checked before use (layout_ok).
U64 kOffSelectorPage = 0xf4;      // ItemSelector: u32 dots per page (only the lit dot's page is drawn)
U64 kOffInputText = 0x68;         // TextEdit input: -> its text element
U64 kOffInputCaret = 0xd8;        // TextEdit input: u32 caret, in characters
U64 kOffInputCaretEnd = 0xdc;     // TextEdit input: u32 selection end (== caret: no selection)
U64 kOffInputCaretMoved = 0xf1;   // TextEdit input: u8, set by the game's caret keys (redraw the caret now)
U64 kOffInputBusy = 0xf9;         // TextEdit input: u8, nonzero while the box is not taking text
U64 kOffElemRect = 0xd0;          // element: float left, top, right, bottom (client px; +4 / +8 / +0xc)
U64 kOffRootTabBar = 0x180;       // RootQuickSlot -> TabBar script
U64 kOffRootCenterTip = 0x188;    // -> centre tooltip script (CommonInfoDescription)
U64 kOffRootCursor = 0x1d0;       // -> RadialCursor script
U64 kOffRootBodies = 0x1e0;       // -> tab body scripts[]
U64 kOffRootBodyCount = 0x1e8;    // u32
U64 kOffRootSlotVecs = 0x1f0;     // -> {slot script*[] , u32 count}[] per tab (0x10 each)
U64 kOffRootSlotVecCount = 0x1f8; // u32
U64 kOffRootFocusIdx = 0x28d;     // u8 depth mode (the lantern-style submenu is open): the focused-slot getter's argument
U64 kOffRootDepth = 0x210;        // -> depth menu script (QuickSlotDepth_ItemSignal)
U64 kOffDepthEntryCount = 0xd0;   // u32
U64 kOffRootDepthSel = 0x286;     // u16 highlighted submenu entry (0xFFFF: none)
U64 kOffSlotSelector = 0x108;     // slot script -> its dots (ItemSelector script)
U64 kOffSelectorCount = 0xf0;     // ItemSelector: u32 dot count
U64 kOffSelectorLit = 0xf8;       // ItemSelector: u32 the lit dot (written by the select at its end)
U64 kOffTabHeaders = 0xc8;        // TabBar -> header scripts[]
U64 kOffTabHeaderCount = 0xd0;    // u32
U64 kOffTabCurrent = 0x114;       // u32 (no witness)
U64 kOffCursorSectors = 0x120;    // RadialCursor: u16 sector count
U64 kOffModalOwner = 0x28;        // modal: owner callback object
U64 kOffModalShown = 0xbd;        // u8
U64 kOffModalHidden = 0xbe;       // u8
U64 kOffModalD0 = 0xd0;           // -> {.. u8 +0xf2}
U64 kOffModalConfirm = 0xd8;      // confirm button element
U64 kOffModalCancel = 0xe0;       // cancel button element
U64 kOffModalInput = 0x148;       // TextEdit: input element
U64 kOffActorComp = 0x68;         // actor -> component
U64 kOffCompInventory = 0x38;     // component -> inventory / equipment
U64 kOffActorKind = 0x88;         // actor -> {u8 +1: 1 = the player's branch}
U64 kOffInvEquipList = 0xa0;      // inventory -> equipment list
U64 kOffMgrBags = 0x18;           // inventory manager -> bag*[]
U64 kOffHolderParty = 0x158;      // player holder -> party actor*[] (and +0x128/+0x138/+0x148; order varies; no witness)
U64 kOffEntrySlot = 0xc8;         // equipment entry: u16 equip slot (after the 200-byte item)
U64 kOffScriptData = 0x148;       // slot script -> slot data
U64 kOffScriptEquipIdx = 0x190;   // slot script: u16 quickslot id
U64 kOffScriptCategoryIcon = 0x120; // slot script -> element that gets quickslot-item-image-*
U64 kOffDataItem = 0x8c0;         // slot data: item (when valid)
U64 kOffDataItemAlt = 0x618;      // slot data: item otherwise
U64 kOffDataType = 0x9c0;         // slot data: u8 category type
U64 kOffBindingPressed = 0x222;   // HUD key binding: u8 pressed
U64 kOffEventType = 0x38;         // input event: u8 type (3 down, 6 held, 7 up)
U64 kOffViewFlags = 0xb8;         // view: u8 (& 0x60) == 0x40 = shown
U64 kOffViewInner = 0xa0;         // view -> {.. +0x10 panel}
U64 kOffPanelFlags = 0x27d;       // panel: u8 (bit 0 button held, bit 1 opened by holding)
U64 kOffPanelHoldClose = 0x27c;   // panel: u8
U64 kOffPanelElem = 0x120;        // panel -> view element
U64 kOffModelRoot = 0xb0;         // model holder -> character model (0x78 before the 2026-10-30 update; was literal)
U64 kOffBagCount = 0x8;           // bag: short item capacity (0xc before; was literal)
U64 kOffUiTextures = 0x312a8;     // UI manager -> texture manager (0x311e8 before the 2026-10-31 patch; was literal)
// Classes those objects must have, and functions that only serve as offset witnesses.
U64 kRvaTabBarVt = 0x5a24dc8;        // UIGamePlayControlCommon_TabBar
U64 kRvaRadialCursorVt = 0x5a690e0;  // UIGamePlayControlCommon_RadialCursor
U64 kRvaCenterTipVt = 0x59d4598;     // UIGamePlayControlCommonInfoDescription
U64 kRvaHeaderVt = 0x5a22890;        // UIGamePlayControl_Common_CheckBox (tab headers)
U64 kRvaBodyVt = 0x5916e98;          // UIGamePlayControlCommonDefault (tab bodies)
U64 kRvaEquipSlotVt = 0x5a48208;     // UIGamePlayControlEquipmentItemQuickSlot (Armor / Outfits slots)
U64 kRvaHudVt = 0x5a77ef0;           // UIGamePlayControlRootHudOver
U64 kRvaEquipSlot2Vt = 0x5a487e8;    // UIGamePlayControlCommonEquipmentQuickSlot2 (anchor for its slots)
U64 kRvaBaseScriptVt = 0x59d2678;    // UIBaseScriptObject (anchor for its slots)
U64 kRvaShowPartVt = 0x590b850;      // ClientFrameEventTransformShowPart (anchor for its slots)
U64 kRvaRootSlot0 = 0x1207430;       // RootQuickSlot vtable slots 0, 1, 49, 52
U64 kRvaRootSlot1 = 0x12075a0;
U64 kRvaRootSlot49 = 0xe3cf10;
U64 kRvaRootSlot52 = 0x1209bf0;
U64 kRvaTabBarSlot0 = 0xffe7f0;      // TabBar slots 0, 1, 46, 113
U64 kRvaTabBarSlot1 = 0xffe8b0;
U64 kRvaTabBarSlot46 = 0xfff110;
U64 kRvaTabBarSlot113 = 0xfff290;
U64 kRvaRadialPageListVt = 0x5a68b48; // the RadialCursor's page list (the cursor slots 199 / 215 recipes are its slots)
U64 kRvaCursorSlot51 = 0x11cf5b0;    // RadialCursor slots 51, 199, 215
U64 kRvaCursorSlot199 = 0x11d0240;
U64 kRvaCursorSlot215 = 0x11cf550;
U64 kRvaSlotScript164 = 0x10b5b50;    // EquipmentItemQuickSlot slots 164, 166, 178, 185, 35
U64 kRvaSlotScript166 = 0x10b85f0;
U64 kRvaSlotScript178 = 0x10b7c60;
U64 kRvaSlotScript185 = 0x10b4740;
U64 kRvaSlotScript35 = 0x10b53b0;
U64 kRvaSlotScript181 = 0x10b9300;    // the slot's scroll handlers (mouse wheel / gamepad): previous / next item
U64 kRvaSlotScript182 = 0x10b9820;
U64 kRvaHudInput = 0x12459f0;        // RootHudOver slot 113: the menu buttons' element events
U64 kRvaDepthVt = 0x5a476e8;         // UIGamePlayControlCommon_QuickSlotDepth_ItemSignal (the submenu)
U64 kRvaDepthMode = 0x120b1b0;       // (RootQuickSlot*, u8 on): enter / leave the submenu
U64 kRvaDepthClick = 0x10b4b90;      // (depth entry script): the game's mouse click on a submenu entry (Back / apply)
U64 kRvaDepthKey = 0x10b5110;        // (depth entry script, u8 event): its key press (event 2: confirm, any device)
U64 kRvaSlotEvent = 0x10b4a00;       // slot / submenu entry scripts' slot 113 (script, elem, elem, u8): an element event;
                                     // Space activates the highlighted entry here (apply + leave, as a click does)
U64 kRvaDepthApply = 0x10bf850;       // (depth script, u32 entry): a pressed entry applies its value (not for Back)
U64 kRvaCommonModalSlot110 = 0x416dac0; // CommonModal slots 110 / 159: pattern-checked neighbours that place the
U64 kRvaCommonModalSlot159 = 0xdee950;  // click thunk (slot 113) after an update
U64 kRvaEquipRequest = 0x5bd8f0;     // UI equip request (actor, err, item copy, slot): companions' path too
U64 kRvaCompanionEquip = 0xae4f90;   // the request under it for companions (target, err, id, u16 bag, pos, slot)
U64 kRvaUnequipRequest = 0x5bdc70;   // UI unequip request (actor, err, slot) (thunk)
U64 kRvaWornLookup = 0x22850b0;      // looks worn pieces up in the equipment list
U64 kRvaBagItem = 0x22f4670;         // (mgr, bag id, pos) -> item
U64 kRvaItemMove = 0xaca980;         // the storage screen's move (inventory, err, from owner, to owner, key, ...)
U64 kRvaBagInfo = 0x555160;
          // (const u16* bag id) -> bag info: +0x38 transfer rules (0xa0 each), +0x40 count
// Appearance: the model's per-part visibility layers (part+0x70: bit 16+layer = the layer decides, bit layer = shown;
// the highest deciding layer wins). The game uses layers 0-5 and 7 (4 = the headgear option, 2 = the flight cloak's
// own item); ours is 6.
U64 kRvaVisSet = 0x2f1f540;         // (model, {int* handles, u32 n}*, u8 shown, u8 layer, 0, 0, u8 0)
U64 kRvaVisRelease = 0x2f1f910;     // (model, {int* handles, u32 n}*, u8 layer, 1, 0): the layer stops deciding
U64 kRvaBattleVis = 0xa86bd0;       // (inventory, u8 show, u8 show): the headgear option's switch (its argument is the
                                    // option's verdict "headgear shown now", not the battle state)
U64 kRvaBattleCheck = 0x1929070;    // (actor) -> u8: in battle (its aggro / target table): the headgear decision's input
U64 kRvaSlotHandle = 0xa96b10;      // (inventory, int* out, u16 slot) -> the worn piece's model handle (0 = none)
U64 kRvaModelSwap = 0x2f23140;      // the model rebuild job's swap (job, .., u8 phase, u8* state): new parts are in
U64 kRvaDockedList = 0x227e060;      // (docking manager, u32 tag, list {u32* p, n, cap, inline}): the ids docked with a tag
U64 kRvaEntityReg = 0x71b6fd8;       // -> -> the entity registry; vcall +0x28 (registry, handle out, id): as acquire_player's ctx
U64 kRvaEntityReg2 = 0x71b6fe0;      // (the same, for threads with TLS byte +0x1EB set)
U64 kRvaEquipVisCheck = 0xb63920;   // (vis ctrl, u32* part name, part record, u8 drawn): a weapon part drawn / sheathed
U64 kRvaFrameShowPart = 0x79f490;   // the animations' frame event "FrameEvent-TransformShowPart": shows / hides parts on
                                    // layer 0 mid-move (a mechanic; the rest of layer 0 is the weapon display settings)
U64 kRvaPartNameMap = 0x38c41d0;    // a map lookup whose first rip-relative load is the global holding the part-name
SetTextFn g_set_text = nullptr;
CtxFn g_actor_ctx = nullptr;
CtxReleaseFn g_ctx_release = nullptr;
IconNameFn g_icon_name = nullptr;
StrAssignFn g_str_assign = nullptr;
StrFn g_str_normalize = nullptr;
StrFn g_str_lower = nullptr;
StrFn g_str_destroy = nullptr;
SetStylePropFn g_set_style_prop = nullptr;
TextureExistsFn g_texture_exists = nullptr;
SetStateFn g_set_state = nullptr;
FocusedSlotFn g_focused_slot = nullptr;
EquipFn g_equip = nullptr;
UnequipFn g_unequip = nullptr;
ItemMoveFn g_item_move = nullptr;
BagInfoFn g_bag_info = nullptr;
EquipRequestFn g_equip_request = nullptr;
UnequipRequestFn g_unequip_request = nullptr;
InventoryFn g_inventory = nullptr;
ErrorMgrFn g_error_mgr = nullptr;
ErrorTextFn g_error_text = nullptr;
ToastFn g_toast = nullptr;
FindViewFn g_find_view = nullptr;
VoidFn g_params_init = nullptr;
VoidFn g_params_free = nullptr;
OpenTextModalFn g_open_text_modal = nullptr; // null: no rename prompt (names stay "Outfit N")
GetTextFn g_get_text = nullptr;
NameInvalidFn g_name_invalid = nullptr;
OpenCommonModalFn g_open_common_modal = nullptr; // null: no missing-pieces question (copies are not offered)
InternFn g_intern = nullptr;
VecAddFn g_vec_add = nullptr;
VecRemoveFn g_vec_remove = nullptr;
VoidFn g_vec_sort = nullptr;

// The injected record: a copy of the template record pointing at our own slot array.
alignas(16) std::uint8_t g_fake_rec[kRecordSize];
alignas(16) std::uint8_t g_fake_slots[kOutfitSlots * kSlotSize];

// Where an item's info keeps its stat lists (the 20261104 patch moved both by 8: 0x118 -> 0x120, 0x188 -> 0x190):
// found again at the first menu open from items whose values are known (calibrate_resist_ids).
U64 kOffItemPassives = 0x118; // -> {entries*, count} (null when none: the game's helper +2603A00)
U64 kOffItemStats = 0x188;    // -> {rows*, count}; a row's base stats at +0x58 / +0x60

// --- Item names: the game's item info table (as its own item-name text does) ---
U64 kRvaItemInfo = 0x3dcfc0;        // (unused, u32 key) -> item info; unknown keys -> the default info
U64 kRvaDefaultItemInfo = 0x7148c08; // pointer to that default info

// --- Setup --------------------------------------------------------------------------------------
// Game addresses come from the resolver (resolver.h): cached for this exe, else the known build's, else searched.
// A missing address turns off only the feature that needs it; the core (the Outfits tab itself) is all or nothing.
#include "om_sigs.h"

constexpr std::size_t kRecipeCount = std::size(kRecipes);
resolver::Result g_resolved[kRecipeCount];
std::wstring g_cache_path;
std::string g_features_off; // named in one toast at the first menu open (empty = everything works)

// True if every listed address was resolved (unresolved ones are 0).
bool have(std::initializer_list<U64> rvas)
{
    for (U64 v : rvas)
        if (!v) return false;
    return true;
}

void feature_off(const char* name)
{
    LOG("feature off: %s (game addresses not found)", name);
    g_features_off += (g_features_off.empty() ? "" : ",") + std::string(name); // (keys: features_toast words them)
}

// The hooked vtable slots follow their functions: a patch that adds or removes virtuals shifts the numbers. A
// function found but not in its vtable any more cannot be hooked there: its address is cleared (= feature off).
void adopt_slots()
{
    struct { int* slot; U64* rva; } slots[] = {
        {&kSlotOpen, &kRvaRootOpen}, {&kSlotClose, &kRvaRootClose}, {&kSlotFrame, &kRvaRootFrame},
        {&kSlotFocus, &kRvaRootFocus}, {&kSlotTabChanged, &kRvaRootTabChanged},
        {&kSlotCursorSelect, &kRvaRootCursorSelect}, {&kSlotTextModalClick, &kRvaTextModalClick},
        {&kSlotCommonModalClick, &kRvaCommonModalClick},
    };
    for (auto& s : slots)
        for (std::size_t i = 0; i < kRecipeCount; ++i) {
            if (kRecipeVars[i] != s.rva || !*s.rva) continue;
            if (g_resolved[i].slot < 0) {
                LOG("resolver: %s is not in its vtable; not hooked", kRecipes[i].name);
                *s.rva = 0;
            } else {
                *s.slot = g_resolved[i].slot;
            }
        }
}

void resolve_addresses()
{
    for (std::size_t i = 0; i < kRecipeCount; ++i)
        if (*kRecipeVars[i] != kRecipes[i].known)
            LOG("resolver: om_sigs.h is stale for %s (+%llX in the source, +%X in the table): rerun tools/sigs2h.py",
                kRecipes[i].name, *kRecipeVars[i], kRecipes[i].known);
#ifdef OM_RESOLVE_TEST
    constexpr bool kSearchOnly = true; // test build: every address must be found by search alone
#else
    constexpr bool kSearchOnly = false;
#endif
    resolver::resolve(kRecipes, kRecipeCount, g_resolved, g_cache_path.c_str(), kSearchOnly);
    for (std::size_t i = 0; i < kRecipeCount; ++i) {
        auto h = g_resolved[i].how;
        bool ok = h == resolver::How::Cached || h == resolver::How::Known || h == resolver::How::Found;
        *kRecipeVars[i] = ok ? g_resolved[i].rva : 0;
    }
    adopt_slots();
    // Struct offsets from the game's code (cheap: a few short searches inside known functions, every launch).
    constexpr std::size_t kOffsetCount = std::size(kOffsets);
    U64 values[kOffsetCount];
    for (std::size_t i = 0; i < kOffsetCount; ++i)
        if (*kOffsetVars[i] != kOffsets[i].known)
            LOG("resolver: om_sigs.h is stale for %s (+%llX in the source, +%X in the table): rerun tools/sigs2h.py",
                kOffsets[i].name, *kOffsetVars[i], kOffsets[i].known);
    resolver::OffsetStatus status[kOffsetCount];
    resolver::resolve_offsets(kOffsets, kOffsetCount, g_resolved, values, status);
    // Fields that sit side by side in the game's structs ({pointer, count}, neighbouring flags; true on every build so far): a
    // neighbour two witnesses have seen move takes this one along when this one's own witnesses can't say.
    struct Pair { U64* field; U64* neighbour; std::int64_t delta; };
    const Pair pairs[] = {{&kOffRootBodyCount, &kOffRootBodies, 8}, {&kOffRootSlotVecCount, &kOffRootSlotVecs, 8},
                          {&kOffPanelHoldClose, &kOffPanelFlags, -1}, {&kOffModalShown, &kOffModalHidden, -1},
                          {&kOffSelectorPage, &kOffSelectorCount, 4}}; // (the dots per page sit between the count and the lit dot)
    auto index_of = [&](const U64* v) {
        for (std::size_t i = 0; i < kOffsetCount; ++i)
            if (kOffsetVars[i] == v) return i;
        return kOffsetCount;
    };
    for (const Pair& p : pairs) {
        std::size_t f = index_of(p.field), n = index_of(p.neighbour);
        if (f == kOffsetCount || n == kOffsetCount || status[n] != resolver::OffsetStatus::Moved || status[f] == resolver::OffsetStatus::Moved) continue;
        values[f] = values[n] + p.delta;
        status[f] = resolver::OffsetStatus::Moved;
        LOG("resolver: offset %s follows %s: +%llX", kOffsets[f].name, kOffsets[n].name, values[f]);
    }
    // Fields the plugin WRITES through (or hands to the game): if the game's code contradicts the known offset and
    // the new one isn't certain, that one feature goes off (offset 0) rather than writing into the wrong field.
    // (kOffViewFlags: read, but only to decide those writes.) No witness found at all counts too: the code changed there.
    const U64* writes[] = {&kOffCursorSectors, &kOffModalOwner, &kOffModalD0, &kOffPanelFlags, &kOffPanelHoldClose,
                           &kOffPanelElem, &kOffEventType, &kOffBindingPressed, &kOffViewFlags};
    for (std::size_t i = 0; i < kOffsetCount; ++i) {
        *kOffsetVars[i] = values[i];
        bool unsure = status[i] == resolver::OffsetStatus::Doubtful ||
                      (status[i] == resolver::OffsetStatus::Unchecked && kOffsets[i].nanchors);
        if (unsure &&
            std::find(std::begin(writes), std::end(writes), kOffsetVars[i]) != std::end(writes)) {
            LOG("resolver: offset %s is doubtful and is written through: what uses it is off", kOffsets[i].name);
            *kOffsetVars[i] = 0;
        }
    }
}

} // namespace om
