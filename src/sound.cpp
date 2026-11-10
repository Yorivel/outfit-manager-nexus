// Sounds (2026-11-11): the played character's gear sounds follow his looks - a piece hidden by its look sounds as if it
// weren't worn, by the game's own rules - and an outfit change doesn't play the game's equip sounds. Also the lantern's
// lit state (a hidden lantern shows while it burns).
//
// The game's audio is Wwise, and its exe exports the Wwise API by name (AK::SoundEngine::PostEvent, SetSwitch, ...):
// those exports are hooked. Wwise ids are FNV-1 (32 bit) of the lowercase names; the names come from the game's
// soundbanksinfo (CDInventory/tools/wwnames.py). The game's rules, found with recording builds (game 2.04.08):
// - armorkey: a switch on the character's sound object, set from his CHEST piece alone (no chest: skin; cloth: cloth;
//   plate: platearmor; helm, gloves, boots and cloak change nothing - cycled on the wheel, 2026-11-10; the equip slot
//   table names slot 4 as the armour material's slot too). His armour loop (sfx_move_armor_lp, started by the game
//   after each change of gear) follows it. Hidden chest: kept at skin, the loop started again so it takes it;
// - material_game: the footsteps' shoe material, from his BOOTS alone (no boots: leather). Hidden boots: kept at
//   leather;
// - sfx_move_plateshoe: a light metal tick with every step, asked for with the cloth boots worn and never barefoot or
//   with plate boots (counted per worn set, 2026-11-10): the boots'. Hidden boots: not played (the switches don't
//   silence it);
// - sfx_move_shield: with every step, the shield on the back. Hidden shield: not played;
// - sfx_collide_metal: physics - metal knocking when he starts / stops. Played by the game's collision-sound function
//   (kRvaCollideSound) for a contact record whose +0x60 is the colliding body; the played character's own clinks all
//   come from ONE body (his component +0x298), and only with a metal chest (none with a cloth chest and a plate cloak,
//   plate gloves / boots add none - 2026-11-11): the chest's, like armorkey. Hidden chest: his body's clinks not
//   played; everyone else's (an NPC he bumps into) play;
// - two unnamed events on object 2: the equip sounds, one pair per change of gear. Not played during an outfit load.
// The lantern only sounds while held (shown then): nothing to do. With Unequip Outfit (no looks) nothing is changed.
//
// Whose sounds: every character has a sound object of its own, numbered from 100001 as objects register (the numbers
// differ per session and per save load). The played one's is read the way the game finds it when it sets the
// character's armorkey: actor -> component -> its effects -> its sound component -> sound handle -> number
// (player_object; found by following the game's call chain from the switch back to the gear change, 2026-11-11).
// The offsets are checked by the resolver against the game functions that use them (addrs.cpp, strict: unsure = off);
// the clink's two offsets have no witness, but wrong they only make no clink match his body (nothing muted).
#include "om.h"
#include "addrs.h"
#include "gear.h"
#include "load.h"
#include "looks.h"
#include "log.h"
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <string>

namespace om {

namespace {

constexpr U64 kUiObj = 2, kPartyFirst = 100001;
constexpr U32 kPartySize = 1u << 14; // (objects from 100001: the characters' - numbered on as they spawn, across loads)
constexpr U32 kEvArmorLoop = 0x34F371D9, kEvShield = 0x74C41A56, kEvPlateShoe = 0x554A8706, kEvCollideMetal = 0x0D27F39D,
              kEvEquipA = 0x2361D7A5, kEvEquipB = 0xB074E90E;
constexpr U32 kSwArmorKey = 0x3921C2FF, kSwSkin = 0x310330D2, kSwShoe = 0xB25BD7C3 /* material_game */, kSwLeather = 0xD80FFE88;
constexpr U16 kShieldSlot = 13, kBootsSlot = 6;

using PostFn = U32(__fastcall*)(U32 id, U64 go, U32 flags, void* cb, void* cookie, U32 n, void* ext, U32 playing);
using SwitchFn = U32(__fastcall*)(U32 group, U32 state, U64 go);
using ActionFn = U32(__fastcall*)(U32 event, U32 action, U64 go, int ms, int curve, U32 playing);
using CollideFn = U64(__fastcall*)(U64, U64, U64, U64, U64, U64, U64, U64);
PostFn g_post = nullptr;
SwitchFn g_switch = nullptr;
ActionFn g_action = nullptr;
CollideFn g_collide_orig = nullptr;

std::atomic<U64> g_player_obj{0};  // (the played character's sound object; 0: not known)
std::atomic<U64> g_player_body{0}; // (his physics body: his own collisions)
// each character object: what the game last set, and what is set now
struct Party { std::atomic<U32> armor{0}, shoe{0}, armor_applied{0}, shoe_applied{0}; };
Party g_party[kPartySize];
// from the HUD frame (sound_tick), for the hooks
std::atomic<bool> g_chest_hidden{false}, g_boots_hidden{false}, g_shield_hidden{false};
std::atomic<bool> g_lantern_lit{false}; // (the played character's lantern burns)
thread_local U64 t_contact = 0;         // (the contact being sounded by the collision-sound function, on this thread)

// (every playing instance of an event on an object: the armour loop is started again by the game after each change
// of gear and by sound_tick - one at a time, or they pile up)
void stop_event(U32 event, U64 go)
{
    if (g_action) g_action(event, 0 /* stop */, go, 100, 4 /* linear */, 0);
}

bool muted(U32 id, U64 go)
{
    const U64 player = g_player_obj.load();
    if (player && go == player) {
        if (id == kEvShield) return g_shield_hidden.load();
        if (id == kEvPlateShoe) return g_boots_hidden.load();
    } else if (go == kUiObj) {
        if (id == kEvEquipA || id == kEvEquipB) return g_async.active || GetTickCount64() - g_async.started < 500;
    }
    if (id == kEvCollideMetal && t_contact && g_chest_hidden.load()) { // (his own body's metal knocking: the chest's)
        U64 body = 0;
        const U64 mine = g_player_body.load();
        return mine && kOffContactBody && rd(t_contact + kOffContactBody, body) && body == mine;
    }
    return false;
}

U32 __fastcall post_hook(U32 id, U64 go, U32 flags, void* cb, void* cookie, U32 n, void* ext, U32 playing)
{
    if (muted(id, go)) return 0; // (AK_INVALID_PLAYING_ID: not played)
    if (id == kEvArmorLoop && go && go == g_player_obj.load()) stop_event(id, go); // (ours, or an older one of the game's)
    return g_post(id, go, flags, cb, cookie, n, ext, playing);
}

U32 __fastcall switch_hook(U32 group, U32 state, U64 go)
{
    if (go - kPartyFirst < kPartySize && (group == kSwArmorKey || group == kSwShoe)) {
        Party& p = g_party[go - kPartyFirst];
        const bool played = go == g_player_obj.load();
        if (group == kSwArmorKey) {
            p.armor = state;
            if (played && g_chest_hidden.load()) state = kSwSkin;
            p.armor_applied = state;
        } else {
            p.shoe = state;
            if (played && g_boots_hidden.load()) state = kSwLeather;
            p.shoe_applied = state;
        }
    }
    return g_switch(group, state, go);
}

// (the game's collision-sound function: manager, contact record, ?, sound info, and stack arguments - the contact is kept
// for the posts it makes on this thread)
U64 __fastcall collide_hook(U64 a1, U64 a2, U64 a3, U64 a4, U64 a5, U64 a6, U64 a7, U64 a8)
{
    const U64 was = t_contact;
    t_contact = a2;
    const U64 r = g_collide_orig(a1, a2, a3, a4, a5, a6, a7, a8);
    t_contact = was;
    return r;
}

template <typename F>
bool hook_export(const char* mangled, void* hook, F* orig)
{
    void* t = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), mangled));
    return t && MH_CreateHook(t, hook, reinterpret_cast<void**>(orig)) == MH_OK && MH_EnableHook(t) == MH_OK;
}

// (UI thread) the played character's sound object, as the game finds it when it sets the character's armorkey (its
// gear-change code, kRvaGearSounds from kRvaGearSoundsFrom): actor +0x68 -> +0x60 -> +0x18 (the character's sound
// component) -> +0x70 (its sound handle) -> +0x18 the object's number. 0: not there. And his physics body.
U64 player_object(U64* body)
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 comp = actor ? rp(actor + kOffActorComp) : 0;
    const bool known = kOffCompEffects && kOffEffectsSound && kOffSoundHandle && kOffHandleObject; // (the resolver: 0 = unsure)
    U64 fx = comp && known ? rp(comp + kOffCompEffects) : 0;
    U64 snd = fx ? rp(fx + kOffEffectsSound) : 0;
    U64 handle = snd ? rp(snd + kOffSoundHandle) : 0;
    U32 id = 0;
    const bool ok = handle && rd(handle + kOffHandleObject, id);
    *body = comp && kOffCompBody ? rp(comp + kOffCompBody) : 0;
    if (ctx[0]) release_player(ctx);
    return ok && id - kPartyFirst < kPartySize ? id : 0;
}

// (UI thread) the switches of a character's object as the game has them: when it stops being the played one
void give_back(U64 obj)
{
    if (!obj || !g_switch || !g_post) return;
    Party& p = g_party[obj - kPartyFirst];
    if (U32 a = p.armor.load(); a && p.armor_applied.exchange(a) != a) {
        g_switch(kSwArmorKey, a, obj);
        stop_event(kEvArmorLoop, obj);
        g_post(kEvArmorLoop, obj, 0, nullptr, nullptr, 0, nullptr, 0);
    }
    if (U32 s = p.shoe.load(); s && p.shoe_applied.exchange(s) != s) g_switch(kSwShoe, s, obj);
}

bool worn(U64 list, U16 slot)
{
    U64 e = list ? equipped_item(list, slot) : 0;
    U16 key = 0xFFFF;
    return e && rd(e + 8, key) && key != 0xFFFF;
}

} // namespace

bool lantern_lit() { return g_lantern_lit.load(); }

void sound_install()
{
    const bool post = hook_export(
        "?PostEvent@SoundEngine@AK@@YAII_KIP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXIPEAUAkExternalSourceInfo@@I@Z",
        reinterpret_cast<void*>(&post_hook), &g_post);
    const bool sw = hook_export("?SetSwitch@SoundEngine@AK@@YA?AW4AKRESULT@@II_K@Z", reinterpret_cast<void*>(&switch_hook), &g_switch);
    g_action = reinterpret_cast<ActionFn>(GetProcAddress(
        GetModuleHandleW(nullptr),
        "?ExecuteActionOnEvent@SoundEngine@AK@@YA?AW4AKRESULT@@IW4AkActionOnEventType@12@_KHW4AkCurveInterpolation@@I@Z"));
    if (!post) g_post = nullptr;
    if (!sw) g_switch = nullptr;
    bool collide = false;
    if (have({kRvaCollideSound})) {
        void* t = reinterpret_cast<void*>(g_base + kRvaCollideSound);
        collide = MH_CreateHook(t, reinterpret_cast<void*>(&collide_hook), reinterpret_cast<void**>(&g_collide_orig)) == MH_OK &&
                  MH_EnableHook(t) == MH_OK;
    }
    LOG("sounds: play %s, switches %s, stop %s, collisions %s", post ? "watched" : "NOT found", sw ? "watched" : "NOT found",
        g_action ? "found" : "NOT found", collide ? "watched" : "NOT found (a hidden chest's metal knocking still sounds)");
}

// The HUD frame: whose sounds, what is worn and hidden now (for the hooks), the lantern, and the switches again when a
// look changed.
void sound_tick()
{
    if (!g_switch || !g_post) return;
    const U64 now = GetTickCount64();
    const U16 ch = current_character();

    { // the lantern: burning, read from the played character as the game reads it (kRvaLanternCheck: component +0x50
      // -> +0x18C, a counter, lit while > 0); changed: the looks again (a hidden lantern shows while it burns)
        static bool was_lit = false;
        U64 ctx[4] = {};
        U64 actor = acquire_player(ctx);
        U64 comp = actor ? rp(actor + kOffActorComp) : 0;
        U64 holder = comp && kOffCompLantern ? rp(comp + kOffCompLantern) : 0;
        U32 v = 0;
        const bool lit = kOffLanternLit && holder && rd(holder + kOffLanternLit, v) && v > 0 && v < 64;
        if (ctx[0]) release_player(ctx);
        g_lantern_lit = lit;
        if (lit != was_lit) {
            was_lit = lit;
            LOG("sounds: the lantern %s", lit ? "burns" : "is out");
            look_refresh();
        }
    }
    U64 body = 0;
    { // whose object (the one before gets the game's switches back)
        static U16 was_ch = kNoCharacter;
        const U64 before = g_player_obj.load();
        const U64 obj = player_object(&body);
        g_player_body = body;
        if (obj != before || ch != was_ch) {
            if (obj != before) {
                g_chest_hidden = false, g_boots_hidden = false, g_shield_hidden = false;
                g_player_obj = obj;
                give_back(before);
            }
            LOG("sounds: character %u played: sound object %llu%s", ch, obj, obj ? "" : " (not found: its sounds are left alone)");
            was_ch = ch;
        }
    }
    const U64 obj = g_player_obj.load();
    if (!obj) return;
    const U64 inv = player_inventory();
    const U64 list = inv ? rp(inv + kOffInvEquipList) : 0;
    const U32 hidden = look_hidden_slots();
    const bool busy = g_async.active || look_changing() || fx_transition_busy(); // (an outfit change: looks and gear in flux)
    Party& p = g_party[obj - kPartyFirst];
    const U32 game_armor = p.armor.load(), game_shoe = p.shoe.load();

    // hidden pieces (worn, and hidden by their look)
    g_chest_hidden = worn(list, kChestSlot) && ((hidden >> kChestSlot) & 1);
    g_boots_hidden = worn(list, kBootsSlot) && ((hidden >> kBootsSlot) & 1);
    g_shield_hidden = (hidden & ((1u << kShieldSlot) | (1u << kBackShieldSlot))) != 0;

    // a look changed: the switches again (settled for 300 ms, and no outfit change running)
    const U32 want_armor = g_chest_hidden.load() ? kSwSkin : game_armor;
    const U32 want_shoe = g_boots_hidden.load() ? kSwLeather : game_shoe;
    static U32 pending_armor = 0, pending_shoe = 0;
    static U64 since = 0;
    if (want_armor != pending_armor || want_shoe != pending_shoe) pending_armor = want_armor, pending_shoe = want_shoe, since = now;
    if (busy || now - since < 300) return;
    if (game_armor && want_armor != p.armor_applied.load()) {
        p.armor_applied = want_armor;
        g_switch(kSwArmorKey, want_armor, obj);
        stop_event(kEvArmorLoop, obj);
        g_post(kEvArmorLoop, obj, 0, nullptr, nullptr, 0, nullptr, 0);
        LOG("sounds: the armour sounds %s", want_armor == kSwSkin && game_armor != kSwSkin ? "as with no chest (it is hidden)" : "as the game has them");
    }
    if (game_shoe && want_shoe != p.shoe_applied.load()) {
        p.shoe_applied = want_shoe;
        g_switch(kSwShoe, want_shoe, obj);
        LOG("sounds: the footsteps %s", want_shoe == kSwLeather && game_shoe != kSwLeather ? "as with no boots (they are hidden)" : "as the game has them");
    }
}

} // namespace om
