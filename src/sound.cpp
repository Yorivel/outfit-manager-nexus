// Sounds (2026-11-10): the played character's gear sounds follow his looks, and an outfit change doesn't play the
// game's equip sounds.
//
// The game's audio is Wwise, and its exe exports the Wwise API by name (AK::SoundEngine::PostEvent, SetSwitch, ...):
// those exports are hooked. Wwise ids are FNV-1 (32 bit) of the lowercase names; the names come from the game's
// soundbanksinfo (CDInventory/tools/wwnames.py). Found with a recording build (game 2.04.08):
// - armorkey: a switch the game sets on the character from what he wears (skin / cloth / leatherarmor / platearmor);
//   his armour loop (sfx_move_armor_lp, started by the game after each change of gear) and other gear sounds follow it;
// - sfx_move_plateshoe: with every step (the game plays it in cloth outfits too: left alone while the armour shows);
// - sfx_move_shield: with every step, the shield on the back;
// - sfx_collide_metal: physics - metal parts knocking together when he starts / stops. Collision sounds play on a pool
//   of emitters (objects 400001...), each placed where the collision is; the switch collidetype tells a footstep from
//   gear (defaultcollide). His own are within ~1.1 m of his object;
// - two unnamed events on object 2: the equip sounds, one pair per change of gear.
// The lantern only sounds while held (shown then): nothing to do.
//
// While no worn body armour piece shows (helm, chest, gloves, boots: hidden by their looks), armorkey is kept at skin
// (the armour loop started again so it takes it), and the plate steps and the metal collisions next to him aren't
// played; the shield's rattle goes with a hidden shield. With Unequip Outfit (no looks) nothing is changed.
// The played character's object is 100001 (seen so for Kliff; the other characters are untested).
#include "om.h"
#include "load.h"
#include "looks.h"
#include "log.h"
#include <MinHook.h>
#include <atomic>
#include <cstring>

namespace om {

namespace {

constexpr U64 kPlayerObj = 0x186A1, kUiObj = 2, kPoolFirst = 0x61A80;
constexpr U32 kPoolSize = 256;
constexpr U32 kEvArmorLoop = 0x34F371D9, kEvPlateShoe = 0x554A8706, kEvShield = 0x74C41A56, kEvCollideMetal = 0x0D27F39D,
              kEvEquipA = 0x2361D7A5, kEvEquipB = 0xB074E90E;
constexpr U32 kSwArmorKey = 0x3921C2FF, kSwSkin = 0x310330D2, kSwCollideType = 0x29235BB9, kSwFootstep = 0x6F394B77;
constexpr U16 kShieldSlot = 13;
constexpr double kOwnReach = 1.5; // (m: his own gear's collisions were 0.3 - 1.1 from his object)

using PostFn = U32(__fastcall*)(U32 id, U64 go, U32 flags, void* cb, void* cookie, U32 n, void* ext, U32 playing);
using SwitchFn = U32(__fastcall*)(U32 group, U32 state, U64 go);
using PosFn = U32(__fastcall*)(U64 go, const void* xform, U32 flags);
using ActionFn = U32(__fastcall*)(U32 event, U32 action, U64 go, int ms, int curve, U32 playing);
PostFn g_post = nullptr;
SwitchFn g_switch = nullptr;
PosFn g_setpos = nullptr;
ActionFn g_action = nullptr;

double g_pool_pos[kPoolSize][3], g_player_pos[3]; // (written on the game's threads, read on them: plain values)
bool g_pool_gear[kPoolSize];                      // (the emitter's last collidetype wasn't footstep)
std::atomic<U32> g_game_armor{0}, g_armor_applied{0}; // (armorkey: the game's last value, and what is set)

bool armour_hidden() // (some armour look hides a piece, and no worn body armour piece shows: looks.cpp, bit 31)
{
    const U32 h = look_hidden_slots();
    return (h & ((1u << 3) | (1u << kChestSlot) | (1u << 5) | (1u << 6))) && !(h & (1u << 31));
}

// (every playing instance of an event on an object: the armour loop is started again by the game after each change
// of gear and by sound_tick - one at a time, or they pile up)
void stop_event(U32 event, U64 go)
{
    if (g_action) g_action(event, 0 /* stop */, go, 100, 4 /* linear */, 0);
}

bool muted(U32 id, U64 go)
{
    if (go == kPlayerObj) {
        if (id == kEvPlateShoe) return armour_hidden();
        if (id == kEvShield) return (look_hidden_slots() & ((1u << kShieldSlot) | (1u << kBackShieldSlot))) != 0;
    } else if (go == kUiObj) {
        if (id == kEvEquipA || id == kEvEquipB) return g_async.active || GetTickCount64() - g_async.started < 500;
    } else if (id == kEvCollideMetal && go - kPoolFirst < kPoolSize) {
        const U32 i = U32(go - kPoolFirst);
        if (!g_pool_gear[i] || !armour_hidden()) return false;
        double d2 = 0;
        for (int k = 0; k < 3; ++k) d2 += (g_pool_pos[i][k] - g_player_pos[k]) * (g_pool_pos[i][k] - g_player_pos[k]);
        return d2 < kOwnReach * kOwnReach;
    }
    return false;
}

U32 __fastcall post_hook(U32 id, U64 go, U32 flags, void* cb, void* cookie, U32 n, void* ext, U32 playing)
{
    if (muted(id, go)) return 0; // (AK_INVALID_PLAYING_ID: not played)
    if (id == kEvArmorLoop && go == kPlayerObj) stop_event(id, go); // (ours, or an older one of the game's)
    return g_post(id, go, flags, cb, cookie, n, ext, playing);
}

U32 __fastcall switch_hook(U32 group, U32 state, U64 go)
{
    if (group == kSwCollideType && go - kPoolFirst < kPoolSize) g_pool_gear[go - kPoolFirst] = state != kSwFootstep;
    if (group == kSwArmorKey && go == kPlayerObj) {
        g_game_armor = state;
        if (armour_hidden()) state = kSwSkin;
        g_armor_applied = state;
    }
    return g_switch(group, state, go);
}

// (where each emitter is: AkWorldTransform = front, top (3 floats each), position (3 doubles, metres))
U32 __fastcall setpos_hook(U64 go, const void* xform, U32 flags)
{
    if (xform && (go == kPlayerObj || go - kPoolFirst < kPoolSize))
        std::memcpy(go == kPlayerObj ? g_player_pos : g_pool_pos[go - kPoolFirst], static_cast<const char*>(xform) + 24, 24);
    return g_setpos(go, xform, flags);
}

template <typename F>
bool hook_export(const char* mangled, void* hook, F* orig)
{
    void* t = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), mangled));
    return t && MH_CreateHook(t, hook, reinterpret_cast<void**>(orig)) == MH_OK && MH_EnableHook(t) == MH_OK;
}

} // namespace

void sound_install()
{
    const bool post = hook_export(
        "?PostEvent@SoundEngine@AK@@YAII_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z",
        reinterpret_cast<void*>(&post_hook), &g_post);
    const bool sw = hook_export("?SetSwitch@SoundEngine@AK@@YA?AW4AKRESULT@@II_K@Z", reinterpret_cast<void*>(&switch_hook), &g_switch);
    const bool pos = hook_export("?SetPosition@SoundEngine@AK@@YA?AW4AKRESULT@@_KAEBVAkWorldTransform@@W4AkSetPositionFlags@@@Z",
                                 reinterpret_cast<void*>(&setpos_hook), &g_setpos);
    g_action = reinterpret_cast<ActionFn>(GetProcAddress(
        GetModuleHandleW(nullptr),
        "?ExecuteActionOnEvent@SoundEngine@AK@@YA?AW4AKRESULT@@IW4AkActionOnEventType@12@_KHW4AkCurveInterpolation@@I@Z"));
    if (!post) g_post = nullptr;
    if (!sw) g_switch = nullptr;
    if (!pos) g_setpos = nullptr;
    LOG("sounds: play %s, switches %s, positions %s, stop %s%s", post ? "watched" : "NOT found", sw ? "watched" : "NOT found",
        pos ? "watched" : "NOT found", g_action ? "found" : "NOT found",
        post && sw && pos && g_action ? "" : " (hidden gear may still sound, or outfit changes play the equip sounds)");
}

// The HUD frame: the armour was hidden / shown since - the switch, and the armour loop again so it takes it.
void sound_tick()
{
    const U32 game = g_game_armor.load();
    if (!game || !g_switch || !g_post) return;
    const U32 want = armour_hidden() ? kSwSkin : game;
    // (settled for 300 ms, and no outfit change running: the looks flicker while an outfit changes)
    static U32 pending = 0;
    static U64 since = 0;
    const U64 now = GetTickCount64();
    if (want != pending) pending = want, since = now;
    if (want == g_armor_applied.load() || now - since < 300 || g_async.active || look_changing() || fx_transition_busy()) return;
    g_armor_applied = want;
    g_switch(kSwArmorKey, want, kPlayerObj);
    stop_event(kEvArmorLoop, kPlayerObj);
    g_post(kEvArmorLoop, kPlayerObj, 0, nullptr, nullptr, 0, nullptr, 0);
    LOG("sounds: the armour sounds %s", want == kSwSkin ? "off (no armour piece shows)" : "as the game has them");
}

} // namespace om
