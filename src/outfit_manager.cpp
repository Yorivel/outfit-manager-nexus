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

#include "game.h"
#include "lang.h"
#include "log.h"
#include "resolver.h"
#include "uibin_patch.h"

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <MinHook.h>
#include <algorithm>
#include <intrin.h>
#include <climits>
#include <cmath>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

using U64 = std::uint64_t;
using U32 = std::uint32_t;
using U16 = std::uint16_t;
using U8 = std::uint8_t;

constexpr U64 kUserMax = 0x00007FFFFFFEFFFFull;

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
constexpr U16 kChestSlot = 4;                 // equip slot "equipment-4" (armor icon) = chest
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
constexpr U16 kPropBackgroundImage = 0x26;
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
// Native toast (AlertSystemOverView's ToastMessage), as the game's menus show it: (ui, const char* utf8, error 0);
// ui = any UI script's vfunc +0x118 (kVfUiMgr).
constexpr U64 kVfUiMgr = 0x118; // slot 35 (34 before the 2026-10-30 update)
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
// Vtable slots: the known build's numbers; adopt_slots() takes the slot each function is in now (resolver).
int kSlotTextModalClick = 115;   // (modal, ?, clicked element, char): +0xd8 confirm, +0xe0 cancel
int kSlotCommonModalClick = 115; // same handler role in the CommonModal
U64 kRvaTextModalClick = 0xfd3e20;            // TextEdit modal slot 113
constexpr U32 kParamsSize = 0x1428; // (0x1310 before the 2026-10-30 update: one more label buffer at the end)
constexpr U32 kParamsMessage = 0x118, kParamsMessageCap = 0x801, kParamsMessageLen = 0x92c;
constexpr int kNameMaxChars = 24;
// Native yes / no modal (CommonModalMessage), same view and params block: title, message, both button labels.
U64 kRvaOpenCommonModal = 0xfcd590; // modal* (parent or 0, view, params*, const char* modal name)
U64 kRvaCommonModalVt = 0x5a0eca0;
U64 kRvaCommonModalClick = 0xfcd140; // slot 113 (thunk): confirm / cancel -> owner callback, then hide
U64 kRvaCommonModalName = 0x59eb968; // "CommonModalMessage" (the game passes this string)
constexpr U32 kParamsTitle = 0x0, kParamsTitleLen = 0x114, kParamsConfirm = 0xfc0, kParamsCancel = 0x10d8;
constexpr U32 kParamsLabelCap = 0x100; // inline buffers of 0x108 bytes

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
                                    // strings (+0x58: 16-byte entries, the first u64 the name) - a part's +0x48 indexes it

constexpr U16 kTemplateRecord = 1; // Armor: 12 item slots
constexpr U32 kOutfitSlots = 12;
constexpr U32 kMaxOutfits = kOutfitSlots - 1; // the last slot is always the "+" (saving, the global settings)
constexpr U32 kRecordSize = 0x30; // (copied whole; the 2026-10-30 header is 8 bytes shorter)
// QuickSlotInfo record: slot count and slot array (+0x14 / +0x20 before the 2026-10-30 update)
constexpr U32 kRecSlotCount = 0x0c, kRecSlots = 0x18;
constexpr U32 kSlotSize = 0x50;
constexpr U32 kSlotNameHolder = 0x08; // -> {char* category, ...}
constexpr U32 kSlotHash = 0x1c;
constexpr U32 kSlotEquipIdx = 0x20;

// Category holder for the Outfits slots: the slot binder compares the name with its category table.
struct CategoryHolder {
    const char* name;
    U64 pad[3];
};
const CategoryHolder g_none_category{"none", {}};

U64 g_base = 0;
std::atomic<int> g_ui_tab_added{0}; // gear-wheel UI files that got the Outfits header (patched in memory at load)
HMODULE g_self = nullptr;

// The static-info getters take the key by value since the 2026-10-30 update (first argument unused; was const u16*).
using RecordGetFn = U64(__fastcall*)(U64, U32);
RecordGetFn g_qsi_get = nullptr; // original (trampoline once hooked)
RecordGetFn g_str_get = nullptr;

using OpenFn = U64(__fastcall*)(U64, U64, U64, U64);
using TabFn = U64(__fastcall*)(U64, U64, U64, U64, U64, U64);
using VoidFn = void(__fastcall*)(U64);
using SetTextFn = void(__fastcall*)(U64, const char*, U64);
OpenFn g_open_orig = nullptr;
TabFn g_tab_orig = nullptr;
OpenFn g_build_orig = nullptr;
OpenFn g_gear_orig = nullptr;
TabFn g_select_orig = nullptr;
TabFn g_close_orig = nullptr;
TabFn g_focus_orig = nullptr;
using FloatFn = float(__fastcall*)(U64);
FloatFn g_frame_orig = nullptr;
SetTextFn g_set_text = nullptr;

using CtxFn = U64(__fastcall*)(U64, U64*);
using CtxReleaseFn = void(__fastcall*)(U64*);
using IconNameFn = const char*(__fastcall*)(U64);
using StrAssignFn = void(__fastcall*)(U64*, const char*);
using StrFn = void(__fastcall*)(U64*);
using SetStylePropFn = void(__fastcall*)(U64, const U64*, U64*, const U64*);
using TextureExistsFn = bool(__fastcall*)(U64, U64*);
using UiMgrFn = U64(__fastcall*)(U64);
using SetStateFn = void(__fastcall*)(U64, const U32*, U64);
using FocusedSlotFn = U64(__fastcall*)(U64, U64);
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

// Equip writes an error code through a pointer to its 6th argument slot, so pass one to have it reserved.
using EquipFn = int*(__fastcall*)(U64, int*, U16, short, short, U64);
using UnequipFn = int*(__fastcall*)(U64, int*, short);
using InventoryFn = U64(__fastcall*)(U64);
using ErrorMgrFn = U64(__fastcall*)();
using ErrorTextFn = U64(__fastcall*)(U64, const int*);
EquipFn g_equip = nullptr;
UnequipFn g_unequip = nullptr;
// The storage screen's move (0xd2c630 calls it): inventory, err, owner ids of the source / destination bag, item key,
// item+0xA, count (64-bit: the whole stack slot is read), source bag, source position, the source bag's transfer rule
// (its entry +4 = the destination bag). It only sends the request: the game checks it a moment later (and says why
// when it refuses), so whether a piece moved is seen in the bag afterwards.
using ItemMoveFn = int*(__fastcall*)(U64, int*, U32, U32, short, U16, U64, U16, U16, U32);
using BagInfoFn = U64(__fastcall*)(U64, U32); // (unused, key)
ItemMoveFn g_item_move = nullptr;
BagInfoFn g_bag_info = nullptr;

// Companions (actor kind != 1) only change gear through the game's UI requests (the wheel's / inventory's path):
// equip takes an item copy {200-byte item, u16 +0xc8, u16 bag position +0xca} (its bag comes from the item type).
using EquipRequestFn = int*(__fastcall*)(U64, int*, const void*, short);
using UnequipRequestFn = int*(__fastcall*)(U64, int*, short);
EquipRequestFn g_equip_request = nullptr;
UnequipRequestFn g_unequip_request = nullptr;
InventoryFn g_inventory = nullptr;
ErrorMgrFn g_error_mgr = nullptr;
ErrorTextFn g_error_text = nullptr;
using ToastFn = void(__fastcall*)(U64, const char*, U32);
ToastFn g_toast = nullptr;
using FindViewFn = U64(__fastcall*)(U64, const char*);
using OpenTextModalFn = U64(__fastcall*)(U64, U64, unsigned char*, int);
using GetTextFn = void(__fastcall*)(U64, char*, int);
using NameInvalidFn = bool(__fastcall*)(const char*);
using ModalClickFn = U64(__fastcall*)(U64, U64, U64, U64);
FindViewFn g_find_view = nullptr;
VoidFn g_params_init = nullptr;
VoidFn g_params_free = nullptr;
OpenTextModalFn g_open_text_modal = nullptr; // null: no rename prompt (names stay "Outfit N")
GetTextFn g_get_text = nullptr;
NameInvalidFn g_name_invalid = nullptr;
VoidFn g_hide_script = nullptr;
ModalClickFn g_text_click_orig = nullptr;
using OpenCommonModalFn = U64(__fastcall*)(U64, U64, unsigned char*, const char*);
OpenCommonModalFn g_open_common_modal = nullptr; // null: no missing-pieces question (copies are not offered)
ModalClickFn g_ask_click_orig = nullptr;

using InternFn = void(__fastcall*)(U64*, const char*, U32, U32);
using VecAddFn = bool(__fastcall*)(U64, const U32*);
using VecRemoveFn = U32(__fastcall*)(U64, const U32*);
InternFn g_intern = nullptr;
VecAddFn g_vec_add = nullptr;
VecRemoveFn g_vec_remove = nullptr;
VoidFn g_vec_sort = nullptr;
VoidFn g_restyle = nullptr;

// The injected record: a copy of the template record pointing at our own slot array.
alignas(16) std::uint8_t g_fake_rec[kRecordSize];
alignas(16) std::uint8_t g_fake_slots[kOutfitSlots * kSlotSize];
std::atomic<U32> g_fake_key{0xFFFF}; // record index served from g_fake_rec (0xFFFF = none)
U64 g_outfits_root = 0; // the menu the Outfits tab was injected into (build_hook)

// Saved outfits (game thread only, besides the load before the hooks go in).
struct OutfitItem {
    U16 slot; // equip slot id
    U16 key;  // item key
    U64 uid;  // item instance (kept for equipping later)
    std::uint8_t look = 0; // Appearance (a Look)
    U32 pid = 0;           // the item's permanent id (a key is a row of the game's table: patches renumber keys, not these)
};
// Appearance per piece. Default leaves it to the game; the others decide above the game's own hiding (our layer 6).
enum Look : std::uint8_t { kLookDefault, kLookHidden, kLookCombat, kLookPeace, kLookShown, kLookCount };
// The order the looks are listed / scrolled in (user): default, hidden, shown, shown in combat, hidden in combat.
constexpr std::uint8_t kLookOrder[kLookCount] = {kLookDefault, kLookHidden, kLookShown, kLookCombat, kLookPeace};
std::uint8_t look_pos(std::uint8_t look)
{
    for (std::uint8_t k = 0; k < kLookCount; ++k)
        if (kLookOrder[k] == look) return k;
    return 0;
}
struct Outfit {
    std::string name;
    std::string chest_icon; // icon texture of the chest piece ("" = none)
    std::vector<OutfitItem> items; // armor slots, sorted by slot
    // Highest item uid owned when the outfit was last seen complete (0 = never). The game gives every item it hands
    // out a new, higher uid (a dropped piece picked up again too), so a copy above this is the piece come back.
    U64 seen = 0;
    // The Weapons wheel is saved with the armour (empty slots too). has_weapons: its slots are in `items` (outfits
    // saved before weapons were have none: loading leaves the weapons alone until Overwrite); load_weapons: the
    // outfit's "Weapons: Load / Leave" (Leave: loading and matching ignore the weapon slots).
    bool has_weapons = false;
    bool load_weapons = true;
    std::uint8_t climate = 0; // Conditions: 0 Auto (from its resistance), 1 Cold, 2 Heat (manual), 3 Off (a badge only)
    std::uint8_t stash = 0;   // Conditions: Stash, 0 = the global setting, 1 on, 2 off
    // Its hotkey (the outfit's settings, 2026-11-06): a key or mouse button (virtual-key code; 0 = none) and the
    // modifiers held with it (kModCtrl / kModAlt / kModShift; none: the key alone). Pressed in play, the outfit loads.
    U16 hk_vk = 0;
    std::uint8_t hk_mods = 0;
    // The character's last loaded outfit (one per character, kept per save): its pieces' looks apply wherever the exact
    // piece is worn in its slot, also after other slots were changed by hand.
    bool worn = false;
    // The shield the character always carries on its back (equipment-23: it shows whatever the shield slot holds; on
    // no wheel, so in no outfit): its look in this outfit (a Look), applied to whatever is in that slot.
    std::uint8_t back_look = 0;
    // Whether that slot held anything when the outfit was saved (it is empty while the shield slot holds a shield):
    // 0 unknown (saved before this was kept), 1 yes, 2 no. The eye view lists the back shield only when it can matter.
    std::uint8_t has_back = 0;
    // Looks of the slots the outfit holds nothing in (a look belongs to the slot: it applies to whatever is worn there).
    std::map<U16, std::uint8_t> empty_looks;
};
constexpr U16 kBackShieldSlot = 23;
std::vector<Outfit> g_outfits; // the current character's outfits (what the wheel shows)
std::wstring g_store_path;
U16 g_armor_slots[kOutfitSlots]; // equipment-N category per Armor wheel slot (template record order)
U32 g_armor_slot_count = 0;
constexpr U32 kMaxWeaponSlots = 16;
U16 g_weapon_slots[kMaxWeaponSlots]; // the Weapons wheel's (record 0); 0xFFFF = a wheel slot with no equip slot
std::atomic<U32> g_weapon_slot_count{0};
constexpr U32 kWornMax = kOutfitSlots + kMaxWeaponSlots; // worn entries: armour, then weapons

const OutfitItem* outfit_piece(const Outfit& o, U16 slot); // (the outfit's piece in a slot, or nullptr)
bool is_weapon_slot(U16 slot)
{
    for (U32 i = 0; i < g_weapon_slot_count; ++i)
        if (g_weapon_slots[i] == slot) return true;
    return false;
}
bool uses_weapons(const Outfit& o) { return o.has_weapons && o.load_weapons; }
// The outfit's pieces loading puts on (the weapon ones only when it loads weapons).
bool loads_piece(const Outfit& o, U16 slot) { return uses_weapons(o) || !is_weapon_slot(slot); }

// Every character's outfits; g_outfits is the working copy of g_store[g_character].
// kNoCharacter holds outfits from files written before the lists were per character (adopted by the first
// character the menu opens for).
constexpr U16 kNoCharacter = 0xFFFF;
struct CharacterOutfits {
    U16 character;
    std::vector<Outfit> outfits;
};
using Store = std::deque<CharacterOutfits>; // deque: references stay valid when a character is added
Store g_store;
U16 g_character = kNoCharacter;

std::atomic<U32> g_outfit_count{0}; // == g_outfits.size()

// --- Appearance view: the wheel shows one saved outfit's pieces ------------------------------------
// Entered from the outfit settings' Visibility entry. Its pieces run clockwise from the top like the outfits, then the
// page slot and Back (left of the top, where the "+" slot is). The page slot's dots choose Armour / Weapons /
// Accessories (scroll it), a click opens the page; Back returns to the settings. Scrolling a piece moves its look on.
// Closing the menu leaves it.
int g_eye = -1;             // the outfit shown (-1: the outfits)
int g_cfg = -1;             // the settings view's outfit (the saved count: the "+" slot's, the plugin's; -1: none)
std::vector<std::uint8_t> cfg_entries();
std::vector<const char*> cfg_values(std::uint8_t k, U32& cur);
std::string cfg_value_text(std::uint8_t k, const char* v);
const char* cfg_name(std::uint8_t k);
std::string cfg_description(std::uint8_t k);
std::string cfg_title(std::uint8_t k);
std::uint8_t g_eye_step = 0; // 0 Armour, 1 Weapons, 2 Accessories
std::uint8_t g_eye_pick = 0; // the page slot's dots: scrolled to choose, a click opens it (g_eye_step follows)
constexpr std::uint8_t kEyeSteps = 3;

// The Armor wheel's 12 slots don't fit one page with the page slot: its accessories get their own. Equipment slots
// (named from the pieces in them): 7 / 8 earrings, 9 necklace, 10 / 11 rings, 17 circlet, 18 mask; the armour page
// keeps 3 hat, 4 chest, 5 gloves, 6 boots, 16 cloak.
constexpr U16 kAccessorySlots[] = {7, 8, 9, 10, 11, 17, 18};
bool is_accessory_slot(U16 slot) { return std::find(std::begin(kAccessorySlots), std::end(kAccessorySlots), slot) != std::end(kAccessorySlots); }

constexpr size_t kBackPiece = ~size_t(0); // (eye_pieces: the back shield, not one of the outfit's items)
U64 back_shield_item(); // (the worn item in the back shield's slot, or 0)

// The shown page's slots: every slot of its wheel, in slot order, held by the outfit or not (the Weapons page also the
// back shield).
std::vector<size_t> eye_pieces()
{
    std::vector<size_t> v;
    if (g_eye < 0 || size_t(g_eye) >= g_outfits.size()) return v;
    if (g_eye_step == 1) {
        for (U32 i = 0; i < g_weapon_slot_count; ++i)
            if (g_weapon_slots[i] != 0xFFFF) v.push_back(g_weapon_slots[i]);
    } else {
        for (U32 i = 0; i < g_armor_slot_count; ++i)
            if (is_accessory_slot(g_armor_slots[i]) == (g_eye_step != 0)) v.push_back(g_armor_slots[i]);
    }
    std::sort(v.begin(), v.end());
    if (g_eye_step == 1) v.push_back(kBackPiece);
    return v;
}

bool outfit_has_slot(const Outfit& o, U16 slot)
{
    for (const OutfitItem& it : o.items)
        if (it.slot == slot) return true;
    return false;
}

// The look of a slot in an outfit (the slot's look, whatever is worn there).
std::uint8_t slot_look(const Outfit& o, U16 slot)
{
    for (const OutfitItem& it : o.items)
        if (it.slot == slot) return it.look;
    auto f = o.empty_looks.find(slot);
    return f != o.empty_looks.end() ? f->second : std::uint8_t(kLookDefault);
}

// A slot of the view: the outfit's item there; nothing saved there: an empty item with the slot's look; (kBackPiece)
// the back shield as an item: what its slot holds now (for the icon and name) with the outfit's back look.
template <class T>
bool rd(U64 addr, T& out);
const OutfitItem* eye_item(size_t idx, OutfitItem& tmp)
{
    Outfit& o = g_outfits[size_t(g_eye)];
    if (idx != kBackPiece) {
        for (const OutfitItem& it : o.items)
            if (it.slot == idx) return &it;
        tmp = {static_cast<U16>(idx), 0xFFFF, 0, slot_look(o, static_cast<U16>(idx))};
        return &tmp;
    }
    U64 e = back_shield_item();
    U16 key = 0xFFFF;
    U64 uid = 0;
    if (e) rd(e + 8, key), rd(e, uid);
    tmp = {kBackShieldSlot, key, uid, o.back_look};
    return &tmp;
}
std::uint8_t& eye_look(size_t idx)
{
    Outfit& o = g_outfits[size_t(g_eye)];
    if (idx == kBackPiece) return o.back_look;
    for (OutfitItem& it : o.items)
        if (it.slot == idx) return it.look;
    return o.empty_looks[static_cast<U16>(idx)];
}

// Saved outfits plus one empty "+" slot, until all 12 are used; in the Appearance view the pieces, the page slot and
// Back (the fullest page, Weapons: 8 slots and the back shield, 11 in all).
U32 visible_slots()
{
    if (g_eye >= 0) return static_cast<U32>(eye_pieces().size()) + 2;
    if (g_cfg >= 0) return static_cast<U32>(cfg_entries().size());
    return g_outfit_count < kOutfitSlots ? g_outfit_count + 1 : kOutfitSlots;
}

// Visible slot i sits at (i+1)*360/N, so slot N-1 is the top one: outfits start there and run clockwise,
// which puts the "+" slot (outfit index = saved count) last, just left of the top.
U32 slot_outfit(U32 i) { return (i + 1) % visible_slots(); }

// --- Safe memory access -------------------------------------------------------------------------
bool plausible(U64 p) { return p > 0x10000 && p < kUserMax; }

template <class T>
bool rd(U64 addr, T& out)
{
    __try {
        out = *reinterpret_cast<volatile T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool rd_ptr(U64 addr, U64& out) { return rd(addr, out) && plausible(out); }

U64 rp(U64 addr)
{
    U64 v = 0;
    return rd_ptr(addr, v) ? v : 0;
}

U32 r32(U64 addr)
{
    U32 v = 0xFFFFFFFF;
    rd(addr, v);
    return v;
}

// Copies a C string from game memory (SEH-guarded).
std::string cstr(U64 p, size_t max = 120)
{
    std::string s;
    if (!plausible(p)) return s;
    for (size_t i = 0; i < max; ++i) {
        char c = 0;
        if (!rd(p + i, c) || !c) break;
        s += c;
    }
    return s;
}

std::string hex(U64 p, size_t n)
{
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        std::uint8_t v = 0;
        if (!rd(p + i, v)) return s + "??";
        std::snprintf(b, sizeof b, "%02x", v);
        if (i && i % 8 == 0) s += ' ';
        s += b;
    }
    return s;
}

// --- UI elements --------------------------------------------------------------------------------
// Element: +0x20 class holder {element*, vector<u32 classId>*}. Script object: +8 element.
std::string class_name(U32 id)
{
    U64 tbl = rp(g_base + kRvaClassNames);
    U64 arr = tbl ? rp(tbl + 0x58) : 0;
    std::string n = arr ? cstr(rp(arr + U64(id) * 0x10)) : std::string();
    if (n.empty()) {
        char b[16];
        std::snprintf(b, sizeof b, "#%u", id);
        n = b;
    }
    return n;
}

bool is_element(U64 e)
{
    U64 holder = rp(e + 0x20);
    return holder && rp(holder) == e;
}

std::string classes(U64 elem)
{
    U64 holder = rp(elem + 0x20);
    U64 vec = holder ? rp(holder + 8) : 0;
    U64 data = vec ? rp(vec) : 0;
    U32 n = vec ? r32(vec + 8) : 0;
    if (!data || n > 64) return "(no classes)";
    std::string s;
    for (U32 i = 0; i < n; ++i) {
        if (i) s += ' ';
        s += class_name(r32(data + i * 4ull));
    }
    return s;
}

// Describes a pointer that is either an element or a script object owning one.
std::string describe(U64 p)
{
    char b[64];
    if (is_element(p)) {
        std::snprintf(b, sizeof b, "elem %llX flags b0=%02X b1=%02X: ", p, r32(p + 0xb8) & 0xFF, r32(p + 0xb9) & 0xFF);
        return b + classes(p);
    }
    U64 e = rp(p + 8);
    if (e && is_element(e)) {
        std::snprintf(b, sizeof b, "script %llX vt=+%llX elem %llX b0=%02X: ", p, rp(p) - g_base, e,
                      r32(e + 0xb0) & 0xFF);
        return b + classes(e);
    }
    std::snprintf(b, sizeof b, "? %llX: ", p);
    return b + hex(p, 0x20);
}

// --- Dumps --------------------------------------------------------------------------------------
std::string str_id(U64 at)
{
    U16 id = 0xFFFF;
    if (!rd(at, id)) return "?";
    U64 si = g_str_get(0, id);
    std::string s = si ? cstr(rp(rp(si + 0x10))) : std::string();
    char b[16];
    std::snprintf(b, sizeof b, "[%u]", id);
    return b + s;
}

void dump_quickslotinfo()
{
    U64 mgr = rp(g_base + kRvaQuickSlotInfoMgr);
    U32 count = mgr ? r32(mgr + 8) : 0;
    LOG("QuickSlotInfo mgr %llX: %u records", mgr, count);
    for (U32 i = 0; i < count && i < 16; ++i) {
        U16 key = static_cast<U16>(i);
        U64 rec = g_qsi_get(0, key);
        if (!rec) continue;
        U32 slots = r32(rec + kRecSlotCount);
        U64 sarr = rp(rec + kRecSlots);
        LOG("  rec %u @%llX slots=%u head: %s", i, rec, slots, hex(rec, 0x40).c_str());
        for (U32 s = 0; s < slots && s < 16 && sarr; ++s) {
            U64 so = sarr + s * 0x50ull;
            LOG("    slot %u widget=%s comp=%s raw: %s", s, str_id(so + 2).c_str(), str_id(so + 4).c_str(),
                hex(so, 0x50).c_str());
        }
    }
}

void dump_menu(U64 self, const char* why)
{
    LOG("---- RootQuickSlot %llX: %s", self, why);
    U64 tb = rp(self + kOffRootTabBar);
    if (tb) {
        U32 n = r32(tb + kOffTabHeaderCount);
        LOG("tabbar %llX headers=%u prev=%d cur=%d sel=%d default=%d flags120=%s", tb, n, r32(tb + 0x110),
            r32(tb + kOffTabCurrent), r32(tb + 0x11c), r32(tb + 0x118), hex(tb + 0x120, 8).c_str());
        U64 hv = rp(tb + kOffTabHeaders);
        for (U32 i = 0; i < n && i < 16 && hv; ++i) {
            U64 h = rp(hv + i * 8ull);
            LOG("  header %u e0=%u %s", i, r32(h + 0xe0) & 0xFF, describe(h).c_str());
        }
    }
    U32 bodies = r32(self + kOffRootBodyCount);
    U64 bv = rp(self + kOffRootBodies);
    LOG("bodies=%u  +230(defTab)=%d +284=%04X +286=%04X +288=%d +28c..291=%s", bodies, r32(self + 0x230),
        r32(self + 0x284) & 0xFFFF, r32(self + 0x286) & 0xFFFF, r32(self + 0x288), hex(self + 0x28c, 6).c_str());
    for (U32 i = 0; i < bodies && i < 16 && bv; ++i)
        LOG("  body %u %s", i, describe(rp(bv + i * 8ull)).c_str());
    U64 sv = rp(self + kOffRootSlotVecs);
    U32 tabs = r32(self + kOffRootSlotVecCount);
    for (U32 i = 0; i < tabs && i < 16 && sv; ++i) {
        U32 cnt = r32(sv + i * 0x10ull + 8);
        U64 arr = rp(sv + i * 0x10ull);
        LOG("  tab %u slot scripts=%u first: %s", i, cnt, arr && cnt ? describe(rp(arr)).c_str() : "-");
    }
    for (U64 off : {0x1a0ull, 0x1a8ull, 0x1b0ull, 0x1c0ull, 0x1d0ull, 0x1b8ull}) {
        U64 p = rp(self + off);
        LOG("  +%llX: %s", off, p ? describe(p).c_str() : "null");
    }
    LOG("  elem: %s", describe(self).c_str());
}

// --- Outfits tab --------------------------------------------------------------------------------
// The count the body builder iterates: (***root + 8) -> +0x460 -> +8 (root depends on a TLS flag,
// so both candidates are adjusted).
U64 qsi_table(U64 root_rva)
{
    U64 p = rp(g_base + root_rva);
    p = p ? rp(p) : 0;
    p = p ? rp(p + 8) : 0;
    return p ? rp(p + 0x460) : 0;
}

U64 __fastcall qsi_get_hook(U64 unused, U32 key)
{
    if (U16(key) == g_fake_key.load()) return reinterpret_cast<U64>(g_fake_rec);
    return g_qsi_get(unused, key);
}

// Equip slot ids of the template (Armor) record's slots, read before the fake slots are cleared.
void collect_armor_slots()
{
    g_armor_slot_count = 0;
    std::string ids;
    for (U32 i = 0; i < kOutfitSlots; ++i) {
        U16 s = *reinterpret_cast<U16*>(g_fake_slots + i * kSlotSize + kSlotEquipIdx);
        g_armor_slots[g_armor_slot_count++] = s; // index-aligned with the Armor tab's slot scripts
        ids += ' ' + std::to_string(s);
    }
    LOG("armor equip slots:%s", ids.c_str());
}

// Equip slot ids of the Weapons wheel (record 0, read like the Armor one): an outfit holds both wheels.
constexpr U16 kWeaponRecord = 0;

void collect_weapon_slots()
{
    U16 key = kWeaponRecord;
    U64 rec = g_qsi_get(0, key);
    U64 slots = rec ? rp(rec + kRecSlots) : 0;
    U32 n = rec ? r32(rec + kRecSlotCount) : 0;
    // (read on the UI thread and, at the loading screen, the game's: filled whole, then counted; the same values)
    U16 got[kMaxWeaponSlots];
    U32 count = 0;
    std::string ids;
    for (U32 i = 0; slots && i < n && i < kMaxWeaponSlots; ++i) {
        U16 s = 0xFFFF;
        if (!rd(slots + i * kSlotSize + kSlotEquipIdx, s)) break;
        got[count++] = s;
        ids += ' ' + std::to_string(s);
    }
    for (U32 i = 0; i < count; ++i) g_weapon_slots[i] = got[i];
    g_weapon_slot_count = count;
    static bool logged_none = false; // (not there yet: once)
    if (g_weapon_slot_count || !logged_none) LOG("weapon equip slots (record %u, %u slots):%s", key, n, ids.c_str());
    logged_none |= !g_weapon_slot_count;
}

// Copies the template record (Armor) into g_fake_rec / g_fake_slots; the slots get category "none"
// (the binder then attaches no item and the refresh leaves them alone).
bool prepare_fake_record()
{
    U16 key = kTemplateRecord;
    U64 rec = g_qsi_get(0, key);
    U64 slots = rec ? rp(rec + kRecSlots) : 0;
    if (!slots || r32(rec + kRecSlotCount) != kOutfitSlots) {
        LOG("template record %u unusable (rec %llX slots %u)", key, rec, rec ? r32(rec + kRecSlotCount) : 0);
        return false;
    }
    __try {
        std::memcpy(g_fake_rec, reinterpret_cast<void*>(rec), kRecordSize);
        std::memcpy(g_fake_slots, reinterpret_cast<void*>(slots), sizeof g_fake_slots);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("template record copy faulted");
        return false;
    }
    // The Armor and Weapons wheels' equip slots decide which worn items an outfit stores.
    collect_armor_slots();
    collect_weapon_slots();
    U64 own = reinterpret_cast<U64>(g_fake_slots);
    std::memcpy(g_fake_rec + kRecSlots, &own, sizeof own);
    U64 none = reinterpret_cast<U64>(&g_none_category);
    for (U32 i = 0; i < kOutfitSlots; ++i) {
        std::uint8_t* s = g_fake_slots + i * kSlotSize;
        std::memcpy(s + kSlotNameHolder, &none, sizeof none);
        *reinterpret_cast<U32*>(s + kSlotHash) = 0;
        *reinterpret_cast<U16*>(s + kSlotEquipIdx) = 0xFFFF;
    }
    return true;
}

// --- Type checks --------------------------------------------------------------------------------
// Every object reached through a struct offset must have its class. A pointer read at a moved offset practically
// never has the expected vtable, so a failed check means the game's layout changed in a way the offsets did not
// follow: the mod then stays off (no Outfits tab) and says so once.
bool has_vt(U64 obj, U64 vt_rva) { return vt_rva && obj && rp(obj) == g_base + vt_rva; }

std::string g_layout_bad; // what failed ("" = fine)
bool g_layout_toast_done = false;

// At build time (the bodies are about to be made): the menu and its tab bar.
bool layout_ok_build(U64 self)
{
    U64 tb = rp(self + kOffRootTabBar);
    U32 n = tb ? r32(tb + kOffTabHeaderCount) : 0;
    const char* why = !has_vt(self, kRvaRootQuickSlotVt) ? "menu"
                      : !has_vt(tb, kRvaTabBarVt)       ? "tab bar"
                      : !n || n > 16 || !has_vt(rp(rp(tb + kOffTabHeaders)), kRvaHeaderVt) ? "tab headers"
                                                                                         : nullptr;
    if (why && g_layout_bad.empty()) {
        g_layout_bad = why;
        LOG("layout check failed at build: %s (menu %llX, tab bar %llX, %u headers); mod off", why, self, tb, n);
    }
    return !why;
}

// When the menu opens (everything built): also the centre tooltip, cursor, bodies and the Armor slot scripts.
bool layout_ok(U64 self)
{
    static U64 checked = 0;
    if (!g_layout_bad.empty()) return false;
    if (checked == self) return true;
    if (!layout_ok_build(self)) return false;
    U64 bodies = rp(self + kOffRootBodies), sv = rp(self + kOffRootSlotVecs);
    U32 nb = r32(self + kOffRootBodyCount), ns = r32(self + kOffRootSlotVecCount);
    const char* why = !has_vt(rp(self + kOffRootCenterTip), kRvaCenterTipVt) ? "centre tooltip"
                      : !has_vt(rp(self + kOffRootCursor), kRvaRadialCursorVt) ? "radial cursor"
                      : !nb || nb > 16 || !has_vt(rp(bodies), kRvaBodyVt)   ? "tab bodies"
                      : ns <= kTemplateRecord || ns > 16 ||
                              !has_vt(rp(rp(sv + kTemplateRecord * 0x10ull)), kRvaEquipSlotVt) ? "armor slots"
                                                                                               : nullptr;
    if (why) {
        g_layout_bad = why;
        LOG("layout check failed: %s (bodies %u, slot tabs %u); mod off", why, nb, ns);
        return false;
    }
    checked = self;
    LOG("layout check passed (menu %llX)", self);
    return true;
}

U64 __fastcall build_hook(U64 self, U64 out, U64 a, U64 b)
{
    U64 tables[2] = {qsi_table(kRvaQsiTableRoot0), qsi_table(kRvaQsiTableRoot1)};
    U64 tb = rp(self + kOffRootTabBar);
    U32 headers = tb ? r32(tb + kOffTabHeaderCount) : 0;
    U32 records = tables[0] ? r32(tables[0] + 8) : 0;
    LOG("build bodies: self %llX tabbar %llX headers=%u records=%u tables %llX %llX", self, tb, headers, records,
        tables[0], tables[1]);

    // Only inject when the thtml has exactly one extra header (= the browser mod is active).
    bool inject = records == 4 && headers == records + 1 && layout_ok_build(self) && prepare_fake_record();
    if (!inject) {
        LOG("  no Outfits tab injected%s", g_ui_tab_added ? "" : " (the gear wheel's UI files got no Outfits header at load: see \"gear wheel UI\" above)");
        g_fake_key = 0xFFFF; // (an earlier build's record must not be served for this one)
        return g_build_orig(self, out, a, b);
    }
    g_fake_key = records;
    g_outfits_root = self; // (only this menu has the Outfits tab: every other one stays the game's)
    U32 bumped[2] = {};
    for (int i = 0; i < 2; ++i) {
        if (!tables[i] || (i == 1 && tables[1] == tables[0])) continue;
        U32* cnt = reinterpret_cast<U32*>(tables[i] + 8);
        if (*cnt != records) continue;
        *cnt = records + 1;
        bumped[i] = 1;
    }
    U64 r = g_build_orig(self, out, a, b);
    for (int i = 0; i < 2; ++i)
        if (bumped[i]) *reinterpret_cast<U32*>(tables[i] + 8) = records;
    LOG("  injected record %u; result %u, bodies now %u", records, r32(out), r32(self + kOffRootBodyCount));
    return r;
}

// Depth-first search for the first element below `elem` carrying class `cls`.
U64 find_by_class(U64 elem, const char* cls, int max_depth = 8, int depth = 0)
{
    if (!elem || depth > max_depth) return 0;
    U64 holder = rp(elem + 0x20);
    U64 vec = holder ? rp(holder + 8) : 0;
    U64 data = vec ? rp(vec) : 0;
    U32 n = vec ? r32(vec + 8) : 0;
    for (U32 i = 0; data && i < n && i < 64; ++i)
        if (class_name(r32(data + i * 4ull)) == cls) return elem;
    U64 kids = rp(elem + 0x40);
    U32 count = r32(elem + 0x48);
    for (U32 i = 0; kids && i < count && i < 64; ++i)
        if (U64 hit = find_by_class(rp(kids + i * 8ull), cls, max_depth, depth + 1)) return hit;
    return 0;
}

// The element whose child has class `cls` (for elements with no class of their own).
U64 find_parent_of_class(U64 elem, const char* cls, int max_depth = 8, int depth = 0)
{
    if (!elem || depth > max_depth) return 0;
    U64 kids = rp(elem + 0x40);
    U32 count = r32(elem + 0x48);
    for (U32 i = 0; kids && i < count && i < 64; ++i) {
        U64 k = rp(kids + i * 8ull);
        if (k && find_by_class(k, cls, 0) == k) return elem;
        if (U64 hit = find_parent_of_class(k, cls, max_depth, depth + 1)) return hit;
    }
    return 0;
}

bool g_label_logged = false;

// The header script's "show" virtual (+0x20), only on a tab header (CheckBox script), SEH-guarded.
void show_header(U64 h)
{
    U64 fn = has_vt(h, kRvaHeaderVt) ? rp(rp(h) + 0x20) : 0;
    if (!fn) {
        LOG("Outfits header %llX is not a tab header script; not shown", h);
        return;
    }
    __try {
        reinterpret_cast<VoidFn>(fn)(h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("Outfits header: show faulted");
    }
}

// Header of the Outfits tab, or 0 if the tab was not injected.
U64 outfits_header(U64 self)
{
    U32 idx = g_fake_key.load();
    if (idx == 0xFFFF || !layout_ok(self)) return 0;
    U64 tb = rp(self + kOffRootTabBar);
    U64 hv = tb ? rp(tb + kOffTabHeaders) : 0;
    U32 n = tb ? r32(tb + kOffTabHeaderCount) : 0;
    return hv && n > idx ? rp(hv + idx * 8ull) : 0;
}

// The header has no localstring in the thtml; its text is set here.
void set_label(U64 h)
{
    U64 text = find_by_class(rp(h + 8), "quickslot-tabbar-header-text");
    if (text) g_set_text(text, lang::t("tab.outfits"), 0);
    if (!g_label_logged) {
        g_label_logged = true;
        LOG("Outfits header %s, label elem %llX", describe(h).c_str(), text);
    }
}

// Adds or removes a CSS class the way the game's scripts do (skipped while the element is mid-restyle).
bool set_class(U64 elem, const char* cls, bool on)
{
    U64 holder = rp(elem + 0x20);
    U64 vec = holder ? rp(holder + 8) : 0;
    if (!vec) return false;
    U64 pending = rp(rp(holder) + 0xa0);
    if (pending && rp(pending + 8)) return false;
    U64 id = 0;
    g_intern(&id, cls, 1, 0x2ffff);
    U32 cid = static_cast<U32>(id);
    if (!cid) return false;
    if (on) {
        if (!g_vec_add(vec, &cid)) return true; // already set
        g_vec_sort(vec);
    } else if (g_vec_remove(vec, &cid) == 0xFFFFFFFF) {
        return true; // not set
    }
    g_restyle(holder);
    return true;
}

// Replaces the numbered class previously applied to `elem` (tracked in `applied`) with `want` ("" = none).
bool swap_class(U64 elem, std::string& applied, const std::string& want)
{
    if (applied == want) return true;
    if (!applied.empty()) {
        if (!set_class(elem, applied.c_str(), false)) return false;
        applied.clear();
    }
    if (!want.empty()) {
        if (!set_class(elem, want.c_str(), true)) return false;
        applied = want;
    }
    return true;
}

bool has_class(U64 elem, const char* cls) { return elem && find_by_class(elem, cls, 0) == elem; }

// Sets an element's background image to a texture by name, the way item icons get theirs (f35fb0).
// 1 = set, 0 = no such texture, -1 = fault.
int set_image_raw(U64 self, U64 elem, const char* name)
{
    alignas(16) U64 var[2] = {g_base + kRvaEmptyString, 2};
    int r = -1;
    __try {
        g_str_assign(var, name);
        g_str_normalize(var);
        g_str_lower(var);
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        if (!g_texture_exists(*reinterpret_cast<U64*>(ui + kOffUiTextures), var)) {
            r = 0;
        } else {
            U64 prop = kPropBackgroundImage, flag = 0;
            g_set_style_prop(*reinterpret_cast<U64*>(elem + 0x20), &prop, var, &flag);
            r = 1;
        }
        g_str_destroy(var);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = -1;
    }
    return r;
}

// Saved outfits show their chest piece on the slot's item image (ItemCoolTimeIconOutfit).
struct SlotImage {
    U64 elem = 0;
    std::string icon; // last icon set on elem
    bool ok = false;
    bool shown = false; // should be visible
    bool missing = false; // greyed out (om-missing): the outfit lacks a piece
};
SlotImage g_slot_image[kOutfitSlots];
bool g_rehide_logged = false;
// --- Slot modes: scrolling cycles what an Outfits slot does --------------------------------------
// Like vanilla's item cycling (the dots under a slot): a slot cycles its action (a saved outfit: click loads; the "+"
// slot: click saves) / settings (click opens the settings view). The game's scroll handlers (slot vtable 181 / 182) are hooked; on our
// slots the mode changes instead. Modes stay per slot while the wheel is open and all reset when it closes.
enum SlotMode : std::uint8_t { kModeMain, kModeSettings, kModeConditions, kModeAppearance }; // (eye: saved outfits)
std::uint8_t g_slot_mode[kOutfitSlots] = {};
using ScrollFn = U64(__fastcall*)(U64);
ScrollFn g_scroll_prev_orig = nullptr, g_scroll_next_orig = nullptr;
using SelectorSelectFn = void(__fastcall*)(U64, U32, U64);
SelectorSelectFn g_selector_select = nullptr;
// The slot's badge (browser mod CSS), on its occupancy-chain element (unused by our slots), above the picture and the
// focus, where the game draws its own slot badge (the lantern's Auto arrows): om-plus-c (the "+" slot's "+"),
// om-cog-c / om-flag-c (settings / conditions mode); om-plus-big / om-cog-big for the centred style. The worn wedge
// is on the disable element, the lowest layer.
constexpr bool kPlusSlotCentred = false; // the "+" slot: true = an "add" button (no silhouette, big centred "+" / cog,
                                          // style D); false = silhouette, "+" / cog in the badges' corner (style A)
U64 g_badge_elem[kOutfitSlots] = {};
U64 g_low_elem[kOutfitSlots] = {};   // the disable element: the worn wedge
bool g_low_shown[kOutfitSlots] = {};
const char* g_badge_class[kOutfitSlots] = {}; // badge shown on slot i (nullptr: none)
std::string g_plus_icon; // the "+" slot's picture: the worn chest while no saved outfit is worn ("" : the silhouette)
void update_plus_icon(U64 self);
bool look_changing();
bool g_amber = false; // the anchor (the last loaded / exactly worn outfit) has been changed by hand: its highlight is amber
// Equipment slots the game refuses to empty ("There are no Items available to be equipped after removal": the main
// hand, the shield, the dagger, the lantern - seen; more are learned from that refusal and kept in the file): an outfit
// that leaves one empty still counts as worn whatever is in it.
std::set<U16> g_keep_slots = {0, 1, 12, 15};
constexpr int kErrNoItemAfterRemoval = 0x69BEDE43;
// g_keep_slots and g_placeholder_keys are changed on the UI thread and written into the outfit file by any thread
// (save writes are seen on the game's threads): changes and the file's copy hold this lock.
std::mutex g_keys_mu;
U64 g_mark_elem[kOutfitSlots] = {};  // the Appearance marker's element (the charge-count wrap)
bool g_mark_shown[kOutfitSlots] = {}; // the game re-hides it on focus changes: shown again like the badges


void reset_modes() { std::fill(std::begin(g_slot_mode), std::end(g_slot_mode), std::uint8_t(kModeMain)); }

// --- The game's depth menu (the lantern's Auto / On / Off / Back ring) -----------------------------------------------
// Ours is never opened (the settings and visibility views are on the wheel itself); while the game's own is open (the
// lantern's), its presses and sectors are the game's.
void notify(U64 self, const std::string& text);
U32 outfit_slot_index(U64 self, U64 script);
bool outfits_current(U64 self);
U64 focused_script(U64 self);

bool in_depth(U64 self)
{
    unsigned char v = 0;
    return kOffRootFocusIdx && rd(self + kOffRootFocusIdx, v) && v;
}

void save_outfits();
void publish_looks(const Store& s); // (Appearance)
U16 current_character(); // (the controlled character: below)
void mark_worn(int index);
extern bool g_stash;
std::atomic<U32> g_peek_key{0};           // the Outfit Arc's key (a virtual-key code; 0 = Off)
std::string hotkey_name(U32 vk, std::uint8_t mods); // (an outfit's hotkey as shown: below, with the key names)
// The current character's hotkeys for the input filters (another thread): vk | mods << 8 per outfit, 0 = none.
std::atomic<U32> g_hotkeys[16] = {};
std::atomic<bool> g_peek_pad{false};       // the Outfit Arc on controllers: hold L1 / LB, press the d-pad's down
void peek_key_load(const std::string& id); // (from its id in the outfits file)
const char* peek_key_id();                 // (its id to save; null: Off)
extern bool g_take_from_companions;
extern bool g_messages;
extern std::string g_language;
const char* stash_text();
bool companion_pull_ok();

struct Outfit;
std::string stash_description(const Outfit& o);
const char* climate_text(const Outfit& o);
void outfit_resistance(const Outfit& o, int& ice, int& fire);
const char* weapons_text(const Outfit& o);
const char* outfit_stash_text(const Outfit& o);

// The settings' descriptions (the settings view's centre caption), by group and entry: {title, description}.
enum Page { kPageNone, kPageSettings, kPageOutfit, kPageConditions, kPageSettings2 };
U32 g_depth_outfit = 0; // the saved outfit the conditions' texts are for

std::pair<std::string, std::string> entry_text(int page, U32 index)
{
    if (index == 2) return {lang::t("setting.back"), lang::t("desc.back")};
    if (page == kPageOutfit) {
        if (index == 0) return {lang::t("setting.rename"), lang::t("desc.rename")};
        if (index == 1) return {lang::t("setting.delete"), lang::t("desc.delete")};
    } else if (page == kPageSettings) {
        if (index == 3) return {"", lang::t(g_stash ? "desc.gstash.on" : "desc.gstash.off")};
        if (index == 0) return {"", lang::t(g_take_from_companions ? "desc.party.take" : "desc.party.leave")};
        if (index == 1) return {"", lang::t(g_messages ? "desc.messages.on" : "desc.messages.off")};
    } else if (page == kPageSettings2) {
        if (index == 0) return {lang::t("setting.clear"), lang::t("desc.clear")};
        if (index == 1) return {lang::t("setting.uninstall"), lang::t("desc.uninstall")};
    } else if (page == kPageConditions && g_depth_outfit < g_outfits.size()) {
        const Outfit& o = g_outfits[g_depth_outfit];
        if (index == 3) {
            if (o.climate == 1) return {"", lang::t("desc.climate.cold")};
            if (o.climate == 2) return {"", lang::t("desc.climate.heat")};
            if (o.climate == 3) return {"", lang::t("desc.climate.off")};
            int ice = 0, fire = 0;
            outfit_resistance(o, ice, fire);
            return {"", lang::f("desc.climate.auto", {{"ice", std::to_string(ice)}, {"fire", std::to_string(fire)}})};
        }
        if (index == 0) {
            if (!o.has_weapons) return {"", lang::t("desc.weapons.none")};
            return {"", lang::t(o.load_weapons ? "desc.weapons.load" : "desc.weapons.leave")};
        }
        if (index == 1) return {"", stash_description(o)};
    }
    return {"", ""};
}

// Set while the plugin itself is calling into the game (pump, prompt answers): callbacks the game fires
// synchronously meanwhile (focus, frame getter, cursor) must not start another round of work.
int g_busy = 0;
struct Busy {
    Busy() { ++g_busy; }
    ~Busy() { --g_busy; }
};

int g_force_images = 0; // frames left to re-set the chest textures (after focus changes / opening)

// Shows `icon` on slot i's item image ("" = hide it); false if nothing is shown.
bool show_slot_image(U64 self, U64 root, U32 i, const std::string& icon)
{
    U64 img = find_by_class(root, "quickslot-item-img");
    if (!img) return false;
    SlotImage& si = g_slot_image[i];
    if (!icon.empty() && (si.elem != img || si.icon != icon)) {
        int r = set_image_raw(self, img, icon.c_str());
        si.elem = img;
        si.icon = icon;
        si.ok = r == 1;
        LOG("slot %u image %s: %s (%s)", i, icon.c_str(), r == 1 ? "set" : r == 0 ? "no such texture" : "FAULT",
            describe(img).c_str());
    }
    bool on = !icon.empty() && si.ok;
    si.shown = on;
    set_class(img, "!cpp-none", !on);
    return on;
}

// The game resets a slot's item image when focus moves (it holds no item for our slots): put the shown
// chest images back. `force` also re-sets the texture (the reset may clear it without hiding the element).
void keep_mode_dots(U64 self);

void keep_slot_images(U64 self, bool force)
{
    keep_mode_dots(self);
    for (U32 i = 0; i < kOutfitSlots; ++i) {
        SlotImage& si = g_slot_image[i];
        if (!si.shown || !si.elem || !is_element(si.elem)) continue;
        bool hidden = has_class(si.elem, "!cpp-none");
        if (!hidden && !force) continue;
        if (hidden && !g_rehide_logged) {
            g_rehide_logged = true;
            LOG("slot %u image was hidden by the game (%s)", i, describe(si.elem).c_str());
        }
        if (set_image_raw(self, si.elem, si.icon.c_str()) != 1) continue;
        set_class(si.elem, "!cpp-none", false);
        set_class(si.elem, "om-missing", si.missing);
        set_class(si.elem, "om-present", !si.missing);
    }
    for (U32 i = 0; i < kOutfitSlots; ++i) { // badges and the worn wedge, if the game re-hides their elements
        if (g_badge_class[i] && is_element(g_badge_elem[i]) && has_class(g_badge_elem[i], "!cpp-none"))
            set_class(g_badge_elem[i], "!cpp-none", false);
        if (g_low_shown[i] && is_element(g_low_elem[i]) && has_class(g_low_elem[i], "!cpp-none"))
            set_class(g_low_elem[i], "!cpp-none", false);
        if (g_mark_shown[i] && is_element(g_mark_elem[i]) && has_class(g_mark_elem[i], "!cpp-none"))
            set_class(g_mark_elem[i], "!cpp-none", false);
    }
}

// Visible slots share the whole wheel (browser mod CSS, generated by tools/omring_gen.py): om-list-N on
// the tab body turns its radial layout off (it misplaces slots 1+ at 360/N, and turning it back on for 12
// leaves stale positions) and om-sN-i places slot i at (i+1)*360/N. Below 12, om-ring-N gives the ring N
// segments (12 keeps the vanilla ring), and om-fN-i turns slot i's focus into its 360/N wedge (for every N, so
// all counts share the comrades-wheel focus look).
U32 wheel_n() { return visible_slots() < kOutfitSlots ? visible_slots() : 0; }

std::string numbered(const char* fmt, U32 a, U32 b = 0)
{
    char s[32];
    std::snprintf(s, sizeof s, fmt, a, b);
    return s;
}

std::string g_list_class;
std::string g_slot_class[kOutfitSlots];
std::string g_focus_class[kOutfitSlots];
std::string g_worn_class[kOutfitSlots];
std::string g_placeholder_class[kOutfitSlots]; // the Visibility view: an empty slot's icon (the game's own for that slot)

// The saved outfit the character is wearing (-1: none) gets the game's "selected" wedge in its segment (om-wN-i on
// the slot's otherwise unused reserve element, below the focus) and the "Equipped" tag in the centre tooltip.
int g_worn_outfit = -1;
bool g_worn_changed = false; // the centre tooltip's tag follows on the next pump
int find_worn_outfit(U64 self);
// Outfits with a saved piece that is gone (no other copy to offer) are greyed out (om-missing, browser mod CSS).
bool refresh_missing(U64 self);
bool outfit_missing(size_t o);

void show_mode_dots(U64 script, U32 count, U32 mode);
// The select shows the dots in pages of +0xf4 (the dots per page; only the lit dot's page is drawn). It stays at the
// last view's count (closing the wheel keeps it too), so ours is written with the count: one page. (Closing the wheel
// in the settings / Visibility also left the dots of the next opening's changes unshown: close_hook puts the slots back
// to the outfits first.) Flag 1 skips the select's sound; 0 plays it, kept for a scroll, where the lit dot moves.
U32 mode_count(U32 i);
U32 mode_lit(U32 i);

// Outfits tab: saved outfits, then one empty "+" slot (until all 12 are used); the rest hidden.
bool g_slots_logged = false;
U32 g_applied_visible = 0; // visible slot count the classes were last set for

// only: just that slot (a scroll changes one slot; re-styling them all made the other badges flicker).
// An item's Ice / Fire Resistance level: its passive list (item info +0x118 -> {entries of {u32 kind, u32 level}*,
// u32 count}). The kind's low 16 bits are the passive's id (the high 16 change per session: a runtime handle): 0x704
// Fire Resistance, 0x705 Ice, 0x706 Lightning; the others are other passives (0x70a fishing cooldown, 0x6xx ...).
// Levels add up over the worn pieces; 5 / 10 / 15 each cancel one of the cold / heat gauge's three bars, 15 also gives
// immunity to Freeze / Burn. Item data never changes in a session: cached per key.
using ItemInfoFn = U64(__fastcall*)(U64, U32); // (unused, key)
extern ItemInfoFn g_item_info; // (item key -> item info; set by install)
std::string item_name(U16 key); // "" if unknown
U32 kPassiveFire = 0x74e, kPassiveIce = 0x74f; // (0x704 / 0x705 before 2026-10-30: Kuku armour Ice 10 = 0x74f, Ziggurat Lightning 1 = 0x750)
// Base stats (cloaks...): a second list of the same {kind, level}: info +0x188 -> {rows*, n}, row (0x80 each) +0x58 entries, +0x60 count
// (Plate Cloak of the Shadows: Fire Lv 7; Wolf Cloak: Ice Lv 5).
U32 kBaseFire = 0x72, kBaseIce = 0x73; // (0x68 / 0x69 before 2026-10-30: Scout Cloak Ice 3 = 0x73, Hungering Fang Lightning 7 = 0x74)
std::map<U16, std::pair<int, int>> g_item_resist; // key -> {ice, fire}

// The game's item table: a key is a row, and a patch that adds items renumbers the rows; the permanent id in each row
// stays. Only the first block is used: the rows from the data file, whose body offsets grow from row to row. After it the
// table has room for rows the game fills while it runs (they come and go, and repeat ids). Checked: ids all different.
U64 item_rows(U32* count = nullptr)
{
    static int state = 0; // 0 not checked, 1 fine, 2 not what it should be
    static U32 block = 0;
    U64 table = kRvaItemTable ? rp(g_base + kRvaItemTable) : 0;
    U64 rows = table ? rp(table + 0x28) : 0;
    U32 n = table ? r32(table + 8) : 0;
    if (!rows || !n) return 0; // (not loaded yet)
    if (!state) {
        std::set<U32> ids;
        U32 last = 0;
        for (U32 i = 0; i < n && i < 60000; ++i) {
            U32 id = r32(rows + i * 8ull), off = r32(rows + i * 8ull + 4);
            if (!id || (i && off <= last)) break;
            ids.insert(id);
            last = off;
            block = i + 1;
        }
        bool ok = n <= 60000 && block >= 100 && block * 2 >= n && ids.size() == block;
        state = ok ? 1 : 2;
        LOG("self-test: item table, %u rows, %u from the data file, %zu different ids: %s", n, block, ids.size(),
            ok ? "ok" : "FAILED (keys can't be repaired)");
    }
    if (state != 1) return 0;
    if (count) *count = block;
    return rows;
}

U32 pid_of(U16 key)
{
    U32 n = 0;
    U64 rows = item_rows(&n);
    return rows && key < n ? r32(rows + key * 8ull) : 0;
}

U16 key_of_pid(U32 pid)
{
    static std::map<U32, U16> keys;
    U32 n = 0;
    U64 rows = item_rows(&n);
    if (!rows) return 0xFFFF;
    if (keys.empty())
        for (U32 i = 0; i < n && i < 0xFFFF; ++i)
            if (U32 id = r32(rows + i * 8ull)) keys[id] = static_cast<U16>(i);
    auto f = keys.find(pid);
    return f != keys.end() ? f->second : U16(0xFFFF);
}



// Where an item's info keeps its stat lists (the 20261104 patch moved both by 8: 0x118 -> 0x120, 0x188 -> 0x190):
// found again at the first menu open from items whose values are known (calibrate_resist_ids).
U64 kOffItemPassives = 0x118; // -> {entries*, count} (null when none: the game's helper +2603A00)
U64 kOffItemStats = 0x188;    // -> {rows*, count}; a row's base stats at +0x58 / +0x60

// An item info's passive list (base false) or base stat list (true, from the first refinement row) at `off`.
std::pair<U64, U32> stat_list(U64 info, U64 off, bool base)
{
    if (!info) return {0, 0};
    if (base) {
        U64 stats = rp(info + off), row0 = stats ? rp(stats) : 0;
        return {row0 ? rp(row0 + 0x58) : 0, row0 ? r32(row0 + 0x60) : 0};
    }
    U64 p = rp(info + off);
    return {p ? rp(p) : 0, p ? r32(p + 8) : 0};
}

std::pair<int, int> item_resistance(U16 key)
{
    if (auto f = g_item_resist.find(key); f != g_item_resist.end()) return f->second;
    std::pair<int, int> r{0, 0};
    U16 k = key;
    U64 info = g_item_info ? g_item_info(0, k) : 0;
    auto [list, n] = stat_list(info, kOffItemPassives, false);
    auto [base, nb] = stat_list(info, kOffItemStats, true); // (the resistance is the same in every refinement row)
    auto sum = [&](U64 l, U32 cnt, U32 ice, U32 fire) {
        for (U32 i = 0; l && i < cnt && i < 16; ++i) {
            U32 kind = 0, level = 0;
            if (!rd(l + i * 8ull, kind) || !rd(l + i * 8ull + 4, level) || level > 100) continue;
            if ((kind & 0xFFFF) == ice) r.first += static_cast<int>(level);
            else if ((kind & 0xFFFF) == fire) r.second += static_cast<int>(level);
        }
    };
    sum(list, n, kPassiveIce, kPassiveFire);
    sum(base, nb, kBaseIce, kBaseFire);
    if (g_item_info) g_item_resist[key] = r; // (not cached before the lookup is there)
    if (r.first || r.second) LOG("resistance of item %u \"%s\": ice %d, fire %d", key, item_name(key).c_str(), r.first, r.second);
    return r;
}

// The ids of the resistance stats can renumber on a patch, and the lists can move in the item info: both are read from
// items whose values are known (found by permanent id). First the lists' offsets: for each kind (passive / base), the
// offset where every reference of that kind has an entry with its id and level (one offset only, else the built-in one
// stays). Then the ids: a reference counts when exactly one of its entries has the known level; references must agree;
// without that the built-in id stays.
void calibrate_resist_ids()
{
    struct Ref { U32 pid; U32 level; bool base; U32* id; };
    const Ref refs[] = {{0xF4B2F, 10, false, &kPassiveIce}, {0xF4B2E, 10, false, &kPassiveFire}, {0xF4EC4, 3, true, &kBaseIce},
                        {0xF43FC, 5, true, &kBaseIce},      {0xF43BF, 7, true, &kBaseFire}};
    for (bool base : {false, true}) {
        U64& off = base ? kOffItemStats : kOffItemPassives;
        std::vector<U64> hits;
        for (U64 o = 0x80; o < 0x300; o += 8) {
            int refs_of_kind = 0, matched = 0;
            for (const Ref& r : refs) {
                if (r.base != base) continue;
                ++refs_of_kind;
                U16 key = key_of_pid(r.pid);
                auto [list, n] = stat_list(key != 0xFFFF && g_item_info ? g_item_info(0, key) : 0, o, base);
                bool has = false;
                for (U32 i = 0; list && n <= 64 && i < n; ++i) {
                    U32 kind = 0, level = 0;
                    has |= rd(list + i * 8ull, kind) && rd(list + i * 8ull + 4, level) && level == r.level &&
                           (kind & 0xFFFF) == *r.id;
                }
                matched += has;
            }
            if (refs_of_kind && matched == refs_of_kind) hits.push_back(o);
        }
        if (hits.size() == 1 && hits[0] != off) LOG("self-test: the %s stat list moved: +%llX -> +%llX", base ? "base" : "passive", off, hits[0]);
        if (hits.size() == 1) off = hits[0];
        else LOG("self-test: the %s stat list found at %zu offsets; +%llX kept", base ? "base" : "passive", hits.size(), off);
    }
    std::map<U32*, U32> found;
    std::set<U32*> clash;
    int answered = 0;
    for (const Ref& r : refs) {
        U16 key = key_of_pid(r.pid);
        U64 info = key != 0xFFFF && g_item_info ? g_item_info(0, key) : 0;
        auto [list, n] = stat_list(info, r.base ? kOffItemStats : kOffItemPassives, r.base);
        U32 id = 0;
        int hits = 0;
        for (U32 i = 0; list && i < n && i < 16; ++i) {
            U32 kind = 0, level = 0;
            if (rd(list + i * 8ull, kind) && rd(list + i * 8ull + 4, level) && level == r.level) id = kind & 0xFFFF, ++hits;
        }
        if (hits != 1) continue;
        ++answered;
        if (auto f = found.find(r.id); f != found.end() && f->second != id) clash.insert(r.id);
        found[r.id] = id;
    }
    bool moved = false;
    for (const auto& [var, id] : found)
        if (!clash.count(var) && *var != id) {
            LOG("self-test: a resistance id moved: %X -> %X", *var, id);
            *var = id, moved = true;
        }
    if (moved) g_item_resist.clear();
    LOG("self-test: resistance ids from %d of %zu references (%zu disagree): passive ice %X fire %X, base ice %X fire %X "
        "(lists at +%llX / +%llX)", answered, std::size(refs), clash.size(), kPassiveIce, kPassiveFire, kBaseIce, kBaseFire,
        kOffItemPassives, kOffItemStats);
}

// Abyss gears belong to one COPY of an item: its sockets are at copy+0x60 -> 6-byte entries {u16 gear key (0xFFFF:
// empty), u16, u8 socket, u8}. A gear is an item too, its resistance in its own lists (Flameward II, key 3244: Fire
// Lv 2 as a passive). The copy is found by uid + key: worn, or in a bag (storage / wardrobe too). Re-read at each
// wheel open (gears can be put in / taken out between). Companions' worn copies are not searched (a piece they wear
// counts without its gears).
U64 equipment_list();
std::map<U64, std::pair<int, int>> g_gear_resist; // uid -> its gears' {ice, fire}

U64 find_copy(U64 uid, U16 key); // (an item copy by uid + key: worn or in a bag; defined with the bag code)

std::pair<int, int> gear_resistance(U64 uid, U16 key)
{
    if (auto f = g_gear_resist.find(uid); f != g_gear_resist.end()) return f->second;
    std::pair<int, int> r{0, 0};
    U64 copy = find_copy(uid, key);
    U64 sockets = copy ? rp(copy + 0x60) : 0;
    for (U32 i = 0; sockets && i < 5; ++i) {
        U16 gear = 0xFFFF;
        if (!rd(sockets + i * 6ull, gear) || gear == 0xFFFF || gear == 0) continue;
        auto [gi, gf] = item_resistance(gear);
        r.first += gi, r.second += gf;
    }
    g_gear_resist[uid] = r;
    if (r.first || r.second) LOG("abyss gears of %s (uid %llX): ice %d, fire %d", item_name(key).c_str(), uid, r.first, r.second);
    return r;
}

void outfit_resistance(const Outfit& o, int& ice, int& fire)
{
    ice = fire = 0;
    for (const OutfitItem& it : o.items) {
        if (!loads_piece(o, it.slot)) continue; // (weapons it leaves aren't worn by it)
        auto [i, f] = item_resistance(it.key);
        auto [gi, gf] = gear_resistance(it.uid, it.key);
        ice += i + gi, fire += f + gf;
    }
}

// The outfit's Climate badge: Auto from its resistance (Lv. 10 plain, Lv. 15 phase thermometer; the stronger side),
// or the manual Cold / Heat (plain), or none.
const char* climate_badge(const Outfit& o)
{
    if (o.climate == 1) return "om-cold-c";
    if (o.climate == 2) return "om-heat-c";
    if (o.climate == 3) return nullptr;
    int ice = 0, fire = 0;
    outfit_resistance(o, ice, fire);
    bool cold = ice >= fire;
    int lv = cold ? ice : fire;
    // (the plain thermometer is the FULL meter, the "phase" one the half: Lv 10-14 half, Lv 15+ full; user 23:4x)
    const char* badge = lv >= 15 ? (cold ? "om-cold-c" : "om-heat-c") : lv >= 10 ? (cold ? "om-cold2-c" : "om-heat2-c") : nullptr;
    static std::map<std::string, std::string> logged; // (once per outfit and result)
    std::string what = std::to_string(ice) + "/" + std::to_string(fire) + (badge ? badge : "-");
    if (logged[o.name] != what) {
        logged[o.name] = what;
        std::string pieces;
        for (const OutfitItem& it : o.items) {
            auto [i, f] = item_resistance(it.key);
            if (i || f) pieces += " " + item_name(it.key) + " (ice " + std::to_string(i) + ", fire " + std::to_string(f) + ")";
        }
        LOG("climate \"%s\" (auto): ice %d, fire %d -> %s;%s", o.name.c_str(), ice, fire, badge ? badge : "no badge",
            pieces.empty() ? " no resistant pieces" : pieces.c_str());
    }
    return badge;
}

const char* icon_name_of(U64 item)
{
    __try {
        return g_icon_name ? g_icon_name(item) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// A piece's icon texture: from its copy (worn or in a bag), else from a bare item with its key (not in the inventory).
std::string piece_icon(const OutfitItem& it)
{
    U64 copy = it.slot == kBackShieldSlot ? back_shield_item() : find_copy(it.uid, it.key);
    if (!copy && it.key == 0xFFFF) return {}; // (nothing saved there)
    alignas(16) unsigned char bare[200] = {};
    std::memcpy(bare, &it.uid, 8);
    std::memcpy(bare + 8, &it.key, 2);
    long long one = 1;
    std::memcpy(bare + 0x10, &one, 8);
    const char* name = icon_name_of(copy ? copy : reinterpret_cast<U64>(bare));
    return name ? cstr(reinterpret_cast<U64>(name)) : std::string();
}

const char* look_badge(std::uint8_t look)
{
    return look == kLookHidden ? "om-look-hidden-c" : look == kLookCombat ? "om-look-combat-c"
         : look == kLookPeace  ? "om-look-peace-c"  : look == kLookShown  ? "om-look-shown-c" : nullptr;
}

// The input in use (the last one touched: a pad, or the mouse / keyboard), set by key_thread; a pad's family picks its
// confirm icon. The game swaps its own prompts the same way.
std::atomic<bool> g_pad_in_use{false}, g_pad_is_ps{false}, g_prompt_changed{false};
const char* prompt_class()
{
    return !g_pad_in_use ? "om-prompt-c" : g_pad_is_ps ? "om-cross-c" : "om-abtn-c";
}

const char* const kBadges[] = {"om-plus-big", "om-cog-big", "om-plus-c", "om-cog-c", "om-flag-c", "om-cold-c", "om-heat-c",
                               "om-cold2-c", "om-heat2-c", "om-eye-c", "om-look-shown-c", "om-look-hidden-c",
                               "om-look-combat-c", "om-look-peace-c", "om-return-c", "om-prompt-c", "om-cross-c",
                               "om-abtn-c"};
// (g_badge_class keeps a pointer: into kBadges)
size_t badge_index(const char* c)
{
    for (size_t k = 0; k < std::size(kBadges); ++k)
        if (!std::strcmp(kBadges[k], c)) return k;
    return 0;
}

// Slot i of the Appearance view: a piece (its icon, its look's badge, dimmed when hidden or not in the inventory), the
// page slot (the silhouette of the page chosen) or Back (the label "Back" and the click prompt, as in the settings).
bool clear_label(U64 mark);
// A setting's value under its name, in the game's gold caption style: a second text in the slot's label wrap
// (OmValueText, added to the game's compiled slot by tools/uibin_tab.py; the name's text holds one colour only). The
// wrap shows with the label. "" hides it; without the element (the patch not mounted) nothing is shown.
// The game's key icon for a key (Common_Icon_Keyguide_PC_<name>), wide: a wide cap (120 px) instead of a square one;
// null: the game has none (the text is shown instead).
const char* key_icon(U32 vk, bool& wide)
{
    static char buf[64];
    wide = false;
    const char* n = nullptr;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        snprintf(buf, sizeof buf, "Common_Icon_Keyguide_PC_%c", char(vk));
        return buf;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        snprintf(buf, sizeof buf, "Common_Icon_Keyguide_PC_F%u", vk - VK_F1 + 1);
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(buf, sizeof buf, "Common_Icon_Keyguide_PC_num%u", vk - VK_NUMPAD0);
        return buf;
    }
    switch (vk) {
    case VK_CONTROL: n = "Ctrl"; wide = true; break;
    case VK_MENU: n = "Alt"; wide = true; break;
    case VK_SHIFT: n = "Shift"; wide = true; break;
    case VK_TAB: n = "Tab"; wide = true; break;
    case VK_CAPITAL: n = "Caps"; wide = true; break;
    case VK_BACK: n = "Backspace"; wide = true; break;
    case VK_INSERT: n = "Insert"; wide = true; break;
    case VK_DELETE: n = "Del"; wide = true; break;
    case VK_HOME: n = "Home"; wide = true; break;
    case VK_END: n = "End"; wide = true; break;
    case VK_PRIOR: n = "Pageup"; wide = true; break;
    case VK_NEXT: n = "Pagedown"; wide = true; break;
    case VK_SPACE: n = "Spacer"; wide = true; break;
    case VK_RETURN: n = "Enter"; break;
    case VK_LEFT: n = "Left"; break;
    case VK_RIGHT: n = "Right"; break;
    case VK_UP: n = "Up"; break;
    case VK_DOWN: n = "Down"; break;
    case VK_OEM_3: n = "Backquote"; break;
    case VK_OEM_MINUS: n = "Hyphen"; break;
    case VK_OEM_PLUS: n = "Plus"; break;
    case VK_OEM_4: n = "Lbracket"; break;
    case VK_OEM_6: n = "Rbracket"; break;
    case VK_OEM_5: n = "Backslash"; break;
    case VK_OEM_1: n = "Semicolon"; break;
    case VK_OEM_7: n = "Doublequotes"; break;
    case VK_OEM_COMMA: n = "Comma"; break;
    case VK_OEM_PERIOD: n = "Dot"; break;
    case VK_OEM_2: n = "Slash"; break;
    case VK_MULTIPLY: n = "num_Asterisk"; break;
    case VK_ADD: n = "num_Plus"; break;
    case VK_SUBTRACT: n = "num_Minus"; break;
    case VK_DECIMAL: n = "num_Dot"; break;
    case VK_DIVIDE: n = "num_Slash"; break;
    case VK_MBUTTON: n = "Mouse_Wheel"; break;
    case VK_XBUTTON1: n = "Mouse_x1"; break;
    case VK_XBUTTON2: n = "Mouse_x2"; break;
    default: return nullptr;
    }
    snprintf(buf, sizeof buf, "Common_Icon_Keyguide_PC_%s", n);
    return buf;
}

// The hotkey as key caps on the slot (OmHkCaps): false if the slot has none or a key has no icon (the text then).
// where: 1 under a plain anchor, 2 under the eye marker.
bool show_caps(U64 self, U64 root, U32 vk, std::uint8_t mods, int where)
{
    U64 box = find_by_class(root, "om-hk-caps");
    if (!box) return false;
    bool wide = false;
    const char* key = key_icon(vk, wide);
    if (!key) return set_class(box, "!cpp-none", true), false;
    const U32 mvk[3] = {VK_CONTROL, VK_MENU, VK_SHIFT};
    const std::uint8_t bit[3] = {1, 2, 4};
    for (int k = 0; k < 3; ++k) {
        U64 cap = find_by_class(box, ("om-hk-c" + std::to_string(2 * k)).c_str(), 1);
        U64 plus = find_by_class(box, ("om-hk-c" + std::to_string(2 * k + 1)).c_str(), 1);
        bool on = (mods & bit[k]) != 0;
        bool w = false;
        if (cap && on) set_image_raw(self, cap, key_icon(mvk[k], w));
        if (cap) set_class(cap, "!cpp-none", !on);
        if (plus) {
            if (on && g_set_text) g_set_text(plus, "+", 0);
            set_class(plus, "!cpp-none", !on);
        }
    }
    U64 cap = find_by_class(box, "om-hk-c6", 1);
    if (!cap) return false;
    set_image_raw(self, cap, key_icon(vk, wide));
    set_class(cap, "om-hk-wide", wide);
    set_class(cap, "!cpp-none", false);
    set_class(box, "om-hk-caps-plain", where == 1);
    set_class(box, "om-hk-caps-eye", where == 2);
    set_class(box, "!cpp-none", false);
    return true;
}

bool show_value(U64 root, const std::string& text, int hotkey = 0, bool caps = false); // hotkey: 1 under a plain
                                                                                       // anchor, 2 under the eye
bool show_value(U64 root, const std::string& text, int hotkey, bool caps)
{
    if (!caps)
        if (U64 box = find_by_class(root, "om-hk-caps")) set_class(box, "!cpp-none", true);
    U64 t = find_by_class(root, "om-value-text");
    if (!t) return true;
    if (!text.empty() && g_set_text) g_set_text(t, text.c_str(), 0);
    set_class(t, "om-hotkey-plain", hotkey == 1); // (an outfit's hotkey on its slot: under the icon)
    set_class(t, "om-hotkey-eye", hotkey == 2);
    return set_class(t, "!cpp-none", text.empty());
}
void apply_eye_slot(U64 self, U64 script, U64 root, U32 i, U32 visible, const std::vector<size_t>& pieces)
{
    U32 k = i < visible ? slot_outfit(i) : kOutfitSlots;
    bool piece = k < pieces.size(), page = k == pieces.size(), back = k == pieces.size() + 1;
    OutfitItem tmp{};
    const OutfitItem* it = piece ? eye_item(pieces[k], tmp) : nullptr;
    std::string pic = it ? piece_icon(*it) : std::string();
    show_slot_image(self, root, i, pic);
    if (i < visible) show_mode_dots(script, mode_count(i), mode_lit(i));
    U64 badge = find_by_class(root, "quickslot-occupancy-icon-chain"), low = find_by_class(root, "cpp-icon-disable-item");
    const char* want = it ? look_badge(it->look) : back ? prompt_class() : nullptr; // (Back: the click prompt, as actions)
    g_badge_elem[i] = badge, g_badge_class[i] = want;
    bool ok = true;
    if (badge) {
        ok &= set_class(badge, "self-center", true);
        for (const char* c : kBadges) ok &= set_class(badge, c, want && !std::strcmp(want, c));
        ok &= set_class(badge, "!cpp-none", !want);
    }
    U64 mark = find_parent_of_class(root, "charged-count");
    if (mark) {
        ok &= back ? set_class(mark, "self-center", true) && set_class(mark, "om-label-wrap", true) : clear_label(mark);
        ok &= set_class(mark, "om-eye-c", false) && set_class(mark, "om-eye2-c", false);
        ok &= set_class(mark, "!cpp-none", !back);
        if (U64 text = back ? find_by_class(mark, "charged-count") : 0) {
            if (g_set_text) g_set_text(text, "Back", 0); // (as the settings' Back)
            ok &= set_class(text, "om-label", true) && set_class(text, "om-label-value", false);
        }
    }
    g_mark_elem[i] = mark, g_mark_shown[i] = mark && back;
    ok &= show_value(root, "");
    bool dim = it && it->look != kLookDefault && it->look != kLookShown; // (a look that hides it at times)
    g_slot_image[i].missing = dim;
    ok &= set_class(root, "!cpp-none", i >= visible);
    if (U64 img = find_by_class(root, "quickslot-item-img"))
        ok &= set_class(img, "om-missing", dim) && set_class(img, "om-present", !dim);
    ok &= swap_class(root, g_slot_class[i], i < visible ? numbered("om-s%u-%u", visible, i) : "");
    U64 icon = rp(script + kOffScriptCategoryIcon);
    if (icon && is_element(icon)) {
        ok &= set_class(icon, "quickslot-item-image-none", false);
        bool sword = page && g_eye_pick == 1, necklace = page && g_eye_pick == 2;
        bool holder = piece && pic.empty() && it->slot != kBackShieldSlot; // (an empty slot: the game's placeholder)
        bool armor = (page && g_eye_pick == 0) || (piece && pic.empty() && !holder);
        ok &= set_class(icon, "om-empty-armor", armor);
        ok &= set_class(icon, "om-empty-sword", sword);
        ok &= set_class(icon, "om-empty-necklace", necklace);
        ok &= swap_class(icon, g_placeholder_class[i], holder ? "quickslot-item-image-equipment-" + std::to_string(it->slot) : std::string());
        ok &= set_class(icon, "!cpp-none", !armor && !sword && !necklace && !holder);
        ok &= set_class(icon, "om-missing", holder && dim);
    }
    g_low_elem[i] = low, g_low_shown[i] = false;
    if (low) ok &= swap_class(low, g_worn_class[i], "") && set_class(low, "!cpp-none", true);
    if (U64 focus = find_by_class(root, "quickslot-item-focus-bg-10"))
        ok &= swap_class(focus, g_focus_class[i], i < visible ? numbered("om-f%u-%u", visible, i) : "");
    if (!ok) LOG("  appearance slot %u: class change skipped (%s)", i, describe(root).c_str());
}

// The settings view's label on a slot's charge-count wrap: taken off (the outfits and the pieces don't show one).
bool clear_label(U64 mark)
{
    if (!mark) return true;
    bool ok = set_class(mark, "om-label-wrap", false);
    U64 text = find_by_class(mark, "charged-count");
    if (text && (has_class(text, "om-label") || has_class(text, "om-label-value"))) {
        if (g_set_text) g_set_text(text, "", 0);
        ok &= set_class(text, "om-label", false) && set_class(text, "om-label-value", false);
    }
    return ok;
}

void apply_cfg_slot(U64 self, U64 script, U64 root, U32 i, U32 visible);

void apply_outfit_slots(U64 self, U32 only = kOutfitSlots)
{
    U32 tab = g_fake_key.load();
    if (tab == 0xFFFF || !layout_ok(self)) return;
    U64 sv = rp(self + kOffRootSlotVecs);
    if (!sv || r32(self + kOffRootSlotVecCount) <= tab) return;
    U64 arr = rp(sv + tab * 0x10ull);
    U32 cnt = r32(sv + tab * 0x10ull + 8);
    U32 visible = visible_slots();
    if (only >= kOutfitSlots) { // (the whole wheel)
        g_applied_visible = visible;
        // (during a character switch the worn gear is already the new character's while the list is still the old
        // one's: comparing them would read as "a new set" and drop the old character's looks, 2026-11-06)
        U16 now_char = current_character();
        bool same_char = now_char == kNoCharacter || g_character == kNoCharacter || now_char == g_character;
        if (int w = same_char ? find_worn_outfit(self) : g_worn_outfit; w != g_worn_outfit) {
            LOG("worn outfit: %d -> %d", g_worn_outfit, w);
            g_worn_outfit = w;
            g_worn_changed = true;
            if (w >= 0) mark_worn(w); // (put together by hand: its looks apply too; none worn keeps the last loaded)
        }
        if (same_char) update_plus_icon(self);
        refresh_missing(self);
        U64 body = rp(rp(rp(self + kOffRootBodies) + tab * 8ull) + 8);
        bool list_ok = body && is_element(body) && swap_class(body, g_list_class, numbered("om-list-%u", visible));
        if (!g_slots_logged || !list_ok)
            LOG("  outfit list%s: %s", list_ok ? "" : " (class change skipped)", body ? describe(body).c_str() : "null");
    }
    std::vector<size_t> pieces = eye_pieces();
    for (U32 i = 0; arr && i < cnt && i < kOutfitSlots; ++i) {
        if (only < kOutfitSlots && i != only) continue;
        U64 script = rp(arr + i * 8ull);
        U64 root = script ? rp(script + 8) : 0;
        if (!root || !is_element(root)) continue;
        if (g_eye >= 0) {
            apply_eye_slot(self, script, root, i, visible, pieces);
            continue;
        }
        if (g_cfg >= 0) {
            apply_cfg_slot(self, script, root, i, visible);
            continue;
        }
        bool empty = i < visible && slot_outfit(i) >= g_outfit_count;
        bool saved = i < visible && !empty;
        std::uint8_t mode = i < visible ? g_slot_mode[i] : std::uint8_t(kModeMain);
        // (the "+" slot: the worn chest when what is worn is not a saved outfit - what saving would keep)
        std::string pic = saved ? g_outfits[slot_outfit(i)].chest_icon : empty ? g_plus_icon : std::string();
        if (saved && pic.empty())
            if (const OutfitItem* c = outfit_piece(g_outfits[slot_outfit(i)], kChestSlot)) pic = piece_icon(*c);
        bool chest = show_slot_image(self, root, i, pic);
        if (i < visible) show_mode_dots(script, mode_count(i), mode);
        U64 badge = find_by_class(root, "quickslot-occupancy-icon-chain"), low = find_by_class(root, "cpp-icon-disable-item");
        const char* want = mode != kModeMain && empty  ? (kPlusSlotCentred ? "om-cog-big" : "om-cog-c") // (settings 1 / 2)
                           : mode == kModeSettings               ? "om-cog-c"
                           : mode == kModeConditions             ? "om-flag-c"
                           : mode == kModeAppearance             ? "om-eye-c"
                           : empty && g_outfit_count < kMaxOutfits ? (kPlusSlotCentred ? "om-plus-big" : "om-plus-c")
                           : saved ? climate_badge(g_outfits[slot_outfit(i)]) // (its Climate marker, or none)
                                                                   : nullptr; // (at the cap: no "+")
        g_badge_elem[i] = badge, g_badge_class[i] = want;
        bool badge_ok = true;
        if (badge) {
            badge_ok &= set_class(badge, "self-center", true);
            for (const char* c : kBadges) badge_ok &= set_class(badge, c, want && !std::strcmp(want, c));
            badge_ok &= set_class(badge, "!cpp-none", !want);
        }
        // Appearance marker: a saved outfit with looks set shows the eye (main mode), above its climate thermometer when
        // it has one, else on the badge spot; on the charge-count wrap (unused by our slots), a second badge element.
        U64 mark = find_parent_of_class(root, "charged-count");
        badge_ok &= clear_label(mark);
        // (an outfit's hotkey shows under its icon: in the same wrap, an anchor when no eye shows there)
        const Outfit* ho = saved && mode == kModeMain ? &g_outfits[slot_outfit(i)] : nullptr;
        bool hk = ho && ho->hk_vk;
        bool looks = false;
        if (mark) {
            if (saved && mode == kModeMain)
                for (U16 s = 0; s < 32; ++s) looks |= slot_look(g_outfits[slot_outfit(i)], s) != kLookDefault;
            bool climate = want && (!std::strncmp(want, "om-cold", 7) || !std::strncmp(want, "om-heat", 7));
            bool stacked = looks && climate;
            badge_ok &= set_class(mark, "self-center", true);
            badge_ok &= set_class(mark, "om-eye-c", looks && !stacked);
            badge_ok &= set_class(mark, "om-eye2-c", stacked);
            badge_ok &= set_class(mark, "om-label-wrap", hk && !looks);
            badge_ok &= set_class(mark, "!cpp-none", !looks && !hk);
        }
        g_mark_elem[i] = mark, g_mark_shown[i] = mark && saved && mode == kModeMain &&
                                                 (has_class(mark, "om-eye-c") || has_class(mark, "om-eye2-c") || hk);
        // (hk: the hotkey's text lives in this wrap too; the game hides it on focus changes, 2026-11-06)
        bool caps = hk && show_caps(self, root, ho->hk_vk, ho->hk_mods, looks ? 2 : 1); // (the game's key icons)
        show_value(root, hk && !caps ? hotkey_name(ho->hk_vk, ho->hk_mods) : std::string(), hk && !caps ? (looks ? 2 : 1) : 0,
                   caps);
        bool missing = saved && outfit_missing(slot_outfit(i));
        g_slot_image[i].missing = missing;
        bool ok = set_class(root, "!cpp-none", i >= visible) && badge_ok;
        if (U64 img = find_by_class(root, "quickslot-item-img"))
            ok &= set_class(img, "om-missing", missing) && set_class(img, "om-present", !missing);
        ok &= swap_class(root, g_slot_class[i], i < visible ? numbered("om-s%u-%u", visible, i) : "");
        U64 icon = rp(script + kOffScriptCategoryIcon);
        if (icon && is_element(icon)) {
            ok &= set_class(icon, "quickslot-item-image-none", false);
            ok &= swap_class(icon, g_placeholder_class[i], "");
            bool sil = (saved && !chest) || (empty && !chest && !kPlusSlotCentred); // no chest / the "+" slot (style A)
            ok &= set_class(icon, "om-empty-armor", sil);
            ok &= set_class(icon, "!cpp-none", !sil);
            ok &= set_class(icon, "om-missing", missing);
        }
        bool worn = saved && int(slot_outfit(i)) == g_worn_outfit;
        // Amber: the last loaded outfit while what is worn has been changed by hand since (not saved); green: the "+" slot
        // while an unsaved set is worn (its chest is previewed there).
        bool drifted = !worn && saved && g_worn_outfit < 0 && g_outfits[slot_outfit(i)].worn && g_amber;
        bool ready = empty && !g_plus_icon.empty(); // (green, the game's "positive" colour: an unsaved set to save)
        g_low_elem[i] = low, g_low_shown[i] = worn || drifted || ready;
        if (low) {
            ok &= swap_class(low, g_worn_class[i], worn      ? numbered("om-w%u-%u", visible, i)
                                                   : drifted ? numbered("om-wa%u-%u", visible, i)
                                                   : ready   ? numbered("om-wg%u-%u", visible, i) : "");
            ok &= set_class(low, "!cpp-none", !g_low_shown[i]); // (hidden by the game's markup)
        }
        U64 focus = find_by_class(root, "quickslot-item-focus-bg-10");
        if (focus)
            ok &= swap_class(focus, g_focus_class[i], i < visible ? numbered("om-f%u-%u", visible, i) : "");
        if (!g_slots_logged || !ok) {
            LOG("  outfit slot %u%s: root %s", i, ok ? "" : " (class change skipped)", describe(root).c_str());
            LOG("    icon %s", icon ? describe(icon).c_str() : "null");
            LOG("    badge %s", badge ? describe(badge).c_str() : "null");
            LOG("    mark %s", mark ? describe(mark).c_str() : "null");
            LOG("    worn %s", low ? describe(low).c_str() : "null");
            LOG("    focus %s", focus ? describe(focus).c_str() : "null");
        }
    }
    g_slots_logged = true;
}

U64 g_outfits_root_of_slots() { return g_fake_key.load() != 0xFFFF ? g_outfits_root : 0; }

bool outfits_current(U64 self)
{
    U32 tab = g_fake_key.load();
    if (tab == 0xFFFF || !g_layout_bad.empty() || self != g_outfits_root) return false;
    U64 tb = rp(self + kOffRootTabBar);
    return tb && r32(tb + kOffTabCurrent) == tab;
}

template <class T>
bool wr(U64 addr, T v)
{
    __try {
        *reinterpret_cast<volatile T*>(addr) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Saved outfit: outfit / settings / conditions / appearance; the "+" slot: + / settings 1 / settings 2 (mode 2 there =
// page 2). The Appearance view: the page slot's three pages, a piece's looks, Back none.
U32 cfg_dots(U32 i, U32* lit);
U32 mode_count(U32 i)
{
    if (g_eye >= 0) {
        size_t k = slot_outfit(i), n = eye_pieces().size();
        return k < n ? kLookCount : k == n ? kEyeSteps : 0;
    }
    if (g_cfg >= 0) return cfg_dots(i, nullptr);
    return 2; // (the slot's action, its settings)
}
// The dot lit on slot i: its mode; in the Appearance view a piece's look, the page slot's step.
U32 mode_lit(U32 i)
{
    if (g_cfg >= 0) {
        U32 lit = 0;
        return cfg_dots(i, &lit) ? lit : 0xFFFFFFFFu;
    }
    if (g_eye < 0) return g_slot_mode[i];
    std::vector<size_t> pieces = eye_pieces();
    U32 k = slot_outfit(i);
    return k < pieces.size() ? look_pos(eye_look(pieces[k])) : k == pieces.size() ? g_eye_pick : 0xFFFFFFFFu;
}
// The dot lit on slot i.
U32 mode_lit(U32 i);

// The slot's dots: `count` of them, `mode` lit (the game sizes them from the count field when one is lit).
bool g_dots_logged = false;
// The game's slot refresh (after every equipment change) re-selects every slot's dots with its own count (0 for ours:
// no items) and -1: the select is hooked, and for our slots' selectors it gets our count and mode instead, so the dots
// never go blank. Our own calls pass through it as well. Our slots are looked up live (the Outfits menu's slot list).
using SelectorSelectHookFn = void(__fastcall*)(U64, U32, U64);
SelectorSelectHookFn g_selector_select_orig = nullptr;
extern std::atomic<bool> g_menu_open;
extern DWORD g_ui_thread;

// The gear slots' refresh (EquipSlotVt slot 164, 0xef5900: after every equipment change) rebuilds a slot's item list
// from the inventory and shows its current item (0xef9b80); ours hold none, so it blanked them and the plugin drew them
// again (they flickered). Everything our slots show is ours: their refresh is skipped; every other slot's runs.
using SlotRefreshFn = void(__fastcall*)(U64);
SlotRefreshFn g_slot_refresh_orig = nullptr;
U64 g_outfits_root_of_slots();

void __fastcall slot_refresh_hook(U64 script)
{
    U64 root = script ? rp(script + 0x28) : 0;
    if (root && root == g_outfits_root_of_slots() && outfit_slot_index(root, script) < kOutfitSlots) return;
    g_slot_refresh_orig(script);
}

void __fastcall selector_select_hook(U64 sel, U32 index, U64 flag)
{
    U64 root = g_outfits_root_of_slots();
    U32 tab = g_fake_key.load();
    U64 sv = sel && root && g_menu_open && GetCurrentThreadId() == g_ui_thread ? rp(root + kOffRootSlotVecs) : 0;
    U64 arr = sv ? rp(sv + tab * 0x10ull) : 0;
    U32 cnt = arr ? r32(sv + tab * 0x10ull + 8) : 0;
    for (U32 i = 0; i < cnt && i < visible_slots() && i < kOutfitSlots; ++i)
        if (U64 script = rp(arr + i * 8ull); script && rp(script + kOffSlotSelector) == sel) {
            U32 count = mode_count(i), page = count ? count : 1;
            bool resized = r32(sel + kOffSelectorCount) != count || r32(sel + kOffSelectorPage) != page;
            if (resized) wr<U32>(sel + kOffSelectorCount, count), wr<U32>(sel + kOffSelectorPage, page);
            g_selector_select_orig(sel, mode_lit(i), flag);
            return;
        }
    g_selector_select_orig(sel, index, flag);
}

void show_mode_dots(U64 script, U32 count, U32 mode)
{
    if (!g_selector_select) return;
    U64 sel = rp(script + kOffSlotSelector);
    if (!has_vt(sel, kRvaItemSelectorVt)) {
        if (!g_dots_logged) g_dots_logged = true, LOG("slot dots: script %llX has no selector (%llX)", script, sel);
        return;
    }
    // Unchanged: left alone (re-selecting rebuilds the dots: they blinked whenever the wheel was re-applied).
    U32 page = count ? count : 1;
    bool resized = r32(sel + kOffSelectorCount) != count || r32(sel + kOffSelectorPage) != page;
    bool moved = kOffSelectorLit && r32(sel + kOffSelectorLit) != mode;
    if (!resized && !moved) return;
    if (r32(sel + kOffSelectorPage) != page && !wr<U32>(sel + kOffSelectorPage, page)) return;
    if (r32(sel + kOffSelectorCount) != count && !wr<U32>(sel + kOffSelectorCount, count)) return;
    __try {
        g_selector_select(sel, mode, moved && !resized ? 0 : 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_selector_select = nullptr;
    }
    if (!g_dots_logged) g_dots_logged = true, LOG("slot dots: %u, %u lit (selector %llX)", count, mode, sel);
}

// Every frame: the visible slots' dots as they should be (cheap: two reads each; the select only on a difference).
void keep_mode_dots(U64 self)
{
    U32 tab = g_fake_key.load();
    U64 sv = tab != 0xFFFF ? rp(self + kOffRootSlotVecs) : 0;
    if (!sv || !g_selector_select) return;
    U64 arr = rp(sv + tab * 0x10ull);
    U32 cnt = r32(sv + tab * 0x10ull + 8), visible = visible_slots();
    for (U32 i = 0; arr && i < cnt && i < visible && i < kOutfitSlots; ++i)
        if (U64 script = rp(arr + i * 8ull)) show_mode_dots(script, mode_count(i), mode_lit(i));
}

// The 12-slot ring texture (QuickSlotItemListBg) is shared by every tab body: on the Outfits tab it gets the
// N-segment ring, and the radial cursor (whose sector count the tab change sets from the tab's 12 slot
// scripts) gets N sectors so the stick picks slots by the same angles.
U64 g_ring_bg = 0;
std::string g_ring_class;
bool g_ring_logged = false;

bool in_depth(U64 self);

void sync_ring(U64 self)
{
    if (g_fake_key.load() == 0xFFFF || self != g_outfits_root || in_depth(self)) return; // (submenu: own sectors)
    U64 menu = rp(self + 8);
    if (!g_ring_bg || !is_element(g_ring_bg)) g_ring_bg = find_by_class(menu, "cpp-item-list-background", 24);
    bool on = outfits_current(self);
    U32 n = on ? wheel_n() : 0;
    bool ok = g_ring_bg && swap_class(g_ring_bg, g_ring_class, n ? numbered("om-ring-%u", n) : "");
    U64 cursor = rp(self + kOffRootCursor);
    U16 sectors = 0;
    if (on && cursor && kOffCursorSectors && rd(cursor + kOffCursorSectors, sectors) && sectors <= kOutfitSlots &&
        sectors != visible_slots())
        ok &= wr<U16>(cursor + kOffCursorSectors, static_cast<U16>(visible_slots()));
    if (!g_ring_logged || !ok) {
        g_ring_logged = true;
        LOG("ring (outfits=%d, n=%u, sectors %u)%s: bg %s", on, n, sectors, ok ? "" : " (change failed)",
            g_ring_bg ? describe(g_ring_bg).c_str() : "not found");
    }
}

// --- Outfit store -------------------------------------------------------------------------------
// OutfitManager.outfits: "character\t<key>" starts a character's list (outfits before the first one have
// no character), then one record per outfit: "outfit\t<name>", optional "chest\t<icon>", then
// "item\t<equip slot>\t<item key>\t<uid hex>" lines.
// That first part is the active set (the loaded game's outfits). Then one part per game save slot the outfits are
// bound to: "save\t<slot>\t<save.save size>\t<write time hex>\t<content hash hex>", followed by lists as above.
// Last, saves the game was still writing: "pending\t<slot>\t<start FILETIME hex>", followed by lists as above.
CharacterOutfits& list_for(Store& s, U16 character)
{
    for (CharacterOutfits& c : s)
        if (c.character == character) return c;
    s.push_back({character, {}});
    return s.back();
}

CharacterOutfits& store_for(U16 character) { return list_for(g_store, character); }

size_t outfit_total(const Store& s)
{
    size_t n = 0;
    for (const CharacterOutfits& c : s) n += c.outfits.size();
    return n;
}

// Save binding. The game's save writes / loads are seen on its own threads (file hook, folder watch), so they work
// on g_published, a copy of the active set the UI thread publishes after each change (all under g_store_mu).
// A load replaces g_published and bumps g_generation; the UI thread then takes it over (pull_active).
// A save is recognised by its save.save content (hash), with size + write time as the quick check.
struct SaveId {
    U64 size = 0, time = 0, hash = 0; // 0 = not known
    bool same(const SaveId& o) const
    {
        if (hash && o.hash) return hash == o.hash;
        return size && size == o.size && time == o.time;
    }
};
struct BoundSet {
    SaveId id;
    Store store;
};
std::mutex g_store_mu;
Store g_published;
std::map<U32, BoundSet> g_bound; // per game save slot: the save the mod last saw there + its outfits
SaveId g_loaded_id;               // the save loaded last (its copies are "this save"'s for Clean up)
bool g_loaded_known = false;
// A save the game is writing; it is bound once the file stays put. Kept in the outfit file too, so a write the
// game was not left time to finish (quit / crash right after saving) is bound on the next start.
struct PendingWrite {
    U32 slot;
    std::wstring path;
    Store store;      // the active outfits when the game started writing
    SaveId id;        // last size + time seen
    U64 since;        // tick of the last change
    U64 started;      // system time (FILETIME) the write was first seen
};
std::vector<PendingWrite> g_pending_writes;
std::vector<PendingWrite> g_unfinished; // pending writes read from the file, bound once the save folder is known
std::atomic<U32> g_generation{0};
U32 g_seen_generation = 0; // UI thread

// The outfit file: "# ..." comment, "version\t<n>", the settings, the lists; last "end\t<lines before it>\t<FNV-1a 64 of
// every byte before it>". A file whose end line does not match was damaged outside the plugin: it is left as it is and
// the session runs read-only. Files without one (older builds) are read as before; the next write adds it.
constexpr int kStoreVersion = 1;

void put(std::string& out, const char* fmt, ...)
{
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    int n = std::vsnprintf(nullptr, 0, fmt, a);
    va_end(a);
    if (n > 0) {
        size_t at = out.size();
        out.resize(at + size_t(n) + 1);
        std::vsnprintf(&out[at], size_t(n) + 1, fmt, b);
        out.resize(at + size_t(n));
    }
    va_end(b);
}

U64 fnv1a(const char* p, size_t n)
{
    U64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<unsigned char>(p[i])) * 0x100000001b3ull;
    return h;
}

void write_lists(std::string& out, const Store& s)
{
    for (int pass = 0; pass < 2; ++pass) // the character-less list first (it has no header)
        for (const CharacterOutfits& c : s) {
            if ((c.character == kNoCharacter) != (pass == 0) || c.outfits.empty()) continue;
            if (pass) put(out, "character\t%u\n", c.character);
            for (const Outfit& o : c.outfits) {
                put(out, "outfit\t%s\n", o.name.c_str());
                if (!o.chest_icon.empty()) put(out, "chest\t%s\n", o.chest_icon.c_str());
                if (o.seen) put(out, "seen\t%llX\n", o.seen);
                if (o.has_weapons) put(out, "weapons\t%d\n", o.load_weapons ? 1 : 0);
                if (o.climate) put(out, "climate\t%u\n", o.climate);
                if (o.stash) put(out, "stash\t%u\n", o.stash);
                if (o.hk_vk) put(out, "hotkey\t%u\t%u\n", o.hk_vk, o.hk_mods);
                if (o.worn) put(out, "worn\t1\n");
                if (o.back_look) put(out, "backlook\t%u\n", o.back_look);
                if (o.has_back) put(out, "backshield\t%u\n", o.has_back);
                for (const auto& [slot, look] : o.empty_looks)
                    if (look) put(out, "slotlook\t%u\t%u\n", slot, look);
                for (const OutfitItem& it : o.items) {
                    if (it.pid) put(out, "item\t%u\t%u\t%llX\t%u\t%X\n", it.slot, it.key, it.uid, it.look, it.pid);
                    else if (it.look) put(out, "item\t%u\t%u\t%llX\t%u\n", it.slot, it.key, it.uid, it.look);
                    else put(out, "item\t%u\t%u\t%llX\n", it.slot, it.key, it.uid);
                }
            }
        }
}

// Set when the outfit file exists but could not be read at start-up (locked by a scanner / sync tool / editor):
// writing then would replace the user's outfits with an empty list, so nothing is written this session.
bool g_store_unreadable = false;
const char* g_store_why = ""; // why (for the message in game)

// Writes g_published + g_bound (g_store_mu held).
extern bool g_stash; // (the settings, defined with the loading code)
extern bool g_take_from_companions;
extern bool g_messages;
extern std::string g_language;

extern bool g_uninstalled;

extern std::vector<U16> g_placeholder_keys; // (the game's placeholder items, see is_placeholder)
std::vector<U32> g_placeholder_ids;         // the file's, until the item table is there


bool write_store_file()
{
    if (g_store_path.empty() || g_uninstalled) return false; // (uninstalled: its file is gone and stays gone)
    if (g_store_unreadable) {
        LOG("outfit file not written: it could not be read at start-up (the outfits in it are kept as they are)");
        return false;
    }
    std::string out = "# Outfit Manager outfits\n";
    put(out, "version\t%d\n", kStoreVersion);
    {
        std::lock_guard<std::mutex> keys(g_keys_mu);
        if (!g_placeholder_keys.empty()) {
            std::string ph;
            std::string ids; // (by permanent id when all are known: a key would name another item after a renumbering)
            bool all = true;
            for (U16 k : g_placeholder_keys) {
                U32 id = pid_of(k);
                all = all && id;
                char one[16];
                std::snprintf(one, sizeof one, "%s%X", ids.empty() ? "" : " ", id);
                ids += one;
                ph += (ph.empty() ? "" : " ") + std::to_string(k);
            }
            if (all) put(out, "placeholderids\t%s\n", ids.c_str());
            else put(out, "placeholders\t%s\n", ph.c_str());
        }
        std::string ks;
        for (U16 k : g_keep_slots) ks += (ks.empty() ? "" : " ") + std::to_string(k);
        put(out, "keepslots\t%s\n", ks.c_str());
    }
    put(out, "setting\tstash\t%d\nsetting\tcompanions\t%d\nsetting\tmessages\t%d\n", g_stash ? 1 : 0,
        g_take_from_companions ? 1 : 0, g_messages ? 1 : 0);
    if (!g_language.empty()) put(out, "setting\tlanguage\t%s\n", g_language.c_str());
    if (const char* id = peek_key_id()) put(out, "setting\tpeekkey\t%s\n", id);
    if (g_peek_pad) put(out, "setting\tpeekpad\t1\n");
    write_lists(out, g_published);
    for (const auto& [slot, b] : g_bound) {
        put(out, "save\t%u\t%llu\t%llX\t%llX\n", slot, b.id.size, b.id.time, b.id.hash);
        write_lists(out, b.store);
    }
    for (const auto* list : {&g_pending_writes, &g_unfinished})
        for (const PendingWrite& p : *list) {
            put(out, "pending\t%u\t%llX\n", p.slot, p.started);
            write_lists(out, p.store);
        }
    size_t lines = static_cast<size_t>(std::count(out.begin(), out.end(), '\n'));
    put(out, "end\t%zu\t%016llX\n", lines, fnv1a(out.data(), out.size()));
    std::wstring tmp = g_store_path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) {
        LOG("cannot write the outfit file");
        return false;
    }
    bool written = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    bool ok = std::fclose(f) == 0 && written && MoveFileExW(tmp.c_str(), g_store_path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) LOG("writing the outfit file FAILED");
    return ok;
}

// Closes the rename prompt / missing-pieces question (nothing renamed or loaded). Their answers refer to an entry of
// the current outfit list, so they must not outlive it (save loaded, character changed, menu closed).
void abort_prompts(const char* why);

// UI thread: takes over the active set a game load put in place.
bool pull_active()
{
    if (g_generation.load() == g_seen_generation) return false;
    abort_prompts("a save was loaded"); // (before the lock: an answer may save)
    std::lock_guard<std::mutex> lock(g_store_mu);
    g_seen_generation = g_generation.load();
    g_store = g_published;
    g_outfits = store_for(g_character).outfits;
    g_outfit_count = static_cast<U32>(g_outfits.size());
    publish_looks(g_store);
    LOG("active outfits replaced by the loaded save's: %zu for character %u (%zu in all)", g_outfits.size(),
        g_character, outfit_total(g_store));
    return true;
}

void save_outfits()
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    if (g_generation.load() != g_seen_generation) {
        LOG("outfit change dropped: a save was loaded meanwhile");
        return;
    }
    store_for(g_character).outfits = g_outfits;
    g_published = g_store;
    publish_looks(g_store);
    bool ok = write_store_file();
    LOG("saved %zu outfits (%zu for character %u)%s", outfit_total(g_store), g_outfits.size(), g_character,
        ok ? "" : " (FAILED)");
}

// nullptr if the file text may be read: its end line matches (or it has none: an older build wrote it) and its version
// is not newer than this build's; otherwise what is wrong with it.
const char* check_store_text(const std::string& text)
{
    size_t endpos = text.rfind("\nend\t");
    if (endpos != std::string::npos) {
        size_t body = endpos + 1;
        unsigned long long lines = 0, hash = 0;
        if (sscanf_s(text.c_str() + body, "end\t%llu\t%llX", &lines, &hash) != 2) return "damaged (its end line)";
        size_t rest = text.find('\n', body);
        if (rest != std::string::npos && text.find_first_not_of("\r\n", rest) != std::string::npos)
            return "damaged (text after its end line)";
        if (size_t(std::count(text.begin(), text.begin() + body, '\n')) != lines) return "damaged (lines missing)";
        if (fnv1a(text.data(), body) != hash) return "damaged (its contents changed)";
    } else {
        LOG("the outfit file has no end check (written by an older build); read as before");
    }
    size_t v = text.find("\nversion\t");
    if (v != std::string::npos && std::atoi(text.c_str() + v + 9) > kStoreVersion) return "from a newer Outfit Manager";
    return nullptr;
}

void load_outfits()
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    g_store.clear();
    g_bound.clear();
    g_unfinished.clear();
    std::map<U32, Store> legacy; // first binding build: slot-only entries ("save\t<slot>\t<size>\t<time>")
    FILE* f = nullptr;
    // A missing file is a fresh start; one that exists but can't be opened (briefly locked) gets a few retries, then
    // the session runs read-only (g_store_unreadable) instead of overwriting it.
    DWORD attr = GetFileAttributesW(g_store_path.c_str());
    DWORD gone = attr == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
    bool exists = attr != INVALID_FILE_ATTRIBUTES || (gone != ERROR_FILE_NOT_FOUND && gone != ERROR_PATH_NOT_FOUND);
    for (int attempt = 0; exists && attempt < 10 && (_wfopen_s(&f, g_store_path.c_str(), L"rb") != 0 || !f); ++attempt)
        Sleep(200);
    if (exists && !f) {
        g_store_unreadable = true, g_store_why = "locked (another program has it open)";
        LOG("the outfit file exists but cannot be read (error %lu); this session will not write it", GetLastError());
    }
    std::string text;
    if (f) {
        char buf[16384];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
        f = nullptr;
        if (const char* why = check_store_text(text)) {
            g_store_unreadable = true, g_store_why = why;
            LOG("the outfit file is %s; it is left as it is and this session will not write it (edited by hand? delete its last "
                "line, \"end ...\", and it is read normally)", why);
            text.clear();
        }
    }
    if (!text.empty()) {
        Store* store = &g_store;
        CharacterOutfits* cur = &store_for(kNoCharacter);
        bool skipping = false;
        for (size_t at = 0; at < text.size();) {
            size_t nl = text.find('\n', at);
            std::string s = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            at = nl == std::string::npos ? text.size() : nl + 1;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty()) continue;
            std::vector<std::string> v;
            for (size_t p = 0;;) {
                size_t t = s.find('\t', p);
                v.push_back(s.substr(p, t == std::string::npos ? std::string::npos : t - p));
                if (t == std::string::npos) break;
                p = t + 1;
            }
            // (an outfit beyond the wheel's slots is dropped with every line of it, not merged into the one before)
            if (v[0] == "outfit") {
                skipping = cur->outfits.size() >= kOutfitSlots;
                if (skipping) LOG("outfit file: \"%s\" is beyond the wheel's %u slots; not read", v.size() >= 2 ? v[1].c_str() : "", kOutfitSlots);
                if (skipping) continue;
            } else if (v[0] == "character" || v[0] == "save" || v[0] == "pending") {
                skipping = false;
            } else if (skipping && v[0] != "setting" && v[0] != "keepslots" && v[0] != "placeholders" && v[0] != "placeholderids" && v[0] != "version") {
                continue;
            }
            std::vector<Outfit>& list = cur->outfits;
            if (v[0] == "save" && v.size() >= 4) {
                U32 slot = static_cast<U32>(std::strtoul(v[1].c_str(), nullptr, 10));
                if (v.size() >= 5) {
                    BoundSet& b = g_bound[slot];
                    b.id = {std::strtoull(v[2].c_str(), nullptr, 10), std::strtoull(v[3].c_str(), nullptr, 16),
                            std::strtoull(v[4].c_str(), nullptr, 16)};
                    store = &b.store;
                } else {
                    store = &legacy[slot];
                }
                cur = &list_for(*store, kNoCharacter);
            } else if (v[0] == "pending" && v.size() >= 3) {
                g_unfinished.push_back({static_cast<U32>(std::strtoul(v[1].c_str(), nullptr, 10)), {}, {}, {}, 0,
                                        std::strtoull(v[2].c_str(), nullptr, 16)});
                store = &g_unfinished.back().store;
                cur = &list_for(*store, kNoCharacter);
            } else if (v[0] == "keepslots" && v.size() >= 2) { // (slots the game won't empty, learned)
                const char* q = v[1].c_str();
                char* e = nullptr;
                for (unsigned long k = std::strtoul(q, &e, 10); e != q; k = std::strtoul(q = e, &e, 10))
                    if (k < 0xFFFF) g_keep_slots.insert(U16(k));
            } else if (v[0] == "placeholderids" && v.size() >= 2) { // (turned into keys at the first menu open)
                std::istringstream ids(v[1]);
                for (std::string one; ids >> one;) g_placeholder_ids.push_back(static_cast<U32>(std::strtoul(one.c_str(), nullptr, 16)));
            } else if (v[0] == "placeholders" && v.size() >= 2) { // (the game's placeholder items, seen before)
                const char* p = v[1].c_str();
                char* e = nullptr;
                for (unsigned long k = std::strtoul(p, &e, 10); e != p; k = std::strtoul(p = e, &e, 10))
                    if (k < 0xFFFF && std::find(g_placeholder_keys.begin(), g_placeholder_keys.end(), U16(k)) ==
                                          g_placeholder_keys.end())
                        g_placeholder_keys.push_back(U16(k));
            } else if (v[0] == "setting" && v.size() >= 3) { // (the plugin's settings, set on the "+" slot's page)
                int x = std::atoi(v[2].c_str());
                // stash 0/1; older files: keep 0 Bag / 1 Storage / 2 Wardrobe, before it pull / stash (0 = off)
                if (v[1] == "stash" || v[1] == "keep" || v[1] == "pull") g_stash = x > 0;
                // (other names: settings of another build, e.g. the dropped "hotkey" / "wardrobe_bonus": ignored)
                else if (v[1] == "companions") g_take_from_companions = x != 0;
                else if (v[1] == "messages") g_messages = x != 0;
                else if (v[1] == "language") g_language = v[2];
                else if (v[1] == "peekkey") peek_key_load(v[2]);
                else if (v[1] == "peekpad") g_peek_pad = x != 0;
                // (other names: settings of another build, e.g. the dropped "hotkey": ignored)
            } else if (v[0] == "character" && v.size() >= 2) {
                cur = &list_for(*store, static_cast<U16>(std::strtoul(v[1].c_str(), nullptr, 10)));
            } else if (v[0] == "outfit" && v.size() >= 2) {
                list.push_back({v[1], {}, {}});
            } else if (!list.empty() && v[0] == "chest" && v.size() >= 2) {
                list.back().chest_icon = v[1];
            } else if (!list.empty() && v[0] == "seen" && v.size() >= 2) {
                list.back().seen = std::strtoull(v[1].c_str(), nullptr, 16);
                if (list.back().seen == ~0ull) list.back().seen = 0; // (written by an older version: read as never checked)
            } else if (!list.empty() && v[0] == "weapons" && v.size() >= 2) { // (its weapon slots are in the items)
                list.back().has_weapons = true;
                list.back().load_weapons = v[1] != "0";
            } else if (!list.empty() && v[0] == "stash" && v.size() >= 2) {
                unsigned x = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                if (x <= 2) list.back().stash = static_cast<std::uint8_t>(x);
            } else if (!list.empty() && (v[0] == "climate" || v[0] == "keep") && v.size() >= 2) {
                unsigned x = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                if (v[0] == "climate" && x <= 3) list.back().climate = static_cast<std::uint8_t>(x);
                if (v[0] == "keep" && x <= 3) list.back().stash = x == 0 ? 0 : x == 1 ? 2 : 1; // (older: Keep in)
            } else if (!list.empty() && v[0] == "hotkey" && v.size() >= 3) {
                unsigned vk = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                unsigned m = static_cast<unsigned>(std::strtoul(v[2].c_str(), nullptr, 10));
                if (vk > 0 && vk < 256 && m < 8) list.back().hk_vk = static_cast<U16>(vk), list.back().hk_mods = static_cast<std::uint8_t>(m);
            } else if (!list.empty() && v[0] == "worn") {
                list.back().worn = true;
            } else if (!list.empty() && v[0] == "backlook" && v.size() >= 2) {
                unsigned b = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                list.back().back_look = static_cast<std::uint8_t>(b < kLookCount ? b : 0);
            } else if (!list.empty() && v[0] == "backshield" && v.size() >= 2) {
                unsigned b = static_cast<unsigned>(std::strtoul(v[1].c_str(), nullptr, 10));
                list.back().has_back = static_cast<std::uint8_t>(b <= 2 ? b : 0);
            } else if (!list.empty() && v[0] == "slotlook" && v.size() >= 3) {
                unsigned b = static_cast<unsigned>(std::strtoul(v[2].c_str(), nullptr, 10));
                if (b < kLookCount) list.back().empty_looks[static_cast<U16>(std::strtoul(v[1].c_str(), nullptr, 10))] = static_cast<std::uint8_t>(b);
            } else if (!list.empty() && v[0] == "item" && v.size() >= 4) {
                unsigned look = v.size() >= 5 ? static_cast<unsigned>(std::strtoul(v[4].c_str(), nullptr, 10)) : 0;
                list.back().items.push_back({static_cast<U16>(std::strtoul(v[1].c_str(), nullptr, 10)),
                                             static_cast<U16>(std::strtoul(v[2].c_str(), nullptr, 10)),
                                             std::strtoull(v[3].c_str(), nullptr, 16),
                                             static_cast<std::uint8_t>(look < kLookCount ? look : 0),
                                             v.size() >= 6 ? static_cast<U32>(std::strtoul(v[5].c_str(), nullptr, 16)) : 0});
            }
        }
    }
    // Slot-only bindings (they were bound to the wrong slot): their lists go back to the active set wherever it has
    // none for that character, and the next save loaded takes them.
    for (const auto& [slot, s] : legacy)
        for (const CharacterOutfits& c : s)
            if (!c.outfits.empty() && store_for(c.character).outfits.empty()) {
                store_for(c.character).outfits = c.outfits;
                LOG("old save slot %u entry: %zu outfits for character %u back in the active set", slot,
                    c.outfits.size(), c.character);
            }
    for (const CharacterOutfits& c : g_store)
        if (!c.outfits.empty()) LOG("loaded %zu outfits for character %u", c.outfits.size(), c.character);
    for (const auto& [slot, b] : g_bound)
        LOG("save slot %u: %zu outfits (save size %llu, time %llX, hash %llX)", slot, outfit_total(b.store), b.id.size,
            b.id.time, b.id.hash);
    for (const PendingWrite& p : g_unfinished)
        LOG("save slot %u was still being written when the game closed: %zu outfits wait for it", p.slot,
            outfit_total(p.store));
    // Until a character is known the wheel shows the character-less (old) list.
    g_character = kNoCharacter;
    g_outfits = store_for(kNoCharacter).outfits;
    g_outfit_count = static_cast<U32>(g_outfits.size());
    g_published = g_store;
    publish_looks(g_store);
}

// Makes `character`'s list the working one. Outfits saved before the lists were per character go to the
// first character seen (they were all made on the main character).
bool switch_character(U16 character)
{
    if (character == g_character) return false;
    abort_prompts("the character changed");
    if (g_character != kNoCharacter) store_for(g_character).outfits = g_outfits;
    CharacterOutfits& legacy = store_for(kNoCharacter);
    bool adopt = !legacy.outfits.empty() && store_for(character).outfits.empty();
    if (adopt) {
        store_for(character).outfits = legacy.outfits;
        legacy.outfits.clear();
    }
    LOG("character %u -> %u%s", g_character, character, adopt ? " (adopted the outfits without a character)" : "");
    g_character = character;
    g_outfits = store_for(character).outfits;
    g_outfit_count = static_cast<U32>(g_outfits.size());
    if (adopt) save_outfits();
    return true;
}

// --- Save binding: watching the game's save files (read only) -----------------------------------
// The game keeps each save in ...\save\<user id>\slot<N>\save.save (+ lobby.save). When the game writes a slot (seen
// by watching the save folder, whatever API it uses) that save gets the active outfits of that moment. A save the
// game loads brings back the outfits of the save with the same content (any slot, so copied saves keep theirs);
// a save the mod never saw written starts with an empty wheel. The first save loaded takes the outfits made so far.
// Loads: the game reads every save to list them (start-up, load menu), slots a few ms apart; a load reads one slot
// alone. So a read counts as a load only if no other slot is read within kListGapMs of it.
constexpr U64 kListGapMs = 500;
constexpr U64 kLoadRestMs = 1500; // a read becomes a load after this long alone
constexpr U64 kWriteRestMs = 2000; // a written save is hashed once it stays unchanged this long

struct PendingLoad {
    int slot = -1;
    std::wstring path;
    SaveId id; // size + time when read
    U64 at = 0;
};
PendingLoad g_pending_load;
int g_last_read_slot = -1;
U64 g_last_read_at = 0;
std::atomic<bool> g_save_work{false}; // a load or write is pending
std::wstring g_save_root;             // ...\save\<user id>\ (set once, before the watch thread starts)
std::atomic<bool> g_watching{false};
thread_local bool t_in_file_hook = false; // our own file accesses are not the game's
thread_local bool t_own_pad = false;      // our own pad reading (XInput, the PlayStation pad's reports): never filtered
extern std::atomic<bool> g_peek_live;
bool peek_pad_owned();                    // the open Outfit Arc came from the pad: its d-pad up / down are the arc's

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

SaveId quick_id(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return {};
    return {(U64(a.nFileSizeHigh) << 32) | a.nFileSizeLow,
            (U64(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime, 0};
}

// FNV-1a over the whole file, opened read only and shared with the game; 0 if it cannot be read.
U64 hash_file(const std::wstring& path)
{
    bool nested = t_in_file_hook;
    t_in_file_hook = true;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    t_in_file_hook = nested;
    if (h == INVALID_HANDLE_VALUE) return 0;
    U64 v = 0xCBF29CE484222325ull;
    std::vector<unsigned char> buf(1 << 16);
    DWORD n = 0;
    while (ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n)
        for (DWORD i = 0; i < n; ++i) v = (v ^ buf[i]) * 0x100000001B3ull;
    CloseHandle(h);
    return v ? v : 1;
}

// "...\save\<digits>\slot<N>\<file>" -> N, else -1. `dir` gets the path up to and including the last separator.
int save_slot(const wchar_t* path, std::wstring* file, std::wstring* dir)
{
    if (!path || !wcsstr(path, L"slot")) return -1;
    std::wstring p = path;
    for (wchar_t& c : p) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    size_t f = p.rfind(L'\\');
    if (f == std::wstring::npos || f == 0) return -1;
    size_t d = p.rfind(L'\\', f - 1);
    if (d == std::wstring::npos || d == 0 || p.compare(d + 1, 4, L"slot") != 0) return -1;
    size_t u = p.rfind(L'\\', d - 1);
    if (u == std::wstring::npos || u + 1 == d || u < 5 || p.compare(u - 5, 5, L"\\save") != 0) return -1;
    for (size_t i = u + 1; i < d; ++i)
        if (!std::iswdigit(p[i])) return -1;
    if (d + 5 >= f) return -1;
    for (size_t i = d + 5; i < f; ++i)
        if (!std::iswdigit(p[i])) return -1;
    if (file) *file = p.substr(f + 1);
    if (dir) *dir = std::wstring(path, f + 1);
    return static_cast<int>(std::wcstoul(p.c_str() + d + 5, nullptr, 10));
}

// "slot<N>" or "slot<N>\..." (a name relative to g_save_root) -> N, else -1.
int slot_of_name(const std::wstring& name)
{
    if (name.size() < 5 || _wcsnicmp(name.c_str(), L"slot", 4) != 0) return -1;
    size_t i = 4;
    while (i < name.size() && std::iswdigit(name[i])) ++i;
    if (i == 4 || (i < name.size() && name[i] != L'\\')) return -1;
    return static_cast<int>(std::wcstoul(name.c_str() + 4, nullptr, 10));
}

std::wstring slot_save_path(U32 slot) { return g_save_root + L"slot" + std::to_wstring(slot) + L"\\save.save"; }

// The pending read becomes the active set (g_store_mu held).
void apply_load(const char* why)
{
    PendingLoad l = g_pending_load;
    g_pending_load.slot = -1;
    if (l.slot < 0) return;
    U32 slot = static_cast<U32>(l.slot);
    SaveId id = l.id;
    SaveId now = quick_id(l.path);
    if (now.size == id.size && now.time == id.time) id.hash = hash_file(l.path); // else rewritten since: size + time
    const char* how;
    auto pw = std::find_if(g_pending_writes.begin(), g_pending_writes.end(),
                           [&](const PendingWrite& p) { return p.slot == slot; });
    int from = -1;
    for (const auto& [s, b] : g_bound)
        if (b.id.same(id) && (from < 0 || s == slot)) from = static_cast<int>(s);
    if (pw != g_pending_writes.end()) {
        g_published = pw->store;
        how = "restored (saved moments ago)";
    } else if (g_bound.empty()) {
        g_bound[slot] = {id, g_published};
        how = "first save loaded with the mod: it keeps the current outfits";
    } else if (from >= 0) {
        g_published = g_bound[U32(from)].store;
        g_bound[slot] = {id, g_published};
        how = U32(from) == slot ? "restored" : "restored (same save as another slot)";
    } else {
        g_published.clear();
        how = "the mod never saw this save written: no outfits";
    }
    g_loaded_id = id, g_loaded_known = true;
    ++g_generation;
    publish_looks(g_published); // (before the game builds the character: its looks apply from the loading screen)
    write_store_file();
    LOG("game loaded save slot %u (%s; size %llu, hash %llX): %s -> %zu outfits", slot, why, id.size, id.hash, how,
        outfit_total(g_published));
}

void on_save_read(U32 slot, const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    U64 now = GetTickCount64();
    bool listing = g_last_read_slot >= 0 && U32(g_last_read_slot) != slot && now - g_last_read_at < kListGapMs;
    g_last_read_slot = static_cast<int>(slot);
    g_last_read_at = now;
    if (listing) {
        if (g_pending_load.slot >= 0) LOG("game is listing its saves (start-up / load menu): not a load");
        g_pending_load.slot = -1;
        return;
    }
    g_pending_load = {static_cast<int>(slot), path, quick_id(path), now};
    g_save_work = true;
}

// Watch thread: something in slot<N>\ changed (the game is writing that save, or deleting it).
void on_slot_changed(U32 slot, const std::wstring& name, DWORD action)
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    if (g_pending_load.slot >= 0) {
        // The game reads a save it is about to overwrite (overwrite check / backup): that read is not a load. (The read
        // is at most kLoadRestMs old here; a real load is never followed that fast by a write of the same slot. The
        // file name is not checked: the write may start with a temp file.)
        if (U32(g_pending_load.slot) == slot) {
            LOG("save slot %u was read just before being written: not a load", slot);
            g_pending_load.slot = -1;
        } else {
            apply_load("before a save write");
        }
    }
    U64 now = GetTickCount64();
    auto it = std::find_if(g_pending_writes.begin(), g_pending_writes.end(),
                           [&](const PendingWrite& p) { return p.slot == slot; });
    if (it == g_pending_writes.end()) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        g_pending_writes.push_back(
            {slot, slot_save_path(slot), g_published, {}, now, (U64(ft.dwHighDateTime) << 32) | ft.dwLowDateTime});
        write_store_file(); // noted at once, in case the game closes before the write is bound
        LOG("game is writing save slot %u (%ls, change %lu): %zu outfits go with it", slot, name.c_str(), action,
            outfit_total(g_published));
    } else {
        it->since = now;
    }
    g_save_work = true;
}

// Watch thread start: saves the game was still writing when it last closed get their outfits now, if the save file
// did change since the write began (else the write never finished and the old save keeps its own).
void bind_unfinished()
{
    std::lock_guard<std::mutex> lock(g_store_mu);
    if (g_unfinished.empty()) return;
    constexpr U64 kSlack = 10'000'000; // 1 s in FILETIME units
    for (const PendingWrite& p : g_unfinished) {
        std::wstring path = slot_save_path(p.slot);
        SaveId id = quick_id(path);
        if (id.size && id.time + kSlack >= p.started) {
            id.hash = hash_file(path);
            g_bound[p.slot] = {id, p.store};
            LOG("save slot %u, written as the game closed: %zu outfits bound (size %llu, hash %llX)", p.slot,
                outfit_total(p.store), id.size, id.hash);
        } else {
            LOG("save slot %u was not finished when the game closed: its outfits are dropped", p.slot);
        }
    }
    g_unfinished.clear();
    write_store_file();
}

DWORD WINAPI save_watch_thread(LPVOID)
{
    t_in_file_hook = true;
    bind_unfinished();
    HANDLE dir = CreateFileW(g_save_root.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir == INVALID_HANDLE_VALUE) {
        LOG("cannot watch %ls (%lu): outfits will not follow the game saves", g_save_root.c_str(), GetLastError());
        return 0;
    }
    LOG("watching %ls for save writes", g_save_root.c_str());
    std::vector<DWORD> buf(16384 / sizeof(DWORD));
    DWORD n = 0;
    while (ReadDirectoryChangesW(dir, buf.data(), static_cast<DWORD>(buf.size() * sizeof(DWORD)), TRUE,
                                 FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                     FILE_NOTIFY_CHANGE_LAST_WRITE,
                                 &n, nullptr, nullptr)) {
        if (n == 0) continue; // overflow: the next change of the slot still arrives
        auto* e = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf.data());
        for (;;) {
            std::wstring name(e->FileName, e->FileNameLength / sizeof(wchar_t));
            if (int slot = slot_of_name(name); slot >= 0) on_slot_changed(U32(slot), name, e->Action);
            if (!e->NextEntryOffset) break;
            e = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<BYTE*>(e) + e->NextEntryOffset);
        }
    }
    LOG("save watch stopped (%lu)", GetLastError());
    CloseHandle(dir);
    return 0;
}

// Key thread, every 250 ms: apply a read as a load once it has rested; bind a written save once it stays put.
void poll_saves()
{
    if (!g_save_work) return;
    std::lock_guard<std::mutex> lock(g_store_mu);
    U64 now = GetTickCount64();
    if (g_pending_load.slot >= 0 && now - g_pending_load.at >= kLoadRestMs) apply_load("read alone");
    bool changed = false;
    for (auto it = g_pending_writes.begin(); it != g_pending_writes.end();) {
        SaveId q = quick_id(it->path);
        if (q.size != it->id.size || q.time != it->id.time) {
            it->id = q;
            it->since = now;
        }
        if (now - it->since < kWriteRestMs) {
            ++it;
            continue;
        }
        if (!q.size) {
            if (g_bound.erase(it->slot)) LOG("save slot %u is gone: its outfits are dropped", it->slot);
        } else {
            q.hash = hash_file(it->path);
            LOG("save slot %u written: %zu outfits bound (size %llu, hash %llX)", it->slot, outfit_total(it->store),
                q.size, q.hash);
            g_bound[it->slot] = {q, std::move(it->store)};
        }
        changed = true;
        it = g_pending_writes.erase(it);
    }
    if (changed) write_store_file();
    g_save_work = g_pending_load.slot >= 0 || !g_pending_writes.empty();
}

// UI thread, menu opening: a read that has been alone for a while is certainly a load by now.
void settle_load_now()
{
    if (!g_save_work) return;
    std::lock_guard<std::mutex> lock(g_store_mu);
    if (g_pending_load.slot >= 0 && GetTickCount64() - g_pending_load.at >= kListGapMs) apply_load("menu opened");
}

// A game open of a save file: reads of save.save are load candidates; the first one also starts the folder watch.
void on_save_file(const wchar_t* path, DWORD access, bool ok, const char* api)
{
    std::wstring file, dir;
    int slot = save_slot(path, &file, &dir);
    if (slot < 0 || !ok || file != L"save.save") return;
    if (!g_watching.exchange(true)) {
        g_save_root = dir.substr(0, dir.size() - 1);
        g_save_root.resize(g_save_root.find_last_of(L"\\/") + 1);
        if (HANDLE t = CreateThread(nullptr, 0, &save_watch_thread, nullptr, 0, nullptr)) CloseHandle(t);
    }
    if (!(access & (GENERIC_READ | FILE_READ_DATA)) || (access & (GENERIC_WRITE | FILE_WRITE_DATA))) return;
    LOG("%s read slot%d\\save.save", api, slot);
    on_save_read(U32(slot), dir + L"save.save");
}

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFile2Fn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
CreateFileWFn g_create_file_orig = nullptr;
CreateFile2Fn g_create_file2_orig = nullptr;

HANDLE WINAPI create_file_hook(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                               DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_create_file_orig(path, access, share, sa, disposition, flags, tmpl);
    if (!t_in_file_hook && path && (path[0] == L'\\' || path[0] == L'/') && (wcsstr(path, L"hid#") || wcsstr(path, L"HID#"))) {
        DWORD err = GetLastError();
        t_in_file_hook = true;
        note_pad_handle(path, h);
        t_in_file_hook = false;
        SetLastError(err);
    }
    if (!t_in_file_hook && path && wcsstr(path, L"slot")) {
        DWORD err = GetLastError();
        t_in_file_hook = true;
        on_save_file(path, access, h != INVALID_HANDLE_VALUE, "CreateFileW");
        t_in_file_hook = false;
        SetLastError(err);
    }
    return h;
}

HANDLE WINAPI create_file2_hook(LPCWSTR path, DWORD access, DWORD share, DWORD disposition,
                                LPCREATEFILE2_EXTENDED_PARAMETERS params)
{
    HANDLE h = g_create_file2_orig(path, access, share, disposition, params);
    if (!t_in_file_hook && path && wcsstr(path, L"slot")) {
        DWORD err = GetLastError();
        t_in_file_hook = true;
        on_save_file(path, access, h != INVALID_HANDLE_VALUE, "CreateFile2");
        t_in_file_hook = false;
        SetLastError(err);
    }
    return h;
}

bool install_save_hooks()
{
    struct { const char* name; void* fn; void** orig; } hooks[] = {
        {"CreateFileW", reinterpret_cast<void*>(&create_file_hook), reinterpret_cast<void**>(&g_create_file_orig)},
        {"CreateFile2", reinterpret_cast<void*>(&create_file2_hook), reinterpret_cast<void**>(&g_create_file2_orig)},
    };
    for (auto& h : hooks) {
        void* target = nullptr;
        if (MH_CreateHookApiEx(L"kernelbase", h.name, h.fn, h.orig, &target) != MH_OK || MH_EnableHook(target) != MH_OK) {
            LOG("save hook %s failed", h.name);
            return false;
        }
    }
    return true;
}

// --- Worn gear ----------------------------------------------------------------------------------
struct WornRaw {
    U16 slot; // equipment-N category
    U16 key;
    U64 uid;
    const char* icon;
    U16 qslot; // the slot script's quickslot id (+0x190, logged)
    std::uint8_t type; // slot data category type (+0x960, logged)
    U16 at[2]; // the two u16 after the item copy (item + 0xc8; maybe its container + position, logged)
};

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
U64 acquire_player(U64* ctx);
void release_player(U64* ctx);

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
U64 call_inventory(U64 actor);
std::string item_name(U16 key); // "" if unknown
std::vector<U16> g_placeholder_keys;   // seen in the hidden bag (only ever added to)
std::vector<std::string> g_placeholder_names;
bool g_placeholders_new = false; // a key was added: refresh_missing saves the file (not here: callers may hold its lock)
U64 g_placeholder_read = 0; // tick (re-read every few seconds: cheap, one bag)

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

// The two hands' weapon slots (equipment-0: MainWeapon_*_R, equipment-1: MainWeapon_*_L). Two one-handed weapons worn
// in each other's hands count as the outfit's (2026-11-06, a player's log: the game refuses to take either off -
// "no items available to be equipped after removal" - so a load could never swap them back; every load ended 18 of 20
// and the outfit showed as modified).
constexpr U16 kHandR = 0, kHandL = 1;

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

std::string default_name()
{
    for (U32 n = 1;; ++n) {
        std::string s = lang::f("outfit.default_name", {{"n", std::to_string(n)}});
        bool used = false;
        for (const Outfit& o : g_outfits) used |= o.name == s;
        if (!used) return s;
    }
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

// --- Appearance ---------------------------------------------------------------------------------
// Each character's last loaded outfit decides how its pieces look, on our own visibility layer (above the game's,
// below nothing it uses for its own items: the flight cloak is an item of its own). A piece counts only while that
// exact piece (uid + key) is worn in its slot; anything else in the slot is left to the game. The game's headgear
// switch (kRvaBattleVis) runs on loading (before the loading screen shows the character), on battle changes and model
// rebuilds: the looks are applied right after it, with its in-battle flag for Combat only / Out of combat only.
using VisSetFn = std::uint8_t(__fastcall*)(U64 model, U64 list, std::uint8_t shown, std::uint8_t layer, U64, U64,
                                           std::uint8_t);
using VisReleaseFn = std::uint8_t(__fastcall*)(U64 model, U64 list, std::uint8_t layer, std::uint8_t, std::uint8_t);
using BattleVisFn = void(__fastcall*)(U64 inv, std::uint8_t in_battle, std::uint8_t show_headgear);
using SlotHandleFn = int*(__fastcall*)(U64 inv, int* out, short slot);
VisSetFn g_vis_set = nullptr;
VisReleaseFn g_vis_release = nullptr;
BattleVisFn g_battle_vis_orig = nullptr;
SlotHandleFn g_slot_handle = nullptr;
constexpr std::uint8_t kLookLayer = 6;

struct LookPiece {
    U16 slot;
    U16 key;
    U64 uid;
    std::uint8_t look;
};
std::mutex g_looks_mu;
std::map<U16, std::vector<LookPiece>> g_looks; // character -> the worn outfit's pieces with a look (published copy)

std::mutex g_appear_mu; // the maps below; one sync at a time (game thread, job threads, UI thread)
std::map<U64, std::map<U16, int>> g_look_applied; // inventory -> slot -> the model handle our layer decides
std::map<U64, bool> g_in_battle;                  // inventory -> the headgear switch's last in-battle flag
std::map<U64, U64> g_look_model_inv;              // character model -> its inventory (seen by a sync)
std::vector<U64> g_look_pending;                  // inventories whose model was rebuilt: synced on the next UI frame
std::set<U64> g_look_swapped;                     // ... of those, the rebuilt ones (a model swap: not a layer call)
// Inventories mid outfit change: their model still shows the old pieces until it swaps (the part handles stay the same
// across pieces, so a look set now lands on whichever mesh is there). Until then looks only hide: nothing is shown or
// let go of, so neither the old pieces nor the new ones flash.
std::set<U64> g_look_hold;
std::set<U64> g_look_swap_seen; // ... that have swapped since (the rebuild comes as many swaps, one part each)
// Weapons: a drawn weapon is always shown (its look applies while sheathed). The game's in/out check says when one is
// drawn: (vis ctrl, part name) -> (inventory, slot) of the weapons with a look, and their last drawn state.
std::map<std::pair<U64, U32>, std::pair<U64, U16>> g_weapon_parts;
std::pair<U64, U16> g_bag_in_use{}; // (inventory, slot) of the backpack in use (its own: the draw check rewrites g_drawn)
std::map<std::pair<U64, U16>, bool> g_drawn;
// Bag-mounted tools (the fertilizer sprayer): the item's model is only the tank (Bag); what comes out when it is used
// (Tool_Sprayer, TwoHandWeapon_Thrower) is attached to the bag's socket and is no part of that item. A drawn part
// we don't watch counts for the character's bag item with a look: vis ctrl -> (inventory, its slot).
std::map<U64, std::pair<U64, U16>> g_bag_weapon;
std::map<std::pair<U64, U32>, bool> g_part_drawn; // (vis ctrl, part) -> drawn (a watched weapon part)
// Kliff's description: TwoHandWeapon_Thrower sits in the bag's socket (Bag_RangeWeapon_Thrower_Socket) and
// Tool_Sprayer is the in-hand part (Visible="Out"): the bag tool is in use while either of them is drawn.
constexpr const char* kBagToolParts[] = {"TwoHandWeapon_Thrower", "Tool_Sprayer"};
std::set<U32> g_bag_tool_names; // their part-name indices (looked up once)
std::map<std::pair<U64, U32>, bool> g_bag_part_drawn; // (vis ctrl, part) -> drawn

// Flight: the flight cloak (an item of its own, not in a slot) is shown on the game's layer 2 while flying, and the game
// then hides what would clip through it (the hat, the cloak). Our layer is above the game's, so while flying it forces
// nothing visible: pieces a look would show are left to the game (hidden ones stay hidden).
constexpr std::uint8_t kFlightLayer = 2;
std::map<U64, bool> g_flying; // inventory -> its flight cloak is showing
std::map<std::pair<U64, int>, bool> g_flight_tool; // (inventory, handle) -> a flight tool in a slot is in use
thread_local std::pair<U64, int> t_flight_show; // (model, handle) to show as soon as the game's call has returned
thread_local std::set<int> t_flight_release;    // the pieces to let go of at that moment
std::map<U64, std::set<int>> g_look_forced;     // inventory -> handles our looks force visible now
std::map<U64, std::set<int>> g_look_wanted; // inventory -> handles of its pieces with a look (whether applied or yielding)
// Priority (user): the game's mechanics hiding gear > our looks > the game's menu settings (the headgear option, layer 4).
// A look that would show a piece yields while any other game layer hides it (part+0x70: bit 16+L = layer L decides,
// bit L = it shows).
constexpr std::uint8_t kHeadgearLayer = 4;
// Layer 0 is mostly the drawn / sheathed display the weapon display options feed ("PartInOut": settings, below our
// looks), but animations' frame events hide parts on it too (a mechanic): those hides are recorded by caller.
constexpr std::uint8_t kDisplayLayer = 0;
std::set<std::pair<U64, int>> g_frame_hidden; // (model, handle) an animation frame event hides now
// A weapon's parts by name (case, extras): found once per model state (its part, the table and its size unchanged);
// every sync used to search the model's parts by name again.
struct PartInfo {
    U64 part = 0, tab = 0;
    U32 count = 0;
    int ch_case = 0;
    std::vector<int> extras;
};
std::map<std::pair<U64, int>, PartInfo> g_part_info; // (model, handle) -> what was found

// Copies the anchor outfits' looks for the sync, by slot (g_store_mu held: after every change to the store).
void publish_looks(const Store& s)
{
    std::map<U16, std::vector<LookPiece>> m;
    for (const CharacterOutfits& c : s)
        for (const Outfit& o : c.outfits)
            if (o.worn) {
                for (const OutfitItem& it : o.items)
                    if (it.look != kLookDefault) m[c.character].push_back({it.slot, 0xFFFF, 0, it.look});
                for (const auto& [slot, look] : o.empty_looks)
                    if (look != kLookDefault && !outfit_has_slot(o, slot)) m[c.character].push_back({slot, 0xFFFF, 0, look});
                if (o.back_look != kLookDefault) m[c.character].push_back({kBackShieldSlot, 0xFFFF, 0, o.back_look});
            }
    std::lock_guard<std::mutex> lock(g_looks_mu);
    g_looks = std::move(m);
}

// The character model: the path the headgear switch takes from the inventory's actor.
U64 look_model(U64 actor) { return rp(rp(rp(rp(rp(rp(actor + kOffActorComp) + 0x40) + 0x130)) + 8) + kOffModelRoot); }

bool look_call(U64 model, int handle, int op) // op: 0 hide, 1 show, 2 release
{
    struct {
        int* p;
        U32 n;
    } list{&handle, 1};
    __try {
        if (op == 2) g_vis_release(model, reinterpret_cast<U64>(&list), kLookLayer, 1, 0);
        else g_vis_set(model, reinterpret_cast<U64>(&list), op == 1, kLookLayer, 0, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int look_handle(U64 inv, U16 slot)
{
    int h = 0;
    __try {
        g_slot_handle(inv, &h, static_cast<short>(slot));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        h = 0;
    }
    return h;
}

// The model part of a worn piece: the model's table (+0x58: 16-byte {u32 handle, .., part*}, count +0x60). A part:
// +0x48 its name (part-name string index), children +0x18 (count +0x20), visibility layers +0x70.
// The game layer hiding the part as a mechanic, or -1 (the settings' layers 0 and 4 and ours don't count; an animation's
// frame-event hide on layer 0 does: g_frame_hidden).
int game_hides(U64 model, int handle, U64 part)
{
    if (g_frame_hidden.count({model, handle})) return kDisplayLayer;
    U32 mask = part ? r32(part + 0x70) : 0;
    for (U32 l = 0; l < 16; ++l)
        if (l != kDisplayLayer && l != kHeadgearLayer && l != kLookLayer && (mask >> (16 + l) & 1) && !(mask >> l & 1))
            return static_cast<int>(l);
    return -1;
}

U64 look_part(U64 model, int handle)
{
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (static_cast<int>(r32(tab + i * 16ull)) == handle) return rp(tab + i * 16ull + 8);
    return 0;
}

// Every model part of a worn piece: an item can bring several (the fertilizer sprayer: its tank Bag, and the
// Tool_Sprayer / TwoHandWeapon_Thrower that come out when it is used), each with its children.
std::vector<U64> look_parts(U64 model, int handle)
{
    std::vector<U64> v;
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (static_cast<int>(r32(tab + i * 16ull)) == handle)
            if (U64 part = rp(tab + i * 16ull + 8)) {
                v.push_back(part);
                U64 kids = rp(part + 0x18);
                for (U32 k = 0, c = r32(part + 0x20); kids && k < c && k < 16; ++k)
                    if (U64 kid = rp(kids + k * 8ull)) v.push_back(kid);
            }
    return v;
}

// Part names: the string table behind kRvaPartNameMap (its first `mov rax, [rip+disp]` names the global).
U64 g_part_names = 0;
std::string part_name(U32 index)
{
    if (!g_part_names && kRvaPartNameMap) {
        const auto* code = reinterpret_cast<const std::uint8_t*>(g_base + kRvaPartNameMap);
        for (int i = 0; i < 0x40 - 7 && !g_part_names; ++i)
            if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x05) {
                std::int32_t disp;
                std::memcpy(&disp, code + i + 3, 4);
                if (U64 g = rp(g_base + kRvaPartNameMap + i + 7 + disp)) g_part_names = rp(g + 0x58);
            }
    }
    std::string s;
    U64 p = g_part_names ? rp(g_part_names + U64(index) * 16) : 0;
    for (int i = 0; p && i < 96; ++i) {
        char c = 0;
        if (!rd(p + i, c) || c < 0x20 || c > 0x7E) break;
        s += c;
    }
    if (s.rfind("CD_", 0) == 0) s.erase(0, 3); // (2.04 dropped the prefix from part names)
    return s;
}

// A weapon's case (scabbard) is a part of its own, named like the weapon with "IN" before its side
// (MainWeapon_Sword_R -> MainWeapon_Sword_IN_R, TwoHandWeapon_Sword -> TwoHandWeapon_Sword_IN), as the
// character description pairs them (WeaponCasePart). Its model handle, or 0 when the weapon has none.
int case_handle(U64 model, const std::string& weapon)
{
    if (weapon.empty()) return 0;
    std::vector<std::string> t;
    for (size_t a = 0;;) {
        size_t b = weapon.find('_', a);
        t.push_back(weapon.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    size_t side = t.size();
    for (size_t i = t.size(); i-- > 1;)
        if (t[i] == "R" || t[i] == "L") { side = i; break; }
    t.insert(t.begin() + static_cast<std::ptrdiff_t>(side), "IN");
    std::string want;
    for (size_t i = 0; i < t.size(); ++i) want += (i ? "_" : "") + t[i];
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (U64 part = rp(tab + i * 16ull + 8); part && part_name(r32(part + 0x48)) == want)
            return static_cast<int>(r32(tab + i * 16ull));
    return 0;
}

// Parts of their own that belong with a weapon and follow its look like a scabbard (drawn or not).
std::vector<int> extra_case_handles(U64 model, const std::string& weapon)
{
    static const std::vector<const char*> kBow = {"MainWeapon_Quiver", "MainWeapon_Quiver_Arw",
                                                  "MainWeapon_Quiver_Arw_01", "MainWeapon_Quiver_Arw_02",
                                                  "MainWeapon_Quiver_Arw_03", "MainWeapon_Arw_IN", "MainWeapon_Arw"};
    static const std::vector<const char*> kBag = {"Bag_Belt", "Tool_Sprayer"}; // (a bag tool's straps, its hand part)
    static const std::vector<const char*> kLantern = {"Lantern_Ring"}; // (the ring some lanterns hang from)
    std::vector<int> v;
    const std::vector<const char*>* group = weapon == "MainWeapon_Bow" ? &kBow : weapon == "Bag" ? &kBag : weapon == "Lantern" ? &kLantern : nullptr;
    if (!group) return v;
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (U64 part = rp(tab + i * 16ull + 8)) {
            std::string nm = part_name(r32(part + 0x48));
            for (const char* b : *group)
                if (nm == b) v.push_back(static_cast<int>(r32(tab + i * 16ull)));
        }
    return v;
}

// Once per model: every part on it by name (what a piece brings, what the game adds: straps, the horse's shield).
void log_model_parts(U64 model, U16 ch)
{
    static std::map<U64, std::string> last;
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    std::string s;
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (U64 part = rp(tab + i * 16ull + 8))
            s += " " + std::to_string(static_cast<int>(r32(tab + i * 16ull))) + "=" + part_name(r32(part + 0x48));
    std::string& was = last[model];
    if (s == was) return;
    was = s;
    LOG("appearance: character %u model parts (%u):%s", ch, n, s.c_str());
}

std::map<U64, std::string> g_look_state; // inventory -> the last logged state (logged when it changes)

// A part of the model still loading (its swap is to come; a look set now is queued and lands with it).
bool model_loading(U64 model)
{
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (U64 part = rp(tab + i * 16ull + 8); part && rp(part + 0x78)) return true;
    return false;
}

// Everything kept about a character's inventory (g_appear_mu held): when it has no looks left, or is gone.
void forget_inventory(U64 inv)
{
    g_look_applied.erase(inv), g_in_battle.erase(inv), g_look_wanted.erase(inv), g_flying.erase(inv);
    g_look_state.erase(inv), g_look_hold.erase(inv), g_look_swap_seen.erase(inv);
    for (auto it = g_look_model_inv.begin(); it != g_look_model_inv.end();) {
        if (it->second != inv) {
            ++it;
            continue;
        }
        for (auto f = g_frame_hidden.begin(); f != g_frame_hidden.end();)
            f = f->first == it->first ? g_frame_hidden.erase(f) : std::next(f);
        for (auto c = g_part_info.begin(); c != g_part_info.end();)
            c = c->first.first == it->first ? g_part_info.erase(c) : std::next(c);
        it = g_look_model_inv.erase(it);
    }
    for (auto it = g_drawn.begin(); it != g_drawn.end();) it = it->first.first == inv ? g_drawn.erase(it) : std::next(it);
    for (auto it = g_weapon_parts.begin(); it != g_weapon_parts.end();)
        it = it->second.first == inv ? g_weapon_parts.erase(it) : std::next(it);
    for (auto it = g_bag_weapon.begin(); it != g_bag_weapon.end();)
        it = it->second.first == inv ? g_bag_weapon.erase(it) : std::next(it);
}

void look_sync(U64 inv, bool in_battle, bool swapped = false)
{
    if (!g_vis_set || !inv) return;
    U64 actor = rp(inv + 8);
    if (!actor || rp(rp(actor + kOffActorComp) + kOffCompInventory) != inv) { // (gone since)
        std::lock_guard<std::mutex> lock(g_appear_mu);
        forget_inventory(inv);
        return;
    }
    U64 model = look_model(actor);
    if (!model) return;
    U16 ch = character_of(actor);
    std::vector<LookPiece> pieces;
    {
        std::lock_guard<std::mutex> lock(g_looks_mu);
        if (auto it = g_looks.find(ch); it != g_looks.end()) pieces = it->second;
    }
    U64 vc = rp(rp(rp(actor + kOffActorComp) + 0x40) + 0xE8); // (its in/out vis ctrl: the drawn check's first argument)
    std::lock_guard<std::mutex> lock(g_appear_mu);
    if (swapped) g_look_swap_seen.insert(inv);
    if (g_look_hold.count(inv) && g_look_swap_seen.count(inv) && !model_loading(model))
        g_look_hold.erase(inv), g_look_swap_seen.erase(inv); // (the last part is in: the looks apply in full)
    bool hold = g_look_hold.count(inv) != 0;
    // (a character without looks - every NPC the headgear switch runs for - is not kept at all)
    auto ap = g_look_applied.find(inv);
    if (pieces.empty() && (ap == g_look_applied.end() || ap->second.empty())) {
        if (ap != g_look_applied.end() && !hold) forget_inventory(inv);
        return;
    }
    g_in_battle[inv] = in_battle;
    for (auto it = g_look_model_inv.begin(); it != g_look_model_inv.end();) // (its model: this one only)
        it = it->second == inv && it->first != model ? g_look_model_inv.erase(it) : std::next(it);
    g_look_model_inv[model] = inv;
    std::map<U16, int>& applied = g_look_applied[inv]; // slot (a case: slot | 0x100) -> handle
    bool flying = g_flying.count(inv) && g_flying[inv];
    // (the weapon slots come from the Weapons wheel's record: read here too, the load screen comes before any wheel)
    if (!g_weapon_slot_count && g_qsi_get) collect_weapon_slots();
    U64 list = rp(inv + kOffInvEquipList);
    std::map<U16, int> now;
    std::set<int> wanted, forced;
    std::string state; // per piece: what it is now and why (the log line)
    for (const LookPiece& p : pieces) {
        state += " equipment-" + std::to_string(p.slot);
        U64 e = list ? equipped_item(list, p.slot) : 0;
        U64 uid = 0;
        U16 key = 0xFFFF;
        if (!e || !rd(e, uid) || !rd(e + 8, key)) { state += " empty"; continue; }
        if (key == 0xFFFF) { state += " empty"; continue; } // (a look belongs to the slot: whatever is there)
        int h = look_handle(inv, p.slot);
        if (!h) { state += " no model part"; continue; }
        wanted.insert(h);
        bool shown = p.look == kLookShown || (p.look == kLookCombat && in_battle) || (p.look == kLookPeace && !in_battle);
        bool case_shown = shown; // (a weapon's case: with its look, and while the weapon is drawn)
        bool drawn = false;
        int ch_case = 0;
        std::vector<int> extras;
        if (is_weapon_slot(p.slot) || p.slot == kBackShieldSlot) { // (drawn: always shown; its parts are watched)
            if (U64 part = look_part(model, h); part && vc) {
                PartInfo& pi = g_part_info[{model, h}];
                U64 tab = rp(model + 0x58);
                U32 count = r32(model + 0x60);
                if (pi.part != part || pi.tab != tab || pi.count != count) { // (new to this model state: look it up)
                    std::string own = part_name(r32(part + 0x48));
                    if (own == "Bag") {
                        g_bag_weapon[vc] = {inv, p.slot};
                        static bool searched = false; // (their indices in the part-name table: searched once)
                        if (!searched)
                            for (U32 n = 1; n < 0x20000 && g_bag_tool_names.size() < std::size(kBagToolParts); ++n) {
                                std::string nm = part_name(n);
                                for (const char* t : kBagToolParts)
                                    if (nm == t) g_bag_tool_names.insert(n);
                            }
                        searched = true;
                    }
                    std::string names;
                    for (U64 q : look_parts(model, h)) {
                        bool fresh = g_weapon_parts.insert_or_assign({vc, r32(q + 0x48)}, std::make_pair(inv, p.slot)).second;
                        if (fresh) names += " " + part_name(r32(q + 0x48));
                    }
                    if (!names.empty()) LOG("appearance: equipment-%u's parts watched for drawing:%s", p.slot, names.c_str());
                    pi = {part, tab, count, case_handle(model, own), extra_case_handles(model, own)};
                    log_model_parts(model, ch);
                }
                ch_case = pi.ch_case;
                extras = pi.extras;
            }
            if (auto it = g_drawn.find({inv, p.slot}); it != g_drawn.end() && it->second) drawn = true;
            if (g_bag_in_use == std::make_pair(inv, p.slot)) drawn = true;
            if (auto it = g_flight_tool.find({inv, h}); it != g_flight_tool.end() && it->second) drawn = true;
            case_shown = shown || drawn;
        }
        int by = flying ? -1 : game_hides(model, h, look_part(model, h));
        if ((shown || drawn) && (flying || by >= 0)) { // (the game's mechanics win; released below)
            state += flying ? " left to the game (flight)" : " left to the game (its layer " + std::to_string(by) + " hides it)";
            continue;
        }
        // (mid outfit change: only hiding; what is shown waits for the model swap, kept as it was meanwhile)
        if (!(hold && (shown || drawn)) && look_call(model, h, shown || drawn ? 1 : 0)) now[p.slot] = h;
        if (shown || drawn) forced.insert(h);
        state += shown || drawn ? hold ? " shown once on" : " shown" : " hidden";
        if (drawn && !shown) state += " (drawn)";
        if (ch_case && !(hold && case_shown) && look_call(model, ch_case, case_shown ? 1 : 0)) {
            now[static_cast<U16>(p.slot | 0x100)] = ch_case;
            state += case_shown ? ", case shown" : ", case hidden";
        }
        // (a weapon's parts of their own - the bow's quiver and arrows, a bag tool's straps: hidden with it, otherwise
        // left to the game: forcing the straps shown on our layer did not show them, the game's own rule does)
        for (size_t x = 0; x < extras.size() && x < 8; ++x) {
            if (case_shown) { if (!hold) look_call(model, extras[x], 2); }
            else if (look_call(model, extras[x], 0)) now[static_cast<U16>(p.slot | (0x200 + 0x100 * x))] = extras[x];
            if (x == 0) state += case_shown ? ", its parts left to the game" : ", its parts hidden";
        }
    }
    for (const auto& [slot, h] : applied) // (handles let go of since: the flight's too; mid change: kept till the swap)
        if (auto it = now.find(slot); it == now.end() || it->second != h) {
            if (hold) now.emplace(slot, h);
            else look_call(model, h, 2);
        }
    applied = std::move(now);
    g_look_wanted[inv] = std::move(wanted);
    g_look_forced[inv] = std::move(forced);
    if (in_battle) state += " (in battle)";
    if (hold) state += " (outfit changing: until the model swaps)";
    std::string& last = g_look_state[inv];
    if (state != last) {
        last = state;
        LOG("appearance: character %u:%s", ch, state.empty() ? " nothing to do" : state.c_str());
    }
}

// The battle state itself (the headgear decision asks it: kRvaBattleCheck(actor)); -1 when it can't be asked.
using BattleCheckFn = std::uint8_t(__fastcall*)(U64 actor);
BattleCheckFn g_battle_check = nullptr;
int in_battle_of(U64 inv)
{
    U64 actor = rp(inv + 8);
    if (!g_battle_check || !actor) return -1;
    __try {
        return g_battle_check(actor) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void __fastcall battle_vis_hook(U64 inv, std::uint8_t show, std::uint8_t show2)
{
    g_battle_vis_orig(inv, show, show2);
    int battle = in_battle_of(inv); // (its argument is the headgear option's verdict: "always shown" makes it 1 for good)
    look_sync(inv, battle >= 0 ? battle != 0 : show != 0);
}

// The game's own layer calls: the flight cloak's layer 2 going on / off (take-off / landing) on a character's model.
// It is told apart from the equipment by its handle (no equipment slot has it). Synced on the next UI frame.
VisSetFn g_vis_set_orig = nullptr;
VisReleaseFn g_vis_release_orig = nullptr;
// A game layer call on a character's model: the flight cloak's layer going on / off, or another layer on a piece with a
// look (a mechanic hiding / showing it: the look yields or comes back). Synced on the next UI frame.

void game_layer(U64 model, U64 list, std::uint8_t layer, bool shown, bool release, U64 from)
{
    if (layer == kLookLayer || layer == kHeadgearLayer) return;
    U64 p = rp(list);
    U32 n = r32(list + 8);
    if (!p || !n) return;
    bool frame = layer == kDisplayLayer && kRvaFrameShowPart && from - (g_base + kRvaFrameShowPart) < 0x400;
    std::lock_guard<std::mutex> lock(g_appear_mu);
    // (the rocket pack is plain equipment: the game makes no call for it. Its flight shows the flight helmet, on this layer)
    if (auto m = g_look_model_inv.find(model); layer == kDisplayLayer && m != g_look_model_inv.end())
        for (U32 i = 0; i < n && i < 64; ++i) {
            U64 part = look_part(model, static_cast<int>(r32(p + i * 4)));
            if (!part || part_name(r32(part + 0x48)) != "Helm_Flight") continue;
            for (U16 sl = 0; sl < 32; ++sl) {
                int t = look_handle(m->second, sl);
                U64 tool = t ? look_part(model, t) : 0;
                if (!tool || part_name(r32(tool + 0x48)) != "Bag_Rocket") continue;
                bool on = shown && !release, &f = g_flight_tool[{m->second, t}];
                if (f != on && std::find(g_look_pending.begin(), g_look_pending.end(), m->second) == g_look_pending.end())
                    g_look_pending.push_back(m->second);
                if (on && !f) { // (at once, in vis_set_hook: the next UI frame can come late at take-off)
                    t_flight_show = {model, t};
                    t_flight_release = g_look_forced[m->second];
                }
                f = on;
                g_flying[m->second] = on; // (like the flight cloak: forced pieces go back to the game, which hides the helm)
                break;
            }
            break;
        }
    if (layer == kDisplayLayer && !frame) return; // (the weapon display settings: below our looks)
    if (frame)
        for (U32 i = 0; i < n && i < 64; ++i) {
            int h = static_cast<int>(r32(p + i * 4));
            if (shown || release) g_frame_hidden.erase({model, h});
            else g_frame_hidden.insert({model, h});
        }
    auto m = g_look_model_inv.find(model);
    if (m == g_look_model_inv.end()) return;
    U64 inv = m->second;
    bool queue = false;
    if (layer == kFlightLayer) {
        int h = static_cast<int>(r32(p));
        bool equipment = false;
        for (U16 sl = 0; sl < 32 && !equipment; ++sl) equipment = look_handle(inv, sl) == h;
        if (!equipment) { // (the flight cloak: no equipment slot has its handle)
            bool on = shown && !release, &f = g_flying[inv];
            queue = f != on;
            f = on;
        }
    }
    if (!queue) // (one of our pieces? its look may yield / come back)
        if (auto a = g_look_wanted.find(inv); a != g_look_wanted.end())
            for (U32 i = 0; i < n && i < 64 && !queue; ++i) queue = a->second.count(static_cast<int>(r32(p + i * 4)));
    if (queue && std::find(g_look_pending.begin(), g_look_pending.end(), inv) == g_look_pending.end())
        g_look_pending.push_back(inv);
}
std::uint8_t __fastcall vis_set_hook(U64 model, U64 list, std::uint8_t shown, std::uint8_t layer, U64 a, U64 b, std::uint8_t c)
{
    std::uint8_t r = g_vis_set_orig(model, list, shown, layer, a, b, c);
    game_layer(model, list, layer, shown != 0, false, reinterpret_cast<U64>(_ReturnAddress()));
    if (t_flight_show.first) {
        auto [m, h] = t_flight_show;
        t_flight_show = {};
        for (int f : t_flight_release) look_call(m, f, 2);
        t_flight_release.clear();
        look_call(m, h, 1);
    }
    return r;
}
std::uint8_t __fastcall vis_release_hook(U64 model, U64 list, std::uint8_t layer, std::uint8_t a, std::uint8_t b)
{
    std::uint8_t r = g_vis_release_orig(model, list, layer, a, b);
    game_layer(model, list, layer, false, true, reinterpret_cast<U64>(_ReturnAddress()));
    return r;
}

// The game's in/out check (drawing / sheathing, per part): a weapon with a look drawn or put away gets its looks synced
// on the next UI frame (not here: the game is in the middle of laying the character's parts out).
using EquipVisCheckFn = float(__fastcall*)(U64 vc, const U32* part, std::uint8_t* rec, std::uint8_t drawn);
EquipVisCheckFn g_equip_vis_check_orig = nullptr;
float __fastcall equip_vis_check_hook(U64 vc, const U32* part, std::uint8_t* rec, std::uint8_t drawn)
{
    float r = g_equip_vis_check_orig(vc, part, rec, drawn);
    U32 name = 0;
    if (!part || !rd(reinterpret_cast<U64>(part), name)) return r;
    std::lock_guard<std::mutex> lock(g_appear_mu);
    if (g_weapon_parts.empty()) return r;
    auto it = g_weapon_parts.find({vc, name});
    std::pair<U64, U16> owner;
    if (it != g_weapon_parts.end()) owner = it->second, g_part_drawn[{vc, name}] = drawn == 1;
    else if (auto b = g_bag_weapon.find(vc); b != g_bag_weapon.end() && g_bag_tool_names.count(name)) {
        owner = b->second;
        g_bag_part_drawn[{vc, name}] = drawn == 1;
    } else {
        return r;
    }
    bool now = false; // (drawn while any of its parts is; a bag item also while the tool's parts are)
    for (const auto& [key, own] : g_weapon_parts)
        if (key.first == vc && own == owner) now |= g_part_drawn[key];
    if (auto b = g_bag_weapon.find(vc); b != g_bag_weapon.end() && b->second == owner)
        for (U32 n : g_bag_tool_names) now |= g_bag_part_drawn[{vc, n}];
    bool& d = g_drawn[owner];
    if (d == now) return r;
    d = now;
    U64 inv = owner.first;
    if (std::find(g_look_pending.begin(), g_look_pending.end(), inv) == g_look_pending.end()) g_look_pending.push_back(inv);
    return r;
}

// The model rebuild job's swap (a job thread): pieces put on outside the wheel (the Armor tab, the game) get their new
// model parts here, without a headgear switch; their character is synced on the next UI frame.
using ModelSwapFn = void(__fastcall*)(U64 job, U64, U64, char, std::uint8_t*);
ModelSwapFn g_model_swap_orig = nullptr;
std::atomic<bool> g_worn_dirty{true}; // a model swapped (gear changed somewhere): the wheel rechecks what is worn
std::atomic<bool> g_anchor_dirty{false}; // ... and, with the menu closed, the anchor (anchor_recheck)
void __fastcall model_swap_hook(U64 job, U64 a, U64 b, char c, std::uint8_t* state)
{
    g_model_swap_orig(job, a, b, c, state);
    g_worn_dirty = true;
    g_anchor_dirty = true;
    U64 model = rp(rp(rp(job + 8) + 8) + kOffModelRoot);
    std::lock_guard<std::mutex> lock(g_appear_mu);
    auto it = g_look_model_inv.find(model);
    if (it == g_look_model_inv.end()) return;
    g_look_swapped.insert(it->second);
    if (std::find(g_look_pending.begin(), g_look_pending.end(), it->second) == g_look_pending.end())
        g_look_pending.push_back(it->second);
}

// UI thread, every frame: the characters whose model was rebuilt.
void look_pending()
{
    struct Todo {
        U64 inv;
        bool battle, swapped;
    };
    std::vector<Todo> todo;
    {
        std::lock_guard<std::mutex> lock(g_appear_mu);
        if (g_look_pending.empty()) return;
        for (U64 inv : g_look_pending) todo.push_back({inv, g_in_battle[inv], g_look_swapped.count(inv) != 0});
        g_look_pending.clear();
        g_look_swapped.clear();
    }
    for (const Todo& t : todo) look_sync(t.inv, t.battle, t.swapped);
}

// After the looks or the worn outfit changed (UI thread): the player now, the others on their next switch.
U64 player_inventory()
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 inv = actor ? rp(rp(actor + kOffActorComp) + kOffCompInventory) : 0;
    if (ctx[0]) release_player(ctx);
    return inv;
}

// The player's outfit is about to change (true: looks only hide until the model swaps) or didn't after all (false).
void look_hold(bool on)
{
    U64 inv = player_inventory();
    if (!inv) return;
    std::lock_guard<std::mutex> lock(g_appear_mu);
    g_look_swap_seen.erase(inv);
    if (on) g_look_hold.insert(inv);
    else g_look_hold.erase(inv);
}

void look_refresh();
// The player's outfit is still going on the model (from the requests until its last part swapped in).
bool look_changing()
{
    U64 inv = player_inventory();
    std::lock_guard<std::mutex> lock(g_appear_mu);
    return inv && g_look_hold.count(inv);
}

// A load is over: a hold with no part left loading (none came, or all are in) lets go.
void look_settled()
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 inv = actor ? rp(rp(actor + kOffActorComp) + kOffCompInventory) : 0;
    U64 model = actor ? look_model(actor) : 0;
    if (ctx[0]) release_player(ctx);
    if (!inv || !model) return;
    {
        std::lock_guard<std::mutex> lock(g_appear_mu);
        if (!g_look_hold.count(inv)) return;
        if (model_loading(model)) return; // (its last swaps let go)
        g_look_hold.erase(inv), g_look_swap_seen.erase(inv);
    }
    look_refresh();
}

void look_refresh()
{
    U64 inv = player_inventory();
    if (!inv) return;
    bool battle = false;
    {
        std::lock_guard<std::mutex> lock(g_appear_mu);
        if (auto it = g_in_battle.find(inv); it != g_in_battle.end()) battle = it->second;
    }
    look_sync(inv, battle);
}

// Marks outfit `index` (or none: -1) as the current character's last loaded one; saves when that changes.
void mark_worn(int index)
{
    bool changed = false;
    for (size_t i = 0; i < g_outfits.size(); ++i)
        if (g_outfits[i].worn != (static_cast<int>(i) == index)) g_outfits[i].worn = static_cast<int>(i) == index, changed = true;
    if (!changed) return;
    LOG("appearance: last loaded outfit is now %d", index);
    save_outfits(); // (publishes the looks)
    look_refresh();
}

// A model swapped with the menu closed (gear changed in the inventory, the game): an exact match moves the anchor,
// leaving it lets its looks go. With no armour read at all nothing is decided (the next menu open does).
void anchor_recheck()
{
    if (GetCurrentThreadId() != g_ui_thread || g_menu_open || !g_anchor_dirty.exchange(false)) return;
    if (g_character == kNoCharacter || current_character() != g_character || look_changing()) return;
    WornRaw w[kWornMax];
    int n = read_worn(0, w, kWornMax);
    bool armour = false;
    for (int k = 0; k < n; ++k) armour |= w[k].key != 0xFFFF && !is_weapon_slot(w[k].slot) && !is_accessory_slot(w[k].slot);
    if (!armour) return;
    int exact = find_worn_outfit(0);
    if (exact != g_worn_outfit) g_worn_outfit = exact, g_worn_changed = true;
    if (exact >= 0) mark_worn(exact);
    else if (int a = anchor_outfit(); a >= 0 && left_anchor(g_outfits[size_t(a)], w, n)) {
        LOG("\"%s\": too much changed, what is worn is a new set", g_outfits[size_t(a)].name.c_str());
        mark_worn(-1);
    }
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
extern DWORD g_ui_thread;

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

// --- Bags and settings --------------------------------------------------------------------------
// One inventory for all three characters (each wears its own gear). Bag ids: 1 carried items, 4 key items (both "on
// you"), 7 private storage, 14 wardrobe, 13 a hidden bag (never used); the others hold no gear.
// User settings (the "+" slot's settings view).
bool g_stash = false;               // after a load, saved pieces left in the bag go to storage (off until turned on)
bool stash_of(const Outfit& o) { return o.stash ? o.stash == 1 : g_stash; } // (its own, or the global)
bool g_take_from_companions = true; // pieces another character wears: take them (asked first)
bool g_messages = true;             // the after-the-fact reports (pieces left, not taken, overwritten); "+" settings page
std::string g_language;             // the mod's language: a lang::kLanguages code, "" = the game's (detected at start)
std::wstring g_lang_dir;            // the language files: <plugin folder>/Outfit Manager/lang/
std::string g_detected_language = "en"; // the game's, read at start (its options file, Steam)

// Loads the chosen language (the setting, else the game's) unless it is loaded already.
void apply_language()
{
    std::string want = g_language.empty() ? g_detected_language : g_language;
    if (want != lang::code()) lang::load(g_lang_dir, want);
}

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

// Character keys -> names (keys are game data, the same in every save).
const char* character_name(U16 key)
{
    switch (key) {
    case 0: return lang::t("character.kliff");
    case 3: return lang::t("character.damiane");
    case 5: return lang::t("character.oongka");
    }
    return lang::t("character.other");
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
std::string item_name(U16 key); // "" if unknown
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
struct DeferredEquip {
    U64 uid;
    U16 slot, key;
    std::string what; // "Helmet (Wooden Mask...)"
    std::string from; // whose it was
    bool held = false; // refused while another piece held its slot: put on once the game lets go of the slot
};
// A worn piece a load takes off or replaces: after the load it must still be worn or in a bag (lost_check).
struct LeavingPiece {
    U64 uid;
    U16 key;
    U16 slot;
};
struct AsyncLoad {
    std::string outfit; // checked: the list may change meanwhile
    std::vector<U16> leave;
    std::vector<DeferredEquip> equips;
    U64 started = 0;
    bool active = false;
    std::vector<LeavingPiece> leaving;
};
void lost_check(U64 self, const std::string& outfit, const std::vector<LeavingPiece>& leaving); // (below)
AsyncLoad g_async;
struct Outfit;
void stash_pieces(U64 self, const Outfit* loaded); // (when a load is done)
void stash_verify(U64 self);

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
bool stash_pending(U64 uid); // (a piece the stash just moved: its own check follows it, not the guess below)

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

std::string item_name(U16 key); // "" if unknown

// --- Item names: the game's item info table (as its own item-name text does) ---
U64 kRvaItemInfo = 0x3dcfc0;        // (unused, u32 key) -> item info; unknown keys -> the default info
U64 kRvaDefaultItemInfo = 0x7148c08; // pointer to that default info
using ItemInfoFn = U64(__fastcall*)(U64, U32); // (unused, key)
using LocTextFn = const char*(__fastcall*)(U64, U64);
ItemInfoFn g_item_info = nullptr; // set by install() after a prologue check
std::map<U16, std::string> g_item_names;

const char* item_name_raw(U16 key)
{
    __try {
        U64 info = g_item_info(0, key);
        if (!info || info == *reinterpret_cast<U64*>(g_base + kRvaDefaultItemInfo)) return nullptr;
        U64 text = *reinterpret_cast<U64*>(info + 0x90); // localized name, vfunc 1 (text, 0) -> UTF-8
        if (!text) return nullptr;
        return reinterpret_cast<LocTextFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(text) + 8))(text, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::string item_name(U16 key)
{
    if (!g_item_info || key == 0xFFFF) return {};
    if (auto f = g_item_names.find(key); f != g_item_names.end()) return f->second;
    const char* s = item_name_raw(key);
    std::string n = s ? std::string(s, strnlen(s, 256)) : std::string();
    LOG("item %u: name \"%s\"", key, n.c_str());
    return g_item_names[key] = n;
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
void report(U64 self, const std::string& text); // (a message: unless messages are off)
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

// --- Centre tooltip -----------------------------------------------------------------------------
// Focusing a saved outfit reuses the centre tooltip (dark disc) for its name: the item-only parts are
// hidden (only those the game had showing, restored on the next focus / tab change / close). The worn
// outfit keeps (or gets) the game's "Equipped" tag above its name. While one of our submenu pages is open the
// highlighted entry is named there and explained in the item-count caption below the line (g_center_desc).
const char* const kTooltipEquipped = "cpp-item-equipped";
const char* const kTooltipCount = "cpp-item-info-item-count";
const char* const kTooltipItemParts[] = {kTooltipEquipped, "cpp-item-tooltip-stat-set",
                                         "cpp-item-tooltip-durabiltiy-gauge-wrap", "quickslot-buff-stat-wrap",
                                         kTooltipCount};
std::string g_center_desc; // the caption's text ("" : hidden, as for an outfit)
std::vector<U64> g_hidden_parts;
std::vector<U64> g_shown_parts; // parts the game had hidden that we show
std::string g_center_name;
int g_center_outfit = -1;
int g_center_frames = 0; // re-assert the name for a while (a deferred tooltip refresh may overwrite it)
// Restores the engine skipped (element mid-restyle), retried every frame on any tab; otherwise the part (e.g. the
// Equipped tag) stayed hidden in the Weapons / Armor wheels too.
std::vector<U64> g_unhide_retry, g_rehide_retry;

void retry_restore()
{
    auto undo = [](std::vector<U64>& v, bool hide) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [hide](U64 e) { return !is_element(e) || set_class(e, "!cpp-none", hide); }),
                v.end());
    };
    undo(g_unhide_retry, false);
    undo(g_rehide_retry, true);
}

void center_restore()
{
    g_unhide_retry.insert(g_unhide_retry.end(), g_hidden_parts.begin(), g_hidden_parts.end());
    g_rehide_retry.insert(g_rehide_retry.end(), g_shown_parts.begin(), g_shown_parts.end());
    g_hidden_parts.clear();
    g_shown_parts.clear();
    g_center_frames = 0;
    retry_restore();
    if (!g_unhide_retry.empty() || !g_rehide_retry.empty())
        LOG("centre restore deferred (mid-restyle): %zu to show, %zu to hide", g_unhide_retry.size(),
            g_rehide_retry.size());
}

// A part still waiting for its restore that gets hidden (shown) again is ours again instead.
bool take_retry(std::vector<U64>& v, U64 e)
{
    auto it = std::find(v.begin(), v.end(), e);
    if (it == v.end()) return false;
    v.erase(std::remove(v.begin(), v.end(), e), v.end());
    return true;
}

void set_tooltip_state(U64 tip, U64 state_rva)
{
    U32 state = r32(g_base + state_rva);
    if (r32(tip + 0xa8) != state) g_set_state(tip, &state, 0);
}

bool g_center_logged = false;

void center_show(U64 self)
{
    U64 tip = rp(self + kOffRootCenterTip);
    U64 te = tip ? rp(tip + 8) : 0;
    if (!te || !is_element(te)) return;
    U64 name = find_by_class(te, "quickslot-center-tooltip-name", 12);
    if (name) g_set_text(name, g_center_name.c_str(), 0);
    for (const char* part : kTooltipItemParts) {
        U64 e = find_by_class(te, part, 12);
        if (!e) continue;
        bool desc = part == kTooltipCount && !g_center_desc.empty();
        if (part == kTooltipCount) {
            if (desc) g_set_text(e, g_center_desc.c_str(), 0);
            set_class(e, "om-desc", desc);
        }
        if ((part == kTooltipEquipped && g_center_outfit >= 0 && g_center_outfit == g_worn_outfit) || desc) {
            if (take_retry(g_rehide_retry, e)) {
                g_shown_parts.push_back(e);
            } else if (take_retry(g_unhide_retry, e)) { // the game had it showing anyway
                if (!set_class(e, "!cpp-none", false)) g_unhide_retry.push_back(e);
            } else if (has_class(e, "!cpp-none") && set_class(e, "!cpp-none", false)) {
                g_shown_parts.push_back(e);
            }
            static bool logged = false;
            if (!logged && !desc) logged = true, LOG("Equipped tag shown: %s", describe(e).c_str());
        } else if (take_retry(g_unhide_retry, e)) {
            g_hidden_parts.push_back(e);
        } else if (take_retry(g_rehide_retry, e)) { // the game had it hidden anyway
            if (!set_class(e, "!cpp-none", true)) g_rehide_retry.push_back(e);
        } else if (!has_class(e, "!cpp-none") && set_class(e, "!cpp-none", true)) {
            g_hidden_parts.push_back(e);
        }
    }
    set_tooltip_state(tip, kRvaStateShow);
    if (!g_center_logged) {
        g_center_logged = true;
        LOG("centre tooltip %s; name elem %llX, state %X", describe(te).c_str(), name, r32(tip + 0xa8));
    }
}

void center_hide(U64 self)
{
    center_restore();
    if (U64 tip = rp(self + kOffRootCenterTip)) set_tooltip_state(tip, kRvaStateHide);
}

// --- Focus and keys -----------------------------------------------------------------------------
// Index of a slot script in the Outfits tab (kOutfitSlots if not there).
U32 outfit_slot_index(U64 self, U64 script)
{
    U32 tab = g_fake_key.load();
    U64 sv = rp(self + kOffRootSlotVecs);
    if (!script || tab == 0xFFFF || !sv) return kOutfitSlots;
    U64 arr = rp(sv + tab * 0x10ull);
    U32 cnt = r32(sv + tab * 0x10ull + 8);
    for (U32 i = 0; arr && i < cnt && i < kOutfitSlots; ++i)
        if (rp(arr + i * 8ull) == script) return i;
    return kOutfitSlots;
}

U64 focused_script(U64 self)
{
    std::uint8_t mode = 0;
    if (!rd(self + kOffRootFocusIdx, mode)) return 0;
    __try {
        return g_focused_slot(self, mode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Shows the focused saved outfit's name in the centre, or what the "+" slot does.
void center_for_focus(U64 self, U64 script, bool hide_otherwise)
{
    U32 i = outfit_slot_index(self, script);
    if (g_cfg >= 0) { // the settings view: the setting, its value, what it does
        std::vector<std::uint8_t> e = cfg_entries();
        U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
        if (k >= e.size()) {
            if (hide_otherwise) center_hide(self);
            return;
        }
        U32 cur = 0;
        std::vector<const char*> v = cfg_values(e[k], cur);
        g_center_name = cfg_title(e[k]);
        g_center_desc = (v.empty() ? std::string() : cfg_value_text(e[k], v[cur]) + ": ") + cfg_description(e[k]);
        g_center_outfit = -1;
        g_center_frames = 30;
        center_show(self);
        return;
    }
    if (g_eye >= 0 && size_t(g_eye) < g_outfits.size()) { // the Appearance view: the piece and its look / the page step
        std::vector<size_t> pieces = eye_pieces();
        U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
        static const char* const kLookText[kLookCount] = { // (by Look value)
            "look.default", "look.hidden", "look.combat", "look.peace", "look.shown"};
        static const char* const kStepName[kEyeSteps] = {"page.armour", "page.weapons", "page.accessories"};
        if (k < pieces.size()) {
            OutfitItem tmp{};
            const OutfitItem& it = *eye_item(pieces[k], tmp);
            std::string nm = it.key != 0xFFFF ? item_name(it.key) : std::string();
            if (pieces[k] == kBackPiece)
                nm = nm.empty() ? std::string(lang::t("piece.back_shield")) : lang::f("piece.back_shield_item", {{"item", nm}});
            g_center_name = nm.empty() ? std::string(lang::t("piece.empty")) : nm;
            g_center_desc = lang::t(kLookText[it.look < kLookCount ? it.look : 0]);
        } else if (k == pieces.size()) {
            g_center_name = lang::t(kStepName[g_eye_pick]);
            g_center_desc = lang::t(g_eye_pick == g_eye_step ? "page.this" : "page.open");
        } else if (k == pieces.size() + 1) {
            g_center_name = lang::t("center.back_settings");
            g_center_desc = lang::t("desc.back_settings");
        } else {
            if (hide_otherwise) center_hide(self);
            return;
        }
        g_center_outfit = -1;
        g_center_frames = 30;
        center_show(self);
        return;
    }
    U32 o = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (o < kOutfitSlots) {
        std::uint8_t mode = g_slot_mode[i];
        bool saved = o < g_outfits.size();
        g_center_name = mode == kModeSettings ? std::string(lang::t(saved ? "center.outfit_settings" : "center.settings"))
                        : saved && g_outfits[o].worn && g_worn_outfit != int(o) ? lang::f("center.modified", {{"outfit", g_outfits[o].name}})
                        : saved                              ? g_outfits[o].name
                        : g_outfits.size() >= kMaxOutfits    ? std::string(lang::t("center.limit"))
                                                             : std::string(lang::t("center.save"));
        g_center_outfit = saved && mode == kModeMain ? int(o) : -1;
        g_center_frames = 30;
        center_show(self);
    } else {
        g_center_outfit = -1;
        if (hide_otherwise) center_hide(self);
    }
}

// Key presses are latched by the key thread (tick of the press) and handled on the UI thread.
std::atomic<U64> g_space_at{0};
std::atomic<bool> g_menu_open{false};
DWORD g_ui_thread = 0;

bool take_press(std::atomic<U64>& at)
{
    U64 t = at.exchange(0);
    return t && GetTickCount64() - t < 500;
}

void refresh_wheel(U64 self)
{
    // (a load from the quick peek passes the status screen: no wheel to refresh, and its layout check would read the
    // status screen as a broken wheel and turn the mod off)
    if (self != g_outfits_root) return;
    if (g_applied_visible != visible_slots()) reset_modes(); // (the slots moved)
    apply_outfit_slots(self);
    sync_ring(self);
}

// --- Appearance view: entering, paging, leaving, and the presses in it -----------------------------
void look_refresh();

void eye_refresh(U64 self)
{
    g_force_images = 3; // (the game resets the focused slot's picture: set again for a few frames)
    if (g_eye >= 0) { // (the page's pieces, for the log)
        std::string s;
        for (size_t k : eye_pieces()) {
            OutfitItem tmp{};
            const OutfitItem& it = *eye_item(k, tmp);
            s += " equipment-" + std::to_string(it.slot) + " \"" + (it.key != 0xFFFF ? item_name(it.key) : std::string()) + "\"";
        }
        LOG("appearance view page %u:%s", g_eye_step, s.empty() ? " (no pieces)" : s.c_str());
    }
    refresh_wheel(self);
    center_for_focus(self, focused_script(self), true);
}

void eye_enter(U64 self, U32 o)
{
    g_eye = static_cast<int>(o), g_eye_step = 0, g_eye_pick = 0;
    LOG("appearance view: \"%s\"", g_outfits[o].name.c_str());
    eye_refresh(self);
}

void cfg_enter(U64 self, U32 o);
void eye_leave(U64 self)
{
    LOG("appearance view: back to the settings");
    U32 o = static_cast<U32>(g_eye);
    g_eye = -1, g_eye_step = 0;
    g_center_desc.clear();
    cfg_enter(self, o); // (entered from there)
}

// A scroll on slot i of the view: a piece's look moves on / back (Default, Hidden, Combat only, Out of combat only,
// Shown: its dots), the page slot chooses Armour / Weapons / Accessories (a click opens it); Back doesn't scroll.
void eye_scroll(U64 self, U32 i, int step)
{
    std::vector<size_t> pieces = eye_pieces();
    U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (k < pieces.size()) {
        Outfit& o = g_outfits[size_t(g_eye)];
        std::uint8_t& look = eye_look(pieces[k]);
        look = kLookOrder[(look_pos(look) + kLookCount + step) % kLookCount];
        OutfitItem tmp{};
        const OutfitItem& it = *eye_item(pieces[k], tmp);
        LOG("appearance: \"%s\" equipment-%u %s -> look %u", o.name.c_str(), it.slot, (it.key != 0xFFFF ? item_name(it.key) : std::string("(empty)")).c_str(), look);
        save_outfits(); // (publishes the looks)
        look_refresh(); // (the worn outfit's pieces change at once)
        apply_outfit_slots(self, i);
        center_for_focus(self, focused_script(self), true);
    } else if (k == pieces.size()) { // (the page slot: scrolling chooses, a click opens it)
        g_eye_pick = std::uint8_t((g_eye_pick + kEyeSteps + step) % kEyeSteps);
        apply_outfit_slots(self, i);
        center_for_focus(self, focused_script(self), true);
    }
}

// A click / Space on slot i of the view: the page slot opens the page chosen (or the next one), Back returns to the
// settings (pieces: scrolled).
void eye_press(U64 self, U32 i)
{
    std::vector<size_t> pieces = eye_pieces();
    U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (k == pieces.size()) {
        g_eye_step = g_eye_pick = g_eye_pick != g_eye_step ? g_eye_pick : std::uint8_t((g_eye_step + 1) % kEyeSteps);
        LOG("appearance view: step %u", g_eye_step);
        eye_refresh(self);
    } else if (k == pieces.size() + 1) {
        eye_leave(self);
    }
}

// Shows text in the game's own toast notification (and the log).
void notify(U64 self, const std::string& text)
{
    LOG("notify: %s", text.c_str());
    if (!g_toast) return;
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        if (ui) g_toast(ui, text.c_str(), 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("notify: toast faulted");
    }
}

// A report after the fact (pieces left, not taken, overwritten): silenced by "Messages: Off" (still logged).
void report(U64 self, const std::string& text)
{
    if (g_messages) notify(self, text);
    else LOG("(message off) %s", text.c_str());
}

// --- Rename prompt: the game's own TextEditModalMessage (as its naming prompts open it) ----------
// Confirm with text renames the outfit just saved; an empty confirm, cancel, Esc or closing the menu keep "Outfit N".
std::atomic<U64> g_name_modal{0}; // the open prompt (0: none)
U64 g_name_self = 0;       // the gear menu it was opened from
std::string g_name_default;
size_t g_name_index = 0;   // the outfit being named (checked against g_name_default)
U64 g_name_opened = 0;     // tick
U64 g_modal_ended = 0;      // tick: presses made while typing are dropped for a moment after the prompt closes
unsigned char g_name_shown = 0; // modal +0xbd at open (the shown flag; the hide clears it)
alignas(16) unsigned char g_modal_params[0x1800];
static_assert(sizeof(g_modal_params) >= kParamsSize, "the modal parameter block does not fit");

// Object-free (for __try): the opened modal or 0.
U64 open_text_modal_raw(U64 self, const char* message)
{
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        U64 view = ui ? g_find_view(ui, "ModalMessageView") : 0;
        if (!view) return 0;
        g_params_init(reinterpret_cast<U64>(g_modal_params));
        strncpy_s(reinterpret_cast<char*>(g_modal_params + kParamsMessage), kParamsMessageCap, message, _TRUNCATE);
        *reinterpret_cast<U32*>(g_modal_params + kParamsMessageLen) = 0xFFFFFFFF; // length: recompute
        U64 modal = g_open_text_modal(0, view, g_modal_params, kNameMaxChars);
        g_params_free(reinterpret_cast<U64>(g_modal_params));
        if (modal && !has_vt(modal, kRvaTextModalVt)) return 0; // not the modal we know: leave it alone
        if (modal) *reinterpret_cast<U64*>(modal + kOffModalOwner) = 0; // no owner callback (a stale one from earlier use)
        return modal;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Object-free: the prompt's current text (empty if it can't be read).
void read_modal_text_raw(U64 modal, char* out, int size)
{
    out[0] = 0;
    __try {
        U64 input = *reinterpret_cast<U64*>(modal + kOffModalInput);
        if (input && *reinterpret_cast<unsigned char*>(input + kOffInputBusy) == 0)
            g_get_text(*reinterpret_cast<U64*>(input + kOffInputText), out, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
    out[size - 1] = 0;
}

// Object-free: closes the prompt the way its own buttons do.
void hide_modal_raw(U64 modal)
{
    __try {
        g_hide_script(modal);
        if (kOffModalD0)
            if (U64 d0 = *reinterpret_cast<U64*>(modal + kOffModalD0)) *reinterpret_cast<unsigned char*>(d0 + 0xf2) = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("rename: hide faulted");
    }
}

U64 call_click_raw(U64 modal, U64 a, U64 elem, U64 flag)
{
    __try {
        return g_text_click_orig(modal, a, elem, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("rename: game click handler faulted");
        return 0;
    }
}

bool name_invalid_raw(const char* s)
{
    __try {
        return g_name_invalid(s);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string clean_name(const std::string& in)
{
    std::string s;
    for (char c : in)
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) s += c; // no tabs / newlines (store format)
    size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// A prompt takes the input while it is open, so the gear menu misses the release of the button that holds it open
// and stays up afterwards. Once a prompt is gone, do what the game's release handler (0x1098f10, event 1) does.
using QueueHideFn = void(__fastcall*)(U64, U64);
QueueHideFn g_queue_hide = nullptr;
bool g_release_pending = false; // set when a prompt closes while the menu is open; done by pump on the next frame

// The input side of the same problem: the HUD key binding that holds the menu (0xcc40c0's binding: +0x232 "pressed")
// never sees its key go up either, so the next press only counts as the missing release. The hook remembers the binding
// whose down / held event left it pressed (with a copy of that event); after a prompt the game is handed that event
// again as an up (type 7 at +0x38) and runs its own release: binding state, hold progress, the HUD's release handler
// (which closes the menu).
using BindingInputFn = void(__fastcall*)(U64, U64, U64);
BindingInputFn g_binding_input_orig = nullptr;
constexpr U32 kInputEventCopy = 0x40; // the fields the state machine reads end at +0x3a
struct HeldBinding {
    U64 dispatcher = 0, binding = 0;
    alignas(16) unsigned char event[kInputEventCopy] = {};
};
std::map<U64, HeldBinding> g_pressed; // bindings whose key is down, by binding
U64 g_last_pressed = 0;               // the binding pressed last (at menu open: the one that opened it)
HeldBinding g_held;                   // the menu's binding, taken at menu open
std::mutex g_held_mu; // input may arrive on another thread than the menu's

// At menu open: the binding pressed last is the menu's button.
void note_menu_binding()
{
    std::lock_guard lock(g_held_mu);
    auto it = g_pressed.find(g_last_pressed);
    g_held = it != g_pressed.end() ? it->second : HeldBinding{};
}

// 1 = pressed by a down / held event, 2 = released (up), 0 = neither or unreadable.
int binding_change(U64 event, U64 binding, unsigned char* copy)
{
    __try {
        unsigned char type = *reinterpret_cast<unsigned char*>(event + kOffEventType);
        if ((type == 3 || type == 6) && *reinterpret_cast<unsigned char*>(binding + kOffBindingPressed)) {
            std::memcpy(copy, reinterpret_cast<const void*>(event), kInputEventCopy);
            return 1;
        }
        return type == 7 ? 2 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}


void __fastcall binding_input_hook(U64 dispatcher, U64 event, U64 binding)
{
    g_binding_input_orig(dispatcher, event, binding);
    if (!kOffEventType || !kOffBindingPressed) return; // (offsets doubtful after a game update)
    alignas(16) unsigned char copy[kInputEventCopy];
    int change = binding_change(event, binding, copy);
    if (!change) return;
    std::lock_guard lock(g_held_mu);
    if (change == 1) {
        HeldBinding& h = g_pressed[binding];
        h.dispatcher = dispatcher, h.binding = binding;
        std::memcpy(h.event, copy, kInputEventCopy);
        g_last_pressed = binding;
    } else {
        g_pressed.erase(binding);
        if (binding == g_held.binding) g_held = {};
    }
}

// 1 = released through the game's input, 0 = its up did arrive, -1 = fault.
int send_up(HeldBinding& h)
{
    __try {
        if (!*reinterpret_cast<unsigned char*>(h.binding + kOffBindingPressed)) return 0;
        if (!kOffEventType || kOffEventType >= kInputEventCopy) return -1; // layout doubtful / beyond our copy
        h.event[kOffEventType] = 7; // key up
        h.event[0x10] = 0; // "handled" (the state machine sets it)
        g_binding_input_orig(h.dispatcher, reinterpret_cast<U64>(h.event), h.binding);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// True if a held binding was released through the game's own input path.
bool release_held_binding()
{
    HeldBinding h;
    {
        std::lock_guard lock(g_held_mu);
        h = g_held;
        g_held = {};
        g_pressed.erase(h.binding);
    }
    if (!h.binding || !g_binding_input_orig) return false;
    int r = send_up(h);
    LOG("prompt closed: %s (binding %llX)", r > 0 ? "menu button released through the game's input" :
        r == 0 ? "menu button was already up" : "input release faulted", h.binding);
    return r > 0;
}

void release_menu_button(U64 self)
{
    if (release_held_binding()) return;
    if (!kOffPanelFlags || !kOffPanelHoldClose || !kOffPanelElem) return; // (doubtful after a game update)
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        U64 view = ui ? g_find_view(ui, "ItemQuickSlotView2") : 0;
        if (!view || (*reinterpret_cast<unsigned char*>(view + kOffViewFlags) & 0x60) != 0x40) return; // not shown
        U64 inner = *reinterpret_cast<U64*>(view + kOffViewInner);
        U64 panel = inner ? *reinterpret_cast<U64*>(inner + 0x10) : 0;
        if (!panel) return;
        auto flags = reinterpret_cast<unsigned char*>(panel + kOffPanelFlags);
        *flags &= 0xfe; // the button is up
        bool held_open = (*flags & 2) && *reinterpret_cast<unsigned char*>(panel + kOffPanelHoldClose); // opened by holding
        LOG("prompt closed: menu button released (%s)", held_open ? "menu closes" : "menu was opened by a tap: stays");
        if (held_open) {
            U64 elem = *reinterpret_cast<U64*>(panel + kOffPanelElem);
            if (!is_element(elem)) {
                LOG("prompt closed: the menu's view element is not where expected; not closing it");
                return;
            }
            g_queue_hide(*reinterpret_cast<U64*>(elem + 0x58), elem);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("prompt closed: menu button release faulted");
    }
}

void finish_naming(const std::string& text, const char* why)
{
    if (!g_name_modal) return;
    Busy busy;
    g_name_modal = 0;
    g_modal_ended = GetTickCount64();
    g_release_pending = g_menu_open;
    U64 self = g_name_self;
    std::string name = clean_name(text);
    bool here = g_name_index < g_outfits.size() && g_outfits[g_name_index].name == g_name_default;
    LOG("rename: %s, text \"%s\" -> \"%s\"%s", why, text.c_str(), name.c_str(), here ? "" : " (outfit gone)");
    if (!here || name.empty() || name == g_name_default) return;
    for (const Outfit& o : g_outfits)
        if (o.name == name) {
            if (g_menu_open) notify(self, lang::f("toast.name_taken", {{"outfit", name}}));
            return;
        }
    g_outfits[g_name_index].name = name;
    save_outfits();
    if (g_menu_open && outfits_current(self)) {
        center_restore();
        center_for_focus(self, focused_script(self), true);
    }
}

// Slot 113 of the TextEdit modal (its button / key handler): only our prompt is handled here.
U64 __fastcall text_click_hook(U64 modal, U64 a, U64 elem, U64 flag)
{
    if (!g_name_modal || modal != g_name_modal || (flag & 0xFF)) return g_text_click_orig(modal, a, elem, flag);
    if (elem && elem == rp(modal + kOffModalConfirm)) {
        char buf[0x105] = {};
        read_modal_text_raw(modal, buf, sizeof buf);
        std::string text = buf;
        if (clean_name(text).empty()) { // the game ignores an empty confirm; here it keeps the default name
            hide_modal_raw(modal);
            finish_naming("", "confirmed empty");
            return 0;
        }
        if (name_invalid_raw(buf)) {
            LOG("rename: \"%s\" rejected by the game's name filter", buf);
            return call_click_raw(modal, a, elem, flag); // shows the game's error and keeps the prompt open
        }
        U64 r = call_click_raw(modal, a, elem, flag); // validates again and closes
        finish_naming(text, "confirmed");
        return r;
    }
    if (elem && elem == rp(modal + kOffModalCancel)) {
        U64 r = call_click_raw(modal, a, elem, flag);
        finish_naming("", "cancelled");
        return r;
    }
    return g_text_click_orig(modal, a, elem, flag);
}

// --- Typed characters the prompt drops -------------------------------------------------------------
// The game's text box makes its own characters from the keys: letters (shifted too) and digits, but no Shift / AltGr
// symbols (! + / * ...) or accented letters, though pasting them works. While our prompt is open the characters
// Windows makes for the game window (WM_CHAR) are collected; once typing pauses for two frames, the ones the box didn't
// take are put in at the caret (input +0xd8 / +0xdc: caret and selection end, in characters), which then moves past them
// (+0xf1: the caret-moved flag the game's own keys set, so the caret is drawn there at once).
bool game_focused();
WNDPROC g_game_wndproc = nullptr;
std::mutex g_typed_mu;
std::wstring g_typed;       // characters since the batch began
std::string g_typed_base;   // the box text on the frame before the batch
U32 g_typed_caret = 0;      // and its caret
int g_typed_quiet = 0;      // frames since the last character
std::string g_name_text;    // the box text on the last frame
U32 g_name_caret = 0;       // its caret

extern std::atomic<bool> g_peek_live;
extern std::atomic<int> g_peek_wheel;
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

// Object-free: the prompt's caret (false if it can't be read, or a selection is open).
bool read_modal_caret_raw(U64 modal, U32& caret)
{
    __try {
        U64 input = *reinterpret_cast<U64*>(modal + kOffModalInput);
        if (!input) return false;
        caret = *reinterpret_cast<U32*>(input + kOffInputCaret);
        return caret == *reinterpret_cast<U32*>(input + kOffInputCaretEnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Object-free: sets the prompt's text and caret (false if it can't be reached).
bool write_modal_text_raw(U64 modal, const char* text, U32 caret)
{
    __try {
        U64 input = *reinterpret_cast<U64*>(modal + kOffModalInput);
        if (!input || *reinterpret_cast<unsigned char*>(input + kOffInputBusy) != 0) return false;
        g_set_text(*reinterpret_cast<U64*>(input + kOffInputText), text, 0);
        *reinterpret_cast<U32*>(input + kOffInputCaret) = caret;
        *reinterpret_cast<U32*>(input + kOffInputCaretEnd) = caret;
        *reinterpret_cast<unsigned char*>(input + kOffInputCaretMoved) = 1; // as the game's own caret keys: redrawn now
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

size_t utf8_chars(const std::string& s)
{
    size_t n = 0;
    for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

// The byte offset of character `chars` (the end if past it).
size_t utf8_offset(const std::string& s, size_t chars)
{
    size_t i = 0;
    for (; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80 && chars-- == 0) break;
    return i;
}

void type_dropped_chars()
{
    char buf[0x105] = {};
    read_modal_text_raw(g_name_modal, buf, sizeof buf);
    U32 caret = 0;
    bool caret_ok = read_modal_caret_raw(g_name_modal, caret);
    std::string now = buf, base;
    std::wstring typed;
    U32 base_caret = 0;
    {
        std::lock_guard<std::mutex> lock(g_typed_mu);
        g_name_text = now;
        g_name_caret = caret_ok ? caret : 0xFFFFFFFF;
        if (g_typed.empty() || ++g_typed_quiet < 2) return;
        typed.swap(g_typed);
        base = g_typed_base;
        base_caret = g_typed_caret;
    }
    // What the game typed went in at the caret: the text around it must be as before (else: deleted / a selection /
    // edited in the same moment, and it is left as it is).
    size_t pos = base_caret == 0xFFFFFFFF ? std::string::npos : utf8_offset(base, base_caret);
    size_t tail = pos == std::string::npos ? 0 : base.size() - pos;
    if (pos == std::string::npos || now.size() < base.size() || now.compare(0, pos, base, 0, pos) != 0 ||
        now.compare(now.size() - tail, tail, base, pos, tail) != 0) {
        LOG("rename: typed characters skipped (text \"%s\" was \"%s\", caret %u)", now.c_str(), base.c_str(), base_caret);
        return;
    }
    std::string out = now;
    bool added = false;
    for (wchar_t c : typed) {
        char u[8] = {};
        int n = WideCharToMultiByte(CP_UTF8, 0, &c, 1, u, sizeof u, nullptr, nullptr);
        if (n <= 0) continue;
        if (out.compare(pos, size_t(n), u, size_t(n)) == 0) { pos += size_t(n); continue; } // the game took it
        if (utf8_chars(out) >= size_t(kNameMaxChars)) break;
        out.insert(pos, u, size_t(n));
        pos += size_t(n);
        added = true;
    }
    if (!added) return;
    U32 at = U32(utf8_chars(out.substr(0, pos)));
    if (write_modal_text_raw(g_name_modal, out.c_str(), at)) {
        LOG("rename: typed characters added: \"%s\" -> \"%s\"", now.c_str(), out.c_str());
        std::lock_guard<std::mutex> lock(g_typed_mu);
        g_name_text = out;
        g_name_caret = at;
    }
}

void open_rename_prompt(U64 self, size_t index)
{
    if (!g_open_text_modal || g_name_modal) return;
    std::string msg = lang::f("prompt.name", {{"outfit", g_outfits[index].name}});
    U64 modal = open_text_modal_raw(self, msg.c_str());
    if (!modal) {
        LOG("rename: prompt did not open; name stays \"%s\"", g_outfits[index].name.c_str());
        return;
    }
    g_name_modal = modal;
    g_name_self = self;
    g_name_index = index;
    g_name_default = g_outfits[index].name;
    g_name_opened = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_typed_mu);
        g_typed.clear();
        g_name_text.clear();
        g_name_caret = 0;
    }
    watch_game_chars();
    rd(modal + kOffModalShown, g_name_shown);
    unsigned char hiding = 0;
    rd(modal + kOffModalHidden, hiding);
    LOG("rename: prompt %llX open for \"%s\" (+0xbd %u, +0xbe %u, input %llX)", modal, g_name_default.c_str(),
        g_name_shown, hiding, rp(modal + kOffModalInput));
}

// While the prompt is open: notices it closing some other way (the shown flag drops, or a 3-minute safety stop).
void watch_rename_prompt()
{
    if (!g_name_modal) return;
    type_dropped_chars();
    unsigned char shown = g_name_shown;
    rd(g_name_modal + kOffModalShown, shown);
    if (g_name_shown && !shown) finish_naming("", "prompt closed elsewhere");
    else if (GetTickCount64() - g_name_opened > 180000) {
        hide_modal_raw(g_name_modal);
        finish_naming("", "timed out");
    }
}

// --- Questions before loading: the game's own yes / no CommonModalMessage --------------------------
// 1. Pieces another character wears (setting on): "... is worn by Oongka. Take it?" - Take: their unequip request, then
//    the piece is put on once it is in the bags (pump); Leave as is: those slots stay.
// 2. Pieces that are gone while another copy is owned: "Wear copy" (saved into the outfit) / "Leave as is".
// Slots left as they are get one notification saying why. Esc / closing the menu loads nothing.
struct Take {
    size_t k;     // outfit item index
    U16 who;      // character key
    U64 actor;    // their actor
    U16 worn_slot; // the slot they wear it in
};
struct Left {
    U16 slot;
    const char* why; // its line: left.storage, ... ({piece}, {who})
    U16 who = 0;     // left.stays_with: whose
};
struct AskedLoad {
    U32 index = 0;
    std::string name; // checked: the list may change while the question is open
    std::vector<std::pair<size_t, U64>> copies;
    std::vector<Take> takes;
    std::vector<Left> left;
    int stage = 0; // the open question: 1 takes, 2 copies
    int confirm = 0; // instead a yes / no from the outfit page: kConfirmOverwrite, kConfirmDelete, kConfirmSwap
    U32 other = 0;          // kConfirmSwap: the outfit worn now (checked by name, as index is)
    std::string other_name;
    bool takes_answered = false, take = false, copies_answered = false, use_copies = false;
};
std::atomic<U64> g_ask_modal{0}; // the open question (0: none)
U64 g_ask_self = 0;
U64 g_ask_opened = 0;
unsigned char g_ask_shown = 0;
AskedLoad g_pending;
enum AskAnswer { kAskAbort, kAskYes, kAskNo };

U64 open_common_modal_raw(U64 self, const char* title, const char* message, const char* yes, const char* no)
{
    __try {
        U64 ui = reinterpret_cast<UiMgrFn>(*reinterpret_cast<U64*>(*reinterpret_cast<U64*>(self) + kVfUiMgr))(self);
        U64 view = ui ? g_find_view(ui, "ModalMessageView") : 0;
        if (!view) return 0;
        unsigned char* p = g_modal_params;
        g_params_init(reinterpret_cast<U64>(p));
        strncpy_s(reinterpret_cast<char*>(p + kParamsTitle), kParamsLabelCap, title, _TRUNCATE);
        *reinterpret_cast<U32*>(p + kParamsTitleLen) = 0xFFFFFFFF;
        strncpy_s(reinterpret_cast<char*>(p + kParamsMessage), kParamsMessageCap, message, _TRUNCATE);
        *reinterpret_cast<U32*>(p + kParamsMessageLen) = 0xFFFFFFFF;
        // (an empty label hides its button)
        strncpy_s(reinterpret_cast<char*>(p + kParamsConfirm), kParamsLabelCap, yes, _TRUNCATE);
        strncpy_s(reinterpret_cast<char*>(p + kParamsCancel), kParamsLabelCap, no, _TRUNCATE);
        U64 modal = g_open_common_modal(0, view, p, reinterpret_cast<const char*>(g_base + kRvaCommonModalName));
        g_params_free(reinterpret_cast<U64>(p));
        if (modal && !has_vt(modal, kRvaCommonModalVt)) return 0; // not the modal we know: leave it alone
        if (modal) *reinterpret_cast<U64*>(modal + kOffModalOwner) = 0; // no owner callback
        return modal;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

U64 call_ask_click_raw(U64 modal, U64 a, U64 elem, U64 flag)
{
    __try {
        return g_ask_click_orig(modal, a, elem, flag);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("missing pieces: game click handler faulted");
        return 0;
    }
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

void ask_next(U64 self, AskedLoad p);
enum Confirm { kConfirmNone, kConfirmOverwrite, kConfirmDelete, kConfirmSwap, kConfirmCleanSave, kConfirmUninstall };

// --- Settings view: an outfit's settings (or the plugin's, from the "+" slot) on the wheel ---------------------------
// The second scroll mode of a slot opens it (no submenu). Its segments: a setting's name and value (its dots are the
// choices: scroll to change) or an action (the click prompt), then Back (left of the top). The centre names the
// highlighted setting, its value and what it does.
void open_rename_prompt(U64 self, size_t index);
void open_confirm(U64 self, U32 index, int what, U32 other);
void overwrite_pressed(U64 self, U32 index, bool reset);
void open_clean_confirm(U64 self, int what);

bool g_overwrite_reset = false; // the settings view's Overwrite: false keeps the outfit's settings, true resets them

enum CfgKind : std::uint8_t {
    kCfgVisibility, kCfgClimate, kCfgWeapons, kCfgStash, kCfgPosition, kCfgRename, kCfgOverwrite, kCfgDelete, // a saved outfit's
    kCfgGStash, kCfgParty, kCfgMessages, kCfgClear, kCfgUninstall,                               // the "+" slot's
    kCfgBack, kCfgLanguage, kCfgPeekKey, kCfgPeekPad, kCfgHotkey
};

// The Outfit Arc's key (the "+" page's Outfit Arc; Off by default: no key is free in every layout, so the player sets
// one). A click on the setting waits for the next key or mouse button (left / right excepted: they click), read with
// GetAsyncKeyState so it works however the game reads its input; Esc cancels, Backspace / Delete turn it off. Held: the
// arc shows; the wheel moves along it; released: the highlighted outfit goes on. The key and, while the arc is open, the
// wheel are kept from the game (raw_input_hook, name_wndproc).
std::atomic<bool> g_peek_capture{false}; // the setting waits for a key
int g_capture_outfit = -1;               // ... for that outfit's Hotkey (-1: for the Outfit Arc's key)
U32 g_peek_capture_slot = 0;             // its slot (refreshed when the key comes)
bool g_peek_capture_held[256] = {};      // keys already down when it started waiting (not taken as the answer)

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

std::vector<std::uint8_t> cfg_entries()
{
    if (g_cfg < 0) return {};
    if (size_t(g_cfg) < g_outfits.size())
        return {kCfgVisibility, kCfgClimate, kCfgWeapons, kCfgStash, kCfgHotkey, kCfgPosition, kCfgRename, kCfgOverwrite,
                kCfgDelete, kCfgBack};
    // (the Outfit Arc's binding for the input in use: the key, or the pad's; pump re-shows the page when it changes)
    return {kCfgGStash, kCfgParty, kCfgMessages, kCfgLanguage, g_pad_in_use ? kCfgPeekPad : kCfgPeekKey, kCfgClear,
            kCfgUninstall, kCfgBack};
}

const char* cfg_name(std::uint8_t k)
{
    static const char* const n[] = {"setting.visibility", "setting.climate", "setting.weapons", "setting.stash",
                                    "setting.position",   "setting.rename",  "setting.overwrite", "setting.delete",
                                    "setting.stash",      "setting.party",   "setting.messages", "setting.clear",
                                    "setting.uninstall",  "setting.back",    "setting.language", "setting.peekkey",
                                    "setting.peekpad",    "setting.hotkey"};
    return k <= kCfgHotkey ? lang::t(n[k]) : "";
}

// A text that stays where it is (the settings' values are kept as pointers between refreshes: a reused std::string's
// buffer moves once the text outgrows it, 2026-11-06: "Ctrl + Shift + 1" showed leftover memory). Each distinct text
// is kept once, for the session (a few key names).
const char* stable_text(const std::string& t)
{
    static std::set<std::string> kept;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    return kept.insert(t).first->c_str();
}

// A value setting's choices (their line keys; the Language setting: Auto and each language's own name) and the current
// one (none: an action).
std::vector<const char*> cfg_values(std::uint8_t k, U32& cur)
{
    cur = 0;
    if (size_t(g_cfg) < g_outfits.size()) {
        const Outfit& o = g_outfits[size_t(g_cfg)];
        if (k == kCfgClimate) { // (shown Auto / Off / Cold / Heat; stored 0 Auto, 3 Off, 1 Cold, 2 Heat)
            static const U32 pos[4] = {0, 2, 3, 1};
            cur = pos[o.climate & 3];
            return {"value.auto", "value.off", "value.cold", "value.heat"};
        }
        if (k == kCfgWeapons) {
            cur = uses_weapons(o) ? 0 : 1;
            return {"value.load", "value.leave"};
        }
        if (k == kCfgStash) {
            cur = o.stash % 3;
            return {"value.default", "value.on", "value.off"};
        }
        if (k == kCfgHotkey) { // (one value: the hotkey, or the wait for one)
            bool waiting = g_peek_capture && g_capture_outfit == g_cfg;
            return {waiting ? "value.press_key" : stable_text(hotkey_name(o.hk_vk, o.hk_mods))};
        }
        if (k == kCfgOverwrite) { // (an action with a choice: scrolled, then clicked)
            cur = g_overwrite_reset ? 1 : 0;
            return {"value.keep_settings", "value.reset_all"};
        }
    }
    if (k == kCfgGStash) return cur = g_stash ? 1 : 0, std::vector<const char*>{"value.off", "value.on"};
    if (k == kCfgParty) return cur = g_take_from_companions ? 1 : 0, std::vector<const char*>{"value.leave", "value.take"};
    if (k == kCfgMessages) return cur = g_messages ? 1 : 0, std::vector<const char*>{"value.off", "value.on"};
    if (k == kCfgPeekPad) return cur = g_peek_pad ? 1 : 0, std::vector<const char*>{"value.off", "value.peekpad"};
    if (k == kCfgPeekKey) // (one value: the key, or the wait for one)
        return {g_peek_capture && g_capture_outfit < 0 ? "value.press_key" : stable_text(peek_key_name(g_peek_key.load()))};
    if (k == kCfgLanguage) {
        std::vector<const char*> v{"value.auto_language"};
        for (size_t i = 0; i < lang::kLanguageCount; ++i) {
            v.push_back(lang::kLanguages[i].code);
            if (g_language == lang::kLanguages[i].code) cur = U32(i + 1);
        }
        return v;
    }
    return {};
}

// A value as shown (the Language setting's: the language's own name; Auto: with the game's).
std::string cfg_value_text(std::uint8_t k, const char* v)
{
    if (k == kCfgPeekKey || k == kCfgHotkey) // (a line key, or the key's own label)
        return !std::strcmp(v, "value.press_key") || !std::strcmp(v, "value.no_hotkey") ? lang::t(v) : v;
    if (k == kCfgPeekPad) return lang::f(v, {{"button", g_pad_is_ps ? "L1" : "LB"}}); // (the pad in use's own name)
    if (k != kCfgLanguage) return lang::t(v);
    std::string auto_code = std::string(v) == "value.auto_language" ? g_detected_language : v;
    const char* name = "English";
    for (size_t i = 0; i < lang::kLanguageCount; ++i)
        if (auto_code == lang::kLanguages[i].code) name = lang::kLanguages[i].name;
    return std::string(v) == "value.auto_language" ? lang::f("value.auto_language", {{"language", name}}) : name;
}

// The setting's name on its slot and in the centre; Position shows the outfit's place (no dots: there can be many).
std::string cfg_title(std::uint8_t k)
{
    if (k != kCfgPosition) return cfg_name(k);
    return lang::f("setting.position", {{"n", std::to_string(g_cfg + 1)}, {"total", std::to_string(g_outfits.size())}});
}

// What the setting does (the centre's caption).
std::string cfg_description(std::uint8_t k)
{
    std::pair<std::string, std::string> t;
    if (k == kCfgPosition) return lang::t("desc.position");
    if (size_t(g_cfg) < g_outfits.size()) {
        g_depth_outfit = static_cast<U32>(g_cfg); // (entry_text's outfit)
        if (k == kCfgClimate) t = entry_text(kPageConditions, 3);
        else if (k == kCfgWeapons) t = entry_text(kPageConditions, 0);
        else if (k == kCfgStash) t = entry_text(kPageConditions, 1);
        else if (k == kCfgRename) t = entry_text(kPageOutfit, 0);
        else if (k == kCfgOverwrite)
            t.second = lang::t(g_overwrite_reset ? "desc.overwrite.reset" : "desc.overwrite.keep");
        else if (k == kCfgDelete) t = entry_text(kPageOutfit, 1);
        else if (k == kCfgVisibility) t.second = lang::t("desc.visibility");
        else if (k == kCfgHotkey) {
            const Outfit& o = g_outfits[size_t(g_cfg)];
            t.second = g_peek_capture && g_capture_outfit == g_cfg ? lang::t("desc.hotkey.wait")
                       : o.hk_vk ? lang::f("desc.hotkey.on", {{"hotkey", hotkey_name(o.hk_vk, o.hk_mods)}})
                                 : lang::t("desc.hotkey.off");
        }
    } else {
        if (k == kCfgGStash) t = entry_text(kPageSettings, 3);
        else if (k == kCfgParty) t = entry_text(kPageSettings, 0);
        else if (k == kCfgMessages) t = entry_text(kPageSettings, 1);
        else if (k == kCfgClear) t = entry_text(kPageSettings2, 0);
        else if (k == kCfgUninstall) t = entry_text(kPageSettings2, 1);
        else if (k == kCfgLanguage) t.second = lang::t("desc.language");
        else if (k == kCfgPeekPad)
            t.second = lang::f(g_peek_pad ? "desc.peekpad.on" : "desc.peekpad.off", {{"button", g_pad_is_ps ? "L1" : "LB"}});
        else if (k == kCfgPeekKey)
            t.second = lang::t(g_peek_capture ? "desc.peekkey.wait" : g_peek_key.load() ? "desc.peekkey.on" : "desc.peekkey.off");
    }
    if (k == kCfgBack) t.second = lang::t("desc.back");
    return t.second;
}

void cfg_refresh(U64 self)
{
    g_force_images = 3;
    refresh_wheel(self);
    center_for_focus(self, focused_script(self), true);
}

void cfg_enter(U64 self, U32 o)
{
    g_eye = -1, g_cfg = static_cast<int>(o), g_overwrite_reset = false;
    LOG("settings view: %s", o < g_outfits.size() ? ("\"" + g_outfits[o].name + "\"").c_str() : "the plugin's");
    cfg_refresh(self);
}

void cfg_leave(U64 self)
{
    LOG("settings view: back to the outfits");
    g_cfg = -1;
    g_center_desc.clear();
    reset_modes();
    cfg_refresh(self);
}

// Position: the outfit changes places with its neighbour (no wrap at the ends); the view stays on it, the worn outfit's
// index follows. Prompts and a load in progress hold a name they check, but a load's late parts use the index: not then.
void cfg_move(U64 self, U32 i, int step)
{
    int from = g_cfg, to = from + (step > 0 ? 1 : -1);
    if (from < 0 || to < 0 || size_t(to) >= g_outfits.size() || size_t(from) >= g_outfits.size() || g_async.active) return;
    std::swap(g_outfits[size_t(from)], g_outfits[size_t(to)]);
    g_cfg = to;
    if (g_worn_outfit == from) g_worn_outfit = to;
    else if (g_worn_outfit == to) g_worn_outfit = from;
    LOG("settings \"%s\": position %d -> %d", g_outfits[size_t(to)].name.c_str(), from + 1, to + 1);
    save_outfits();
    apply_outfit_slots(self, i);
    center_for_focus(self, focused_script(self), true);
}

// A scroll on a value setting moves it on / back (and saves); on the rest nothing.
void cfg_scroll(U64 self, U32 i, int step)
{
    std::vector<std::uint8_t> e = cfg_entries();
    U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (k >= e.size()) return;
    if (e[k] == kCfgPosition) return cfg_move(self, i, step);
    U32 cur = 0;
    std::vector<const char*> v = cfg_values(e[k], cur);
    if (v.empty()) return;
    U32 to = U32((cur + v.size() + step) % v.size());
    if (e[k] == kCfgOverwrite) { // (how the next Overwrite goes: nothing saved)
        g_overwrite_reset = to == 1;
        LOG("settings view: Overwrite -> %s", v[to]);
        apply_outfit_slots(self, i);
        center_for_focus(self, focused_script(self), true);
        return;
    }
    if (size_t(g_cfg) < g_outfits.size()) {
        Outfit& o = g_outfits[size_t(g_cfg)];
        if (e[k] == kCfgClimate) {
            static const std::uint8_t val[4] = {0, 3, 1, 2};
            o.climate = val[to];
        } else if (e[k] == kCfgWeapons) {
            if (!o.has_weapons) {
                notify(self, lang::f("toast.no_weapons", {{"outfit", o.name}}));
                return;
            }
            o.load_weapons = to == 0;
        } else if (e[k] == kCfgStash) {
            o.stash = static_cast<std::uint8_t>(to);
        }
        LOG("settings \"%s\": %s -> %s", o.name.c_str(), cfg_name(e[k]), v[to]);
    } else {
        if (e[k] == kCfgGStash) g_stash = to == 1;
        else if (e[k] == kCfgParty) g_take_from_companions = to == 1;
        else if (e[k] == kCfgMessages) g_messages = to == 1;
        else if (e[k] == kCfgPeekPad) g_peek_pad = to == 1;
        else if (e[k] == kCfgLanguage) {
            g_language = to == 0 ? std::string() : std::string(v[to]);
            apply_language();
            if (U64 h = outfits_header(self)) set_label(h); // (the tab's name)
        }
        LOG("settings: %s -> %s", cfg_name(e[k]), v[to]);
    }
    save_outfits();
    apply_outfit_slots(self, i);
    center_for_focus(self, focused_script(self), true);
}

// A click on an action: Visibility opens the outfit's pieces; the others the existing prompts (Delete and the "+" slot's
// clean-ups leave the view first: their answer changes the list).
void cfg_press(U64 self, U32 i)
{
    std::vector<std::uint8_t> e = cfg_entries();
    U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (k >= e.size()) return;
    U32 o = static_cast<U32>(g_cfg);
    LOG("settings view: %s pressed", cfg_name(e[k]));
    switch (e[k]) {
    case kCfgVisibility: g_cfg = -1, eye_enter(self, o); break;
    case kCfgRename: open_rename_prompt(self, o); break;
    case kCfgOverwrite: overwrite_pressed(self, o, g_overwrite_reset); break;
    case kCfgDelete: cfg_leave(self), open_confirm(self, o, kConfirmDelete, 0); break;
    case kCfgClear: cfg_leave(self), open_clean_confirm(self, kConfirmCleanSave); break;
    case kCfgUninstall: cfg_leave(self), open_clean_confirm(self, kConfirmUninstall); break;
    case kCfgBack: cfg_leave(self); break;
    case kCfgPeekKey: // (waits for the key: peek_capture_frame)
    case kCfgHotkey:
        for (int vk = 0; vk < 256; ++vk) g_peek_capture_held[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        g_capture_outfit = e[k] == kCfgHotkey ? int(o) : -1;
        g_peek_capture_slot = i;
        g_peek_capture = true;
        apply_outfit_slots(self, i);
        center_for_focus(self, focused_script(self), true);
        break;
    default: break; // (a value setting: scrolled)
    }
}

// The dots of slot i in the settings view: a value setting's choices (lit: the current one); none for the rest.
U32 cfg_dots(U32 i, U32* lit)
{
    std::vector<std::uint8_t> e = cfg_entries();
    U32 k = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
    if (k >= e.size()) return 0;
    U32 cur = 0;
    std::vector<const char*> v = cfg_values(e[k], cur);
    if (lit) *lit = cur;
    return e[k] == kCfgLanguage || e[k] == kCfgPeekKey || e[k] == kCfgHotkey ? 0 : static_cast<U32>(v.size()); // (the value says it)
}

// Slot i of the settings view: the setting's name (and value) on the slot's text, its dots, the click prompt for an
// action; Back: its label and the click prompt, like an action.
void apply_cfg_slot(U64 self, U64 script, U64 root, U32 i, U32 visible)
{
    std::vector<std::uint8_t> e = cfg_entries();
    U32 k = i < visible ? slot_outfit(i) : kOutfitSlots;
    bool entry = k < e.size();
    std::uint8_t kind = entry ? e[k] : kCfgBack;
    U32 cur = 0;
    std::vector<const char*> vals = entry ? cfg_values(kind, cur) : std::vector<const char*>{};
    bool back = entry && kind == kCfgBack, action = entry && !back && vals.empty() && kind != kCfgPosition;
    show_slot_image(self, root, i, std::string());
    if (i < visible) show_mode_dots(script, mode_count(i), mode_lit(i));
    U64 badge = find_by_class(root, "quickslot-occupancy-icon-chain"), low = find_by_class(root, "cpp-icon-disable-item");
    const char* want = action || back ? prompt_class() : nullptr;
    g_badge_elem[i] = badge, g_badge_class[i] = want ? kBadges[badge_index(want)] : nullptr;
    bool ok = true;
    if (badge) {
        ok &= set_class(badge, "self-center", true);
        for (const char* c : kBadges) ok &= set_class(badge, c, want && !std::strcmp(want, c));
        ok &= set_class(badge, "!cpp-none", !want);
    }
    U64 mark = find_parent_of_class(root, "charged-count");
    bool label = entry; // (Back too: "Back")
    if (mark) {
        ok &= set_class(mark, "om-eye-c", false) && set_class(mark, "om-eye2-c", false);
        ok &= set_class(mark, "self-center", true) && set_class(mark, "om-label-wrap", label);
        ok &= set_class(mark, "!cpp-none", !label);
        if (U64 text = find_by_class(mark, "charged-count")) {
            std::string t = label ? cfg_title(kind) : "";
            if (g_set_text) g_set_text(text, t.c_str(), 0);
            ok &= set_class(text, "om-label", label) && set_class(text, "om-label-value", false);
        }
    }
    g_mark_elem[i] = mark, g_mark_shown[i] = mark && label;
    ok &= show_value(root, vals.empty() ? std::string() : cfg_value_text(kind, vals[cur]));
    g_slot_image[i].missing = false;
    ok &= set_class(root, "!cpp-none", i >= visible);
    ok &= swap_class(root, g_slot_class[i], i < visible ? numbered("om-s%u-%u", visible, i) : "");
    U64 icon = rp(script + kOffScriptCategoryIcon);
    if (icon && is_element(icon)) {
        ok &= set_class(icon, "quickslot-item-image-none", false);
        ok &= swap_class(icon, g_placeholder_class[i], "");
        ok &= set_class(icon, "om-empty-armor", false);
        ok &= set_class(icon, "om-empty-sword", false) && set_class(icon, "om-empty-necklace", false);
        ok &= set_class(icon, "!cpp-none", true);
        ok &= set_class(icon, "om-missing", false);
    }
    g_low_elem[i] = low, g_low_shown[i] = false;
    if (low) ok &= swap_class(low, g_worn_class[i], "") && set_class(low, "!cpp-none", true);
    if (U64 focus = find_by_class(root, "quickslot-item-focus-bg-10"))
        ok &= swap_class(focus, g_focus_class[i], i < visible ? numbered("om-f%u-%u", visible, i) : "");
    if (!ok) LOG("  settings slot %u: class change skipped (%s)", i, describe(root).c_str());
}
void clean_outfits(U64 self);
void uninstall(U64 self);
void confirmed(U64 self, U32 index, int what, U32 other);
bool g_ask_next = false; // the next question of g_pending opens on the next pump (not inside a modal's click)

void finish_ask(int answer, const char* why)
{
    if (!g_ask_modal) return;
    g_ask_modal = 0;
    g_modal_ended = GetTickCount64();
    g_release_pending = g_menu_open;
    AskedLoad p = std::move(g_pending);
    g_pending = {};
    bool here = p.index < g_outfits.size() && g_outfits[p.index].name == p.name;
    LOG("question %d: %s (\"%s\"%s)", p.stage, why, p.name.c_str(), here ? "" : ", outfit gone");
    if (p.confirm == kConfirmCleanSave || p.confirm == kConfirmUninstall) { // (about no one outfit)
        if (answer != kAskYes || !g_menu_open) return;
        if (p.confirm == kConfirmCleanSave) clean_outfits(g_ask_self);
        else uninstall(g_ask_self);
        return;
    }
    bool from_wheel = g_ask_self == g_outfits_root; // (or the quick peek: asked in the game, no wheel to close)
    if (!here || answer == kAskAbort || (from_wheel && !g_menu_open)) return;
    if (p.confirm) {
        bool other_here = p.confirm != kConfirmSwap ||
                          (p.other < g_outfits.size() && g_outfits[p.other].name == p.other_name);
        if (answer == kAskYes && other_here) confirmed(g_ask_self, p.index, p.confirm, p.other);
        return;
    }
    if (p.stage == 1) p.takes_answered = true, p.take = answer == kAskYes;
    else p.copies_answered = true, p.use_copies = answer == kAskYes;
    if (p.stage == 1 && !p.copies.empty()) { // the copies question next (opened by pump)
        g_release_pending = false;
        g_pending = std::move(p);
        g_ask_next = true;
        return;
    }
    finish_load(g_ask_self, p);
}

void abort_prompts(const char* why)
{
    if (g_name_modal) {
        hide_modal_raw(g_name_modal);
        finish_naming("", why);
    }
    if (g_ask_modal) {
        hide_modal_raw(g_ask_modal);
        finish_ask(kAskAbort, why);
    }
    if (g_ask_next) {
        g_ask_next = false;
        g_pending = {};
        LOG("questions: %s; nothing loaded", why);
    }
}

// Slot 113 of the CommonModal (its button / key handler): only our question is handled here. Without an owner the game
// stores no answer, so the clicked button tells it.
U64 __fastcall ask_click_hook(U64 modal, U64 a, U64 elem, U64 flag)
{
    if (!g_ask_modal || modal != g_ask_modal || (flag & 0xFF)) return g_ask_click_orig(modal, a, elem, flag);
    bool yes = elem && elem == rp(modal + kOffModalConfirm), no = elem && elem == rp(modal + kOffModalCancel);
    bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0; // Esc may arrive as the cancel button: it loads nothing
    LOG("question: click elem %llX (confirm %d, cancel %d, Esc %d)", elem, yes, no, esc);
    U64 r = call_ask_click_raw(modal, a, elem, flag);
    if (esc) finish_ask(kAskAbort, "Esc; nothing loaded");
    else if (yes || no) finish_ask(yes ? kAskYes : kAskNo, yes ? "yes" : "leave as is");
    return r;
}

// Opens the next question of a load (pieces on other characters, then copies), or loads.
void ask_next(U64 self, AskedLoad p)
{
    if (p.index >= g_outfits.size()) return;
    const Outfit& o = g_outfits[p.index];
    std::string msg, yes;
    if (!p.takes.empty() && !p.takes_answered) {
        p.stage = 1;
        for (const Take& t : p.takes)
            msg += (msg.empty() ? std::string() : lang::f("list.separator")) +
                   lang::f("ask.take_item", {{"piece", piece_label(o, o.items[t.k].slot)}, {"who", character_name(t.who)}});
        long long n = static_cast<long long>(p.takes.size());
        msg = lang::n("ask.take", n, {{"piece", piece_label(o, o.items[p.takes[0].k].slot)},
                                      {"who", character_name(p.takes[0].who)}, {"list", msg}});
        yes = lang::n("ask.take_yes", n);
    } else if (!p.copies.empty() && !p.copies_answered) {
        p.stage = 2;
        std::vector<U16> copy_slots;
        for (const auto& [k, uid] : p.copies) copy_slots.push_back(o.items[k].slot);
        long long n = static_cast<long long>(p.copies.size());
        msg = lang::n("ask.copy", n, {{"list", slot_list(o, copy_slots)}});
        yes = lang::n("ask.copy_yes", n);
    } else {
        finish_load(self, p);
        return;
    }
    U64 modal = g_open_common_modal ? open_common_modal_raw(self, o.name.c_str(), msg.c_str(), yes.c_str(), lang::t("ask.leave")) : 0;
    if (!modal) {
        LOG("question %d did not open; those slots stay as they are", p.stage);
        if (p.stage == 1) p.takes_answered = true;
        else p.copies_answered = true;
        ask_next(self, std::move(p));
        return;
    }
    g_pending = std::move(p);
    g_ask_self = self;
    g_ask_opened = GetTickCount64();
    rd(modal + kOffModalShown, g_ask_shown);
    g_ask_modal = modal;
    LOG("question %d %llX open (+0xbd %u): %s", g_pending.stage, modal, g_ask_shown, msg.c_str());
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

// pump: the next question of a load, and the asynchronous parts of the last one.
void open_clean_confirm(U64 self, int what);

// The next question of a load (opened a moment after the last one closed).
void pump_ask(U64 self)
{
    if (g_ask_next && !g_ask_modal && !g_name_modal && GetTickCount64() - g_modal_ended > 150) {
        g_ask_next = false;
        AskedLoad p = std::move(g_pending);
        g_pending = {};
        if (p.index < g_outfits.size() && g_outfits[p.index].name == p.name) ask_next(self, std::move(p));
    }
}

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

// Every move gives the piece a new uid. The stash knows what it moved, so it follows each piece exactly: the copy of its
// item in storage that was not there before the requests is the piece (adopt_returned stays the fallback for moves
// made by hand).
struct StashSent {
    U64 uid;
    U16 key;
};
struct StashCheck {
    std::vector<StashSent> sent;
    std::vector<U64> before; // uids in storage before the requests
    U64 at = 0;              // tick of the requests (0: nothing pending)
};
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

// Each saved piece and whether it goes away: only when every outfit using it has Stash on (one with it off keeps the
// piece in the bag: Off wins, so a piece shared with an outfit kept at hand stays at hand).
struct StashDest {
    U64 uid;
    U16 key;
    bool on; // goes away (every outfit using it has Stash on)
};
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

// While the question is open: notices it closing some other way (Esc, a click outside) or a 3-minute safety stop.
void watch_ask()
{
    if (!g_ask_modal) return;
    unsigned char shown = g_ask_shown;
    rd(g_ask_modal + kOffModalShown, shown);
    if (g_ask_shown && !shown) finish_ask(kAskAbort, "closed without an answer; nothing loaded");
    else if (GetTickCount64() - g_ask_opened > 180000) {
        hide_modal_raw(g_ask_modal);
        finish_ask(kAskAbort, "timed out; nothing loaded");
    }
}

// The "+" slot pressed: save the worn armor as a new outfit, unless an identical one exists (then names it).
void save_new_outfit(U64 self)
{
    if (g_uninstalled) {
        notify(self, lang::t("toast.uninstalled"));
        return;
    }
    if (g_outfits.size() >= kMaxOutfits) {
        notify(self, lang::f("toast.limit", {{"n", std::to_string(kMaxOutfits)}}));
        return;
    }
    Outfit o;
    if (!snapshot(self, o)) return;
    for (const Outfit& s : g_outfits)
        if (same_saved(s, o)) {
            notify(self, lang::f("toast.already_saved", {{"outfit", s.name}}));
            return;
        }
    o.name = default_name();
    if (int a = anchor_outfit(); a >= 0) carry_looks(g_outfits[size_t(a)], o); // (a changed outfit saved as new looks the same)
    g_outfits.push_back(o);
    g_outfit_count = static_cast<U32>(g_outfits.size());
    LOG("saved \"%s\" (%zu items, chest %s)", o.name.c_str(), o.items.size(),
        o.chest_icon.empty() ? "none" : o.chest_icon.c_str());
    save_outfits();
    refresh_wheel(self);
    center_for_focus(self, focused_script(self), true);
    open_rename_prompt(self, g_outfits.size() - 1);
}

// Delete (the outfit page, confirmed): the later outfits move up and the wheel shrinks.
void delete_outfit(U64 self, U32 o)
{
    LOG("deleted \"%s\"", g_outfits[o].name.c_str());
    g_outfits.erase(g_outfits.begin() + o);
    g_outfit_count = static_cast<U32>(g_outfits.size());
    save_outfits();
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
}

// --- The submenu pages (entries: 0 right, 1 bottom, 2 left = Back, 3 top) --------------------------------------------
// The outfit's Stash, explained; with the pieces another outfit keeps in the bag (Off wins for a shared piece).
std::string stash_description(const Outfit& o)
{
    bool on = stash_of(o);
    std::string s = lang::t(!o.stash ? (g_stash ? "desc.stash.default_on" : "desc.stash.default_off")
                            : on      ? "desc.stash.on"
                                      : "desc.stash.off");
    if (!on) return s;
    int held = 0;
    std::string by;
    for (const auto& [uid, key, away] : stash_destinations())
        if (!away)
            for (const OutfitItem& it : o.items)
                if (it.uid == uid && it.key == key && loads_piece(o, it.slot)) {
                    ++held;
                    for (const Outfit& other : g_outfits) // (the outfit that keeps it, on this character)
                        if (by.empty() && &other != &o && !stash_of(other))
                            for (const OutfitItem& x : other.items)
                                if (x.uid == uid && x.key == key && loads_piece(other, x.slot)) by = other.name;
                }
    if (held) s += " " + (by.empty() ? lang::n("desc.stash.kept_other", held) : lang::n("desc.stash.kept_by", held, {{"outfit", by}}));
    return s;
}

// A saved outfit's settings, as words.
const char* climate_text(const Outfit& o)
{
    static const char* const t[4] = {"Climate: Auto", "Climate: Cold", "Climate: Heat", "Climate: Off"};
    return t[o.climate & 3];
}
const char* weapons_text(const Outfit& o) { return uses_weapons(o) ? "Weapons: Load" : "Weapons: Leave"; }
const char* outfit_stash_text(const Outfit& o)
{
    static const char* const k[3] = {"Stash: Default", "Stash: On", "Stash: Off"};
    return k[o.stash % 3];
}

const char* stash_text() { return g_stash ? "Stash: On" : "Stash: Off"; }

// Clean up: this save's outfits, i.e. the current outfits of every character and the copies of the save loaded last
// (so loading it again does not bring them back; the other saves keep theirs, as with any game state).
void clean_outfits(U64 self)
{
    size_t gone = 0, saves = 0;
    {
        std::lock_guard<std::mutex> lock(g_store_mu);
        if (g_generation.load() != g_seen_generation) {
            LOG("clean up dropped: a save was loaded meanwhile");
            return;
        }
        store_for(g_character).outfits = g_outfits;
        gone = outfit_total(g_store);
        g_store.clear();
        g_published.clear();
        for (auto& [slot, b] : g_bound)
            if (g_loaded_known && b.id.same(g_loaded_id)) {
                if (!b.store.empty()) ++saves;
                b.store.clear();
            }
        bool ok = write_store_file();
        LOG("clean up: %zu current outfits and the copies of %zu saves deleted%s", gone, saves,
            ok ? "" : " (writing FAILED)");
    }
    g_outfits.clear();
    g_outfit_count = 0;
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
    notify(self, lang::t("toast.cleared"));
}

// Uninstall: every file the mod keeps (the outfits of every save and the settings, the address cache, the log, a
// write's leftover) is deleted, so removing the mod in DMM leaves nothing behind. The mod never writes into game saves.
// It stays loaded until the game closes, inert: nothing it does writes a file again.
bool g_uninstalled = false;
std::wstring g_log_path;
extern std::wstring g_cache_path;

void uninstall(U64 self)
{
    {
        std::lock_guard<std::mutex> lock(g_store_mu);
        g_uninstalled = true;
        g_store.clear(), g_published.clear(), g_bound.clear(), g_pending_writes.clear(), g_unfinished.clear();
    }
    g_outfits.clear();
    g_outfit_count = 0;
    LOG("uninstall: deleting the outfit file, the cache and this log; the mod stays inert until the game closes");
    logx::close();
    int failed = 0;
    for (const std::wstring& f : {g_store_path, g_store_path + L".tmp", g_cache_path, g_log_path})
        if (!f.empty() && !DeleteFileW(f.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) ++failed;
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
    notify(self, lang::t(failed ? "toast.remove_failed" : "toast.removed"));
}

// The Clean up and Uninstall questions: a deleting button and Cancel (the dialog's second button is Esc's: Esc always
// cancels).
void open_clean_confirm(U64 self, int what)
{
    if (g_ask_modal || g_ask_next) return;
    std::string msg, yes;
    if (what == kConfirmCleanSave) {
        size_t total = 0;
        std::string who;
        store_for(g_character); // (the current character counts from its working list, g_outfits)
        for (const CharacterOutfits& c : g_store) {
            size_t n = c.character == g_character ? g_outfits.size() : c.outfits.size();
            if (!n) continue;
            total += n;
            who += (who.empty() ? std::string() : lang::f("list.separator")) +
                   lang::f("ask.clear_who", {{"character", c.character == kNoCharacter ? lang::t("character.unassigned")
                                                                                        : character_name(c.character)},
                                             {"n", std::to_string(n)}});
        }
        if (!total) {
            notify(self, lang::t("toast.nothing_to_clear"));
            return;
        }
        msg = lang::f("ask.clear", {{"n", std::to_string(total)}, {"who", who}});
        yes = lang::t("ask.yes.delete");
    } else {
        msg = lang::t("ask.uninstall");
        yes = lang::t("ask.yes.remove");
    }
    const char* title = lang::t(what == kConfirmCleanSave ? "setting.clear" : "setting.uninstall");
    U64 modal = g_open_common_modal ? open_common_modal_raw(self, title, msg.c_str(), yes.c_str(), lang::t("ask.cancel")) : 0;
    if (!modal) {
        LOG("clean up: the dialog did not open; nothing deleted");
        return;
    }
    AskedLoad p;
    p.confirm = what;
    g_pending = std::move(p);
    g_ask_self = self;
    g_ask_opened = GetTickCount64();
    rd(modal + kOffModalShown, g_ask_shown);
    g_ask_modal = modal;
    LOG("clean up question %d %llX open: %s", what, modal, msg.c_str());
}

// Overwrite / Delete / Swap ask first, in the game's yes / no dialog (the loading questions' machinery: finish_ask
// answers). other: kConfirmSwap's second outfit; kConfirmOverwrite: 1 resets the outfit's settings, 0 keeps them.
void open_confirm(U64 self, U32 index, int what, U32 other = 0)
{
    if (g_ask_modal || g_ask_next || index >= g_outfits.size()) return;
    if (what == kConfirmSwap && other >= g_outfits.size()) return;
    const std::string& name = g_outfits[index].name;
    std::string msg = what == kConfirmOverwrite ? lang::f(other ? "ask.overwrite.reset" : "ask.overwrite.keep", {{"outfit", name}})
                      : what == kConfirmDelete  ? lang::f("ask.delete", {{"outfit", name}})
                                                : lang::f("ask.swap", {{"outfit", name}, {"worn", g_outfits[other].name}});
    const char* yes = lang::t(what == kConfirmOverwrite ? "ask.yes.overwrite" : what == kConfirmDelete ? "ask.yes.delete" : "ask.yes.swap");
    U64 modal = g_open_common_modal ? open_common_modal_raw(self, name.c_str(), msg.c_str(), yes, lang::t("ask.cancel")) : 0;
    if (!modal) {
        LOG("confirm %d for \"%s\": the dialog did not open; nothing done", what, name.c_str());
        return;
    }
    AskedLoad p;
    p.index = index, p.name = name, p.confirm = what;
    p.other = other; // (Swap: the other outfit; Overwrite: reset or keep)
    if (what == kConfirmSwap) p.other_name = g_outfits[other].name;
    g_pending = std::move(p);
    g_ask_self = self;
    g_ask_opened = GetTickCount64();
    rd(modal + kOffModalShown, g_ask_shown);
    g_ask_modal = modal;
    LOG("confirm %d %llX open: %s", what, modal, msg.c_str());
}

void confirmed(U64 self, U32 index, int what, U32 other)
{
    if (index >= g_outfits.size()) return;
    if (what == kConfirmDelete) {
        delete_outfit(self, index);
        return;
    }
    if (what == kConfirmSwap) {
        if (other >= g_outfits.size() || other == index) return;
        std::swap(g_outfits[index], g_outfits[other]);
        LOG("Swap: \"%s\" and \"%s\" changed slots", g_outfits[other].name.c_str(), g_outfits[index].name.c_str());
        save_outfits();
        refresh_wheel(self);
        center_restore();
        center_for_focus(self, focused_script(self), true);
        report(self, lang::f("report.swapped", {{"a", g_outfits[other].name}, {"b", g_outfits[index].name}}));
        return;
    }
    Outfit o;
    if (!snapshot(self, o)) return;
    for (size_t j = 0; j < g_outfits.size(); ++j)
        if (same_saved(g_outfits[j], o)) {
            notify(self, lang::f(j == index ? "toast.already_holds" : "toast.already_saved", {{"outfit", g_outfits[j].name}}));
            return;
        }
    const Outfit& was = g_outfits[index];
    o.name = was.name;
    o.worn = was.worn; // (whether it is the last loaded one: not a setting)
    bool keep = other == 0;
    if (keep) { // (its settings and looks stay; reset: as newly saved)
        if (was.has_weapons) o.load_weapons = was.load_weapons;
        o.climate = was.climate, o.stash = was.stash;
        o.hk_vk = was.hk_vk, o.hk_mods = was.hk_mods;
        carry_looks(was, o);
    }
    g_outfits[index] = o;
    LOG("Overwrite (%s): \"%s\" now holds %zu items (chest %s)", keep ? "settings kept" : "settings reset",
        o.name.c_str(), o.items.size(), o.chest_icon.empty() ? "none" : o.chest_icon.c_str());
    save_outfits();
    refresh_wheel(self);
    center_restore();
    center_for_focus(self, focused_script(self), true);
    report(self, lang::f("report.now_holds", {{"outfit", o.name}}));
}

// Overwrite: what is worn decides the question. Already this outfit: said so; another saved outfit: offer to swap
// their slots; anything else: confirm the overwrite.
void overwrite_pressed(U64 self, U32 index, bool reset)
{
    Outfit o;
    if (index >= g_outfits.size() || !snapshot(self, o)) return;
    for (size_t j = 0; j < g_outfits.size(); ++j)
        if (same_saved(g_outfits[j], o)) {
            if (j == index) notify(self, lang::f("toast.already_holds", {{"outfit", g_outfits[j].name}}));
            else open_confirm(self, index, kConfirmSwap, static_cast<U32>(j));
            return;
        }
    open_confirm(self, index, kConfirmOverwrite, reset ? 1 : 0);
}

// A left click anywhere acts like Space on the highlighted (focused) slot, except on the tab headers (switching tabs
// must not also load the highlighted outfit; the tab changes only after the press).
std::atomic<U64> g_click_at{0};
std::atomic<U64> g_click_pos{~0ull}; // client px of the press: x << 32 | y (~0: unknown)

bool click_on_header(U64 self)
{
    U64 pos = g_click_pos.load();
    if (pos == ~0ull) return false;
    float x = float(int(U32(pos >> 32))), y = float(int(U32(pos)));
    U64 tb = rp(self + kOffRootTabBar);
    U64 hv = tb ? rp(tb + kOffTabHeaders) : 0;
    U32 n = tb ? r32(tb + kOffTabHeaderCount) : 0;
    std::string seen;
    for (U32 i = 0; hv && i < n && i < 8; ++i) {
        U64 h = rp(hv + i * 8ull);
        U64 e = h ? rp(h + 8) : 0;
        float l, t, r, b; // element layout rect (client px), as tooltip.cpp reads it
        if (!e || !is_element(e) || has_class(e, "!cpp-none") || !rd(e + kOffElemRect, l) ||
            !rd(e + kOffElemRect + 4, t) || !rd(e + kOffElemRect + 8, r) || !rd(e + kOffElemRect + 0xc, b) || !(r > l && b > t))
            continue;
        char buf[64];
        std::snprintf(buf, sizeof buf, " %u:%.0f,%.0f-%.0f,%.0f", i, l, t, r, b);
        seen += buf;
        constexpr float pad = 6;
        if (x >= l - pad && x <= r + pad && y >= t - pad && y <= b + pad) {
            LOG("click at %.0f,%.0f on tab header %u (%.0f,%.0f-%.0f,%.0f): ignored", x, y, i, l, t, r, b);
            return true;
        }
    }
    LOG("click at %.0f,%.0f; headers%s", x, y, seen.empty() ? " (no rects)" : seen.c_str());
    return false;
}

// The L2 / R2 prompts by the focused slot (the view's QuickSlotSelectPrev / NextKeyGuide, shown for a gamepad only):
// the game hides them on our slots (nothing to cycle, as it sees them), so on the Outfits tab they are shown again,
// which also switches its own L2 / R2 back on there. Their group (#QuickSlotItemKeyguideGroup) is placed by the game's
// code: its style's left (+0) and top (+8), in layout units, are the focused slot's centre; on the Outfits tab the mod
// does the same. Layout units per screen pixel: learnt on the game's tabs (its own left / the focused slot's centre),
// else layout height 1080 over the window's height. On its own tabs the game keeps managing all of it.
float g_prompt_scale = 0;
U64 g_hover_script = 0; // the slot the game's focus call last named (0: none)

void pad_prompts(U64 self)
{
    static U64 prev = 0, next = 0, group = 0;
    U64 menu = rp(self + 8);
    if (!prev || !is_element(prev)) prev = find_by_class(menu, "quickslot-item-focus-keyguide-prev", 24);
    if (!next || !is_element(next)) next = find_by_class(menu, "quickslot-item-focus-keyguide-next", 24);
    if (!group || !is_element(group)) group = find_parent_of_class(menu, "quickslot-keyguide-wrap", 24);
    U64 gh = group ? rp(group + 0x20) : 0, style = gh ? rp(gh + 0x20) : 0;
    U64 script = focused_script(self), fe = script ? rp(script + 8) : 0;
    auto box = [](U64 e, float& cx, float& cy) {
        float l = 0, t = 0, r = 0, b = 0;
        if (!e || !rd(e + kOffElemRect, l) || !rd(e + kOffElemRect + 4, t) || !rd(e + kOffElemRect + 8, r) ||
            !rd(e + kOffElemRect + 0xc, b) || r <= l || b <= t) return false;
        cx = (l + r) / 2, cy = (t + b) / 2;
        return true;
    };
    // The prompts flank the slot's item picture when it is shown. A hidden picture (the "+" slot's, every settings
    // entry's) keeps a stale box, so then the spot is computed: the wheel's centre (the ring background's) plus the
    // slot's offset from it, as the mod's own css places the slots (om-sN-i: margins + half the 50px slot; in layout
    // units, the unit the prompts' style uses).
    static constexpr float kSlotAt[12][12][2] = {
#include "om_slot_offsets.inc"
    };
    float px = 0, py = 0, left = 0;
    if (!outfits_current(self)) { // the game's tab: learn the scale from where it put the group
        U64 img = fe ? find_by_class(fe, "quickslot-item-img", 8) : 0;
        if (img && !has_class(img, "!cpp-none") && box(img, px, py) && style && rd(style, left) && left > 0 &&
            left < 100000)
            g_prompt_scale = left / px;
        return;
    }
    // The slot really hovered: the one the game's focus call last named (focus_hook). After a view or page change none
    // may be hovered (the focus value still names the last one): then no prompts.
    // A view / page change re-lays the wheel without a new focus call: the last one names a slot of the old layout, so
    // it is forgotten until the game's next focus call.
    static int view_was = -1;
    int view = g_eye >= 0 ? 0x100 | g_eye_step : g_cfg >= 0 ? 0x200 : 0; // (the settings view: one layout, whichever
    if (view != view_was) {                                                // outfit, as Position moves it)
        if (view_was != -1) g_hover_script = 0;
        view_was = view;
    }
    U32 n = visible_slots(), i = outfit_slot_index(self, g_hover_script);
    if (i < n) script = g_hover_script, fe = rp(script + 8);
    if (in_depth(self) || n > 12) return;
    if (i >= n) { // nothing hovered
        for (U64 e : {prev, next})
            if (e && !has_class(e, "!cpp-none")) set_class(e, "!cpp-none", true);
        return;
    }
    // Entries with nothing to scroll, as Rename / Delete: no L2 / R2, as vanilla. Position and Language have no dots but
    // scroll (Position moves the outfit on the wheel; Language has a value per language).
    bool scrolls = mode_count(i) > 1;
    if (!scrolls && g_cfg >= 0) {
        std::vector<std::uint8_t> e = cfg_entries();
        U32 k = slot_outfit(i);
        scrolls = k < e.size() && (e[k] == kCfgPosition || e[k] == kCfgLanguage);
    }
    for (U64 e : {prev, next})
        if (e && has_class(e, "!cpp-none") == scrolls) set_class(e, "!cpp-none", !scrolls);
    if (!scrolls) return;
    float k = g_prompt_scale;
    RECT cr{};
    HWND w = GetForegroundWindow();
    bool client = w && GetClientRect(w, &cr) && cr.bottom > 0;
    if (k <= 0) {
        if (!client) return;
        k = 1080.0f / float(cr.bottom);
    }
    if (!style) return;
    float x = 0, y = 0, cur_x = 0, cur_y = 0;
    // Only a saved outfit's chest picture is trusted (the settings / appearance entries' pictures keep stale boxes).
    bool real_picture = g_cfg < 0 && (g_eye >= 0 ? slot_outfit(i) < eye_pieces().size() : slot_outfit(i) < g_outfits.size());
    U64 img = fe && real_picture ? find_by_class(fe, "quickslot-item-img", 8) : 0;
    if (img && !has_class(img, "!cpp-none") && box(img, px, py)) { // a visible picture: centred on it
        x = px * k, y = py * k;
    } else { // hidden: the computed spot
        if (!(g_ring_bg && is_element(g_ring_bg) && box(g_ring_bg, px, py))) { // (else the screen's centre, as the wheel is)
            if (!client) return;
            px = cr.right / 2.0f, py = cr.bottom / 2.0f;
        }
        // the layout the slot really has: its om-sN-i class (the view's slot count can change before it is re-applied)
        unsigned cn = 0, ci = 0;
        if (sscanf_s(g_slot_class[i].c_str(), "om-s%u-%u", &cn, &ci) != 2 || cn < 1 || cn > 12 || ci >= cn) return;
        x = px * k + kSlotAt[cn - 1][ci][0], y = py * k + kSlotAt[cn - 1][ci][1];
    }
    if (rd(style, cur_x) && rd(style + 8, cur_y) && (std::fabs(cur_x - x) > 0.5f || std::fabs(cur_y - y) > 0.5f))
        wr<float>(style, x), wr<float>(style + 8, y); // (the game lays it out from these on its next pass)
}


// Runs on the UI thread from the menu's frequent callbacks.
// The Outfit Arc setting waiting for a key: the next key or mouse button newly down (left / right excepted).
void peek_capture_frame(U64 self)
{
    if (!g_peek_capture) return;
    int got = -1;
    for (int vk = 1; vk < 256 && got < 0; ++vk) {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            (vk >= VK_LSHIFT && vk <= VK_RMENU) || vk == VK_LWIN || vk == VK_RWIN)
            continue; // (the modifiers: held with a hotkey's key, never the key)
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !g_peek_capture_held[vk]) got = vk;
        g_peek_capture_held[vk] = down;
    }
    if (got < 0 || !game_focused()) return;
    g_peek_capture = false;
    if (g_capture_outfit >= 0) { // an outfit's Hotkey
        size_t o = size_t(g_capture_outfit);
        g_capture_outfit = -1;
        if (o >= g_outfits.size()) return;
        if (got == VK_ESCAPE) {
            LOG("settings \"%s\": hotkey: cancelled", g_outfits[o].name.c_str());
        } else if (got == VK_BACK || got == VK_DELETE) {
            g_outfits[o].hk_vk = 0, g_outfits[o].hk_mods = 0;
            LOG("settings \"%s\": hotkey cleared", g_outfits[o].name.c_str());
            save_outfits();
        } else if (U32(got) == g_peek_key.load()) { // (it opens the Outfit Arc: not also a hotkey)
            notify(self, lang::f("toast.hotkey_arc", {{"hotkey", peek_key_name(U32(got))}}));
        } else {
            std::uint8_t m = mods_now();
            for (size_t j = 0; j < g_outfits.size(); ++j) // (one outfit per hotkey: it moves here)
                if (j != o && g_outfits[j].hk_vk == got && g_outfits[j].hk_mods == m) {
                    g_outfits[j].hk_vk = 0, g_outfits[j].hk_mods = 0;
                    notify(self, lang::f("toast.hotkey_moved", {{"hotkey", hotkey_name(U32(got), m)}, {"outfit", g_outfits[j].name}}));
                }
            g_outfits[o].hk_vk = static_cast<U16>(got), g_outfits[o].hk_mods = m;
            LOG("settings \"%s\": hotkey -> %s (%d, mods %u)", g_outfits[o].name.c_str(), hotkey_name(U32(got), m).c_str(), got, m);
            save_outfits();
        }
        apply_outfit_slots(self, g_peek_capture_slot);
        center_for_focus(self, focused_script(self), true);
        return;
    }
    if (got == VK_ESCAPE) LOG("settings: Outfit Arc key: cancelled");
    else {
        g_peek_key = got == VK_BACK || got == VK_DELETE ? 0 : U32(got);
        LOG("settings: Outfit Arc key -> %s (%d)", peek_key_name(g_peek_key.load()).c_str(), g_peek_key.load());
        save_outfits();
    }
    apply_outfit_slots(self, g_peek_capture_slot);
    center_for_focus(self, focused_script(self), true);
}

void pump(U64 self)
{
    if (GetCurrentThreadId() != g_ui_thread || !g_menu_open || g_busy) return;
    peek_capture_frame(self);
    { // the "+" page shows the Outfit Arc binding of the input in use: re-shown when the input changes
        static int shown = -1;
        int now = g_pad_in_use ? (g_pad_is_ps ? 2 : 1) : 0;
        if (g_cfg >= 0 && size_t(g_cfg) >= g_outfits.size() && shown >= 0 && now != shown) {
            if (!now) g_peek_capture = false;
            cfg_refresh(self); // (a full redraw: the slot's name, value and images)
        }
        shown = now;
    }
    Busy busy;
    pull_active();
    pump_async(self); // (on any tab: a load's asynchronous parts finish even after switching tabs)
    stash_verify(self);
    if (!outfits_current(self)) {
        pad_prompts(self); // (learns where the game puts the prompts)
        return;
    }
    if (g_applied_visible != visible_slots()) refresh_wheel(self);
    // The game's slot refresh no longer clears our pictures (slot_refresh_hook): they are only put back if hidden,
    // and re-set outright just after the menu opens / the Outfits tab shows (g_force_images). Re-setting them on every
    // focus change, prompt and load (as before) made them flicker.
    keep_slot_images(self, g_force_images > 0);
    if (g_force_images > 0) --g_force_images;
    watch_rename_prompt();
    watch_ask();
    if (g_release_pending && !g_name_modal && !g_ask_modal) {
        g_release_pending = false;
        if (g_queue_hide) release_menu_button(self);
    }
    if (g_name_modal || g_ask_modal || GetTickCount64() - g_modal_ended < 300) { // a prompt (and the key that closed it)
        g_click_at = 0, g_space_at = 0;
    }
    bool click = take_press(g_click_at) && !click_on_header(self);
    bool space = take_press(g_space_at);
    if (in_depth(self)) click = space = false; // (the game's own submenu, the lantern's: its presses are the game's)
    pad_prompts(self); // (shown, they also switch the game's own L2 / R2 back on: it scrolls our slots through its handler)
    if (g_prompt_changed.exchange(false)) apply_outfit_slots(self); // (the confirm icons: mouse / Cross / A)
    if ((click || space) && g_eye >= 0) {
        U32 i = outfit_slot_index(self, focused_script(self));
        LOG("%s in the appearance view: slot %u", click ? "click" : "Space", i);
        if (i < visible_slots()) eye_press(self, i);
        click = space = false;
    }
    if ((click || space) && g_cfg >= 0) {
        U32 i = outfit_slot_index(self, focused_script(self));
        LOG("%s in the settings view: slot %u", click ? "click" : "Space", i);
        if (i < visible_slots()) cfg_press(self, i);
        click = space = false;
    }
    if (click || space) {
        U64 script = focused_script(self);
        U32 i = outfit_slot_index(self, script);
        U32 o = i < visible_slots() ? slot_outfit(i) : kOutfitSlots;
        LOG("%s: focused slot %u (script %llX) -> outfit %u of %zu", click ? "click" : "Space", i,
            script, o, g_outfits.size());
        std::uint8_t mode = i < visible_slots() ? g_slot_mode[i] : std::uint8_t(kModeMain);
        bool press = click || space;
        if (press && mode == kModeSettings && o <= g_outfits.size()) cfg_enter(self, o); // (the "+" slot: the plugin's)
        else if (press && o == g_outfits.size() && o < kOutfitSlots) save_new_outfit(self);
        else if (press && o < g_outfits.size()) start_loading(self, o), refresh_wheel(self);
    }
    // The worn armor can also change outside our loader (Armor tab, game events): rechecked when a model swaps (without
    // that hook: a few times a second).
    static U64 worn_checked = 0;
    bool due = g_model_swap_orig ? g_worn_dirty.exchange(false) : GetTickCount64() - worn_checked > 250;
    if (due) {
        worn_checked = GetTickCount64();
        if (find_worn_outfit(self) != g_worn_outfit) apply_outfit_slots(self);
    }
    // Pieces can leave the inventory while the menu is open (dropped, sold): the greyed-out outfits follow.
    if (U64 now = GetTickCount64(); now - g_missing_checked > 1000 && refresh_missing(self)) apply_outfit_slots(self);
    if (g_worn_changed) {
        g_worn_changed = false;
        center_restore(); // the Equipped tag follows
        center_for_focus(self, focused_script(self), false);
    }
    if (g_center_frames > 0) {
        --g_center_frames;
        center_show(self);
    }
}

// Call counters (logged) tell which callbacks run every frame while the menu is open.
struct CallStat {
    const char* name;
    U32 calls = 0;
    U64 since = 0;
    void hit()
    {
        if (calls++ == 0) {
            since = GetTickCount64();
            LOG("%s: first call (thread %lu, ui thread %lu)", name, GetCurrentThreadId(), g_ui_thread);
        } else if (calls == 600) {
            LOG("%s: 600 calls in %llu ms", name, GetTickCount64() - since);
        }
    }
};
CallStat g_frame_stat{"frame getter"};
CallStat g_select_stat{"cursor select"};

// Stick selection: the cursor maps the stick angle to one of the tab's sectors; on the Outfits tab a
// sector without a visible slot keeps the current focus (as a centered stick does).
U64 __fastcall cursor_select_hook(U64 self, U64 cursor, U64 flag, U64 idx, U64 a5, U64 a6)
{
    if (g_menu_open) g_select_stat.hit();
    pump(self);
    short i = static_cast<short>(idx);
    if (i >= 0 && U32(i) >= visible_slots() && outfits_current(self) && !in_depth(self))
        idx = (idx & ~0xFFFFull) | 0xFFFF;
    return g_select_orig(self, cursor, flag, idx, a5, a6);
}

// The HUD (always up in play) runs the same per-frame base function: a tick for what must finish with the wheel
// closed (the stash check, which reports). One comparison per frame while nothing is pending.
using FrameFn = float(__fastcall*)(U64);
FrameFn g_hud_frame_orig = nullptr;



// Using a backpack (the Insect Gatherer's opens and collects): no layer call or drawn part tells, the object docked on the
// bag runs it, so its own state is read (ConditionData_DockingGimmickState's path). Every bag with an object has this tag.
// A state is the hash of its name (lowercase; hashlittle, seed 0xC5EDE). Anything but a resting state is the bag in use:
// the objects name theirs differently (Collect, Active, Attack...). Read only while the character has a Bag with a look.
constexpr U32 kBagDockTag = 0xB89AF0D1; // equip_backpack
constexpr U32 kBagResting[] = {
    0x52029BCF, // Initial
    0x866C7489, // Wait
    0x150B14D0, // GimmickOn
    0xA488AC26, // Deactive
    0xC47247D0, // GimmickOn_Hide (the game's own hidden mode)
    0xBFBE4D8B, // GimmickOn_Deactive
};

bool docked_state(U64 actor, U32 tag, U32& state)
{
    using ListFn = void(__fastcall*)(U64, U32, void*);
    using GetFn = void(__fastcall*)(U64, U64*, U32);
    state = 0;
    U64 h[4] = {};
    bool ok = false;
    __try {
        U64 mgr = rp(rp(actor + 0x68) + 0x170);
        if (!mgr || r32(mgr + 0x98) > 64) return false; // (more than the list holds: the call would allocate)
        struct { U32* p; U32 n, cap; U32 buf[64]; } v{}; // (the buffer follows the header: the game checks for that)
        v.p = v.buf, v.cap = 64;
        reinterpret_cast<ListFn>(g_base + kRvaDockedList)(mgr, tag, &v);
        if (!v.n) return false;
        U64 tls = *reinterpret_cast<U64*>(__readgsqword(0x58));
        U64 reg = rp(rp(g_base + (*reinterpret_cast<std::uint8_t*>(tls + 0x1eb) ? kRvaEntityReg2 : kRvaEntityReg)));
        if (!reg) return false;
        reinterpret_cast<GetFn>(rp(rp(reg) + 0x28))(reg, h, v.p[0]);
        if (static_cast<std::uint8_t>(h[2]) && h[1] && *reinterpret_cast<std::uint8_t*>(rp(h[1] + 0x88) + 1) == 7) {
            state = r32(rp(rp(h[1] + 0x68) + 0x30) + 0x270);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (h[0] || h[1]) release_player(h);
    return ok;
}

void bag_use_tick()
{
    static std::pair<U64, U16> shown{}; // the bag counted as drawn now
    static bool off = !kRvaDockedList || !kRvaEntityReg || !kRvaEntityReg2; // (not found after a patch)
    if (off) return;
    std::pair<U64, U16> owner{};
    {
        std::lock_guard<std::mutex> lock(g_appear_mu);
        if (g_bag_weapon.empty() && !shown.first) return;
    }
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx), c = actor ? rp(actor + kOffActorComp) : 0, inv = c ? rp(c + kOffCompInventory) : 0;
    bool use = false;
    if (inv) {
        {
            std::lock_guard<std::mutex> lock(g_appear_mu);
            for (const auto& [vc, o] : g_bag_weapon)
                if (o.first == inv) owner = o;
        }
        U32 st = 0;
        if (owner.first && docked_state(actor, kBagDockTag, st))
            use = std::find(std::begin(kBagResting), std::end(kBagResting), st) == std::end(kBagResting);
    }
    if (ctx[0]) release_player(ctx);
    std::pair<U64, U16> want = use ? owner : std::pair<U64, U16>{};
    if (want == shown) return;
    std::lock_guard<std::mutex> lock(g_appear_mu);
    g_bag_in_use = want;
    for (const std::pair<U64, U16>& o : {shown, want}) {
        if (!o.first) continue;
        if (std::find(g_look_pending.begin(), g_look_pending.end(), o.first) == g_look_pending.end()) g_look_pending.push_back(o.first);
    }
    LOG("appearance: backpack in use: %s", use ? "yes" : "no");
    shown = want;
}

float __fastcall hud_frame_hook(U64 self)
{
    bag_use_tick();
    if (GetCurrentThreadId() == g_ui_thread) {
        if (g_stash_check.at) stash_verify(self);
        // A load's late parts (a piece taken from another character, a slot let go of) also finish with the wheel
        // closed; its questions need the wheel, so only that part runs here.
        if (g_async.active && !g_menu_open && !g_ask_next && !g_ask_modal) pump_async(self);
        look_pending(), anchor_recheck();
    }
    return g_hud_frame_orig(self);
}

float __fastcall frame_hook(U64 self)
{
    if (g_menu_open) g_frame_stat.hit();
    if ((!g_unhide_retry.empty() || !g_rehide_retry.empty()) && GetCurrentThreadId() == g_ui_thread) {
        retry_restore();
        if (g_unhide_retry.empty() && g_rehide_retry.empty()) LOG("centre restore done");
    }
    pump(self);
    if (GetCurrentThreadId() == g_ui_thread) look_pending(), anchor_recheck();
    return g_frame_orig(self);
}

// Slot focus (stick or mouse): the game fills the centre tooltip from the slot's item, or hides it.
U64 __fastcall focus_hook(U64 self, U64 script, U64 a, U64 b, U64 c, U64 d)
{
    center_restore();
    U64 r = g_focus_orig(self, script, a, b, c, d);
    g_hover_script = script; // (0 or another tab's: nothing of ours hovered; the L2 / R2 prompts follow this)
    if (outfits_current(self)) {
        center_for_focus(self, script, false);
        keep_slot_images(self, false);
        pump(self);
    }
    return r;
}


// Logs class changes on the menu's root element, SetState on the menu script, and any show/hide-like class change
// anywhere while the menu is open (capped per open).

// A scroll on a slot: on ours (Outfits tab, outside a submenu) the slot's mode moves on; the rest is the game's.
// The slot script's root is at +0x28 (the game's scroll handlers read it there).
void scroll_slot(U64 self, U64 slot, U32 i, int step);
bool scroll_mode(U64 slot, int step)
{
    if (!g_menu_open || g_busy || GetCurrentThreadId() != g_ui_thread) return false;
    U64 self = rp(slot + 0x28);
    if (!self || !outfits_current(self) || in_depth(self)) return false;
    U32 i = outfit_slot_index(self, slot);
    if (i >= visible_slots()) return false;
    Busy busy;
    scroll_slot(self, slot, i, step);
    return true;
}

// The scroll itself (mouse wheel through the game's handler above, or a pad's L2 / R2 from pump).
void scroll_slot(U64 self, U64 slot, U32 i, int step)
{
    if (g_eye >= 0) { // the view: a piece's look moves on / back; the page slot steps (never leaves)
        eye_scroll(self, i, step);
        return;
    }
    if (g_cfg >= 0) { // the settings view: a value setting moves on / back
        cfg_scroll(self, i, step);
        return;
    }
    U32 n = mode_count(i);
    std::uint8_t before = g_slot_mode[i];
    g_slot_mode[i] = std::uint8_t((before + n + step) % n);
    LOG("scroll %s on slot %u: mode %u -> %u", step > 0 ? "next" : "prev", i, before, g_slot_mode[i]);
    apply_outfit_slots(self, i);
    center_for_focus(self, slot, false);
}

U64 __fastcall scroll_prev_hook(U64 slot) { return scroll_mode(slot, -1) ? 1 : g_scroll_prev_orig(slot); }
U64 __fastcall scroll_next_hook(U64 slot) { return scroll_mode(slot, 1) ? 1 : g_scroll_next_orig(slot); }

U64 __fastcall close_hook(U64 self, U64 a, U64 b, U64 c, U64 d, U64 e)
{
    bool sub = g_eye >= 0 || g_cfg >= 0;
    reset_modes();
    g_eye = -1, g_eye_step = 0, g_cfg = -1; // (the views end with the menu)
    // Closed in the settings / Visibility: the slots are put back to the outfits as Back does, while the wheel still
    // shows. Left with that view's dots, the next opening's dot changes stayed unshown (the old count's dots) until a
    // hover; closing from the outfits never did that.
    if (sub && GetCurrentThreadId() == g_ui_thread && outfits_current(self)) {
        Busy busy;
        refresh_wheel(self);
    }
    g_menu_open = false;
    g_center_desc.clear();
    abort_prompts("menu closed");
    center_restore();
    return g_close_orig(self, a, b, c, d, e);
}

// Runs when the menu returns from a wheel (which hid every header): show the Outfits header again.
U64 __fastcall gear_mode_hook(U64 self, U64 a, U64 b, U64 c)
{
    U64 r = g_gear_orig(self, a, b, c);
    if (U64 h = outfits_header(self)) {
        show_header(h); // (as the gear mode does for 0 and 1)
        set_label(h);
        apply_outfit_slots(self);
    }
    sync_ring(self);
    return r;
}

// --- Diagnostics hooks --------------------------------------------------------------------------
bool g_dumped_qsi = false;

void features_toast(U64 self);


extern std::map<U64, std::pair<int, int>> g_gear_resist;

// The saved keys against the permanent ids: a piece whose key no longer names its item goes to the key that does (keys are
// renumbered by patches). A saved piece with no id yet gets one only when that exact uid + key is in the inventory, so an
// entry saved before a renumbering can't record a wrong id. The file is copied to OutfitManager_backups before a repair.
void repair_keys()
{
    if (!item_rows()) return;
    if (!g_placeholder_ids.empty()) {
        std::lock_guard<std::mutex> lock(g_keys_mu);
        for (U32 id : g_placeholder_ids)
            if (U16 k = key_of_pid(id); k != 0xFFFF && std::find(g_placeholder_keys.begin(), g_placeholder_keys.end(), k) == g_placeholder_keys.end())
                g_placeholder_keys.push_back(k);
        g_placeholder_ids.clear();
    }
    static std::map<std::pair<U64, U16>, bool> owned; // (uid, key) looked for this session
    int fixed = 0, stamped = 0, gone = 0;
    std::string repairs;
    for (Outfit& o : g_outfits)
        for (OutfitItem& it : o.items) {
            if (it.pid) {
                if (pid_of(it.key) == it.pid) continue;
                U16 k = key_of_pid(it.pid);
                if (k == 0xFFFF) {
                    ++gone;
                    LOG("key repair: \"%s\" equipment-%u: item id %X is no longer in the game (key %u kept)", o.name.c_str(), it.slot, it.pid, it.key);
                    continue;
                }
                repairs += " " + std::to_string(it.key) + "->" + std::to_string(k);
                it.key = k;
                ++fixed;
            } else if (std::pair<U64, U16> id{it.uid, it.key}; owned.try_emplace(id, false).second) {
                owned[id] = find_copy(it.uid, it.key) != 0;
            }
            if (!it.pid && owned[{it.uid, it.key}]) {
                it.pid = pid_of(it.key);
                stamped += it.pid != 0;
            }
        }
    if (fixed) {
        std::wstring dir = g_store_path.substr(0, g_store_path.find_last_of(L"\\/")) + L"\\OutfitManager_backups";
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[64];
        swprintf_s(name, L"\\OutfitManager.outfits.%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        bool copied = CopyFileW(g_store_path.c_str(), (dir + name).c_str(), FALSE) != 0;
        LOG("key repair: %d pieces:%s (file copied first: %s)", fixed, repairs.c_str(), copied ? "yes" : "NO");
        g_item_resist.clear();
    }
    if (stamped) LOG("key repair: %d pieces got their permanent id", stamped);
    if (fixed || stamped) save_outfits();
}

// --- Self-test ----------------------------------------------------------------------------------------------------
// A patch can move a field no pattern can see. So at the first menu open each one is read on the real thing, read-only:
// what doesn't look right turns its feature off (with the usual toast) instead of working on the wrong memory. A check
// that can't be made yet (no player, nothing worn) stays open for the next menu open.
void feature_off(const char* name);

bool readable_name(const std::string& n)
{
    if (n.empty() || n.size() > 128) return false;
    for (unsigned char c : n)
        if (c < 0x20 || c == 0x7f) return false;
    return true;
}

bool g_tab_checked = false;

// The resistance ids / lists, checked once against items of known values before any climate is worked out (the gear
// wheel's self-test and the Outfit Arc's opening both call it: 2026-11-06, the arc opened first and read every
// outfit's resistance as 0, so its thermometers didn't show).
void ensure_resist_ids()
{
    static bool done = false;
    if (!done && g_item_info && item_rows()) done = true, calibrate_resist_ids();
}

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
void ensure_resist_ids(); // (below, with the self-test)
void prepare_outfits()
{
    ensure_resist_ids();
    settle_load_now();
    pull_active();
    if (U16 ch = current_character(); ch != kNoCharacter) switch_character(ch);
    repair_keys();
}

U64 __fastcall open_hook(U64 self, U64 a, U64 b, U64 c)
{
    if (g_ui_thread && g_ui_thread != GetCurrentThreadId())
        LOG("ui thread: the wheel opens on %lu, the status screen gave %lu", GetCurrentThreadId(), g_ui_thread);
    g_ui_thread = GetCurrentThreadId();
    center_restore();
    prepare_outfits(); // (each character has its own outfits: the wheel shows those of the one it opens for)
    U64 r = g_open_orig(self, a, b, c);
    g_menu_open = true;
    g_gear_resist.clear(); // (abyss gears can change between opens: read again)
    { // what each character wears, once per session (names and uids: for reading logs and making test outfits)
        static std::vector<U16> logged;
        U16 ch = g_character;
        if (ch != kNoCharacter && std::find(logged.begin(), logged.end(), ch) == logged.end()) {
            logged.push_back(ch);
            WornRaw w[kWornMax];
            int n = read_worn(self, w, kWornMax);
            LOG("worn by %s (character %u):", character_name(ch), ch);
            for (int i = 0; i < n; ++i)
                if (w[i].key != 0xFFFF)
                    LOG("  equipment-%u key %u \"%s\" uid %llX", w[i].slot, w[i].key, item_name(w[i].key).c_str(), w[i].uid);
        }
    }
    reset_modes();
    g_force_images = 3;
    note_menu_binding();
    bool layout_bad = g_fake_key.load() != 0xFFFF ? !layout_ok(self) : !g_layout_bad.empty(); // (build-time failure)
    if (layout_bad && !g_layout_toast_done && g_layout_bad != "menu") {
        g_layout_toast_done = true;
        notify(self, lang::f("toast.layout_changed", {{"what", g_layout_bad}}));
    }
    self_test(self);
    features_toast(self);
    if (U64 h = outfits_header(self)) {
        // The thtml hides it: only a working plugin shows it, and only beside a visible Weapons header (gear mode, not
        // the comrades / items wheels). Visible = no hiding class: the other wheels hide it with "cpp-hide", the thtml
        // with "!cpp-none" (a fresh header has neither, nor "cpp-show").
        U64 weapons = rp(rp(rp(self + kOffRootTabBar) + kOffTabHeaders));
        U64 we = weapons ? rp(weapons + 8) : 0;
        if (we && !has_class(we, "cpp-hide") && !has_class(we, "!cpp-none")) show_header(h);
        set_label(h);
        apply_outfit_slots(self);
    }
    sync_ring(self);
    if (!g_dumped_qsi) {
        g_dumped_qsi = true;
        dump_quickslotinfo();
        LOG("icon script flag %u, ui thread %lu", r32(g_base + kRvaIconScriptFlag) & 0xFF, g_ui_thread);
    }
    if (self == g_outfits_root) dump_menu(self, "open");
    return r;
}

U64 __fastcall tab_hook(U64 self, U64 tabbar, U64 flag, U64 old_idx, U64 new_idx, U64 header)
{
    center_restore();
    U64 r = g_tab_orig(self, tabbar, flag, old_idx, new_idx, header);
    if (tabbar == rp(self + kOffRootTabBar)) {
        if (U32(new_idx) == g_fake_key.load()) {
            apply_outfit_slots(self);
            g_force_images = 3;
        }
        sync_ring(self);
    }
    return r;
}

// --- Setup --------------------------------------------------------------------------------------
// Game addresses come from the resolver (resolver.h): cached for this exe, else the known build's, else searched.
// A missing address turns off only the feature that needs it; the core (the Outfits tab itself) is all or nothing.
#include "om_sigs.h"

constexpr std::size_t kRecipeCount = std::size(kRecipes);
resolver::Result g_resolved[kRecipeCount];
std::wstring g_cache_path;
std::string g_features_off; // named in one toast at the first menu open (empty = everything works)
bool g_features_toast_done = false;

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

// Optional: without it saving works as before and names stay "Outfit N".
void install_rename_prompt()
{
    if (!have({kRvaTextModalVt, kRvaTextModalClick, kRvaOpenTextModal, kRvaModalParamsInit, kRvaModalParamsFree,
               kRvaFindView, kRvaGetText, kRvaNameInvalid, kRvaHideScript, kOffModalOwner, kOffModalD0})) {
        feature_off("feature.renaming");
        return;
    }
    void** vt = reinterpret_cast<void**>(g_base + kRvaTextModalVt);
    g_find_view = reinterpret_cast<FindViewFn>(g_base + kRvaFindView);
    g_params_init = reinterpret_cast<VoidFn>(g_base + kRvaModalParamsInit);
    g_params_free = reinterpret_cast<VoidFn>(g_base + kRvaModalParamsFree);
    g_get_text = reinterpret_cast<GetTextFn>(g_base + kRvaGetText);
    g_name_invalid = reinterpret_cast<NameInvalidFn>(g_base + kRvaNameInvalid);
    g_hide_script = reinterpret_cast<VoidFn>(g_base + kRvaHideScript);
    g_text_click_orig = reinterpret_cast<ModalClickFn>(vt[kSlotTextModalClick]);
    if (!game::patch_vtable(vt, kSlotTextModalClick, reinterpret_cast<void*>(&text_click_hook))) {
        LOG("rename prompt: vtable patch failed; outfits keep their default names");
        feature_off("feature.renaming");
        return;
    }
    if (have({kRvaQueueHide})) // closing the held menu after a prompt (else it stays up until the button is pressed)
        g_queue_hide = reinterpret_cast<QueueHideFn>(g_base + kRvaQueueHide);
    if (have({kRvaBindingInput})) { // ... and releasing its key binding (else the next press only releases it)
        void* t = reinterpret_cast<void*>(g_base + kRvaBindingInput);
        if (MH_CreateHook(t, reinterpret_cast<void*>(&binding_input_hook),
                          reinterpret_cast<void**>(&g_binding_input_orig)) != MH_OK ||
            MH_EnableHook(t) != MH_OK) {
            g_binding_input_orig = nullptr;
            LOG("rename prompt: key binding hook failed; the menu button may need an extra press after a prompt");
        }
    }
    g_open_text_modal = reinterpret_cast<OpenTextModalFn>(g_base + kRvaOpenTextModal); // last: enables the prompt
    LOG("rename prompt ready (TextEdit modal slot %d hooked)", kSlotTextModalClick);
}

// Optional too (needs the rename prompt's helpers): without it a gone piece's slot is simply left as is.
void install_ask_dialog()
{
    if (!g_find_view ||
        !have({kRvaCommonModalVt, kRvaCommonModalClick, kRvaOpenCommonModal, kRvaCommonModalName, kOffModalOwner}) ||
        memcmp(reinterpret_cast<const void*>(g_base + kRvaCommonModalName), "CommonModalMessage", 19) != 0) {
        feature_off("feature.missing_pieces");
        return;
    }
    void** vt = reinterpret_cast<void**>(g_base + kRvaCommonModalVt);
    g_ask_click_orig = reinterpret_cast<ModalClickFn>(vt[kSlotCommonModalClick]);
    if (!game::patch_vtable(vt, kSlotCommonModalClick, reinterpret_cast<void*>(&ask_click_hook))) {
        LOG("missing-pieces question: vtable patch failed; gone pieces are left as is");
        feature_off("feature.missing_pieces");
        return;
    }
    g_open_common_modal = reinterpret_cast<OpenCommonModalFn>(g_base + kRvaOpenCommonModal); // last: enables it
    LOG("missing-pieces question ready (CommonModal slot %d hooked)", kSlotCommonModalClick);
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
extern std::atomic<U32> g_wheel_notches;
void wheel_hook(bool on);             // (the arc's wheel hook: below)
bool g_peek_by_pad = false;           // the open arc came from the pad (L1 / LB held: it closes when that lets go)
std::atomic<bool> g_peek_pad_open{false}; // (the same, for the pad filters on other threads)
bool peek_pad_owned() { return g_peek_live && g_peek_pad_open; }
U8 g_peek_pad_last = 0;               // the pad's buttons on the last frame (for presses)
U8 pad_buttons_raw();                 // (XInput and PlayStation pads, below)
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

// The arc opened or closed: the input filters follow (g_peek_live), the wheel hook goes on / off.
void peek_hotkeys(U64 self, bool free);
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

}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        if (in_game_process()) {
            wchar_t buf[MAX_PATH]; // (the log open now: the gear wheel's files can load before the init thread runs)
            GetModuleFileNameW(module, buf, MAX_PATH);
            std::wstring dir = buf;
            dir.resize(dir.find_last_of(L"/\\") + 1);
            logx::open((dir + L"OutfitManager.log").c_str());
            install_ui_patch(); // (before the game builds its UI: the files load once, at start)
        }
        if (in_game_process())
            if (HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr))
                CloseHandle(t);
    }
    return TRUE;
}
