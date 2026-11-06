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
constexpr U32 kOutfitSlots = 12;
constexpr U32 kMaxOutfits = kOutfitSlots - 1; // the last slot is always the "+" (saving, the global settings)
constexpr U32 kRecordSize = 0x30; // (copied whole; the 2026-10-30 header is 8 bytes shorter)
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
constexpr U32 kMaxWeaponSlots = 16;
constexpr U32 kWornMax = kOutfitSlots + kMaxWeaponSlots; // worn entries: armour, then weapons

// Every character's outfits; g_outfits is the working copy of g_store[g_character].
// kNoCharacter holds outfits from files written before the lists were per character (adopted by the first
// character the menu opens for).
constexpr U16 kNoCharacter = 0xFFFF;
struct CharacterOutfits {
    U16 character;
    std::vector<Outfit> outfits;
};
using Store = std::deque<CharacterOutfits>; // deque: references stay valid when a character is added

// The Armor wheel's 12 slots don't fit one page with the page slot: its accessories get their own. Equipment slots
// (named from the pieces in them): 7 / 8 earrings, 9 necklace, 10 / 11 rings, 17 circlet, 18 mask; the armour page
// keeps 3 hat, 4 chest, 5 gloves, 6 boots, 16 cloak.
constexpr U16 kAccessorySlots[] = {7, 8, 9, 10, 11, 17, 18};

constexpr size_t kBackPiece = ~size_t(0); // (eye_pieces: the back shield, not one of the outfit's items)

// The two hands' weapon slots (equipment-0: MainWeapon_*_R, equipment-1: MainWeapon_*_L). Two one-handed weapons worn
// in each other's hands count as the outfit's (2026-11-06, a player's log: the game refuses to take either off -
// "no items available to be equipped after removal" - so a load could never swap them back; every load ended 18 of 20
// and the outfit showed as modified).
constexpr U16 kHandR = 0, kHandL = 1;

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
bool uses_weapons(const Outfit& o);
bool loads_piece(const Outfit& o, U16 slot);
extern Store g_store;
extern U16 g_character;
extern std::atomic<U32> g_outfit_count;

} // namespace om

#include "mem.h"
