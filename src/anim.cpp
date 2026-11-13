// The played character's helmet take-off / take-on animations on demand (the helmet key, #34; found 2026-11-12).
//
// They are states of the upper body's action chart (basic_upper_interactive, shared by Kliff, Damiane and Oongka: each
// state's node has a clip per character). The game reaches them from code paths OM can't call, so OM plays them the way a
// chart branch does:
//  - The branch walker hands each candidate branch {branch*, link*, owner, chart} to kRvaBranchTry (comp, layer,
//    candidate*, ...). While a request is pending, the played character's upper-layer candidate becomes our own branch
//    (a copy of the chart's take-off branch with the wanted target) and the evaluator (kRvaBranchEval, sleep.cpp's hook
//    asks anim_branch_passes) lets it hold. The game then looks the state up, validates it and plays it itself.
//  - The game's internal server answers a started action with a start-action acknowledgement (kRvaCtrlAck, the
//    control component's handler), which puts both layers back to idle: the server's chart never took our branch. That
//    one answer is skipped while the upper layer plays our state, and only if it would end it.
//  - The animation puts a fixed item in the right hand (the Visione, permanent id kPidVisionePropItem): the item info
//    lookup (kRvaItemInfo) answers it with the worn helmet while the animation runs. The animation equips it on the
//    helmet slot as a visual item: some 30 call sites (the equipment component, a frame event, a bag scan, logged
//    2026-11-13) all need the helmet - except the item's charges (kRvaItemCharges): the Visione's recharge, divided by
//    a recharge time a helmet's info doesn't have (a crash with the Plate Helm of the Shadows).
// The caller (the helmet key, arc.cpp) hides / shows the helmet when the state is left (anim_outcome).
#include "anim.h"
#include "addrs.h"
#include "gear.h"
#include "items.h"
#include "log.h"
#include "mem.h"
#include <MinHook.h>
#include <atomic>
#include <intrin.h>
#include <cstring>

namespace om {
U16 key_of_pid(U32 pid); // (items.cpp)
bool sleep_evaluator_hooked(); // (sleep.cpp)

namespace {

constexpr U32 kStateTakeOff = 0x84645118; // basic_upper_interactive s178 (cd_phm_basic_00_00_nor_std_helmet_takeoff_00)
constexpr U32 kStateTakeOn = 0x6EEB07AA;  // s148 (..._helmet_takeon_00)

// The chart's own take-off branch (basic_upper_interactive b277, 0x38 bytes: +0x08 blend frames 8, +0x14 the target
// state's name hash, +0x1C type 4); its conditions are never read (anim_branch_passes)
alignas(16) std::uint8_t g_branch[0x38] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x80, 0xbf,
    0x00, 0x00, 0x00, 0x00, 0x18, 0x51, 0x64, 0x84, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xa9, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x22, 0x22, 0x1e,
    0x00, 0x00, 0x00, 0x01, 0xff, 0xff, 0x00, 0x00};

enum Phase : int { kIdle, kRequested, kPlaying, kEnded };
std::atomic<int> g_phase{kIdle};
std::atomic<bool> g_ok{false};
std::atomic<U32> g_player_id{0};         // (actor +0x60 of the played character; anim_tick)
std::atomic<U32> g_want{0};              // the state asked for
std::atomic<U64> g_comp{0};              // the control component playing it
std::atomic<U64> g_asked_at{0}, g_started_at{0};
std::atomic<bool> g_seen{false};         // (the upper layer was in the state)
std::atomic<int> g_outcome{0};           // (AnimOutcome once kEnded)
std::atomic<U16> g_prop_from{0xFFFF}, g_prop_to{0xFFFF};
std::atomic<bool> g_injecting{false};
thread_local bool t_inject = false;

bool is_player(U64 comp)
{
    U64 actor = 0;
    U32 id = 0;
    return rd(comp + 8, actor) && plausible(actor) && rd(actor + 0x60, id) && id && id == g_player_id.load();
}

U32 upper_state(U64 comp) // (the upper layer's state: its name hash; 0 unknown)
{
    U64 layer = 0, st = 0;
    U32 h = 0;
    if (rd(comp + kOffChartLayers, layer) && plausible(layer) && rd(layer + kOffLayerState, st) && plausible(st))
        rd(st + kOffStateName, h);
    return h;
}

void end(AnimOutcome o, const char* why)
{
    g_outcome = int(o);
    g_phase = kEnded;
    LOG("helmet animation %08X: %s", g_want.load(), why);
}

using TryFn = U64(__fastcall*)(U64, U64, U64, U64, U64, U64, U64, U64, U64, U64);
TryFn g_try_orig = nullptr;
U64 __fastcall try_hook(U64 comp, U64 layer, U64 cur, U64 cand, U64 a5, U64 a6, U64 a7, U64 a8, U64 a9, U64 a10)
{
    if (g_phase.load() != kRequested || (layer & 0xFF) != 0 || !plausible(cand) || !is_player(comp) ||
        g_injecting.exchange(true))
        return g_try_orig(comp, layer, cur, cand, a5, a6, a7, a8, a9, a10);
    U64 saved = 0;
    rd(cand, saved);
    const U32 want = g_want.load();
    std::memcpy(g_branch + 0x14, &want, 4);
    *reinterpret_cast<U64*>(cand) = reinterpret_cast<U64>(g_branch);
    t_inject = true;
    const U64 r = g_try_orig(comp, layer, cur, cand, a5, a6, a7, a8, a9, a10);
    t_inject = false;
    *reinterpret_cast<U64*>(cand) = saved;
    if ((r & 0xFF) == 0) { // (taken)
        g_comp = comp;
        g_seen = false;
        g_started_at = GetTickCount64();
        g_phase = kPlaying;
    }
    g_injecting = false;
    return r;
}

using AckFn = int*(__fastcall*)(U64, U64, U64, U64);
AckFn g_ack_orig = nullptr;
int g_ack_fine = 0;
int* __fastcall ack_hook(U64 comp, U64 ctx, U64 records, U64 x)
{
    if (g_phase.load() == kPlaying && comp == g_comp.load()) {
        const U32 want = g_want.load();
        U32 to = 0;
        rd(records, to); // (the upper layer's record: the state's name hash first)
        if (to != want && upper_state(comp) == want) {
            g_ack_fine = 0;
            return &g_ack_fine;
        }
    }
    return g_ack_orig(comp, ctx, records, x);
}

RecordGetFn g_info_orig = nullptr;
U64 __fastcall info_hook(const U16* key)
{
    const int p = g_phase.load();
    if ((p == kRequested || p == kPlaying) && key && *key == g_prop_from.load()) {
        const U16 to = g_prop_to.load();
        const U64 from = reinterpret_cast<U64>(_ReturnAddress()) - g_base;
        if (to != 0xFFFF && from - kRvaItemCharges >= kItemChargesSize) return g_info_orig(&to);
    }
    return g_info_orig(key);
}

// The animation's own moment for the helmet (ClientFrameEventSetVisualEquipItem, kRvaSetVisualEquipEv: this, int* out,
// event data, x, ctx*: [ctx] -> the actor): it would put the Visione on (or off) the helmet slot as a visual item, left
// there afterwards (the slot refresh then puts it back each time the helmet is taken off: the hair flickered). Its data
// (+8 -> +0x10) says put on (1) or taken off (0). The take-off has two: put on at its start, taken off when the hand
// lifts it; the take-on one: put on when the hand has set it on the head. For the played character during our
// animation none is run: the one matching the animation (take-off: off, take-on: on) is the cue (anim_cue).
std::atomic<bool> g_cue{false};
using EvFn = int*(__fastcall*)(U64, int*, U64, U64, U64);
EvFn g_ev_orig = nullptr;
int* __fastcall ev_hook(U64 self, int* out, U64 data, U64 x, U64 ctx)
{
    if (g_phase.load() == kPlaying) {
        U64 actor = 0;
        U32 id = 0;
        if (rd(ctx, actor) && plausible(actor) && rd(actor + 0x60, id) && id && id == g_player_id.load()) {
            U64 rec = 0;
            std::uint8_t on = 0;
            if (rd(data + 8, rec) && plausible(rec)) rd(rec + 0x10, on);
            const bool take_off = g_want.load() == kStateTakeOff;
            const bool cue = take_off ? on == 0 : on != 0;
            LOG("helmet animation %08X: its helmet %s%s", g_want.load(), on ? "put on" : "taken off",
                cue ? ": the moment" : "");
            if (cue) g_cue = true;
            *out = 0;
            return out;
        }
    }
    return g_ev_orig(self, out, data, x, ctx);
}

} // namespace

bool anim_branch_passes(const U64* candidate)
{
    U64 b = 0;
    return t_inject && candidate && rd(reinterpret_cast<U64>(candidate), b) && b == reinterpret_cast<U64>(g_branch);
}

void anim_install()
{
    if (!have({kRvaBranchTry, kRvaCtrlAck, kRvaItemInfo, kRvaSetVisualEquipEv, kRvaItemCharges}) || !sleep_evaluator_hooked()) {
        LOG("helmet animations: off (the action chart's functions weren't all found)");
        return;
    }
    struct { U64 rva; void* fn; void** orig; } hooks[] = {
        {kRvaBranchTry, reinterpret_cast<void*>(&try_hook), reinterpret_cast<void**>(&g_try_orig)},
        {kRvaCtrlAck, reinterpret_cast<void*>(&ack_hook), reinterpret_cast<void**>(&g_ack_orig)},
        {kRvaItemInfo, reinterpret_cast<void*>(&info_hook), reinterpret_cast<void**>(&g_info_orig)},
        {kRvaSetVisualEquipEv, reinterpret_cast<void*>(&ev_hook), reinterpret_cast<void**>(&g_ev_orig)},
    };
    for (auto& h : hooks) {
        void* t = reinterpret_cast<void*>(g_base + h.rva);
        if (MH_CreateHook(t, h.fn, h.orig) != MH_OK || MH_EnableHook(t) != MH_OK) {
            LOG("helmet animations: off (hook at +%llX failed)", h.rva);
            return;
        }
    }
    g_ok = true;
    LOG("helmet animations: on");
}

void anim_tick()
{
    if (!g_ok.load()) return;
    { // the played character
        U64 ctx[4] = {};
        const U64 actor = acquire_player(ctx);
        U32 id = 0;
        if (actor) rd(actor + 0x60, id);
        if (ctx[0]) release_player(ctx);
        g_player_id = id;
    }
    const U64 now = GetTickCount64();
    switch (g_phase.load()) {
    case kRequested:
        if (now - g_asked_at.load() > 600) end(AnimOutcome::Failed, "not started (no branch taken)");
        break;
    case kPlaying: {
        const U32 st = upper_state(g_comp.load());
        if (st == g_want.load()) {
            if (!g_seen.exchange(true)) LOG("helmet animation %08X: playing", st);
        } else if (g_seen.load()) {
            end(AnimOutcome::Played, "over");
        } else if (now - g_started_at.load() > 500) {
            end(AnimOutcome::Failed, "the game didn't enter it");
        }
        if (g_phase.load() == kPlaying && now - g_started_at.load() > 10000) end(AnimOutcome::Played, "ten seconds: over");
        break;
    }
    default:
        break;
    }
}

bool anim_helmet_start(bool take_off, U16 helmet_key)
{
    if (!g_ok.load() || !g_player_id.load() || g_phase.load() != kIdle) return false;
    static const U16 visione = key_of_pid(kPidVisionePropItem);
    g_prop_from = visione;
    g_prop_to = helmet_key;
    g_want = take_off ? kStateTakeOff : kStateTakeOn;
    g_cue = false;
    g_asked_at = GetTickCount64();
    g_phase = kRequested;
    LOG("helmet animation %08X: asked (%s, helmet key %u in the hand for key %u)", g_want.load(),
        take_off ? "take-off" : "take-on", helmet_key, visione);
    return true;
}

bool anim_cue() { return g_cue.exchange(false); }

AnimOutcome anim_outcome()
{
    int e = kEnded;
    if (!g_phase.compare_exchange_strong(e, kIdle)) return AnimOutcome::Running;
    return AnimOutcome(g_outcome.load());
}

} // namespace om
