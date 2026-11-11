// Looks: how each piece shows (the visibility engine).
#include "om.h"
#include "looks.h"
#include "ui.h"
#include "addrs.h"
#include "gear.h"
#include "store.h"
#include "looks_view.h"
#include "load.h"
#include "stash.h"
#include "prompts.h"
#include "wheel_draw.h"
#include "wheel_hooks.h"

namespace om {

int sleep_looks(); // (sleep.cpp)

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
U64 player_inventory(); // (below)
U64 g_sheathe_refresh_at = 0;
U64 g_case_wait_since = 0; // (a drawn hidden weapon's scabbard waits for its hologram: since then; 0 = none waiting)
struct FxTargets {
    std::vector<U32> names;      // (the weapon's part name ids - its glow)
    int case_handle = 0;         // (its scabbard's model part handle - the hologram)
    std::vector<U32> holo;       // (the scabbard and the weapon's own parts - a bow's quiver and arrows: the hologram)
    std::vector<U32> case_names; // (its scabbard's part name ids: kept hidden in the draw check until its hologram is on)
};
std::map<std::pair<U64, U16>, U64> g_draw_wait;
// Weapons shown / hidden by their look changing in play (combat starts or ends, a look switched): their glow, the
// hologram on their scabbard - appearing: held hidden until its hologram has gone out once (at most 300 ms); going: kept
// until the glow's hold ends (1.9 s). The player only, not mid outfit change. (g_appear_mu)
std::map<std::pair<U64, U16>, bool> g_fx_was_shown;
constexpr U16 kBowSlot = 2; // (equipment-2: the bow - its part renders like armour on the back: the hologram on it too)
std::map<std::pair<U64, U16>, U64> g_look_appear_at, g_look_go_at; // (a drawn hidden weapon: since when its scabbard waits; g_appear_mu)
std::map<std::pair<U64, U16>, FxTargets> g_fx_targets; // (the player's hidden weapons, from the last sync; g_appear_mu) // (a sheathed hidden weapon stays until its glow's peak: the looks again then)
std::map<std::pair<U64, U16>, bool> g_fx_was_drawn; // (the player's hidden weapons: drawn at the last sync)
std::map<std::pair<U64, U16>, U64> g_sheathe_hide_at; // (... and when a sheathed one may go)
U64 g_weapon_hide_after = 0; // (the transition: weapons about to be hidden stay until their glow's peak ends - GetTickCount64)
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
                if (o.underwear_look != kLookDefault) m[c.character].push_back({kUnderwearSlot, 0xFFFF, 0, o.underwear_look});
            }
    std::lock_guard<std::mutex> lock(g_looks_mu);
    g_looks = std::move(m);
}

// The character model: the path the headgear switch takes from the inventory's actor.
U64 look_model(U64 actor)
{
    if (!kOffCompModel || !kOffModelScenes) return 0; // (the resolver unsure: no model rather than a wrong one)
    return rp(rp(rp(rp(rp(rp(actor + kOffActorComp) + kOffCompModel) + kOffModelScenes)) + 8) + kOffModelRoot);
}

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

// A model's part by name: its handle, or 0 (the underwear: no slot leads to it). Cached per model state.
int named_handle(U64 model, const char* want)
{
    static std::map<std::pair<U64, std::string>, std::pair<U64, int>> cache; // (model, name) -> (part table, handle)
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    auto& c = cache[{model, want}];
    if (c.first == tab && c.second) return c.second;
    c = {tab, 0};
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (U64 part = rp(tab + i * 16ull + 8); part && part_name(r32(part + 0x48)) == want) {
            c.second = static_cast<int>(r32(tab + i * 16ull));
            break;
        }
    return c.second;
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

std::atomic<U32> g_player_hidden_slots{0};
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
    for (auto it = g_fx_targets.begin(); it != g_fx_targets.end();) it = it->first.first == inv ? g_fx_targets.erase(it) : std::next(it);
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
    // (asleep, sleep.cpp: any outfit but a sleepwear one steps back as in flight; a sleepwear outfit's looks win over the
    // game's in-bed hiding - its game_hides yield is skipped)
    const int sleep = inv == player_inventory() ? sleep_looks() : 0;
    if (sleep == 1) flying = true;
    // (the weapon slots come from the Weapons wheel's record: read here too, the load screen comes before any wheel)
    if (!g_weapon_slot_count && g_qsi_get) collect_weapon_slots();
    U64 list = rp(inv + kOffInvEquipList);
    std::map<U16, int> now;
    std::set<int> wanted, forced;
    std::string state; // per piece: what it is now and why (the log line)
    std::set<U16> deferred; // (weapon slots whose hiding waits for the transition's glow)
    U32 hidden_mask = 0;    // (the slots hidden by their look: sound.cpp mutes their movement sounds)
    const bool hide_wait = GetTickCount64() < g_weapon_hide_after;
    const std::set<int>& was_forced = g_look_forced[inv];
    for (const LookPiece& p : pieces) {
        state += " equipment-" + std::to_string(p.slot);
        const bool underwear = p.slot == kUnderwearSlot; // (no item: the model's Underwear part, by name)
        U64 e = list && !underwear ? equipped_item(list, p.slot) : 0;
        U64 uid = 0;
        U16 key = 0xFFFF;
        if (!underwear) {
            if (!e || !rd(e, uid) || !rd(e + 8, key)) { state += " empty"; continue; }
            if (key == 0xFFFF) { state += " empty"; continue; } // (a look belongs to the slot: whatever is there)
        }
        int h = underwear ? named_handle(model, "Underwear") : look_handle(inv, p.slot);
        if (!h) { state += " no model part"; continue; }
        wanted.insert(h);
        bool shown = p.look == kLookShown || (p.look == kLookCombat && in_battle) || (p.look == kLookPeace && !in_battle);
        bool case_shown = shown; // (a weapon's case: with its look, and while the weapon is drawn)
        bool drawn = false;
        int ch_case = 0;
        std::vector<int> extras;
        if (is_weapon_slot(p.slot) || is_display_slot(p.slot)) { // (drawn: always shown; its parts are watched)
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
            // (a lit lantern shows, on the hip too: hidden only while it is out - sound.cpp)
            if (p.slot == kLanternSlot && inv == player_inventory() && lantern_lit()) drawn = true;
            if (g_bag_in_use == std::make_pair(inv, p.slot)) drawn = true;
            if (auto it = g_flight_tool.find({inv, h}); it != g_flight_tool.end() && it->second) drawn = true;
            case_shown = shown || drawn;
            // a hidden weapon drawn or sheathed (the player, not mid outfit change): the sneak peek glow on its parts -
            // drawn: it appears glowing; sheathed: the glow flares up and it goes at the peak (kept shown till then)
            if (shown) g_fx_targets.erase({inv, p.slot}); // (shown by its look: not a hidden weapon any more)
            if (!shown && !hold && inv == player_inventory()) {
                const std::pair<U64, U16> key{inv, p.slot};
                const U64 now_ms = GetTickCount64();
                auto was = g_fx_was_drawn.find(key);
                { // (what the draw hook needs: the weapon's part names and its scabbard - cached for the hook)
                    FxTargets& tg = g_fx_targets[key];
                    tg.names.clear();
                    for (const auto& [k2, own] : g_weapon_parts)
                        if (own == key) tg.names.push_back(k2.second);
                    tg.case_handle = ch_case;
                    tg.case_names.clear();
                    tg.holo.clear();
                    std::vector<int> hh = extras; // (the scabbard and the extras: they appear with it)
                    if (ch_case) hh.push_back(ch_case);
                    if (p.slot == kBowSlot) hh.push_back(h); // (the bow itself: drawn like armour on the back)
                    for (int x : hh) {
                        tg.holo.push_back(static_cast<U32>(x));
                        for (U64 q : look_parts(model, x)) {
                            U32 nm = r32(q + 0x48);
                            tg.case_names.push_back(nm);
                            if (std::find(tg.names.begin(), tg.names.end(), nm) == tg.names.end()) tg.names.push_back(nm);
                        }
                    }
                }
                if (was != g_fx_was_drawn.end() && was->second != drawn && !drawn && transition_on()) { // (sheathed; drawn: in the hook)
                    std::vector<U32> names;
                    for (const auto& [k2, own] : g_weapon_parts)
                        if (own == key) names.push_back(k2.second);
                    // (its case - the scabbard the sword goes into - and its own parts, the quiver and arrows: they
                    // appear and go with it, so they glow too)
                    std::vector<int> more = extras;
                    if (ch_case) more.push_back(ch_case);
                    LOG("appearance: equipment-%u glow: case handle %d, %zu extra(s), %zu part(s) of its own", p.slot, ch_case,
                        extras.size(), names.size());
                    for (int hh : more)
                        for (U64 q : look_parts(model, hh))
                            if (U32 nm = r32(q + 0x48); std::find(names.begin(), names.end(), nm) == names.end())
                                names.push_back(nm);
                    fx_weapon_glow(names, !drawn);
                    { // (the scabbard and the extras - a bow's quiver and arrows: drawn like armour - the hologram)
                        std::vector<U32> hl;
                        for (int x : extras) hl.push_back(static_cast<U32>(x));
                        if (ch_case) hl.push_back(static_cast<U32>(ch_case));
                        if (p.slot == kBowSlot) hl.push_back(static_cast<U32>(h)); // (the bow: drawn like armour on the back)
                        if (!hl.empty()) fx_scabbard_holo(hl, !drawn);
                    }
                    if (!drawn) {
                        g_sheathe_hide_at[key] = now_ms + 1900; // (the glow's hold ends: in 0.4 + hold 1.5 s)
                        g_sheathe_refresh_at = std::max(g_sheathe_refresh_at, now_ms + 1910);
                    } else {
                        g_sheathe_hide_at.erase(key);
                    }
                }
                g_fx_was_drawn[key] = drawn;
                if (auto h2 = g_sheathe_hide_at.find(key); !drawn && h2 != g_sheathe_hide_at.end()) {
                    if (now_ms < h2->second) drawn = true, case_shown = true, state += " (sheathed: hidden at its glow's peak)";
                    else g_sheathe_hide_at.erase(h2);
                }
            }
        }
        if (!flying && inv == player_inventory()) { // (its look changed in play: the hologram on it)
            const std::pair<U64, U16> key{inv, p.slot};
            const U64 now_ms = GetTickCount64();
            // (the look's own state only: draws and sheathes have their effect of their own - counting them here gave
            // a sheathed bow a second hold after the first, so it stayed solid past its hologram)
            const bool vis = shown;
            // (an outfit change and its transition change looks too: only noted then, no effect of its own)
            const bool quiet = hold || hide_wait || fx_transition_busy() || !transition_on();
            auto was = g_fx_was_shown.find(key);
            const bool weapon = is_weapon_slot(p.slot) || is_display_slot(p.slot); // (weapons only - user, 2026-11-07)
            if (weapon && !quiet && was != g_fx_was_shown.end() && was->second != vis) {
                std::vector<U32> holo;
                std::vector<U32> names;
                for (const auto& [k2, own] : g_weapon_parts)
                    if (own == key) names.push_back(k2.second);
                std::vector<int> hh = extras; // (the scabbard and the extras - a bow's quiver and arrows)
                if (ch_case) hh.push_back(ch_case);
                if (p.slot == kBowSlot) hh.push_back(h); // (the bow itself: drawn like armour on the back)
                for (int x : hh) {
                    holo.push_back(static_cast<U32>(x));
                    for (U64 q : look_parts(model, x))
                        if (U32 nm = r32(q + 0x48); std::find(names.begin(), names.end(), nm) == names.end()) names.push_back(nm);
                }
                fx_weapon_glow(names, !vis);
                if (!holo.empty()) fx_scabbard_holo(holo, !vis);
                if (vis) g_look_appear_at[key] = now_ms, g_look_go_at.erase(key), g_case_wait_since = now_ms;
                else g_look_go_at[key] = now_ms + 1900, g_look_appear_at.erase(key),
                     g_sheathe_refresh_at = std::max(g_sheathe_refresh_at, now_ms + 1910);
                LOG("appearance: equipment-%u %s by its look: the hologram on it", p.slot, vis ? "appears" : "goes");
            }
            g_fx_was_shown[key] = vis;
            if (auto ap = g_look_appear_at.find(key); ap != g_look_appear_at.end()) { // (appearing: hidden till its hologram)
                if (!vis || fx_scab_applied_after(ap->second) || now_ms - ap->second > 300) g_look_appear_at.erase(ap);
                else shown = false, drawn = false, case_shown = false, state += " (appears once its hologram is on)";
            }
            if (auto go = g_look_go_at.find(key); go != g_look_go_at.end()) { // (going: kept till the glow's hold ends)
                if (vis || now_ms >= go->second) g_look_go_at.erase(go);
                else shown = true, case_shown = true, state += " (goes at its glow's peak)";
            }
        }
        // (a weapon going from shown to hidden during the transition: left as it is until the glow's peak ends -
        // hidden at once, it cut the glow short)
        if (hide_wait && !(shown || drawn) && (is_weapon_slot(p.slot) || is_display_slot(p.slot))) {
            auto ai = applied.find(p.slot);
            bool hidden_now = ai != applied.end() && ai->second == h && !was_forced.count(h);
            if (!hidden_now) {
                deferred.insert(p.slot);
                state += " hidden after the glow";
                continue;
            }
        }
        int by = flying || sleep == 2 ? -1 : game_hides(model, h, look_part(model, h));
        if ((shown || drawn) && (flying || by >= 0)) { // (the game's mechanics win; released below)
            state += flying ? " left to the game (flight)" : " left to the game (its layer " + std::to_string(by) + " hides it)";
            continue;
        }
        // (mid outfit change: only hiding; what is shown waits for the model swap, kept as it was meanwhile)
        if (!(hold && (shown || drawn)) && look_call(model, h, shown || drawn ? 1 : 0)) now[p.slot] = h;
        if (shown || drawn) forced.insert(h);
        state += shown || drawn ? hold ? " shown once on" : " shown" : " hidden";
        if (!(shown || drawn) && p.slot < 32) hidden_mask |= 1u << p.slot;
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
            if (hold || deferred.count(static_cast<U16>(slot & 0xFF))) now.emplace(slot, h);
            else look_call(model, h, 2);
        }
    applied = std::move(now);
    if (inv == player_inventory()) g_player_hidden_slots = hidden_mask; // (sound.cpp: hidden gear's sounds)
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
U32 look_hidden_slots() { return g_player_hidden_slots.load(); }
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
EquipVisCheckFn g_equip_vis_check_orig = nullptr;
float __fastcall equip_vis_check_hook(U64 vc, const U32* part, std::uint8_t* rec, std::uint8_t drawn)
{
    float r = g_equip_vis_check_orig(vc, part, rec, drawn);
    U32 name = 0;
    if (!part || !rd(reinterpret_cast<U64>(part), name)) return r;
    std::lock_guard<std::mutex> lock(g_appear_mu);
    if (g_weapon_parts.empty()) return r;
    // a drawn hidden weapon's scabbard: hidden here until its hologram has gone out once (at most 300 ms) - the game
    // shows it on the draw's frame, before the hologram: it flashed in plain
    for (auto it = g_draw_wait.begin(); it != g_draw_wait.end();) {
        auto tg = g_fx_targets.find(it->first);
        const bool over = tg == g_fx_targets.end() || fx_scab_applied_after(it->second) || GetTickCount64() - it->second > 300;
        if (over) { it = g_draw_wait.erase(it); continue; }
        if (std::find(tg->second.case_names.begin(), tg->second.case_names.end(), name) != tg->second.case_names.end()) {
            static int logged = 0;
            if (logged < 6) ++logged, LOG("appearance: scabbard part %s held hidden in the draw check (was %.2f)",
                                          part_name(name).c_str(), r);
            return 0.0f;
        }
        ++it;
    }
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
    if (now && transition_on()) // (a hidden weapon drawn: its effects start on this frame - from the looks a frame
                                // later, its scabbard showed plain first)
        if (auto tg = g_fx_targets.find(owner); tg != g_fx_targets.end()) {
            fx_weapon_glow(tg->second.names, false);
            if (!tg->second.holo.empty()) {
                fx_scabbard_holo(tg->second.holo, false);
                g_draw_wait[owner] = GetTickCount64();
            }
        }
    U64 inv = owner.first;
    if (std::find(g_look_pending.begin(), g_look_pending.end(), inv) == g_look_pending.end()) g_look_pending.push_back(inv);
    return r;
}
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
    // (a weapon set is never the applied outfit: loading one leaves the applied outfit as it is, #27)
    if (index >= 0 && size_t(index) < g_outfits.size() && is_weapon_set(g_outfits[size_t(index)])) return;
    if (index >= 0) g_unmanaged.erase(g_character); // (a load or a save: the outfit is managed again)
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
    if (exact >= 0 && !let_go(exact)) mark_worn(exact); // (the outfit let go: not taken up again by itself)
    else if (int a = anchor_outfit(); a >= 0 && left_anchor(g_outfits[size_t(a)], w, n)) {
        LOG("\"%s\": too much changed, what is worn is a new set", g_outfits[size_t(a)].name.c_str());
        mark_worn(-1);
    }
}
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

// #21: the managed outfit let go - nothing is taken off; its looks stop applying (no outfit is marked worn) until an
// outfit is loaded or saved (mark_worn with an outfit clears it). Kept per character, in the outfits file.
void release_outfit(U64 self)
{
    std::string name;
    for (const Outfit& o : g_outfits)
        if (o.worn) name = o.name;
    int worn = -1;
    for (size_t i = 0; i < g_outfits.size(); ++i)
        if (g_outfits[i].worn) worn = int(i);
    if (worn < 0) return;
    if (fx_defer_release(self)) return; // (the outfit effect: the glow up first; fx_tick lets it go at the peak)
    g_unmanaged[g_character] = worn;
    LOG("let go of the outfit (character %u): its looks no longer apply", g_character);
    mark_worn(-1);
    save_outfits(); // (the flag; mark_worn saves only when a mark changed)
    look_refresh();
    if (self) report(self, lang::f("toast.released", {{"outfit", name}})); // (a report: Messages Off hides it)
}
} // namespace om
