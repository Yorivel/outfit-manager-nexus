// The outfit transition (#9) and the hidden weapons' effects.
//
// Loading an outfit: Kliff glows up in the game's hologram (the skinned-mesh material's Holo parameters), the outfit
// changes at the peak, and the glow fades on the new outfit. His weapons get the game's mesh effect glow (the "sneak
// peek" sweep, dark grey). Weapons the new outfit hides stay until their glow's peak ends, then go at its brightest.
// Hidden weapons drawn / sheathed (and weapons shown or hidden by their combat look): the glow on the weapon, the
// hologram on its scabbard / quiver / bow (drawn like armour, the glow doesn't show on them).
//
// Findings this rests on (2026-11-07, build 26480954; 2976 addresses in brackets):
//  - Material values only take on the game thread: sent from the player's material update (kRvaMatUpdate [0x4d30f0]).
//  - UseMaterialEffect (the permutation the Holo / Burn... groups need) is a COUNTER on the effect component
//    (kRvaPermSet [0x4d4270]: 1 adds, 0 takes away; parts are switched at 0 -> 1 and 1 -> 0 only, the parts the model
//    has then). So exactly one "on" and one "off" per run. Its last argument 0 skips parts with byte +0x89 set (the
//    weapons, quiver, ring, earrings) - the restarts use that so the weapons' glow isn't touched.
//  - The effect keeps the meshes it was switched on for drawn: restarted (off + on) as soon as the old pieces have left
//    the part table, and again when every new piece has loaded (no part with a pending load at +0x78).
//  - The weapons' glow particles pushed the world's falling leaves out of the particle budget in busy spots: the
//    global gv_effect_totalParticleCountRatio (found at install: find_budget) is doubled while any glow plays.
//  - Values sent for single parts (kRvaMatChange with a part list) are only STORED in the character's value table
//    (effect component +0x400); they reach the meshes when the game runs its apply pass over the table
//    (kRvaMatApply), which it does when it sends a value to the whole character or in its own update - not every
//    frame. A transition sends whole-character values each frame, so it never noticed; the part hologram's values
//    arrived whenever the game happened to send something (the effect played on some draws and not on others). The
//    part hologram now runs the pass itself, right after its values (2026-11-12).
//  - While UseMaterialEffect is on for a part, bare skin (and what is fitted to it) shows the hologram whatever
//    values it is sent: with the permutation on for the whole character, all of him lit up with a drawn tool ("the
//    ghost"). So the part hologram switches it on for ITS parts only: the switch's last argument 0 skips the parts
//    flagged at +0x89, and for the call every other part is flagged (perm_parts; 2026-11-12, tested with the bow,
//    the swords, the sprayer, the fishing rod, on a bare and a dressed torso).
//  - A piece's glow colour is piece +0x98 (ARGB) / +0x94 (alpha / 255): the game copies the settings' colour only when
//    a piece is made, so it is written every time.
#include "om.h"
#include "gear.h"
#include "addrs.h"
#include "looks.h"
#include "load.h"
#include "ui.h"
#include "settings.h"
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace om {
namespace {

// ---- game calls (guarded: a game update that moves a layout must not take the game down) ----

bool g_faulted = false; // (a guarded call faulted: the effects stop for this session)
int fault_filter()
{
    if (!g_faulted) LOG("transition: a game call faulted; the outfit effects are off for this session");
    g_faulted = true;
    return EXCEPTION_EXECUTE_HANDLER;
}

U32 name_id(const char* name)
{
    int id = 0;
    __try {
        reinterpret_cast<void(__fastcall*)(int*, const char*, int, int)>(g_base + kRvaNameId)(&id, name, 1, 0x2FFFF);
    } __except (fault_filter()) {
        return 0;
    }
    return U32(id);
}
U32 id_of(const char* name) // (the ids don't change while the game runs: looked up once each)
{
    static std::mutex mu;
    static std::vector<std::pair<const char*, U32>> cache;
    std::lock_guard<std::mutex> lock(mu);
    for (const auto& [n, id] : cache)
        if (n == name) return id;
    U32 id = name_id(name);
    cache.push_back({name, id});
    return id;
}

// a float material value to the whole character: a message (kRvaMsgInit with the name id, the float message vtable,
// the value at +0x30) sent through the effect component (kRvaMatSend)
bool send_float(U64 comp, U32 id, float v)
{
    alignas(16) U8 msg[0x80] = {};
    struct { U64 p; U32 id, pad; } key{0, id, 0};
    __try {
        reinterpret_cast<void(__fastcall*)(void*, void*)>(g_base + kRvaMsgInit)(msg, &key);
        *reinterpret_cast<U64*>(msg) = g_base + kRvaMsgFloatVtable;
        *reinterpret_cast<float*>(msg + 0x30) = v;
        reinterpret_cast<void(__fastcall*)(U64, U8, void*, U64)>(g_base + kRvaMatSend)(comp, 0, msg, 0);
        return true;
    } __except (fault_filter()) {
        return false;
    }
}
// a value (1..4 floats) through the change function in mode 2: at once, to the listed parts (model part handles) or,
// with none, to the whole character
bool mat_change(U64 actor, U32 id, const float* v, U32 n, const std::vector<U32>& parts)
{
    float timev[4] = {};
    float val[4] = {};
    for (U32 i = 0; i < n && i < 4; ++i) val[i] = v[i];
    struct { const U32* p; U32 n, cap; } pl{parts.empty() ? nullptr : parts.data(), U32(parts.size()), U32(parts.size())};
    __try {
        reinterpret_cast<void(__fastcall*)(U64, void*, float, U32*, float*, U32, U8, U32, void*)>(g_base + kRvaMatChange)(
            actor, timev, 0.0f, &id, val, n, 2, 0, &pl);
        return true;
    } __except (fault_filter()) {
        return false;
    }
}
bool mat_float(U64 actor, U32 id, float v, const std::vector<U32>& parts) { return mat_change(actor, id, &v, 1, parts); }
// UseMaterialEffect on / off (a counter: see the top); all = 0 skips the parts flagged at +0x89 (the weapons)
bool perm(U64 comp, bool on, U8 all)
{
    alignas(16) U8 msg[0x80] = {};
    __try {
        reinterpret_cast<void(__fastcall*)(void*, U64)>(g_base + kRvaMsgInit)(msg, g_base + kRvaPermKey);
        *reinterpret_cast<U64*>(msg) = g_base + kRvaMsgUintVtable;
        *reinterpret_cast<U32*>(msg + 0x30) = on ? 1u : 0u;
        *reinterpret_cast<U32*>(msg + 0x34) = *reinterpret_cast<U32*>(rp(g_base + kRvaPermValue));
        U32 pid = *reinterpret_cast<U32*>(g_base + kRvaPermId);
        reinterpret_cast<void(__fastcall*)(U64, U32*, void*, U8)>(g_base + kRvaPermSet)(comp, &pid, msg, all);
        *reinterpret_cast<U64*>(msg) = g_base + kRvaMsgBaseVtable; // (as the game leaves it: the base message's vtable)
        return true;
    } __except (fault_filter()) {
        return false;
    }
}
bool collect(U64 out, U64 set, U64 obj) // a mesh effect's pieces on an object (the weapons' container)
{
    __try {
        reinterpret_cast<void(__fastcall*)(U64, U64, U64, U8)>(g_base + kRvaMeshCollect)(out, set, obj, 1);
        return true;
    } __except (fault_filter()) {
        return false;
    }
}
bool collect_named(U64 obj, U64 out, U64 set, U64 ids) // ... on the model's parts with these name ids
{
    __try {
        reinterpret_cast<void(__fastcall*)(U64, U64, U64, U64)>(g_base + kRvaMeshCollectNamed)(obj, out, set, ids);
        return true;
    } __except (fault_filter()) {
        return false;
    }
}
bool paint(U64 piece, U64 obj, U64 data) // a collected piece started with the effect's data
{
    __try {
        reinterpret_cast<void(__fastcall*)(U64, U64, U64, U64)>(g_base + kRvaMeshColour)(piece, obj, g_base + kRvaMeshColourArg,
                                                                                          data);
        return true;
    } __except (fault_filter()) {
        return false;
    }
}

// ---- the player ----

struct Player {
    U64 ctx[4] = {};
    U64 actor = 0;
    Player() { actor = acquire_player(ctx); }
    ~Player()
    {
        if (ctx[0]) release_player(ctx);
    }
    U64 comp() const { return actor ? rp(rp(actor + kOffActorComp) + 0x60) : 0; } // (its effect component)
    U64 holder() const { return actor ? rp(rp(rp(rp(rp(actor + kOffActorComp) + 0x40) + 0x130)) + 8) : 0; } // (model holder)
};
std::vector<U32> part_handles(U64 model) // every model part handle (the clean-up)
{
    std::vector<U32> out;
    U64 tab = model ? rp(model + 0x58) : 0;
    U32 n = model ? r32(model + 0x60) : 0;
    for (U32 i = 0; tab && i < n && i < 1024; ++i)
        if (rp(tab + i * 16ull + 8)) out.push_back(r32(tab + i * 16ull));
    return out;
}

// ---- the look ----

// The chosen look ("Sneak-peek hologram", 2026-11-07): whole character, strength x 0..1
constexpr float kEmissive = 2.5f, kSpread = 3.0f, kRadius = 3.0f;
constexpr U64 kInMs = 450, kOutMs = 800;
// the weapons' glow: dark grey (the user's pick), the sneak peek's own timing (shorter ones cut its sweep short)
constexpr U32 kGlowColour = 0xFF202830;
constexpr float kGlowIn = 0.4f, kGlowHold = 1.5f, kGlowOut = 0.6f;
constexpr U64 kGlowMs = 2600, kGlowPeakMs = 1950;
// the mesh effect's settings / data templates (from the game's own weapon effect call)
const U32 kSet[18] = {0, 0, 0, 0, 0x3F800000, 0xFF37281E, 0, 0x3E4CCCCD, 0x3E99999A, 0x3E4CCCCD, 1, 0, 0, 0, 0x4192, 0, 0, 0};
const U32 kData[10] = {0, 0, 0, 0x7F01, 0, 0xBF800000, 0xBF800000, 0, 0, 0};

int setting() { return g_faulted ? 0 : transition_setting(); } // 0 off, 1 performance (no glow particles), 2 quality

void send_look(U64 comp, U64 actor, float x)
{
    auto g = [&](const char* nm, float v) { send_float(comp, id_of(nm), v); };
    g("_holoFXRatio", x > 0.0f ? 1.0f : 0.0f);
    g("_holo_Line_Pattern", 0.0f);
    g("_holo_Emissive_Intensity", kEmissive * x);
    g("_holo_SphereMask_Radius", kRadius * x);
    g("_holo_Noise_Spread", kSpread);
    g("_particleInjectRatio", x);
    g("_holo_Weapon_Effect", x > 0.0f ? 1.0f : 0.0f);
    g("_holo_VertexDiscard_Ratio", 0.0f);
    // (as sent with the chosen look: the colour message itself never took; the start of the sweep above the head)
    static const float kColour[4] = {0.25f, 0.55f, 1.0f, 1.0f}, kStart[3] = {0.0f, 3.0f, 0.0f};
    if (actor) mat_change(actor, id_of("_holo_Color"), kColour, 4, {}), mat_change(actor, id_of("_holo_Diffusion_StartPosition"), kStart, 3, {});
}
void clean_up(U64 comp, U64 actor) // every hologram value 0, on the whole character and on every part it has now
{
    std::vector<U32> all = actor ? part_handles(look_model(actor)) : std::vector<U32>{};
    for (const char* nm : {"_holoFXRatio", "_holo_Emissive_Intensity", "_holo_SphereMask_Radius", "_particleInjectRatio",
                           "_holo_VertexDiscard_Ratio", "_holo_Line_Pattern"}) {
        send_float(comp, id_of(nm), 0.0f);
        if (actor && !all.empty()) mat_float(actor, id_of(nm), 0.0f, all);
    }
}

// the glow's settings and data for one run
void glow_templates(U8 (&set)[0x48], U8 (&data)[0x28], bool named)
{
    std::memset(set, 0, sizeof set), std::memset(data, 0, sizeof data);
    std::memcpy(set, kSet, sizeof kSet), std::memcpy(data, kData, sizeof kData);
    *reinterpret_cast<U32*>(set + 0x14) = kGlowColour;
    *reinterpret_cast<float*>(set + 0x1C) = kGlowIn;
    *reinterpret_cast<float*>(set + 0x20) = kGlowHold;
    *reinterpret_cast<float*>(set + 0x24) = kGlowOut;
    if (named) set[0x29] = 1; // (names given: as the game's named call)
    if (setting() == 1) reinterpret_cast<float*>(data)[5] = 0.0f; // (performance: no glow particles - data [5] = spawn ratio)
}
int glow_piece(U64 e, U64 data)
{
    U8 f15 = 1;
    if (!e || !rd(e + 0x15, f15) || f15 == 1) return 0;
    wr<U32>(e + 0x98, kGlowColour), wr<float>(e + 0x94, float(kGlowColour >> 24) / 255.0f);
    U64 h = rp(e + 0x18), o2 = h ? rp(h + 8) : 0;
    return o2 && paint(e, o2 - 0x28, data) ? 1 : 0;
}
void glow_all_weapons(const Player& p) // the weapons' container: every weapon piece
{
    U64 x = p.holder();
    U64 h = x ? rp(x + 0x50) : 0, wo = h && rp(h + 8) ? rp(h + 8) - 0x28 : 0;
    alignas(16) static U8 set[0x48], data[0x28];
    glow_templates(set, data, false);
    struct { U64 p; U32 n, cap; } out{};
    int c = 0;
    if (wo && collect(reinterpret_cast<U64>(&out), reinterpret_cast<U64>(set), wo))
        for (U32 i = 0; out.p && i < out.n && i < 64; ++i) c += glow_piece(rp(out.p + i * 8ull), reinterpret_cast<U64>(data));
    LOG("transition: weapon glow on %d piece(s)", c);
}
void glow_named(const Player& p, const std::vector<U32>& names) // the parts with these names (one weapon)
{
    U64 x = p.holder();
    alignas(16) static U8 set[0x48], data[0x28];
    glow_templates(set, data, true);
    int c = 0;
    for (U32 id : names) {
        struct { const U32* p; U32 n, cap; } ids{&id, 1, 1};
        struct { U64 p; U32 n, cap; } out{};
        if (!x || !collect_named(x - 0x28, reinterpret_cast<U64>(&out), reinterpret_cast<U64>(set), reinterpret_cast<U64>(&ids)))
            continue;
        for (U32 i = 0; out.p && i < out.n && i < 64; ++i) c += glow_piece(rp(out.p + i * 8ull), reinterpret_cast<U64>(data));
    }
    LOG("transition: weapon glow on %d piece(s) of %zu part(s)", c, names.size());
}

// ---- the particle budget: doubled while any glow plays ----

float g_budget_saved = -1.0f; // (UI thread)
std::atomic<U64> g_budget_until{0};
U64 g_budget_at = 0;          // (the value's address; found once)
// gv_effect_totalParticleCountRatio: no code reads it by address (only through its object), so it is found as the game
// registers it - `lea rdx, [name]` then `lea rcx, [object]` (2976 0x26850e, build 26480954 +0x2a190e); the value is
// the object's float at +0x38. Searched once, at install.
U64 find_budget()
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    static const char kName[] = "gv_effect_totalParticleCountRatio";
    U64 name = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && !name; ++i) {
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        const U8* b = reinterpret_cast<const U8*>(g_base + sec[i].VirtualAddress);
        const size_t n = sec[i].Misc.VirtualSize;
        __try {
            for (size_t k = 0; k + sizeof kName <= n; ++k)
                if (b[k] == 'g' && std::memcmp(b + k, kName, sizeof kName) == 0) { name = reinterpret_cast<U64>(b + k); break; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (!name) return 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const U8* b = reinterpret_cast<const U8*>(g_base + sec[i].VirtualAddress);
        const size_t n = sec[i].Misc.VirtualSize;
        __try {
            for (size_t k = 0; k + 14 <= n; ++k) {
                if (b[k] != 0x48 || b[k + 1] != 0x8D || b[k + 2] != 0x15) continue;
                const U64 at = reinterpret_cast<U64>(b + k);
                if (at + 7 + *reinterpret_cast<const int*>(b + k + 3) != name) continue;
                if (b[k + 7] != 0x48 || b[k + 8] != 0x8D || b[k + 9] != 0x0D) continue;
                return at + 14 + *reinterpret_cast<const int*>(b + k + 10) + 0x38;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return 0;
}
void budget(bool raise)
{
    if (!g_budget_at) return;
    const U64 at = g_budget_at;
    float v = 0.0f;
    if (!rd(at, v)) return;
    if (raise) {
        if (g_budget_saved < 0.0f) g_budget_saved = v, wr<float>(at, v * 2.0f);
    } else if (g_budget_saved >= 0.0f) {
        if (v == g_budget_saved * 2.0f) wr<float>(at, g_budget_saved); // (the game set its own meanwhile: that one stays)
        g_budget_saved = -1.0f;
    }
}

// ---- shared state (game thread / UI thread) ----

std::mutex g_mu; // (the queues below)
std::vector<std::vector<U32>> g_glow_jobs; // (named weapon glows waiting for the game thread)
struct HoloRun { // the hologram on single parts (scabbards, quivers, the bow): one run for all of them
    std::vector<U32> handles;
    U64 t0 = 0;
    bool sheathe = false;
};
std::vector<HoloRun> g_holo; // (0 or 1 run: two runs sending the same values each frame, the game kept one part list)
bool g_holo_perm = false;    // (game thread: the permutation is on for the part hologram, not for a transition)
std::atomic<U64> g_holo_sent_at{0};

std::atomic<float> g_k{0.0f};            // the transition's strength now (UI thread -> game thread)
std::atomic<bool> g_k_dirty{false};
std::atomic<bool> g_glow_start{false};   // (the weapons' glow: once, after the first "on")
std::atomic<bool> g_restart{false};      // (a restart wanted)
std::atomic<U64> g_restarted_at{0};      // (... and when it ran)
std::atomic<U64> g_glow_end{0};          // (the weapons' glow ends: the end clean-up waits for it - it cut it short)
bool g_perm_on = false;                  // (game thread)

// ---- the game thread: the player's material update ----

using MatUpdFn = void(__fastcall*)(U64);
MatUpdFn g_mat_upd_orig = nullptr;

// UseMaterialEffect for chosen parts only (model part handles): the game's switch, with its last argument 0, skips the
// parts whose byte +0x89 is set. For the call every other part is flagged and the chosen ones are not; the bytes are
// put back right after (same thread, nothing of the game runs in between). The flag's reader is in the game's
// protected code, so its offset has no witness: the bytes are checked first as the game leaves them (0 / 1 only, the
// head unflagged, a weapon flagged where there is one) - anything else, and the switch is made for the whole
// character as before (false).
bool perm_parts(const Player& p, U64 comp, bool on, const std::vector<U32>& parts)
{
    const U64 model = look_model(p.actor), tab = model ? rp(model + 0x58) : 0;
    const U32 n = model ? r32(model + 0x60) : 0;
    std::vector<std::pair<U64, U8>> saved;
    bool sane = tab && n, weapon = false, weapon_flagged = false;
    for (U32 i = 0; sane && i < n && i < 1024; ++i) {
        const U64 q = rp(tab + i * 16ull + 8);
        U8 old = 0;
        if (!q) continue;
        if (!rd(q + 0x89, old) || old > 1) { sane = false; break; }
        const std::string nm = part_name(r32(q + 0x48));
        if (nm == "Head" && old) sane = false;
        if (nm.rfind("MainWeapon_", 0) == 0 || nm.rfind("TwoHandWeapon_", 0) == 0) weapon = true, weapon_flagged |= old == 1;
        saved.push_back({q, old});
    }
    if (!sane || (weapon && !weapon_flagged)) {
        static bool told = false;
        if (!told) told = true, LOG("transition: the parts' effect flags are not as known; the part hologram's mode is set for the whole character");
        return false;
    }
    for (const auto& [q, old] : saved)
        wr<U8>(q + 0x89, std::find(parts.begin(), parts.end(), r32(q + 0x48)) != parts.end() ? 0 : 1);
    perm(comp, on, 0);
    for (const auto& [q, old] : saved) wr<U8>(q + 0x89, old);
    return true;
}
// The game's apply pass over the character's value table, as its own update runs it (kRvaMatApply: the table, a time
// step, the character's scene list, kRvaMatApplyOwner of the component's owner): what was sent for single parts
// reaches the meshes now.
void apply_table(U64 comp)
{
    if (!kRvaMatApply || !kRvaMatApplyOwner || !kOffCompModel || !kOffModelScenes) return;
    const U64 owner = rp(comp + 8);
    const U64 scenes = owner ? rp(rp(rp(owner + kOffActorComp) + kOffCompModel) + kOffModelScenes) : 0;
    if (!owner || !scenes) return;
    __try {
        const U64 obj = reinterpret_cast<U64(__fastcall*)(U64)>(g_base + kRvaMatApplyOwner)(owner);
        reinterpret_cast<void(__fastcall*)(U64, float, U64, U64)>(g_base + kRvaMatApply)(comp + 0x400, 0.0f, scenes, obj);
    } __except (fault_filter()) {
    }
}
std::vector<U32> g_holo_mode_parts; // (game thread: the parts the mode is on for; empty: for the whole character)

void holo_tick(const Player& p, U64 comp)
{
    std::vector<HoloRun> runs;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_holo.empty() && !g_holo_perm) return;
        if (g_k.load() > 0.0f) { // (a transition has the effect now)
            g_holo.clear(), g_holo_perm = false;
            if (!g_holo_mode_parts.empty()) { // (ours was for some parts: off, the transition switches its own on)
                if (!perm_parts(p, comp, false, g_holo_mode_parts)) perm(comp, false, 1);
                g_holo_mode_parts.clear(), g_perm_on = false;
            }
            return;
        }
        runs = g_holo;
    }
    std::vector<U32> want; // (every part of the runs)
    for (const HoloRun& r : runs)
        for (U32 h : r.handles)
            if (std::find(want.begin(), want.end(), h) == want.end()) want.push_back(h);
    if (!g_perm_on) {
        if (perm_parts(p, comp, true, want)) g_holo_mode_parts = want;
        else perm(comp, true, 1);
        g_perm_on = true, g_holo_perm = true;
    } else if (!g_holo_mode_parts.empty() && want.size() > g_holo_mode_parts.size()) { // (a part joined the run)
        if (!perm_parts(p, comp, false, g_holo_mode_parts)) perm(comp, false, 1);
        if (perm_parts(p, comp, true, want)) g_holo_mode_parts = want;
        else perm(comp, true, 1), g_holo_mode_parts.clear();
    }
    const U64 now = GetTickCount64();
    bool over = false;
    for (const HoloRun& r : runs) {
        const U64 t = now - r.t0, tin = r.sheathe ? 400 : 0; // (drawn: it appears at full; sheathed: it builds up)
        const float x = t < tin ? float(t) / float(tin) : t < 1900 ? 1.0f : t < 2500 ? 1.0f - float(t - 1900) / 600.0f : 0.0f;
        mat_float(p.actor, id_of("_holoFXRatio"), x > 0.0f ? 1.0f : 0.0f, r.handles);
        mat_float(p.actor, id_of("_holo_Line_Pattern"), 0.0f, r.handles);
        mat_float(p.actor, id_of("_holo_Emissive_Intensity"), kEmissive * x, r.handles);
        mat_float(p.actor, id_of("_holo_SphereMask_Radius"), kRadius * x, r.handles);
        mat_float(p.actor, id_of("_holo_Noise_Spread"), kSpread, r.handles);
        mat_float(p.actor, id_of("_particleInjectRatio"), x, r.handles);
        // (a weapon's material shows the hologram only with this at 1. A transition sets it for everyone and leaves
        // it at 0, and nothing here set it: a weapon that was there at a transition's end - the bow an outfit brings -
        // showed no hologram until it was equipped anew; 2026-11-12)
        mat_float(p.actor, id_of("_holo_Weapon_Effect"), x > 0.0f ? 1.0f : 0.0f, r.handles);
        over |= t >= 2500;
    }
    apply_table(comp); // (this frame's values reach the meshes now, not when the game next applies its table)
    g_holo_sent_at = now;
    std::lock_guard<std::mutex> lock(g_mu);
    if (over && !g_holo.empty() && now - g_holo.front().t0 >= 2500) g_holo.clear();
    if (g_holo.empty() && g_holo_perm) {
        if (g_holo_mode_parts.empty() || !perm_parts(p, comp, false, g_holo_mode_parts)) perm(comp, false, 1);
        g_holo_mode_parts.clear();
        g_perm_on = false, g_holo_perm = false;
    }
}

std::atomic<bool> g_glow_all_req{false}; // (a weapon set's change, #27: glow the worn weapons, no body effect)

void transition_tick(const Player& p, U64 comp)
{
    float k = g_k.load();
    if (k == 0.0f && g_perm_on && !g_holo_perm && GetTickCount64() < g_glow_end.load()) { g_k_dirty = true; return; } // (later)
    if (g_restart.exchange(false)) {
        if (k > 0.0f) { // (1 -> 0 -> 1, the weapons skipped: the old pieces let go, the new ones take it)
            if (g_perm_on) perm(comp, false, 0);
            perm(comp, true, 0), g_perm_on = true;
        }
        g_restarted_at = GetTickCount64();
    }
    if (k > 0.0f && !g_perm_on) perm(comp, true, 1), g_perm_on = true;
    if (k > 0.0f) g_holo_perm = false; // (the transition owns it now)
    if (g_perm_on && g_glow_start.exchange(false)) glow_all_weapons(p), g_glow_end = GetTickCount64() + kGlowMs;
    send_look(comp, p.actor, k);
    if (k == 0.0f) {
        clean_up(comp, p.actor);
        if (g_perm_on) perm(comp, false, 1), g_perm_on = false;
    }
}

void __fastcall mat_upd_hook(U64 comp)
{
    g_mat_upd_orig(comp);
    present_game_thread(); // (present.cpp: the presentation changes the looks asked for, on the game thread)
    bool glows = false, holo = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        glows = !g_glow_jobs.empty(), holo = !g_holo.empty() || g_holo_perm;
    }
    const bool trans = g_k_dirty.load() || g_k.load() > 0.0f;
    const bool glow_all = g_glow_all_req.load();
    if (!glows && !holo && !trans && !glow_all) return;
    Player p;
    if (!p.actor || comp != p.comp()) return; // (another character's update)
    if (glows) {
        std::vector<std::vector<U32>> jobs;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            jobs.swap(g_glow_jobs);
        }
        for (const auto& names : jobs) glow_named(p, names);
    }
    if (glow_all && g_glow_all_req.exchange(false)) glow_all_weapons(p), g_glow_end = GetTickCount64() + kGlowMs;
    if (holo) holo_tick(p, comp);
    if (trans) g_k_dirty = false, transition_tick(p, comp);
}

// ---- the UI thread: the transition's timeline ----

enum class State { Idle, In, Swapped, Out };
State g_state = State::Idle;
U64 g_load_self = 0, g_t = 0;
U32 g_index = 0;
bool g_weapons_fx = false; // (#27: a weapon set's change - the weapons glow out, swap, glow in; no body effect)
constexpr U64 kSetSwapMs = 450, kSetAttachMs = 600; // (the swap at the old weapons' glow; the new ones glow once on)
constexpr U32 kRelease = 0xFFFFFFFF; // (g_index: Unequip Outfit - at the peak the outfit is let go instead of a load, #21)
bool g_bypass = false;
void set_k(float k) { g_k = k, g_k_dirty = true; }

U64 table_sig(U64 model) // the model's part table: count, handles and part objects
{
    U64 sig = 1469598103934665603ull;
    if (!model) return sig;
    U64 tab = rp(model + 0x58);
    U32 n = r32(model + 0x60);
    sig ^= n, sig *= 1099511628211ull;
    for (U32 i = 0; tab && i < n && i < 1024; ++i) sig ^= rp(tab + i * 16ull + 8) ^ r32(tab + i * 16ull), sig *= 1099511628211ull;
    return sig;
}

} // namespace

bool transition_on() { return setting() != 0; }

bool fx_defer_load(U64 self, U32 index)
{
    if (g_bypass || !transition_on() || !self) return false;
    if (g_state != State::Idle) {
        notify(self, lang::t("toast.still_changing"));
        return true;
    }
    g_load_self = self, g_index = index, g_t = GetTickCount64(), g_state = State::In;
    budget(true);
    g_weapons_fx = index < g_outfits.size() && is_weapon_set(g_outfits[index]);
    if (g_weapons_fx) { // (only the weapons: the worn ones glow now, the new ones once they are on)
        g_glow_all_req = true;
        return true;
    }
    g_weapon_hide_after = g_t + kGlowPeakMs; // (weapons the new outfit hides: they go at their glow's peak)
    g_glow_start = true;
    set_k(0.0f);
    return true;
}

// Unequip Outfit (#21) the way an outfit change looks: the glow up, the outfit let go at the peak (what it hid shows
// again under the glow), the glow down.
bool fx_defer_release(U64 self) { return fx_defer_load(self, kRelease); }

void fx_tick(U64 screen)
{
    (void)screen;
    const U64 now = GetTickCount64();
    // the looks that wait for an effect: a drawn scabbard / an appearing weapon (its hologram is on), a sheathed /
    // going weapon (its glow's peak), the weapons a transition hides (their glow's peak)
    if (g_case_wait_since && (g_holo_sent_at.load() > g_case_wait_since || now - g_case_wait_since > 300))
        g_case_wait_since = 0, look_refresh();
    if (g_sheathe_refresh_at && now >= g_sheathe_refresh_at) g_sheathe_refresh_at = 0, look_refresh();
    if (g_weapon_hide_after && now >= g_weapon_hide_after) g_weapon_hide_after = 0, look_refresh();
    { // the particle budget: raised while any glow plays
        const bool glow = g_state != State::Idle || now < g_glow_end.load() || now < g_budget_until.load();
        if (glow) budget(true);
        else budget(false);
    }
    if (g_state == State::Idle) return;
    const U64 dt = now - g_t;
    if (g_weapons_fx) { // a weapon set (#27): swap at the old weapons' glow, glow the new ones, wait for their glow
        const U64 d = now - g_t;
        if (g_state == State::In && d >= kSetSwapMs) {
            g_bypass = true;
            start_loading(g_load_self, g_index);
            g_bypass = false;
            g_state = State::Swapped, g_t = now;
        } else if (g_state == State::Swapped && d >= kSetAttachMs) {
            g_glow_all_req = true;
            g_state = State::Out, g_t = now;
        } else if (g_state == State::Out && d >= kGlowMs) {
            g_state = State::Idle, g_weapons_fx = false;
        }
        return;
    }
    if (g_state == State::In) { // glow up on the old outfit, then the load
        set_k(std::min(1.0f, float(dt) / float(kInMs)));
        if (dt < kInMs) return;
        g_bypass = true;
        if (g_index == kRelease) release_outfit(g_load_self);
        else start_loading(g_load_self, g_index);
        g_bypass = false;
        g_state = State::Swapped, g_t = now;
    } else if (g_state == State::Swapped) {
        // held at the peak until the outfit has settled: a restart as soon as the old pieces have left the part table
        // (the effect lets go of them), another once every new piece has loaded and the table is quiet for 50 ms (the
        // new ones take it); the hold ends 50 ms after that one ran - at most 3 s
        set_k(1.0f);
        Player p;
        U64 model = p.actor ? look_model(p.actor) : 0;
        const U64 sig = table_sig(model);
        static U64 first = 0, last = 0, changed_at = 0, asked_at = 0;
        static bool early = false;
        if (dt == 0 || !first) first = sig, last = sig, changed_at = now, asked_at = 0, early = false;
        if (!early && sig != first && !g_restart.load()) early = true, g_restart = true;
        if (sig != last) last = sig, changed_at = now;
        const bool busy = (model && model_loading(model)) || g_async.active;
        if (!busy && now - changed_at >= 50 && dt >= 100 && !asked_at && !g_restart.load())
            asked_at = now, g_restarted_at = 0, g_restart = true;
        const U64 ran = g_restarted_at.load();
        const bool settled = asked_at && ran && now - ran >= 50;
        if (!settled && dt < 3000) return;
        first = 0;
        g_state = State::Out, g_t = now;
    } else if (g_state == State::Out) { // glow down on the new outfit
        set_k(std::max(0.0f, 1.0f - float(dt) / float(kOutMs)));
        if (dt < kOutMs) return;
        set_k(0.0f);
        if (dt < kOutMs + 300) return;
        g_state = State::Idle;
    }
}

bool fx_transition_busy() { return g_state != State::Idle; }
int fx_busy_target() { return g_state == State::Idle ? -1 : g_index == kRelease ? -2 : int(g_index); }

void fx_weapon_glow(const std::vector<U32>& names, bool sheathe)
{
    (void)sheathe; // (both ways the same glow: drawn, it appears in it; sheathed, the weapon goes at its peak)
    if (!transition_on() || names.empty()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    g_glow_jobs.push_back(names);
    g_budget_until = GetTickCount64() + kGlowMs + 100;
}

void fx_scabbard_holo(const std::vector<U32>& handles, bool sheathe)
{
    if (!transition_on() || handles.empty()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_holo.empty()) { // (a second one joins the run; another kind - sheathed while drawing - starts it again)
        HoloRun& r = g_holo.front();
        for (U32 h : handles)
            if (std::find(r.handles.begin(), r.handles.end(), h) == r.handles.end()) r.handles.push_back(h);
        if (r.sheathe != sheathe) r.t0 = GetTickCount64(), r.sheathe = sheathe;
        return;
    }
    g_holo.push_back({handles, GetTickCount64(), sheathe});
}

bool fx_scab_applied_after(U64 t) { return g_holo_sent_at.load() > t; }

bool install_transition()
{
    if (!kRvaMatUpdate || !kRvaMatSend || !kRvaMatChange || !kRvaPermSet || !kRvaMsgInit || !kRvaNameId ||
        !kRvaMeshCollect || !kRvaMeshCollectNamed || !kRvaMeshColour) {
        LOG("transition: game functions not found; the outfit effects are off");
        return false;
    }
    void* t = reinterpret_cast<void*>(g_base + kRvaMatUpdate);
    bool ok = MH_CreateHook(t, reinterpret_cast<void*>(&mat_upd_hook), reinterpret_cast<void**>(&g_mat_upd_orig)) == MH_OK &&
              MH_EnableHook(t) == MH_OK;
    g_budget_at = find_budget();
    float v = 0.0f;
    if (g_budget_at && !(rd(g_budget_at, v) && v > 0.0f && v < 100.0f)) g_budget_at = 0; // (not a ratio: not that)
    LOG("transition: %s; particle budget %s", ok ? "ready" : "hook failed, the outfit effects are off",
        g_budget_at ? "found" : "not found (busy scenes may flicker)");
    if (!ok) g_faulted = true;
    return ok;
}

} // namespace om
