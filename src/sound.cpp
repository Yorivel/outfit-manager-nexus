// Sounds (2026-11-10): the played character's gear sounds follow his looks - a piece hidden by its look sounds as if it
// weren't worn, by the game's own rules - and an outfit change doesn't play the game's equip sounds. The lantern's
// sounds also tell when it burns (a hidden lantern shows while lit).
//
// The game's audio is Wwise, and its exe exports the Wwise API by name (AK::SoundEngine::PostEvent, SetSwitch, ...):
// those exports are hooked. Wwise ids are FNV-1 (32 bit) of the lowercase names; the names come from the game's
// soundbanksinfo (CDInventory/tools/wwnames.py). The game's rules, found with recording builds (game 2.04.08):
// - armorkey: a switch on the character, set from his CHEST piece's material alone (no chest: skin, cloth chest: cloth,
//   plate chest: platearmor; helm, gloves, boots and cloak change nothing - cycled on the wheel, 2026-11-10; the equip
//   slot table names slot 4 as the armour material's slot too). His armour loop (sfx_move_armor_lp, started by the
//   game after each change of gear) and his other gear sounds follow it. Hidden chest: kept at skin, the loop started
//   again so it takes it;
// - material_game: a switch on the character, the footsteps' shoe material, from his BOOTS alone (no boots: leather,
//   cloth boots: cloth, plate boots: metal). Hidden boots: kept at leather;
// - sfx_move_shield: with every step, the shield on the back. Hidden shield: not played;
// - sfx_move_plateshoe: a light metal tick with every step, asked for by the game's code with the cloth boots worn
//   (material 6: ~1.2 per step) and never barefoot or with plate boots (material 20), whatever the chest - counted
//   per worn set, 2026-11-10. So it is the boots': hidden boots, not played (the switches don't silence it);
// - sfx_collide_metal: physics - metal parts knocking together when he starts / stops. Collision sounds play on a pool
//   of emitters (objects 400001...), each placed where the collision is; the switch collidetype tells a footstep from
//   gear (defaultcollide). His own are within ~1.1 m of his object. Whose part it was isn't told, so: not played once
//   a metal (or unknown) piece is hidden and every armour piece still showing is known not to be metal;
// - an item's material: ItemInfo +0x4C, its MaterialMatchInfo key (read as _materialMatchInfo by the item table's
//   loader; e.g. 73 plate, 6 cloth, 20 plate boots). What a key sounds as is learned from the game itself, each time
//   it sets armorkey with a chest on or material_game with boots on;
// - two unnamed events on object 2: the equip sounds, one pair per change of gear. Not played during an outfit load;
// - the lantern: lit, sfx_move_lantern_fire_engage plays on the lantern's own object (an object of its own, near the
//   character); put out, sfx_lantern_off. The same from its wheel (On / Off) and in the world (2026-11-10). Put back
//   on the hip while lit, only sfx_lantern_in plays (on the character): it keeps burning. Its rattle only sounds while
//   it is held. Not known: whether it burns when a save is loaded (until it is lit or put out once).
// With Unequip Outfit (no looks) nothing is changed.
//
// Whose sounds: every character has a sound object of its own, numbered from 100001 in the order they came (Kliff
// 100001; Damiane 100002; Oongka 100004 in one session, 100005 in another). The played one's is the object at the
// played actor's place: the actor's component (+0x40, the node the model holder hangs from too) keeps its position
// as 3 floats at +0x168 - found by searching the actor's memory for the objects' positions, the same place for
// Damiane and Oongka. The objects' positions come from Wwise's SetPosition (doubles, metres).
#include "om.h"
#include "addrs.h"
#include "game.h"
#include "gear.h"
#include "items.h"
#include "load.h"
#include "looks.h"
#include "log.h"
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>
#include <string>

namespace om {

namespace {

constexpr U64 kUiObj = 2, kPoolFirst = 0x61A80, kPartyFirst = 100001;
constexpr U32 kPoolSize = 256, kPartySize = 32; // (the characters' objects: the first numbers)
constexpr U32 kNearSize = 1024; // (objects from 100001 whose positions are kept: the characters', their lanterns')
constexpr U32 kEvArmorLoop = 0x34F371D9, kEvShield = 0x74C41A56, kEvPlateShoe = 0x554A8706, kEvCollideMetal = 0x0D27F39D,
              kEvEquipA = 0x2361D7A5, kEvEquipB = 0xB074E90E;
constexpr U32 kEvLanternLit = 0x3A0056B4 /* sfx_move_lantern_fire_engage */, kEvLanternLit2 = 0xC4286E38 /* sfx_lantern_fire_engage */,
              kEvLanternOff = 0xCDE416CD /* sfx_lantern_off */, kEvLanternBlow = 0xE844DB41 /* sfx_lantern_fire_blow */;
constexpr U32 kSwArmorKey = 0x3921C2FF, kSwSkin = 0x310330D2, kSwPlate = 0x0C4744E0, kSwChainmail = 0x6DCCAB69;
constexpr U32 kSwShoe = 0xB25BD7C3 /* material_game */, kSwLeather = 0xD80FFE88, kSwMetal = 0x9375C65E;
constexpr U32 kSwCollideType = 0x29235BB9, kSwFootstep = 0x6F394B77;
constexpr U16 kShieldSlot = 13, kBootsSlot = 6;
constexpr U16 kArmourSlots[] = {3, kChestSlot, 5, kBootsSlot, 16}; // helm, chest, gloves, boots, cloak
constexpr U32 kOffItemMaterial = 0x4C;                             // (ItemInfo: its MaterialMatchInfo key, u16)
constexpr U32 kOffCompNode = 0x40, kOffNodePos = 0x168;            // (the actor's component -> node -> position, 3 floats)
constexpr double kOwnReach = 1.5; // (m: his own gear's collisions were 0.3 - 1.1 from his object)
constexpr double kLanternReach = 2.5; // (m: the played character's lantern - on the hip or in the hand - from his object)

using PostFn = U32(__fastcall*)(U32 id, U64 go, U32 flags, void* cb, void* cookie, U32 n, void* ext, U32 playing);
using SwitchFn = U32(__fastcall*)(U32 group, U32 state, U64 go);
using PosFn = U32(__fastcall*)(U64 go, const void* xform, U32 flags);
using ActionFn = U32(__fastcall*)(U32 event, U32 action, U64 go, int ms, int curve, U32 playing);
PostFn g_post = nullptr;
SwitchFn g_switch = nullptr;
PosFn g_setpos = nullptr;
ActionFn g_action = nullptr;

std::atomic<U64> g_player_obj{0};                 // (the played character's sound object; 0: not known)
double g_pool_pos[kPoolSize][3];                  // (written on the game's threads, read on them: plain values)
double g_party_pos[kNearSize][3];
bool g_pool_gear[kPoolSize];                      // (the emitter's last collidetype wasn't footstep)
// each character object: what the game last set, and what is set now
struct Party { std::atomic<U32> armor{0}, shoe{0}, armor_applied{0}, shoe_applied{0}; };
Party g_party[kPartySize];
// from the HUD frame (sound_tick), for the hooks
std::atomic<bool> g_chest_hidden{false}, g_boots_hidden{false}, g_shield_hidden{false}, g_metal_hidden{false};
// the played character's lantern: burning, and its own sound object (0: not seen yet)
std::atomic<bool> g_lantern_lit{false};
std::atomic<U64> g_lantern_obj{0};

// (UI thread) a material key -> the switch value the game gave a chest (armorkey) or boots (material_game) of it
struct Learned { U16 material; U32 sound; };
Learned g_learned[64];
int g_learned_n = 0;

U32 learned_sound(U16 material)
{
    for (int i = 0; i < g_learned_n; ++i)
        if (g_learned[i].material == material) return g_learned[i].sound;
    return 0;
}

void learn(U16 material, U32 sound, const char* piece)
{
    if (material == 0xFFFF || !sound || learned_sound(material) == sound) return;
    bool known = false;
    for (int i = 0; i < g_learned_n; ++i)
        if (g_learned[i].material == material) g_learned[i].sound = sound, known = true;
    if (!known && g_learned_n < int(std::size(g_learned))) g_learned[g_learned_n++] = {material, sound};
    LOG("sounds: %s of material %u sound as %08X", piece, material, sound);
}

// (every playing instance of an event on an object: the armour loop is started again by the game after each change
// of gear and by sound_tick - one at a time, or they pile up)
void stop_event(U32 event, U64 go)
{
    if (g_action) g_action(event, 0 /* stop */, go, 100, 4 /* linear */, 0);
}

// (the lantern's sounds, on the lantern's own object - lit / put out, from its wheel or in the world alike: the played
// character's when that object is at his)
void lantern_event(U32 id, U64 go)
{
    const bool lit = id == kEvLanternLit || id == kEvLanternLit2, out = id == kEvLanternOff || id == kEvLanternBlow;
    if (!lit && !out) return;
    const U64 player = g_player_obj.load();
    if (go != g_lantern_obj.load()) {
        if (!player || go - kPartyFirst >= kNearSize) return;
        const double* a = g_party_pos[go - kPartyFirst];
        const double* b = g_party_pos[player - kPartyFirst];
        double d2 = 0;
        for (int k = 0; k < 3; ++k) d2 += (a[k] - b[k]) * (a[k] - b[k]);
        if ((a[0] == 0 && a[1] == 0 && a[2] == 0) || d2 > kLanternReach * kLanternReach) return; // (another's)
        g_lantern_obj = go;
    }
    g_lantern_lit = lit;
}

bool muted(U32 id, U64 go)
{
    const U64 player = g_player_obj.load();
    if (player && go == player) {
        if (id == kEvShield) return g_shield_hidden.load();
        if (id == kEvPlateShoe) return g_boots_hidden.load();
    } else if (go == kUiObj) {
        if (id == kEvEquipA || id == kEvEquipB) return g_async.active || GetTickCount64() - g_async.started < 500;
    } else if (id == kEvCollideMetal && go - kPoolFirst < kPoolSize && player) {
        const U32 i = U32(go - kPoolFirst);
        if (!g_pool_gear[i] || !g_metal_hidden.load()) return false;
        const double* at = g_party_pos[player - kPartyFirst];
        double d2 = 0;
        for (int k = 0; k < 3; ++k) d2 += (g_pool_pos[i][k] - at[k]) * (g_pool_pos[i][k] - at[k]);
        return d2 < kOwnReach * kOwnReach;
    }
    return false;
}

U32 __fastcall post_hook(U32 id, U64 go, U32 flags, void* cb, void* cookie, U32 n, void* ext, U32 playing)
{
    lantern_event(id, go);
    if (muted(id, go)) return 0; // (AK_INVALID_PLAYING_ID: not played)
    if (id == kEvArmorLoop && go && go == g_player_obj.load()) stop_event(id, go); // (ours, or an older one of the game's)
    return g_post(id, go, flags, cb, cookie, n, ext, playing);
}

U32 __fastcall switch_hook(U32 group, U32 state, U64 go)
{
    if (group == kSwCollideType && go - kPoolFirst < kPoolSize) g_pool_gear[go - kPoolFirst] = state != kSwFootstep;
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

// (where each emitter is: AkWorldTransform = front, top (3 floats each), position (3 doubles, metres))
U32 __fastcall setpos_hook(U64 go, const void* xform, U32 flags)
{
    if (xform) {
        if (go - kPoolFirst < kPoolSize) std::memcpy(g_pool_pos[go - kPoolFirst], static_cast<const char*>(xform) + 24, 24);
        else if (go - kPartyFirst < kNearSize) std::memcpy(g_party_pos[go - kPartyFirst], static_cast<const char*>(xform) + 24, 24);
    }
    return g_setpos(go, xform, flags);
}

template <typename F>
bool hook_export(const char* mangled, void* hook, F* orig)
{
    void* t = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), mangled));
    return t && MH_CreateHook(t, hook, reinterpret_cast<void**>(orig)) == MH_OK && MH_EnableHook(t) == MH_OK;
}

// (UI thread) the worn piece's material key in a slot: 0xFFFF none / unreadable
U16 worn_material(U64 list, U16 slot)
{
    U64 e = list ? equipped_item(list, slot) : 0;
    U16 key = 0xFFFF, material = 0xFFFF;
    if (!e || !rd(e + 8, key) || key == 0xFFFF || !g_item_info) return 0xFFFF;
    U64 info = g_item_info(0, key);
    if (!info || !rd(info + kOffItemMaterial, material) || material > 4096) return 0xFFFF; // (keys are small numbers)
    return material;
}

// (UI thread) how far a character object is from the played actor: the largest difference of the coordinates, compared
// by size (the game's and Wwise's axes may be ordered / signed differently). 1e9: no position.
double distance_to_player(U32 i, const float* at)
{
    const double* p = g_party_pos[i];
    if (p[0] == 0 && p[1] == 0 && p[2] == 0) return 1e9;
    double x[3] = {std::fabs(double(at[0])), std::fabs(double(at[1])), std::fabs(double(at[2]))};
    double y[3] = {std::fabs(p[0]), std::fabs(p[1]), std::fabs(p[2])};
    std::sort(x, x + 3), std::sort(y, y + 3);
    return std::max({std::fabs(x[0] - y[0]), std::fabs(x[1] - y[1]), std::fabs(x[2] - y[2])});
}

// (UI thread) the played character's sound object: the one at the played actor's place. Kept while it stays close (two
// characters can stand side by side); 0 while none is.
U64 find_player_object(U64 current)
{
    U64 ctx[4] = {};
    U64 actor = acquire_player(ctx);
    U64 comp = actor ? rp(actor + kOffActorComp) : 0;
    U64 node = comp ? rp(comp + kOffCompNode) : 0;
    float at[3] = {};
    const bool ok = node && rd(node + kOffNodePos, at[0]) && rd(node + kOffNodePos + 4, at[1]) && rd(node + kOffNodePos + 8, at[2]);
    if (ctx[0]) release_player(ctx);
    if (!ok || (at[0] == 0 && at[1] == 0 && at[2] == 0)) return current;
    if (current && distance_to_player(U32(current - kPartyFirst), at) < 2.5) return current;
    U32 best = kPartySize;
    double best_d = 1.5;
    for (U32 i = 0; i < kPartySize; ++i)
        if (double d = distance_to_player(i, at); d < best_d) best_d = d, best = i;
    return best < kPartySize ? kPartyFirst + best : 0;
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

} // namespace

bool lantern_lit() { return g_lantern_lit.load(); }



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

// The HUD frame: whose sounds, what is worn and hidden now (for the hooks), what the game's switches say about the
// chest's and the boots' materials, and the switches again when a look changed.
void sound_tick()
{
    if (!g_switch || !g_post) return;
    const U64 now = GetTickCount64();
    const U16 ch = current_character();

    { // the lantern: lit or put out since - the looks again (a hidden lantern shows while it burns); another character
      // played: theirs isn't known to burn
        static bool was_lit = false;
        static U16 lantern_ch = kNoCharacter;
        if (ch != lantern_ch) lantern_ch = ch, g_lantern_lit = false, g_lantern_obj = 0;
        if (const bool lit = g_lantern_lit.load(); lit != was_lit) {
            was_lit = lit;
            LOG("sounds: the lantern %s", lit ? "burns" : "is out");
            look_refresh();
        }
    }
    { // whose object: the one at the played actor's place (the one before gets the game's switches back)
        static U16 was_ch = kNoCharacter;
        const U64 before = g_player_obj.load();
        const U64 obj = find_player_object(ch == was_ch ? before : 0);
        if (obj != before || ch != was_ch) {
            if (obj != before) {
                g_chest_hidden = false, g_boots_hidden = false, g_shield_hidden = false, g_metal_hidden = false;
                g_player_obj = obj;
                give_back(before);
            }
            if (obj != before || ch != was_ch)
                LOG("sounds: character %u played: sound object %llu%s", ch, obj, obj ? "" : " (not found yet: its sounds are left alone)");
            was_ch = ch;
        }
    }
    const U64 obj = g_player_obj.load();
    if (!obj) return;
    const U64 inv = player_inventory();
    const U64 list = inv ? rp(inv + kOffInvEquipList) : 0;
    const U32 hidden = look_hidden_slots();
    U16 material[std::size(kArmourSlots)];
    for (size_t i = 0; i < std::size(kArmourSlots); ++i) material[i] = worn_material(list, kArmourSlots[i]);
    const bool busy = g_async.active || look_changing() || fx_transition_busy(); // (an outfit change: looks and gear in flux)
    Party& p = g_party[obj - kPartyFirst];
    const U32 game_armor = p.armor.load(), game_shoe = p.shoe.load();

    { // (what is worn and what the game set, whenever either changes: the record of the game's rule)
        static U16 was[std::size(kArmourSlots)];
        static U32 was_armor = 0, was_shoe = 0;
        if (std::memcmp(was, material, sizeof was) != 0 || was_armor != game_armor || was_shoe != game_shoe) {
            std::memcpy(was, material, sizeof was), was_armor = game_armor, was_shoe = game_shoe;
            LOG("sounds: materials helm %d, chest %d, gloves %d, boots %d, cloak %d; the game's armorkey %08X, shoe material %08X",
                int(std::int16_t(material[0])), int(std::int16_t(material[1])), int(std::int16_t(material[2])),
                int(std::int16_t(material[3])), int(std::int16_t(material[4])), was_armor, was_shoe);
        }
    }
    // the chest's material -> the armorkey the game gave it, the boots' -> the shoe material (settled for 300 ms: the
    // game sets them as the gear changes; the object too - a character just switched to)
    static U32 seen_armor = 0, seen_shoe = 0;
    static U16 seen_chest = 0xFFFF, seen_boots = 0xFFFF;
    static U64 seen_at = 0, seen_obj = 0;
    if (game_armor != seen_armor || game_shoe != seen_shoe || material[1] != seen_chest || material[3] != seen_boots || obj != seen_obj)
        seen_armor = game_armor, seen_shoe = game_shoe, seen_chest = material[1], seen_boots = material[3], seen_obj = obj, seen_at = now;
    if (!busy && now - seen_at > 300) learn(material[1], game_armor, "chests"), learn(material[3], game_shoe, "boots");

    // hidden pieces
    auto worn_hidden = [&](size_t i) { return material[i] != 0xFFFF && ((hidden >> kArmourSlots[i]) & 1); };
    g_chest_hidden = worn_hidden(1);
    g_boots_hidden = worn_hidden(3);
    g_shield_hidden = (hidden & ((1u << kShieldSlot) | (1u << kBackShieldSlot))) != 0;
    // metal knocks: a metal (or unknown) piece is hidden, and every piece that shows is known not to be metal
    bool hidden_metal = false, shown_metal = false;
    for (size_t i = 0; i < std::size(kArmourSlots); ++i) {
        if (material[i] == 0xFFFF) continue;
        const U32 snd = learned_sound(material[i]);
        const bool maybe_metal = !snd || snd == kSwPlate || snd == kSwChainmail || snd == kSwMetal; // (not learned yet: maybe)
        if (!maybe_metal) continue;
        if ((hidden >> kArmourSlots[i]) & 1) hidden_metal = true;
        else shown_metal = true;
    }
    g_metal_hidden = hidden_metal && !shown_metal;

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
