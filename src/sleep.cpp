// Activity: Sleeping (2026-11-09; keksdieb101's idea on Nexus): when the character goes to sleep, a sleepwear outfit goes on
// (several: one at random); on waking, the outfit worn before comes back, or, when none was, the pieces worn before.
//
// Seen through the game's action charts: the branch evaluator (kRvaBranchEval) is hooked and every taken branch
// (result 0) is looked at. The sleep states are in sequencer_action_pc, a chart only the played character runs (all
// three characters use Kliff's: only 1_phm has sequencer charts): bed_sleep start / loop / end, two variants
// (bed and bedroll / inn). Asleep: a branch into a start or loop state; the contexts that took it (the lower and upper
// body, ~4 ms apart) are remembered. Awake: one of them takes a branch into any state outside the sequencer charts
// and the game's hand-over idles (kQuiet) - control is back (ChartProbe 2026-11-09: basic_upper / basic_lower idle).
//
// The game itself only HIDES the helmet, cloak and weapons in bed, it doesn't unequip them (DIAG 2026-11-09: no slot
// changed), so what is worn is read when the sleep is seen. The hook only sets flags (it runs on the game's worker
// threads); the HUD frame (peek_frame) does the loads.
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
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <random>

namespace om {

#include "sleep_states.inc"

namespace {

// bed_sleep start / loop states (sequencer_action_pc, game 2.04.08): the sleep has begun
constexpr U32 kSleepStart[] = {0xD1F9862D, 0xE2B2EE2F, 0x70571FD6, 0xF50A6DC9}; // stt_02, ing_02, stt_03, ing_03
constexpr U64 kWakeAfterMs = 1500; // (a wake needs the sleep to have lasted: the states around the start aren't a wake)

using EvalFn = U32*(__fastcall*)(U64 ctx, U32* out, U64 a3, U64 a4, U64 a5, U64 a6, U64 a7, const U64* link, U64 a9,
                                 U64 a10);
EvalFn g_eval_orig = nullptr;

// set on the game's threads, taken on the HUD frame
std::atomic<bool> g_hook_asleep{false};
std::atomic<U64> g_hook_ctx[2] = {0, 0};
std::atomic<U64> g_hook_since{0};
std::atomic<bool> g_fell{false}, g_woke{false};

U32* __fastcall eval_hook(U64 ctx, U32* out, U64 a3, U64 a4, U64 a5, U64 a6, U64 a7, const U64* link, U64 a9, U64 a10)
{
    U32* r = g_eval_orig(ctx, out, a3, a4, a5, a6, a7, link, a9, a10);
    __try {
        if (!r || *r != 0 || !link || !link[0]) return r;
        U32 target = *reinterpret_cast<const U32*>(link[0] + 0x14);
        bool start = std::find(std::begin(kSleepStart), std::end(kSleepStart), target) != std::end(kSleepStart);
        if (start) {
            if (!g_hook_asleep.exchange(true)) {
                g_hook_ctx[0] = ctx, g_hook_ctx[1] = 0, g_hook_since = GetTickCount64();
                g_fell = true;
                LOG("sleep: seen falling asleep (state %08X, ctx %llX)", target, ctx);
            } else if (g_hook_ctx[0] != ctx && !g_hook_ctx[1]) {
                g_hook_ctx[1] = ctx; // (the other body half)
            }
        } else if (g_hook_asleep.load() && (ctx == g_hook_ctx[0].load() || ctx == g_hook_ctx[1].load()) &&
                   GetTickCount64() - g_hook_since.load() >= kWakeAfterMs &&
                   !std::binary_search(std::begin(kQuiet), std::end(kQuiet), target)) {
            if (g_hook_asleep.exchange(false)) {
                g_woke = true;
                LOG("sleep: seen waking up (state %08X, ctx %llX, %llu ms asleep)", target, ctx,
                    GetTickCount64() - g_hook_since.load());
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

// HUD side
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
}

// The HUD frame (peek_frame): a sleep that began or ended since the last frame.
void sleep_tick(U64 self)
{
    { // (does the HUD tick during the sleep? a gap of over a second while a sleep is pending or on is logged)
        static U64 last = 0;
        U64 now = GetTickCount64();
        if (last && now - last > 1000 && (g_asleep || g_fell.load() || g_hook_asleep.load()))
            LOG("sleep: the HUD frame was away for %llu ms", now - last);
        last = now;
    }
    if (g_fell.exchange(false) && !g_asleep) fell_asleep(self);
    if (g_woke.exchange(false) && g_asleep) woke_up(self);
    drop_temp();
    update_look_mode();
}

int sleep_looks()
{
    return g_sleep_look.load();
}

} // namespace om
