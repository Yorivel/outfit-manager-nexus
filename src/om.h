// Outfit Manager's shared types, constants and state: included by every module.
#pragma once

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

namespace om {

using U64 = std::uint64_t;
using U32 = std::uint32_t;
using U16 = std::uint16_t;
using U8 = std::uint8_t;

constexpr U64 kUserMax = 0x00007FFFFFFEFFFFull;
constexpr U16 kChestSlot = 4;                 // equip slot "equipment-4" (armor icon) = chest
constexpr U16 kPropBackgroundImage = 0x26;
// Native toast (AlertSystemOverView's ToastMessage), as the game's menus show it: (ui, const char* utf8, error 0);
// ui = any UI script's vfunc +0x118 (kVfUiMgr).
constexpr U64 kVfUiMgr = 0x118; // slot 35 (34 before the 2026-10-30 update)
                                    // strings (+0x58: 16-byte entries, the first u64 the name) - a part's +0x48 indexes it

constexpr U16 kTemplateRecord = 1; // Armor: 12 item slots
constexpr U32 kOutfitSlots = 12;
// #32 outfit pages: up to kPagelessMax outfits share one page with the "+" slot (as before); with more, each page has
// kPageOutfits outfits, the page slot and the "+" slot. kMaxOutfits: a safety cap only.
constexpr U32 kPagelessMax = kOutfitSlots - 1;
constexpr U32 kPageOutfits = kOutfitSlots - 2;
constexpr U32 kMaxOutfits = 200;
constexpr U32 kPageSlot = 0xFFFFFFF0u; // (slot_outfit's value for the page slot)
constexpr U32 kRecordSize = 0x30; // (copied whole; the 2026-10-30 header is 8 bytes shorter)
// QuickSlotInfo record: slot count and slot array (+0x14 / +0x20 before the 2026-10-30 update)
constexpr U32 kRecSlotCount = 0x0c, kRecSlots = 0x18;
constexpr U32 kSlotSize = 0x50;

// The static-info getters take the key by value since the 2026-10-30 update (first argument unused; was const u16*).
using RecordGetFn = U64(__fastcall*)(U64, U32);

using OpenFn = U64(__fastcall*)(U64, U64, U64, U64);
using TabFn = U64(__fastcall*)(U64, U64, U64, U64, U64, U64);
using VoidFn = void(__fastcall*)(U64);
using SetTextFn = void(__fastcall*)(U64, const char*, U64);
using FloatFn = float(__fastcall*)(U64);

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

// Equip writes an error code through a pointer to its 6th argument slot, so pass one to have it reserved.
using EquipFn = int*(__fastcall*)(U64, int*, U16, short, short, U64);
using UnequipFn = int*(__fastcall*)(U64, int*, short);
using InventoryFn = U64(__fastcall*)(U64);
using ErrorMgrFn = U64(__fastcall*)();
using ErrorTextFn = U64(__fastcall*)(U64, const int*);
// The storage screen's move (0xd2c630 calls it): inventory, err, owner ids of the source / destination bag, item key,
// item+0xA, count (64-bit: the whole stack slot is read), source bag, source position, the source bag's transfer rule
// (its entry +4 = the destination bag). It only sends the request: the game checks it a moment later (and says why
// when it refuses), so whether a piece moved is seen in the bag afterwards.
using ItemMoveFn = int*(__fastcall*)(U64, int*, U32, U32, short, U16, U64, U16, U16, U32);
using BagInfoFn = U64(__fastcall*)(U64, U32); // (unused, key)

// Companions (actor kind != 1) only change gear through the game's UI requests (the wheel's / inventory's path):
// equip takes an item copy {200-byte item, u16 +0xc8, u16 bag position +0xca} (its bag comes from the item type).
using EquipRequestFn = int*(__fastcall*)(U64, int*, const void*, short);
using UnequipRequestFn = int*(__fastcall*)(U64, int*, short);
using ToastFn = void(__fastcall*)(U64, const char*, U32);
using FindViewFn = U64(__fastcall*)(U64, const char*);
using OpenTextModalFn = U64(__fastcall*)(U64, U64, unsigned char*, int);
using GetTextFn = void(__fastcall*)(U64, char*, int);
using NameInvalidFn = bool(__fastcall*)(const char*);
using ModalClickFn = U64(__fastcall*)(U64, U64, U64, U64);
using OpenCommonModalFn = U64(__fastcall*)(U64, U64, unsigned char*, const char*);

using InternFn = void(__fastcall*)(U64*, const char*, U32, U32);
using VecAddFn = bool(__fastcall*)(U64, const U32*);
using VecRemoveFn = U32(__fastcall*)(U64, const U32*);

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
    // Weapons: Weapons only (#27): a weapon set. Loading puts on only its weapons (armour left alone), it is never the
    // applied outfit (no looks, nothing to unequip), and its slot shows the main weapon with the weapon set badge, gold
    // while exactly its weapons are worn. Its armour stays saved: back to Load / Leave, it is an outfit again.
    bool weapons_only = false;
    std::string weapon_icon; // (the main weapon's icon, for a weapon set's picture: set when saved / loaded)
    std::uint8_t climate = 0; // Conditions: 0 Auto (from its resistance), 1 Cold, 2 Heat (manual), 3 Off (a badge only)
    std::uint8_t stash = 0;   // Conditions: Stash, 0 = the global setting, 1 on, 2 off
    // Activity (2026-11-09): what the outfit is worn for, put on when the character starts it (several: one at random),
    // the outfit or items worn before back when it ends. kActivitySleep: in bed (sleep.cpp); its own looks win over the
    // game's in-bed hiding. (The first build's "Sleepwear: On" reads as Sleeping.)
    std::uint8_t activity = 0; // kActivityNone / kActivitySleep
    // A temporary outfit (sleep.cpp: the pieces worn before a sleep, going back on): loaded like any other, never saved.
    bool temp = false;
    // Its hotkey (the outfit's settings, 2026-11-06): a key or mouse button (virtual-key code; 0 = none) and the
    // modifiers held with it (kModCtrl / kModAlt / kModShift; none: the key alone). Pressed in play, the outfit loads.
    U16 hk_vk = 0;
    std::uint8_t hk_mods = 0;
    // The character's last loaded outfit (one per character, kept per save): its pieces' looks apply wherever the exact
    // piece is worn in its slot, also after other slots were changed by hand.
    bool worn = false;
    // The Outfit Arc (#32): pinned outfits are on it in pin order (1.. as they were pinned); 0: not pinned (on it among
    // the recent ones while it is one of the last applied); -1: not read (a file from before pins: pinned, in the
    // wheel's order - normalize_pins). used: when it was last applied (a count over the character's outfits; 0 never).
    int pin = -1;
    U32 used = 0;
    // The shield the character always carries on its back (equipment-23: it shows whatever the shield slot holds; on
    // no wheel, so in no outfit): its look in this outfit (a Look), applied to whatever is in that slot.
    std::uint8_t back_look = 0;
    std::uint8_t underwear_look = 0; // (the body's Underwear part: a look like a piece's, no item behind it)
    // Whether that slot held anything when the outfit was saved (it is empty while the shield slot holds a shield):
    // 0 unknown (saved before this was kept), 1 yes, 2 no. The eye view lists the back shield only when it can matter.
    std::uint8_t has_back = 0;
    // Looks of the slots the outfit holds nothing in (a look belongs to the slot: it applies to whatever is worn there).
    std::map<U16, std::uint8_t> empty_looks;
};
constexpr std::uint8_t kActivityNone = 0, kActivitySleep = 1, kActivityCount = 2;
constexpr U16 kBackShieldSlot = 23;
// The display slots (the game's equip slot table, CDInventory/tools/equipslots.py): on no wheel, each shows a carried
// weapon on the body, mirroring a weapon slot - Kliff 22 sword, 23 shield, 24 bow; Damiane 22 rapier, 23 right-hand
// shield, 24 pistol; Oongka 22 two-hand axe, 23 cannon. 23 has its look in Outfit::back_look, 22 / 24 as slot looks.
constexpr U16 kCarriedSlotA = 22, kCarriedSlotB = 24;
constexpr U16 kLanternSlot = 15;
inline bool is_display_slot(U16 slot) { return slot >= kCarriedSlotA && slot <= kCarriedSlotB; }
constexpr U32 kMaxWeaponSlots = 16;
constexpr U32 kWornMax = kOutfitSlots + kMaxWeaponSlots; // worn entries: armour, then weapons

// Every character's outfits; g_outfits is the working copy of g_store[g_character].
// kNoCharacter holds outfits from files written before the lists were per character (adopted by the first
// character the menu opens for).
constexpr U16 kNoCharacter = 0xFFFF;
struct CharacterOutfits {
    U16 character;
    std::vector<Outfit> outfits;
    std::uint8_t helmet_pin = 0; // the helmet key's pin (#34: 0 none, 1 hidden, 2 shown): bound to saves with the outfits
    // #33: the looks while no outfit is applied (only its empty_looks, back_look and underwear_look are used); edited in
    // the Appearance view from the "+" slot's settings, bound to saves with the outfits
    Outfit no_outfit;
    bool no_outfit_set() const
    {
        if (no_outfit.back_look || no_outfit.underwear_look) return true;
        for (const auto& [slot, look] : no_outfit.empty_looks)
            if (look) return true;
        return false;
    }
};
using Store = std::deque<CharacterOutfits>; // deque: references stay valid when a character is added
constexpr std::uint8_t kEyeSteps = 3;

// The Armor wheel's 12 slots don't fit one page with the page slot: its accessories get their own. Equipment slots
// (named from the pieces in them): 7 / 8 earrings, 9 necklace, 10 / 11 rings, 17 circlet, 18 mask; the armour page
// keeps 3 hat, 4 chest, 5 gloves, 6 boots, 16 cloak.
constexpr U16 kAccessorySlots[] = {7, 8, 9, 10, 11, 17, 18};

constexpr size_t kBackPiece = ~size_t(0); // (eye_pieces: the back shield, not one of the outfit's items)
// The underwear: every character's model has a part named "Underwear" (under the armour, the game hides it): its look
// is published under a slot no equipment uses, and the looks find its part by name.
constexpr size_t kUnderwearPiece = ~size_t(1);
constexpr U16 kUnderwearSlot = 30;

// Saved outfits show their chest piece on the slot's item image (ItemCoolTimeIconOutfit).
struct SlotImage {
    U64 elem = 0;
    std::string icon; // last icon set on elem
    bool ok = false;
    bool shown = false; // should be visible
    bool missing = false; // greyed out (om-missing): the outfit lacks a piece
};
// --- Slot modes: scrolling cycles what an Outfits slot does --------------------------------------
// Like vanilla's item cycling (the dots under a slot): a slot cycles its action (a saved outfit: click loads; the "+"
// slot: click saves) / settings (click opens the settings view). The game's scroll handlers (slot vtable 181 / 182) are hooked; on our
// slots the mode changes instead. Modes stay per slot while the wheel is open and all reset when it closes.
enum SlotMode : std::uint8_t { kModeMain, kModeSettings, kModeConditions, kModeAppearance }; // (eye: saved outfits)
// The managed outfit's third dot (#21): its looks stop applying - nothing is taken off - until an outfit is loaded or
// saved. (The old Conditions page's value: that page is gone.)
constexpr std::uint8_t kModeRelease = 2;
// (per character: the outfit let go - no outfit is taken up by itself then, not even another one matched by hand (its
// looks would hide pieces unasked); a load or a save clears it)
extern std::map<U16, int> g_unmanaged;
bool let_go(int outfit); // (that outfit of the current character was let go)
void release_outfit(U64 self);
using ScrollFn = U64(__fastcall*)(U64);
using SelectorSelectFn = void(__fastcall*)(U64, U32, U64);
constexpr int kErrNoItemAfterRemoval = 0x69BEDE43;

// The settings' descriptions (the settings view's centre caption), by group and entry: {title, description}.
enum Page { kPageNone, kPageSettings, kPageOutfit, kPageConditions, kPageSettings2 };
extern int g_busy;
struct Busy {
    Busy() { ++g_busy; }
    ~Busy() { --g_busy; }
};

// only: just that slot (a scroll changes one slot; re-styling them all made the other badges flicker).
// An item's Ice / Fire Resistance level: its passive list (item info +0x118 -> {entries of {u32 kind, u32 level}*,
// u32 count}). The kind's low 16 bits are the passive's id (the high 16 change per session: a runtime handle): 0x704
// Fire Resistance, 0x705 Ice, 0x706 Lightning; the others are other passives (0x70a fishing cooldown, 0x6xx ...).
// Levels add up over the worn pieces; 5 / 10 / 15 each cancel one of the cold / heat gauge's three bars, 15 also gives
// immunity to Freeze / Burn. Item data never changes in a session: cached per key.
using ItemInfoFn = U64(__fastcall*)(U64, U32); // (unused, key)

const char* const kBadges[] = {"om-plus-big", "om-cog-big", "om-plus-c", "om-cog-c", "om-flag-c", "om-cold-c", "om-heat-c",
                               "om-cold2-c", "om-heat2-c", "om-eye-c", "om-look-shown-c", "om-look-hidden-c",
                               "om-look-combat-c", "om-look-peace-c", "om-return-c", "om-prompt-c", "om-cross-c",
                               "om-abtn-c", "om-unequip-c", "om-wset-c", "om-wset-gold-c", "om-wset-busy-c",
                               "om-sleep-c"};
// The game's slot refresh (after every equipment change) re-selects every slot's dots with its own count (0 for ours:
// no items) and -1: the select is hooked, and for our slots' selectors it gets our count and mode instead, so the dots
// never go blank. Our own calls pass through it as well. Our slots are looked up live (the Outfits menu's slot list).
using SelectorSelectHookFn = void(__fastcall*)(U64, U32, U64);

// The gear slots' refresh (EquipSlotVt slot 164, 0xef5900: after every equipment change) rebuilds a slot's item list
// from the inventory and shows its current item (0xef9b80); ours hold none, so it blanked them and the plugin drew them
// again (they flickered). Everything our slots show is ours: their refresh is skipped; every other slot's runs.
using SlotRefreshFn = void(__fastcall*)(U64);

// Save binding. The game's save writes / loads are seen on its own threads (file hook, folder watch), so they work
// on g_published, a copy of the active set the UI thread publishes after each change (all under g_store_mu).
// A load replaces g_published and bumps g_generation; the UI thread then takes it over (pull_active).
// A save is recognised by its save.save content (hash), with size + write time as the quick check. A save whose bytes
// changed (a save editor) is still the same save if its world clock and place match (savekey.cpp).
struct SaveId {
    U64 size = 0, time = 0, hash = 0; // 0 = not known
    U64 clock = 0;                    // the save's world clock (_fieldTimeRaw), 0 = not known
    U32 place = 0;                    // a hash of the position it was made at
    bool same(const SaveId& o) const
    {
        if (hash && o.hash) return hash == o.hash;
        return size && size == o.size && time == o.time;
    }
    bool same_moment(const SaveId& o) const { return clock && clock == o.clock && place && place == o.place; }
};
struct BoundSet {
    SaveId id;
    Store store;
};
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

// The two hands' weapon slots (equipment-0: MainWeapon_*_R, equipment-1: MainWeapon_*_L). Two one-handed weapons worn
// in each other's hands count as the outfit's (2026-11-06, a player's log: the game refuses to take either off -
// "no items available to be equipped after removal" - so a load could never swap them back; every load ended 18 of 20
// and the outfit showed as modified).
constexpr U16 kHandR = 0, kHandL = 1;

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

// The battle state itself (the headgear decision asks it: kRvaBattleCheck(actor)); -1 when it can't be asked.
using BattleCheckFn = std::uint8_t(__fastcall*)(U64 actor);

// The game's in/out check (drawing / sheathing, per part): a weapon with a look drawn or put away gets its looks synced
// on the next UI frame (not here: the game is in the middle of laying the character's parts out).
using EquipVisCheckFn = float(__fastcall*)(U64 vc, const U32* part, std::uint8_t* rec, std::uint8_t drawn);

// The model rebuild job's swap (a job thread): pieces put on outside the wheel (the Armor tab, the game) get their new
// model parts here, without a headgear switch; their character is synced on the next UI frame.
using ModelSwapFn = void(__fastcall*)(U64 job, U64, U64, char, std::uint8_t*);
constexpr int kFault = INT32_MIN;

constexpr U16 kBagCarried = 1, kBagKeyItems = 4, kBagStorage = 7, kBagWardrobe = 14;

// A companion's piece in storage / the wardrobe: the UI request derives the bag from the item's type (the carried bag),
// but the request under it (0x9e0290) takes a bag id, as Kliff's direct equip does. For our call it gets the piece's
// own bag (hooked; every other call passes through untouched), so the game's own path does the rest.
using CompanionEquipFn = U64(__fastcall*)(U64, U64, U64, U64, U64, U64);

// Bag = {item[] (200 bytes each) @0, short capacity @+0xc, u16 id @+0x10}; the inventory manager lists them.
struct ItemAt {
    U16 bag;
    short pos;
    U64 item; // its 200 bytes in the bag
};

constexpr U64 bag_bit(U16 bag) { return bag < 64 ? 1ull << bag : 0; }
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

// --- Where the pieces are -----------------------------------------------------------------------
// A saved piece is somewhere while its uid is: worn by this character, in a bag (carried, storage, wardrobe, other) or
// worn by another character. One that is nowhere (sold, dropped, dismantled) can be replaced by another copy of the
// same item (key) that the outfit does not use yet. (Moving an item into / out of storage or the wardrobe gives it a
// new uid: adopt_returned follows it.)
enum class Where : std::uint8_t { Worn, Carried, Storage, Wardrobe, Other, Companion };
std::string where(U64 a);
struct Owned {
    U64 uid;
    U16 key;
    Where where;
    U16 who = kNoCharacter; // Companion: whose
    U64 actor = 0;          // Companion: their actor
    U16 slot = 0xFFFF;      // Companion: the equip slot they wear it in
};

// then searches 3 pointer hops out from the controlled actor and the player holder for those actors (pointers) or
// their u32 ids (actor+0x60).

struct PieceCheck {
    std::vector<std::pair<size_t, U64>> copies; // outfit item index -> uid of the copy offered for it
    std::vector<size_t> gone;                   // item indices with no copy
};
using ItemInfoFn = U64(__fastcall*)(U64, U32); // (unused, key)

// A prompt takes the input while it is open, so the gear menu misses the release of the button that holds it open
// and stays up afterwards. Once a prompt is gone, do what the game's release handler (0x1098f10, event 1) does.
using QueueHideFn = void(__fastcall*)(U64, U64);

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

enum Confirm { kConfirmNone, kConfirmOverwrite, kConfirmDelete, kConfirmSwap, kConfirmCleanSave, kConfirmUninstall };

enum CfgKind : std::uint8_t {
    kCfgVisibility, kCfgClimate, kCfgWeapons, kCfgStash, kCfgPosition, kCfgRename, kCfgOverwrite, kCfgDelete, // a saved outfit's
    kCfgGStash, kCfgParty, kCfgMessages, kCfgClear, kCfgUninstall,                               // the "+" slot's
    kCfgBack, kCfgLanguage, kCfgPeekKey, kCfgPeekPad, kCfgHotkey, kCfgEffects, kCfgActivity, kCfgHelmetKey
};

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

// Each saved piece and whether it goes away: only when every outfit using it has Stash on (one with it off keeps the
// piece in the bag: Off wins, so a piece shared with an outfit kept at hand stays at hand).
struct StashDest {
    U64 uid;
    U16 key;
    bool on; // goes away (every outfit using it has Stash on)
};

// The HUD (always up in play) runs the same per-frame base function: a tick for what must finish with the wheel
// closed (the stash check, which reports). One comparison per frame while nothing is pending.
using FrameFn = float(__fastcall*)(U64);

// Controllers: the stick picks a slot (the game's own cursor) and L2 / R2 scroll it through the game's own handler once
// the L2 / R2 prompts show (pad_prompts). The wheel has no confirm button of its own, so the pad is read directly and
// Cross / A counts as Space:
//  - Xbox pads (and anything shown as one, e.g. Steam Input's virtual pad): XInput, loaded at run time; a port that
//    reads as disconnected is skipped for 2 s (polling an empty port is slow).
//  - PlayStation pads (DualSense, DualSense Edge, DualShock 4; the game reads them natively, XInput never sees them):
//    their HID input reports, read by a thread that waits for the pad (no polling); the latest buttons are kept here.
constexpr U8 kPadCross = 1;   // the bits pad_buttons() returns: Cross / A held,
constexpr U8 kPadL1 = 4;      // L1 / LB held (the Outfit Arc's modifier),
constexpr U8 kPadUp = 8;      // the d-pad's up (or a diagonal with it),
constexpr U8 kPadDown = 16;   // and down

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

// On a mount the HP tracker (60% up the tracked box: rider and mount together) sits at the rider's head, and the arc
// with it (2026-11-07). The mount's own stamina tracker sits at 75% of the mount's box - its back, where the rider's
// hips are, for every mount. So on a mount the HP tracker follows the mount at that ratio (arc.cpp peek_ride_place).
// Riding (build 26480954, found by diffing the player on foot / mounted in two sessions; fields straight in the actor
// held the mount's id in one session and 0 in the next - list entries): in the actor's component, [comp+0x1B8]+0x30 is a
// riding flag (0 / 1) and [comp+0x1A0]+0xC8 the ridden actor's id (-1 on foot). Riding only when both say so; anything
// else (a patch moving them) is "not riding", which leaves the arc where it always was.
constexpr U64 kOffCompRide = 0x1B8, kOffRideFlag = 0x30, kOffCompRidden = 0x1A0, kOffRiddenId = 0xC8;
// In that same object +0x91 is a state byte: 0x10 riding, 0x0B from a dismount's start until the rider is off (2026-11-07,
// six dismounts on two mounts: back to 0x10 after 0.94 s / 1.24 s; other values on foot later).
constexpr U64 kOffRiderState = 0x91;
constexpr U8 kRiderGettingOff = 0x0B;
// A tracker's follow info (build 26480954, 2026-11-07: found by comparing the HP tracker with the mount's stamina one):
// [elem+0xE0]+0xD0 is the followed actor's id, +0xEC its obb-height-ratio (HP 0.6, mount stamina 0.75). The game reads
// both every frame: the tracker moves the next frame.
// The outfit transition and the hidden weapons' effects (transition.cpp)
bool install_transition();
bool fx_defer_load(U64 self, U32 index); // true: the load waits for the glow up (it is started from fx_tick)
int fx_busy_target();                     // while the effect plays: the outfit going on, -2 Unequip Outfit; else -1
bool fx_defer_release(U64 self);          // the same for Unequip Outfit: release_outfit runs at the glow's peak
void fx_tick(U64 screen);                // the UI frame
int transition_setting();                // the Effects setting: 0 off, 1 performance, 2 quality
constexpr U64 kOffTrkFollow = 0xE0, kOffFollowActor = 0xD0, kOffFollowRatio = 0xEC;
constexpr float kTrkRatio = 0.6f, kMountRatio = 0.75f;

// (defined in core.cpp)
extern U64 g_base;
extern HMODULE g_self;
std::uint8_t look_pos(std::uint8_t look);
extern std::vector<Outfit> g_outfits;
extern std::wstring g_store_path;
extern U16 g_armor_slots[kOutfitSlots];
extern U32 g_armor_slot_count;
extern U16 g_weapon_slots[kMaxWeaponSlots];
extern std::atomic<U32> g_weapon_slot_count;
bool is_weapon_slot(U16 slot);
void log_unlisted_slots(); // (the log: equipped items in slots on neither wheel - ammo, the DLC's gear)
void log_weapon_wheel(U64 self); // (the log: what each Weapons wheel position holds, also ones with no equip slot)
bool uses_weapons(const Outfit& o);
bool is_weapon_set(const Outfit& o); // (Weapons: Weapons only, #27)
int worn_weapon_set(U64 self);       // (the weapon set whose weapons are exactly the worn ones, or -1)
bool loads_piece(const Outfit& o, U16 slot);
extern Store g_store;
extern U16 g_character;
extern std::atomic<U32> g_outfit_count;

} // namespace om

#include "mem.h"
