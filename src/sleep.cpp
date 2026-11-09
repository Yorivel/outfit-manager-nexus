// Activity: Sleeping (2026-11-09; keksdieb101's idea on Nexus): when the character gets into bed, a sleepwear outfit goes
// on (several: one at random); when they get out again - having slept or not - the outfit worn before comes back, or,
// when none was, the pieces worn before.
//
// Getting into bed: the game reads the bed's scene from its packs every time a bed is used - gimmick_sleep_bed_left /
// _right (the side Kliff climbs in from), gimmick_camp_straw - about a second after the interaction, while the screen
// is black (MO2CDLoader's lookup log, 2026-11-09). The pack lookup (found as MO2CDLoader finds it: the call in the pack
// reader's open slot, class ResourceLoadWorker_Package) is hooked and watches for those paths.
// Whose bed: the game takes the character over for the interaction - a branch into one of ai_basic_lower's one-step
// states (kStep), taken by the played character's chart contexts just before; those contexts are remembered.
// Getting out: one of them takes a branch into a state outside the scripted / hand-over ones (kQuiet: the player-only
// sequencer charts and ai_basic_lower's idles) - control is back (cancelled at the hours menu, or awake).
// The branch evaluator (kRvaBranchEval) is hooked for those branches (result 0 = taken). A sleep that begins without a
// known bed scene (a sleeping spot with another scene) is still seen at the sleep itself: bed_sleep start / loop
// (sequencer_action_pc).
//
// The game only HIDES the helmet, cloak and weapons in bed, it doesn't unequip them (2026-11-09: no slot changed), so
// what is worn is read when the bed is seen. The hooks only set flags and post kWmSleep to the game window: its thread
// is the UI thread, and its messages keep coming while the HUD is hidden (the HUD frame, peek_frame, is the fallback).
// The sleep's outfit changes play the outfit effect; its timeline (fx_tick, on HUD frames) is carried by a window timer
// while the HUD is hidden for the bed's fade (fx_carry).
//
// In bed, the looks (looks.cpp, sleep_looks): a sleepwear outfit's own looks win over the game's in-bed hiding (a cloak
// set to Shown stays shown; Default goes the game's way); any other outfit steps back as in flight.
#include "om.h"
#include "addrs.h"
#include "gear.h"
#include "load.h"
#include "looks.h"
#include "store.h"
#include "log.h"
#include "game.h"
#include "wheel_hooks.h"
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace om {

#include "sleep_states.inc"

extern std::atomic<HWND> g_game_hwnd; // (input.cpp)
constexpr UINT kWmSleep = WM_APP + 0x51;

namespace {

// bed_sleep start / loop states (sequencer_action_pc, game 2.04.08): the sleep itself (the fallback signal)
constexpr U32 kSleepStart[] = {0xD1F9862D, 0xE2B2EE2F, 0x70571FD6, 0xF50A6DC9}; // stt_02, ing_02, stt_03, ing_03
// ai_basic_lower's one-step states: the game taking the character over for an interaction
constexpr U32 kStep[] = {0x00D286FD, 0x2CAC00E9, 0x7D527861, 0x96A64775, 0xEF3C23FF};
// the scenes a bed / sleeping spot plays when used (sequencer/.../gimmickcalledseq/<name>.*)
const char* const kBedScenes[] = {"gimmick_sleep_bed_", "gimmick_camp_straw"};
constexpr U64 kStepWindowMs = 4000; // (the step before the scene: within this)
constexpr U32 kPlayerIdle = 0x662EC8AD; // (basic_upper s109: the played character's plain idle, taken by his context only)
constexpr U64 kOutAfterMs = 1500;   // (getting out needs the bed to have lasted: the states around getting in aren't it)

using EvalFn = U32*(__fastcall*)(U64 ctx, U32* out, U64 a3, U64 a4, U64 a5, U64 a6, U64 a7, const U64* link, U64 a9,
                                 U64 a10);
EvalFn g_eval_orig = nullptr;
using FindFn = bool(__fastcall*)(U64 map, U64 key, U64* out_path, std::uint8_t* out_record);
FindFn g_find_orig = nullptr;

// set on the game's threads, taken on the UI thread
std::atomic<bool> g_hook_asleep{false}; // (in bed: sleeping, or at the hours menu)
std::atomic<U64> g_hook_ctx[2] = {0, 0};
std::atomic<U64> g_hook_since{0};
std::atomic<bool> g_fell{false}, g_woke{false};
std::atomic<U64> g_step_ctx[2] = {0, 0}; // the contexts the game last took over (kStep), and when
std::atomic<U64> g_step_at{0};
std::atomic<U64> g_out_at{0}; // (out of bed: when; the scene read again while getting up is no new bed)
std::atomic<U64> g_player_ctx{0}; // (the context that last took kPlayerIdle: the played character's upper body)
std::atomic<U64> g_player_ring[8] = {}; // (its last states: target << 32 | the tick's low 32 bits; logged at a bed)
std::atomic<U32> g_player_ring_at{0};

void post()
{
    if (HWND w = g_game_hwnd.load()) PostMessageW(w, kWmSleep, 0, 0); // (now, not at the next HUD frame)
}

void into_bed(U64 ctx0, U64 ctx1, const char* why)
{
    if (g_hook_asleep.exchange(true)) return;
    g_hook_ctx[0] = ctx0, g_hook_ctx[1] = ctx1, g_hook_since = GetTickCount64();
    g_fell = true;
    post();
    LOG("sleep: into bed (%s; ctx %llX %llX)", why, ctx0, ctx1);
}

U32* __fastcall eval_hook(U64 ctx, U32* out, U64 a3, U64 a4, U64 a5, U64 a6, U64 a7, const U64* link, U64 a9, U64 a10)
{
    U32* r = g_eval_orig(ctx, out, a3, a4, a5, a6, a7, link, a9, a10);
    __try {
        if (!r || *r != 0 || !link || !link[0]) return r;
        U32 target = *reinterpret_cast<const U32*>(link[0] + 0x14);
        U64 now = GetTickCount64();
        if (target == kPlayerIdle) g_player_ctx = ctx;
        if (ctx == g_player_ctx.load()) {
            U32 i = g_player_ring_at.load();
            if (U32(g_player_ring[(i + 7) % 8].load() >> 32) != target)
                g_player_ring[i % 8] = U64(target) << 32 | U32(now), g_player_ring_at = i + 1;
        }
        if (std::find(std::begin(kStep), std::end(kStep), target) != std::end(kStep)) {
            if (now - g_step_at.load() > 500) g_step_ctx[0] = ctx, g_step_ctx[1] = 0; // (a new take-over)
            else if (g_step_ctx[0].load() != ctx) g_step_ctx[1] = ctx;              // (the other body half)
            g_step_at = now;
        }
        if (std::find(std::begin(kSleepStart), std::end(kSleepStart), target) != std::end(kSleepStart)) {
            into_bed(ctx, 0, "the sleep itself");
            if (g_hook_ctx[0].load() != ctx && !g_hook_ctx[1].load()) g_hook_ctx[1] = ctx;
        } else if (g_hook_asleep.load() && (ctx == g_hook_ctx[0].load() || ctx == g_hook_ctx[1].load()) &&
                   now - g_hook_since.load() >= kOutAfterMs &&
                   !std::binary_search(std::begin(kQuiet), std::end(kQuiet), target)) {
            if (g_hook_asleep.exchange(false)) {
                g_woke = true;
                g_out_at = now;
                post();
                LOG("sleep: out of bed (state %08X, ctx %llX, after %llu ms)", target, ctx, now - g_hook_since.load());
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

// The pack lookup: a bed's scene being read is the character getting into bed.
bool __fastcall find_hook(U64 map, U64 key, U64* out_path, std::uint8_t* out_record)
{
    __try {
        const char* path = key ? **reinterpret_cast<const char***>(key) : nullptr;
        if (path) {
            while (*path == '/') ++path;
            if ((path[0] == 's' || path[0] == 'S') && _strnicmp(path, "sequencer/", 10) == 0 && !g_hook_asleep.load())
                if (const char* g = std::strstr(path, "gimmickcalledseq/"))
                    for (const char* scene : kBedScenes)
                        if (_strnicmp(g + 17, scene, std::strlen(scene)) == 0) {
                            U64 now = GetTickCount64();
                            if (now - g_out_at.load() < 5000) break; // (getting up)
                            U64 pc = g_player_ctx.load();
                            bool stepped = now - g_step_at.load() < kStepWindowMs && g_step_ctx[0].load();
                            if (!pc && !stepped) {
                                LOG("sleep: a bed scene read before the played character was seen: ignored (%s)", g + 17);
                                break;
                            }
                            { // (the played character's last states, for telling his bed from another's later)
                                char buf[200];
                                int len = 0;
                                U32 at = g_player_ring_at.load();
                                for (U32 k = 0; k < 8 && len < 180; ++k)
                                    if (U64 e = g_player_ring[(at + k) % 8].load())
                                        len += std::snprintf(buf + len, sizeof buf - size_t(len), " %08X(-%u)", U32(e >> 32),
                                                             U32(now) - U32(e));
                                buf[len] = 0;
                                LOG("sleep: bed scene %s; the character's last states:%s%s", g + 17, buf,
                                    stepped ? " (taken over just before)" : "");
                            }
                            U64 c1 = stepped ? g_step_ctx[0].load() : 0;
                            if (c1 == pc) c1 = stepped ? g_step_ctx[1].load() : 0;
                            into_bed(pc ? pc : g_step_ctx[0].load(), c1, g + 17);
                            break;
                        }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return g_find_orig(map, key, out_path, out_record);
}

// The pack lookup, as MO2CDLoader finds it: the pack reader's open slot (vtable +0x10) calls it first, right after
// `mov rcx, [rcx+20h]`.
U64 find_pack_lookup()
{
    void** vt = game::find_vtable(".?AVResourceLoadWorker_Package@pa@@");
    if (!vt) return 0;
    U64 open = reinterpret_cast<U64>(vt[2]);
    const game::Module m = game::main_module();
    auto inside = [&](U64 a) { return a >= m.base && a + 16 < m.base + m.size; };
    if (!inside(open)) return 0;
    for (U64 p = open; p < open + 0x100; ++p) {
        const std::uint8_t* b = reinterpret_cast<const std::uint8_t*>(p);
        if (b[0] == 0x48 && b[1] == 0x8b && b[2] == 0x49 && b[3] == 0x20 && b[4] == 0xe8) {
            U64 t = p + 9 + *reinterpret_cast<const std::int32_t*>(p + 5);
            return inside(t) ? t : 0;
        }
    }
    return 0;
}

// HUD side (the HUD frame, or the game window's thread when it is the UI thread: kWmSleep)
U64 g_sleep_self = 0; // (the status screen the HUD frame last gave: its object outlives a hidden HUD)
bool g_asleep = false;
std::atomic<int> g_sleep_look{0}; // what sleep_looks() answers: 0 awake, 1 step back, 2 sleepwear wins (read by look_sync)
bool g_swapped = false;        // a sleepwear outfit was put on for this sleep: something to give back on waking
int g_before = -1;             // the outfit worn before (-1: none; then g_before_items)
Outfit g_before_items;         // the pieces worn before, when no outfit was
bool g_have_items = false;
bool g_temp_loading = false;   // the pieces worn before are going back on, as a temporary outfit (removed after)
std::string g_before_name;     // (the outfit worn before, found again by name: the list may change while asleep)
U16 g_sleep_char = kNoCharacter;

int worn_index()
{
    for (size_t i = 0; i < g_outfits.size(); ++i)
        if (g_outfits[i].worn) return int(i);
    return -1;
}

void update_look_mode()
{
    int w = worn_index();
    int mode = !g_asleep ? 0 : (w >= 0 && g_outfits[size_t(w)].activity == kActivitySleep && !g_outfits[size_t(w)].temp) ? 2 : 1;
    if (mode != g_sleep_look.load()) {
        g_sleep_look = mode;
        LOG("sleep: looks %s", mode == 0 ? "as usual" : mode == 1 ? "left to the game's in-bed hiding" : "the sleepwear outfit's own");
        look_refresh();
    }
}

// The outfit effect's timeline (fx_tick) runs on HUD frames, and the HUD hides for the bed's fade: while an effect the
// sleep started plays, a window timer on the same (UI) thread carries it on frames the HUD misses.
constexpr UINT_PTR kFxTimer = 0x51EE;
U64 g_hud_at = 0; // (the last HUD frame)

void fx_carry()
{
    if (!fx_transition_busy()) return;
    if (HWND w = g_game_hwnd.load()) SetTimer(w, kFxTimer, 15, nullptr);
}

void fell_asleep(U64 self)
{
    g_asleep = true;
    g_swapped = false;
    prepare_outfits(); // (the character's own outfits)
    g_sleep_char = g_character;
    std::vector<U32> wear;
    for (size_t i = 0; i < g_outfits.size(); ++i)
        if (g_outfits[i].activity == kActivitySleep && !g_outfits[i].temp && !is_weapon_set(g_outfits[i])) wear.push_back(U32(i));
    int worn = find_worn_outfit(self);
    LOG("sleep: asleep (character %u, %zu sleepwear outfit(s), worn outfit %d)", g_character, wear.size(), worn);
    if (wear.empty()) return; // (the looks step back; nothing to change)
    if (worn >= 0 && g_outfits[size_t(worn)].activity == kActivitySleep) {
        LOG("sleep: \"%s\" is sleepwear already: kept", g_outfits[size_t(worn)].name.c_str());
        return;
    }
    g_before = worn;
    g_before_name = worn >= 0 ? g_outfits[size_t(worn)].name : std::string();
    g_have_items = false;
    if (worn < 0) {
        g_before_items = Outfit{};
        g_have_items = snapshot(self, g_before_items) && !g_before_items.items.empty();
        LOG("sleep: no outfit worn; %zu pieces noted to come back", g_have_items ? g_before_items.items.size() : size_t(0));
    }
    static std::mt19937 rng{std::random_device{}()};
    U32 pick = wear[std::uniform_int_distribution<size_t>(0, wear.size() - 1)(rng)];
    LOG("sleep: putting on \"%s\"%s", g_outfits[pick].name.c_str(), wear.size() > 1 ? " (picked at random)" : "");
    g_swapped = true;
    start_loading(self, pick);
    fx_carry();
}

void woke_up(U64 self)
{
    g_asleep = false;
    if (!g_swapped) {
        LOG("sleep: awake");
        return;
    }
    g_swapped = false;
    if (g_character != g_sleep_char) {
        LOG("sleep: awake as another character: nothing given back");
        return;
    }
    prepare_outfits();
    int back = -1;
    if (!g_before_name.empty())
        for (size_t i = 0; i < g_outfits.size(); ++i)
            if (g_outfits[i].name == g_before_name && !g_outfits[i].temp) back = int(i);
    if (back >= 0) {
        LOG("sleep: awake; \"%s\" goes back on", g_outfits[size_t(back)].name.c_str());
        start_loading(self, U32(back));
        fx_carry();
    } else if (g_have_items) {
        // (the pieces as a temporary outfit: loaded like any other, never saved, removed once it is on)
        Outfit t = g_before_items;
        t.name = "(before sleep)";
        t.temp = true;
        t.load_weapons = t.has_weapons;
        g_outfits.push_back(t);
        g_temp_loading = true;
        LOG("sleep: awake; the %zu pieces worn before go back on", t.items.size());
        start_loading(self, U32(g_outfits.size() - 1));
        fx_carry();
    } else {
        LOG("sleep: awake; the outfit worn before (\"%s\") is gone: nothing given back", g_before_name.c_str());
    }
    g_have_items = false;
}

void drop_temp()
{
    if (!g_temp_loading || g_async.active || fx_busy_target() != -1 || look_changing()) return;
    g_temp_loading = false;
    bool was_worn = false;
    for (size_t i = g_outfits.size(); i-- > 0;)
        if (g_outfits[i].temp) was_worn |= g_outfits[i].worn, g_outfits.erase(g_outfits.begin() + std::ptrdiff_t(i));
    if (was_worn) mark_worn(-1); // (what is worn is no outfit again, as before the sleep)
    LOG("sleep: the pieces worn before are back on");
}

} // namespace

void sleep_install()
{
    if (!have({kRvaBranchEval})) {
        LOG("sleep: the action chart evaluator wasn't found; sleepwear is off");
        return;
    }
    void* t = reinterpret_cast<void*>(g_base + kRvaBranchEval);
    if (MH_CreateHook(t, reinterpret_cast<void*>(&eval_hook), reinterpret_cast<void**>(&g_eval_orig)) != MH_OK ||
        MH_EnableHook(t) != MH_OK) {
        LOG("sleep: the action chart evaluator couldn't be hooked; sleepwear is off");
        g_eval_orig = nullptr;
        return;
    }
    LOG("sleep: watching for sleep (evaluator +%llX)", kRvaBranchEval);
    // getting into bed: the pack lookup (MO2CDLoader may hook it too: MinHook chains)
    U64 f = find_pack_lookup();
    void* ft = reinterpret_cast<void*>(f);
    if (!f || MH_CreateHook(ft, reinterpret_cast<void*>(&find_hook), reinterpret_cast<void**>(&g_find_orig)) != MH_OK ||
        MH_EnableHook(ft) != MH_OK) {
        LOG("sleep: the pack lookup %s; sleepwear goes on at the sleep itself, not when getting into bed",
            f ? "couldn't be hooked" : "wasn't found");
        g_find_orig = nullptr;
        return;
    }
    LOG("sleep: watching beds (pack lookup +%llX)", f - g_base);
}

// The HUD frame (peek_frame): a sleep that began or ended since the last frame.
void sleep_tick(U64 self)
{
    g_sleep_self = self;
    g_hud_at = GetTickCount64();
    if (g_fell.exchange(false) && !g_asleep) fell_asleep(self);
    if (g_woke.exchange(false) && g_asleep) woke_up(self);
    drop_temp();
    update_look_mode();
}

// kWmSleep on the game window's thread: the sleep's work at once, when that thread is the UI thread (else the HUD frame
// does it when it comes back). True: the message was ours.
bool sleep_message()
{
    static bool said = false;
    bool ui = g_ui_thread && GetCurrentThreadId() == g_ui_thread;
    if (!said) said = true, LOG("sleep: the window's thread %s the UI thread%s", ui ? "is" : "is NOT",
                                ui ? ": the outfit changes at once" : ": changes wait for the HUD");
    if (ui && g_sleep_self) {
        if (g_fell.load() || g_woke.load()) LOG("sleep: handled from the window message");
        sleep_tick(g_sleep_self);
    }
    return true;
}

// WM_TIMER on the game window: kFxTimer carries the sleep's outfit effect while the HUD frames are away. True: ours.
bool sleep_fx_timer(WPARAM id)
{
    if (id != kFxTimer) return false;
    if (!g_ui_thread || GetCurrentThreadId() != g_ui_thread || !fx_transition_busy()) {
        if (HWND w = g_game_hwnd.load()) KillTimer(w, kFxTimer);
        return true;
    }
    if (g_sleep_self && GetTickCount64() - g_hud_at > 40) fx_tick(g_sleep_self); // (the HUD away: the timer's frame)
    return true;
}

int sleep_looks()
{
    return g_sleep_look.load();
}

} // namespace om
