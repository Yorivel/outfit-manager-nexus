// Game addresses, struct offsets and game functions (found again by the resolver after a game update).
#include "om.h"
#include "addrs.h"
#include "prompts.h"

namespace om {

// --- Game addresses (CrimsonDesert 1.0.0.2976) ---------------------------------------------------
U64 kRvaRootQuickSlotVt = 0x5af7608;
U64 kRvaQuickSlotInfoGet = 0x12489b0;  // record* (unused, u32 key)
U64 kRvaStringInfoGet = 0x3db010;      // stringinfo* (unused, u32 id); +0x10 -> char**
U64 kRvaQuickSlotInfoMgr = 0x7261910;  // ptr; 2.05: +0x28 count, +0x20 rows of 24 bytes (the record at +8)
U64 kRvaClassNames = 0x7160060;        // ptr; +0x58 -> {char* name, ...}[id] (stride 0x10)
U64 kRvaBuildBodies = 0x1245870;       // RootQuickSlot: one tab body per QuickSlotInfo record
U64 kRvaGearMode = 0x12461d0;          // RootQuickSlot: show Weapons/Armor headers, select tab 0
U64 kRvaSetText = 0x4256be0;           // (element, const char* utf8, flag)
U64 kRvaQsiTableRoot0 = 0x7245558; // ***root +8 -> +0x460: table the builder counts
U64 kRvaQsiTableRoot1 = 0x7245560;
U64 kRvaIntern = 0x14de770;         // (u64* out id, const char* class, 1, 0x2ffff)
U64 kRvaClassVecAdd = 0x3915af0;    // bool (vec*, const u32* id): appends if missing
U64 kRvaClassVecRemove = 0x3915b90; // u32 index or -1 (vec*, const u32* id)
U64 kRvaClassVecSort = 0x3916520;   // (vec*)
U64 kRvaRestyle = 0x3912620;        // (class holder*)

// Worn gear: actor context {vt, actor, u8 valid, ..} of the player (released like a ref-holder).
U64 kRvaPlayerHolder = 0x7245218;   // ptr -> ptr -> argument of the context getter
U64 kRvaActorCtx = 0x9cdc90;        // (U64 holder, U64 ctx[4])
U64 kRvaCtxRelease = 0x1659380;     // (U64 ctx[4]) after ctx[0] = kRvaCtxVt
U64 kRvaCtxVt = 0x5b40000;
U64 kRvaItemIconName = 0x2671820;   // const char* (item*): persistent icon texture name
U64 kRvaEmptyItem = 0x71d6098;      // ptr -> empty-item sentinel
// Dynamic background image, as the item icons set theirs: string var {strobj*, u8 type 2}.
U64 kRvaEmptyString = 0x6f29f48;    // empty string object (var default)
U64 kRvaStrAssign = 0x464550;       // (var*, const char*)
U64 kRvaStrNormalize = 0x1644a20;   // (var*)
U64 kRvaStrLower = 0x1644e00;       // (var*)
U64 kRvaStrDestroy = 0x3e6f50;      // (var*)
U64 kRvaSetStyleProp = 0x73f0b0;    // (class holder*, const u16* prop, var*, const u16* flag)
U64 kRvaItemTable = 0x7246370;       // global -> the item table (2.05: +0x28 u32 rows, +0x20 rows of 24 bytes: {u32 permanent id, u32 body offset, record}, by key)
U64 kRvaItemSelectorVt = 0x5a9de58; // UIGamePlayControlCommon_ItemSelector (a slot's dots, Selection.CommonItemSelector)
U64 kRvaSelectorSelect = 0x1049000;  // (selector, u32 index, u8): lights dot `index` (index >= count: none; shows count)
U64 kRvaTextureExists = 0x42b2c50;  // bool (*(uiMgr + kOffUiTextures), var*)
U64 kRvaIconWitness0 = 0xe3b040;     // only offset witnesses for kOffUiTextures: the game's own "texture exists?" callers
U64 kRvaIconWitness1 = 0xe3ba80;
U64 kRvaTextCaretWitness0 = 0x4204db0; // only offset witnesses for the text box fields (kOffInput*): the game's own
U64 kRvaTextCaretWitness1 = 0x4206240; // text box code (caret keys, selection, typing)
U64 kRvaTextCaretWitness2 = 0x420ad50;
U64 kRvaElemRectWitness0 = 0x4a7ea70;  // only offset witnesses for kOffElemRect: the game's element overlap test
U64 kRvaElemRectWitness1 = 0x4b3ce50;  // and a layout setter
U64 kRvaIconScriptFlag = 0x7204258; // u8: game uses the image-script path instead (logged only)
// Centre tooltip (RootQuickSlot+0x188, UIGamePlayControlCommonInfoDescription) show/hide state machine.
U64 kRvaSetState = 0xe291d0;        // (script*, const u32* state, 0)
U64 kRvaStateShow = 0x71a1964;      // u32 state ids (current state at script+0xa8)
U64 kRvaStateHide = 0x71a1968;
U64 kRvaFocusedSlot = 0x1248200;    // slot script* (RootQuickSlot*, u8 *(self+0x28d))
// Equipping: the player's client-side equip / unequip, as the inventory screen uses them on the UI thread
// (0x507d20 / 0x508090 call these for the player; inv = rp(rp(actor+0x68)+0x38), actor at inv+8). The request
// handler cores (0x2ad34c0 / 0x2ad7790) need a worker thread's TLS context and fault on the UI thread.
U64 kRvaEquip = 0xac39c0;           // int* (inv, int* err, u16 bag id, short pos, short slot, u8 scratch)
U64 kRvaUnequip = 0xac73a0;         // int* (inv, int* err, short slot)
U64 kRvaInventory = 0x23626f0;      // mgr* (actor): +0x18 bag*[], u32 count +0x20
U64 kRvaErrorMgr = 0x164c420;       // mgr* ()
U64 kRvaErrorText = 0x164c970;      // entry* (mgr, const int* err); text = **entry
U64 kRvaToast = 0x11ab5a0;
// Native text-input modal (TextEditModalMessage), opened as the game's own naming prompts open it.
U64 kRvaFindView = 0x3927fb0;       // view* (ui, const char* name), e.g. "ModalMessageView"
U64 kRvaModalParamsInit = 0xecbb10; // (params*): fixed char buffers {char[cap] .. char* heap, u32 cap, u32 len}
U64 kRvaModalParamsFree = 0xecbe90; // (params*)
U64 kRvaOpenTextModal = 0x100eaa0;   // modal* (parent or 0, view, params*, int max length)
U64 kRvaGetText = 0x4256a80;        // (element, char* out, int size): strncat of the element's utf8 text
U64 kRvaNameInvalid = 0x14e3280;    // bool (const char* utf8): the game's name filter (true = rejected)
U64 kRvaHideScript = 0xe26f10;      // (script*): hide; sets +0xbe
U64 kRvaQueueHide = 0x392b040;      // (ui manager, view element): queues the view's close (the menu button's release does)
U64 kRvaBindingInput = 0xe84650;    // (dispatcher, input event, key binding): a HUD key binding's input state machine
U64 kRvaTextModalVt = 0x5a957e8;
U64 kRvaTextModalClick = 0x100f1c0;            // TextEdit modal slot 113
// Native yes / no modal (CommonModalMessage), same view and params block: title, message, both button labels.
U64 kRvaOpenCommonModal = 0x1008930; // modal* (parent or 0, view, params*, const char* modal name)
U64 kRvaCommonModalVt = 0x5a952a8;
U64 kRvaCommonModalClick = 0x10084e0; // slot 113 (thunk): confirm / cancel -> owner callback, then hide
U64 kRvaCommonModalName = 0x5a6fdd0; // "CommonModalMessage" (the game passes this string)

int kSlotOpen = 17;        // kRvaRootOpen
int kSlotClose = 20;       // kRvaRootClose
int kSlotFrame = 47;       // kRvaRootFrame, per-frame float getter (shared base)
int kSlotFocus = 123;      // kRvaRootFocus (slot focus -> tooltips, vfunc +0x3d8: self, slot script)
int kSlotTabChanged = 114; // kRvaRootTabChanged (TabBar owner callback, vfunc +0x390)
int kSlotCursorSelect = 139; // kRvaRootCursorSelect (RadialCursor owner callback, vfunc +0x458: slot index or -1)
// What sits in those RootQuickSlot slots (checked by the resolver; a moved slot = feature off).
U64 kRvaRootOpen = 0x1244750;          // thunk
U64 kRvaRootClose = 0x12449b0;
U64 kRvaRootFrame = 0x72c2c0;
U64 kRvaRootFocus = 0x1243b00;
U64 kRvaRootTabChanged = 0x12435e0;
U64 kRvaRootCursorSelect = 0x12440c0;

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
U64 kRvaTabBarVt = 0x5aa6cb0;        // UIGamePlayControlCommon_TabBar
U64 kRvaRadialCursorVt = 0x5aebbf8;  // UIGamePlayControlCommon_RadialCursor
U64 kRvaCenterTipVt = 0x5a5a5f8;     // UIGamePlayControlCommonInfoDescription
U64 kRvaHeaderVt = 0x5aa8c80;        // UIGamePlayControl_Common_CheckBox (tab headers)
U64 kRvaBodyVt = 0x599b888;          // UIGamePlayControlCommonDefault (tab bodies)
U64 kRvaEquipSlotVt = 0x5ac9230;     // UIGamePlayControlEquipmentItemQuickSlot (Armor / Outfits slots)
U64 kRvaHudVt = 0x5afb400;           // UIGamePlayControlRootHudOver
U64 kRvaEquipSlot2Vt = 0x5ac9810;    // UIGamePlayControlCommonEquipmentQuickSlot2 (anchor for its slots)
U64 kRvaBaseScriptVt = 0x5a56530;    // UIBaseScriptObject (anchor for its slots)
U64 kRvaShowPartVt = 0x598fd48;      // ClientFrameEventTransformShowPart (anchor for its slots)
U64 kRvaRootSlot0 = 0x1242a10;       // RootQuickSlot vtable slots 0, 1, 49, 52
U64 kRvaRootSlot1 = 0x1242b80;
U64 kRvaRootSlot49 = 0xe77e50;
U64 kRvaRootSlot52 = 0x12451d0;
U64 kRvaTabBarSlot0 = 0x103a320;      // TabBar slots 0, 1, 46, 113
U64 kRvaTabBarSlot1 = 0x103a3e0;
U64 kRvaTabBarSlot46 = 0x103ac40;
U64 kRvaTabBarSlot113 = 0x103adc0;
U64 kRvaRadialPageListVt = 0x5aeb660; // the RadialCursor's page list (the cursor slots 199 / 215 recipes are its slots)
U64 kRvaCursorSlot51 = 0x120aa10;    // RadialCursor slots 51, 199, 215
U64 kRvaCursorSlot199 = 0x120b6a0;
U64 kRvaCursorSlot215 = 0x120a9b0;
U64 kRvaSlotScript164 = 0x10ee8b0;    // EquipmentItemQuickSlot slots 164, 166, 178, 185, 35
U64 kRvaBranchTry = 0x258ff30;        // the branch walker's try of one candidate (comp, u8 layer, cur*, {branch*, link*,
                                      // owner, chart}*, ...): evaluates it (kRvaBranchEval), looks its target up, takes
                                      // it; 0 = taken (anim.cpp)
U64 kRvaSetVisualEquipVt = 0x598efb0; // ClientFrameEventSetVisualEquipItem
U64 kRvaSetVisualEquipEv = 0x7d5ce0;  // its slot 1: the frame event run (this, int* out, data, x, ctx*): a visual item
                                      // put on / taken off an equip slot mid-animation (anim.cpp)
U64 kRvaItemCharges = 0x266e420;      // (item data*, ...): an item's charges now, recharged over time - divides by its
                                      // recharge time; its kRvaItemInfo call is within kItemChargesSize (anim.cpp)
U64 kRvaCharCtrlVt = 0x59c1660;       // ClientCharacterControlActorComponent
U64 kRvaCtrlAck = 0x976730;           // ClientCharacterControlActorComponent slot 43: the server's start-action
                                      // acknowledgement applied (comp, ctx*, records[2]*, x) -> int* (anim.cpp)
U64 kRvaBranchEval = 0x2591570;       // action chart branch evaluator: (ctx, out result, ..., link) - sleep (sleep.cpp)
U64 kRvaSlotScript166 = 0x10f1360;
U64 kRvaSlotScript178 = 0x10f09e0;
U64 kRvaSlotScript185 = 0x10ed4b0;
U64 kRvaSlotScript35 = 0x10ee120;
U64 kRvaSlotScript181 = 0x10f20b0;    // the slot's scroll handlers (mouse wheel / gamepad): previous / next item
U64 kRvaSlotScript182 = 0x10f25d0;
U64 kRvaHudInput = 0x1281690;        // RootHudOver slot 113: the menu buttons' element events
U64 kRvaDepthVt = 0x5aca3d0;         // UIGamePlayControlCommon_QuickSlotDepth_ItemSignal (the submenu)
U64 kRvaDepthMode = 0x1246750;       // (RootQuickSlot*, u8 on): enter / leave the submenu
U64 kRvaDepthClick = 0x10ed900;      // (depth entry script): the game's mouse click on a submenu entry (Back / apply)
U64 kRvaDepthKey = 0x10ede80;        // (depth entry script, u8 event): its key press (event 2: confirm, any device)
U64 kRvaSlotEvent = 0x10ed770;       // slot / submenu entry scripts' slot 113 (script, elem, elem, u8): an element event;
                                     // Space activates the highlighted entry here (apply + leave, as a click does)
U64 kRvaDepthApply = 0x10f8770;       // (depth script, u32 entry): a pressed entry applies its value (not for Back)
U64 kRvaCommonModalSlot110 = 0x41f1860; // CommonModal slots 110 / 159: pattern-checked neighbours that place the
U64 kRvaCommonModalSlot159 = 0xe28fc0;  // click thunk (slot 113) after an update
U64 kRvaEquipRequest = 0x5e9bd0;     // UI equip request (actor, err, item copy, slot): companions' path too
U64 kRvaCompanionEquip = 0xb1c880;   // the request under it for companions (target, err, id, u16 bag, pos, slot)
U64 kRvaUnequipRequest = 0x5e9f50;   // UI unequip request (actor, err, slot) (thunk)
U64 kRvaWornLookup = 0x22f9240;      // looks worn pieces up in the equipment list
U64 kRvaBagItem = 0x23687f0;         // (mgr, bag id, pos) -> item
U64 kRvaItemMove = 0xb01d50;         // the storage screen's move (inventory, err, from owner, to owner, key, ...)
U64 kRvaBagInfo = 0x57b410;
          // (const u16* bag id) -> bag info: +0x38 transfer rules (0xa0 each), +0x40 count
// Appearance: the model's per-part visibility layers (part+0x70: bit 16+layer = the layer decides, bit layer = shown;
// the highest deciding layer wins). The game uses layers 0-5 and 7 (4 = the headgear option, 2 = the flight cloak's
// own item); ours is 6.
U64 kRvaVisSet = 0x2f8dc80;         // (model, {int* handles, u32 n}*, u8 shown, u8 layer, 0, 0, u8 0)
U64 kRvaVisRelease = 0x2f8e010;     // (model, {int* handles, u32 n}*, u8 layer, 1, 0): the layer stops deciding
U64 kRvaBattleVis = 0xabd840;       // (inventory, u8 show, u8 show): the headgear option's switch (its argument is the
                                    // option's verdict "headgear shown now", not the battle state)
U64 kRvaBattleCheck = 0x19908f0;    // (actor) -> u8: in battle (its aggro / target table): the headgear decision's input
U64 kRvaSlotHandle = 0xacda40;      // (inventory, int* out, u16 slot) -> the worn piece's model handle (0 = none)
U64 kRvaModelSwap = 0x2f91800;      // the model rebuild job's swap (job, .., u8 phase, u8* state): new parts are in
U64 kRvaDockedList = 0x22f21d0;      // (docking manager, u32 tag, list {u32* p, n, cap, inline}): the ids docked with a tag
U64 kRvaEntityReg = 0x7245548;       // -> -> the entity registry; vcall +0x28 (registry, handle out, id): as acquire_player's ctx
U64 kRvaEntityReg2 = 0x7245550;      // (the same, for threads with TLS byte +0x1EB set)
U64 kRvaEquipVisCheck = 0xb9bf80;   // (vis ctrl, u32* part name, part record, u8 drawn): a weapon part drawn / sheathed
U64 kRvaFrameShowPart = 0x7cd320;   // the animations' frame event "FrameEvent-TransformShowPart": shows / hides parts on
                                    // layer 0 mid-move (a mechanic; the rest of layer 0 is the weapon display settings)
U64 kRvaPartNameMap = 0x3943550;    // a map lookup whose first rip-relative load is the global holding the part-name
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
U64 kOffItemPassives = 0x120; // -> {entries*, count} (null when none: the game's helper +2603A00); 0x118 before the
                               // 2026-11-06 build - the self-test finds it from items with known resistances (items.cpp)
U64 kOffItemStats = 0x190;    // -> {rows*, count}; a row's base stats at +0x58 / +0x60 (0x188 before 2026-11-06)

// --- Item names: the game's item info table (as its own item-name text does) ---
U64 kRvaItemInfo = 0x3daf10;        // (unused, u32 key) -> item info; unknown keys -> the default info
// The presentation (present.cpp): what the game builds and shows for a slot, apart from what is equipped.
U64 kRvaPresentChange = 0xab9ee0;   // (equip slot component, int* err, {put-on list*, 0}*, u8 0, take-off list*): the
                                    // central change of what a character presents - called
U64 kRvaPresentRefresh = 0xacfea0;  // (component, u16 slot, u16 shown item key, item*): a slot's presentation again
                                    // after a visual item left it (none on it: the keyed item comes off) - called
U64 kRvaItemInit = 0x266e830;       // (item object*, 0xD0 bytes): an empty item - called
U64 kRvaItemFree = 0x266fed0;       // (item object*): lets go of what it holds - called
U64 kRvaItemCopy = 0x266ffb0;       // (item object* to, item* from) - called
U64 kRvaFootwearSound = 0x1995080;  // (actor, ?, u16 slot or 0xFFFF) -> the footwear's sound material, 0: none - hooked
U64 kRvaDefaultItemInfo = 0x71d6d68; // pointer to that default info

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
                           &kOffPanelElem, &kOffEventType, &kOffBindingPressed, &kOffViewFlags,
                           // (not written, but what the game plays and shows follows them: off rather than wrong)
                           &kOffCompLantern, &kOffLanternLit, &kOffCompEffects, &kOffEffectsSound, &kOffSoundHandle,
                           &kOffHandleObject, &kOffContactBody, &kOffEventId, &kOffEventAlt, &kOffCompModel, &kOffModelScenes,
                           &kOffSwitchEvent, &kOffHolderGimmick, &kOffGimmickParent, &kOffSceneObjPart};
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


// Sounds and the lantern (sound.cpp; game build of 2026-11-06). Witnesses: the game functions that use the offsets below.
U64 kRvaLanternLight = 0x6171e0;     // (lights the character's lantern: +0x68 -> +0x50 -> +0x18C counter += 1)
U64 kRvaLanternDim = 0x617380;       // (puts it out: the counter -= 1)
U64 kRvaLanternCheck = 0x189da30;    // (the game's "is it lit": counter > 0)
U64 kRvaEffectSwitchVt = 0x5db2828;  // GimmickEventHandlerData_EffectSwitch (anchor for its slot)
U64 kRvaEffectSwitchRun = 0x2658390; // its slot 4: runs a gimmick chart's EffectSwitch (data, result*, ?, gimmick actor) -
                                     // hooked (sound.cpp: the lantern's pulse near a memory fragment)
U64 kRvaEffectSwitchSlot2 = 0x26581d0; // (its slot 2: a witness below slot 4, which is only a jump - places it)
U64 kRvaHandlerRunCheck = 0x2572370; // (every handler's slot 7: runs it only if its event, data +kOffSwitchEvent, matches)
U64 kRvaSearchNearVt = 0x5db2e20;    // GimmickEventHandlerData_SearchNearLevelGimmick
U64 kRvaSearchNearRun = 0x26670f0;   // (its slot 4: actor +kOffActorComp -> +kOffHolderGimmick, the gimmick component)
U64 kRvaSendGimEvVt = 0x5df7fb0;     // GimmickEventHandlerData_SendGimmickEvent
U64 kRvaSendGimEvRun = 0x2655e20;    // (its slot 4: the same path)
U64 kRvaGimmickCompVt = 0x59c3768;   // ClientGimmickActorComponent
U64 kRvaGimmickCompLoad = 0x23314a0; // (its slots 11 / 25: read / write its fields, +kOffGimmickParent among them)
U64 kRvaGimmickCompSave = 0x23305c0;
U64 kOffSwitchEvent = 0x44;          // gimmick event handler data -> u32 the chart event that runs it (name hash)
U64 kOffHolderGimmick = 0x30;        // actor component -> the gimmick component
U64 kOffGimmickParent = 0x288;       // gimmick component -> u32 its docking parent's actor id (actor +0x60)
U64 kRvaGearSounds = 0xbc1cb0;       // (sets the character's armorkey / material_game: its sound handle +0x70, number +0x18)
U64 kRvaGearSoundsFrom = 0xabfdb0;   // (calls it with the character's sound component: +0x68 -> +0x60 -> +0x18)
U64 kRvaCollideSound = 0x35546c0;    // (manager, contact record, ?, sound info, ...): sounds a collision - hooked
U64 kRvaPlayRequest = 0x3bd12d0;     // (audio manager, u32 sound object, position*, event key*, ...): the game's play request
                                     // (posts at once, or queues for the audio thread) - hooked
U64 kRvaEventLookup = 0x2872260;     // (event key*): -> the event's record (its Wwise id at +kOffEventId) - called
U64 kRvaEventPost = 0x3bd05f0;       // (the audio thread's post of a queued request: reads the record's id and alternative)
U64 kRvaContactCopy = 0x5b9660;      // (copies a collision contact record: its body handle at +kOffContactBody, ref-counted)
U64 kRvaSceneObjVt = 0x5989550;      // SceneObjectClient (what a contact's body handle holds): its main vtable - the handle
                                     // points at its part at +kOffSceneObjPart (vtable +590D1A8, RTTI offset 40); the
                                     // resolver's search finds a class's main vtable, so that one is kept here
U64 kRvaSceneObjInit = 0x537ce0;     // (SceneObjectClient's constructor: stores its vtables at +0, +kOffSceneObjPart, +0x40)
U64 kOffSceneObjPart = 0x28;         // SceneObjectClient -> the part a body handle points at
U64 kOffCompLantern = 0x50;          // character component -> its lantern state
U64 kOffLanternLit = 0x184;          // lantern state -> u32 lit counter (> 0: lit)
U64 kOffCompEffects = 0x60;          // character component -> its effects (also transition.cpp's effect component)
U64 kOffEffectsSound = 0x18;         // effects -> the character's sound component
U64 kOffSoundHandle = 0x70;          // sound component -> its sound handle
U64 kOffHandleObject = 0x18;         // sound handle -> u32 its Wwise game object
U64 kOffContactBody = 0x88;          // collision contact record -> the body handle it came from
U64 kOffEventId = 0x4;               // event record -> u32 its Wwise event id
U64 kOffEventAlt = 0x68;             // event record -> the key of its alternative (played instead from afar)
U64 kOffCompModel = 0x40;            // character component -> its model component (the headgear switch's path)
U64 kOffModelScenes = 0x130;         // model component -> his scene objects: +0 the main one's handle (-> the model)
U64 kOffScenesBody = 0x8;            // his scene objects -> the handle of the one his own collisions come from (a
                                     // contact's +kOffContactBody for his clinks; only his own handles are in there)

// The outfit transition and the hidden weapons' effects (transition.cpp; build 26480954, 2976 addresses in brackets).
U64 kRvaMatUpdate = 0x5b1cc0;        // (effect component): the player's material update - hooked, the values go out here [0x4d30f0]
U64 kRvaMatSend = 0x5b1c00;          // (effect component, u8 0, message*, 0): a material value to the character [0x4d3030]
U64 kRvaMatChange = 0x5cc8c0;        // (actor, time*, f32, u32* id, f32* values, u32 count, u8 mode 2, u32, parts*) [0x4ecc80]
U64 kRvaMatApply = 0x586720;         // (value table = effect component +0x400, f32 time step, scene list, owner object): the
                                     // game's apply pass - what was sent for single parts reaches the meshes
U64 kRvaMatApplyOwner = 0x5e1aa0;    // (the component's owner = component +8) -> the owner object the pass is given
U64 kRvaPermSet = 0x5b2e00;          // (effect component, u32* permutation id, message*, u8 all): UseMaterialEffect counter [0x4d4270]
U64 kRvaMsgInit = 0x3d1e1e0;         // (message*, key*): a material value message [0x39c3ee0]
U64 kRvaMsgFloatVtable = 0x6159b80;  // float message's vtable (value at +0x30) [0x145d0feb0]
U64 kRvaMsgUintVtable = 0x6158080;   // uint message's vtable [0x145d119b0]
U64 kRvaMsgBaseVtable = 0x614e470;   // the base message's vtable (left in it after the call, as the game does)
U64 kRvaPermKey = 0x7229db0;         // the UseMaterialEffect message's key [0x146a84e68]
U64 kRvaPermValue = 0x7229dc4;       // -> its u32 at +0x34 [0x146a84e80]
U64 kRvaPermId = 0x71d6ef8;          // u32: the permutation's id [0x146cfdb48]
U64 kRvaNameId = 0x14de770;          // (int* out, const char* name, 1, 0x2FFFF): a name's id [0x12becb0]
U64 kRvaMeshCollect = 0x3027df0;     // (out list*, settings*, object*, u8 1): a mesh effect's pieces on an object
U64 kRvaMeshCollectNamed = 0x3027b80; // (object*, out list*, settings*, ids*): ... on the parts with these names [0x2d0f550]
U64 kRvaMeshColour = 0x58afb0;       // (piece*, object*, arg, data*): a piece started with the effect's data [0x4b2100]
U64 kRvaMeshColourArg = 0x71aeb08;   // its third argument (the game passes this global) [0x146cd7ca8]

} // namespace om
