// Activity: Sleeping (2026-11-09; keksdieb101's idea on Nexus): when the character gets into bed, a sleepwear outfit goes
// on (several: one at random); when they get out again - having slept or not - the outfit worn before comes back, or,
// when none was, the pieces worn before.
//
// A bed is its stage chart (the scene's .pastage, gimmick_sleep_bed_left / _right; cd_seq_minigame_sleep alike):
// SLEEP_IDLE -> SLEEP_START (SLEEP_PET_START with a pet) -> SLEEP_SELECT_UI (the hours menu) -> SLEEP_nHOUR -> (the
// time skip's loading) -> SLEEP_COMPLETE (waking, standing up), or SLEEP_CANCEL -> LEAVE (standing up), or SLEEP_NEGATIVE /
// _COOLTIME / _EXCLUSIVE (refused) - and every way ends with Timeline.branch(Terminate) once its last timeline is over
// (ForceTerminate when the game breaks it off: a crime, a fight...). Two of the game's own events, nothing else:
// - the server's branch to a named state (kRvaStageBranchByName): the first state past SLEEP_IDLE is the game's verdict,
//   about 0.1 s after getting in (2026-11-17: accepted SLEEP_PET_START at +93 ms, refused SLEEP_NEGATIVE_COOLTIME at
//   +78 ms) - accepted: in bed, the sleepwear goes on (before the outfit effect would show anything); refused: nothing
//   changes (#49; user: the game's refusal must not swap; getting into bed still does). The hours menu: the sleep's
//   looks step back (below).
// - the stage chart processor's end (kRvaStageEnd): out of bed - after a sleep, a refused sleep, standing up from the
//   hours menu, or the game breaking it off (at the moment he gets control back: his first own state 15 ms later).
// A bed's chart is told by its hours menu state (kHoursMenu, by name: a branch of one of the chart's states goes to it -
// read from the processor's chart, not from the state it is in, which on the chart's end has one branch, Terminate).
// Without either hook (not found after a game update): no sleepwear - nothing guessed instead.
// Before (until 2026-11-17): the bed scene's file read from the packs, tied to the played character by a step state
// within 4 s; out of bed by a chart state some exits never took, or any state outside the scripted ones 1.5 s in (the
// time skip's idle ended a sleep 2.6 s early); a refusal by his idle in bed 0.1 s in (a race with the outfit effect).
//
// The game only HIDES the helmet, cloak and weapons in bed, it doesn't unequip them (2026-11-09: no slot changed), so
// what is worn is read when the bed is accepted. The hooks only set flags and post kWmSleep to the game window: its
// thread is the UI thread, and its messages keep coming while the HUD is hidden (the HUD frame, peek_frame, is the
// fallback). The sleep's outfit changes play the outfit effect; its timeline (fx_tick, on HUD frames) is carried by a
// window timer while the HUD is hidden for the bed's fade (fx_carry).
//
// In bed, the looks (looks.cpp, sleep_looks): a sleepwear outfit's own looks win over the game's in-bed hiding (a cloak
// set to Shown stays shown; Default goes the game's way); any other outfit steps back as in flight - from the hours
// menu on (he lies down; getting in, the hidden pieces would show while he climbs in).
#include "om.h"
#include "addrs.h"
#include "gear.h"
#include "load.h"
#include "looks.h"
#include "store.h"
#include "log.h"
#include "game.h"
#include "wheel_hooks.h"
#include "anim.h"
#include "ui.h"
#include <MinHook.h>
#include <atomic>
#include <random>
#include <string>
#include <vector>

namespace om {

extern std::atomic<HWND> g_game_hwnd; // (input.cpp)
constexpr UINT kWmSleep = WM_APP + 0x51;

namespace {

// a bed's stage chart: the state of its hours menu (the beds' gimmick_sleep_bed_left / _right and cd_seq_minigame_sleep)
constexpr const char* kHoursMenu = "SLEEP_SELECT_UI";

using EvalFn = U32*(__fastcall*)(U64 ctx, U32* out, U64 a3, U64 a4, U64 a5, U64 a6, U64 a7, const U64* link, U64 a9,
                                 U64 a10);
EvalFn g_eval_orig = nullptr;
using StageEndFn = U64(__fastcall*)(U64 proc, U64 type, U64 a3, U64 a4); // (kRvaStageEnd: processor, u8 end type)
StageEndFn g_stage_end_orig = nullptr;
using StageBranchFn = U64(__fastcall*)(U64 proc, U64 a2, U64 a3, U64 a4, const U32* name, U64 a6, U64 a7, U64 a8, U64 a9,
                                       U64 a10); // (kRvaStageBranchByName: processor, ..., the state's name 5th)
StageBranchFn g_stage_branch_orig = nullptr;

// set on the game's threads, taken on the UI thread
std::atomic<bool> g_in_bed{false};  // (an accepted bed, until its stage chart ends)
std::atomic<U64> g_bed_since{0};    // (when it was accepted: for the log)
std::atomic<bool> g_lying{false};   // (its hours menu came: he lies down - the looks step back)
std::atomic<bool> g_fell{false}, g_woke{false};
std::atomic<U32> g_bed_state{0}; // (the state a bed's chart last went to, its name; 0: none since the last bed's end)

void post()
{
    if (HWND w = g_game_hwnd.load()) PostMessageW(w, kWmSleep, 0, 0); // (now, not at the next HUD frame)
}

// A bed's stage chart: a branch of one of its states goes to the hours menu (kHoursMenu). (A state's own name is no
// resolver entry - offset 0 - so the names are read from the branches.)
bool is_bed_chart(U64 proc)
{
    const U64 chart = rp(proc + kOffStageProcChart);
    const U64 states = chart ? rp(chart + kOffStageChartStates) : 0;
    const U32 n = chart ? r32(chart + kOffStageChartCount) : 0;
    if (!states || !n || n > 4096) return false;
    for (U32 i = 0; i < n; ++i) {
        const U64 st = rp(states + 8ull * i);
        const U64 br = st ? rp(st + kOffStageBranches) : 0;
        const U32 m = st ? r32(st + kOffStageBranchCount) : 0;
        if (!br || m > 256) continue;
        for (U32 k = 0; k < m; ++k)
            if (const U64 b = rp(br + 8ull * k); b && class_name(r32(b + kOffStageBranchTarget)) == kHoursMenu) return true;
    }
    return false;
}

// A bed's stage chart ends (the client's copy and the internal server's alike - the first one counts): out of bed.
void bed_chart_ended(U64 type)
{
    const U32 last = g_bed_state.exchange(0);
    if (!g_in_bed.exchange(false)) return; // (a refused bed, or the second copy's end)
    g_lying = false, g_woke = true;
    post();
    LOG("sleep: out of bed (the bed's stage chart ended, type %u, after %s; %llu ms in bed)", unsigned(type & 0xFF),
        last ? class_name(last).c_str() : "no state seen", GetTickCount64() - g_bed_since.load());
}

U64 __fastcall stage_end_hook(U64 proc, U64 type, U64 a3, U64 a4)
{
    __try {
        if (is_bed_chart(proc)) bed_chart_ended(type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return g_stage_end_orig(proc, type, a3, a4);
}

// A stage chart went to the state `to`: a bed's SLEEP_* states. The first one past SLEEP_IDLE is the game's verdict:
// SLEEP_NEGATIVE* refused (nothing changes), any other (SLEEP_START, SLEEP_PET_START, ...) accepted - in bed, the
// sleepwear goes on. The hours menu: he lies down.
void bed_chart_branched(U64 proc, U32 to)
{
    const std::string n = class_name(to);
    if (n.rfind("SLEEP_", 0) != 0 || !is_bed_chart(proc)) return;
    const U32 before = g_bed_state.exchange(to);
    if (g_in_bed.load()) {
        LOG("sleep: the bed's stage: %s (%llu ms in bed)", n.c_str(), GetTickCount64() - g_bed_since.load());
        if (n == kHoursMenu && !g_lying.exchange(true)) post();
        return;
    }
    LOG("sleep: the bed's stage: %s", n.c_str());
    if (n == "SLEEP_IDLE" || (before && class_name(before) != "SLEEP_IDLE")) return; // (no verdict yet / not the first)
    if (n.rfind("SLEEP_NEGATIVE", 0) == 0) {
        LOG("sleep: the game refused the bed: nothing changes");
        return;
    }
    g_bed_since = GetTickCount64(), g_lying = n == kHoursMenu;
    g_in_bed = true, g_fell = true;
    post();
    LOG("sleep: into bed (the game accepted it: %s)", n.c_str());
}

U64 __fastcall stage_branch_hook(U64 proc, U64 a2, U64 a3, U64 a4, const U32* name, U64 a6, U64 a7, U64 a8, U64 a9, U64 a10)
{
    U64 was = 0;
    U32 to = 0;
    __try {
        was = rp(proc + kOffStageProcState), to = name ? *name : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    const U64 r = g_stage_branch_orig(proc, a2, a3, a4, name, a6, a7, a8, a9, a10);
    __try {
        if (to && to != g_bed_state.load() && rp(proc + kOffStageProcState) != was) bed_chart_branched(proc, to);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

// The action chart's branch evaluator: the helmet animations' own branch (anim.cpp) and flight (looks.cpp) - no longer
// anything of the sleep's (its name stays: sleep_evaluator_hooked).
U32* __fastcall eval_hook(U64 ctx, U32* out, U64 a3, U64 a4, U64 a5, U64 a6, U64 a7, const U64* link, U64 a9, U64 a10)
{
    if (anim_branch_passes(link)) { // (the helmet animations' own branch: it holds; anim.cpp)
        *out = 0;
        return out;
    }
    U32* r = g_eval_orig(ctx, out, a3, a4, a5, a6, a7, link, a9, a10);
    __try {
        if (r && *r == 0 && link && link[0])
            look_glide(ctx, *reinterpret_cast<const U32*>(link[0] + 0x14)); // (in the air under a glider or the rocket pack)
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

// HUD side (the HUD frame, or the game window's thread when it is the UI thread: kWmSleep)
U64 g_sleep_self = 0; // (the status screen the HUD frame last gave: its object outlives a hidden HUD)
bool g_asleep = false;
std::atomic<int> g_sleep_look{0}; // what sleep_looks() answers: 0 awake, 1 step back, 2 sleepwear wins (read by look_sync)
// An activity's outfit swap (Sleeping, Combat - #45): what was worn when it began, given back when it ends. Combat wins
// over Sleeping: begun in bed, its "before" is the sleepwear, which comes back first; a sleep doesn't swap during combat.
struct Swap {
    const char* what, * began, * ended, * temp_name; // (the log's words; the pieces' temporary outfit's name)
    std::uint8_t activity;
    bool swapped = false;   // its outfit was put on: something to give back when it ends
    std::string before_name; // (the outfit worn before, found again by name: the list may change meanwhile; "": none)
    Outfit before_items;    // the pieces worn before, when no outfit was
    bool have_items = false;
    U16 ch = kNoCharacter;
    bool let_go = false;    // ("No outfit" was on before (Unequip Outfit, #21): it comes back at the end)
    int let_go_of = -1;     // (its g_unmanaged value: the outfit let go)
    bool pending_end = false; // (it ended while an outfit change still ran: given back once that change is over)
};
Swap g_sleep_swap{"sleep", "asleep", "awake", "(before sleep)", kActivitySleep};
Swap g_combat_swap{"combat", "in combat", "out of combat", "(before combat)", kActivityCombat};
Swap* g_temp_for = nullptr;    // the swap whose pieces are going back on as a temporary outfit (removed after)
bool g_in_combat = false;      // (the played character's combat, as the HUD side last took it)
std::atomic<bool> g_combat_now{false}; // (the in-battle switch's last report, looks.cpp: the tick follows it)

int worn_index()
{
    for (size_t i = 0; i < g_outfits.size(); ++i)
        if (g_outfits[i].worn) return int(i);
    return -1;
}

void update_look_mode()
{
    int w = worn_index();
    // In bed, the looks change only once the sleepwear is on or he lies down (the hours menu): getting in, the hidden
    // pieces would show while he climbs in (2026-11-17).
    const bool sleepwear = w >= 0 && g_outfits[size_t(w)].activity == kActivitySleep && !g_outfits[size_t(w)].temp;
    int mode = !g_asleep ? 0 : sleepwear ? 2 : g_lying.load() ? 1 : 0;
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

// Begins an activity: its outfit (one at random) goes on, what was worn noted to come back.
void begin(U64 self, Swap& s)
{
    s.swapped = false, s.pending_end = false;
    prepare_outfits(); // (the character's own outfits)
    s.ch = g_character;
    std::vector<U32> wear;
    for (size_t i = 0; i < g_outfits.size(); ++i)
        if (g_outfits[i].activity == s.activity && !g_outfits[i].temp && !is_weapon_set(g_outfits[i])) wear.push_back(U32(i));
    int worn = find_worn_outfit(self);
    s.let_go = false;
    if (auto u = g_unmanaged.find(g_character); u != g_unmanaged.end()) { // ("No outfit": the pieces, not an outfit)
        s.let_go = true, s.let_go_of = u->second;
        worn = -1;
    }
    LOG("%s: %s (character %u, %zu %s outfit(s), worn outfit %d%s)", s.what, s.began, g_character, wear.size(), s.what,
        worn, s.let_go ? ", No outfit" : "");
    if (wear.empty()) return; // (nothing to change)
    if (worn >= 0 && g_outfits[size_t(worn)].activity == s.activity) {
        LOG("%s: \"%s\" is its outfit already: kept", s.what, g_outfits[size_t(worn)].name.c_str());
        return;
    }
    s.before_name = worn >= 0 ? g_outfits[size_t(worn)].name : std::string();
    s.have_items = false;
    if (worn < 0) {
        s.before_items = Outfit{};
        s.have_items = snapshot(self, s.before_items) && !s.before_items.items.empty();
        LOG("%s: no outfit worn; %zu pieces noted to come back", s.what, s.have_items ? s.before_items.items.size() : size_t(0));
    }
    static std::mt19937 rng{std::random_device{}()};
    U32 pick = wear[std::uniform_int_distribution<size_t>(0, wear.size() - 1)(rng)];
    LOG("%s: putting on \"%s\"%s", s.what, g_outfits[pick].name.c_str(), wear.size() > 1 ? " (picked at random)" : "");
    s.swapped = true;
    start_loading(self, pick);
    fx_carry();
}

// Ends an activity: what was worn before it goes back on (an outfit, or the pieces as a temporary outfit).
// An outfit change still running: the outfit effect playing (also unseen, behind the bed's fade), the load's steps,
// the model taking the new pieces. A swap's end waits for it: started meanwhile, the load was turned away ("an outfit
// is still being put on") and the pieces never came back (a sleep the game refused, 2026-11-17).
bool change_running() { return fx_transition_busy() || g_async.active || look_changing(); }

void end(U64 self, Swap& s)
{
    if (s.swapped && change_running()) {
        if (!s.pending_end) LOG("%s: %s; what was worn before waits for the outfit change still running", s.what, s.ended);
        s.pending_end = true;
        return;
    }
    s.pending_end = false;
    if (!s.swapped) {
        LOG("%s: %s", s.what, s.ended);
        return;
    }
    s.swapped = false;
    if (g_character != s.ch) {
        LOG("%s: %s as another character: nothing given back", s.what, s.ended);
        return;
    }
    prepare_outfits();
    int back = -1;
    if (!s.before_name.empty())
        for (size_t i = 0; i < g_outfits.size(); ++i)
            if (g_outfits[i].name == s.before_name && !g_outfits[i].temp) back = int(i);
    if (back >= 0) {
        LOG("%s: %s; \"%s\" goes back on", s.what, s.ended, g_outfits[size_t(back)].name.c_str());
        start_loading(self, U32(back));
        fx_carry();
    } else if (s.have_items) {
        // (the pieces as a temporary outfit: loaded like any other, never saved, removed once it is on)
        Outfit t = s.before_items;
        t.name = s.temp_name;
        t.temp = true;
        t.load_weapons = t.has_weapons;
        g_outfits.push_back(t);
        g_temp_for = &s;
        LOG("%s: %s; the %zu pieces worn before go back on", s.what, s.ended, t.items.size());
        start_loading(self, U32(g_outfits.size() - 1));
        fx_carry();
    } else {
        LOG("%s: %s; the outfit worn before (\"%s\") is gone: nothing given back", s.what, s.ended, s.before_name.c_str());
    }
    s.have_items = false;
}

void fell_asleep(U64 self)
{
    g_asleep = true;
    if (g_in_combat) { // (Combat wins: no sleepwear during a fight - next to impossible anyway)
        g_sleep_swap.swapped = false;
        LOG("sleep: asleep in combat: the combat outfit stays");
        return;
    }
    begin(self, g_sleep_swap);
}

void woke_up(U64 self)
{
    g_asleep = false;
    end(self, g_sleep_swap);
}

void drop_temp()
{
    if (!g_temp_for || g_async.active || fx_busy_target() != -1 || look_changing()) return;
    Swap& s = *g_temp_for;
    g_temp_for = nullptr;
    bool was_worn = false;
    for (size_t i = g_outfits.size(); i-- > 0;)
        if (g_outfits[i].temp) was_worn |= g_outfits[i].worn, g_outfits.erase(g_outfits.begin() + std::ptrdiff_t(i));
    if (was_worn) mark_worn(-1); // (what is worn is no outfit again, as before)
    if (s.let_go) g_unmanaged[s.ch] = s.let_go_of, s.let_go = false; // ("No outfit" again)
    LOG("%s: the pieces worn before are back on", s.what);
}

} // namespace

bool sleep_evaluator_hooked() { return g_eval_orig != nullptr; }

void sleep_install()
{
    // the helmet animations' branch and flight (anim.cpp, looks.cpp) - not the sleep's
    if (!have({kRvaBranchEval})) {
        LOG("sleep: the action chart evaluator wasn't found (helmet animations and flight need it)");
    } else if (void* t = reinterpret_cast<void*>(g_base + kRvaBranchEval);
               MH_CreateHook(t, reinterpret_cast<void*>(&eval_hook), reinterpret_cast<void**>(&g_eval_orig)) != MH_OK ||
               MH_EnableHook(t) != MH_OK) {
        LOG("sleep: the action chart evaluator couldn't be hooked (helmet animations and flight need it)");
        MH_RemoveHook(t);
        g_eval_orig = nullptr;
    } else {
        LOG("sleep: watching the action charts (evaluator +%llX)", kRvaBranchEval);
    }
    // a bed: its stage chart's verdict (in bed) and end (out of bed) - without both no sleepwear (nothing guessed)
    if (!have({kRvaStageEnd, kRvaStageBranchByName, kRvaClassNames, kOffStageProcChart, kOffStageChartStates,
               kOffStageChartCount, kOffStageBranches, kOffStageBranchCount, kOffStageBranchTarget, kOffStageProcState})) {
        LOG("sleep: a bed's stage chart wasn't found; sleepwear is off");
        return;
    }
    void* et = reinterpret_cast<void*>(g_base + kRvaStageEnd);
    void* bt = reinterpret_cast<void*>(g_base + kRvaStageBranchByName);
    if (MH_CreateHook(et, reinterpret_cast<void*>(&stage_end_hook), reinterpret_cast<void**>(&g_stage_end_orig)) != MH_OK ||
        MH_CreateHook(bt, reinterpret_cast<void*>(&stage_branch_hook), reinterpret_cast<void**>(&g_stage_branch_orig)) !=
            MH_OK ||
        MH_EnableHook(et) != MH_OK || MH_EnableHook(bt) != MH_OK) {
        LOG("sleep: a bed's stage chart couldn't be hooked; sleepwear is off");
        MH_RemoveHook(et), MH_RemoveHook(bt);
        g_stage_end_orig = nullptr, g_stage_branch_orig = nullptr;
        return;
    }
    LOG("sleep: watching beds (stage chart branch +%llX, end +%llX)", kRvaStageBranchByName, kRvaStageEnd);
}

// The HUD frame (peek_frame): a sleep that began or ended since the last frame.
void sleep_tick(U64 self)
{
    g_sleep_self = self;
    g_hud_at = GetTickCount64();
    // (combat first: it wins over a sleep - a combat begun in bed takes the sleepwear as what comes back after it)
    if (const bool now = g_combat_now; now != g_in_combat) {
        g_in_combat = now;
        if (now) begin(self, g_combat_swap);
        else end(self, g_combat_swap);
    }
    if (g_fell.exchange(false) && !g_asleep) fell_asleep(self);
    if (g_woke.exchange(false) && g_asleep) woke_up(self);
    for (Swap* s : {&g_combat_swap, &g_sleep_swap}) // (an end held for a running change: now that it is over)
        if (s->pending_end && !change_running()) end(self, *s);
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

// The played character's combat began (true) or ended (looks.cpp, the in-battle switch's hook): the combat outfit's swap
// is done on the HUD side (sleep_tick), at once when the game window's thread is the UI thread (the window message).
void combat_changed(bool in)
{
    g_combat_now = in;
    post();
}

} // namespace om
